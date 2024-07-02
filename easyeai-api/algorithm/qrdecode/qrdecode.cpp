/**
 *
 * Copyright 2021 by Guangzhou Easy EAI Technologny Co.,Ltd.
 * website: www.easy-eai.com
 *
 * Author: JX <liaojunxian@easy-eai.com>
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * License file for more details.
 * 
 */

//===========================================system===========================================
#include <opencv2/opencv.hpp>
#include <iostream>
#include <stdio.h>


//======================================= qrcode =======================================
#include "qrdecode.h"
#include "zbar.h"

int qr_decode(cv::Mat src, struct qrcode_info *p_info)
{
    cv::Mat gray;
    if( src.channels() == 1 )
    {
        gray = src;
    }
    else 
    {
#if (CV_VERSION_MAJOR >= 4)
		cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
#else
		cv::cvtColor(src, gray, CV_RGB2GRAY);
#endif
    }
    int width   = gray.cols;
    int height  = gray.rows;
    

    // create a reader
    zbar::ImageScanner scanner;
    // configure the reader
    scanner.set_config(zbar::ZBAR_NONE, zbar::ZBAR_CFG_ENABLE, 1);
    unsigned char *pdata = (unsigned char *)gray.data;
    zbar::Image imageZbar(width, height, "Y800", pdata, width * height);
    int n = scanner.scan(imageZbar);
    if (n > 0)
    {
        
        // extract results
        for (zbar::Image::SymbolIterator symbol = imageZbar.symbol_begin();
            symbol != imageZbar.symbol_end();
            ++symbol) 
        {
            // do something useful with results
            std::string decodedFmt = symbol->get_type_name();
            std::string symbolData = symbol->get_data();
                
             
            int min_x = symbol->get_location_x(0);
            int min_y= symbol->get_location_y(0);
            int max_x= symbol->get_location_x(0);
            int max_y= symbol->get_location_y(0);

            for (int i = 0; i < symbol->get_location_size();i ++) 
            {
                min_x = min_x < symbol->get_location_x(i) ? min_x : symbol->get_location_x(i);
                max_x = max_x > symbol->get_location_x(i) ? max_x : symbol->get_location_x(i);

                min_y = min_y < symbol->get_location_y(i) ? min_y : symbol->get_location_y(i);
                max_y = max_y > symbol->get_location_y(i) ? max_y : symbol->get_location_y(i);
            }
            


            // 二维码坐标
            p_info->x1 = min_x;
            p_info->x2 = max_x;
            p_info->y1 = min_y;
            p_info->y2 = max_y;

            // 二维码类型
            memset(p_info->type, 0, sizeof(p_info->type));
            memcpy(p_info->type, decodedFmt.c_str(), strlen(decodedFmt.c_str()));

            // 二维码内容
            memset(p_info->result, 0, sizeof(p_info->result));
            memcpy(p_info->result, symbolData.c_str(), strlen(symbolData.c_str()));

            //printf("type result: %s\n", decodedFmt.c_str());
            //printf("result: %s\n", symbolData.c_str());

        }
        imageZbar.set_data(NULL, 0);

        return 0;
    }
    else
    {
        imageZbar.set_data(NULL, 0);

        return -1;
    }
}
