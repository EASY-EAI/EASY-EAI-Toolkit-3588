
#include <opencv2/opencv.hpp>
#include <stdio.h>
#include <sys/time.h>
#include "face_detect.h"

using namespace cv;


int main(int argc, char **argv)
{
	if( argc != 2)
	{
		printf("./test-face-detect xxx\n");
		return -1;
	}

	struct timeval start;
	struct timeval end;
	float time_use=0;

	rknn_context ctx;
	std::vector<det> result;

	Mat image;
	image = cv::imread(argv[1], 1);	

	face_detect_init(&ctx, "face_detect.model");

	gettimeofday(&start,NULL); 
	face_detect_run(ctx, image, result);

	gettimeofday(&end,NULL);
	time_use=(end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);//微秒
	printf("time_use is %f\n",time_use/1000);

	printf("face num:%d\n", (int)result.size());

	for (int i = 0; i < (int)result.size(); i++)
	{
		int x = (int)(result[i].box.x);
		int y = (int)(result[i].box.y);
		int w = (int)(result[i].box.width);
		int h = (int)(result[i].box.height);
		rectangle(image, Rect(x, y, w, h), Scalar(0, 255, 0), 2, 8, 0);

		
		for (int j = 0; j < (int)result[i].landmarks.size(); ++j) 
		{
			cv::circle(image, cv::Point((int)result[i].landmarks[j].x, (int)result[i].landmarks[j].y), 2, cv::Scalar(225, 0, 225), 2, 8);
		}
	}

	imwrite("result.jpg", image);

	face_detect_release(ctx);

	return 0;
}

