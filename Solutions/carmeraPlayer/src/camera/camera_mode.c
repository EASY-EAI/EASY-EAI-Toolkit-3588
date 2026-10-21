#include "camera_mode.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int video_num;
    const char *media_dev;
    const char *sensor_4k;
    const char *video_dev;
} CameraRoute;

/* 两路 ISP 主路径与对应 CIF media 设备、IMX415 entity 的固定映射。 */
static const CameraRoute routes[] = {
    { 44, "/dev/media2", "\"m00_b_imx415 3-0036\":0[fmt:SGBRG12_1X12/3864x2192]", "/dev/video44" },
    { 53, "/dev/media3", "\"m01_b_imx415 7-0036\":0[fmt:SGBRG12_1X12/3864x2192]", "/dev/video53" },
};

/* 用参数数组启动工具，避免把 entity 名称交给 shell 再解析。 */
static int run_status(char *const argv[])
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    do {
        if (waitpid(pid, &status, 0) >= 0)
            break;
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    } while (1);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run(char *const argv[])
{
    int status = run_status(argv);
    if (status == 0)
        return 0;
    fprintf(stderr, "%s failed (status %d)\n", argv[0], status);
    return -1;
}

static int aiq_service(const char *action)
{
    char *const argv[] = { "systemctl", (char *)action, "rkaiq_3A.service", NULL };
    return run(argv);
}

static int set_sensor(const CameraRoute *route)
{
    char *const argv[] = { "media-ctl", "-d", (char *)route->media_dev, "-V",
                           (char *)route->sensor_4k, NULL };
    return run(argv);
}

static int set_output(const CameraRoute *route)
{
    char *const argv[] = { "v4l2-ctl", "-d", (char *)route->video_dev,
                           "--set-fmt-video=width=3840,height=2160,pixelformat=NV12",
                           NULL };
    return run(argv);
}

int camera_mode_prepare(int width, int height, const int *video_nums, int count)
{
    const CameraRoute *selected[2];

    /* 默认 1080p 沿用板上的模式；仅完整 4K 输出需要主动切 sensor。 */
    if (width != 3840 || height != 2160)
        return 0;
    if (count < 1 || count > 2)
        return -1;

    for (int i = 0; i < count; ++i) {
        const CameraRoute *route = NULL;
        for (unsigned j = 0; j < sizeof(routes) / sizeof(routes[0]); ++j) {
            if (video_nums[i] == routes[j].video_num) {
                route = &routes[j];
                break;
            }
        }
        if (!route) {
            fprintf(stderr, "4K mode unsupported on /dev/video%d\n", video_nums[i]);
            return -1;
        }
        selected[i] = route;
    }

    /* 模式切换前停止 3A，避免它继续使用旧 sensor/ISP 配置。 */
    if (aiq_service("stop") != 0)
        return -1;

    /* 设置 sensor 和 ISP 输出成功后再启动 3A；反向顺序曾导致 ISP 超时。 */
    fprintf(stderr, "configuring IMX415 4K RAW12 before starting 3A\n");
    for (int i = 0; i < count; ++i) {
        if (set_sensor(selected[i]) != 0 || set_output(selected[i]) != 0)
            return -1;
    }
    if (aiq_service("start") != 0)
        return -1;
    /* 等待 3A 初始化完成，再让采集线程执行 STREAMON。 */
    sleep(2);
    return 0;
}
