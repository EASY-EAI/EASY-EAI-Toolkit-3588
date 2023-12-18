/**
 *
 * Copyright 2021 by Guangzhou Easy EAI Technologny Co.,Ltd.
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
 
#include <sys/time.h>

#include <wchar.h>
#include <assert.h>
#include <locale.h>
#include <ctype.h>
#include <cmath>

#include "CvxText.h"

#include "font_engine.h"
 
// 打开字库
CvxText::CvxText(const char* freeType)
{
    assert(freeType != NULL);
 
    // 打开字库文件, 创建一个字体
    if(FT_Init_FreeType(&m_library)) throw;
    if(FT_New_Face(m_library, freeType, 0, &m_face)) throw;
 
    // 设置字体输出参数
    restoreFont();
 
    // 设置C语言的字符集环境
    setlocale(LC_ALL, "");
}
 
// 释放FreeType资源
CvxText::~CvxText()
{
    FT_Done_Face(m_face);
    FT_Done_FreeType(m_library);
}
 
// 设置字体参数:
//
// font         - 字体类型, 目前不支持
// size         - 字体大小/空白比例/间隔比例/旋转角度
// underline   - 下画线
// diaphaneity   - 透明度
void CvxText::getFont(int* type, cv::Scalar* size, bool* underline, float* diaphaneity)
{
    if (type) *type = m_fontType;
    if (size) *size = m_fontSize;
    if (underline) *underline = m_fontUnderline;
    if (diaphaneity) *diaphaneity = m_fontDiaphaneity;
}
 
void CvxText::setFont(int* type, cv::Scalar* size, bool* underline, float* diaphaneity)
{
    // 参数合法性检查
    if (type) {
        if(type >= 0) m_fontType = *type;
    }
    if (size) {
        m_fontSize.val[0] = std::fabs(size->val[0]);
        m_fontSize.val[1] = std::fabs(size->val[1]);
        m_fontSize.val[2] = std::fabs(size->val[2]);
        m_fontSize.val[3] = std::fabs(size->val[3]);
    }
    if (underline) {
        m_fontUnderline   = *underline;
    }
    if (diaphaneity) {
        m_fontDiaphaneity = *diaphaneity;
    }
 
    FT_Set_Pixel_Sizes(m_face, (int)m_fontSize.val[0], 0);
}
 
// 恢复原始的字体设置
void CvxText::restoreFont()
{
    m_fontType = 0;            // 字体类型(不支持)
 
    m_fontSize.val[0] = 20;      // 字体大小
    m_fontSize.val[1] = 0.5;   // 空白字符大小比例
    m_fontSize.val[2] = 0.1;   // 间隔大小比例
    m_fontSize.val[3] = 0;      // 旋转角度(不支持)
 
    m_fontUnderline   = false;   // 下画线(不支持)
 
    m_fontDiaphaneity = 1.0;   // 色彩比例(可产生透明效果)
 
    // 设置字符大小
    FT_Set_Pixel_Sizes(m_face, (int)m_fontSize.val[0], 0);
}
 
// 输出函数(颜色默认为白色)
int CvxText::putText(cv::Mat& img, char* text, cv::Point pos)
{
    return putText(img, text, pos, CV_RGB(255, 255, 255));
}
 
int CvxText::putText(cv::Mat& img, const wchar_t* text, cv::Point pos)
{
    return putText(img, text, pos, CV_RGB(255,255,255));
}
 
int CvxText::putText(cv::Mat& img, const char* text, cv::Point pos, cv::Scalar color)
{
    if (img.data == nullptr) return -1;
    if (text == nullptr) return -1;
 
    int i;
    for (i = 0; text[i] != '\0'; ++i) {
        wchar_t wc = text[i];
 
        // 解析双字节符号
        if(!isascii(wc)) mbtowc(&wc, &text[i++], 2);
 
        // 输出当前的字符
        putWChar(img, wc, pos, color);
    }
 
    return i;
}
 
int CvxText::putText(cv::Mat& img, const wchar_t* text, cv::Point pos, cv::Scalar color)
{
    if (img.data == nullptr) return -1;
    if (text == nullptr) return -1;
 
    int i;
    for(i = 0; text[i] != '\0'; ++i) {
        // 输出当前的字符
        putWChar(img, text[i], pos, color);
    }
 
    return i;
}
 
// 输出当前字符, 更新m_pos位置
void CvxText::putWChar(cv::Mat& img, wchar_t wc, cv::Point& pos, cv::Scalar color)
{
    // 根据unicode生成字体的二值位图
    FT_UInt glyph_index = FT_Get_Char_Index(m_face, wc);
    FT_Load_Glyph(m_face, glyph_index, FT_LOAD_DEFAULT);
    FT_Render_Glyph(m_face->glyph, FT_RENDER_MODE_MONO);
 
    FT_GlyphSlot slot = m_face->glyph;
 
    // 行列数
    int rows = slot->bitmap.rows;
    int cols = slot->bitmap.width;
 
    for (int i = 0; i < rows; ++i) {
        for(int j = 0; j < cols; ++j) {
            int off  = i * slot->bitmap.pitch + j/8;
 
            if (slot->bitmap.buffer[off] & (0xC0 >> (j%8))) {
                int r = pos.y - (rows-1-i);
                int c = pos.x + j;
 
                if(r >= 0 && r < img.rows && c >= 0 && c < img.cols) {
                    cv::Vec3b pixel = img.at<cv::Vec3b>(cv::Point(c, r));
                    cv::Scalar scalar = cv::Scalar(pixel.val[0], pixel.val[1], pixel.val[2]);
 
                    // 进行色彩融合
                    float p = m_fontDiaphaneity;
                    for (int k = 0; k < 4; ++k) {
                        scalar.val[k] = scalar.val[k]*(1-p) + color.val[k]*p;
                    }
 
                    img.at<cv::Vec3b>(cv::Point(c, r))[0] = (unsigned char)(scalar.val[0]);
                    img.at<cv::Vec3b>(cv::Point(c, r))[1] = (unsigned char)(scalar.val[1]);
                    img.at<cv::Vec3b>(cv::Point(c, r))[2] = (unsigned char)(scalar.val[2]);
                }
            }
        }
    }
 
    // 修改下一个字的输出位置
    double space = m_fontSize.val[0]*m_fontSize.val[1];
    double sep   = m_fontSize.val[0]*m_fontSize.val[2];
 
    pos.x += (int)((cols? cols: space) + sep);
}

static void uniCode_to_Wchar(wchar_t *pWCharStr, char *pUniCodeStr, int32_t wCharSize)
{
	uint8_t *pWChar = (uint8_t *)pWCharStr;
	uint8_t *pUniCode = (uint8_t *)pUniCodeStr;
	
	memset(pWChar, 0, wCharSize*sizeof(wchar_t));
	for(int i = 0; i < wCharSize; i++){
		*pWChar = *pUniCode;
		pUniCode++; pWChar++;
		
		*pWChar = *pUniCode;
		pUniCode++; pWChar += 3;
	}	
}
static int ToWchar(const char* &src, wchar_t* &dest, const char *code)
{
    if (src == NULL) {
        dest = NULL;
        return 0;
    }
	
	int w_size = 0; //字符数量
	if(0 == strcmp(code, CODE_UTF8)){
		w_size = utf8_strlen(src) + 1;
	}else if(0 == strcmp(code, CODE_GBK)){
		w_size = 1;//gbk_strlen(src) + 1;
	}
	
	// 每个UniCode占两个字节
	char *pUniCodeStr = (char *)malloc(2*w_size);
	
	if(pUniCodeStr){
		memset(pUniCodeStr, 0, 2*w_size);
		if(0 == strcmp(code, CODE_UTF8)){
			utf8_to_unicode(src, pUniCodeStr, 2*w_size/*uniCodeSize*/);
		}else if(0 == strcmp(code, CODE_GBK)){
			gbk_to_unicode(src, pUniCodeStr, 2*w_size/*uniCodeSize*/);
		}
		
		dest = new wchar_t[w_size];
		if(dest){
			uniCode_to_Wchar(dest, pUniCodeStr, w_size);
		}
		free(pUniCodeStr);
	}
	
	
	return 0;
}

Font_t *pg_Font = NULL;
int32_t global_font_create(const char *fontLibPath, const char *codec)
{
	int charLen = 0;
	
	pg_Font = (Font_t *)malloc(sizeof(Font_t));
	if(pg_Font){
		memset(pg_Font, 0, sizeof(Font_t));
		
		charLen = strlen(fontLibPath) >= (sizeof(pg_Font->fontLib)-1) ? (sizeof(pg_Font->fontLib)-1) : strlen(fontLibPath);
		memcpy(pg_Font->fontLib, fontLibPath, charLen);
		
		charLen = strlen(codec) >= (sizeof(pg_Font->textCodec)-1) ? (sizeof(pg_Font->textCodec)-1) : strlen(codec);
		memcpy(pg_Font->textCodec, codec, charLen);
		
		return 0;
	}
	
	return -1;
}

int32_t global_font_set_textCodec(const char *codec)
{
	int charLen = 0;
	
	if(pg_Font){
		memset(pg_Font->textCodec, 0, sizeof(pg_Font->textCodec));
		
		charLen = strlen(codec) >= (sizeof(pg_Font->textCodec)-1) ? (sizeof(pg_Font->textCodec)-1) : strlen(codec);
		memcpy(pg_Font->textCodec, codec, charLen);	
		return 0;
	}
	return -1;
}

int32_t global_font_set_fontSize(uint32_t fontSize)
{
	if(pg_Font){
		
		pg_Font->fontSize = fontSize;		
		return 0;
	}
	return -1;
}

int32_t global_font_destory()
{	
	if(pg_Font){
		free(pg_Font);
		pg_Font = NULL;
		return 0;
	}
	return -1;
}

int32_t putText(uint8_t *imgData, uint32_t imgWidth, uint32_t imgHeight, const char *text, uint32_t posX, uint32_t posY, FontColor color)
{	
	cv::Mat image;

	if((NULL == imgData) || (NULL == text)){
		printf("[error]: input data error, imgData or text is empty \n");
		return -1;
	}
	
	if(!pg_Font){
		printf("[error]: global font is not create \n");
		return -1;
	}
	
	if((0 == strlen(pg_Font->fontLib)) || (0 == strlen(pg_Font->textCodec))){
		printf("[error]: font engine is not init \n");
		return -1;
	}
	
	image = cv::Mat(imgHeight, imgWidth, CV_8UC3, imgData);
	
	CvxText cvxText(pg_Font->fontLib);	//根据字库创建字体
	
    cv::Scalar size1{ (double)pg_Font->fontSize, 0.5, 0.1, 0 }; // (字体大小, 无效的, 字符间距, 无效的 }
	float alpha = (float)color.Alpha/255;
    cvxText.setFont(nullptr, &size1, nullptr, &alpha);

    wchar_t *w_str;
    int ret = ToWchar(text, w_str, pg_Font->textCodec);
	if(w_str){
		cvxText.putText(image, w_str, cv::Point(posX, (posY+pg_Font->fontSize)), cv::Scalar(color.Bule, color.Green, color.Red));
		delete [] w_str; 
	}
	
	return 0;
}





