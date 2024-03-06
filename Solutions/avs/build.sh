#!/bin/sh

set -e

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
cd $SHELL_FOLDER

CUR_DIR_NAME=`basename "$SHELL_FOLDER"`
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
	rm -rf Release/$CUR_DIR_NAME
	exit 0
fi

# build this project
rm -rf build
mkdir build
sudo cp ./lib64/librockit.so /usr/lib/
sudo cp ./lib64/librkgfx_avs.so /usr/lib/
sudo cp ./lib64/libgraphic_lsf.so /usr/lib/
sudo cp ./lib64/libpanoStitchApp.so /usr/lib/
cd build
cmake ..
make -j24

# make Release files
if [ -d "../config" ]; then
    mv "../config" "../Release"
fi
mkdir -p "../Release/8x_equirectangular/avs_mesh"
mkdir -p "../Release/8x_equirectangular/output_res"

mkdir -p "../Release/6x_rectlinear/avs_mesh"
mkdir -p "../Release/6x_rectlinear/output_res"

mkdir -p "../Release/4x_qr/output_res"

mkdir -p "../Release/6x_hor_ver/output_res"

mkdir -p "../Release" && cp $CUR_DIR_NAME "../Release"
chmod 777 ../Release -R

exit 0
