#include "evdev_input.h"

#include <linux/input.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

#define BIT_WORD(b)  ((unsigned)(b) / (8 * sizeof(unsigned long)))
#define BIT_MASK(b)  (1UL << ((unsigned)(b) % (8 * sizeof(unsigned long))))
#define TEST_BIT(arr, b)  ((arr)[BIT_WORD(b)] & BIT_MASK(b))

EvdevInput::EvdevInput(int w, int h, QObject *parent)
    : QObject(parent), m_screenW(w), m_screenH(h),
      m_x(w / 2), m_y(h / 2), m_pressed(false)
{
}

EvdevInput::~EvdevInput()
{
    stop();
}

void EvdevInput::openDevices()
{
    char name[32];
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof(name), "/dev/input/event%d", i);
        int fd = open(name, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        unsigned long evBits[4] = {0, 0, 0, 0};   /* EV_MAX   = 0x1f */
        unsigned long keyBits[12] = {0};         /* KEY_MAX  = 0x2ff */
        unsigned long relBits[1] = {0};          /* REL_MAX  = 0x0f */
        unsigned long absBits[1] = {0};          /* ABS_MAX  = 0x3f */
        ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);

        bool isMouse = TEST_BIT(evBits, EV_REL) && TEST_BIT(relBits, REL_X) &&
                       TEST_BIT(keyBits, BTN_LEFT);
        bool isTouch = TEST_BIT(evBits, EV_ABS) && TEST_BIT(absBits, ABS_X) &&
                       TEST_BIT(keyBits, BTN_TOUCH);
        if (!isMouse && !isTouch) {
            close(fd);
            continue;
        }

        DevInfo info;
        memset(&info, 0, sizeof(info));
        info.fd = fd;
        info.isAbs = isTouch;
        if (isTouch) {
            struct input_absinfo abs;
            if (ioctl(fd, EVIOCGABS(ABS_X), &abs) == 0) {
                info.absMinX = abs.minimum;
                info.absMaxX = abs.maximum;
            }
            if (ioctl(fd, EVIOCGABS(ABS_Y), &abs) == 0) {
                info.absMinY = abs.minimum;
                info.absMaxY = abs.maximum;
            }
        }
        m_devices.push_back(info);
        printf("[input] %s added (%s)\n", name, isTouch ? "touch" : "mouse");
    }
}

void EvdevInput::closeDevices()
{
    for (size_t i = 0; i < m_devices.size(); i++)
        close(m_devices[i].fd);
    m_devices.clear();
}

bool EvdevInput::start()
{
    if (m_running.load())
        return true;
    m_quit.store(false);
    m_running.store(true);
    m_thread = std::thread(&EvdevInput::run, this);
    return true;
}

void EvdevInput::stop()
{
    if (!m_running.load())
        return;
    m_quit.store(true);
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);
}

void EvdevInput::run()
{
    openDevices();

    while (!m_quit.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (size_t i = 0; i < m_devices.size(); i++) {
            FD_SET(m_devices[i].fd, &rfds);
            if (m_devices[i].fd > maxfd)
                maxfd = m_devices[i].fd;
        }
        if (maxfd < 0) {
            usleep(200 * 1000);
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;
        int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0)
            continue;

        bool moved = false;
        for (size_t i = 0; i < m_devices.size(); i++) {
            DevInfo &dev = m_devices[i];
            if (!FD_ISSET(dev.fd, &rfds))
                continue;
            struct input_event ev;
            while (read(dev.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                switch (ev.type) {
                case EV_REL:
                    if (ev.code == REL_X) {
                        m_x += ev.value;
                        moved = true;
                    } else if (ev.code == REL_Y) {
                        m_y += ev.value;
                        moved = true;
                    }
                    break;
                case EV_ABS:
                    if (ev.code == ABS_X) {
                        int span = dev.absMaxX - dev.absMinX;
                        if (dev.isAbs && span > 0)
                            m_x = (ev.value - dev.absMinX) * (m_screenW - 1) / span;
                        moved = true;
                    } else if (ev.code == ABS_Y) {
                        int span = dev.absMaxY - dev.absMinY;
                        if (dev.isAbs && span > 0)
                            m_y = (ev.value - dev.absMinY) * (m_screenH - 1) / span;
                        moved = true;
                    }
                    break;
                case EV_KEY:
                    if ((ev.code == BTN_LEFT || ev.code == BTN_TOUCH) &&
                        m_pressed != (ev.value != 0)) {
                        m_pressed = (ev.value != 0);
                        emit pointerEvent(m_pressed ? PointerPress : PointerRelease,
                                         m_x, m_y);
                    }
                    break;
                default:
                    break;
                }
            }
        }
        if (m_x < 0) m_x = 0;
        if (m_y < 0) m_y = 0;
        if (m_x > m_screenW - 1) m_x = m_screenW - 1;
        if (m_y > m_screenH - 1) m_y = m_screenH - 1;
        if (moved)
            emit pointerEvent(PointerMove, m_x, m_y);
    }

    closeDevices();
}
