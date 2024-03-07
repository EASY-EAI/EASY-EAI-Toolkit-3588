#ifndef __CAPTURER_H__
#define __CAPTURER_H__

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
    const char *srcType;
    const char *loaction;
    VSrcDesc_t  videoDesc;
    const char *audioDstType;
}SrcCfg_t;

#endif

