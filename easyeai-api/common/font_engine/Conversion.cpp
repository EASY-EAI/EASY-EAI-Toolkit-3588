#include <string>
#include <string.h>

#include "iconv.h"
#include "libcharset.h"
#include "localcharset.h"
#include "font_engine.h"

//编码转换，source_charset是源编码，to_charset是目标编码  
int32_t code_convert(const char *source_charset, const char *to_charset, const std::string& sourceStr, char *outData, int32_t outLen) //sourceStr是源编码字符串  
{  
    iconv_t cd = iconv_open(to_charset, source_charset);//获取转换句柄，void类型  
    if (cd == 0)  
        return -1;  
  
    char *inbuf = (char *)sourceStr.c_str();
    size_t inlen = sourceStr.size();
	char *pouData = outData;	// 多加这个指针是因为 iconv函数内部会改掉outbuf的值，如果传进来的是个堆地址，则会因无法释放原地址而导致内存泄漏
    size_t outlen = outLen;		// 同上，iconv函数也会改掉outbytesLeft的值。
	
    memset(pouData, 0, outlen);  
    if (iconv(cd, &inbuf, &inlen, &pouData, &outlen) == -1){
        iconv_close(cd);
        return -1;
    }

    iconv_close(cd);
    return outLen-outlen;
}  

//gbk转UTF-8    
int32_t gbk_to_utf8(const char *gbkStr, char *utf8Str, int32_t bufLen)// 传入的strGbk是GBK编码   
{
	if( (NULL == gbkStr) || (NULL == utf8Str) )
		return 0;
	
	std::string inStr = gbkStr;
	return code_convert("gb2312", "utf-8", inStr, utf8Str, bufLen);
}

//gbk转unicode
int32_t gbk_to_unicode(const char *gbkStr, char *unicodeStr, int32_t bufLen)// 传入的strGbk是GBK编码   
{
	if( (NULL == gbkStr) || (NULL == unicodeStr) )
		return 0;
	
	std::string inStr = gbkStr;
	return code_convert("gb2312", "UCS-2LE", inStr, unicodeStr, bufLen);
}


// 统计UTF-8字符串长度
static bool bIsUTF8Start(uint8_t c)
{	
	// 最高位为0
	if(0 == (c>>7)){
		return true;
	// 最高两位为 11(二进制)
	}else if(0xc0 == (c&0xc0)){
		return true;
	}else{
		return false;
	}
}
int32_t utf8_strlen(const char *utf8Str)
{
    int i = 0, j = 0;
    while (utf8Str[i]) {
        if (bIsUTF8Start(utf8Str[i])) {
            j++;
        } 
        i++;
    }
    return j;
}

//UTF-8转gbk
int32_t utf8_to_gbk(const char *utf8Str, char *gbkStr, int32_t bufLen)  
{  
	if( (NULL == utf8Str) || (NULL == gbkStr) )
		return 0;
	
	std::string inStr = utf8Str;
	return code_convert("utf-8", "gb2312", inStr, gbkStr, bufLen);
}

//UTF-8转unicode
int32_t utf8_to_unicode(const char *utf8Str, char *uniCodeStr, int32_t uniCodeLen)  
{  
	if( (NULL == utf8Str) || (NULL == uniCodeStr) )
		return 0;
	
	std::string inStr = utf8Str;
	return code_convert("utf-8", "UCS-2LE", inStr, uniCodeStr, uniCodeLen);
}

