#define _GNU_SOURCE

/**
 * GPU 合成实现（Mali G610 / libmali GBM+EGL/GLES）。
 *
 * 与 Rockit `RK_MPI_VO_BindLayer` + 多 Chn `SendFrame` 的对应关系：
 *   - gpu_composer_init() ≈ VO 启用 GPU 合成后端 / 打开显示设备
 *   - gpu_compose_side_by_side   ≈ 同一 layer 上两路窗口左右排布后一次合成
 *   - gpu_composer_deinit        ≈ 销毁 VO 设备 / 释放 GPU 上下文
 *
 * 实现要点：
 *   - 只链接 `libmali`（内置 GBM/EGL/GLES），勿混链 Mesa libgbm/libEGL；
 *   - gpu_composer_init() 自行打开 DRM 设备，用它建 GBM + EGL surfaceless 上下文；
 *   - gpu_compose_side_by_side：输入 NV12 dma-buf（GPU_MEM_FD），
 *     输出 ARGB8888 dma-buf（GLES 不能直接写 NV12）；demo 在 -C gpu
 *     时把视频 plane 挂 ARGB dumb FB。
 *
 * UI 绘制（gpu_ui_*）：同一套 EGL/GBM 上下文里再挂一个 2D 程序，把 UI 画布
 * 的 ARGB8888 dma-buf 导入为 FBO，负责圆角矩形（片元里按 SDF 算覆盖度，
 * 自带 1px 抗锯齿）、纯色矩形/梯形等多边形、字形贴图，以及用 scissor+glClear
 * 挖出的透明洞（over 混合无法把 alpha 擦回 0）。
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
    /* UI 2D 绘制：独立程序/顶点缓冲，逐次 begin/end 绑定目标画布 FBO。 */
    GLuint ui_prog;
    GLuint ui_vbo;
    GLuint ui_white;
    GLuint ui_fbo;
    GLuint ui_tex_dst;
    EGLImageKHR ui_img;
    GLint ui_loc_canvas;
    GLint ui_loc_color;
    GLint ui_loc_mode;
    GLint ui_loc_rect;
    GLint ui_loc_radius;
    GLint ui_loc_border;
    GLint ui_loc_tex;
    /* 当前画布尺寸：erase 接口需要把画布坐标换算成 glScissor 的窗口坐标。 */
    int ui_w;
    int ui_h;
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

/*
 * UI 2D：位置按画布像素给出，顶点着色器换算到 NDC。
 * 画布是 GL FBO 的渲染目标：FBO 纹素 (0,0) 在 NDC (-1,-1)，而该纹素即
 * dma-buf 的第 0 行（DRM 把它显示在屏幕顶部），故画布 y=0 要映射到 NDC -1。
 */
static const char *k_ui_vs =
    "uniform vec2 u_canvas;\n"
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "varying vec2 v_pos;\n"
    "void main(){\n"
    "  v_uv = a_uv;\n"
    "  v_pos = a_pos;\n"
    "  gl_Position = vec4(a_pos.x / u_canvas.x * 2.0 - 1.0,\n"
    "                     a_pos.y / u_canvas.y * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/*
 * u_mode==0：单色几何或纹理贴图（u_color * 纹理；实心几何绑白纹理）。
 * u_mode==1：圆角矩形 SDF，u_rect=(圆心x, 圆心y, 半宽, 半高)，按覆盖度取
 * 1px 抗锯齿；u_border>0 时只保留靠内侧 border 宽的描边环。
 */
static const char *k_ui_fs =
    "precision highp float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_mode;\n"
    "uniform vec4 u_color;\n"
    "uniform vec4 u_rect;\n"
    "uniform float u_radius;\n"
    "uniform float u_border;\n"
    "varying vec2 v_uv;\n"
    "varying vec2 v_pos;\n"
    "void main(){\n"
    "  if (u_mode == 1) {\n"
    "    vec2 q = abs(v_pos - u_rect.xy) - (u_rect.zw - vec2(u_radius));\n"
    "    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - u_radius;\n"
    "    float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "    if (u_border > 0.0)\n"
    "      a *= clamp(0.5 + d + u_border, 0.0, 1.0);\n"
    "    gl_FragColor = vec4(u_color.rgb, u_color.a * a);\n"
    "  } else {\n"
    "    gl_FragColor = u_color * texture2D(u_tex, v_uv);\n"
    "  }\n"
    "}\n";

static int buffer_ok(const GpuBuffer *b)
{
    if (!b || b->width <= 0 || b->height <= 0)
        return 0;
    if (b->format == GPU_FMT_UNKNOWN)
        return 0;
    if (b->stride < 0)
        return 0;
    switch (b->mem_type) {
    case GPU_MEM_FD:
        return b->fd >= 0;
    case GPU_MEM_VIR:
        return b->vir != NULL;
    default:
        return 0;
    }
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
    EGLint attrs[] = {
        EGL_WIDTH, b->width,
        EGL_HEIGHT, b->height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT, b->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, b->stride > 0 ? b->stride : b->width,
        EGL_DMA_BUF_PLANE1_FD_EXT, b->fd,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
            (b->stride > 0 ? b->stride : b->width) * b->height,
        EGL_DMA_BUF_PLANE1_PITCH_EXT, b->stride > 0 ? b->stride : b->width,
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

    /*
     * GBM 需要一块自己打开的 DRM 设备（显示链路由 display 库持有，互不复用），
     * 因此 fd 的开关都收敛在本模块内。
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

    /* UI 2D 程序：常驻一份，供 ui_render 绘制圆角/梯形/文字。 */
    g_gpu.ui_prog = link_program(k_ui_vs, k_ui_fs);
    if (!g_gpu.ui_prog)
        goto fail;
    g_gpu.ui_loc_canvas = glGetUniformLocation(g_gpu.ui_prog, "u_canvas");
    g_gpu.ui_loc_color = glGetUniformLocation(g_gpu.ui_prog, "u_color");
    g_gpu.ui_loc_mode = glGetUniformLocation(g_gpu.ui_prog, "u_mode");
    g_gpu.ui_loc_rect = glGetUniformLocation(g_gpu.ui_prog, "u_rect");
    g_gpu.ui_loc_radius = glGetUniformLocation(g_gpu.ui_prog, "u_radius");
    g_gpu.ui_loc_border = glGetUniformLocation(g_gpu.ui_prog, "u_border");
    g_gpu.ui_loc_tex = glGetUniformLocation(g_gpu.ui_prog, "u_tex");

    glGenBuffers(1, &g_gpu.ui_vbo);

    /* 实心几何统一绑这张 1x1 白纹理，与文字贴图共用同一采样分支。 */
    {
        static const uint8_t white[4] = { 255, 255, 255, 255 };

        g_gpu.ui_white = gpu_ui_upload_texture(1, 1, white);
        if (!g_gpu.ui_white)
            goto fail;
    }

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
    if (g_gpu.ui_white)
        glDeleteTextures(1, &g_gpu.ui_white);
    if (g_gpu.ui_vbo)
        glDeleteBuffers(1, &g_gpu.ui_vbo);
    if (g_gpu.ui_prog)
        glDeleteProgram(g_gpu.ui_prog);
    if (g_gpu.vbo)
        glDeleteBuffers(1, &g_gpu.vbo);
    if (g_gpu.prog)
        glDeleteProgram(g_gpu.prog);
    if (g_gpu.ctx != EGL_NO_CONTEXT)
        eglDestroyContext(g_gpu.dpy, g_gpu.ctx);
    if (g_gpu.dpy != EGL_NO_DISPLAY)
        eglTerminate(g_gpu.dpy);
    if (g_gpu.gbm)
        gbm_device_destroy(g_gpu.gbm);
    if (g_gpu.drm_fd >= 0)
        close(g_gpu.drm_fd);
    memset(&g_gpu, 0, sizeof(g_gpu));
}

int gpu_compose_side_by_side(const GpuBuffer *left, const GpuBuffer *right,
                             GpuBuffer *dst, int swap)
{
    const GpuBuffer *frames[2];
    EGLImageKHR img_l = EGL_NO_IMAGE_KHR;
    EGLImageKHR img_r = EGL_NO_IMAGE_KHR;
    EGLImageKHR img_d = EGL_NO_IMAGE_KHR;
    GLuint tex[2] = {0, 0};
    GLuint tex_dst = 0;
    GLuint fbo = 0;
    int ret = -1;

    if (!g_gpu.ready || !buffer_ok(left) || !buffer_ok(right) || !buffer_ok(dst))
        return -1;
    /* 本实现要求 dma-buf：输入 NV12，输出 ARGB8888。 */
    if (left->mem_type != GPU_MEM_FD || right->mem_type != GPU_MEM_FD ||
        dst->mem_type != GPU_MEM_FD)
        return -1;
    if (left->format != GPU_FMT_NV12 || right->format != GPU_FMT_NV12)
        return -1;
    if (dst->format != GPU_FMT_ARGB8888 && dst->format != GPU_FMT_XRGB8888)
        return -1;
    if (dst->width < left->width * 2 || dst->height < left->height)
        return -1;

    frames[0] = swap ? right : left;
    frames[1] = swap ? left : right;

    if (!eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_gpu.ctx))
        return -1;

    img_l = import_nv12(frames[0]);
    img_r = import_nv12(frames[1]);
    img_d = import_argb(dst);
    if (img_l == EGL_NO_IMAGE_KHR || img_r == EGL_NO_IMAGE_KHR ||
        img_d == EGL_NO_IMAGE_KHR) {
        LOG_E("gpu: eglCreateImageKHR failed 0x%x\n", eglGetError());
        goto out;
    }

    glGenTextures(2, tex);
    glGenTextures(1, &tex_dst);
    glGenFramebuffers(1, &fbo);
    if (!bind_external_tex(tex[0], img_l) || !bind_external_tex(tex[1], img_r)) {
        LOG_E("gpu: bind external tex failed\n");
        goto out;
    }

    glBindTexture(GL_TEXTURE_2D, tex_dst);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_gpu.image_target_tex(GL_TEXTURE_2D, img_d);
    if (glGetError() != GL_NO_ERROR) {
        LOG_E("gpu: bind dst tex failed\n");
        goto out;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex_dst, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_E("gpu: FBO incomplete 0x%x\n",
                glCheckFramebufferStatus(GL_FRAMEBUFFER));
        goto out;
    }

    glViewport(0, 0, dst->width, dst->height);
    glDisable(GL_BLEND);
    glUseProgram(g_gpu.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex[0]);
    glUniform1i(g_gpu.loc_tex0, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex[1]);
    glUniform1i(g_gpu.loc_tex1, 1);

    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    ret = 0;

out:
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (tex_dst) glDeleteTextures(1, &tex_dst);
    if (tex[0] || tex[1]) glDeleteTextures(2, tex);
    if (img_l != EGL_NO_IMAGE_KHR) g_gpu.destroy_image(g_gpu.dpy, img_l);
    if (img_r != EGL_NO_IMAGE_KHR) g_gpu.destroy_image(g_gpu.dpy, img_r);
    if (img_d != EGL_NO_IMAGE_KHR) g_gpu.destroy_image(g_gpu.dpy, img_d);
    return ret;
}

/* ---------------- UI 绘制（几何 + 文字） ---------------- */

static void argb_to_rgba(uint32_t argb, GLfloat *rgba)
{
    rgba[0] = (GLfloat)((argb >> 16) & 0xFF) / 255.0f;
    rgba[1] = (GLfloat)((argb >> 8) & 0xFF) / 255.0f;
    rgba[2] = (GLfloat)(argb & 0xFF) / 255.0f;
    rgba[3] = (GLfloat)((argb >> 24) & 0xFF) / 255.0f;
}

/* 清掉历史错误，避免 glGetError 判断被更早的操作干扰。 */
static void gl_drain_errors(void)
{
    while (glGetError() != GL_NO_ERROR)
        ;
}

unsigned gpu_ui_upload_texture(int width, int height, const uint8_t *rgba)
{
    GLuint tex = 0;

    if (width <= 0 || height <= 0 || !rgba)
        return 0;
    gl_drain_errors();
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* 字形图集是 1bit 点阵，用 NEAREST 保持放大后的硬边像素感。 */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR)
        return 0;
    return tex;
}

int gpu_ui_begin(const GpuBuffer *dst)
{
    GLuint tex = 0;

    if (!g_gpu.ready || !buffer_ok(dst))
        return -1;
    if (dst->mem_type != GPU_MEM_FD)
        return -1;
    if (dst->format != GPU_FMT_ARGB8888 && dst->format != GPU_FMT_XRGB8888)
        return -1;
    if (!eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_gpu.ctx))
        return -1;

    g_gpu.ui_img = import_argb(dst);
    if (g_gpu.ui_img == EGL_NO_IMAGE_KHR) {
        LOG_E("gpu: ui import dst image failed 0x%x\n", eglGetError());
        return -1;
    }

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl_drain_errors();
    g_gpu.image_target_tex(GL_TEXTURE_2D, g_gpu.ui_img);
    if (glGetError() != GL_NO_ERROR) {
        LOG_E("gpu: ui bind dst tex failed\n");
        goto fail;
    }
    g_gpu.ui_tex_dst = tex;
    tex = 0;

    glGenFramebuffers(1, &g_gpu.ui_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu.ui_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g_gpu.ui_tex_dst, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_E("gpu: ui FBO incomplete 0x%x\n",
                glCheckFramebufferStatus(GL_FRAMEBUFFER));
        goto fail;
    }

    glViewport(0, 0, dst->width, dst->height);
    g_gpu.ui_w = dst->width;
    g_gpu.ui_h = dst->height;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    /* 颜色按 over 混合，alpha 通道按 src + dst*(1-src) 累积，保证 DRM 合成的
     * 图层透明度正确（圆角边缘需要半透明）。 */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_gpu.ui_prog);
    glUniform2f(g_gpu.ui_loc_canvas, (GLfloat)dst->width, (GLfloat)dst->height);
    glUniform1i(g_gpu.ui_loc_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_gpu.ui_white);

    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.ui_vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (void *)(2 * sizeof(GLfloat)));
    return 0;

fail:
    if (tex)
        glDeleteTextures(1, &tex);
    gpu_ui_end();
    return -1;
}

void gpu_ui_end(void)
{
    if (!g_gpu.ready)
        return;
    glFinish();
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    if (g_gpu.ui_fbo) {
        glDeleteFramebuffers(1, &g_gpu.ui_fbo);
        g_gpu.ui_fbo = 0;
    }
    if (g_gpu.ui_tex_dst) {
        glDeleteTextures(1, &g_gpu.ui_tex_dst);
        g_gpu.ui_tex_dst = 0;
    }
    if (g_gpu.ui_img != EGL_NO_IMAGE_KHR) {
        g_gpu.destroy_image(g_gpu.dpy, g_gpu.ui_img);
        g_gpu.ui_img = EGL_NO_IMAGE_KHR;
    }
}

void gpu_ui_flush(void)
{
    if (g_gpu.ready)
        glFinish();
}

void gpu_ui_color(uint32_t argb)
{
    GLfloat rgba[4];

    if (!g_gpu.ready)
        return;
    argb_to_rgba(argb, rgba);
    glUniform4fv(g_gpu.ui_loc_color, 1, rgba);
}

void gpu_ui_texture(unsigned tex)
{
    if (!g_gpu.ready)
        return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex ? (GLuint)tex : g_gpu.ui_white);
}

static void ui_submit(const GpuUiVertex *verts, int count)
{
    if (!verts || count <= 0)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizei)(count * sizeof(*verts)), verts,
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, count);
}

void gpu_ui_draw(const GpuUiVertex *verts, int count)
{
    if (!g_gpu.ready || !verts || count <= 0)
        return;
    glUniform1i(g_gpu.ui_loc_mode, 0);
    ui_submit(verts, count);
}

void gpu_ui_rounded_rect(float x, float y, float w, float h,
                         float radius, float border, uint32_t argb)
{
    /* 外扩 1px，让 SDF 在形状边界处有 1px 的抗锯齿过渡带。 */
    const float x0 = x - 1.0f;
    const float y0 = y - 1.0f;
    const float x1 = x + w + 1.0f;
    const float y1 = y + h + 1.0f;
    const GpuUiVertex verts[6] = {
        { x0, y0, 0.5f, 0.5f },
        { x1, y0, 0.5f, 0.5f },
        { x0, y1, 0.5f, 0.5f },
        { x1, y0, 0.5f, 0.5f },
        { x1, y1, 0.5f, 0.5f },
        { x0, y1, 0.5f, 0.5f },
    };

    if (!g_gpu.ready || w <= 0.0f || h <= 0.0f)
        return;
    glUniform1i(g_gpu.ui_loc_mode, 1);
    glUniform4f(g_gpu.ui_loc_rect, x + w * 0.5f, y + h * 0.5f, w * 0.5f,
                h * 0.5f);
    glUniform1f(g_gpu.ui_loc_radius, radius);
    glUniform1f(g_gpu.ui_loc_border, border);
    gpu_ui_color(argb);
    ui_submit(verts, 6);
}

void gpu_ui_erase_rect(float x, float y, float w, float h)
{
    GLint sx, sy, sw, sh;

    if (!g_gpu.ready || w <= 0.0f || h <= 0.0f)
        return;

    /*
     * 画布坐标与 glScissor 的窗口坐标同向：本 FBO 的窗口 y=0 即 NDC -1，对应
     * dma-buf 第 0 行（屏幕顶部），故画布 y 可直接作为窗口 y，无需翻转。
     */
    sx = (GLint)x;
    sy = (GLint)y;
    sw = (GLint)w;
    sh = (GLint)h;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > g_gpu.ui_w) sw = g_gpu.ui_w - sx;
    if (sy + sh > g_gpu.ui_h) sh = g_gpu.ui_h - sy;
    if (sw <= 0 || sh <= 0)
        return;

    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, sy, sw, sh);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}
