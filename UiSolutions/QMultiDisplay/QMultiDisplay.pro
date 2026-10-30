QT += core gui widgets

greaterThan(QT_MAJOR_VERSION, 4): CONFIG += c++11

TEMPLATE = app
TARGET = QCameraOverlay
DESTDIR = $$PWD/Release
# 中间产物一并落 Release：交叉与板端原生两种编译若把 .o 留在源码树里，
# make 会跨架构复用旧目标文件（链接报 Relocations in generic ELF）
OBJECTS_DIR = $$PWD/Release
MOC_DIR     = $$PWD/Release
RCC_DIR     = $$PWD/Release
UI_DIR      = $$PWD/Release

QMAKE_CXXFLAGS += -fpermissive -Wall
QMAKE_CFLAGS   += -Wall
# 与 cameraPlayer 的 CMake 链接选项对齐：--no-as-needed 保证 libmali
# 进入 DT_NEEDED，EGL/GLES 符号统一由 libmali 解析（板上无 Mesa 竞争）
QMAKE_LFLAGS   += -Wl,--no-as-needed

# ------------------------------------------------------------------ easyeai-api
# 需在子模块之前定义：camera.pri 从 media/gpu、media/rga、media/camera 引用共用模块
EASYEAI_ROOT = $$PWD/../../easyeai-api

# ------------------------------------------------------------------ 子模块
# disp   ：共享显示引擎（全进程唯一 disp_init，视频层 + UI 层，互斥提交）
# camera ：日志入口 app_log.h + 采集/合成共享模块（media/camera 等）
# video  ：采集/合成/视频上屏（cameraPlayer 视频通路的移植）
# input  ：evdev 鼠标/触摸 → Qt 事件转发
# ui     ：Qt 半透明界面（offscreen 渲染到 UI 图层显存）
include($$PWD/QSrcCode/camera/camera.pri)
include($$PWD/QSrcCode/disp/disp.pri)
include($$PWD/QSrcCode/input/input.pri)
include($$PWD/QSrcCode/ui/ui.pri)
include($$PWD/QSrcCode/video/video.pri)

# 底栏底图与欢迎按钮图标（取自原 QMultiDisplay 工程的设计资源）
RESOURCES += $$PWD/QResource/resource.qrc

INCLUDEPATH += $$EASYEAI_ROOT/media/display \
               $$EASYEAI_ROOT/media/gpu \
               $$EASYEAI_ROOT/media/rga \
               $$EASYEAI_ROOT/media/camera \
               $$EASYEAI_ROOT/common/log_manager_pro

# gpu_util.c 用 <drm_fourcc.h> 取四cc 宏，该头在 libdrm 子目录下、不在编译器
# 默认搜索路径里（与 cameraPlayer 的 CMake custom_inc 同一写法）。交叉编译时
# SYSROOT 由 easyeai 环境导出，板端原生编译未导出即 /usr/include/libdrm。
INCLUDEPATH += $$(SYSROOT)/usr/include/libdrm

# libdisplay 内部依赖 libdrm 与 log_manager_pro，链接顺序不可颠倒；
# -lrga/-lmali 供双目合成（RGA / GPU）
LIBS += -L$$EASYEAI_ROOT/media/display/ubuntu22.04 -ldisplay
LIBS += -L$$EASYEAI_ROOT/common/log_manager_pro/ubuntu22.04 -llog_manager_pro
LIBS += -ldrm -lrga -lmali -lpthread
