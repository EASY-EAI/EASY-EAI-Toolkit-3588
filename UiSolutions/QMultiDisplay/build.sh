#!/bin/sh

set -e
cd "$(dirname "$0")"

#   ./build.sh        增量编译并部署
#   ./build.sh all    清空中间产物后全量编译并部署
#   ./build.sh clear  只清空中间产物
case "$1" in
    all|clear)
        rm -f Makefile
        rm -rf Release Debug
        if [ "$1" = "clear" ]; then
            exit 0
        fi
        ;;
esac

# 只为取 SYSROOT 等导出变量；脚本里 alias 不会展开，qmake 的 spec 必须显式指定
if [ -f ~/.bash_aliases ]; then
    . ~/.bash_aliases
fi

if ! command -v qmake >/dev/null 2>&1; then
    echo "qmake not found" && exit 1
fi

if [ -n "$SYSROOT" ] && command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
    qmake -spec linux-aarch64-gnu-g++ QMultiDisplay.pro
else
    qmake QMultiDisplay.pro
fi
make -j"$(nproc)"

# 交叉编译时 SYSROOT 指向挂载的板端根文件系统；板端原生编译未导出即为板内路径
APP_DIR=${SYSROOT}/userdata/UiSolu
mkdir -p "$APP_DIR"
cp Release/QCameraOverlay "$APP_DIR/"

echo "build OK -> Release/QCameraOverlay, deployed -> $APP_DIR/QCameraOverlay"
