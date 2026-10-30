#ifndef GPU_INTERNAL_H
#define GPU_INTERNAL_H

/*
 * media/gpu 模块内部接口：仅用于 gpu_util.c 与 gpu_ui.c 之间共享 EGL/GL
 * 会话及导入、编译辅助函数，不对外暴露。
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "gpu_util.h"
#include "log_manager_pro.h"

struct gbm_device;

/* NV12 输入缓存上限：两路相机各 BUF_COUNT=4 → 最多 8 个不同 dma_fd。 */
#define GPU_NV12_CACHE_MAX 8

/* 输出 dma-buf → FBO 缓存：双缓冲交替写入 2 个 dst fd，留 2 个余量。 */
#define GPU_DST_CACHE_MAX 4

/* NV12 dma-buf → {EGLImage, texture} 缓存条目。 */
typedef struct {
    int fd;                 /* -1 = 空闲 */
    GLuint tex;             /* GL_TEXTURE_EXTERNAL_OES */
    EGLImageKHR img;
} Nv12CacheEntry;

/* ARGB dma-buf → {EGLImage, texture, FBO} 缓存条目。 */
typedef struct {
    int fd;                 /* -1 = 空闲 */
    int w, h;
    GLuint tex;
    EGLImageKHR img;
    GLuint fbo;
} DstCacheEntry;

typedef struct {
    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint prog;
    GLuint prog_single;
    GLuint vbo;
    GLuint loc_tex0;
    GLuint loc_tex1;
    GLuint loc_single_tex0;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_tex;

    /* side-by-side / single 输出资源，按 dst dma_fd 缓存。 */
    DstCacheEntry dst_cache[GPU_DST_CACHE_MAX];

    Nv12CacheEntry nv12_cache[GPU_NV12_CACHE_MAX];

    int ready;
} GpuSession;

/* gpu_composer_init 成功后的会话，gpu_ui.c 只读使用。 */
extern GpuSession g_gpu;

/* GPU 模块日志句柄：gpu_composer_init 注册，gpu_ui.c 的 LOG_* 共用。 */
extern log_handle_t g_gpu_log;

int gpu_int_buffer_ok(const GpuBuffer *b);

/* 两个程序都用 a_pos=0 / a_uv=1 的属性布局，便于共用同一种顶点格式。 */
GLuint gpu_int_link_program(const char *vs_src, const char *fs_src);

/* 把 ARGB8888/XRGB8888 dma-buf 导入为 EGLImage（需已有 EGL 会话）。 */
EGLImageKHR gpu_int_import_argb(const GpuBuffer *b);

#endif /* GPU_INTERNAL_H */