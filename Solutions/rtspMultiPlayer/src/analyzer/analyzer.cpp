//=====================  C++  =====================
#include <string>
//=====================   C   =====================
#include "system.h"
//=====================  PRJ  =====================
#include "analyzer.h"
#include "../player/display.h"


//#define PTRINT uint32_t
#define PTRINT uint64_t
#define WIN_WIDTH  1920
#define WIN_HEIGHT 1080
#define DISP_BPP   4
/* rotate source image 0 degrees clockwise */
#define HAL_TRANSFORM_ROT_0 0x00

static pthread_mutex_t gmutex;

static char **gppDispMap = NULL;
static int gChnNums = 0;
int analyzer_init(char **ppDispBuf, int chnNums)
{
    gppDispMap = ppDispBuf;
    gChnNums = chnNums;

    // 图像资源处理初始化
    pthread_mutex_init(&gmutex,NULL);

    // 模型初始化
    algorithm_init();

    return 0;
}



typedef struct {
    RgaSURF_FORMAT fmt;
    int width;
    int height;
    int hor_stride;
    int ver_stride;
    int rotation;
    int fd;
    void *pBuf;
}Image;
static int srcImg_ConvertTo_dstImg(Image *pDst, Image *pSrc)
{
	rga_info_t src, dst;
	int ret = -1;

	if (!pSrc || !pDst) {
		printf("%s: NULL PTR!\n", __func__);
		return -1;
	}

    pthread_mutex_lock(&gmutex);
	//图像参数转换
	memset(&src, 0, sizeof(rga_info_t));
	if (pSrc->fd >= 0) {
		src.fd = pSrc->fd;
	} else {
		src.fd = -1;
		src.virAddr = pSrc->pBuf;
	}
	src.mmuFlag = 1;
	src.rotation =  pSrc->rotation;
	rga_set_rect(&src.rect, 0, 0, pSrc->width, pSrc->height, pSrc->hor_stride, pSrc->ver_stride, pSrc->fmt);

	memset(&dst, 0, sizeof(rga_info_t));
	if (pDst->fd >= 0) {
		dst.fd = pDst->fd;
	} else {
		dst.fd = -1;
		dst.virAddr = pDst->pBuf;
	}
	dst.mmuFlag = 1;
	dst.rotation =  pDst->rotation;
	rga_set_rect(&dst.rect, 0, 0, pDst->width, pDst->height, pDst->hor_stride, pDst->ver_stride, pDst->fmt);
	if (c_RkRgaBlit(&src, &dst, NULL)) {
		printf("%s: rga fail\n", __func__);
		ret = -1;
	}
	else {
		ret = 0;
	}
    pthread_mutex_unlock(&gmutex);

	return ret;
}
static RgaSURF_FORMAT rgaFmt(char *strFmt)
{
    if(0 == strcmp(strFmt, "NV12")){
        return RK_FORMAT_YCbCr_420_SP;
    }else if(0 == strcmp(strFmt, "NV21")){
        return RK_FORMAT_YCrCb_420_SP;
    }else if(0 == strcmp(strFmt, "BGR")){
        return RK_FORMAT_BGR_888;
    }else if(0 == strcmp(strFmt, "RGB")){
        return RK_FORMAT_RGB_888;
    }else{
        return RK_FORMAT_UNKNOWN;
    }
}
static PTRINT calcBufMapOffset(int chnId, int units, int pitch)
{
    int xUnitOffset = chnId%units;
    int yUnitOffset = chnId/units;
    
    int winWidth  = WIN_WIDTH/units;
    int winHeight = WIN_HEIGHT/units;
    
    PTRINT BufMapOffset = (PTRINT)DISP_BPP*(yUnitOffset*winHeight*pitch + xUnitOffset*winWidth);
    //printf("yU = %u, winHeight = %u, xU = %u, winWidth = %u, offset = %lu\n", yUnitOffset, winHeight, xUnitOffset, winWidth, BufMapOffset);
    return BufMapOffset;
}
static int commitImgtoDispBufMap(int chnId, void *pSrcData, RgaSURF_FORMAT srcFmt, int srcWidth, int srcHeight, int srcHStride, int srcVStride)
{
    if(gChnNums <= 0)
        return -1;

    int units = 0;
    while(1){
        units++;
        if(gChnNums <= (units*units)){
            break;
        }
    }

    int pitch = display_frame_stride() / DISP_BPP;
    if(pitch <= 0)
        pitch = WIN_WIDTH;

    Image srcImage, dstImage;
    memset(&srcImage, 0, sizeof(srcImage));
    memset(&dstImage, 0, sizeof(dstImage));
    
    srcImage.fmt = srcFmt;
    srcImage.width = srcWidth;
    srcImage.height = srcHeight;
    srcImage.hor_stride = srcHStride;
    srcImage.ver_stride = srcVStride;
    srcImage.rotation = HAL_TRANSFORM_ROT_0;
    srcImage.fd = -1;
    srcImage.pBuf = pSrcData;

    PTRINT dstBufPtr = (PTRINT)*gppDispMap + calcBufMapOffset(chnId, units, pitch);
    dstImage.fmt = RK_FORMAT_BGRA_8888;
    dstImage.width = WIN_WIDTH/units;
    dstImage.height = WIN_HEIGHT/units;
    dstImage.hor_stride = pitch;
    dstImage.ver_stride = WIN_HEIGHT/units;
    dstImage.rotation = HAL_TRANSFORM_ROT_0;
    dstImage.fd = display_frame_fd();
    dstImage.pBuf = (void *)dstBufPtr;
    
    return srcImg_ConvertTo_dstImg(&dstImage, &srcImage);
}
int videoOutHandle(char *imgData, ImgDesc_t imgDesc)
{
    if(*gppDispMap){
        if(0 == commitImgtoDispBufMap(imgDesc.chnId, (void *)imgData, rgaFmt(imgDesc.fmt), imgDesc.width, imgDesc.height, imgDesc.horStride, imgDesc.verStride))
            display_frame_commit();
    }
    algorithm_process();
    
    return 0;
}

