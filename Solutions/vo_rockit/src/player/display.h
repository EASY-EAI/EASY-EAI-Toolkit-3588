#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <stdbool.h>

#include "rk_mpi_vi.h"

/* 同一 layer 上的通道数: chn0=左窗, chn1=右窗 */
#define DISPLAY_CHN_NUM 2

/*
 * VO 显示模块 (单 layer 双通道: 左右两窗)
 *
 * 在同一个 VO layer 上启用 chn0/chn1 两个通道, 各占一个显示矩形:
 * rockit 的 VO layer 支持一层多通道 (VO_PART_MODE_SINGLE:
 * "use software to make multi-picture in one hardware cell"),
 * 每通道独立 stRect, 画面缩放由 composer 完成。
 *
 * 每个通道一个独立帧队列 + 独立显示线程:
 * 采集线程入队, 显示线程出队 SendFrame 到对应通道后归还 VI buffer。
 * 队列深度为 2, 采集与显示可并行执行, 消除串行等待。
 */

/*
 * 初始化 VO
 *   voLayer  - VO 图层号 (对应 -l 参数, 本板 HDMI(vp1) 用 1 或 5)
 *   voDev    - VO 设备号 (对应 -d 参数, 本板 HDMI 为 1)
 *   width    - 显示宽
 *   height   - 显示高
 *   intfType - 输出接口类型 (VO_INTF_MIPI/HDMI/LCD/DEFAULT)
 *   camW/camH- 摄像头采集分辨率, 用于按宽高比计算左右窗矩形
 * 返回: 0 成功, -1 失败
 */
int display_init(int voLayer, int voDev, int width, int height, int intfType,
		int camW, int camH);

/*
 * 启动显示线程 (非阻塞, 每通道一个线程), 从各自帧队列取帧并 SendFrame 到 VO
 * 返回: 0 成功, -1 失败
 */
int display_start(void);

/*
 * 停止显示线程并关闭 VO
 * 队列中尚未显示的残留帧会被归还给 VI
 */
void display_stop(void);

/*
 * 采集线程调用: 将一帧 VI 帧零拷贝推入 camIdx 对应通道的显示帧队列
 * 队列满时阻塞等待, 由显示线程消费后唤醒
 * 镜像模式(display_set_mirror)下统一进队列 0
 * 返回: 0 成功, -1 表示显示已停止(调用方需自行归还该帧)
 */
int display_push_frame(int camIdx, int pipeId, int chnId, VIDEO_FRAME_INFO_S *frame);

/*
 * 单摄镜像模式: 只有一路采集时, 两个显示线程共用队列 0,
 * 同一路画面轮流送左右两窗 (第二路摄像头未接入时验证一层多通道用)
 * 必须在 display_start 之前调用
 */
void display_set_mirror(bool on);

/*
 * 对调左右画面 (交换采集源到通道的映射, 不改通道矩形, 下一帧生效)
 * 线程安全; 当前用于 kill -USR1 触发, 鼠标按键复用同一入口
 */
void display_swap_sources(void);

/*
 * 取左右两窗的显示矩形 (ui 模块画边框用)
 * 返回: 0 成功, -1 表示 display 尚未初始化
 */
int display_get_chn_rects(RECT_S rects[DISPLAY_CHN_NUM]);

#endif
