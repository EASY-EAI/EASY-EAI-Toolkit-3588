//=====================  C++  =====================
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
//=====================   C   =====================
#include <gst/video/video-info.h>
#include "system.h"
//#include "network.h"
//#include "ipcData.h"
//#include "config.h"
//=====================  SDK  =====================
#include "system_opt.h"
//#include "ini_wrapper.h"
//#include "ipc.h"
//=====================  PRJ  =====================
#include "../analyzer/analyzer.h"
#include "capturer.h"

// 错误处理函数
static gboolean on_error(GstBus *bus, GstMessage *message, gpointer data) {
    GError *err;
    gchar *debug_info;

    // 解析错误消息
    gst_message_parse_error(message, &err, &debug_info);
    
    g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(message->src), err->message);
    g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");

    g_error_free(err);
    g_free(debug_info);

    // 返回FALSE表示从消息处理队列中删除消息
    return TRUE;
}

/* Functions below print the Capabilities in a human-friendly format */
static gboolean print_pad_info (GQuark field, const GValue * value, gpointer pfx) {
    gchar *str = gst_value_serialize (value);
    //g_print("%s  %15s: %s\n", (gchar *)pfx, g_quark_to_string (field), str);
    g_free(str);
    return TRUE;
}

/* This function will be called by the pad-added signal */
static void pad_added_handler (GstElement *src, GstPad *new_pad, GstChannel_t *data)
{
    GstPadLinkReturn ret;
    GstPad *video_sinkPad = NULL;
    GstPad *audio_sinkPad = NULL;
    
    g_print ("Received new pad '%s' from '%s':\n", GST_PAD_NAME (new_pad), GST_ELEMENT_NAME (src));

    /* Check the new pad's type */
    GstCaps *new_pad_caps = gst_pad_get_current_caps (new_pad);
    GstStructure *new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);

    //把Pads的信息都打印出来
    gst_structure_foreach(new_pad_struct, print_pad_info, (gpointer)"     ");
    
    const gchar *new_pad_type = gst_structure_get_name(new_pad_struct);
    const gchar *new_pad_media_type = gst_structure_get_string(new_pad_struct, "media");
    //const gchar *new_pad_media_type = gst_value_serialize(gst_structure_get_value(new_pad_struct, "media"));
    //sleep(10);
    if (g_str_has_prefix (new_pad_media_type, "video")) {
        video_sinkPad = gst_element_get_static_pad (data->h26xRTPDepay, "sink");
        if(video_sinkPad){
            if(gst_pad_is_linked(video_sinkPad)){
                g_print ("We are '%s' already linked. Ignoring.\n", new_pad_media_type);
                goto exit;
            }
            /* Attempt the link */
            ret = gst_pad_link (new_pad, video_sinkPad);
        }
        
    }else if(g_str_has_prefix (new_pad_media_type, "audio")){
        audio_sinkPad = gst_element_get_static_pad(data->aQueue, "sink");
        if(audio_sinkPad){
            if(gst_pad_is_linked(audio_sinkPad)){
                g_print ("We are '%s' already linked. Ignoring.\n", new_pad_media_type);
                goto exit;
            }
            /* Attempt the link */
            ret = gst_pad_link (new_pad, audio_sinkPad);
        }

    }else{
        g_print ("It has type '%s-[%s]' which is not raw video or audio. Ignoring.\n", new_pad_type, new_pad_media_type);
        goto exit;
    }
    
    if (GST_PAD_LINK_FAILED (ret)) {
        g_print ("Type is '%s-[%s]' but link failed.\n", new_pad_type, new_pad_media_type);
    } else {
        g_print ("Link succeeded (type '%s-[%s]').\n", new_pad_type, new_pad_media_type);
    }

exit:
    /* Unreference the new pad's caps, if we got them */
    if (new_pad_caps != NULL)
        gst_caps_unref (new_pad_caps);
    
    /* Unreference the sink pad */
    if(video_sinkPad)
        gst_object_unref(video_sinkPad);
    if(audio_sinkPad)
        gst_object_unref(audio_sinkPad);
}

// 回调函数，用于处理从 pad 接收到的数据
static GstPadProbeReturn padProbeCallback(GstPad *pad, GstPadProbeInfo *probeInfo, gpointer user_data) {
    // 格式化sinkPad信息
    GstCaps *caps = gst_pad_get_current_caps(pad);
    GstStructure *structure = gst_caps_get_structure(caps, 0);
    const gchar *fmt = gst_structure_get_string(structure, "format");
    GstVideoInfo videoInfo;
    gst_video_info_from_caps(&videoInfo, caps);
    gint width  = GST_VIDEO_INFO_WIDTH(&videoInfo);
    gint height = GST_VIDEO_INFO_HEIGHT(&videoInfo);
    if ((0==width)||(0==height)) {
        g_print ("gst_structure_get_int fail\n");
        return GST_PAD_PROBE_DROP;
    }
    
    // info和buffer中的数据不能直接操作
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(probeInfo);
    // >> 使用mapinfo获取图像数据
    GstMapInfo map_info;
    if (!gst_buffer_map(buffer, &map_info, GST_MAP_READ)) {		//映射出数据
        g_print("gst_buffer_map() error!");
        return GST_PAD_PROBE_DROP;
    }
    g_print("%s [%dx%d] data size = %ld \n", fmt, width, height, map_info.size);
    
    return GST_PAD_PROBE_OK;
}

static GstFlowReturn new_sample(GstElement *sink, gpointer user_data){
    GstChannel_t *data = (GstChannel_t *)user_data;

    /* Retrieve the buffer */
    GstSample *sample;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (sample){
        //提取一帧sample中的caps, 通过caps获取【视频数据】的【描述信息】
        GstCaps *caps = gst_sample_get_caps(sample);
        if (!caps) {
            g_print ("gst_sample_get_caps fail\n");
            gst_sample_unref(sample);
            return GST_FLOW_ERROR;
        }
        GstStructure *structure = gst_caps_get_structure(caps, 0);
        const gchar *fmt = gst_structure_get_string(structure, "format");
        GstVideoInfo video_info;
        gst_video_info_from_caps(&video_info, caps);
        gint width  = GST_VIDEO_INFO_WIDTH(&video_info);
        gint height = GST_VIDEO_INFO_HEIGHT(&video_info);
        if ((0==width)||(0==height)) {
            g_print ("gst_structure_get_int fail\n");
            gst_sample_unref (sample);
            return GST_FLOW_ERROR;
        }
        
        //提取一帧sample中的buffer, 注意:这个buffer是无法直接用的,它不是char类型
        GstBuffer *buffer = gst_sample_get_buffer(sample);		
        if(!buffer){
            g_print ("gst_sample_get_buffer fail\n");
            gst_sample_unref (sample);
            return GST_FLOW_ERROR;
        }
        //把buffer映射到map，这样我们就可以通过map.data取到buffer的数据
        GstMapInfo map;
        if (gst_buffer_map (buffer, &map, GST_MAP_READ)){
            // g_print("data size = %ld , info_size = %ld\n", map.size, GST_VIDEO_INFO_SIZE(&video_info));
            ImgDesc_t imgDesc = {0};
            imgDesc.chnId = data->chnId;
            imgDesc.width = width;
            imgDesc.height = height;
            imgDesc.dataSize = map.size;
            strcpy(imgDesc.fmt, fmt);
            videoOutHandle((char *)map.data, imgDesc);
            
            gst_buffer_unmap (buffer, &map);	//解除映射
        }
        gst_sample_unref (sample);	//释放资源
        return GST_FLOW_OK;
    }
    return GST_FLOW_OK ;
}

// =======================================================================================================
void *busListen(void *para)
{
	GstElement *pPipeLine = (GstElement *)para;
    if(NULL == pPipeLine)
	    pthread_exit(NULL);
        
    /* Listen to the bus */
    GstBus *bus = gst_element_get_bus (pPipeLine);
    // 连接到错误信号
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::error", G_CALLBACK(on_error), NULL);
    
    gboolean terminate = FALSE;
    GstMessage *msg;
    do {
        msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, (GstMessageType)(GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_WARNING | GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        /* Parse message */
        if (msg != NULL) {
            GError *err;
            gchar *debug_info;
            
            switch (GST_MESSAGE_TYPE (msg)) {
                case GST_MESSAGE_ERROR:
                    gst_message_parse_error (msg, &err, &debug_info);
                    g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
                    g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
                    g_clear_error (&err);
                    g_free (debug_info);
                    terminate = TRUE;
                    break;
                case GST_MESSAGE_EOS:
                    g_print ("End-Of-Stream reached.\n");
                    terminate = TRUE;
                    break;
                case GST_MESSAGE_STATE_CHANGED:
                    /* We are only interested in state-changed messages from the pipeline */
                    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (pPipeLine)) {
                        GstState old_state, new_state, pending_state;
                        gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
                        g_print ("Pipeline state changed from %s to %s:\n",
                        gst_element_state_get_name (old_state), gst_element_state_get_name (new_state));
                    }
                    break;
                default:
                    /* We should not reach here */
                    g_printerr ("Unexpected message received.\n");
                    break;
            }
            gst_message_unref (msg);
        }
    } while (!terminate);

    /* Free resources */
    gst_object_unref(bus);
    gst_element_set_state (pPipeLine, GST_STATE_NULL);
    gst_object_unref (pPipeLine);
   
    pthread_exit(NULL);
}

DecChannel::DecChannel(int chnId, std::string strUrl, std::string strVedioFmt) :
	bObjIsInited(false),
    mStrUrl(strUrl),
    mStrVideoFmt(strVedioFmt)
{
    memset(&mGstChn, 0, sizeof(mGstChn));
    mGstChn.chnId = chnId;
}
DecChannel::~DecChannel()
{

}
int DecChannel::init()
{
    //CustomData data;
    GstStateChangeReturn ret;

    /* Create the empty pipeline */
    mGstChn.pipeline = gst_pipeline_new ("test-pipeline");
    /* Create the elements */
    mGstChn.source = gst_element_factory_make("rtspsrc", "source");    
    if (!mGstChn.pipeline || !mGstChn.source ) {
        g_printerr ("Not all elements could be created.\n");
        return -1;
    }
    
    /* Build the pipeline. Note that we are NOT linking the source at this
     * point. We will do it later. */
    gst_bin_add_many(GST_BIN(mGstChn.pipeline), mGstChn.source, NULL);

    /* Create video & audio decode channel */
    if(0 != createVideoDecChannel()){
        gst_object_unref (mGstChn.pipeline);
        return -1;
    }
    //if(0 != createAudioDecChannel()){
    //    gst_object_unref (mGstChn.pipeline);
    //    return -1;
    //}
    
    /* Set the URL to play */
    g_object_set(mGstChn.source, "location", mStrUrl.c_str(), NULL);
    /* Connect to the pad-added signal */
    g_signal_connect(mGstChn.source, "pad-added", G_CALLBACK(pad_added_handler), &mGstChn);

    /* Start playing */
    ret = gst_element_set_state (mGstChn.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr ("Unable to set the pipeline to the playing state.\n");
        gst_object_unref (mGstChn.pipeline);
        return -1;
    }

	if(0 == CreateNormalThread(busListen, mGstChn.pipeline, &mTid)){
        bObjIsInited = true;
	}
    
    return 0;
}

int DecChannel::createVideoDecChannel()
{
    if(0 == strcmp(mStrVideoFmt.c_str(), "h264")){
        mGstChn.h26xRTPDepay = gst_element_factory_make("rtph264depay",  "h26xRTPDepay");
        mGstChn.h26xParse    = gst_element_factory_make("h264parse",     "h26xParse");
    }else if(0 == strcmp(mStrVideoFmt.c_str(), "h265")){
        mGstChn.h26xRTPDepay = gst_element_factory_make("rtph265depay",  "h26xRTPDepay");
        mGstChn.h26xParse    = gst_element_factory_make("h265parse",     "h26xParse");
    }else{
        g_printerr ("invaild stream format.\n");
        return -1;
    }
    //mGstChn.vDec        = gst_element_factory_make("avdec_h265", "vDec");
    mGstChn.vDec        = gst_element_factory_make("mppvideodec", "vDec");
    mGstChn.vScale      = gst_element_factory_make("videoscale",  "vScale");
    mGstChn.vCapsfilter = gst_element_factory_make("capsfilter",  "vCapsfilter");
    mGstChn.vSink       = gst_element_factory_make("appsink",     "vSink");

    if (!mGstChn.h26xRTPDepay || !mGstChn.h26xParse || !mGstChn.vDec || !mGstChn.vScale || !mGstChn.vCapsfilter || !mGstChn.vSink) {
        g_printerr ("Not all video elements could be created.\n");
        return -1;
    }

#if 0 //由于图像缩放比较吃CPU，因此这里暂不开放
    // vCapsfilter要与vScale搭配使用，此处操作是设置vScale的属性，例如 width 和 height。
    // <==> gst-launch-1.0命令的"... ! videoscale ! video/x-raw,width=1280,height=720 ! ..."
    GstCaps *caps = gst_caps_new_simple("video/x-raw",   "width",G_TYPE_INT,1280,   "height",G_TYPE_INT,720,   NULL);
    g_object_set(mGstChn.vCapsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);
#endif
    // 把
    g_object_set(mGstChn.vDec, "format", GST_VIDEO_FORMAT_BGR, NULL);
#if 0
     //date probe
     GstPad *pad = gst_element_get_static_pad(mGstChn.vSink, "sink");
     gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback)padProbeCallback, NULL, NULL);
     gst_object_unref (pad);
#else
    //  用appsink接住输出的视频流，用作后续处理
    //  参考：https://blog.csdn.net/qq_41563600/article/details/121257849
    //GstCaps *caps = gst_caps_new_simple("video/x-raw",   "format",G_TYPE_STRING,"BGR",   NULL);
    GstCaps *caps = gst_caps_from_string(g_strdup_printf("video/x-raw,format=BGR"));
    if(caps){
        g_object_set(mGstChn.vSink, "caps", caps, NULL);
        gst_caps_unref(caps);
    }
    g_object_set(mGstChn.vSink, "emit-signals", TRUE, NULL);
    g_signal_connect(mGstChn.vSink, "new-sample", G_CALLBACK(new_sample), &mGstChn);
#endif
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.h26xRTPDepay);
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.h26xParse);
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.vDec);
    //gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.vScale);
    //gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.vCapsfilter);
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.vSink);

    if (!gst_element_link_many (mGstChn.h26xRTPDepay, mGstChn.h26xParse, mGstChn.vDec, /*mGstChn.vScale, mGstChn.vCapsfilter,*/ mGstChn.vSink, NULL)) {
        g_printerr ("Video Elements could not be linked.\n");
        return -1;
    }

    return 0;
}
int DecChannel::createAudioDecChannel()
{
    mGstChn.aQueue        = gst_element_factory_make("queue", "aQueue");
    
    mGstChn.audioRTPDepay = gst_element_factory_make("rtppcmadepay", "pcmaRTPDepay");
    mGstChn.aDec          = gst_element_factory_make("alawdec", "aDec");
    
    mGstChn.aConvert  = gst_element_factory_make("audioconvert",  "aConvert");
    mGstChn.aResample = gst_element_factory_make("audioresample", "aResample");
    mGstChn.aSink     = gst_element_factory_make("autoaudiosink", "aSink");
    
    if (!mGstChn.aQueue || !mGstChn.audioRTPDepay || !mGstChn.aDec || !mGstChn.aConvert || !mGstChn.aResample || !mGstChn.aSink) {
        g_printerr ("Not all audio elements could be created.\n");
        return -1;
    }
    
    //gst_bin_add_many(GST_BIN(mGstChn.pipeline), mGstChn.audioRTPDepay, mGstChn.aDec, mGstChn.aConvert, mGstChn.aResample, mGstChn.aSink, NULL);
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.aQueue);
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.audioRTPDepay);
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.aDec);
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.aConvert);
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.aResample);
    gst_bin_add(GST_BIN(mGstChn.pipeline), mGstChn.aSink);
    
    if (!gst_element_link_many(mGstChn.aQueue, mGstChn.audioRTPDepay, mGstChn.aDec, mGstChn.aConvert, mGstChn.aResample, mGstChn.aSink, NULL)) {
        g_printerr ("Audio Elements could not be linked.\n");
        return -1;
    }
    return 0;
}

