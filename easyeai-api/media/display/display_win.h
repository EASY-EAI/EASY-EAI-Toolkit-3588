/**
 * display_win — 多窗口登记与状态管理；合成与上屏均由调用方完成。
 *
 * 与 drm_display 完全解耦：本模块不调用 drm_display 的任何接口，不持有缓冲区，
 * 也不绑定具体合成后端（RGA / GPU / CPU 均可）。本模块只维护窗口槽的几何、层级、
 * 可见性、透明度与已登记帧（dma-buf 引用）；调用方通过 dp_win_list_by_zpos()
 * 按层级顺序遍历窗口、用 dp_win_get_view() 取出快照后自行合成，合成完成后
 * 调 dp_win_clear_dirty() 清除脏标记。
 *
 * 用法：
 *   1. dp_win_init() 传入图层区域宽高，初始化模块；
 *   2. dp_win_add() / dp_win_set_frame() 登记窗口与帧；
 *   3. 需要合成时：dp_win_list_by_zpos() 取按 zpos 升序的窗口下标，逐个
 *      dp_win_get_view() 取快照，自行合成到目标缓冲区并上屏，
 *      完成后 dp_win_clear_dirty()；
 *   4. dp_win_uninit() 释放模块；帧与目标缓冲区生命周期始终归调用方。
 *
 * 模块为进程内单实例，不保证线程安全，多线程使用需调用方自行加锁。
 */

#ifndef DISPLAY_WIN_H
#define DISPLAY_WIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "display_pixfmt.h"

#define DISPLAY_WIN_REG_MAX 8 // 最多登记的窗口数量

typedef struct {
    int zpos;   // 窗口层级，zpos 越大显示越靠前
    int win_x;  // 窗口相对图层区域的起始 X 坐标
    int win_y;  // 窗口相对图层区域的起始 Y 坐标
    int win_w;  // 窗口区域宽度
    int win_h;  // 窗口区域高度
} window_t;

/**
 * 显示帧描述：引用外部图像帧，缓冲区通路二选一（均有效时以 dmabuf_fd 为准）：
 * - dmabuf_fd : 调用方导出的 dma-buf fd（>= 0），合成完成前须保持有效，勿 close；
 * - vir_addr  : 用户态虚拟地址（如 drm mmap 后的地址、malloc 内存），合成完成前
 *               须保持有效。仅供 CPU 软件合成使用；硬件后端（RGA/GPU）能否支持
 *               由调用方自行确认（实测板端 librga 2.1.0 对普通用户态虚拟地址不可用）。
 * - pitch_bytes: 首平面一行字节跨度（与 DRM 的 pitch 一致）。
 * - format     : SDK 像素格式（DisplayPixFmt_t），合成前由调用方自行映射到后端格式。
 */
typedef struct display_frame {
    int dmabuf_fd;
    void *vir_addr;
    int width;
    int height;
    int pitch_bytes;
    int rotation;       // 顺时针旋转角度，支持 0、90、180、270 度
    DisplayPixFmt_t format;
    /* 源裁剪区域（相对帧左上角），宽高 <= 0 时取整帧；用于视频局部放大、精灵图等 */
    int src_x;
    int src_y;
    int src_w;
    int src_h;
} display_frame_t;

/**
 * 窗口合成视图快照：dp_win_get_view() 拷出，调用方据此自行合成。
 * 快照为值拷贝，之后对窗口的修改不影响已取出的快照。
 */
typedef struct {
    window_t win;           // 几何与层级
    bool visible;           // 是否参与合成
    uint8_t alpha;          // 透明度，语义由调用方解释（约定 0 完全透明，255 不透明）
    bool has_frame;         // 是否已登记帧；false 时 frame 字段无效
    display_frame_t frame;  // 已登记的帧
    bool dirty;             // 自上次 dp_win_clear_dirty() 以来是否有变更
} dp_win_view_t;

/**
 * @brief 初始化窗口管理模块（须先于其它接口调用）
 * @param width  图层区域宽度，单位：像素，须 > 0
 * @param height 图层区域高度，单位：像素，须 > 0
 * @return 0 成功，< 0 失败（-EINVAL 参数无效，-EBUSY 已初始化）
 * @note 本模块不绑定合成缓冲区，目标缓冲区由调用方自行申请与管理
 */
int dp_win_init(int width, int height);

/** @brief 释放窗口管理模块，清空全部窗口槽；未初始化时为空操作 */
void dp_win_uninit(void);

/* ---------- 窗口槽：chn 为层内下标 0..DISPLAY_WIN_REG_MAX-1 ---------- */

/**
 * @brief 登记窗口到空闲槽位
 * @param w [in] 窗口描述，需完整落在 init 时的图层区域内
 * @return 成功返回窗口索引（0 至 DISPLAY_WIN_REG_MAX-1），< 0 失败
 *         （-EINVAL 未初始化或参数无效，-ENOMEM 无空闲槽位）
 */
int dp_win_add(const window_t *w);

/**
 * @brief 移除窗口并清空槽位（含已登记的帧与脏标记）
 * @param chn dp_win_add() 返回的窗口索引
 */
void dp_win_remove(int chn);

/**
 * @brief 查询窗口索引是否已占用
 * @param chn 窗口索引
 * @return true 已占用，false 空闲、索引越界或未初始化
 */
bool dp_win_used(int chn);

/**
 * @brief 读取已占用窗口的描述，不转移所有权
 * @param chn 窗口索引
 * @return 成功返回窗口描述指针，空闲、越界或未初始化返回 NULL
 */
const window_t *dp_win_get(int chn);

/** @brief 清空全部窗口槽（含已登记的帧） */
void dp_win_clear_all(void);

/**
 * @brief 将已占用窗口的下标按 zpos 升序写入 chns（先画底层，后画顶层）
 * @param chns [out] 下标数组，容量至少 DISPLAY_WIN_REG_MAX
 * @return 已占用窗口个数；未初始化或 chns 为 NULL 返回 0
 */
int dp_win_list_by_zpos(int *chns);

/**
 * @brief 仅登记窗口帧并标脏，不做任何合成
 * @param chn   dp_win_add() 返回的窗口索引
 * @param frame [in] 显示帧描述，dmabuf_fd 与 vir_addr 至少其一有效；
 *              合成完成前缓冲区须保持有效
 * @return 0 成功，< 0 失败（-EINVAL 窗口未占用或帧参数无效）
 */
int dp_win_set_frame(int chn, const display_frame_t *frame);

/**
 * @brief 帧不变时重新标脏，提示调用方重绘该窗口
 * @param chn 窗口索引
 */
void dp_win_invalidate(int chn);

/**
 * @brief 清除窗口已登记的帧（保留窗口几何与槽位）
 * @param chn 窗口索引
 */
void dp_win_clear_frame(int chn);

/**
 * @brief 修改窗口几何（位置与尺寸），不丢失已登记的帧
 * @param chn     窗口索引
 * @param x,y,w,h 新的窗口区域，须完整落在图层区域内
 * @return 0 成功，< 0 失败（-EINVAL 窗口未占用或区域越界）
 */
int dp_win_set_rect(int chn, int x, int y, int w, int h);

/**
 * @brief 显示/隐藏窗口，保留几何与已登记的帧
 * @param chn     窗口索引
 * @param visible true 显示，false 隐藏（是否参与合成由调用方判断）
 * @note 隐藏的窗口仍占用槽位，重新显示后无需重新 set_frame
 */
void dp_win_set_visible(int chn, bool visible);

/**
 * @brief 修改窗口层级（z-order），zpos 越大越靠前
 * @param chn  窗口索引
 * @param zpos 新的层级值
 * @return 0 成功，-EINVAL 未初始化或窗口无效
 */
int dp_win_set_zpos(int chn, int zpos);

/**
 * @brief 设置窗口透明度属性
 * @param chn   窗口索引
 * @param alpha 约定 0 完全透明，255 不透明；默认 255。具体混合语义由调用方的
 *              合成后端解释（本模块不合成，仅记录属性）
 */
void dp_win_set_alpha(int chn, uint8_t alpha);

/**
 * @brief 取出窗口的合成视图快照（值拷贝）
 * @param chn  窗口索引
 * @param view [out] 快照输出
 * @return true 成功，false 未初始化、窗口未占用、索引越界或 view 为 NULL
 */
bool dp_win_get_view(int chn, dp_win_view_t *view);

/** @brief 清除全部窗口的脏标记（通常在调用方完成一次合成后调用） */
void dp_win_clear_dirty(void);

/**
 * @brief 计算源画面在目标区域内的等比适配矩形（letterbox），结果居中
 * @param area_w/area_h 目标区域宽高，须 > 0
 * @param src_w/src_h   源画面宽高，须 > 0
 * @param out_x/out_y   [out] 适配矩形左上角相对目标区域原点的偏移
 * @param out_w/out_h   [out] 适配矩形宽高
 * @note 纯计算函数，不做硬件操作；参数非法时输出全 0。
 *       常与 window_t 组合，把任意比例画面按比例铺进固定窗口而不变形
 */
void dp_win_fit_rect(int area_w, int area_h, int src_w, int src_h, int *out_x, int *out_y,
    int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
#endif
