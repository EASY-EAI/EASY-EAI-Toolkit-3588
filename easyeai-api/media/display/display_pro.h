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

/**
 * display_pro — DRM/KMS 显示后端，进程内单实例，不保证线程安全。
 *
 * 接口按功能分区：设备生命周期 → 图层 → 缓冲区 → 提交 → 状态与事件 → DRM master。
 * 前四区操作 disp_init()/disp_init_planes()/disp_alloc_buffer() 等返回的句柄；
 * 「状态与事件」为低频只读查询，不要放进每帧路径。
 */

#ifndef DISPLAY_PRO_H
#define DISPLAY_PRO_H

#if defined(DISPLAY_SIMPLE_H_INCLUDED) && !defined(DISPLAY_SIMPLE_IMPL)
#error "display_pro.h 与 display.h 二选一，不能在同一编译单元混用"
#endif

#define DISPLAY_PRO_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 类型：像素格式 ---------- */

/*
 * 调用方用本枚举描述像素格式；枚举值与具体后端无关，
 * 使用本头文件无需引入 libdrm 等后端头文件。
 */
typedef enum {
    DISP_PIX_FMT_ARGB8888 = 0, // 32bpp，每像素 A/R/G/B 各 8 位
    DISP_PIX_FMT_XRGB8888,     // 32bpp，每像素 X/R/G/B 各 8 位
    DISP_PIX_FMT_NV12,         // 8 位 Y 平面 + UV 交织平面（4:2:0 半平面）
    DISP_PIX_FMT_BUTT          // 枚举边界，非有效格式
} DispPixFmt_t;

/* ---------- 类型：显示缓冲区 ---------- */

typedef struct {
    size_t fbSize;       // 映射缓冲区大小，单位：字节；导入的 DMA-BUF 未映射时为 0
    int fbStride;        // 每行跨度，单位：字节；导入 NV12 时仅表示 Y 平面的跨度
    int dmabuf_fd;       // 模块持有的 DMA-BUF fd，未导出时为 -1；调用方不可关闭
    char *pFramebuffer; // mmap 映射地址；导入的 DMA-BUF 未映射时为 NULL
}DispBufferInfo_t;

typedef struct {
    int dmabuf_fd;           // 调用方导出的 DMA-BUF 文件描述符，接口只复制不接管
    int width;               // framebuffer 宽度，单位：像素
    int height;              // framebuffer 高度，单位：像素
    DispPixFmt_t format;  // SDK 像素格式
    uint32_t pitches[4];     // 各数据平面每行的字节数；RGB 用第 0 项，NV12 用前 2 项
    uint32_t offsets[4];     // 各数据平面在 DMA-BUF 中的字节偏移，未使用项填 0
} DispImportDesc_t;

/* ---------- 类型：设备与连接器枚举结果（disp_init 的一次性快照） ---------- */

#define DRM_MAX_CONNECTORS 12    // 单次 DRM 初始化最多记录的连接器数量
#define DRM_MAX_PLANES 8         // 每个连接器最多记录的可用图层数量
#define DRM_MAX_PLANE_FORMATS 4  // 每个图层最多记录的像素格式数量，须不小于 DispPixFmt_t 的有效格式数
#define DRM_MAX_BUFFERS 16       // 单次 DRM 初始化最多登记的显示缓冲区数量
typedef struct {
    uint32_t id;       // DRM 图层对象 ID，供 disp_init_planes() 选择
    int type;          // 图层类型，为 DRM_PLANE_TYPE_*
    int formatCount;   // 该图层支持的 SDK 像素格式数量
    // 该图层支持的 SDK 像素格式列表，有效项为前 formatCount 项，不支持的格式不会出现；
    // 调用方据此按格式能力挑图层，例如是否支持 NV12、是否支持 ARGB8888 全屏画布
    DispPixFmt_t formats[DRM_MAX_PLANE_FORMATS];
} DispPlane_t;

typedef struct {
    int connectorCount; // DRM 枚举到的连接器数量，包含未启用的连接器
    struct {
        int enabled;       // 已连接、有效模式且分配到 CRTC 时为 1
        int index;         // 本次初始化分配的索引，供其他 DRM 接口使用
        int screenWidth;   // 当前选用模式的水平分辨率，单位：像素
        int screenHeight;  // 当前选用模式的垂直分辨率，单位：像素
        int screenRefresh; // 当前选用模式的刷新率，单位：Hz
        int planeCount;    // 当前连接器可绑定的图层数量
        DispPlane_t planes[DRM_MAX_PLANES]; // 可用前 planeCount 项
    } connectors[DRM_MAX_CONNECTORS]; // 下标与 index 一致
}DispDev_t;

/* ---------- 类型：实时状态（每次查询获取当前值，与上面的初始化快照相对） ---------- */

// 连接器 DPMS 取值；除 UNKNOWN 外的数值与 DRM_MODE_DPMS_* 一致，可直接互转
typedef enum {
    DISP_DPMS_UNKNOWN = -1, // 无法获取 DPMS 状态，不能据此判断输出
    DISP_DPMS_ON      = 0,  // 输出开启，画面可见
    DISP_DPMS_STANDBY = 1,  // 待机，多数显示器已切断显示通路
    DISP_DPMS_SUSPEND = 2,  // 悬挂，显示通路已切断
    DISP_DPMS_OFF     = 4   // 关闭，显示通路已切断，画面不可见
} DispDpms_t;

/*
 * 连接器实时状态快照，由 disp_get_connector_status() 填充。
 * 与 DispDev_t 的区别：DispDev_t 是 disp_init() 的一次性快照且不会自行更新，
 * 本结构每次调用都重新获取当前状态，能反映启动之后发生的拔插、CRTC 被关、息屏等变化。
 */
typedef struct {
    uint32_t connectorId;   // DRM 连接器对象 ID，跨刷新稳定，可直接用于日志
    int connected;          // 1 表示已连接；0 表示已断开或已插不上
    int crtcBound;          // 1 表示该连接器当前绑定了 CRTC；0 表示未绑定，输出必然无效
    uint32_t crtcId;        // 当前驱动该连接器的 CRTC ID，crtcBound 为 0 时无意义
    int crtcEnabled;        // 1 表示该 CRTC 正在扫描输出；0 表示已被关闭
    DispDpms_t dpms;        // 连接器 DPMS 状态，无法获取时为 DISP_DPMS_UNKNOWN
    int width;              // 当前生效模式的水平分辨率，单位：像素；未连接或取不到模式时为 0
    int height;             // 当前生效模式的垂直分辨率，单位：像素；未连接或取不到模式时为 0
    int vrefresh;           // 当前生效模式的刷新率，单位：Hz；未连接或取不到模式时为 0
    int scanoutActive;      // 1 表示画面真正会上屏；0 表示提交即便返回成功，屏幕仍是黑的
} DispConnectorStatus_t;

/*
 * 图层实时绑定快照，由 disp_get_plane_state() 填充。
 * 用于校验 disp_commit() 之后图层是否仍挂在 CRTC 上：提交返回成功不等于图层
 * 没被其它 DRM master 摘下来。
 */
typedef struct {
    uint32_t planeId;  // DRM 图层对象 ID
    uint32_t crtcId;   // 图层当前绑定的 CRTC ID，0 表示已从 CRTC 上摘除
    uint32_t fbId;     // 图层当前使用的帧缓冲 ID，0 表示未挂帧
    int attached;      // crtcId 与 fbId 均非 0 时为 1，否则为 0
} DispPlaneState_t;

/* ---------- 设备生命周期 ---------- */

/**
 * @brief 初始化 DRM 显示设备，枚举连接器并填充其可绑定的图层及格式能力
 * @param pDisplay [out] 显示设备结构，成功时填充，失败时清零
 * @return 0 成功，< 0 失败（-EINVAL 参数无效，-EBUSY 已初始化，-1 打开设备或获取资源失败）
 * @note 每个图层的 id/type 与支持的像素格式列表（formats/formatCount）随枚举一并填充，
 *       调用方可据此按格式能力挑选图层后再交给 disp_init_planes()
 */
int disp_init(DispDev_t *pDisplay);

/**
 * @brief 释放 DRM 资源，兜底释放仍登记的图层与缓冲区，并关闭 DRM 设备
 * @param pDisplay [in/out] 显示设备结构，返回时清零
 */
void disp_uninit(DispDev_t *pDisplay);

/* ---------- 图层：获取、区域与叠加属性 ---------- */

/**
 * @brief 按图层 ID 初始化图层，仅支持已分配 CRTC 的连接器
 * @param conn_index disp_init() 分配的连接器索引
 * @param plane_id   DRM 图层对象 ID，取自 connectors[conn_index].planes[]
 * @return 成功返回图层索引（0 至 DRM_MAX_PLANES-1），< 0 失败
 * @note 同一连接器可多次调用以初始化多个图层；同一图层重复初始化会失败，释放后索引可被重新分配
 */
int disp_init_planes(int conn_index, uint32_t plane_id);

/**
 * @brief 释放图层句柄；显示缓冲区由调用方通过 disp_free_buffer() 释放
 * @param handle disp_init_planes() 返回的图层索引
 */
void disp_uninit_planes(int handle);

/**
 * @brief 设置图层的 framebuffer 源区域，仅保存到图层句柄，实际提交由 disp_commit() 完成
 * @param handle     图层索引
 * @param src_x      源区域水平起点，位于 framebuffer 内
 * @param src_y      源区域垂直起点，位于 framebuffer 内
 * @param src_width  源区域宽度
 * @param src_height 源区域高度
 * @return 0 成功，< 0 失败（尺寸可与 CRTC 目标区域不同以实现缩放，实际边界在提交时与缓冲区尺寸一同校验）
 * @note 重复调用以最后一次配置为准
 */
int disp_plane_set_src_region(int handle, int src_x, int src_y, int src_width, int src_height);

/**
 * @brief 设置图层在连接器 CRTC 内的目标显示区域，仅保存到图层句柄，实际提交由 disp_commit() 完成
 * @param handle      图层索引
 * @param crtc_x      目标区域水平起点
 * @param crtc_y      目标区域垂直起点
 * @param crtc_width  目标区域宽度
 * @param crtc_height 目标区域高度
 * @return 0 成功，< 0 失败（区域超出当前显示模式分辨率时返回 -EINVAL）
 * @note 重复调用以最后一次配置为准
 */
int disp_plane_set_crtc_region(int handle, int crtc_x, int crtc_y, int crtc_width, int crtc_height);

/**
 * @brief 设置图层叠放顺序，调用成功后立即生效
 * @param handle 图层索引
 * @param zpos   叠放顺序值，数值越大通常越靠前，实际范围由驱动决定
 * @return 0 成功，< 0 失败
 */
int disp_plane_set_zpos(int handle, uint32_t zpos);

/**
 * @brief 设置图层整体透明度，调用成功后立即生效
 * @param handle 图层索引
 * @param alpha  透明度，通常 0 表示全透明、65535 表示不透明
 * @return 0 成功，< 0 失败
 */
int disp_plane_set_alpha(int handle, uint16_t alpha);

/**
 * @brief 设置图层像素混合模式，调用成功后立即生效
 * @param handle 图层索引
 * @param mode   混合模式，取 DRM "pixel blend mode" 属性枚举值：
 *               0 (DRM_MODE_BLEND_PREMULTI)  预乘 Alpha 混合；
 *               1 (DRM_MODE_BLEND_COVERAGE)  非预乘 Alpha 混合；
 *               2 (DRM_MODE_BLEND_PIXEL_NONE) 不做 Alpha 混合
 * @return 0 成功，< 0 失败
 * @note 宏定义见 drm_mode.h；实际支持的枚举项由驱动决定，传入不支持的值会返回失败
 */
int disp_plane_set_blend_mode(int handle, uint64_t mode);


/* ---------- 缓冲区：申请、导入与释放 ---------- */

/**
 * @brief 按实际图像尺寸申请显示缓冲区，支持 ARGB8888、XRGB8888 和 NV12
 * @param width  缓冲区宽度，单位：像素（NV12 要求为偶数）
 * @param height 缓冲区高度，单位：像素（NV12 要求为偶数）
 * @param format SDK 像素格式，取 DISP_PIX_FMT_* 之一
 * @return 成功返回缓冲区索引（0 至 DRM_MAX_BUFFERS-1），< 0 失败
 * @note 需在 disp_init() 成功后申请；NV12 的 UV 平面从 pFramebuffer + fbStride * height 开始；
 *       摄像头 DMA-BUF 应改用 disp_import_dmabuf() 导入
 */
int disp_alloc_buffer(int width, int height, DispPixFmt_t format);

/**
 * @brief 导入外部 DMA-BUF 作为显示缓冲区
 * @param desc [in] 导入描述，需填好 dmabuf_fd、尺寸、SDK 像素格式及各平面 pitches/offsets
 * @return 成功返回缓冲区索引（0 至 DRM_MAX_BUFFERS-1），< 0 失败
 * @note NV12 要求宽高为偶数、pitches[1] 非 0 且 offsets[1] 不早于 offsets[0]；
 *       导入的缓冲区不做 mmap（fbSize 为 0、pFramebuffer 为 NULL）；
 *       接口只复制 desc->dmabuf_fd，原始 fd 仍由调用方负责关闭
 */
int disp_import_dmabuf(const DispImportDesc_t *desc);

/**
 * @brief 按索引读取缓冲区信息，不转移 fd 与映射地址的所有权
 * @param buffer_handle 缓冲区索引
 * @param info          [out] 缓冲区信息
 * @return 0 成功，< 0 失败（索引无效或 info 为空）
 * @note fd 与映射地址仅在缓冲区释放前有效，调用方不可自行关闭或解除映射
 */
int disp_get_buffer_info(int buffer_handle, DispBufferInfo_t *info);

/**
 * @brief 释放缓冲区，回收其占用的帧缓冲、fd 与映射
 * @param buffer_handle 缓冲区索引
 * @return 0 成功，< 0 失败
 * @note 释放后索引可被重新分配，调用方不得继续使用旧句柄或此前读取的缓冲区信息
 */
int disp_free_buffer(int buffer_handle);

/* ---------- 提交：把缓冲区按图层配置送显 ---------- */

/**
 * @brief 将缓冲区按图层当前区域配置提交显示（blocking，默认等下一个 vsync）
 * @param handle        图层索引
 * @param buffer_handle 缓冲区索引
 * @return 0 成功，< 0 失败（属性校验失败返回负错误码；提交失败返回 -1，原因见 errno）
 * @note 提交前必须成功调用两个区域接口；源区域需落在缓冲区尺寸内，
 *       NV12 还要求源区域坐标与尺寸均为偶数
 */
int disp_commit(int handle, int buffer_handle);

/**
 * @brief 单图层提交，可指定 flags
 * @param handle        图层索引
 * @param buffer_handle 缓冲区索引
 * @param flags         提交行为标志：
 *                      0                      blocking（等下一个 vsync，默认）
 *                      DRM_MODE_ATOMIC_NONBLOCK 非阻塞，立即返回；若上一次提交
 *                                              还没完成则返回 -EBUSY，调用方需自行重试
 * @return 0 成功，< 0 失败
 */
int disp_commit_ex(int handle, int buffer_handle, uint32_t flags);

/**
 * @brief 多图层合并为一次提交
 * @param plane_handles  图层索引数组（disp_init_planes 返回值）
 * @param buffer_handles 缓冲区索引数组（disp_alloc_buffer / disp_import_dmabuf 返回值）
 * @param count          数组长度
 * @param flags          同 disp_commit_ex
 * @return 0 成功，< 0 失败
 * @note 一次提交保证所有图层在同一 vsync 切换，无撕裂；
 *       比多次独立 disp_commit（每次都等 vsync）大幅缩短帧周期
 */
int disp_commit_multi(const int *plane_handles, const int *buffer_handles,
                     int count, uint32_t flags);

/**
 * @brief 预检一次提交能否被接受，不改变任何显示状态
 * @param handle        图层索引
 * @param buffer_handle 缓冲区索引
 * @return 0 表示参数与状态合法，< 0 失败（错误码与 disp_commit_ex 一致）
 * @note 校验范围与 disp_commit_ex 相同，返回 0 同样不代表画面可见；
 *       判断是否上屏用 disp_get_connector_status() 与 disp_get_plane_state()
 */
int disp_commit_test(int handle, int buffer_handle);

/* ---------- 状态与事件：只读查询，低频调用 ---------- */

/**
 * @brief 读取模块持有的 DRM 设备文件描述符，供调用方自行查询或监听事件
 * @return DRM 设备 fd；disp_init() 未成功时返回 -1
 * @note fd 所有权归模块，调用方不得 close()，也不得在 disp_uninit() 之后继续使用；
 *       可用于 poll() 等待热插拔/vsync 事件后自行调用 drmHandleEvent()、drmWaitVblank()，
 *       或按需自行调用 drmSetMaster()
 */
int disp_get_fd(void);

/**
 * @brief 查询单个连接器的实时输出状态
 * @param conn_index  disp_init() 分配的连接器索引，包含当时未启用的连接器
 * @param status      [out] 实时状态快照
 * @return 0 成功，< 0 失败（-EINVAL 参数或索引无效，-ENODEV 未初始化，其余为负的 errno）
 * @note 每次调用都会获取当前状态，因此能反映启动后的拔插与息屏；查询可能触发硬件探测，
 *       开销较大，只应在诊断或低频轮询里调用，不要每帧调用
 * @note status->scanoutActive 为 0 而 disp_commit() 仍返回 0 是本接口的核心用途：
 *       输出通路被他人关闭时，plane 提交依旧合法
 */
int disp_get_connector_status(int conn_index, DispConnectorStatus_t *status);

/**
 * @brief 用实时状态刷新调用方持有的连接器快照，含分辨率与图层能力表
 * @param pDisplay [in/out] 必须由本次 disp_init() 填好的显示设备结构
 * @return 0 成功，< 0 失败（-EINVAL 参数无效，-ENODEV 未初始化，
 *         -ESTALE 结构来自另一次初始化，-1 刷新图层能力失败）
 * @note 只更新 pDisplay，已持有的 plane/buffer 句柄在本调用后仍然有效；
 *       连接器拓扑变化仍需 disp_uninit() 后重新 disp_init()
 * @note 单个连接器查询失败时保留该连接器原有条目，其余连接器照常刷新
 * @note connectors[].enabled 沿用 disp_init() 的语义，表示"可提交"，不表示"正在上屏"；
 *       是否上屏用 disp_get_connector_status() 读 scanoutActive
 */
int disp_refresh_connectors(DispDev_t *pDisplay);

/**
 * @brief 查询图层的实际绑定关系，确认上一次提交是否仍然生效
 * @param handle 图层索引
 * @param state  [out] 实时图层状态快照
 * @return 0 成功，< 0 失败（-EINVAL 参数或句柄无效，-ENODEV 未初始化，其余为负的 errno）
 * @note state->attached 为 0 说明图层已被摘下 CRTC（输出关闭、被其它 master 重配等），
 *       此时需要重新 disp_plane_set_*_region() 加 disp_commit()，或整体重建显示上下文
 */
int disp_get_plane_state(int handle, DispPlaneState_t *state);

/* ---------- DRM master：输出控制权归属 ---------- */

/**
 * @brief 查询本模块当前的 DRM master 身份
 * @param is_master [out] 1 表示是 master，0 表示不是
 * @return 0 成功，< 0 失败（-EINVAL 参数无效，-ENODEV 未初始化，其余为负的 errno）
 * @note 返回值为当前实际身份，可能与调用方预期不同：无其它客户端占用控制权时，
 *       以 root 打开设备的进程通常已是 master，无需额外申请
 * @note master 只说明有权改输出配置，不说明画面在上屏；输出状态用
 *       disp_get_connector_status() 判断
 */
int disp_is_master(int *is_master);

/**
 * @brief 申请 DRM master，取得显示管线的独占控制权
 * @return 0 成功，< 0 失败（-ENODEV 未初始化，-EACCES 他人已持有且调用方无权接管，
 *         其余为负的 errno）
 * @note 送显不以此为前提，正常显示流程不必调用；仅在需要独占重配输出（如自己接管
 *       模式设置）时使用
 * @note 取得 master 也不保证画面长亮：桌面等其它客户端的息屏策略可能关闭输出，此时
 *       本方提交依旧返回 0 而屏幕全黑。要免受影响，应停掉桌面管理器
 * @note 失败不改变已有身份，调用方可继续用 disp_is_master() 确认实际归属
 */
int disp_set_master(void);

/**
 * @brief 释放 DRM master，把控制权交还给其它客户端（例如桌面）
 * @return 0 成功，< 0 失败（-ENODEV 未初始化，其余为负的 errno）
 * @note 不是 master 时同样返回 0，可安全重复调用；disp_uninit() 前释放可避免桌面恢复受阻
 */
int disp_drop_master(void);


#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_PRO_H */
