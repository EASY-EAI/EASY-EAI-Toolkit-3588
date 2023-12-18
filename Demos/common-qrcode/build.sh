#!/bin/sh

set -e

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
cd $SHELL_FOLDER

if [ "$1" = "clear" ]; then
    rm -rf build
    rm -rf Release/test-*
    exit 0
fi

mkdir -p Release
rm -rf build && mkdir build && cd build
cmake ..
make -j24

chmod 777 test-*
cp test-* ../Release
