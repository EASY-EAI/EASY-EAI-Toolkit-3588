//=====================  C++  =====================
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
//=====================   C   =====================
#include "system.h"
//#include "network.h"
//#include "ipcData.h"
//#include "config.h"
//=====================  SDK  =====================
#include "system_opt.h"
//#include "ini_wrapper.h"
//#include "ipc.h"
//=====================  PRJ  =====================
#include "playChn.h"

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
    
    const gchar *new_pad_type = gst_structure_get_name(new_pad_struct);
    const gchar *new_pad_media_type = gst_value_serialize(gst_structure_get_value(new_pad_struct, "media"));
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
        //audio_sinkPad = gst_element_get_static_pad(data->aConvert, "sink");
        if(audio_sinkPad){
            if(gst_pad_is_linked(audio_sinkPad)){
                g_print ("We are '%s' already linked. Ignoring.\n", new_pad_media_type);
                goto exit;
            }
            /* Attempt the link */
    //    ret = gst_pad_link (new_pad, audio_sinkPad);
            ret = GST_PAD_LINK_WAS_LINKED;
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

PlayChannel::PlayChannel(std::string strUrl, std::string strVedioFmt) :
	bObjIsInited(false),
    mStrUrl(strUrl),
    mStrVideoFmt(strVedioFmt)
{
    memset(&mGstChn, 0, sizeof(mGstChn));
}
PlayChannel::~PlayChannel()
{

}
int PlayChannel::init()
{
    //CustomData data;
    GstBus *bus;
    GstMessage *msg;
    GstStateChangeReturn ret;
    gboolean terminate = FALSE;

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
    createVideoDecChannel();
    //createAudioDecChannel();
    
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
    //createVideoDecChannel(&data);

    /* Listen to the bus */
    bus = gst_element_get_bus (mGstChn.pipeline);
    // 连接到错误信号
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::error", G_CALLBACK(on_error), NULL);
    bObjIsInited = true;
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
                    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (mGstChn.pipeline)) {
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
   gst_element_set_state (mGstChn.pipeline, GST_STATE_NULL);
   gst_object_unref (mGstChn.pipeline);
   
   return 0;
}

int PlayChannel::createVideoDecChannel()
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
    mGstChn.vDec  = gst_element_factory_make("mppvideodec",   "vDec");
    mGstChn.vSink = gst_element_factory_make("autovideosink", "vSink");

    if (!mGstChn.h26xRTPDepay || !mGstChn.h26xParse || !mGstChn.vDec || !mGstChn.vSink) {
        g_printerr ("Not all video elements could be created.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(mGstChn.pipeline), mGstChn.h26xRTPDepay, mGstChn.h26xParse, mGstChn.vDec, mGstChn.vSink, NULL);

    if (!gst_element_link_many (mGstChn.h26xRTPDepay, mGstChn.h26xParse, mGstChn.vDec, mGstChn.vSink, NULL)) {
        g_printerr ("Video Elements could not be linked.\n");
        gst_object_unref (mGstChn.pipeline);
        return -1;
    }

    return 0;
}
int PlayChannel::createAudioDecChannel()
{
    mGstChn.aConvert  = gst_element_factory_make("audioconvert", "aConvert");
    mGstChn.aResample = gst_element_factory_make("audioresample", "aResample");
    mGstChn.aSink     = gst_element_factory_make("autoaudiosink", "aSink");

    if (!mGstChn.aConvert || !mGstChn.aResample || !mGstChn.aSink) {
        g_printerr ("Not all audio elements could be created.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(mGstChn.pipeline), mGstChn.aConvert, mGstChn.aResample, mGstChn.aSink, NULL);
    
    if (!gst_element_link_many (mGstChn.aConvert, mGstChn.aResample, mGstChn.aSink, NULL)) {
        g_printerr ("Audio Elements could not be linked.\n");
        gst_object_unref (mGstChn.pipeline);
        return -1;
    }
    return 0;
}

