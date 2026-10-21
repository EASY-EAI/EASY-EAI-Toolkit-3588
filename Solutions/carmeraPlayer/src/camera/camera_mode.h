#ifndef CAMERA_MODE_H
#define CAMERA_MODE_H

/* Stop AIQ and configure 4K RAW12 before restarting it; 1080p uses board setup. */
int camera_mode_prepare(int width, int height, const int *video_nums, int count);

#endif
