/**
*
* Copyright 2024 by Guangzhou Easy EAI Technologny Co.,Ltd.
* website: www.easy-eai.com
*
* Author: Jiehao.Zhong <zhongjiehao@easy-eai.com>
* 
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* License file for more details.
* 
*/

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

#define DEFAULT_RTSP_PORT   "8554"
#define ATTACH_URL          "/test"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
static SrcCfg_t SrcCfg_tab[] = {
	{
		.loaction  = "/dev/video44",
		.videoDesc = {"NV12", FALSE, 1920, 1080, 30, 0, "h264"},
		.audioDstType = "null",
	}, {
		.loaction  = "/dev/video53",
		.videoDesc = {"NV12", FALSE, 1920, 1080, 30, 0, "h264"},
		.audioDstType = "null",
	}, {
		.loaction  = "/dev/video71",
		.videoDesc = {"NV12", FALSE, 1920, 1080, 30, 0, "h264"},
		.audioDstType = "null",
	}, {
		.loaction  = "/dev/video62",
		.videoDesc = {"NV12", FALSE, 1920, 1080, 60, 0, "h265"},
		.audioDstType = "null",
    }
};

int main(int argc, char **argv)
{
    
    GOptionEntry entries[] = {
        {"port", 'p', 0, G_OPTION_ARG_STRING, (char*)DEFAULT_RTSP_PORT, "Port to listen on (default: " DEFAULT_RTSP_PORT ")", "PORT"},
        {NULL}
    };

    GError *error = NULL;
    GOptionContext *optctx = g_option_context_new("<filename.mp4> - Test RTSP Server, MP4");
    g_option_context_add_main_entries(optctx, entries, NULL);
    g_option_context_add_group(optctx, gst_init_get_option_group());

    if(!g_option_context_parse(optctx, NULL, NULL, &error)){
        g_printerr("Error parsing options : %s\n", error->message);
        g_option_context_free(optctx);
        g_clear_error(&error);
        return -1;
    }
    g_option_context_free(optctx);



    /***************************************   RTSP Server   ***************************************/
    gst_init(&argc, &argv);
    
    gchar *str = NULL;
    GMainLoop *loop = NULL;
    GstRTSPServer *server = NULL;
    GstRTSPMountPoints *mounts = NULL;
    GstRTSPMediaFactory *factory = NULL;
    
    loop = g_main_loop_new(NULL, FALSE);
    server = gst_rtsp_server_new();
    g_object_set(server, "service", DEFAULT_RTSP_PORT, NULL);
    mounts = gst_rtsp_server_get_mount_points(server);
    
    //GstRTSPMediaMapping *mapping;
    //mapping = gst_rtsp_server_get_media_mapping(server);

    int count = ARRAY_SIZE(SrcCfg_tab);
    gchar strVideoEncoder[32], strVideoPayload[32];
    for(int index = 0; index < count; index++){
        factory = gst_rtsp_media_factory_new();
        if(NULL == factory){
            continue;
        }
        
        memset(strVideoEncoder, 0, sizeof(strVideoEncoder));
        snprintf(strVideoEncoder, sizeof(strVideoEncoder), "mpp%senc", SrcCfg_tab[index].videoDesc.videoDstType);
        memset(strVideoPayload, 0, sizeof(strVideoPayload));
        snprintf(strVideoPayload, sizeof(strVideoPayload), "rtp%spay", SrcCfg_tab[index].videoDesc.videoDstType);
        str = g_strdup_printf("appsrc name=videosrc ! %s ! %s name=pay0 pt=96", strVideoEncoder, strVideoPayload);
        gst_rtsp_media_factory_set_launch(factory, str);
        g_free(str);
        
        gst_rtsp_media_factory_set_shared(factory, TRUE);
        g_signal_connect(factory, "media-configure", (GCallback)media_configure, &SrcCfg_tab[index]);
        
        str = g_strdup_printf("%s%d", ATTACH_URL, index);
        gst_rtsp_mount_points_add_factory(mounts, str, factory);
        g_print("stream ready at rtsp://127.0.0.1:%s%s\n", DEFAULT_RTSP_PORT, str);
        g_free(str);
    }
    
    g_object_unref(mounts);
    
    gst_rtsp_server_attach(server, NULL);
    
    g_main_loop_run(loop);
    
	return 0;
}



