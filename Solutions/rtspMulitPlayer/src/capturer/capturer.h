#ifndef __CAPTURER_H__
#define __CAPTURER_H__

#include <stdbool.h>
#include <stdint.h>

#include "decChannel.h"

class Capturer
{
public:
	Capturer(int chnId, SrcCfg_t config);
	~Capturer();

    int32_t init(){if(mpDecChannel){return mpDecChannel->init();}else{return -1;}}
	int32_t IsInited(){if(mpDecChannel){return mpDecChannel->IsInited();}else{return 0;}}
    //int32_t channelId(){return mi32ChnId;}
    
private:
    DecChannel *mpDecChannel;
};

#endif
