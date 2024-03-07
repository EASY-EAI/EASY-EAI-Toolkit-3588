#ifndef __APPSOURCE_H__
#define __APPSOURCE_H__

//=====================   C   =====================
#include <stdbool.h>
#include <stdint.h>
//=====================  PRJ  =====================
#include "srcCommon.h"

extern void appSrc_Init(gpointer data);
extern void appSrc_unInit(gpointer data);
extern void need_data(GstElement *appsrc, guint unused, gpointer user_data);

#endif

