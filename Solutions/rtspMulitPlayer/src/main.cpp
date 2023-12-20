//=====================  C++  =====================
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
//======================  C  ======================
#include <unistd.h>
//=====================  PRJ  =====================
#include "playChannel/playChn.h"

int main(int argc, char **argv)
{
    /* Initialize GStreamer */
    gst_init(&argc, &argv);
    
    PlayChannel playChn1("rtsp://admin:a12345678@192.168.1.68");
    playChn1.init();

    //PlayChannel playChn2("rtsp://admin:a12345678@192.168.1.69");
    //playChn2.init();

    while(1){
        sleep(2);
    }
    
    return 0;
}

