#define _GNU_SOURCE

/**
 * GPU 合成实现（Mali G610 / libmali GBM+EGL/GLES）。
 *
 * 本文件只负责双摄拼接 / 单摄整幅合成；UI 2D 接口在 gpu_ui.c，
 * 二者通过 gpu_internal.h 共享 gpu_composer_init 建立的 EGL 会话。
 * 使用方按需选择是否把 gpu_ui.c 加入编译，无任何编译开关宏。
 *
 * 实现要点：
 *   - 只链接 `libmali`（内置 GBM/EGL/GLES），勿混链 Mesa libgbm/libEGL；
 *   - gpu_composer_init() 自行打开 DRM 设备建 GBM + EGL surfaceless 上下文；
 *   - gpu_compose_side_by_side：输入 NV12 dma-buf，输出 ARGB8888 dma-buf
 *     （GLES 不能直接写 NV12）；左半屏采样左路整帧、右半屏采样右路整帧，
 *     每路按半边尺寸自动缩放；
 *   - gpu_compose_single：单路整帧铺满输出，用于 -m 单摄模式。
 *
 * 资源复用：
 *   - 拼接 / 单摄输出 (dst) 的 FBO+tex+EGLImage 按 dma_fd 缓存，双缓冲时
 *     两块输出缓冲各有一份，避免每帧重建；
 *   - 输入 NV12 按 dma_fd 缓存 EGLImage+texture（V4L2 BUF_COUNT=4，
 *     两路共 8 个 fd，GPU_NV12_CACHE_MAX=8 足够）。
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "gpu_internal.h"
#include "gpu_util.h"

/* GPU 模块日志句柄：gpu_composer_init 注册，gpu_ui.c 经 gpu_internal.h 共用。 */
log_handle_t g_gpu_log = LOG_HANDLE_INVALID;

#define APP_LOG_HANDLE g_gpu_log
#include "app_log.h"

#ifndef EGL_NO_CONFIG_KHR
#define EGL_NO_CONFIG_KHR ((EGLConfig)0)
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT 0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT 0x3277
#endif

/* libmali 自带 GBM；不要链 Mesa libgbm，否则会混库崩溃。 */
struct gbm_device;
struct gbm_device *gbm_create_device(int fd);
void gbm_device_destroy(struct gbm_device *gbm);

GpuSession g_gpu;

static const char *k_vs =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main(){\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

/* 左右各采一路 external OES（NV12 dma-buf）；输出 ARGB/RGBA FBO。 */
static const char *k_fs =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES u_tex0;\n"
    "uniform samplerExternalOES u_tex1;\n"
    "varying vec2 v_uv;\n"
    "void main(){\n"
    "  if (v_uv.x < 0.5) {\n"
    "    vec2 uv = vec2(v_uv.x * 2.0, v_uv.y);\n"
    "    gl_FragColor = texture2D(u_tex0, uv);\n"
    "  } else {\n"
    "    vec2 uv = vec2((v_uv.x - 0.5) * 2.0, v_uv.y);\n"
    "    gl_FragColor = texture2D(u_tex1, uv);\n"
    "  }\n"
    "}\n";

/* 单路整帧铺满输出（-m 单摄模式）。 */
static const char *k_fs_single =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES u_tex0;\n"
    "varying vec2 v_uv;\n"
    "void main(){\n"
    "  gl_FragColor = texture2D(u_tex0, v_uv);\n"
    "}\n";

int gpu_int_buffer_ok(const GpuBuffer *b)
{
    if (!b || b->width <= 0 || b->height <= 0)
        return 0;
    if (b->format == GPU_FMT_UNKNOWN)
        return 0;
    return b->fd >= 0 && b->stride >= 0;
}

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOG_E("gpu shader compile: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* 两个程序都用 a_pos=0 / a_uv=1 的属性布局，便于共用同一种顶点格式。 */
GLuint gpu_int_link_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog;
    GLint ok = 0;

    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        LOG_E("gpu program link: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static EGLImageKHR import_nv12(const GpuBuffer *b)
{
    EGLint pitch = b->stride > 0 ? b->stride : b->width;

    EGLint attrs[] = {
        EGL_WIDTH, b->width,
        EGL_HEIGHT, b->height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT, b->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch,
        EGL_DMA_BUF_PLANE1_FD_EXT, b->fd,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, pitch * b->height,
        EGL_DMA_BUF_PLANE1_PITCH_EXT, pitch,
        EGL_NONE
    };
    return g_gpu.create_image(g_gpu.dpy, EGL_NO_CONTEXT,
                              EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
}

EGLImageKHR gpu_int_import_argb(const GpuBuffer *b)
{
    EGLint attrs[] = {
        EGL_WIDTH, b->width,
        EGL_HEIGHT, b->height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, b->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, b->stride > 0 ? b->stride : b->width * 4,
        EGL_NONE
    };
    return g_gpu.create_image(g_gpu.dpy, EGL_NO_CONTEXT,
                              EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
}

static int bind_external_tex(GLuint tex, EGLImageKHR img)
{
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gpu.image_target_tex(GL_TEXTURE_EXTERNAL_OES, img);
    return glGetError() == GL_NO_ERROR;
}

/* ---------------- NV12 dma_fd → EGLImage+tex 缓存 ---------------- */

static Nv12CacheEntry *nv12_cache_get(const GpuBuffer *b)
{
    for (int i = 0; i < GPU_NV12_CACHE_MAX; ++i) {
        Nv12CacheEntry *e = &g_gpu.nv12_cache[i];
        if (e->fd == b->fd && e->tex != 0)
            return e;
    }
    for (int i = 0; i < GPU_NV12_CACHE_MAX; ++i) {
        Nv12CacheEntry *e = &g_gpu.nv12_cache[i];
        if (e->fd < 0 || e->tex == 0) {
            if (e->img != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
                g_gpu.destroy_image(g_gpu.dpy, e->img);
                e->img = EGL_NO_IMAGE_KHR;
            }
            if (e->tex) {
                glDeleteTextures(1, &e->tex);
                e->tex = 0;
            }

            EGLImageKHR img = import_nv12(b);
            if (img == EGL_NO_IMAGE_KHR) {
                LOG_E("gpu: nv12 cache eglCreateImageKHR failed 0x%x\n", eglGetError());
                return NULL;
            }
            GLuint tex;
            glGenTextures(1, &tex);
            if (!bind_external_tex(tex, img)) {
                LOG_E("gpu: nv12 cache bind external tex failed\n");
                glDeleteTextures(1, &tex);
                g_gpu.destroy_image(g_gpu.dpy, img);
                return NULL;
            }
            e->fd = b->fd;
            e->tex = tex;
            e->img = img;
            return e;
        }
    }
    LOG_E("gpu: nv12 cache full (all %d entries in use)\n", GPU_NV12_CACHE_MAX);
    return NULL;
}

static void nv12_cache_destroy_all(void)
{
    for (int i = 0; i < GPU_NV12_CACHE_MAX; ++i) {
        Nv12CacheEntry *e = &g_gpu.nv12_cache[i];
        if (e->tex) {
            glDeleteTextures(1, &e->tex);
            e->tex = 0;
        }
        if (e->img != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
            g_gpu.destroy_image(g_gpu.dpy, e->img);
            e->img = EGL_NO_IMAGE_KHR;
        }
        e->fd = -1;
    }
}

/* ---------------- 输出 dma-buf 持久资源 ---------------- */

static void dst_cache_destroy_entry(DstCacheEntry *e)
{
    if (e->fbo) {
        glDeleteFramebuffers(1, &e->fbo);
        e->fbo = 0;
    }
    if (e->tex) {
        glDeleteTextures(1, &e->tex);
        e->tex = 0;
    }
    if (e->img != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
        g_gpu.destroy_image(g_gpu.dpy, e->img);
        e->img = EGL_NO_IMAGE_KHR;
    }
    e->fd = -1;
    e->w = e->h = 0;
}

static int dst_cache_init_entry(DstCacheEntry *e, const GpuBuffer *dst)
{
    e->img = gpu_int_import_argb(dst);
    if (e->img == EGL_NO_IMAGE_KHR) {
        LOG_E("gpu: dst eglCreateImageKHR failed 0x%x\n", eglGetError());
        return -1;
    }
    glGenTextures(1, &e->tex);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gpu.image_target_tex(GL_TEXTURE_2D, e->img);
    if (glGetError() != GL_NO_ERROR) {
        LOG_E("gpu: dst bind tex failed\n");
        return -1;
    }
    glGenFramebuffers(1, &e->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, e->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_E("gpu: dst FBO incomplete 0x%x\n",
                glCheckFramebufferStatus(GL_FRAMEBUFFER));
        return -1;
    }
    e->fd = dst->fd;
    e->w = dst->width;
    e->h = dst->height;
    return 0;
}

/* 取 dst 对应的 FBO 条目；双缓冲时两个 fd 各占一条，首次使用时创建。 */
static DstCacheEntry *dst_cache_get(const GpuBuffer *dst)
{
    DstCacheEntry *slot = NULL;

    for (int i = 0; i < GPU_DST_CACHE_MAX; ++i) {
        DstCacheEntry *e = &g_gpu.dst_cache[i];

        if (e->fbo && e->fd == dst->fd && e->w == dst->width &&
            e->h == dst->height)
            return e;
    }
    for (int i = 0; i < GPU_DST_CACHE_MAX; ++i) {
        if (!g_gpu.dst_cache[i].fbo) {
            slot = &g_gpu.dst_cache[i];
            break;
        }
    }
    if (!slot) {
        LOG_E("gpu: dst cache full (all %d entries in use)\n",
                GPU_DST_CACHE_MAX);
        return NULL;
    }

    dst_cache_destroy_entry(slot);
    if (dst_cache_init_entry(slot, dst) != 0) {
        dst_cache_destroy_entry(slot);
        return NULL;
    }
    return slot;
}

static void dst_cache_destroy_all(void)
{
    for (int i = 0; i < GPU_DST_CACHE_MAX; ++i)
        dst_cache_destroy_entry(&g_gpu.dst_cache[i]);
}

int gpu_composer_init(log_mgr_t log_mgr)
{
    EGLint maj = 0, min = 0;
    const EGLint ctx_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    static const float quad[] = {
        /* pos x,y   uv x,y */
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
        -1.f,  1.f,  0.f, 1.f,
         1.f,  1.f,  1.f, 1.f,
    };

    g_gpu_log = app_log_register_module(log_mgr, "gpu");

    memset(&g_gpu, 0, sizeof(g_gpu));
    for (int i = 0; i < GPU_DST_CACHE_MAX; ++i)
        g_gpu.dst_cache[i].fd = -1;
    for (int i = 0; i < GPU_NV12_CACHE_MAX; ++i)
        g_gpu.nv12_cache[i].fd = -1;

    /*
     * GBM 需要一块自己打开的 DRM 设备（显示链路由 display_pro / display_win
     * 持有，互不复用），因此 fd 的开关都收敛在本模块内。
     */
    g_gpu.drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (g_gpu.drm_fd < 0) {
        LOG_E("gpu: open /dev/dri/card0 failed: %s\n", strerror(errno));
        return -1;
    }

    g_gpu.gbm = gbm_create_device(g_gpu.drm_fd);
    if (!g_gpu.gbm) {
        LOG_E("gpu: gbm_create_device failed\n");
        goto fail;
    }

    g_gpu.dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, g_gpu.gbm, NULL);
    if (g_gpu.dpy == EGL_NO_DISPLAY) {
        LOG_E("gpu: eglGetPlatformDisplay failed 0x%x\n", eglGetError());
        goto fail;
    }
    if (!eglInitialize(g_gpu.dpy, &maj, &min)) {
        LOG_E("gpu: eglInitialize failed 0x%x\n", eglGetError());
        goto fail;
    }
    LOG_I("gpu: EGL %d.%d vendor=%s\n", maj, min,
           eglQueryString(g_gpu.dpy, EGL_VENDOR));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        LOG_E("gpu: eglBindAPI failed\n");
        goto fail;
    }

    /* 使用 no-config + surfaceless（板端 Mali 已验证支持）。 */
    g_gpu.ctx = eglCreateContext(g_gpu.dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (g_gpu.ctx == EGL_NO_CONTEXT) {
        /* 回退：选一个 config 再建 context */
        EGLConfig cfg;
        EGLint n = 0;
        const EGLint cfg_attr[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE
        };
        if (!eglChooseConfig(g_gpu.dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
            LOG_E("gpu: eglChooseConfig failed 0x%x\n", eglGetError());
            goto fail;
        }
        g_gpu.ctx = eglCreateContext(g_gpu.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    }
    if (g_gpu.ctx == EGL_NO_CONTEXT) {
        LOG_E("gpu: eglCreateContext failed 0x%x\n", eglGetError());
        goto fail;
    }
    if (!eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_gpu.ctx)) {
        LOG_E("gpu: eglMakeCurrent(surfaceless) failed 0x%x\n", eglGetError());
        goto fail;
    }
    LOG_I("gpu: GL_VENDOR=%s\n", glGetString(GL_VENDOR));
    LOG_I("gpu: GL_RENDERER=%s\n", glGetString(GL_RENDERER));

    g_gpu.create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    g_gpu.destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    g_gpu.image_target_tex = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!g_gpu.create_image || !g_gpu.destroy_image || !g_gpu.image_target_tex) {
        LOG_E("gpu: missing EGL/GLES extension procs\n");
        goto fail;
    }

    g_gpu.prog = gpu_int_link_program(k_vs, k_fs);
    if (!g_gpu.prog)
        goto fail;
    g_gpu.loc_tex0 = glGetUniformLocation(g_gpu.prog, "u_tex0");
    g_gpu.loc_tex1 = glGetUniformLocation(g_gpu.prog, "u_tex1");

    g_gpu.prog_single = gpu_int_link_program(k_vs, k_fs_single);
    if (!g_gpu.prog_single)
        goto fail;
    g_gpu.loc_single_tex0 = glGetUniformLocation(g_gpu.prog_single, "u_tex0");

    glGenBuffers(1, &g_gpu.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    /*
     * 初始化在主线程完成，合成却在视频线程：上下文若留在主线程 current，
     * 视频线程 eglMakeCurrent 会返回 EGL_BAD_ACCESS(0x3002)，必须在此解绑。
     */
    eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    g_gpu.ready = 1;
    return 0;

fail:
    gpu_composer_deinit();
    return -1;
}

void gpu_composer_deinit(void)
{
    if (g_gpu.dpy != EGL_NO_DISPLAY)
        eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_gpu.ctx != EGL_NO_CONTEXT) {
        dst_cache_destroy_all();
        nv12_cache_destroy_all();
        if (g_gpu.vbo)
            glDeleteBuffers(1, &g_gpu.vbo);
        if (g_gpu.prog_single)
            glDeleteProgram(g_gpu.prog_single);
        if (g_gpu.prog)
            glDeleteProgram(g_gpu.prog);
    }
    if (g_gpu.ctx != EGL_NO_CONTEXT)
        eglDestroyContext(g_gpu.dpy, g_gpu.ctx);
    if (g_gpu.dpy != EGL_NO_DISPLAY)
        eglTerminate(g_gpu.dpy);
    if (g_gpu.gbm)
        gbm_device_destroy(g_gpu.gbm);
    if (g_gpu.drm_fd >= 0)
        close(g_gpu.drm_fd);
    memset(&g_gpu, 0, sizeof(g_gpu));
    /* 置 -1 以区分「未打开」，避免重复 deinit 时误关 fd 0。 */
    g_gpu.drm_fd = -1;
    for (int i = 0; i < GPU_DST_CACHE_MAX; ++i)
        g_gpu.dst_cache[i].fd = -1;
    for (int i = 0; i < GPU_NV12_CACHE_MAX; ++i)
        g_gpu.nv12_cache[i].fd = -1;
}

int gpu_compose_side_by_side(const GpuBuffer *left, const GpuBuffer *right,
                             GpuBuffer *dst, int swap)
{
    const GpuBuffer *frames[2];
    Nv12CacheEntry *entry[2] = { NULL, NULL };
    DstCacheEntry *dst_entry;

    if (!g_gpu.ready || !gpu_int_buffer_ok(left) || !gpu_int_buffer_ok(right) ||
        !gpu_int_buffer_ok(dst))
        return -1;
    if (left->format != GPU_FMT_NV12 || right->format != GPU_FMT_NV12)
        return -1;
    if (dst->format != GPU_FMT_ARGB8888 && dst->format != GPU_FMT_XRGB8888)
        return -1;
    if (dst->width < 2 || dst->height <= 0)
        return -1;

    frames[0] = swap ? right : left;
    frames[1] = swap ? left : right;

    if (!eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_gpu.ctx))
        return -1;

    dst_entry = dst_cache_get(dst);
    if (!dst_entry)
        return -1;

    /*
     * V4L2 只有 BUF_COUNT=4 个 buffer，dma_fd 轮转，
     * 首次遇到创建 EGLImage+tex。
     */
    for (int i = 0; i < 2; ++i) {
        entry[i] = nv12_cache_get(frames[i]);
        if (!entry[i]) {
            LOG_E("gpu: nv12 cache lookup failed (fd=%d)\n", frames[i]->fd);
            return -1;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, dst_entry->fbo);
    glViewport(0, 0, dst->width, dst->height);
    glDisable(GL_BLEND);
    glUseProgram(g_gpu.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, entry[0]->tex);
    glUniform1i(g_gpu.loc_tex0, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, entry[1]->tex);
    glUniform1i(g_gpu.loc_tex1, 1);

    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}
int gpu_compose_single(const GpuBuffer *src, GpuBuffer *dst)
{
    Nv12CacheEntry *entry;
    DstCacheEntry *dst_entry;

    if (!g_gpu.ready || !gpu_int_buffer_ok(src) || !gpu_int_buffer_ok(dst))
        return -1;
    if (src->format != GPU_FMT_NV12)
        return -1;
    if (dst->format != GPU_FMT_ARGB8888 && dst->format != GPU_FMT_XRGB8888)
        return -1;
    if (dst->width <= 0 || dst->height <= 0)
        return -1;

    if (!eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_gpu.ctx))
        return -1;

    dst_entry = dst_cache_get(dst);
    if (!dst_entry)
        return -1;

    entry = nv12_cache_get(src);
    if (!entry) {
        LOG_E("gpu: nv12 cache lookup failed (fd=%d)\n", src->fd);
        return -1;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, dst_entry->fbo);
    glViewport(0, 0, dst->width, dst->height);
    glDisable(GL_BLEND);
    glUseProgram(g_gpu.prog_single);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, entry->tex);
    glUniform1i(g_gpu.loc_single_tex0, 0);

    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}
