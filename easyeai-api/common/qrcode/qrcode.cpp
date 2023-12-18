 /**
 *
 * Copyright 2021 by Guangzhou Easy EAI Technologny Co.,Ltd.
 * website: www.easy-eai.com
 *
 * Author: Jiehao.Zhong <zhongjiehao@easy-eai.com>
 *
 * Based on libqrencode C library distributed under LGPL 2.1
 * Copyright (C) 2006, 2007, 2008, 2009 Kentaro Fukuchi <fukuchi@megaui.net>
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * License file for more details.
 * 
 */
 
 
#include <opencv2/opencv.hpp>

#include "qrencode.h"
#include "qrcode.h"

using namespace cv;

long StrToQRCode(const char *file, const char *pStr)
{
    // 使用qrencode进行字符串编码
    QRcode * code = QRcode_encodeString(pStr, 0, QR_ECLEVEL_H, QR_MODE_8, 1);
    if (code == NULL) {
        return -1;
    }
	
    cv::Mat img = cv::Mat(code->width, code->width, CV_8U);

    for (int i = 0; i < code->width; ++i) {
        for (int j = 0; j < code->width; ++j)
        {
            img.at<uchar>(i, j) = (code->data[i*code->width + j] & 0x01) == 0x01 ? 0 : 255;
        }
    }
    cv::resize(img, img, cv::Size(img.rows * 10, img.cols * 10),0,0, cv::INTER_NEAREST);

    cv::Mat result = cv::Mat::zeros(img.rows + 30, img.cols + 30, CV_8U);
    //白底
    result = 255 - result;
    //转换成彩色
    cv::cvtColor(result, result, cv::COLOR_GRAY2BGR);
    cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);
    //建立roi
    cv::Rect roi_rect = cv::Rect((result.rows - img.rows) / 2, (result.cols - img.rows) / 2, img.cols, img.rows);
    //roi关联到目标图像，并把源图像复制到指定roi
    img.copyTo(result(roi_rect));

    cv::imwrite(file, result);
	
    return 0;
}