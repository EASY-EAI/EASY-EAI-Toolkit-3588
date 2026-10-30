/**
 * GPU UI 2D 实现（gpu_ui_*）：把窗口帧的 ARGB8888 dma-buf 导入为纹理，
 * 按窗口矩形贴到全屏画布的 FBO 上；画布再作为显示 plane 上屏，screen 洞
 * 区域保留 alpha=0，由 plane 混合透出下层视频。
 *
 * 与 gpu_util.c 分开编译：仅通过 gpu_internal.h 共享 gpu_composer_init
 * 建立的 EGL 会话与导入/编译辅助函数，模块内不再有任何编译开关。
 * 本文件是否加入编译由使用方的 UI 构建选项决定，调用点由应用侧控制。
 *
 * 资源复用：
 *   - canvas (gpu_ui_begin 的 dst) 的 FBO+tex+EGLImage 首次创建后复用；
 *   - UI 程序与顶点缓冲在首次 gpu_ui_begin 时创建；
 *   - gpu_ui_begin/end 只做绑定 + 清屏 + glFinish + 解绑。
 */

#include "gpu_internal.h"
#include "gpu_util.h"

#define APP_LOG_HANDLE g_gpu_log
#include "app_log.h"

/* 常驻 UI 纹理上限：shell/button/stats/cursor 各一，留余量。 */
#define GPU_UI_TEX_MAX 8

typedef struct {
    GLuint tex;
    EGLImageKHR img;
} GpuUiTexEntry;

/*
 * gpu_ui_begin 的 dst 固定是同一块 canvas buffer，首次创建后持久复用，
 * 仅当 fd / 尺寸变化时重建。dst_fd = -1 表示尚未初始化。
 */
static struct {
    GLuint prog;
    GLuint vbo;
    GLuint fbo;
    GLuint tex_dst;
    EGLImageKHR img;
    int dst_fd;
    int dst_w, dst_h;
    GLint loc_canvas;
    GLint loc_color;
    GLint loc_tex;
    GpuUiTexEntry tex[GPU_UI_TEX_MAX];
} g_ui = { .dst_fd = -1 };

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

/* 清掉历史 GL 错误，避免把上一次调用的残留错误误判为本次失败。 */
static void gl_drain_errors(void)
{
    while (glGetError() != GL_NO_ERROR)
        ;
}

/* UI 程序与顶点缓冲首次使用时创建；需在 eglMakeCurrent 之后调用。 */
static int ui_ensure_program(void)
{
    if (g_ui.prog)
        return 0;
    g_ui.prog = gpu_int_link_program(k_ui_vs, k_ui_fs);
    if (!g_ui.prog)
        return -1;
    g_ui.loc_canvas = glGetUniformLocation(g_ui.prog, "u_canvas");
    g_ui.loc_color = glGetUniformLocation(g_ui.prog, "u_color");
    g_ui.loc_tex = glGetUniformLocation(g_ui.prog, "u_tex");
    glGenBuffers(1, &g_ui.vbo);
    return 0;
}

unsigned gpu_ui_import_texture(const GpuBuffer *src)
{
    EGLImageKHR img;
    GLuint tex = 0;
    int slot = -1;
    int i;

    if (!g_gpu.ready || !gpu_int_buffer_ok(src))
        return 0;
    if (src->format != GPU_FMT_ARGB8888 && src->format != GPU_FMT_XRGB8888)
        return 0;

    for (i = 0; i < GPU_UI_TEX_MAX; ++i) {
        if (!g_ui.tex[i].tex) {
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

    img = gpu_int_import_argb(src);
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

    g_ui.tex[slot].tex = tex;
    g_ui.tex[slot].img = img;
    return tex;
}

void gpu_ui_release_texture(unsigned tex)
{
    int i;

    if (!g_gpu.ready || !tex)
        return;
    for (i = 0; i < GPU_UI_TEX_MAX; ++i) {
        if (g_ui.tex[i].tex != tex)
            continue;
        glDeleteTextures(1, &g_ui.tex[i].tex);
        if (g_ui.tex[i].img != EGL_NO_IMAGE_KHR)
            g_gpu.destroy_image(g_gpu.dpy, g_ui.tex[i].img);
        g_ui.tex[i].tex = 0;
        g_ui.tex[i].img = EGL_NO_IMAGE_KHR;
        return;
    }
}

/* UI canvas 持久资源初始化（首次 gpu_ui_begin 或 fd/尺寸变化时执行）。 */
static int ui_init_canvas(const GpuBuffer *dst)
{
    g_ui.img = gpu_int_import_argb(dst);
    if (g_ui.img == EGL_NO_IMAGE_KHR) {
        LOG_E("gpu: ui canvas eglCreateImageKHR failed 0x%x\n", eglGetError());
        return -1;
    }
    gl_drain_errors();
    glGenTextures(1, &g_ui.tex_dst);
    glGenFramebuffers(1, &g_ui.fbo);
    glBindTexture(GL_TEXTURE_2D, g_ui.tex_dst);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gpu.image_target_tex(GL_TEXTURE_2D, g_ui.img);
    if (glGetError() != GL_NO_ERROR) {
        LOG_E("gpu: ui canvas bind texture failed\n");
        glDeleteTextures(1, &g_ui.tex_dst);
        g_ui.tex_dst = 0;
        glDeleteFramebuffers(1, &g_ui.fbo);
        g_ui.fbo = 0;
        g_gpu.destroy_image(g_gpu.dpy, g_ui.img);
        g_ui.img = EGL_NO_IMAGE_KHR;
        return -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_ui.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_ui.tex_dst, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_E("gpu: ui canvas FBO incomplete 0x%x\n",
                glCheckFramebufferStatus(GL_FRAMEBUFFER));
        glDeleteFramebuffers(1, &g_ui.fbo);
        g_ui.fbo = 0;
        glDeleteTextures(1, &g_ui.tex_dst);
        g_ui.tex_dst = 0;
        g_gpu.destroy_image(g_gpu.dpy, g_ui.img);
        g_ui.img = EGL_NO_IMAGE_KHR;
        return -1;
    }
    g_ui.dst_fd = dst->fd;
    g_ui.dst_w = dst->width;
    g_ui.dst_h = dst->height;
    return 0;
}

int gpu_ui_begin(const GpuBuffer *dst)
{
    if (!g_gpu.ready || !gpu_int_buffer_ok(dst))
        return -1;
    if (dst->format != GPU_FMT_ARGB8888 && dst->format != GPU_FMT_XRGB8888)
        return -1;
    if (!eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_gpu.ctx))
        return -1;
    if (ui_ensure_program() != 0)
        return -1;

    if (g_ui.dst_fd != dst->fd || g_ui.dst_w != dst->width ||
        g_ui.dst_h != dst->height) {
        if (g_ui.img != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
            g_gpu.destroy_image(g_gpu.dpy, g_ui.img);
            g_ui.img = EGL_NO_IMAGE_KHR;
        }
        if (g_ui.fbo) {
            glDeleteFramebuffers(1, &g_ui.fbo);
            g_ui.fbo = 0;
        }
        if (g_ui.tex_dst) {
            glDeleteTextures(1, &g_ui.tex_dst);
            g_ui.tex_dst = 0;
        }
        if (ui_init_canvas(dst) != 0)
            return -1;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g_ui.fbo);
    glViewport(0, 0, dst->width, dst->height);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    /* RGB 走常规 over；alpha 也按 over 累积，画布 alpha 决定与下层视频的混合。 */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    /* 全透明清屏：未贴窗口处 alpha=0，由 plane 混合透出下层视频。 */
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_ui.prog);
    glUniform2f(g_ui.loc_canvas, (float)dst->width, (float)dst->height);
    glUniform1i(g_ui.loc_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui.vbo);
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
    /* 注意：不 destroy fbo / tex_dst / img —— 持久复用 */
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
    glUniform4f(g_ui.loc_color,
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
    glBindBuffer(GL_ARRAY_BUFFER, g_ui.vbo);
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

/* 释放本文件创建的全部 GL 资源；须在 gpu_composer_deinit 之前调用。 */
void gpu_ui_deinit(void)
{
    int i;

    if (!g_gpu.ready ||
        !eglMakeCurrent(g_gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_gpu.ctx))
        return;

    for (i = 0; i < GPU_UI_TEX_MAX; ++i) {
        if (!g_ui.tex[i].tex)
            continue;
        glDeleteTextures(1, &g_ui.tex[i].tex);
        if (g_gpu.destroy_image && g_ui.tex[i].img != EGL_NO_IMAGE_KHR)
            g_gpu.destroy_image(g_gpu.dpy, g_ui.tex[i].img);
        g_ui.tex[i].tex = 0;
        g_ui.tex[i].img = EGL_NO_IMAGE_KHR;
    }
    if (g_ui.img != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
        g_gpu.destroy_image(g_gpu.dpy, g_ui.img);
        g_ui.img = EGL_NO_IMAGE_KHR;
    }
    if (g_ui.fbo) {
        glDeleteFramebuffers(1, &g_ui.fbo);
        g_ui.fbo = 0;
    }
    if (g_ui.tex_dst) {
        glDeleteTextures(1, &g_ui.tex_dst);
        g_ui.tex_dst = 0;
    }
    if (g_ui.vbo) {
        glDeleteBuffers(1, &g_ui.vbo);
        g_ui.vbo = 0;
    }
    if (g_ui.prog) {
        glDeleteProgram(g_ui.prog);
        g_ui.prog = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_ui.dst_fd = -1;
    g_ui.dst_w = g_ui.dst_h = 0;
}