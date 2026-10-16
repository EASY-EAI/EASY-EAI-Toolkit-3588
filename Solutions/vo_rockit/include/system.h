#ifndef __SYSTEM_H__
#define __SYSTEM_H__

#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <pthread.h>
#include <errno.h>

#define SYS_TRUE 1
#define SYS_FALSE 0

typedef bool SYS_BOOL;
typedef void SYS_VOID;

typedef long long SYS_S64;
typedef long SYS_S32;
typedef short SYS_S16;
typedef char SYS_S8;

typedef unsigned long long SYS_U64;
typedef unsigned long SYS_U32;
typedef unsigned short SYS_U16;
typedef unsigned char SYS_U8;

#endif
