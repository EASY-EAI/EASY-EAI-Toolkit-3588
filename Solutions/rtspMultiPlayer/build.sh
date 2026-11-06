#!/bin/sh

set -e

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
cd $SHELL_FOLDER

CUR_DIR_NAME=`basename "$SHELL_FOLDER"`

# 交叉编译时让 pkg-config 只查目标 sysroot，避免误用宿主机 .pc
if [ -n "$SYSROOT" ]; then
	export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
	export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig
	unset PKG_CONFIG_PATH
fi

warring() {
	echo "DESCRIPTION"
	echo "EASYAI-3588 Solution Project."
	echo " "
	echo "./build.sh       : build solution"
	echo "./build.sh clear : clear all compiled files(just preserve source code)"
	echo " "
}

if [ "$1" = "clear" ]; then
	rm -rf build
	rm -rf Release
	exit 0
fi

# build this project
rm -rf build
mkdir build
cd build
cmake ..
make -j24

# make Release files
mkdir -p "../Release" && cp $CUR_DIR_NAME "../Release"
chmod 777 ../Release -R

exit 0
