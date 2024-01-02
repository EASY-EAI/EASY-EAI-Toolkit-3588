//=====================  C++  =====================
#include <string>
//=====================   C   =====================
#include "system.h"
//=====================  PRJ  =====================
#include "capturer.h"

Capturer::Capturer(int chnId, std::string strUrl, std::string strVedioFmt) :
    mpDecChannel(NULL)
{
    mpDecChannel = new DecChannel(chnId, strUrl, strVedioFmt);
}
Capturer::~Capturer()
{
    if(mpDecChannel)
        delete mpDecChannel;
    mpDecChannel = NULL;
}

