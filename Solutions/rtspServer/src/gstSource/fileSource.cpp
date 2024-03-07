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
#include "fileSource.h"


static void on_ssrc_active(GObject *session, GObject *source, GstRTSPMedia *media)
{
    GstStructure *stats;
    GST_INFO("source %p in session %p is active", source, session);
    
    g_object_get(source, "stats", &stats, NULL);
    if(stats){
        gchar *sstr;
        sstr = gst_structure_to_string(stats);
        g_print("structure: %s\n", sstr);
        g_free(sstr);

        gst_structure_free(stats);
    }
}

static void on_sender_ssrc_active(GObject *session, GObject *source, GstRTSPMedia *media)
{
    on_ssrc_active(session, source, media)
}

void media_prepared(GstRTSPMedia *media)
{
    guint i, n_streams;
    n_streams = gst_rtsp_media_n_streams(media);

    GST_INFO("media %p is prepared and has %u streams", media, n_streams);

    for(i = 0; i < n_streams; i++){
        GstRTSPStream *stream;
        GObject *session;

        stream = gst_rtsp_media_get_stream(media, i);
        if(stream){
            continue;
        }

        session = gst_rtsp_stream_get_rtpsession(stream);
        GST_INFO("watching session %p on stream %u", session, i);

        g_signal_connect(session, "on-ssrc-active", (GCallback)on_ssrc_active, media);
        g_signal_connect(session, "on-sender-ssrc_active", (GCallback)on_sender_ssrc_active, media);
    }
}



