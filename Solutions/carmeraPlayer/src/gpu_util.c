#define _GNU_SOURCE

/**
 * GPU 合成实现（Mali G610 / libmali GBM+EGL/GLES）。
 *
 * 编译开关 ENABLE_UI：
 *   - 未定义：仅提供 gpu_compose_side_by_side（无 UI 模式）；
 *   - 定义  ：额外提供 gpu_ui_* 2D 接口，供 ui_main 绘制画布。
 *
 * 实现要点：
 *   - 只链接 `libmali`（内置 GBM/EGL/GLES），勿混链 Mesa libgbm/libEGL；
 *   - gpu_composer_init() 自行打开 DRM 设备建 GBM + EGL surfaceless 上下文；
 *   - gpu_compose_side_by_side：输入 NV12 dma-buf，输出 ARGB8888 dma-buf
 *     （GLES 不能直接写 NV12）；左半屏采样左路整帧、右半屏采样右路整帧，
 *     每路按半边尺寸自动缩放；
 *   - gpu_ui_*：把窗口帧的 ARGB8888 dma-buf 导入为纹理，按窗口矩形贴到全屏
 *     画布的 FBO 上；画布再作为显示 plane 上屏，screen 洞区域保留 alpha=0，
 *     由 plane 混合透出下层视频。
 *
 * 资源复用：
 *   - side-by-side 输出 (dst) 的 FBO+tex+EGLImage 在首次调用时创建，后续复用；
 *   - 输入 NV12 按 dma_fd 缓存 EGLImage+texture（V4L2 BUF_COUNT=4，
 *     两路共 8 个 fd，GPU_NV12_CACHE_MAX=8 足够）；
 *   - UI canvas (gpu_ui_begin 的 dst) 的 FBO+tex+EGLImage 首次创建后复用；
 *   - gpu_ui_begin/end 只做绑定 + 清屏 + glFinish + 解绑。
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

#include "app_log.h"
#include "gpu_util.h"

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

/* NV12 输入缓存上限：两路相机各 BUF_COUNT=4 → 最多 8 个不同 dma_fd。 */
#define GPU_NV12_CACHE_MAX 8

#ifdef ENABLE_UI
/* 常驻 UI 纹理上限：shell/button/stats/cursor 各一，留余量。 */
#define GPU_UI_TEX_MAX 8

typedef struct {
    GLuint tex;
    EGLImageKHR img;
} GpuUiTexEntry;
#endif

/* NV12 dma-buf → {EGLImage, texture} 缓存条目。 */
typedef struct {
    int fd;                 /* -1 = 空闲 */
    GLuint tex;             /* GL_TEXTURE_EXTERNAL_OES */
    EGLImageKHR img;
} Nv12CacheEntry;

typedef struct {
    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint prog;
    GLuint vbo;
    GLuint loc_tex0;
    GLuint loc_tex1;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_tex;

    /* side-by-side 输出持久资源（dst 固定 buffer，首次调用时创建）。 */
    GLuint sbs_fbo;
    GLuint sbs_tex_dst;
    EGLImageKHR sbs_img_dst;
    int sbs_dst_fd;         /* -1 = 未初始化 */
    int sbs_dst_w, sbs_dst_h;

    Nv12CacheEntry nv12_cache[GPU_NV12_CACHE_MAX];

#ifdef ENABLE_UI
    /* canvas 持久资源（gpu_ui_begin 的 dst 固定 buffer，首次创建后复用）。 */
    GLuint ui_prog;
    GLuint ui_vbo;
    GLuint ui_fbo;
    GLuint ui_tex_dst;
    EGLImageKHR ui_img;
    int ui_dst_fd;          /* -1 = 未初始化 */
    int ui_dst_w, ui_dst_h;

    GLint ui_loc_canvas;
    GLint ui_loc_color;
    GLint ui_loc_tex;
    GpuUiTexEntry ui_tex[GPU_UI_TEX_MAX];
#endif
    int ready;
} GpuSession;

static GpuSession g_gpu;

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
    "    vec2 uv = vec2(v_uv.x * 2.0, 1.0 - v_uv.y);\n"
    "    gl_FragColor = texture2D(u_tex0, uv);\n"
    "  } else {\n"
    "    vec2 uv = vec2((v_uv.x - 0.5) * 2.0, 1.0 - v_uv.y);\n"
    "    gl_FragColor = texture2D(u_tex1, uv);\n"
    "  }\n"
    "}\n";

#ifdef ENABLE_UI
/*
 * UI 2D：位置按画布像素给出（原点左上、y 轴向下）。
 * 画布是 GL FBO 的渲染目标：FBO 纹素 (0,0) 在 NDC (-1,-1)，而该纹素即
 * dma-buf 的第 0 行（DRM 把它显示在屏幕顶部），故画布 y=0 要映射到 NDC -1。
 */
static const char *k_ui_vs =
    "uniform vec2 u_canvas;\n"
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main(){\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos.x / u_canvas.x * 2.0 - 1.0,\n"
    "                     a_pos.y / u_canvas.y * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* 窗口帧贴图：u_color 固定白色，输出纹理原值（含 alpha）。 */
static const char *k_ui_fs =
    "precision highp float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n"
    "varying vec2 v_uv;\n"
    "void main(){\n"
    "  gl_FragColor = u_color * texture2D(u_tex, v_uv);\n"
    "}\n";
#endif /* ENABLE_UI */

static int buffer_ok(const GpuBuffer *b)
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
static GLuint link_program(const char *vs_src, const char *fs_src)
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

static EGLImageKHR import_argb(const GpuBuffer *b)
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

/* ---------------- side-by-side 输出持久资源 ---------------- */

static int sbs_init_dst(const GpuBuffer *dst)
{
    g_gpu.sbs_img_dst = import_argb(dst);
    if (g_gpu.sbs_img_dst == EGL_NO_IMAGE_KHR) {
        LOG_E("gpu: sbs dst eglCreateImageKHR failed 0x%x\n", eglGetError());
        return -1;
    }
    glGenTextures(1, &g_gpu.sbs_tex_dst);
    glBindTexture(GL_TEXTURE_2D, g_gpu.sbs_tex_dst);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gpu.image_target_tex(GL_TEXTURE_2D, g_gpu.sbs_img_dst);
    if (glGetError() != GL_NO_ERROR) {
        LOG_E("gpu: sbs dst bind tex failed\n");
        glDeleteTextures(1, &g_gpu.sbs_tex_dst);
        g_gpu.sbs_tex_dst = 0;
        g_gpu.destroy_image(g_gpu.dpy, g_gpu.sbs_img_dst);
        g_gpu.sbs_img_dst = EGL_NO_IMAGE_KHR;
        return -1;
    }
    glGenFramebuffers(1, &g_gpu.sbs_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu.sbs_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_gpu.sbs_tex_dst, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_E("gpu: sbs FBO incomplete 0x%x\n",
                glCheckFramebufferStatus(GL_FRAMEBUFFER));
        glDeleteFramebuffers(1, &g_gpu.sbs_fbo);
        g_gpu.sbs_fbo = 0;
        glDeleteTextures(1, &g_gpu.sbs_tex_dst);
        g_gpu.sbs_tex_dst = 0;
        g_gpu.destroy_image(g_gpu.dpy, g_gpu.sbs_img_dst);
        g_gpu.sbs_img_dst = EGL_NO_IMAGE_KHR;
        return -1;
    }
    g_gpu.sbs_dst_fd = dst->fd;
    g_gpu.sbs_dst_w = dst->width;
    g_gpu.sbs_dst_h = dst->height;
    return 0;
}

static void sbs_deinit_dst(void)
{
    if (g_gpu.sbs_fbo) {
        glDeleteFramebuffers(1, &g_gpu.sbs_fbo);
        g_gpu.sbs_fbo = 0;
    }
    if (g_gpu.sbs_tex_dst) {
        glDeleteTextures(1, &g_gpu.sbs_tex_dst);
        g_gpu.sbs_tex_dst = 0;
    }
    if (g_gpu.sbs_img_dst != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
        g_gpu.destroy_image(g_gpu.dpy, g_gpu.sbs_img_dst);
        g_gpu.sbs_img_dst = EGL_NO_IMAGE_KHR;
    }
    g_gpu.sbs_dst_fd = -1;
    g_gpu.sbs_dst_w = g_gpu.sbs_dst_h = 0;
}

int gpu_composer_init(void)
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

    memset(&g_gpu, 0, sizeof(g_gpu));
    g_gpu.sbs_dst_fd = -1;
#ifdef ENABLE_UI
    g_gpu.ui_dst_fd = -1;
#endif
    for (int i = 0; i < GPU_NV12_CACHE_MAX; ++i)
        g_gpu.nv12_cache[i].fd = -1;

    /*
     * GBM 需要一块自己打开的 DRM 设备（显示链路由 drm_display / display_win
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

    g_gpu.prog = link_program(k_vs, k_fs);
    if (!g_gpu.prog)
        goto fail;
    g_gpu.loc_tex0 = glGetUniformLocation(g_gpu.prog, "u_tex0");
    g_gpu.loc_tex1 = glGetUniformLocation(g_gpu.prog, "u_tex1");

    glGenBuffers(1, &g_gpu.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

#ifdef ENABLE_UI
    g_gpu.ui_prog = link_program(k_ui_vs, k_ui_fs);
    if (!g_gpu.ui_prog)
        goto fail;
    g_gpu.ui_loc_canvas = glGetUniformLocation(g_gpu.ui_prog, "u_canvas");
    g_gpu.ui_loc_color = glGetUniformLocation(g_gpu.ui_prog, "u_color");
    g_gpu.ui_loc_tex = glGetUniformLocation(g_gpu.ui_prog, "u_tex");
    glGenBuffers(1, &g_gpu.ui_vbo);
#endif

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
        sbs_deinit_dst();
        nv12_cache_destroy_all();
#ifdef ENABLE_UI
        for (int i = 0; i < GPU_UI_TEX_MAX; ++i) {
            if (!g_gpu.ui_tex[i].tex)
                continue;
            glDeleteTextures(1, &g_gpu.ui_tex[i].tex);
            if (g_gpu.destroy_image && g_gpu.ui_tex[i].img != EGL_NO_IMAGE_KHR)
                g_gpu.destroy_image(g_gpu.dpy, g_gpu.ui_tex[i].img);
            g_gpu.ui_tex[i].tex = 0;
            g_gpu.ui_tex[i].img = EGL_NO_IMAGE_KHR;
        }
        if (g_gpu.ui_img != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
            g_gpu.destroy_image(g_gpu.dpy, g_gpu.ui_img);
            g_gpu.ui_img = EGL_NO_IMAGE_KHR;
        }
        if (g_gpu.ui_fbo) {
            glDeleteFramebuffers(1, &g_gpu.ui_fbo);
            g_gpu.ui_fbo = 0;
        }
        if (g_gpu.ui_tex_dst) {
            glDeleteTextures(1, &g_gpu.ui_tex_dst);
            g_gpu.ui_tex_dst = 0;
        }
        if (g_gpu.ui_vbo)
            glDeleteBuffers(1, &g_gpu.ui_vbo);
        if (g_gpu.ui_prog)
            glDeleteProgram(g_gpu.ui_prog);
#endif
        if (g_gpu.vbo)
            glDeleteBuffers(1, &g_gpu.vbo);
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
    g_gpu.sbs_dst_fd = -1;
#ifdef ENABLE_UI
    g_gpu.ui_dst_fd = -1;
#endif
    for (int i = 0; i < GPU_NV12_CACHE_MAX; ++i)
        g_gpu.nv12_cache[i].fd = -1;
}

int gpu_compose_side_by_side(const GpuBuffer *left, const GpuBuffer *right,
                             GpuBuffer *dst, int swap)
{
    const GpuBuffer *frames[2];
    Nv12CacheEntry *entry[2] = { NULL, NULL };

    if (!g_gpu.ready || !buffer_ok(left) || !buffer_ok(right) || !buffer_ok(dst))
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

    /* dst buffer 固定不变（drm_alloc_buffer 分配的），首次创建后复用。 */
    if (g_gpu.sbs_dst_fd != dst->fd || g_gpu.sbs_dst_w != dst->width ||
        g_gpu.sbs_dst_h != dst->height) {
        sbs_deinit_dst();
        if (sbs_init_dst(dst) != 0)
            return -1;
    }

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

    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu.sbs_fbo);
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

#ifdef ENABLE_UI
/* ---------------- UI 2D：窗口帧贴图到画布 ---------------- */

/* 清掉历史 GL 错误，避免把上一次调用的残留错误误判为本次失败。 */
static void gl_drain_errors(void)
{
    while (glGetError() != GL_NO_ERROR)
        ;
}

unsigned gpu_ui_import_texture(const GpuBuffer *src)
{
    EGLImageKHR img;
    GLuint tex = 0;
    int slot = -1;
    int i;

    if (!g_gpu.ready || !buffer_ok(src))
        return 0;
    if (src->format != GPU_FMT_ARGB8888 && src->format != GPU_FMT_XRGB8888)
        return 0;

    for (i = 0; i < GPU_UI_TEX_MAX; ++i) {
        if (!g_gpu.ui_tex[i].tex) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        LOG_E("gpu: ui texture table full\n");
        return 0;
    }
    if (!eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_gpu.ctx))
        return 0;

    img = import_argb(src);
    if (img == EGL_NO_IMAGE_KHR) {
        LOG_E("gpu: ui eglCreateImageKHR failed 0x%x\n", eglGetError());
        return 0;
    }

    gl_drain_errors();
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gpu.image_target_tex(GL_TEXTURE_2D, img);
    if (glGetError() != GL_NO_ERROR) {
        LOG_E("gpu: ui bind texture failed\n");
        glDeleteTextures(1, &tex);
        g_gpu.destroy_image(g_gpu.dpy, img);
        return 0;
    }

    g_gpu.ui_tex[slot].tex = tex;
    g_gpu.ui_tex[slot].img = img;
    return tex;
}

void gpu_ui_release_texture(unsigned tex)
{
    int i;

    if (!g_gpu.ready || !tex)
        return;
    for (i = 0; i < GPU_UI_TEX_MAX; ++i) {
        if (g_gpu.ui_tex[i].tex != tex)
            continue;
        glDeleteTextures(1, &g_gpu.ui_tex[i].tex);
        if (g_gpu.ui_tex[i].img != EGL_NO_IMAGE_KHR)
            g_gpu.destroy_image(g_gpu.dpy, g_gpu.ui_tex[i].img);
        g_gpu.ui_tex[i].tex = 0;
        g_gpu.ui_tex[i].img = EGL_NO_IMAGE_KHR;
        return;
    }
}

/* UI canvas 持久资源初始化（首次 gpu_ui_begin 时执行）。 */
static int ui_init_canvas(const GpuBuffer *dst)
{
    g_gpu.ui_img = import_argb(dst);
    if (g_gpu.ui_img == EGL_NO_IMAGE_KHR) {
        LOG_E("gpu: ui canvas eglCreateImageKHR failed 0x%x\n", eglGetError());
        return -1;
    }
    gl_drain_errors();
    glGenTextures(1, &g_gpu.ui_tex_dst);
    glGenFramebuffers(1, &g_gpu.ui_fbo);
    glBindTexture(GL_TEXTURE_2D, g_gpu.ui_tex_dst);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gpu.image_target_tex(GL_TEXTURE_2D, g_gpu.ui_img);
    if (glGetError() != GL_NO_ERROR) {
        LOG_E("gpu: ui canvas bind texture failed\n");
        glDeleteTextures(1, &g_gpu.ui_tex_dst);
        g_gpu.ui_tex_dst = 0;
        glDeleteFramebuffers(1, &g_gpu.ui_fbo);
        g_gpu.ui_fbo = 0;
        g_gpu.destroy_image(g_gpu.dpy, g_gpu.ui_img);
        g_gpu.ui_img = EGL_NO_IMAGE_KHR;
        return -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu.ui_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_gpu.ui_tex_dst, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_E("gpu: ui canvas FBO incomplete 0x%x\n",
                glCheckFramebufferStatus(GL_FRAMEBUFFER));
        glDeleteFramebuffers(1, &g_gpu.ui_fbo);
        g_gpu.ui_fbo = 0;
        glDeleteTextures(1, &g_gpu.ui_tex_dst);
        g_gpu.ui_tex_dst = 0;
        g_gpu.destroy_image(g_gpu.dpy, g_gpu.ui_img);
        g_gpu.ui_img = EGL_NO_IMAGE_KHR;
        return -1;
    }
    g_gpu.ui_dst_fd = dst->fd;
    g_gpu.ui_dst_w = dst->width;
    g_gpu.ui_dst_h = dst->height;
    return 0;
}

int gpu_ui_begin(const GpuBuffer *dst)
{
    if (!g_gpu.ready || !buffer_ok(dst))
        return -1;
    if (dst->format != GPU_FMT_ARGB8888 && dst->format != GPU_FMT_XRGB8888)
        return -1;
    if (!eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_gpu.ctx))
        return -1;

    /* canvas buffer 固定不变，首次创建后复用；fd/尺寸变化则重建。 */
    if (g_gpu.ui_dst_fd != dst->fd || g_gpu.ui_dst_w != dst->width ||
        g_gpu.ui_dst_h != dst->height) {
        if (g_gpu.ui_img != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
            g_gpu.destroy_image(g_gpu.dpy, g_gpu.ui_img);
            g_gpu.ui_img = EGL_NO_IMAGE_KHR;
        }
        if (g_gpu.ui_fbo) {
            glDeleteFramebuffers(1, &g_gpu.ui_fbo);
            g_gpu.ui_fbo = 0;
        }
        if (g_gpu.ui_tex_dst) {
            glDeleteTextures(1, &g_gpu.ui_tex_dst);
            g_gpu.ui_tex_dst = 0;
        }
        if (ui_init_canvas(dst) != 0)
            return -1;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu.ui_fbo);
    glViewport(0, 0, dst->width, dst->height);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    /* RGB 走常规 over；alpha 也按 over 累积，画布 alpha 决定与下层视频的混合。 */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    /* 全透明清屏：未贴窗口处 alpha=0，由 plane 混合透出下层视频。 */
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_gpu.ui_prog);
    glUniform2f(g_gpu.ui_loc_canvas, (float)dst->width, (float)dst->height);
    glUniform1i(g_gpu.ui_loc_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.ui_vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    return 0;
}

void gpu_ui_end(void)
{
    if (g_gpu.dpy == EGL_NO_DISPLAY)
        return;

    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* 注意：不 destroy ui_fbo / ui_tex_dst / ui_img —— 持久复用 */
}

void gpu_ui_flush(void)
{
    if (g_gpu.dpy == EGL_NO_DISPLAY)
        return;
    glFinish();
}

void gpu_ui_color(uint32_t argb)
{
    if (!g_gpu.ready)
        return;
    glUniform4f(g_gpu.ui_loc_color,
                (float)((argb >> 16) & 0xFFu) / 255.f,
                (float)((argb >> 8) & 0xFFu) / 255.f,
                (float)(argb & 0xFFu) / 255.f,
                (float)((argb >> 24) & 0xFFu) / 255.f);
}

void gpu_ui_texture(unsigned tex)
{
    if (!g_gpu.ready)
        return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
}

void gpu_ui_draw(const GpuUiVertex *verts, int count)
{
    if (!g_gpu.ready || !verts || count <= 0)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(count * sizeof(GpuUiVertex)),
                 verts, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GpuUiVertex), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GpuUiVertex),
                          (void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, count);
}

void gpu_ui_blit(unsigned tex, float x, float y, float w, float h)
{
    /*
     * EGLImage 导入的纹理 v 轴与缓冲行序一致（v=0 对应缓冲第 0 行，即图像
     * 顶行），与 k_ui_vs 把画布 y=0 映射到 NDC -1 的约定相同；因此窗口矩形
     * 上边取 v=0、下边取 v=1。若真机发现窗口内容上下颠倒，互换这两个常量。
     */
    const float v_top = 0.f;
    const float v_bot = 1.f;
    GpuUiVertex v[6];

    if (!tex || w <= 0.f || h <= 0.f)
        return;

    v[0].x = x;       v[0].y = y;       v[0].u = 0.f; v[0].v = v_top;
    v[1].x = x + w;   v[1].y = y;       v[1].u = 1.f; v[1].v = v_top;
    v[2].x = x;       v[2].y = y + h;   v[2].u = 0.f; v[2].v = v_bot;
    v[3] = v[2];
    v[4] = v[1];
    v[5].x = x + w;   v[5].y = y + h;   v[5].u = 1.f; v[5].v = v_bot;

    gpu_ui_color(0xFFFFFFFFu);
    gpu_ui_texture(tex);
    gpu_ui_draw(v, 6);
}
#endif /* ENABLE_UI */
