//=====================  C++  =====================
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
//=====================  PRJ  =====================
#include "playChannel/playChn.h"

int main(int argc, char **argv)
{
    /* Initialize GStreamer */
    gst_init(&argc, &argv);
    
    PlayChannel playChn1("rtsp://admin:a12345678@192.168.1.68");
    playChn1.init();
    
   return 0;
}

