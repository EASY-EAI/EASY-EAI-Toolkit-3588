/**
 * QMultiDisplay — 摄像头上屏 + Qt UI 半透明叠加
 *
 * 架构（一条显示链路）：
 *
 *   IMX415 ×1/2 ──V4L2 采集线程──► FrameQueue(深度1，丢旧帧)
 *        │ 单目: NV12 dma-buf 逐帧导入图层（零拷贝）
 *        │ 双目: RGA / GPU 各缩半宽拼接 → 2 块输出缓冲轮转
 *        ▼
 *   视频图层 zpos=0 ──► VOP2 同一 VP 硬件像素级混合 ◄── UI 图层 zpos=10
 *        (disp_engine 互斥锁串行化两条提交路径)          ARGB 半透明
 *                                                          ▲
 *                                     QWidget::render() → QImage 包显存 mmap
 *                                     (Qt offscreen，不碰显示设备) ── evdev 输入
 *
 * 参数（-n/-w/-h/-C/-L/-R 与 cameraPlayer 完全一致）：
 *   -n 1|2    摄像头数，默认 1；2 = 双目各缩半宽拼接上屏
 *   -w/-h     每路采集尺寸，默认 1920x1080，须偶数
 *   -C rga|gpu 双目合成后端，默认 gpu
 *   -L/-R     显式指定 /dev/videoN（默认按 media 拓扑自动发现）
 *   --plane   强制 UI 图层对象 ID（默认自动挑首个 ARGB 且 ≠ 视频层）
 *   --alpha   UI 整层不透明度 0~255，默认 255
 */

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QString>
#include <QTimer>
#include <QWindow>

#include <csignal>
#include <cstdio>

#include "disp_engine.h"
#include "evdev_input.h"
#include "overlay_widget.h"
#include "video_stitch.h"

/* widget->render() 内部会派发 Paint 事件，据此过滤掉"假脏" */
static bool s_rendering = false;

class DirtyFilter : public QObject
{
public:
    explicit DirtyFilter(QObject *parent) : QObject(parent) {}
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!s_rendering && event->type() == QEvent::UpdateRequest)
            dirty = true;
        return QObject::eventFilter(watched, event);
    }
    bool dirty = false;
};

struct AppConfig {
    VideoConfig video;
    int plane = -1;    /* UI 图层显式指定；-1 自动 */
    int alpha = 255;   /* UI 整层不透明度 */
    bool help = false;
};

static AppConfig parseArgs(int argc, char *argv[])
{
    AppConfig c;
    c.video.cam_count = 1;
    c.video.cam_w = 1920;
    c.video.cam_h = 1080;
    c.video.compose_gpu = 1;
    c.video.video_nums[0] = -1;
    c.video.video_nums[1] = -1;

    for (int i = 1; i < argc; ++i) {
        const QString arg = argv[i];
        auto nextInt = [&](int *dst) -> bool {
            if (i + 1 >= argc)
                return false;
            *dst = QString(argv[++i]).toInt();
            return true;
        };
        if (arg == QLatin1String("-n")) {
            if (!nextInt(&c.video.cam_count)) c.help = true;
        } else if (arg == QLatin1String("-w")) {
            if (!nextInt(&c.video.cam_w)) c.help = true;
        } else if (arg == QLatin1String("-h")) {
            /* 与 cameraPlayer 一致：-h 是采集高度，帮助用 --help / -? */
            if (!nextInt(&c.video.cam_h)) c.help = true;
        } else if (arg == QLatin1String("-L")) {
            if (!nextInt(&c.video.video_nums[0])) c.help = true;
        } else if (arg == QLatin1String("-R")) {
            if (!nextInt(&c.video.video_nums[1])) c.help = true;
        } else if (arg == QLatin1String("--plane")) {
            if (!nextInt(&c.plane)) c.help = true;
        } else if (arg == QLatin1String("--alpha")) {
            if (!nextInt(&c.alpha)) c.help = true;
        } else if (arg == QLatin1String("-C")) {
            if (i + 1 >= argc) { c.help = true; break; }
            const QString m = argv[++i];
            if (m == QLatin1String("gpu"))
                c.video.compose_gpu = 1;
            else if (m == QLatin1String("rga"))
                c.video.compose_gpu = 0;
            else
                c.help = true;
        } else if (arg == QLatin1String("--help") || arg == QLatin1String("-?")) {
            c.help = true;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            c.help = true;
        }
    }
    return c;
}

static void printUsage(const char *prog)
{
    printf("Usage: %s [-n 1|2] [-w cam_w] [-h cam_h] [-C rga|gpu]\n"
           "          [-L video_num] [-R video_num] [--plane id] [--alpha 0-255]\n"
           "  -n 1|2   camera count (default 1; 2 = dual side-by-side stitched)\n"
           "  -w/-h    per-camera capture size (default 1920x1080, must be even)\n"
           "  -C       dual compose backend: rga | gpu (default gpu)\n"
           "  -L/-R    explicit /dev/videoN override for camera 0/1\n"
           "           (default: auto-detect live ISP mainpaths)\n"
           "  --plane  force UI DRM plane object id (default: auto pick)\n"
           "  --alpha  UI layer opacity 0-255 (default 255)\n"
           "  --help   show this help\n",
           prog);
}

/* Ctrl+C / kill：先通知视频路径停止，再让 Qt 事件循环退出。
   QCoreApplication::quit 在信号处理器里调用并非严格异步信号安全，
   演示程序可接受。 */
static void requestStop(int)
{
    video_request_stop();
    QCoreApplication::quit();
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    setvbuf(stdout, nullptr, _IONBF, 0);

    const AppConfig cfg = parseArgs(argc, argv);
    if (cfg.help) {
        printUsage(argv[0]);
        return 0;
    }
    if (cfg.video.cam_count != 1 && cfg.video.cam_count != 2) {
        fprintf(stderr, "invalid -n %d (want 1|2)\n", cfg.video.cam_count);
        printUsage(argv[0]);
        return 1;
    }
    if (cfg.video.cam_w <= 0 || cfg.video.cam_h <= 0 ||
        (cfg.video.cam_w & 1) || (cfg.video.cam_h & 1)) {
        fprintf(stderr, "camera size must be positive and even, got %dx%d\n",
                cfg.video.cam_w, cfg.video.cam_h);
        return 1;
    }

    /* 日志（g_log 供 capture/mipi_camera/gpu_util/rga_util 共用） */
    if (video_log_init("/userdata/logs/qmultidisplay.ini", "QMultiDisplay") != 0)
        fprintf(stderr, "log_manager_pro init failed, logging disabled\n");

    /*
     * 显示引擎：全进程唯一 disp_init，按视频格式选视频层，
     * 再挑首个 ARGB 且不与视频层重叠的图层做 UI 层。
     */
    if (disp_engine_init(video_plane_format(&cfg.video), cfg.plane, cfg.alpha) != 0) {
        fprintf(stderr, "[engine] display engine init failed\n");
        return 1;
    }

    /* 采集 + 合成 + 视频图层提交（后台线程） */
    if (video_start(&cfg.video) != 0) {
        fprintf(stderr, "[video] video path start failed\n");
        disp_engine_deinit();
        return 1;
    }

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);

    QApplication app(argc, argv);

    const DispUiInfo *ui = disp_ui_info();
    OverlayWidget widget(ui->width, ui->height, ui->plane_id, ui->alpha8);
    widget.show();

    /* evdev → QMouseEvent（offscreen 平台没有系统输入） */
    EvdevInput input(ui->width, ui->height);
    QObject::connect(&input, &EvdevInput::pointerEvent, &widget,
                     [&widget](int type, int x, int y) {
        QEvent::Type et = QEvent::MouseMove;
        Qt::MouseButtons btns = Qt::NoButton;
        if (type == EvdevInput::PointerPress) {
            et = QEvent::MouseButtonPress;
            btns = Qt::LeftButton;
        } else if (type == EvdevInput::PointerRelease) {
            et = QEvent::MouseButtonRelease;
            btns = Qt::LeftButton;
        }
        QMouseEvent ev(et, QPointF(x, y), QPointF(x, y),
                       Qt::LeftButton, btns, Qt::NoModifier);
        if (widget.windowHandle())
            QCoreApplication::sendEvent(widget.windowHandle(), &ev);
        widget.setPointer(x, y);
    });
    input.start();

    /* 界面有变化才重绘上屏（UpdateRequest），render() 自身的 Paint 不算脏 */
    DirtyFilter filter(qApp);
    qApp->installEventFilter(&filter);

    long uiFrames = 0;
    auto renderFrame = [&]() {
        void *map = nullptr;
        int stride = 0;
        if (disp_ui_begin(&map, &stride) != 0 || !map)
            return;
        const QImage::Format fmt = (ui->blend_mode == DISP_BLEND_PREMULTI)
                                       ? QImage::Format_ARGB32_Premultiplied
                                       : QImage::Format_ARGB32;
        QImage frame((uchar *)map, ui->width, ui->height, stride, fmt);
        frame.fill(Qt::transparent);
        s_rendering = true;
        QPainter painter(&frame);
        widget.render(&painter);
        painter.end();
        s_rendering = false;
        if (disp_ui_commit() == 0)
            uiFrames++;
    };

    QTimer renderTimer;
    renderTimer.start(33);
    QObject::connect(&renderTimer, &QTimer::timeout, &widget, [&]() {
        if (filter.dirty) {
            filter.dirty = false;
            renderFrame();
        }
    });

    QTimer statsTimer;
    statsTimer.start(1000);
    QObject::connect(&statsTimer, &QTimer::timeout, &widget, [&]() {
        VideoStats vs;
        video_get_stats(&vs);
        widget.setFps((int)uiFrames, vs.fps);
        uiFrames = 0;
    });

    const int rc = QApplication::exec();

    input.stop();
    video_stop();          /* 先停视频路径并释放视频缓冲 */
    disp_engine_deinit();  /* 再释放图层/UI 缓冲/DRM 设备 */
    printf("[main] exited, rc=%d\n", rc);
    return rc;
}
