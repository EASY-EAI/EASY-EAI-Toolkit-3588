/**
*
* Copyright 2024 by Guangzhou Easy EAI Technologny Co.,Ltd.
* website: www.easy-eai.com
*
* Author: Jiehao.Zhong <zhongjiehao@easy-eai.com>
* 
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* License file for more details.
* 
*/

#ifndef GST_OPERATION_H
#define GST_OPERATION_H

#include <gst/gst.h>

typedef struct {
    gchar strFmt[32];
    gint width;
    gint height;
    gint horStride;
    gint verStride;
}FrameDesc_t;

#if defined(__cplusplus)
extern "C" {
#endif

extern GstBuffer *gstopt_sample_get_buffer(GstSample *sample, FrameDesc_t *pFrameDesc);

#if defined(__cplusplus)
}
#endif

#endif

