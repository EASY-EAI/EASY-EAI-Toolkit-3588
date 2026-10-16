#ifndef VO_DEMO_STATS_H
#define VO_DEMO_STATS_H

/**
 * 状态栏展示用的运行统计快照。
 * 单位：fps 为帧/秒；cpu / mem_percent / rga / gpu 为百分比；mem_*_mb 为兆字节。
 * rga / gpu 始终采样，与 -C 选定的合成后端无关。
 */
typedef struct {
    float fps;
    int display_w;
    int display_h;
    float cpu;
    long mem_used_mb;
    long mem_total_mb;
    float mem_percent;
    float rga; /* RGA 各 scheduler 的 load% 最大值；< 0 表示读取失败 */
    float gpu; /* Mali GPU 占用率（%）；< 0 表示读取失败 */
} DemoStats;

/**
 * 统计采样上下文。调用方持有一份，经 stats_init 后反复传给 stats_update。
 */
typedef struct {
    double last_time;
    unsigned long last_frames;
    unsigned long long prev_total;
    unsigned long long prev_idle;
} StatsContext;

/**
 * 初始化统计采样上下文。
 *
 * @param ctx  待初始化的上下文，不可为 NULL
 */
void stats_init(StatsContext *ctx);

/**
 * 按约一秒周期刷新 FPS、CPU、内存、RGA 与 GPU 占用到 out。
 * 与 -C 选定的合成后端无关：RGA / GPU 始终采样，状态栏三项都常驻显示。
 *
 * @param ctx     已 stats_init 的采样上下文
 * @param out     输出快照；调用前可预先填好 display_w/h 等静态字段
 * @param frames  自启动以来已成功显示的累计帧数
 * @return 1 表示 out 中的动态字段已更新；0 表示尚未到下一次采样时间
 */
int stats_update(StatsContext *ctx, DemoStats *out, unsigned long frames);

#endif /* VO_DEMO_STATS_H */
