#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *winTitle;
    int x;
    int y;
    int width;
    int height;
}Display_t;

extern char **dispBufferMap(Display_t *dispDesc);
extern int display(Display_t *dispDesc);

#endif

