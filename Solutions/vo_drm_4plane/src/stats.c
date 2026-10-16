#define _GNU_SOURCE

/**
 * 运行统计采样实现。
 *
 * RGA / GPU 占用始终采样，与 -C 选定的合成后端无关：
 *   - GPU：/sys/class/devfreq/fb000000.gpu/load -> out->gpu
 *   - RGA：/sys/kernel/debug/rkrga/load（多 scheduler 取最大 load%）-> out->rga
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "stats.h"

#define GPU_UTIL_PATH "/sys/class/devfreq/fb000000.gpu/load"
#define RGA_LOAD_PATH "/sys/kernel/debug/rkrga/load"

void stats_init(StatsContext *ctx)
{
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
}

static float read_gpu_util_percent(void)
{
    FILE *fp = fopen(GPU_UTIL_PATH, "r");
    int util = -1;

    if (!fp)
        return -1.f;
    if (fscanf(fp, "%d", &util) != 1)
        util = -1;
    fclose(fp);
    if (util < 0)
        return -1.f;
    if (util > 100)
        util = 100;
    return (float)util;
}

/**
 * 解析 rkrga load：取各 scheduler 的 load 最大值。
 * 文件格式示例：
 *   scheduler[0]: rga3
 *            load = 12%
 */
static float read_rga_util_percent(void)
{
    FILE *fp = fopen(RGA_LOAD_PATH, "r");
    char line[128];
    int max_load = -1;

    if (!fp)
        return -1.f;
    while (fgets(line, sizeof(line), fp)) {
        const char *p = strstr(line, "load");
        int v;

        if (!p)
            continue;
        p = strchr(p, '=');
        if (!p)
            continue;
        if (sscanf(p + 1, " %d%%", &v) == 1) {
            if (v > max_load)
                max_load = v;
        }
    }
    fclose(fp);
    if (max_load < 0)
        return -1.f;
    if (max_load > 100)
        max_load = 100;
    return (float)max_load;
}

int stats_update(StatsContext *ctx, DemoStats *out, unsigned long frames)
{
    struct timespec now;
    FILE *fp;
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total, idle_all;

    if (!ctx || !out)
        return 0;

    clock_gettime(CLOCK_MONOTONIC, &now);
    double t = now.tv_sec + now.tv_nsec / 1e9;
    if (ctx->last_time > 0.0) {
        double dt = t - ctx->last_time;
        if (dt < 1.0)
            return 0;
        out->fps = (float)((double)(frames - ctx->last_frames) / dt);
    }
    ctx->last_time = t;
    ctx->last_frames = frames;

    fp = fopen("/proc/stat", "r");
    if (fp && fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                     &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) == 8) {
        total = user + nice + system + idle + iowait + irq + softirq + steal;
        idle_all = idle + iowait;
        if (ctx->prev_total && total > ctx->prev_total)
            out->cpu = (float)(100.0 * (double)(total - ctx->prev_total - (idle_all - ctx->prev_idle)) /
                               (double)(total - ctx->prev_total));
        ctx->prev_total = total;
        ctx->prev_idle = idle_all;
        fclose(fp);
    } else if (fp) {
        fclose(fp);
    }

    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        long total_kb = 0, avail_kb = 0;
        char key[32];
        long value;
        while (fscanf(fp, "%31s %ld kB", key, &value) == 2) {
            if (!strcmp(key, "MemTotal:"))
                total_kb = value;
            else if (!strcmp(key, "MemAvailable:"))
                avail_kb = value;
        }
        if (total_kb) {
            out->mem_total_mb = total_kb / 1024;
            out->mem_used_mb = (total_kb - avail_kb) / 1024;
            out->mem_percent = (float)(100.0 * (double)(total_kb - avail_kb) /
                                       (double)total_kb);
        }
        fclose(fp);
    }

    /* RGA / GPU 占用始终采样，与 -C 选定的合成后端无关。 */
    out->rga = read_rga_util_percent();
    out->gpu = read_gpu_util_percent();

    return 1;
}
