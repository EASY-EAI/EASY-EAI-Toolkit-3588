
#include <opencv2/opencv.hpp>
#include <stdio.h>
#include <sys/time.h>
#include"person_detect.h"

using namespace cv;
using namespace std;

static Scalar colorArray[10]={
    Scalar(255, 0, 0, 255),
    Scalar(0, 255, 0, 255),
    Scalar(0,0,139,255),
    Scalar(0,100,0,255),
    Scalar(139,139,0,255),
    Scalar(209,206,0,255),
    Scalar(0,127,255,255),
    Scalar(139,61,72,255),
    Scalar(0,255,0,255),
    Scalar(255,0,0,255),
};

int plot_one_box(Mat src, int x1, int x2, int y1, int y2, char *label, char colour)
{
    int tl = round(0.002 * (src.rows + src.cols) / 2) + 1;
    rectangle(src, cv::Point(x1, y1), cv::Point(x2, y2), colorArray[(unsigned char)colour], 3);

    int tf = max(tl -1, 1);

    int base_line = 0;
    cv::Size t_size = getTextSize(label, FONT_HERSHEY_SIMPLEX, (float)tl/3, tf, &base_line);
    int x3 = x1 + t_size.width;
    int y3 = y1 - t_size.height - 3;

    rectangle(src, cv::Point(x1, y1), cv::Point(x3, y3), colorArray[(unsigned char)colour], -1);
    putText(src, label, cv::Point(x1, y1 - 2), FONT_HERSHEY_SIMPLEX, (float)tl/3, cv::Scalar(255, 255, 255, 255), tf, 8);
    return 0;
}

int main(int argc, char **argv)
{
	if (argc != 3)
    {
        printf("%s <model_path> <image_path>\n", argv[0]);
        return -1;
    }

    const char *model_path = argv[1];
    const char *image_path = argv[2];

	/* 参数初始化 */
	detect_result_group_t detect_result_group;

	/* 算法模型初始化 */
	rknn_context ctx;
	person_detect_init(&ctx, model_path);

	/* 算法运行 */
	cv::Mat src;
	src = cv::imread(image_path, 1);

	struct timeval start;
	struct timeval end;
	float time_use=0;

	gettimeofday(&start,NULL); 

	person_detect_run(ctx, src, &detect_result_group);

	gettimeofday(&end,NULL);
	time_use=(end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);//微秒
	printf("time_use is %f\n",time_use/1000);

	/* 算法结果在图像中画出并保存 */
	// Draw Objects
	char text[256];
	for (int i = 0; i < detect_result_group.count; i++) 
	{

		detect_result_t* det_result = &(detect_result_group.results[i]);
		if( det_result->prop < 0.4)
		{
			continue;
		}

		sprintf(text, "%s %.1f%%", det_result->name, det_result->prop * 100);
		printf("%s @ (%d %d %d %d) %f\n", det_result->name, det_result->box.left, det_result->box.top,
			   det_result->box.right, det_result->box.bottom, det_result->prop);
		int x1 = det_result->box.left;
		int y1 = det_result->box.top;
		int x2 = det_result->box.right;
		int y2 = det_result->box.bottom;
		/*
		rectangle(src, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(255, 0, 0, 255), 3);
		putText(src, text, cv::Point(x1, y1 + 12), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
		*/
		plot_one_box(src, x1, x2, y1, y2, text, i%10);
	} 

	cv::imwrite("result.jpg", src);	


	/* 算法模型空间释放 */
	person_detect_release(ctx);

	return 0;
}

