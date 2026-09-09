#!/bin/sh 

set -e 
alias_file=~/.bash_aliases
if [ -e "$alias_file" ]; then
        . $alias_file
fi

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
cd $SHELL_FOLDER
CUR_DIR_NAME=`basename "$SHELL_FOLDER"`

APP_DIR=${SYSROOT}/userdata/UiSolu
APP_NAME=${CUR_DIR_NAME}

##  Build:   ./build.sh
##  reBuild: ./build.sh all
##  Clear:   ./build.sh clear
## ========================================
set +e
if [ "$1" = "all" ]; then
	rm -f Makefile
	rm -rf Release
	rm -rf Debug
elif [ "$1" = "clear" ];then
	rm -f Makefile
	rm -rf Release
	rm -rf Debug
	exit 0
fi
set -e

QCORE_FILE=${SYSROOT}/usr/lib/libQt5Core.so.5.15.2
if [ ! -d "Release" ]; then
	# this cmd include create dir operation(mkdir Release/)
	if [ -e "$QCORE_FILE" ]; then
		${SYSROOT}/usr/bin/qmake $APP_NAME.pro
	else
		qmake $APP_NAME.pro
	fi
fi

make
cp QResource/image/background.jpg Release

##  custom shell
## ========================================
mkdir -p ${APP_DIR}
cp Release/background.jpg ${APP_DIR}
cp Release/$APP_NAME ${APP_DIR}


exit 0
