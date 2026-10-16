/**
 *
 * Copyright 2025 by Guangzhou Easy EAI Technologny Co.,Ltd.
 * website: www.easy-eai.com
 *
 * Author: ZJH <zhongjiehao@easy-eai.com>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * License file for more details.
 *
 */

#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include <stddef.h>
#include <stdint.h>
#include <drm_fourcc.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    size_t fbSize;       // 映射缓冲区大小，单位：字节；导入的 DMA-BUF 未映射时为 0
    int fbStride;        // 每行跨度，单位：字节；导入 NV12 时仅表示 Y 平面的跨度
    int dmabuf_fd;       // 模块持有的 DMA-BUF fd，未导出时为 -1；调用方不可关闭
    char *pFramebuffer; // mmap 映射地址；导入的 DMA-BUF 未映射时为 NULL
}Drmbuff_t;

typedef struct {
    int dmabuf_fd;        // 调用方导出的 DMA-BUF 文件描述符，接口只复制不接管
    int width;            // framebuffer 宽度，单位：像素
    int height;           // framebuffer 高度，单位：像素
    uint32_t format;      // DRM_FORMAT_* 像素格式
    uint32_t pitches[4];  // 各数据平面每行的字节数；RGB 用第 0 项，NV12 用前 2 项
    uint32_t offsets[4];  // 各数据平面在 DMA-BUF 中的字节偏移，未使用项填 0
} DrmImportDesc_t;

#define DRM_MAX_CONNECTORS 12    // 单次 DRM 初始化最多记录的连接器数量
#define DRM_MAX_PLANES 8         // 每个连接器最多记录的可用图层数量
#define DRM_MAX_PLANE_FORMATS 24 // 每个图层最多记录的像素格式数量（RK3588 VOP2 实测单 plane 上限为 21）
#define DRM_MAX_BUFFERS 16       // 单次 DRM 初始化最多登记的显示缓冲区数量
typedef struct {
    uint32_t id;       // DRM 图层对象 ID，供 drm_init_planes() 选择
    int type;          // 图层类型，为 DRM_PLANE_TYPE_*
    int formatCount;   // 该图层支持的像素格式数量（RK3588 VOP2 实测为 14 或 21）
    // 该图层支持的 DRM_FORMAT_* 像素格式列表，有效项为前 formatCount 项；
    // 调用方据此按格式能力挑图层，例如是否支持 NV12、是否支持 ARGB8888 全屏画布
    uint32_t formats[DRM_MAX_PLANE_FORMATS];
} Display_plane_t;

typedef struct {
    int connectorCount; // DRM 枚举到的连接器数量，包含未启用的连接器
    struct {
        int enabled;       // 已连接、有效模式且分配到 CRTC 时为 1
        int index;         // 本次初始化分配的索引，供其他 DRM 接口使用
        int screenWidth;   // 当前选用模式的水平分辨率，单位：像素
        int screenHeight;  // 当前选用模式的垂直分辨率，单位：像素
        int screenRefresh; // 当前选用模式的刷新率，单位：Hz
        int planeCount;    // 当前连接器可绑定的图层数量
        Display_plane_t planes[DRM_MAX_PLANES]; // 可用前 planeCount 项
    } connectors[DRM_MAX_CONNECTORS]; // 下标与 index 一致
}Display_dev_t;

/**
 * @brief 初始化 DRM 显示设备，枚举连接器并填充其可绑定的图层及格式能力
 * @param pDisplay [out] 显示设备结构，成功时填充，失败时清零
 * @return 0 成功，< 0 失败（-EINVAL 参数无效，-EBUSY 已初始化，-1 打开设备或获取资源失败）
 * @note 每个图层的 id/type 与支持的像素格式列表（formats/formatCount）随枚举一并填充，
 *       调用方可据此按格式能力挑选图层后再交给 drm_init_planes()
 */
int drm_init(Display_dev_t *pDisplay);

/**
 * @brief 释放 DRM 资源，兜底释放仍登记的图层与缓冲区，并关闭 DRM 设备
 * @param pDisplay [in/out] 显示设备结构，返回时清零
 */
void drm_uninit(Display_dev_t *pDisplay);

/**
 * @brief 按图层 ID 初始化图层，仅支持已分配 CRTC 的连接器
 * @param conn_index drm_init() 分配的连接器索引
 * @param plane_id   DRM 图层对象 ID，取自 connectors[conn_index].planes[]
 * @return 成功返回内部图层数组索引（0 至 DRM_MAX_PLANES-1），< 0 失败
 * @note 同一连接器可多次调用以初始化多个图层；同一图层重复初始化会失败，释放后索引可被重新分配
 */
int drm_init_planes(int conn_index, uint32_t plane_id);

/**
 * @brief 释放内部 DRM 图层引用；显示缓冲区由调用方通过 drm_free_buffer() 释放
 * @param handle drm_init_planes() 返回的图层索引
 */
void drm_uninit_planes(int handle);

/**
 * @brief 设置图层的 framebuffer 源区域，仅保存到图层句柄，实际提交由 drm_commit() 完成
 * @param handle     图层索引
 * @param src_x      源区域水平起点，位于 framebuffer 内
 * @param src_y      源区域垂直起点，位于 framebuffer 内
 * @param src_width  源区域宽度
 * @param src_height 源区域高度
 * @return 0 成功，< 0 失败（尺寸可与 CRTC 目标区域不同以实现缩放，实际边界在提交时与缓冲区尺寸一同校验）
 * @note 重复调用以最后一次配置为准
 */
int drm_plane_set_src_region(int handle, int src_x, int src_y, int src_width, int src_height);

/**
 * @brief 设置图层在连接器 CRTC 内的目标显示区域，仅保存到图层句柄，实际提交由 drm_commit() 完成
 * @param handle      图层索引
 * @param crtc_x      目标区域水平起点
 * @param crtc_y      目标区域垂直起点
 * @param crtc_width  目标区域宽度
 * @param crtc_height 目标区域高度
 * @return 0 成功，< 0 失败（区域超出当前显示模式分辨率时返回 -EINVAL）
 * @note 重复调用以最后一次配置为准
 */
int drm_plane_set_crtc_region(int handle, int crtc_x, int crtc_y, int crtc_width, int crtc_height);

/**
 * @brief 设置图层叠放顺序，立即通过 atomic commit 生效
 * @param handle 图层索引
 * @param zpos   叠放顺序值，数值越大通常越靠前，实际范围由驱动决定
 * @return 0 成功，< 0 失败
 */
int drm_plane_set_zpos(int handle, uint32_t zpos);

/**
 * @brief 设置图层整体透明度，立即通过 atomic commit 生效
 * @param handle 图层索引
 * @param alpha  透明度，通常 0 表示全透明、65535 表示不透明
 * @return 0 成功，< 0 失败
 */
int drm_plane_set_alpha(int handle, uint16_t alpha);

/**
 * @brief 设置图层像素混合模式，立即通过 atomic commit 生效
 * @param handle 图层索引
 * @param mode   混合模式，取 DRM "pixel blend mode" 属性枚举值：
 *               0 (DRM_MODE_BLEND_PREMULTI)  预乘 Alpha 混合；
 *               1 (DRM_MODE_BLEND_COVERAGE)  非预乘 Alpha 混合；
 *               2 (DRM_MODE_BLEND_PIXEL_NONE) 不做 Alpha 混合
 * @return 0 成功，< 0 失败
 * @note 宏定义见内核 drm_mode.h；实际支持的枚举项由驱动决定，传入不支持的值时由驱动拒绝
 */
int drm_plane_set_blend_mode(int handle, uint64_t mode);


/**
 * @brief 按实际图像尺寸申请显示缓冲区，支持 DRM_FORMAT_ARGB8888、DRM_FORMAT_XRGB8888 和 DRM_FORMAT_NV12
 * @param width  缓冲区宽度，单位：像素（NV12 要求为偶数）
 * @param height 缓冲区高度，单位：像素（NV12 要求为偶数）
 * @param format DRM_FORMAT_* 像素格式
 * @return 成功返回内部缓冲区数组索引（0 至 DRM_MAX_BUFFERS-1），< 0 失败
 * @note 需在 drm_init() 成功后申请；NV12 的 UV 平面从 pFramebuffer + fbStride * height 开始；
 *       摄像头 DMA-BUF 应改用 drm_import_dmabuf() 导入
 */
int drm_alloc_buffer(int width, int height, uint32_t format);

/**
 * @brief 导入外部 DMA-BUF 作为显示缓冲区
 * @param desc [in] 导入描述，需填好 dmabuf_fd、尺寸、格式及各平面 pitches/offsets
 * @return 成功返回缓冲区索引（0 至 DRM_MAX_BUFFERS-1），< 0 失败
 * @note NV12 要求宽高为偶数、pitches[1] 非 0 且 offsets[1] 不早于 offsets[0]；
 *       导入的缓冲区不做 mmap（fbSize 为 0、pFramebuffer 为 NULL）；
 *       接口只复制 desc->dmabuf_fd，原始 fd 仍由调用方负责关闭
 */
int drm_import_dmabuf(const DrmImportDesc_t *desc);

/**
 * @brief 按索引读取缓冲区信息，不转移 fd 与映射地址的所有权
 * @param buffer_handle 缓冲区索引
 * @param info          [out] 缓冲区信息
 * @return 0 成功，< 0 失败（索引无效或 info 为空）
 * @note fd 与映射地址仅在缓冲区释放前有效，调用方不可自行关闭或解除映射
 */
int drm_get_buffer_info(int buffer_handle, Drmbuff_t *info);

/**
 * @brief 释放缓冲区，移除帧缓冲并收回内部 fd 与映射
 * @param buffer_handle 缓冲区索引
 * @return 0 成功，< 0 失败
 * @note 释放后索引可被重新分配，调用方不得继续使用旧句柄或此前读取的缓冲区信息
 */
int drm_free_buffer(int buffer_handle);

/**
 * @brief 将缓冲区按图层当前区域配置提交显示
 * @param handle        图层索引
 * @param buffer_handle 缓冲区索引
 * @return 0 成功，< 0 失败（属性校验失败返回负错误码；atomic commit 失败返回 -1，原因见 errno）
 * @note 提交前必须成功调用两个区域接口；源区域需落在缓冲区尺寸内，
 *       NV12 还要求源区域坐标与尺寸均为偶数
 */
int drm_commit(int handle, int buffer_handle);


#ifdef __cplusplus
}
#endif

#endif /* DRM_DISPLAY_H */

