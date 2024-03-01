##
## 说明：
## 本子模块(api.pri)用于：用户针对easyeai_api的拓展封装，或者自定义补充封装
##

## Qt工程  针对easyapi_api目录与库的引用
## ========================== easyeai_api ========================== ##
COMMONAPI_DIR = ../../easyeai-api/common
ALGORITHM_DIR = ../../easyeai-api/algorithm

INCLUDEPATH += \
    $${COMMONAPI_DIR}/system_opt/ \

HEADERS += \
    $${COMMONAPI_DIR}/system_opt/*.h \

SOURCES += \
    $${COMMONAPI_DIR}/system_opt/*.cpp \

## 有些API需要依赖某些系统库(如-lpthread)，要注意通过LIBS变量加上:
#LIBS += -L/xxxx/xxxx/xxxx/ -lxxxx
LIBS += -lpthread
## ================================================================= ##

