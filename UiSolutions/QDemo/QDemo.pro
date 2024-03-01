#-------------------------------------------------
#
# Project created by QtCreator 2021-06-16T19:02:03
#
#-------------------------------------------------

QT       += core gui

CONFIG += qt c++11 console
QMAKE_LIBS_OPENGL=

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

## target name
TARGET = QDemo
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /userdata/UiSolu
!isEmpty(target.path): INSTALLS += target

## target type(app or lib)
TEMPLATE = app

DISTFILES +=

# The following define makes your compiler emit warnings if you use
# any feature of Qt which as been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
#DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

# system
LIBS += -lm -lpthread

# opencv
INCLUDEPATH += $$(SYSROOT)/usr/include/opencv4
#LIBS += $$system(pkg-config --libs-only-l opencv4)

QMAKE_CXXFLAGS += -fpermissive -Wall

## add sub model pri files
include($$PWD/QSrcCode/apiWrapper/api.pri)
include($$PWD/QSrcCode/common/common.pri)
include($$PWD/QSrcCode/ui/ui.pri)

## add resource files
RESOURCES += \
   $$PWD/QResource/resource.qrc \


## assign output files Directory
DESTDIR = $$PWD/Release
OBJECTS_DIR = $$PWD/Release
MOC_DIR = $$PWD/Release
RCC_DIR = $$PWD/Release
UI_DIR = $$PWD/Release
