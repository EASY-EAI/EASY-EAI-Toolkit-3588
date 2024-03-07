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

void appSrc_Init(gpointer data)
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
        pSourceCfg->videoDesc.bInited = TRUE;
    }
}

void appSrc_unInit(gpointer data)
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

void need_data(GstElement *appsrc, guint unused, gpointer user_data)
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


