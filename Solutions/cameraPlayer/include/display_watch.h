#ifndef DISPLAY_WATCH_H
#define DISPLAY_WATCH_H

#include <stdbool.h>

/*
 * 显示通路监控：运行期间按固定周期查询连接器与图层实时状态。
 *
 * disp_commit() 返回 0 只说明内核接受了这次提交，不说明画面还在上屏：桌面 DPMS 息屏
 * 关掉 CRTC、或图层被别的客户端重配摘下时，提交照样成功而屏幕是黑的——实测这与本进程
 * 是不是 DRM master 无关。只按返回码判断会让程序日志看着完全正常而屏幕全黑，因此运行
 * 循环里按固定周期向驱动现场查询连接器与图层状态，只在基线和状态翻转时打日志。
 *
 * 连接器查询会触发驱动探测（含 DDC 访问），必须低频，不能放进每帧以外的密集路径；
 * DISPLAY_WATCH_POLL_SEC 就是这个限频周期。
 */
#define DISPLAY_WATCH_POLL_SEC 2.0

/* 一次提交的输出通路最多涉及两个图层。 */
#define DISPLAY_WATCH_MAX_PLANES 2

/**
 * 登记要监控的连接器与图层，并把状态复位成"假定正常"。
 * 不做任何查询：基线由运行循环第一次 display_watch_tick() 取，那时已经提交过首帧，
 * 图层绑定状态才有意义，提前查询会把"还没提交第一帧"误报成输出中断。
 *
 * @param conn_index  disp_init() 分配的连接器索引
 * @param planes      本进程持有的图层句柄数组，< 0 的槽位忽略
 * @param plane_count planes 有效长度，不超过 DISPLAY_WATCH_MAX_PLANES
 */
void display_watch_start(int conn_index, const int *planes, int plane_count);

/**
 * 到达限频周期时刷新一次显示通路状态，并在基线或状态翻转时打日志。
 * 每个运行循环迭代都可以无条件调用，未到周期的调用是廉价的时刻比较。
 *
 * @return 当前画面是否真正在上屏（未查询时返回上一次判定）
 */
bool display_watch_tick(void);

/**
 * 供统计行拼接的状态片段，形如 "scanout=on dpms=ON"。
 *
 * @return 只读字符串，下次调用前有效
 */
const char *display_watch_state_str(void);

/**
 * 把提交类接口的失败码翻译成可打印的原因：失败时可能返回 -1 并置 errno，
 * 也可能直接返回负的 errno。
 *
 * @param ret 接口返回值
 * @return strerror 文本；无有效错误码时为 "unknown"
 */
const char *drm_errstr(int ret);

/**
 * 取提交类接口失败码对应的 errno 值，用于区分 EBUSY（NONBLOCK 未排空）等可重试情形。
 *
 * @param ret 接口返回值
 * @return 错误码；ret 非失败时为 0
 */
int drm_errcode(int ret);

#endif /* DISPLAY_WATCH_H */
