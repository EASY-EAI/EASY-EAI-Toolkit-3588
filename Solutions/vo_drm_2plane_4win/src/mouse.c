#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "mouse.h"

struct MouseState {
    volatile int x;
    volatile int y;
    volatile int click;
    volatile int click_x;
    volatile int click_y;
    pthread_t thread;
    bool running;
    bool joined;
};

static int mouse_probe(void)
{
    char path[64];
    unsigned char ev_bits[(EV_MAX + 7) / 8];
    unsigned char rel_bits[(REL_MAX + 7) / 8];
    for (int i = 0; i < 32; ++i) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        memset(ev_bits, 0, sizeof(ev_bits));
        memset(rel_bits, 0, sizeof(rel_bits));
        if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) >= 0 &&
            ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) >= 0 &&
            (ev_bits[EV_REL / 8] & (1 << (EV_REL % 8))) &&
            (rel_bits[REL_X / 8] & (1 << (REL_X % 8))))
            return fd;
        close(fd);
    }
    return -1;
}

static void *mouse_thread(void *arg)
{
    MouseState *m = arg;
    while (m->running) {
        int fd = mouse_probe();
        if (fd < 0) {
            usleep(200000);
            continue;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        struct input_event ev;
        while (m->running && poll(&pfd, 1, 500) > 0) {
            while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_REL && ev.code == REL_X)
                    m->x += ev.value;
                if (ev.type == EV_REL && ev.code == REL_Y)
                    m->y += ev.value;
                if (ev.type == EV_KEY && ev.code == BTN_LEFT && ev.value == 1) {
                    m->click_x = m->x;
                    m->click_y = m->y;
                    m->click = 1;
                }
            }
        }
        close(fd);
    }
    return NULL;
}

MouseState *mouse_create(void)
{
    MouseState *m = calloc(1, sizeof(*m));
    if (m)
        m->joined = true;
    return m;
}

void mouse_destroy(MouseState *m)
{
    if (!m)
        return;
    mouse_stop(m);
    free(m);
}

int mouse_start(MouseState *m)
{
    if (!m)
        return -1;
    if (m->running)
        return 0;
    m->running = true;
    m->joined = false;
    if (pthread_create(&m->thread, NULL, mouse_thread, m) != 0) {
        m->running = false;
        m->joined = true;
        return -1;
    }
    return 0;
}

void mouse_stop(MouseState *m)
{
    if (!m || m->joined)
        return;
    m->running = false;
    pthread_join(m->thread, NULL);
    m->joined = true;
    memset(&m->thread, 0, sizeof(m->thread));
}

void mouse_get_pos(const MouseState *m, int *x, int *y)
{
    if (!m)
        return;
    if (x)
        *x = m->x;
    if (y)
        *y = m->y;
}

void mouse_set_pos(MouseState *m, int x, int y)
{
    if (!m)
        return;
    m->x = x;
    m->y = y;
}

int mouse_consume_click(MouseState *m, int *x, int *y)
{
    if (!m || !m->click)
        return 0;
    if (x)
        *x = m->click_x;
    if (y)
        *y = m->click_y;
    m->click = 0;
    return 1;
}
