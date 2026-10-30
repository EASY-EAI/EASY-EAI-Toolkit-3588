/**
 * video_stitch — cameraPlayer 显示通路的移植
 *
 * 原 stitch_main.c 去掉：disp_init/选层/双缓冲申请（改经 disp_engine 共享
 * 引擎）、display_watch 健康监测、getopt/信号处理（归 Qt main.cpp）。
 * 保留：V4L2 采集线程、单摄 NV12 dma-buf 零拷贝逐帧导入、双目 RGA/GPU
 * 半宽拼接双缓冲轮转、每秒统计。
 *
 * 线程模型：video_start/video_stop 由主线程调用；采集线程 + 视频线程在
 * 后台运行；所有 DRM 访问经 disp_engine 内互斥锁。
 */

#ifndef VIDEO_STITCH_H
#define VIDEO_STITCH_H

#include "display_pro.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 视频路径运行配置（main.cpp 解析参数后填好传入） */
typedef struct {
    int cam_count;       /* 1|2 路摄像头 */
    int cam_w;           /* 每路采集宽（像素，须偶数），默认 1920 */
    int cam_h;           /* 每路采集高（像素，须偶数），默认 1080 */
    int compose_gpu;     /* 双目合成后端：1=GPU（默认） 0=RGA */
    int video_nums[2];   /* 显式 /dev/videoN；-1 按 media 拓扑自动发现 */
} VideoConfig;

/** 最近一次统计快照（视频线程每秒刷新，主线程读取） */
typedef struct {
    double fps;               /* <0 表示尚未统计 */
    unsigned long frames_total;
    unsigned long errors_total;
} VideoStats;

/** 初始化日志管理器并注册本模块句柄；mgr 由 video_start 传给各 media 模块 init */
int video_log_init(const char *config_file, const char *module);

/**
 * 请求停止：置 g_quit 并唤醒采集队列。
 * SIGINT/SIGTERM 处理器可调用（语义同 stitch 的 on_signal）。
 */
void video_request_stop(void);

/**
 * 按配置启动采集 + 合成 + 视频图层提交（后台线程），0 成功。
 * 须在 disp_engine_init() 之后调用。
 */
int video_start(const VideoConfig *cfg);

/** 停止并回收全部视频线程与缓冲（须在 disp_engine_deinit() 之前调用） */
void video_stop(void);

/** 读取最近一次统计快照（线程安全） */
void video_get_stats(VideoStats *out);

/** 由配置推视频图层所需像素格式（disp_engine_init 之前调用）：
 *  双目 GPU 合成输出 ARGB8888，其余为 NV12 */
DispPixFmt_t video_plane_format(const VideoConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_STITCH_H */
