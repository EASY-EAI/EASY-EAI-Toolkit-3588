
#include <opencv2/opencv.hpp>
#include <stdio.h>
#include <sys/time.h>
#include "qrdecode.h"

using namespace cv;


int main(int argc, char **argv)
{
	if( argc != 2)
	{
		printf("./test-qrdecode xxx\n");
		return -1;
	}

	struct timeval start;
	struct timeval end;
	float time_use=0;

	Mat image;
	image = cv::imread(argv[1], 1);	


	gettimeofday(&start,NULL); 

	struct qrcode_info info;
	qr_decode(image, &info);

	gettimeofday(&end,NULL);
	time_use=(end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);//微秒
	printf("time_use is %f\n",time_use/1000);

	printf("x1:%d\n", info.x1);
	printf("x2:%d\n", info.x2);
	printf("y1:%d\n", info.y1);
	printf("y2:%d\n", info.y2);
	printf("type:%s\n", info.type);
	printf("result:%s\n", info.result);

	rectangle(image, Point(info.x1, info.y1), Point(info.x2, info.y2), Scalar(0, 255, 0), 3);

	imwrite("result.jpg", image);

    return 0;
}

