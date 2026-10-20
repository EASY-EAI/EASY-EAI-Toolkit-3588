#!/bin/sh

set -e

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
cd $SHELL_FOLDER

CUR_DIR_NAME=`basename "$SHELL_FOLDER"`
warring() {
	echo "DESCRIPTION"
	echo "EASYAI-3588 Solution Project."
	echo " "
	echo "./build.sh       : build solution (no UI)"
	echo "./build.sh ui    : build solution with UI (ENABLE_UI)"
	echo "./build.sh clear : clear all compiled files(just preserve source code)"
	echo " "
}

if [ "$1" = "clear" ]; then
	rm -rf build
	rm -rf Release
	exit 0
fi

CMAKE_ARGS=
if [ "$1" = "ui" ]; then
	CMAKE_ARGS="-DENABLE_UI=ON"
elif [ -n "$1" ]; then
	warring
	exit 1
fi

rm -rf build
mkdir build
cd build
cmake $CMAKE_ARGS ..
make -j24

rm -rf "../Release"
mkdir -p "../Release" && cp $CUR_DIR_NAME "../Release"
chmod 777 ../Release -R

exit 0
