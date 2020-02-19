/* ========================================================================== */
/*                                                                            */
/*   seqgen2x.c                                                               */
// Sam Siewert, December 2017
//
// Sequencer Generic @ 2x Rate for 10Hz Capture
//
// The purpose of this code is to provide an example for how to best
// sequence a set of periodic services for problems similar to and including
// the final project in real-time systems.
//
// For example: Service_1 for camera frame aquisition
//              Service_2 for image analysis and timestamping
//              Service_3 for image processing (difference images)
//              Service_4 for save time-stamped image to file service
//              Service_5 for save processed image to file service
//              Service_6 for send image to remote server to save copy
//              Service_7 for elapsed time in syslog each minute for debug
//
// At least two of the services need to be real-time and need to run on a single
// core or run without affinity on the SMP cores available to the Linux 
// scheduler as a group.  All services can be real-time, but you could choose
// to make just the first 2 real-time and the others best effort.
//
// For the standard project, to time-stamp images at the 1 Hz rate with unique
// clock images (unique second hand / seconds) per image, you might use the 
// following rates for each service:
//
// Sequencer - 60 Hz 
//                   [gives semaphores to all other services]
// Service_1 - 30 Hz, every other Sequencer loop
//                   [buffers 3 images per second]
// Service_2 - 10 Hz, every 6th Sequencer loop 
//                   [time-stamp middle sample image with cvPutText or header]
// Service_3 - 5 Hz , every 12th Sequencer loop
//                   [difference current and previous time stamped images]
// Service_4 - 10 Hz, every 6th Sequencer loop
//                   [save time stamped image with cvSaveImage or write()]
// Service_5 - 5 Hz , every 12th Sequencer loop
//                   [save difference image with cvSaveImage or write()]
// Service_6 - 10 Hz, every 6th Sequencer loop
//                   [write current time-stamped image to TCP socket server]
// Service_7 - 1 Hz , every 60th Sequencer loop
//                   [syslog the time for debug]
//
// With the above, priorities by RM policy would be:
//
// Sequencer = RT_MAX	@ 60 Hz
// Servcie_1 = RT_MAX-1	@ 30 Hz
// Service_2 = RT_MAX-2	@ 10 Hz
// Service_3 = RT_MAX-3	@ 5  Hz
// Service_4 = RT_MAX-2	@ 10 Hz
// Service_5 = RT_MAX-3	@ 5  Hz
// Service_6 = RT_MAX-2	@ 10 Hz
// Service_7 = RT_MIN	@ 1  Hz
//
// Here are a few hardware/platform configuration settings on your Jetson
// that you should also check before running this code:
//
// 1) Check to ensure all your CPU cores on in an online state.
//
// 2) Check /sys/devices/system/cpu or do lscpu.
//
//    Tegra is normally configured to hot-plug CPU cores, so to make all
//    available, as root do:
//
//    echo 0 > /sys/devices/system/cpu/cpuquiet/tegra_cpuquiet/enable
//    echo 1 > /sys/devices/system/cpu/cpu1/online
//    echo 1 > /sys/devices/system/cpu/cpu2/online
//    echo 1 > /sys/devices/system/cpu/cpu3/online
//
// 3) Check for precision time resolution and support with cat /proc/timer_list
//
// 4) Ideally all printf calls should be eliminated as they can interfere with
//    timing.  They should be replaced with an in-memory event logger or at
//    least calls to syslog.
//
// 5) For simplicity, you can just allow Linux to dynamically load balance
//    threads to CPU cores (not set affinity) and as long as you have more
//    threads than you have cores, this is still an over-subscribed system
//    where RM policy is required over the set of cores.

// This is necessary for CPU affinity macros in Linux
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <semaphore.h>

#include <syslog.h>
#include <sys/time.h>

#include <errno.h>

#define NSEC_PER_SEC (1000000000)
#define NSEC_PER_MSEC (1000000)
#define NSEC_PER_MICROSEC (1000)

#define USEC_PER_MSEC (1000)
#define NANOSEC_PER_SEC (1000000000)
#define NUM_CPU_CORES (1)
#define TRUE (1)
#define FALSE (0)

#define NUM_THREADS (7+1)

int abortTest=FALSE;
int abortS1=FALSE, abortS2=FALSE, abortS3=FALSE, abortS4=FALSE, abortS5=FALSE, abortS6=FALSE, abortS7=FALSE;
sem_t semS1, semS2, semS3, semS4, semS5, semS6, semS7;
struct timeval start_time_val;

typedef struct
{
    int threadIdx;
    unsigned long long sequencePeriods;
} threadParams_t;


void *Sequencer(void *threadp);

void *Service_1(void *threadp);
void *Service_2(void *threadp);
void *Service_3(void *threadp);
void *Service_4(void *threadp);
void *Service_5(void *threadp);
void *Service_6(void *threadp);
void *Service_7(void *threadp);
double getTimeMsec(void);
void print_scheduler(void);

struct timespec start_time = {0, 0};
struct timespec finish_time0 = {0, 0};
struct timespec thread_dt0 = {0, 0};

struct timespec temp = {0, 0};

struct timespec wcet1 = {0, 0};
struct timespec wcet2 = {0, 0};
struct timespec wcet3 = {0, 0};
struct timespec wcet4 = {0, 0};
struct timespec wcet5 = {0, 0};
struct timespec wcet6 = {0, 0};
struct timespec wcet7 = {0, 0};

int delta_t(struct timespec *stop, struct timespec *start, struct timespec *delta_t)
{
  int dt_sec=stop->tv_sec - start->tv_sec;
  int dt_nsec=stop->tv_nsec - start->tv_nsec;

  if(dt_sec >= 0)
  {
    if(dt_nsec >= 0)
    {
      delta_t->tv_sec=dt_sec;
      delta_t->tv_nsec=dt_nsec;
    }
    else
    {
      delta_t->tv_sec=dt_sec-1;
      delta_t->tv_nsec=NSEC_PER_SEC+dt_nsec;
    }
  }
  else
  {
    if(dt_nsec >= 0)
    {
      delta_t->tv_sec=dt_sec;
      delta_t->tv_nsec=dt_nsec;
    }
    else
    {
      delta_t->tv_sec=dt_sec-1;
      delta_t->tv_nsec=NSEC_PER_SEC+dt_nsec;
    }
  }

  return(1);
}


void main(void)
{
    struct timeval current_time_val;
    int i, rc, scope;
    cpu_set_t threadcpu;
    pthread_t threads[NUM_THREADS];
    threadParams_t threadParams[NUM_THREADS];
    pthread_attr_t rt_sched_attr[NUM_THREADS];
    int rt_max_prio, rt_min_prio;
    struct sched_param rt_param[NUM_THREADS];
    struct sched_param main_param;
    pthread_attr_t main_attr;
    pid_t mainpid;
    cpu_set_t allcpuset;

    printf("Starting High Rate Sequencer Demo\n");
    gettimeofday(&start_time_val, (struct timezone *)0);
    gettimeofday(&current_time_val, (struct timezone *)0);
    syslog(LOG_CRIT, "START High Rate Sequencer @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

   printf("System has %d processors configured and %d available.\n", get_nprocs_conf(), get_nprocs());

   CPU_ZERO(&allcpuset);

   for(i=0; i < NUM_CPU_CORES; i++)
       CPU_SET(i, &allcpuset);

   printf("Using CPUS=%d from total available.\n", CPU_COUNT(&allcpuset));


    // initialize the sequencer semaphores
    //
    if (sem_init (&semS1, 0, 0)) { printf ("Failed to initialize S1 semaphore\n"); exit (-1); }
    if (sem_init (&semS2, 0, 0)) { printf ("Failed to initialize S2 semaphore\n"); exit (-1); }
    if (sem_init (&semS3, 0, 0)) { printf ("Failed to initialize S3 semaphore\n"); exit (-1); }
    if (sem_init (&semS4, 0, 0)) { printf ("Failed to initialize S4 semaphore\n"); exit (-1); }
    if (sem_init (&semS5, 0, 0)) { printf ("Failed to initialize S5 semaphore\n"); exit (-1); }
    if (sem_init (&semS6, 0, 0)) { printf ("Failed to initialize S6 semaphore\n"); exit (-1); }
    if (sem_init (&semS7, 0, 0)) { printf ("Failed to initialize S7 semaphore\n"); exit (-1); }

    mainpid=getpid();

    rt_max_prio = sched_get_priority_max(SCHED_FIFO);
    rt_min_prio = sched_get_priority_min(SCHED_FIFO);

    rc=sched_getparam(mainpid, &main_param);
    main_param.sched_priority=rt_max_prio;
    rc=sched_setscheduler(getpid(), SCHED_FIFO, &main_param);
    if(rc < 0) perror("main_param");
    print_scheduler();


    pthread_attr_getscope(&main_attr, &scope);

    if(scope == PTHREAD_SCOPE_SYSTEM)
      printf("PTHREAD SCOPE SYSTEM\n");
    else if (scope == PTHREAD_SCOPE_PROCESS)
      printf("PTHREAD SCOPE PROCESS\n");
    else
      printf("PTHREAD SCOPE UNKNOWN\n");

    printf("rt_max_prio=%d\n", rt_max_prio);
    printf("rt_min_prio=%d\n", rt_min_prio);

    for(i=0; i < NUM_THREADS; i++)
    {

      CPU_ZERO(&threadcpu);
      CPU_SET(3, &threadcpu);

      rc=pthread_attr_init(&rt_sched_attr[i]);
      rc=pthread_attr_setinheritsched(&rt_sched_attr[i], PTHREAD_EXPLICIT_SCHED);
      rc=pthread_attr_setschedpolicy(&rt_sched_attr[i], SCHED_FIFO);
      //rc=pthread_attr_setaffinity_np(&rt_sched_attr[i], sizeof(cpu_set_t), &threadcpu);

      rt_param[i].sched_priority=rt_max_prio-i;
      pthread_attr_setschedparam(&rt_sched_attr[i], &rt_param[i]);

      threadParams[i].threadIdx=i;
    }
   
    printf("Service threads will run on %d CPU cores\n", CPU_COUNT(&threadcpu));

    // Create Service threads which will block awaiting release for:
    //

    // Servcie_1 = RT_MAX-1	@ 30 Hz
    //
    rt_param[1].sched_priority=rt_max_prio-1;
    pthread_attr_setschedparam(&rt_sched_attr[1], &rt_param[1]);
    rc=pthread_create(&threads[1],               // pointer to thread descriptor
                      &rt_sched_attr[1],         // use specific attributes
                      //(void *)0,               // default attributes
                      Service_1,                 // thread function entry point
                      (void *)&(threadParams[1]) // parameters to pass in
                     );
    if(rc < 0)
        perror("pthread_create for service 1");
    else
        printf("pthread_create successful for service 1\n");


    // Service_2 = RT_MAX-2	@ 10 Hz
    //
    rt_param[2].sched_priority=rt_max_prio-2;
    pthread_attr_setschedparam(&rt_sched_attr[2], &rt_param[2]);
    rc=pthread_create(&threads[2], &rt_sched_attr[2], Service_2, (void *)&(threadParams[2]));
    if(rc < 0)
        perror("pthread_create for service 2");
    else
        printf("pthread_create successful for service 2\n");


    // Service_3 = RT_MAX-3	@ 5 Hz
    //
    rt_param[3].sched_priority=rt_max_prio-3;
    pthread_attr_setschedparam(&rt_sched_attr[3], &rt_param[3]);
    rc=pthread_create(&threads[3], &rt_sched_attr[3], Service_3, (void *)&(threadParams[3]));
    if(rc < 0)
        perror("pthread_create for service 3");
    else
        printf("pthread_create successful for service 3\n");


    // Service_4 = RT_MAX-2	@ 10 Hz
    //
    rt_param[4].sched_priority=rt_max_prio-3;
    pthread_attr_setschedparam(&rt_sched_attr[4], &rt_param[4]);
    rc=pthread_create(&threads[4], &rt_sched_attr[4], Service_4, (void *)&(threadParams[4]));
    if(rc < 0)
        perror("pthread_create for service 4");
    else
        printf("pthread_create successful for service 4\n");


    // Service_5 = RT_MAX-3	@ 5 Hz
    //
    rt_param[5].sched_priority=rt_max_prio-3;
    pthread_attr_setschedparam(&rt_sched_attr[5], &rt_param[5]);
    rc=pthread_create(&threads[5], &rt_sched_attr[5], Service_5, (void *)&(threadParams[5]));
    if(rc < 0)
        perror("pthread_create for service 5");
    else
        printf("pthread_create successful for service 5\n");


    // Service_6 = RT_MAX-2	@ 10 Hz
    //
    rt_param[6].sched_priority=rt_max_prio-2;
    pthread_attr_setschedparam(&rt_sched_attr[6], &rt_param[6]);
    rc=pthread_create(&threads[6], &rt_sched_attr[6], Service_6, (void *)&(threadParams[6]));
    if(rc < 0)
        perror("pthread_create for service 6");
    else
        printf("pthread_create successful for service 6\n");


    // Service_7 = RT_MIN	1 Hz
    //
    rt_param[7].sched_priority=rt_min_prio;
    pthread_attr_setschedparam(&rt_sched_attr[7], &rt_param[7]);
    rc=pthread_create(&threads[7], &rt_sched_attr[7], Service_7, (void *)&(threadParams[7]));
    if(rc < 0)
        perror("pthread_create for service 7");
    else
        printf("pthread_create successful for service 7\n");


    // Wait for service threads to initialize and await relese by sequencer.
    //
    // Note that the sleep is not necessary of RT service threads are created wtih 
    // correct POSIX SCHED_FIFO priorities compared to non-RT priority of this main
    // program.
    //
    // usleep(1000000);
 
    // Create Sequencer thread, which like a cyclic executive, is highest prio
    printf("Start sequencer\n");
    threadParams[0].sequencePeriods=1800;

    // Sequencer = RT_MAX	@ 60 Hz
    //
    rt_param[0].sched_priority=rt_max_prio;
    pthread_attr_setschedparam(&rt_sched_attr[0], &rt_param[0]);
    rc=pthread_create(&threads[0], &rt_sched_attr[0], Sequencer, (void *)&(threadParams[0]));
    if(rc < 0)
        perror("pthread_create for sequencer service 0");
    else
        printf("pthread_create successful for sequeencer service 0\n");


   for(i=0;i<NUM_THREADS;i++)
       pthread_join(threads[i], NULL);

   printf("\nTEST COMPLETE\n");
   printf("Thread idx=1 ,WCET timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", wcet1.tv_sec, (wcet1.tv_nsec / NSEC_PER_MSEC), (wcet1.tv_nsec / NSEC_PER_MICROSEC), wcet1.tv_nsec);
   printf("Thread idx=2 ,WCET timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", wcet2.tv_sec, (wcet2.tv_nsec / NSEC_PER_MSEC), (wcet2.tv_nsec / NSEC_PER_MICROSEC), wcet2.tv_nsec);
   printf("Thread idx=3 ,WCET timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", wcet3.tv_sec, (wcet3.tv_nsec / NSEC_PER_MSEC), (wcet3.tv_nsec / NSEC_PER_MICROSEC), wcet3.tv_nsec);
   printf("Thread idx=4 ,WCET timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", wcet4.tv_sec, (wcet4.tv_nsec / NSEC_PER_MSEC), (wcet4.tv_nsec / NSEC_PER_MICROSEC), wcet4.tv_nsec);
   printf("Thread idx=5 ,WCET timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", wcet5.tv_sec, (wcet5.tv_nsec / NSEC_PER_MSEC), (wcet5.tv_nsec / NSEC_PER_MICROSEC), wcet5.tv_nsec);
   printf("Thread idx=6 ,WCET timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", wcet6.tv_sec, (wcet6.tv_nsec / NSEC_PER_MSEC), (wcet6.tv_nsec / NSEC_PER_MICROSEC), wcet6.tv_nsec);
   printf("Thread idx=7 ,WCET timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", wcet7.tv_sec, (wcet7.tv_nsec / NSEC_PER_MSEC), (wcet7.tv_nsec / NSEC_PER_MICROSEC), wcet7.tv_nsec);

}


void *Sequencer(void *threadp)
{
    struct timeval current_time_val;
    struct timespec delay_time = {0,16666666}; // delay for 16.67 msec, 60 Hz
    struct timespec remaining_time;
    double current_time;
    double residual;
    int rc, delay_cnt=0;
    unsigned long long seqCnt=0;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    gettimeofday(&current_time_val, (struct timezone *)0);
    syslog(LOG_CRIT, "Sequencer thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
    printf("Sequencer thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

    do
    {
        delay_cnt=0; residual=0.0;

        //gettimeofday(&current_time_val, (struct timezone *)0);
        //syslog(LOG_CRIT, "Sequencer thread prior to delay @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
        do
        {
            rc=nanosleep(&delay_time, &remaining_time);

            if(rc == EINTR)
            { 
                residual = remaining_time.tv_sec + ((double)remaining_time.tv_nsec / (double)NANOSEC_PER_SEC);

                if(residual > 0.0) printf("residual=%lf, sec=%d, nsec=%d\n", residual, (int)remaining_time.tv_sec, (int)remaining_time.tv_nsec);
 
                delay_cnt++;
            }
            else if(rc < 0)
            {
                perror("Sequencer nanosleep");
                exit(-1);
            }
           
        } while((residual > 0.0) && (delay_cnt < 100));

        seqCnt++;
        gettimeofday(&current_time_val, (struct timezone *)0);
        syslog(LOG_CRIT, "Sequencer cycle %llu @ sec=%d, msec=%d\n", seqCnt, (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);


        if(delay_cnt > 1) printf("Sequencer looping delay %d\n", delay_cnt);


        // Release each service at a sub-rate of the generic sequencer rate

        // Servcie_1 = RT_MAX-1	@ 30 Hz
        if((seqCnt % 2) == 0) sem_post(&semS1);

        // Service_2 = RT_MAX-2	@ 10 Hz
        if((seqCnt % 6) == 0) sem_post(&semS2);

        // Service_3 = RT_MAX-3	@ 5 Hz
        if((seqCnt % 12) == 0) sem_post(&semS3);

        // Service_4 = RT_MAX-2	@ 10 Hz
        if((seqCnt % 6) == 0) sem_post(&semS4);

        // Service_5 = RT_MAX-3	@ 5 Hz
        if((seqCnt % 12) == 0) sem_post(&semS5);

        // Service_6 = RT_MAX-2	@ 10 Hz
        if((seqCnt % 6) == 0) sem_post(&semS6);

        // Service_7 = RT_MIN	1 Hz
        if((seqCnt % 60) == 0) sem_post(&semS7);

        //gettimeofday(&current_time_val, (struct timezone *)0);
        //syslog(LOG_CRIT, "Sequencer release all sub-services @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

    } while(!abortTest && (seqCnt < threadParams->sequencePeriods));

    sem_post(&semS1); sem_post(&semS2); sem_post(&semS3);
    sem_post(&semS4); sem_post(&semS5); sem_post(&semS6);
    sem_post(&semS7);
    abortS1=TRUE; abortS2=TRUE; abortS3=TRUE;
    abortS4=TRUE; abortS5=TRUE; abortS6=TRUE;
    abortS7=TRUE;

    pthread_exit((void *)0);
}



void *Service_1(void *threadp)
{
    struct timeval current_time_val;
    double current_time;
    unsigned long long S1Cnt=0;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    gettimeofday(&current_time_val, (struct timezone *)0);
    syslog(LOG_CRIT, "Frame Sampler thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
    printf("Frame Sampler thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

    while(!abortS1)
    {
        sem_wait(&semS1);
        clock_gettime(CLOCK_REALTIME, &start_time);
        S1Cnt++;

        gettimeofday(&current_time_val, (struct timezone *)0);
        syslog(LOG_CRIT, "Frame Sampler release %llu @ sec=%d, msec=%d\n", S1Cnt, (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
        
        clock_gettime(CLOCK_REALTIME, &finish_time0);
        delta_t(&finish_time0, &start_time, &thread_dt0);
        printf("Thread idx=1 Count = %lld ,timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n",  S1Cnt, thread_dt0.tv_sec, (thread_dt0.tv_nsec / NSEC_PER_MSEC), (thread_dt0.tv_nsec / NSEC_PER_MICROSEC),thread_dt0.tv_nsec);
        
        temp = thread_dt0;
        delta_t(&temp, &wcet1, &thread_dt0);
        
        if((thread_dt0.tv_sec)>=0)
        {
            wcet1 = temp;
        }
    }

    pthread_exit((void *)0);
}


void *Service_2(void *threadp)
{
    struct timeval current_time_val;
    double current_time;
    unsigned long long S2Cnt=0;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    gettimeofday(&current_time_val, (struct timezone *)0);
    syslog(LOG_CRIT, "Time-stamp with Image Analysis thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
    printf("Time-stamp with Image Analysis thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

    while(!abortS2)
    {
        sem_wait(&semS2);
        clock_gettime(CLOCK_REALTIME, &start_time);
        
        S2Cnt++;
        
        gettimeofday(&current_time_val, (struct timezone *)0);
        syslog(LOG_CRIT, "Time-stamp with Image Analysis release %llu @ sec=%d, msec=%d\n",(int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
        
        clock_gettime(CLOCK_REALTIME, &finish_time0);
        delta_t(&finish_time0, &start_time, &thread_dt0);
        printf("Thread idx=2 Count = %lld , timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", S2Cnt, thread_dt0.tv_sec, (thread_dt0.tv_nsec / NSEC_PER_MSEC), (thread_dt0.tv_nsec / NSEC_PER_MICROSEC),thread_dt0.tv_nsec);
        
        temp = thread_dt0;
        delta_t(&temp, &wcet2, &thread_dt0);
        
        if((thread_dt0.tv_sec)>=0)
        {
            wcet2 = temp;
        }
    }

    pthread_exit((void *)0);
}

void *Service_3(void *threadp)
{
    struct timeval current_time_val;
    double current_time;
    unsigned long long S3Cnt=0;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    gettimeofday(&current_time_val, (struct timezone *)0);
    syslog(LOG_CRIT, "Difference Image Proc thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
    printf("Difference Image Proc thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

    while(!abortS3)
    {
        sem_wait(&semS3);
        clock_gettime(CLOCK_REALTIME, &start_time);
        
        S3Cnt++;

        gettimeofday(&current_time_val, (struct timezone *)0);
        syslog(LOG_CRIT, "Difference Image Proc release %llu @ sec=%d, msec=%d\n", S3Cnt, (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
  
        clock_gettime(CLOCK_REALTIME, &finish_time0);
        delta_t(&finish_time0, &start_time, &thread_dt0);
        printf("Thread idx=3 Count = %lld ,timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n",S3Cnt, thread_dt0.tv_sec, (thread_dt0.tv_nsec / NSEC_PER_MSEC), (thread_dt0.tv_nsec / NSEC_PER_MICROSEC),thread_dt0.tv_nsec);

        
        temp = thread_dt0;
        delta_t(&temp, &wcet3, &thread_dt0);
        
        if((thread_dt0.tv_sec)>=0)
        {
            wcet3 = temp;
        }
    }

    pthread_exit((void *)0);
}

void *Service_4(void *threadp)
{
    struct timeval current_time_val;
    double current_time;
    unsigned long long S4Cnt=0;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    gettimeofday(&current_time_val, (struct timezone *)0);
    syslog(LOG_CRIT, "Time-stamp Image Save to File thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
    printf("Time-stamp Image Save to File thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

    while(!abortS4)
    {
        sem_wait(&semS4);
        clock_gettime(CLOCK_REALTIME, &start_time);
        
        S4Cnt++;

        gettimeofday(&current_time_val, (struct timezone *)0);
        syslog(LOG_CRIT, "Time-stamp Image Save to File release %llu @ sec=%d, msec=%d\n", S4Cnt, (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
        
        clock_gettime(CLOCK_REALTIME, &finish_time0);
        delta_t(&finish_time0, &start_time, &thread_dt0);
        printf("Thread idx=4 Count = %lld ,timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", S4Cnt, thread_dt0.tv_sec, (thread_dt0.tv_nsec / NSEC_PER_MSEC), (thread_dt0.tv_nsec / NSEC_PER_MICROSEC),thread_dt0.tv_nsec);
        
        temp = thread_dt0;
        delta_t(&temp, &wcet4, &thread_dt0);
        
        if((thread_dt0.tv_sec)>=0)
        {
            wcet4 = temp;
        }
    }

    pthread_exit((void *)0);
}

void *Service_5(void *threadp)
{
    struct timeval current_time_val;
    double current_time;
    unsigned long long S5Cnt=0;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    gettimeofday(&current_time_val, (struct timezone *)0);
    syslog(LOG_CRIT, "Processed Image Save to File thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
    printf("Processed Image Save to File thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

    while(!abortS5)
    {
        sem_wait(&semS5);
        clock_gettime(CLOCK_REALTIME, &start_time);
        
        S5Cnt++;

        gettimeofday(&current_time_val, (struct timezone *)0);
        syslog(LOG_CRIT, "Processed Image Save to File release %llu @ sec=%d, msec=%d\n", S5Cnt, (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
        
        clock_gettime(CLOCK_REALTIME, &finish_time0);
        delta_t(&finish_time0, &start_time, &thread_dt0);
        printf("Thread idx=5 Count = %lld ,timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", S5Cnt, thread_dt0.tv_sec, (thread_dt0.tv_nsec / NSEC_PER_MSEC), (thread_dt0.tv_nsec / NSEC_PER_MICROSEC),thread_dt0.tv_nsec);
   
        temp = thread_dt0;
        delta_t(&temp, &wcet5, &thread_dt0);
        
        if((thread_dt0.tv_sec)>=0)
        {
            wcet5 = temp;
        }
    }

    pthread_exit((void *)0);
}

void *Service_6(void *threadp)
{
    struct timeval current_time_val;
    double current_time;
    unsigned long long S6Cnt=0;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    gettimeofday(&current_time_val, (struct timezone *)0);
    syslog(LOG_CRIT, "Send Time-stamped Image to Remote thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
    printf("Send Time-stamped Image to Remote thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

    while(!abortS6)
    {
       sem_wait(&semS6);
        clock_gettime(CLOCK_REALTIME, &start_time);
        
        S6Cnt++;

        gettimeofday(&current_time_val, (struct timezone *)0);
        syslog(LOG_CRIT, "Send Time-stamped Image to Remote release %llu @ sec=%d, msec=%d\n", S6Cnt, (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
        
        clock_gettime(CLOCK_REALTIME, &finish_time0);
        delta_t(&finish_time0, &start_time, &thread_dt0);
        printf("Thread idx=6 Count = %lld ,timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", S6Cnt, thread_dt0.tv_sec,  (thread_dt0.tv_nsec / NSEC_PER_MSEC), (thread_dt0.tv_nsec / NSEC_PER_MICROSEC),thread_dt0.tv_nsec);
    
        temp = thread_dt0;
        delta_t(&temp, &wcet6, &thread_dt0);
        
        if((thread_dt0.tv_sec)>=0)
        {
            wcet6 = temp;
        }
   }

    pthread_exit((void *)0);
}

void *Service_7(void *threadp)
{
    struct timeval current_time_val;
    double current_time;
    unsigned long long S7Cnt=0;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    gettimeofday(&current_time_val, (struct timezone *)0);
    syslog(LOG_CRIT, "Second Tick Debug thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
    printf("Second Tick Debug thread @ sec=%d, msec=%d\n", (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);

    while(!abortS7)
    {
        sem_wait(&semS7);
        clock_gettime(CLOCK_REALTIME, &start_time);
        
        S7Cnt++;

        gettimeofday(&current_time_val, (struct timezone *)0);
        syslog(LOG_CRIT, "10 Sec Tick Debug release %llu @ sec=%d, msec=%d\n", S7Cnt, (int)(current_time_val.tv_sec-start_time_val.tv_sec), (int)current_time_val.tv_usec/USEC_PER_MSEC);
        
        clock_gettime(CLOCK_REALTIME, &finish_time0);
        delta_t(&finish_time0, &start_time, &thread_dt0);
        printf("Thread idx=7 Count = %lld ,timestamp %ld sec, %ld msec (%ld microsec), ((%ld nanosec))\n\n", S7Cnt, thread_dt0.tv_sec, (thread_dt0.tv_nsec / NSEC_PER_MSEC), (thread_dt0.tv_nsec / NSEC_PER_MICROSEC),thread_dt0.tv_nsec);
    
        temp = thread_dt0;
        delta_t(&temp, &wcet7, &thread_dt0);
        
        if((thread_dt0.tv_sec)>=0)
        {
            wcet7 = temp;
        }
    }

    pthread_exit((void *)0);
}


double getTimeMsec(void)
{
  struct timespec event_ts = {0, 0};

  clock_gettime(CLOCK_MONOTONIC, &event_ts);
  return ((event_ts.tv_sec)*1000.0) + ((event_ts.tv_nsec)/1000000.0);
}


void print_scheduler(void)
{
   int schedType;

   schedType = sched_getscheduler(getpid());

   switch(schedType)
   {
       case SCHED_FIFO:
           printf("Pthread Policy is SCHED_FIFO\n");
           break;
       case SCHED_OTHER:
           printf("Pthread Policy is SCHED_OTHER\n"); exit(-1);
         break;
       case SCHED_RR:
           printf("Pthread Policy is SCHED_RR\n"); exit(-1);
           break;
       default:
           printf("Pthread Policy is UNKNOWN\n"); exit(-1);
   }
}
