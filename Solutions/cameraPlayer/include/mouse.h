#ifndef MOUSE_H
#define MOUSE_H

/**
 * 鼠标输入会话的不透明句柄。
 * 由 mouse_create 分配，mouse_destroy 释放。
 */
typedef struct MouseState MouseState;

/**
 * 创建鼠标输入会话。
 *
 * @return 非 NULL 成功；NULL 表示内存分配失败
 */
MouseState *mouse_create(void);

/**
 * 销毁鼠标会话并释放资源。
 * 若会话仍在运行，会先停止输入监视。
 *
 * @param m  mouse_create 返回值；允许传 NULL
 */
void mouse_destroy(MouseState *m);

/**
 * 启动鼠标输入监视。
 *
 * @param m  有效的 MouseState
 * @return 0 成功（含已处于运行状态）；负值启动失败
 */
int mouse_start(MouseState *m);

/**
 * 停止鼠标输入监视并等待监视结束。
 *
 * @param m  有效的 MouseState；未启动时为空操作
 */
void mouse_stop(MouseState *m);

/**
 * 读取当前指针坐标。
 *
 * @param m  有效的 MouseState
 * @param x  输出 X，可为 NULL
 * @param y  输出 Y，可为 NULL
 */
void mouse_get_pos(const MouseState *m, int *x, int *y);

/**
 * 设置当前指针坐标（例如初始化到屏幕中心）。
 *
 * @param m  有效的 MouseState
 * @param x  新的 X
 * @param y  新的 Y
 */
void mouse_set_pos(MouseState *m, int x, int y);

/**
 * 若存在未处理的左键点击，则取出并清除该事件。
 *
 * @param m  有效的 MouseState
 * @param x  输出点击时的 X，可为 NULL
 * @param y  输出点击时的 Y，可为 NULL
 * @return 1 取出一次点击；0 当前无待处理点击
 */
int mouse_consume_click(MouseState *m, int *x, int *y);

#endif /* MOUSE_H */
