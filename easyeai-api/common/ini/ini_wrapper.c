/**
 *
 * Copyright 2021 by Guangzhou Easy EAI Technologny Co.,Ltd.
 * website: www.easy-eai.com
 *
 * Author: Jiehao.Zhong <zhongjiehao@easy-eai.com>
 * 
 * this interface repackaged by the third-party open source library (libini), libini version: 1.1.10
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * License file for more details.
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "libini.h"

//#include "ini_wrapper.h"

int32_t ini_read_int(const char *file, const char *pSection, const char *pKey)
{
    int32_t value = -1;
    
    //文件是否存在
    if (access(file, F_OK) == -1)
        return value;
    
    //打开文件
    ini_fd_t ini_file = ini_open(file, "r", ";");
    if (ini_file == 0){
        printf("ini_open %s error!,%s\n", file, strerror(errno));
        return value;
    }
    
    //读取配置信息
    ini_locateHeading(ini_file, pSection);
    ini_locateKey(ini_file, pKey);
    
    int res = ini_readInt(ini_file, &value);
    if(-1 == res){
        value = -1;
        printf("ini_readInt error!,%s,[%s]->%s\n",strerror(errno), pSection, pKey);
    }
    
    //关闭文件
    ini_close(ini_file);
    
    return value;
}

// 这个接口需要多线程使用时，最好也加下锁
const char *ini_read_string(const char *file, const char *pSection, const char *pKey)
{
    const char *retStr = NULL;
    static char strValue[2048] = {0};
    
    //文件是否存在
    if (access(file, F_OK) == -1)
        return retStr;
    
    //打开文件
    ini_fd_t ini_file = ini_open(file, "r", ";");
    if (ini_file == 0) {
        printf("ini_open %s error!,%s\n", file, strerror(errno));
        return retStr;
    }
    
    //读取配置信息
    ini_locateHeading(ini_file, pSection);
    ini_locateKey(ini_file, pKey);

    //这里多线程调用就不行了，得注意下加锁
    // ========================== lock ==========================
    bzero(strValue, sizeof(strValue));
    int res = ini_readString(ini_file, strValue, sizeof(strValue));
    if(-1 == res){
        printf("ini_readString error!,%s,[%s]->%s\n", strerror(errno), pSection, pKey);
    }else{
        retStr = strValue;
    }
    // ========================= unlock =========================
    
    //关闭文件
    ini_close(ini_file);
    
    return retStr;
}

int32_t ini_write_int(const char *file, const char *pSection, const char *pKey, int Val)
{
    int ret = -1;
#if 0    //可不要。注释掉就是没有文件则强制创建
    //文件是否存在
    if (access(file, F_OK) == -1)
        return ret;
#endif
    //打开文件
    ini_fd_t ini_file = ini_open(file, "wb", ";");
    if (ini_file == 0) {
        printf("ini_open error!,%s\n", strerror(errno));
        return ret;
    }
    
    //写入配置信息
    ini_locateHeading(ini_file, pSection);
    ini_locateKey(ini_file, pKey);
    
    int res = ini_writeInt(ini_file, Val);
    if(-1 == res){
        printf("ini_writeInt error!,%s,[%s]->%s\n", strerror(errno), pSection, pKey);
    }else{
        ret = 0;
    }
    
    //关闭文件
    ini_close(ini_file);
    
    return ret;
}


int32_t ini_write_string(const char *file, const char *pSection, const char *pKey, const char *pStr)
{
    int32_t ret = -1;
#if 1    //可不要。注释掉就是没有文件则强制创建
    //文件是否存在
    if (access(file, F_OK) == -1)
        return ret;
#endif
    //打开文件
    ini_fd_t ini_file = ini_open(file, "w", ";");
    if (ini_file == 0) {
        printf("ini_open error!\n");
        return ret;
    }
    
    //写入配置信息
    ini_locateHeading(ini_file, pSection);
    ini_locateKey(ini_file, pKey);
    
    int res = ini_writeString(ini_file, pStr);
    if(-1 == res) {
        printf("ini_writeString error!,%s,[%s]->%s\n", strerror(errno), pSection, pKey);
    }else{
        ret = 0;
    }
    
    //关闭文件
    ini_close(ini_file);
    
    return ret;
}

