#!/bin/sh

set -e

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
CURRENT_FOLDER=`basename $SHELL_FOLDER`
cd $SHELL_FOLDER

# clear
if [ "$1" = "clear" ]; then
	rm -rf build
	rm Release/test-* -f
	exit 0
fi

# build
rm -rf build && mkdir build && cd build
cmake ..
make -j24

# release
chmod 777 test-*
mkdir -p "../Release" && mv test-* ../Release

## copy to Board
mkdir -p $SYSROOT/userdata/Demo/$CURRENT_FOLDER
if [ "$1" = "cpres" ]; then
	cp ../Release/* $SYSROOT/userdata/Demo/$CURRENT_FOLDER
else
	cp ../Release/test-* $SYSROOT/userdata/Demo/$CURRENT_FOLDER
fi
