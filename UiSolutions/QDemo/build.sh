#!/bin/sh 

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
cd $SHELL_FOLDER
CUR_DIR_NAME=`basename "$SHELL_FOLDER"`

##  Build:   ./build.sh
##  reBuild: ./build.sh all
##  Clear:   ./build.sh clear
## ========================================
set +e
if [ "$1" = "all" ]; then
	rm -f Makefile
	rm -rf Release
elif [ "$1" = "clear" ];then
	rm -f Makefile
	rm -rf Release
	exit 0
fi
set -e

APP_NAME=${CUR_DIR_NAME}
qmake $APP_NAME.pro
make -j8

cp QResource/image/background.jpg Release

exit 0
