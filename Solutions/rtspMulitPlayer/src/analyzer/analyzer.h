#ifndef __ANALYZER_H__
#define __ANALYZER_H__

#include <stdint.h>

extern int analyzer_init(char **ppDispBuf, int chnNums);

typedef struct {
    char fmt[16];
    int chnId;
    int width;
    int height;
    int dataSize;
}ImgDesc_t;
extern int videoOutHandle(char *imgData, ImgDesc_t imgDesc);

#endif

