#include <unistd.h>
#include <stdio.h>

#include <string.h>
#include "system_opt.h"

int main(void)
{
	printf("\n================= [system_time_parameter_api_demo] =====================\n");
	printf("current timeval  = %lu us\n", get_timeval_us());
	printf("current timeval  = %lu ms\n", get_timeval_ms());
	printf("current timeval  = %lu s\n", get_timeval_s());
	printf("current timeStamp = %d\n", get_time_stamp());
	uint32_t curDate = 0, curTime = 0;
	set_system_date_time(2021, 12, 03, 11, 07, 06);
	get_system_date_time(&curDate, &curTime);
	printf("[string] current date = %u, time = %u\n", curDate, curTime);
	
	uint32_t year, month, day;
	uint32_t hour, min, second;
	
	year  = curDate/10000;
	month = curDate%10000/100;
	day   = curDate%100;	
	hour   = curTime/10000;
	min    = curTime%10000/100;
	second = curTime%100;
	printf("[number] current date = %u-%02u-%02u, time = %02u:%02u:%02u\n", year, month, day, hour, min, second);
	
	switch(calc_week_day(year, month, day))
	{
        case 0: printf("[Today ] is Monday   \n"); break;
        case 1: printf("[Today ] is Tuesday  \n"); break;
        case 2: printf("[Today ] is Wednesday\n"); break;
        case 3: printf("[Today ] is Thursday \n"); break;
        case 4: printf("[Today ] is Friday   \n"); break;
        case 5: printf("[Today ] is Saturday \n"); break;
        case 6: printf("[Today ] is Sunday   \n"); break;
	}
	
	return 0;
}
