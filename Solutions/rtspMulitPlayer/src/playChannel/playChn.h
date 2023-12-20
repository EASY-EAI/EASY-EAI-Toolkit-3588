#ifndef __PLAYCHANNEL_H__
#define __PLAYCHANNEL_H__

#include <gst/gst.h>

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct gst_Channel {
    GstElement *pipeline;
    GstElement *source;
    // ====== video ======
    GstElement *h26xRTPDepay;
    GstElement *h26xParse;
    GstElement *vDec;
    GstElement *vSink;
    // ====== audio ======
    GstElement *aConvert;
    GstElement *aResample;
    GstElement *aSink;
} GstChannel_t;

class PlayChannel
{
public:
	PlayChannel(std::string strUrl, std::string strVedioFmt = "h264");
	~PlayChannel();

	int init();
	int32_t IsInited(){return bObjIsInited;}
    //int32_t channelId(){return m_dwChnId;}

    GstChannel_t mGstChn;

protected:
	int createVideoDecChannel();
    int createAudioDecChannel();
private:
    //int32_t m_dwChnId;
    
	pthread_t mTid;
	int bObjIsInited;

    std::string mStrUrl;
    std::string mStrVideoFmt;
};

#endif
