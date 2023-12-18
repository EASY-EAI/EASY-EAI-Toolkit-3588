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

//===========================================system===========================================
#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>

#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include <termios.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <netdb.h>
#include <iconv.h>
#include <fcntl.h>
#include <dirent.h>
#include <dirent.h>
#include <semaphore.h>

//=========================================== C++ ===========================================
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

//======================================= system_opt =======================================
#include "system_opt.h"

using namespace std;

/***********************************************************
    系统中有些地方需要对指针进行赋值，但在赋值之前需要
    判断指针是否为空，这里统一定义一个宏定义
************************************************************/
#define TRY_EVALUATE_POINTER(pointer, value) do{\
    if(pointer)\
    {\
        *(pointer) = value;\
    }\
}while(0)


/*********************************************************************
Function:
Description:
	查看cpu的实时温度
Example:
	double cpuTemp = cpu_tempture();
parameter:
    无
Return:
	cpu的实时温度，单位：摄氏度
********************************************************************/
double cpu_tempture()
{
    FILE *fd = NULL;
    int temp;
    char buff[256];

    fd = fopen("/sys/class/thermal/thermal_zone0/temp","r");
    if(fd){
        fgets(buff,sizeof(buff),fd);
        sscanf(buff, "%d", &temp);

        fclose(fd);
        
        return (double)temp/1000.0;
    }else{
        return 0.0;
    }
}

/*********************************************************************
Function:
Description:
	查看npu的实时温度
Example:
	double npuTemp = npu_tempture();
parameter:
    无
Return:
	npu的实时温度，单位：摄氏度
********************************************************************/
double npu_tempture()
{
    FILE *fd = NULL;
    int temp;
    char buff[256];

    fd = fopen("/sys/class/thermal/thermal_zone1/temp","r");
    if(fd){
        fgets(buff,sizeof(buff),fd);
        sscanf(buff, "%d", &temp);

        fclose(fd);
        
        return (double)temp/1000.0;
    }else{
        return 0.0;
    }
}

/*********************************************************************
Function:
Description:
	获取CPU状态到cpu_occupy_t结构体
Example:
	get_cpu_occupy((cpu_occupy_t *)&cpu_stat1);
parameter:
    cpust: 用于存放cpu状态的结构体
Return:
	无
********************************************************************/
void get_cpu_occupy(cpu_occupy_t *cpust)
{
    FILE *fd;
    char buff[256];
    cpu_occupy_t *cpu_occupy;

    cpu_occupy=cpust;
    fd = fopen ("/proc/stat", "r");

    if(fd == NULL){
        perror("fopen:");
        exit (0);
    }

    fgets (buff, sizeof(buff), fd);
    sscanf (buff, "%s %u %u %u %u %u %u %u", 
                        cpu_occupy->name,
                        &cpu_occupy->user,
                        &cpu_occupy->nice,
                        &cpu_occupy->system,
                        &cpu_occupy->idle,
                        &cpu_occupy->iowait,
                        &cpu_occupy->irq,
                        &cpu_occupy->softirq);
    fclose(fd);
}

/*********************************************************************
Function:
Description:
	通过新旧2次的CPU状态计算出CPU占用率
Example:
	double cpu_usage()
    {
        static bool bIsFirstTimeGet = true;
        static cpu_occupy_t cpu_stat1;
        cpu_occupy_t cpu_stat2;

        double cpu;

        if(bIsFirstTimeGet){
            bIsFirstTimeGet = false;
            
            get_cpu_occupy((cpu_occupy_t *)&cpu_stat1);
            msleep(500);
            get_cpu_occupy((cpu_occupy_t *)&cpu_stat2);
        }else{
            get_cpu_occupy((cpu_occupy_t *)&cpu_stat2);
        }
        //计算cpu使用率
        cpu = cal_cpu_occupy ((cpu_occupy_t *)&cpu_stat1, (cpu_occupy_t *)&cpu_stat2);
        memcpy(&cpu_stat1, &cpu_stat2, sizeof(cpu_occupy_t));

        return cpu;
    }
parameter:
    o: 上次获取的cpu状态
    n: 新获取的cpu状态
Return:
	cpu的占用率
********************************************************************/
double cal_cpu_occupy(cpu_occupy_t *o, cpu_occupy_t *n)
{
    double od, nd;
    double id, sd;
    double cpu_use;

    od = (double) (o->user + o->nice + o->system + o->idle + o->softirq + o->iowait + o->irq);//第一次(用户+优先级+系统+空闲)的时间再赋给od
    nd = (double) (n->user + n->nice + n->system + n->idle + n->softirq + n->iowait + n->irq);//第二次(用户+优先级+系统+空闲)的时间再赋给od
    id = (double) (n->idle); //用户第一次和第二次的时间之差再赋给id
    sd = (double) (o->idle) ; //系统第一次和第二次的时间之差再赋给sd
    
    if((nd-od) != 0)
        cpu_use =100.0 - ((id-sd))/(nd-od)*100.00; //((用户+系统)乘100)除(第一次和第二次的时间差)再赋给g_cpu_used
    else
        cpu_use = 0;

    return cpu_use;
}

/*********************************************************************
Function:
Description:
	查看可用内存的已使用空间占用率
Example:
	double memUsage = memory_usage();
parameter:
    无
Return:
	可用内存的已使用空间占用率
********************************************************************/
double memory_usage()
{
#if 0
    double totalRam = 0.0;
    double freeRam = 0.0;
    double usedRam = 0.0;
    struct sysinfo sysInfo;
    if(sysinfo(&sysInfo) == 0)
    {
        totalRam = (double)sysInfo.totalram;
        freeRam  = (double)(sysInfo.freeram + sysInfo.bufferram);
        usedRam = totalRam - freeRam;
        printf("total:[uint](%ld)--[double](%f)\n", sysInfo.totalram, totalRam);
        printf("free:[uint](%ld)--[double](%f)\n", sysInfo.freeram + sysInfo.bufferram, freeRam);
        printf("buff:[uint](%ld)--[double](  )\n", sysInfo.bufferram);
        printf("used:[uint](   )--[double](%f)\n", usedRam);
        printf (100*(double)usedRam/(double)totalRam);
    }
    return 100.0;
#else
    char strMem[64];
    uint64_t totalRam = 0;
    uint64_t usedRam = 0;

    memset(strMem, 0, sizeof(strMem));
    exec_cmd_by_popen("free -b | grep Mem | awk '{print $2}'", strMem);
    strMem[strlen(strMem)-1] = 0;
    totalRam = atoi(strMem);

    memset(strMem, 0, sizeof(strMem));
    exec_cmd_by_popen("free -b | grep Mem | awk '{print $3}'", strMem);
    strMem[strlen(strMem)-1] = 0;
    usedRam = atoi(strMem);

    //printf("total : %llu, used : %llu\n", totalRam, usedRam);

    return 100.0 * (double)usedRam/(double)totalRam;
#endif
}

/*********************************************************************
Function:
Description:
	获取挂载点(分区)占用率
Example:
	double diskUsage = partition_usage("/userdata");
parameter:
    path: 需要查询的挂载点(分区)在文件系统的目录
Return:
	挂载点(分区)已被使用的空间占用率
********************************************************************/
double partition_usage(const char *path)
{
    /*
    struct statfs 
    { 
       long    f_type;     // 文件系统类型  
       long    f_bsize;    // 经过优化的传输块大小  
       long    f_blocks;   // 文件系统数据块总数 
       long    f_bfree;    // 可用块数
       long    f_bavail;   // 非超级用户可获取的块数 
       long    f_files;    // 文件结点总数
       long    f_ffree;    // 可用文件结点数
       fsid_t  f_fsid;     // 文件系统标识
       long    f_namelen;  // 文件名的最大长度
    };
    */
    struct statfs s;
    memset(&s, 0, sizeof(struct statfs));
    
    if(0 == statfs(path, &s)){

        double percentage = (s.f_blocks - s.f_bfree) * 100 /(s.f_blocks - s.f_bfree + s.f_bavail) + 1;
#if 0
        int64_t bsize = s.f_bsize;                // in bytes
        int64_t totalSize = (bsize * s.f_blocks);      // in bytes
        int64_t freeSize = (bsize * s.f_bfree);          // in bytes
        int64_t availSize = (bsize * s.f_bavail);         // in bytes
#endif
        return percentage;
    }else{
        return 100.0;
    }
}

/*********************************************************************
Function:
Description:
	获取系统时间戳，通常用于性能测试
Example:
	uint64_t timeval_bf = get_timeval_us();
parameter:
    无
Return:
	系统时间戳(UTC时间，没有进行时区偏移)，单位：微秒
********************************************************************/
uint64_t get_timeval_us()
{
    struct timeval tv;
	gettimeofday(&tv, NULL);	// UTC时间
	
	return ((uint64_t)tv.tv_sec * 1000000 + tv.tv_usec);
}

/*********************************************************************
Function:
Description:
	获取系统时间戳，通常用于性能测试
Example:
	uint64_t timeval_bf = get_timeval_ms();
parameter:
    无
Return:
	系统时间戳(UTC时间，没有进行时区偏移)，单位：毫秒
********************************************************************/
uint64_t get_timeval_ms()
{
    struct timeval tv;
	gettimeofday(&tv, NULL);	// UTC时间
	return ((uint64_t)tv.tv_sec * 1000 + tv.tv_usec/1000);
}

/*********************************************************************
Function:
Description:
	获取系统时间戳，通常用于性能测试
Example:
	uint64_t timeval_bf = get_timeval_s();
parameter:
    无
Return:
	系统时间戳(UTC时间，没有进行时区偏移)，单位：秒
********************************************************************/
uint64_t get_timeval_s()
{
    struct timeval tv;
	gettimeofday(&tv, NULL);	// UTC时间
	
	return (uint64_t)tv.tv_sec;
}

/*********************************************************************
Function:
Description:
	毫秒级延时
Example:
	osTask_usDelay(10);
parameter:
    s:延时时间-单位：微秒
Return:
	无
说明:
	- nanosleep一旦被调用，进程就进入 TASK_INTERRUPTIBLE 状态，直到进程被唤醒，就回到 TASK_RUNNIN 状态。
	- TASK_INTERRUPTIBLE 和 TASK_UNINTERRUPTIBLE 的区别：
		TASK_INTERRUPTIBLE 是可以被 [信号] 和 [wake_up()] 唤醒的，当信号到来时，进程会被设置为可运行。
		TASK_UNINTERRUPTIBLE 只能被 [wake_up()] 唤醒。
********************************************************************/
uint32_t osTask_usDelay(uint32_t us)
{
	uint32_t elaTime;
	struct timespec delayTime, elaspedTime;

	delayTime.tv_sec  = us / 1000000;
	delayTime.tv_nsec = (us % 1000000) * 1000;

	nanosleep(&delayTime, &elaspedTime);

	elaTime = (elaspedTime.tv_sec*1000000 + elaspedTime.tv_nsec/1000);
	return elaTime;
}

uint32_t osTask_msDelay(uint32_t ms)
{
	uint32_t elaTime;
	struct timespec delayTime, elaspedTime;

	delayTime.tv_sec  = ms / 1000;
	delayTime.tv_nsec = (ms % 1000) * 1000000;

	nanosleep(&delayTime, &elaspedTime);

	elaTime = (elaspedTime.tv_sec*1000 + elaspedTime.tv_nsec/1000000);
	return elaTime;
}

uint32_t osTask_sDelay(uint32_t s)
{
	uint32_t elaTime;
	struct timespec delayTime, elaspedTime;

	delayTime.tv_sec  = s;
	delayTime.tv_nsec = 0;

	nanosleep(&delayTime, &elaspedTime);

	elaTime = elaspedTime.tv_sec;
	return elaTime;
}

/*********************************************************************
Function:
Description:
	毫秒级延时
Example:
	msleep(10);
parameter:
    ms:延时时间-单位：毫秒
Return:
	若进程/线程被中断，则返回剩余时间(ms)
********************************************************************/
uint32_t msleep (uint32_t ms)
{
	uint32_t ret = usleep(ms*1000);
	if(ret)
		return (ret/1000);
	else
		return 0;
}

/*********************************************************************
Function:
Description:
	获取系统时间戳
Example:
	int timeStamp = get_time_stamp();
parameter:
	无
Return:
	系统当前时间戳(UTC时间，没有进行时区偏移)
********************************************************************/
int32_t get_time_stamp()
{
	time_t t;
	t = time(NULL);	// UTC时间
 
	return time(&t);
}

/*********************************************************************
Function:
Description:
	把日期和时间按:年月日时分秒排列到参数中
Example:
	uint32_t curDate, curTime
	get_system_date_time(&curDate, &curTime);
parameter:
    *curDate:当前日期
    *curTime:当前时间(已做时区偏移)
Return:
	无
********************************************************************/
void get_system_date_time(uint32_t *curDate, uint32_t *curTime)
{
    time_t timer;//time_t就是long int 类型
    struct tm *tblock;

    timer = time(NULL);	// UTC时间
    tblock = localtime(&timer); //获取本时区时间

	*curDate = 10000 * (tblock->tm_year+1900) + 100 * (tblock->tm_mon + 1) + (tblock->tm_mday);
 	*curTime = 10000 * (tblock->tm_hour) + 100 * (tblock->tm_min) + (tblock->tm_sec);
}

/*********************************************************************
Function:
Description:
	设置系统时间，注意自动校时
Example:
	set_system_date_time(2021, 12, 22, 11, 14, 59);
parameter:
    year：年
     mon：月
     day：日
    hour：时(当前时区时间，无须额外进行时区偏移)
     min：分
    second：秒
Return:
	无
********************************************************************/
void set_system_date_time(int year, int mon, int day, int hour, int min, int second)
{
	time_t tStamp;
    struct timeval tv;

    struct tm t;
	
    year -= 1900;
    t.tm_year = year;
    t.tm_mon = mon-1;
    t.tm_mday = day;

    t.tm_hour = hour;	// 填入本时区时间
    t.tm_min = min;
    t.tm_sec = second;

	tStamp = mktime(&t); // mktime返回值为UTC时间
    if(-1 == tStamp){
        perror("mktime");
    }else{
		tv.tv_sec = tStamp;
		tv.tv_usec = 0;
		settimeofday(&tv, NULL);	// UTC时间
	}
}

/*********************************************************************
Function:
Description:
	计算当天是星期几
Example:
	calc_week_day(2022, 3, 18);
parameter:
	y:年
	m:月
	d:日
Return:
	星期几(0-星期一；6-星期日)
********************************************************************/
uint8_t calc_week_day(int y,int m, int d)
{
    uint8_t iWeek;
    if(m==1||m==2) {
        m+=12;
        y--;
    }
    iWeek=(d+2*m+3*(m+1)/5+y+y/4-y/100+y/400)%7;
#ifdef __TEST_SOFT__
    switch(iWeek)
    {
        case 0: printf("星期一\n"); break;
        case 1: printf("星期二\n"); break;
        case 2: printf("星期三\n"); break;
        case 3: printf("星期四\n"); break;
        case 4: printf("星期五\n"); break;
        case 5: printf("星期六\n"); break;
        case 6: printf("星期日\n"); break;
    }
#endif
    return iWeek;
}


/*********************************************************************
Function:
Description:
	以分离的模式创建一个线程,如果新线程与主线程共享变量，该线程必 须要确保
	它在运行期间， 线程体中使用的变量没有被主线程释放。否则会出问题。
Example:
    void *xxxThreadBody(void *arg)
	{
		pthread_exit(NULL);
	}
	
	pthread_t pId;
	int share_para;
	CreateNormalThread(xxxThreadBody, &share_para, &pId);
parameter:
    entry : 线程体执行函数
    *para : 传入线程体的参数，用作共享变量
    *pid  : 传入的pid为NULL，会直接退出整个进程
Return:
	 0：创建成功
	-1：创建失败
********************************************************************/
int32_t CreateNormalThread(ThreadEntryPtrType entry, void *para, pthread_t *pid)
{
    pthread_t ThreadId;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);//绑定
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);//分离
    if(pthread_create(&ThreadId, &attr, entry, para) == 0)//创建线程
    {
        pthread_attr_destroy(&attr);
        TRY_EVALUATE_POINTER(pid, ThreadId);

        return 0;
    }

    pthread_attr_destroy(&attr);

    return -1;
}

int32_t CreateJoinThread(ThreadEntryPtrType entry, void *para, pthread_t *pid)
{
    pthread_t ThreadId;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if(pthread_create(&ThreadId, &attr, entry, para) == 0)
    {
        pthread_attr_destroy(&attr);
        TRY_EVALUATE_POINTER(pid, ThreadId);

        return 0;
    }

    pthread_attr_destroy(&attr);

    return -1;
}

int32_t WaitExitThread(pthread_t pid)
{
    if (pthread_join(pid, NULL) == 0){
        return 0;
    }
    return -1;
}

/*********************************************************************
Function:
Description:
	执行shell命令
Example:
	SYSTEM("ls");
parameter:
	*cmdstring: shell命令语句
Return:
	对于fork失败，system()函数返回-1。
	如果exec执行成功，也即command顺利执行完毕，则返回command通过exit或return返回的值。
	(注意，command顺利执行不代表执行成功，比如command："rm debuglog.txt"，不管文件存不存在该command都顺利执行了)
	如果exec执行失败，也即command没有顺利执行，比如被信号中断，或者command命令根本不存在，system()函数返回127.
注意：
	* 无论是system()还是SYSTEM()，在调用时，信号的处理方式不为SIG_DFL，则父进程无法通过waitpid()函数对子进程进行收尸。这样会产生僵尸进程。
	* 如果你希望使用 wait () 或 waitpid () 对子进程收尸，那么你必须在调用前(事实上是fork ()前)将SIGCHLD信号置为SIG_DFL处理方式。
	  调用后(事实上wait()/waitpid()后)再将信号处理方式设置为从前的值。
调用建议：
	1、建议system()函数只用来执行shell命令，因为一般来讲，system()/SYSTEM()返回值不是0就说明出错了；
	2、建议监控一下system()/SYSTEM()函数的执行完毕后的errno值，争取出错时给出更多有用信息；
	3、如果waitpid()函数是被信号中断而返回负数的，则继续调用waitpid()函数。这个包括SIGINT，不违反POSIX.1定义；
	4、system()/SYSTEM()非阻塞方式注意点：’&’转后台，同时将输出重定向。否则变为阻塞方式；
	5、建议考虑一下system()/SYSTEM()函数的替代函数popen ()；
********************************************************************/
static int32_t SYSTEM(const char *cmdstring)
{
    pid_t pid;
    int status;

    if(cmdstring == NULL)
    {
        return (1);
    }

	// 此函数与系统的system()差异是:[系统用fork()，此函数用vfork()]
	// fork()  -子进程拷贝父进程的数据段和代码段，这里通过拷贝页表实现。
	//         -父子进程的执行次序不确定。
	// vfork() -子进程与父进程共享地址空间，无需拷贝页表，效率更高。
	//         -保证子进程先运行，在调用 exec 或 exit 之前与父进程数据是共享的。父进程在子进程调用 exec 或 exit 之后才可能被调度运行，如果在调用这两个函数之前子进程依赖于父进程的进一步动作，则会导致死锁。
    if((pid = vfork())<0)
    {
        status = -1;
    }
    else if(pid == 0)	//子进程中运行脚本命令
    {
        execl("/bin/sh", "sh", "-c", cmdstring, (char *)0);
        exit(127); //子进程正常执行则不会执行此语句
    }
    else		//父进程中等待子进程返回
    {
        while(waitpid(pid, &status, 0) < 0)
        {
            if(errno != EINTR)
            {
                status = -1;
                break;
            }
        }
    }
    return status;
}

/*********************************************************************
Function:
Description:
	执行shell命令
Example:
	exec_cmd_by_system("ls");
parameter:
	*cmd: shell命令语句
Return:
	同SYSTEM(const char *cmdstring);
注意：
	如果 SIGCHLD 信号行为被设置为 SIG_IGN 时，waitpid () 函数有可能因为找不到子进程而报 ECHILD 错误。
	解析：
    * systeme() 函数依赖了系统的一个特性，那就是内核初始化进程时对 SIGCHLD 信号的处理方式为 SIG_DFL。
	
	-[信号的处理方式为 SIG_DFL]是什么什么意思呢？
	* 即内核发现进程的子进程终止后给进程发送一个 SIGCHLD 信号，进程收到该信号后采用 SIG_DFL 方式处理。
	
	-那么 SIG_DFL 又是什么方式呢？
	* SIG_DFL 是一个宏，定义了一个信号处理函数指针，事实上该信号处理函数什么也没做。这个特性正是 system () 函数需要的。
	* system () 函数首先 fork () 一个子进程执行 command 命令，执行完后 system () 函数会使用 waitpid () 函数对子进程进行收尸。	
********************************************************************/
int32_t exec_cmd_by_system(const char *cmd)
{
	int32_t ret = 0;
	sighandler_t old_handler;

	old_handler = signal(SIGCHLD, SIG_DFL);	//防止产生僵尸进程
	ret = SYSTEM(cmd);
	signal(SIGCHLD, old_handler);

	return ret;
}

/*********************************************************************
Function:
Description:
	执行shell命令
Example:
	char result[1024]={0};
	exec_cmd_by_popen("ls", result);
parameter:
	*cmd: shell命令语句
	*result: 执行shell命令语句后，返回的结果将存进该段内存中
Return:
	无
注意：
	- 如果 cmd 执行失败，子进程会把错误信息打印到标准错误输出，父进程就无法获取。
	  若需要捕获错误信息，可以重定向子进程的错误输出，让错误输出重定向到标准输出（2>&1），这样父进程就可以捕获子进程的错误信息了。
	  如：exec_cmd_by_popen("ls 2>&1", result);
********************************************************************/
int32_t exec_cmd_by_popen(const char *cmd, char *result)
{
    char buf_ps[1024];
    char ps[1024]={0};
    FILE *ptr;
    strcpy(ps, cmd);
    if((ptr=popen(ps, "r"))!=NULL){
        while(fgets(buf_ps, 1024, ptr)!=NULL)
        {
//	       可以通过这行来获取shell命令行中的每一行的输出
//	   	   printf("%s", buf_ps);
           strcat(result, buf_ps);
           if(strlen(result)>1024)
               break;
        }
        pclose(ptr);
        ptr = NULL;
		return 0;
    } else {
        printf("popen %s error\n", ps);
		return -1;
    }
}

