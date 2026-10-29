INCLUDEPATH += $$PWD

HEADERS += $$PWD/app_log.h
SOURCES += $$EASYEAI_ROOT/media/camera/camera_discover.c \
           $$EASYEAI_ROOT/media/camera/mipi_camera.c \
           $$EASYEAI_ROOT/media/camera/capture.c \
           $$EASYEAI_ROOT/media/gpu/gpu_util.c \
           $$EASYEAI_ROOT/media/rga/rga_util.c
