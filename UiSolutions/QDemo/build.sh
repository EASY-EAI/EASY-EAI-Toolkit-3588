#!/bin/sh 

set -e 
alias_file=~/.bash_aliases
if [ -e "$alias_file" ]; then
        . $alias_file
fi

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
cd $SHELL_FOLDER
CUR_DIR_NAME=`basename "$SHELL_FOLDER"`

APP_NAME=${CUR_DIR_NAME}

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

qmake $APP_NAME.pro
make -j8

cp QResource/image/background.jpg Release

exit 0
