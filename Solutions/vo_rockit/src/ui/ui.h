#ifndef __UI_H__
#define __UI_H__

#include "rk_comm_video.h"

/*
 * UI 覆盖层模块 (graphic layer, BGRA8888 逐像素透明)
 *
 * 在视频 layer 之上叠加一个 graphic layer (本板 vp1: 视频 layer1=Cluster1,
 * UI layer5=Esmart1), 内容全部由 CPU 画到一块 DMA buffer 后 SendFrame:
 *   - 左右两窗外侧的绿色边框 (画在视频矩形外侧, 不遮挡画面内容)
 *   - 底部按键栏: logo "EASY EAI" + "SWAP L/R" 对调按键
 *   - 自绘鼠标光标 (RK3588 没有 DRM cursor plane, 需 UI 自画)
 *
 * 鼠标输入通过 evdev 直读 (/dev/input/event*), 点按按键对调左右画面,
 * 复用 display_swap_sources(); 设备拔出后自动重新探测。
 *
 * 初始化失败只记录日志并继续纯视频显示 (UI 降级), 不影响主流程。
 */

/*
 * 初始化 UI 覆盖层
 *   voDev        - VO 设备号 (与 display 一致, 本板 HDMI 为 1)
 *   uiLayer      - UI 图层号 (与视频 layer 同 vp 的 Esmart 窗口: layer1 -> layer5)
 *   width/height - 画布尺寸 (与显示分辨率一致)
 *   winRects/winNum - 视频窗口矩形 (用于画外侧边框), 取 display_get_chn_rects
 * 返回: 0 成功, -1 失败
 */
int ui_init(int voDev, int uiLayer, int width, int height,
		const RECT_S *winRects, int winNum);

/* 启动渲染线程与鼠标输入线程 (非阻塞) */
int ui_start(void);

/* 停止线程并关闭 UI 图层, 未初始化时调用安全 */
void ui_stop(void);

#endif
