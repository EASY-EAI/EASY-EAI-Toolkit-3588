#ifndef __APPSOURCE_H__
#define __APPSOURCE_H__

//=====================   C   =====================
#include <stdbool.h>
#include <stdint.h>
//=====================  PRJ  =====================
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

typedef struct {
    const char *videoSrcType;
    gboolean bInited;
    gint width;
    gint height;
    gint framerate;
    GstClockTime timestamp;
    const char *videoDstType;
}VSrcDesc_t;

typedef struct {
    const char *loaction;
    VSrcDesc_t  videoDesc;
    const char *audioDstType;
}SrcCfg_t;

extern void media_configure(GstRTSPMediaFactory * factory, GstRTSPMedia *media, gpointer user_data);

#endif

