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
#include "gstSource/appSource.h"
#include "gstSource/fileSource.h"

#define DEFAULT_RTSP_PORT   "8554"
#define ATTACH_URL          "/test"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
static SrcCfg_t SrcCfg_tab[] = {
	{
		.srcType   = "v4l2src",
		.loaction  = "/dev/video44",
		.videoDesc = {"NV12", FALSE, 1920, 1080, 30, 0, "h264"},
		.audioDstType = "null",
	}, {
		.srcType   = "v4l2src",
		.loaction  = "/dev/video53",
		.videoDesc = {"NV12", FALSE, 1920, 1080, 30, 0, "h264"},
		.audioDstType = "null",
	}, {
		.srcType   = "v4l2src",
		.loaction  = "/dev/video71",
		.videoDesc = {"NV12", FALSE, 1920, 1080, 30, 0, "h264"},
		.audioDstType = "null",
	}, {
		.srcType   = "v4l2src",
		.loaction  = "/dev/video62",
		.videoDesc = {"NV12", FALSE, 1920, 1080, 60, 0, "h265"},
		.audioDstType = "null",
/*
	}, {
        .srcType   = "filesrc",
        .loaction  = "./Release/1.mp4",
		.videoDesc = {"NV12", FALSE, 1920, 1080, 25, 0, "h264"},
*/
    }
};

/* called when a new media pipeline is constructed. We can query the
 * pipeline and configure our appsrc */
static void media_configure(GstRTSPMediaFactory * factory, GstRTSPMedia *media, gpointer user_data)
{
    SrcCfg_t *pSourceCfg = (SrcCfg_t *)user_data;
    if(0 == strcmp(pSourceCfg->srcType, "filesrc")){
        g_signal_connect(media, "prepared", (GCallback)media_prepared, user_data);

    }else if(0 == strcmp(pSourceCfg->srcType, "v4l2src")){
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
}

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
    gchar strVideoParse[32], strVideoEncoder[32], strVideoPayload[32];
    for(int index = 0; index < count; index++){
        memset(strVideoParse, 0, sizeof(strVideoParse));
        memset(strVideoEncoder, 0, sizeof(strVideoEncoder));
        memset(strVideoPayload, 0, sizeof(strVideoPayload));
        if(0 == strcmp(SrcCfg_tab[index].videoDesc.videoDstType, "h265")){
            snprintf(strVideoParse, sizeof(strVideoParse), "h265parse");
            snprintf(strVideoEncoder, sizeof(strVideoEncoder), "mpph265enc");
            snprintf(strVideoPayload, sizeof(strVideoPayload), "rtph265pay");
        }else{  //default: h264
            snprintf(strVideoParse, sizeof(strVideoParse), "h264parse");
            snprintf(strVideoEncoder, sizeof(strVideoEncoder), "mpph264enc");
            snprintf(strVideoPayload, sizeof(strVideoPayload), "rtph264pay");
        }
    
        if(0 == strcmp(SrcCfg_tab[index].srcType, "filesrc")){
            str = g_strdup_printf("filesrc location=%s ! qtdemux ! %s ! %s name=pay0 pt=96", SrcCfg_tab[index].loaction, strVideoParse, strVideoPayload);
        }else if(0 == strcmp(SrcCfg_tab[index].srcType, "v4l2src")){
            str = g_strdup_printf("appsrc name=videosrc ! %s ! %s name=pay0 pt=96", strVideoEncoder, strVideoPayload);
        }
        factory = gst_rtsp_media_factory_new();
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



