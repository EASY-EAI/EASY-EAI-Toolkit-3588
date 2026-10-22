#define _GNU_SOURCE

/**
 * cameraPlayer 入口。
 *
 * 编译开关 ENABLE_UI（CMake: -DENABLE_UI=ON）：
 *   - 定义   ：运行 ui_main，保留 shell/stats/button/cursor UI 绘制；
 *   - 未定义 ：运行 stitch_main（无 UI，
 *              单摄直显或双摄半宽拼接）。
 */

#ifdef ENABLE_UI
int ui_main(int argc, char **argv);
#else
int stitch_main(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
#ifdef ENABLE_UI
    return ui_main(argc, argv);
#else
    return stitch_main(argc, argv);
#endif
}
