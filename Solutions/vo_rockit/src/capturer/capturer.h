#ifndef __CAPTURER_H__
#define __CAPTURER_H__

#include <stdbool.h>
#include <pthread.h>

/*
 * VI 取流模块 (可实例化, 支持多路摄像头)
 *
 * 每个 Capturer_t 实例对应一路 VI dev/chn, 由独立采集线程持续 GetChnFrame,
 * 取到的帧不在此模块显示, 而是以零拷贝方式交给 player 模块中
 * camIdx 对应的帧队列, 由显示线程 SendFrame 到 VO 后再归还 VI buffer。
 */

typedef struct {
	int devId;          /* VI 设备号 */
	int pipeId;         /* VI pipe 号, 本板与 devId 相同 */
	int chnId;          /* VI 通道号 */
	int camIdx;         /* 显示端窗口索引: 0=左窗, 1=右窗 */
	int width;
	int height;
	bool devInited;
	bool chnInited;
	bool running;
	pthread_t tid;
} Capturer_t;

/*
 * 初始化一路 VI 设备与通道
 *   cap          - 实例 (调用方分配, 建议 static/全局)
 *   devId        - VI 设备号, 第 0 路摄像头用 0, 第 1 路用 1
 *   chnId        - VI 通道号 (对应 -I 参数)
 *   camIdx       - 显示窗口索引 (0=左, 1=右)
 *   width/height - 采集分辨率
 * 返回: 0 成功, -1 失败
 */
int capturer_init(Capturer_t *cap, int devId, int chnId, int camIdx, int width, int height);

/*
 * 启动采集线程 (非阻塞)
 * 线程内持续取帧并推入 display 对应 camIdx 的帧队列
 * 返回: 0 成功, -1 失败
 */
int capturer_start(Capturer_t *cap);

/*
 * 停止采集线程并关闭 VI 通道/设备
 * 对未初始化的实例调用是安全的(直接返回), 可无差别用于清理路径
 */
void capturer_stop(Capturer_t *cap);

#endif
