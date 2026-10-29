/**
 * evdev_input — evdev 输入读取线程
 *
 * offscreen 平台下 Qt 不会自动接入输入设备，这里从 /dev/input/event*
 * 读取鼠标（EV_REL）与触摸屏（EV_ABS）事件，换算成屏幕坐标后以信号
 * 形式投递给 Qt 主线程，由主线程包装成 QMouseEvent 发给顶层 QWindow。
 */

#ifndef EVDEV_INPUT_H
#define EVDEV_INPUT_H

#include <QObject>
#include <atomic>
#include <thread>
#include <vector>

class EvdevInput : public QObject
{
    Q_OBJECT

public:
    /* 事件类型 */
    enum EventType {
        PointerMove = 0,
        PointerPress,
        PointerRelease
    };

    explicit EvdevInput(int screenW, int screenH, QObject *parent = nullptr);
    ~EvdevInput() override;

    bool start();
    void stop();

signals:
    void pointerEvent(int type, int x, int y);

private:
    struct DevInfo {
        int fd;
        bool isAbs;   /* true: 触摸屏(EV_ABS) false: 鼠标(EV_REL) */
        int absMinX, absMaxX, absMinY, absMaxY;
    };

    void run();
    void openDevices();
    void closeDevices();

    int m_screenW, m_screenH;
    std::vector<DevInfo> m_devices;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_quit{false};
    int m_x, m_y;
    bool m_pressed;
};

#endif /* EVDEV_INPUT_H */
