//=====================  C++  =====================
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
//=====================   C   =====================
#include "system.h"
#include "config.h"
//=====================  SDK  =====================
#include "system_opt.h"
//=====================  PRJ  =====================
#include "appSource.h"
#include "camera/camera.h"

static void appSrc_Init(gpointer data)
{
    SrcCfg_t *pSourceCfg = (SrcCfg_t *)data;
    if(NULL == pSourceCfg){
        return ;
    }
    if(pSourceCfg->videoDesc.bInited){
        return ;
    }
    
    const char *str = pSourceCfg->loaction;
    int camIndex = atoi(str+strlen(str)-2);
    if(0 == mipicamera_init(camIndex, pSourceCfg->videoDesc.width, pSourceCfg->videoDesc.height, 0)){
        mipicamera_set_format(camIndex, RK_FORMAT_YCbCr_420_SP);
        pSourceCfg->videoDesc.timestamp = 0;

        pSourceCfg->videoDesc.bInited = TRUE;
    }
}

static void appSrc_unInit(gpointer data)
{
    SrcCfg_t *pSourceCfg = (SrcCfg_t *)data;
    if(NULL == pSourceCfg){
        return ;
    }
    
    const char *str = pSourceCfg->loaction;
    int camIndex = atoi(str+strlen(str)-2);
    mipicamera_exit(camIndex);
    
    pSourceCfg->videoDesc.bInited = FALSE;
}

static void need_data(GstElement *appsrc, guint unused, gpointer user_data)
{
    SrcCfg_t *pSourceCfg = (SrcCfg_t *)user_data;
    if(NULL == pSourceCfg){
        return ;
    }
    if(pSourceCfg->videoDesc.bInited){
        GstFlowReturn ret;
        
        guint size = 1.5 * pSourceCfg->videoDesc.width * pSourceCfg->videoDesc.height;    
        GstBuffer *buffer = gst_buffer_new_allocate (NULL, size, NULL);
        
        const char *str = pSourceCfg->loaction;
        int camIndex = atoi(str+strlen(str)-2);
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)){
            mipicamera_getframe(camIndex, (char *)map.data);
            gst_buffer_unmap(buffer, &map);	//解除映射
        }
        
        /* increment the timestamp every 1/framerate second */
        GST_BUFFER_PTS(buffer) = pSourceCfg->videoDesc.timestamp;
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(1, GST_SECOND, pSourceCfg->videoDesc.framerate);
        pSourceCfg->videoDesc.timestamp += GST_BUFFER_DURATION(buffer);
        
        g_signal_emit_by_name(appsrc, "push-buffer", buffer, &ret);
        gst_buffer_unref(buffer);
    }
}


/* called when a new media pipeline is constructed. We can query the
 * pipeline and configure our appsrc */
void media_configure(GstRTSPMediaFactory * factory, GstRTSPMedia *media, gpointer user_data)
{
    SrcCfg_t *pSourceCfg = (SrcCfg_t *)user_data;
    if(NULL == pSourceCfg){
        return ;
    }
    
    /* get the element used for providing the streams of the media */
    GstElement *element = gst_rtsp_media_get_element(media);
    /* get our appsrc, we named it 'mysrc' with the name property */
    GstElement *appsrc = gst_bin_get_by_name_recurse_up(GST_BIN(element), "videosrc");

    /* this instructs appsrc that we will be dealing with timed buffer */
    gst_util_set_object_arg(G_OBJECT(appsrc), "format", "time");
    /* configure the caps of the video */
    GstCaps *caps = gst_caps_new_simple ("video/x-raw",
          "format",    G_TYPE_STRING, pSourceCfg->videoDesc.videoSrcType,
          "width",     G_TYPE_INT,    pSourceCfg->videoDesc.width,
          "height",    G_TYPE_INT,    pSourceCfg->videoDesc.height,
          "framerate", GST_TYPE_FRACTION, pSourceCfg->videoDesc.framerate, 1, NULL);
    if(caps){
        g_object_set(G_OBJECT(appsrc), "caps", caps, NULL);
        gst_caps_unref(caps);
    }
    
    appSrc_Init(pSourceCfg);

    /* make sure ther datais freed when the media is gone */
    g_object_set_data_full(G_OBJECT(media), "my-extra-data", user_data, appSrc_unInit);
    
    /* install the callback that will be called when a buffer is needed */
    g_signal_connect(appsrc, "need-data", (GCallback)need_data, user_data);
    
    gst_object_unref(appsrc);
    gst_object_unref(element);
}


