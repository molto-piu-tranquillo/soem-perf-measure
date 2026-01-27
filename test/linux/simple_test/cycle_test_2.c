/** \file
 * \brief Example code for Simple Open EtherCAT master
 *
 * Usage : cycle_test_2 [ifname1]
 * ifname is NIC interface, f.e. eth0
 *
 * Histogram is updated every 1 second.
 *
 * (c)Arthur Ketels 2010 - 2011
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

// #include <net/if.h>

#include <sched.h>

#include "ethercat.h"

//#include "perf_measure.h"


#define EC_TIMEOUTMON 500

 #define HIST_MIN 1
 #define HIST_MAX 1000

char IOmap[4096];
OSAL_THREAD_HANDLE thread1;
int expectedWKC;
boolean needlf;
volatile int wkc;
boolean inOP;
uint8 currentgroup = 0;

 uint64_t now_ns(void)
 {
     struct timespec t;
     clock_gettime(CLOCK_MONOTONIC, &t);

     return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
 }

void simpletest(char *ifname)
{
    int i, oloop, iloop, chk;
    needlf = FALSE;
    inOP = FALSE;

    printf("Starting simple test\n");

    if (ec_init(ifname))
    {
        printf("ec_init on %s succeeded.\n", ifname);

        if (ec_config_init(FALSE) > 0)
        {
            printf("%d slaves found and configured.\n", ec_slavecount);

            ec_config_map(&IOmap);

            ec_configdc();

            printf("Slaves mapped, state to SAFE_OP.\n");
            ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

            oloop = ec_slave[0].Obytes;
            if ((oloop == 0) && (ec_slave[0].Obits > 0))
                oloop = 1;
            if (oloop > 8)
                oloop = 8;
            iloop = ec_slave[0].Ibytes;
            if ((iloop == 0) && (ec_slave[0].Ibits > 0))
                iloop = 1;
            if (iloop > 8)
                iloop = 8;

            printf("segments : %d : %d %d %d %d\n", ec_group[0].nsegments, ec_group[0].IOsegment[0], ec_group[0].IOsegment[1], ec_group[0].IOsegment[2], ec_group[0].IOsegment[3]);

            printf("Request operational state for all slaves\n");
            expectedWKC = (ec_group[0].outputsWKC * 2) + ec_group[0].inputsWKC;
            printf("Calculated workcounter %d\n", expectedWKC);
            ec_slave[0].state = EC_STATE_OPERATIONAL;
            ec_send_processdata();

            ec_receive_processdata(EC_TIMEOUTRET);
            ec_writestate(0);

            chk = 200;
            do
            {
                ec_send_processdata();
                ec_receive_processdata(EC_TIMEOUTRET);
                ec_statecheck(0, EC_STATE_OPERATIONAL, 50000);
            } while (chk-- && (ec_slave[0].state != EC_STATE_OPERATIONAL));
            if (ec_slave[0].state == EC_STATE_OPERATIONAL)
            {
                printf("Operational state reached for all slaves.\n");
                inOP = TRUE;
                 uint64_t hist[HIST_MAX + 2] = {0};

                 /*이것도 히스토그램 출력용 (1초마다 출력하려면 주석 해제) */
//                 uint64_t last_print_ns = now_ns();

                struct timespec target_ts;
                clock_gettime(CLOCK_MONOTONIC, &target_ts); // 절대시간 계산 위한 기준점 설정

                /* cyclic loop */
                int i = 0;
                // for (i = 1; i <= 10000; i++)
                while (i < 10000)
                {
                    i += 1;

                    // ** 1ms만큼 대기해야 함(절대시각, 누적)
                    // * 1ms(1000000 nsec)만큼 더하다가 자릿수 올림
                    target_ts.tv_nsec += 1000000;
                    if (target_ts.tv_nsec >= 1000000000)
                    {
                        target_ts.tv_sec++;
                        target_ts.tv_nsec -= 1000000000;
                    }
                    //* 해당 target_ts까지 대기
//                    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target_ts, NULL);

                    /* 상대 시간 1ms sleep - bpftrace 측정용 */
                    struct timespec sleep_rel = {0, 1000000};
                    clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_rel, NULL);

                    uint64_t t0 = now_ns();
                    *(uint16 *)ec_slave[1].outputs = 0xF0F0; // 디버깅 용. slave 디스플레이에 뜬다

                    ec_send_processdata();

                    // uint64_t t0 = now_ns();
//                     uint64_t t1 = now_ns();

                    wkc = ec_receive_processdata(EC_TIMEOUTRET);

                    // uint16 inData = 0;
                    // inData = *(uint16 *)ec_slave[1].inputs;

                     uint64_t t1 = now_ns();

/* ========== 이거 다 해제해야 됨 ========== */
                     uint64_t period = (t1 - t0) / 1000;
                     if (period >= HIST_MIN)
                     {
                         if (period <= HIST_MAX)
                         {
                             hist[period]++;
                         }
                         else
                         {
                             hist[HIST_MAX + 1]++;
                         }
                     }
/* ----------- 여기까지 --------------- */

                     /* 1초마다 누적 히스토그램 출력 */
//                     if ((t1 - last_print_ns) >= 1000000000)
//                     {
//                         // printf("Input Data: 0x%04X\n", inData);
//                         printf("\n=== cycle %d ===\n", i);
//                         for (int k = HIST_MIN; k <= HIST_MAX + 1; k++)
//                         {
//                             if (hist[k] != 0)
//                             {
//                                 if (k == HIST_MAX + 1)
//                                 {
//                                     printf("001000+ %06ld\n", hist[k]);
//                                 }
//                                 else
//                                 {
//                                     printf("%06d %06ld\n", k, hist[k]);
//                                 }
//                             }
//                         }
//                         last_print_ns = t1;
//                     }

                    // if (wkc >= expectedWKC)
                    // {
                    //     printf("Processdata cycle %4d, WKC %d , O:", i, wkc);

                    //     for (j = 0; j < oloop; j++)
                    //     {
                    //         printf(" %2.2x", *(ec_slave[0].outputs + j));
                    //     }

                    //     printf(" I:");
                    //     for (j = 0; j < iloop; j++)
                    //     {
                    //         printf(" %2.2x", *(ec_slave[0].inputs + j));
                    //     }
                    //     printf(" T:%" PRId64 "\r", ec_DCtime);
                    //     needlf = TRUE;
                    // }

                    // osal_usleep(1000);

                }
                inOP = FALSE;

                 puts("\n== Final Result ==");
                 for (int cnt = 1; cnt <= HIST_MAX + 1; cnt++)
                 {
                     if (hist[cnt] != 0)
                     {
                         if (cnt == HIST_MAX + 1)
                         {
                             printf("001000+ %06ld\n", hist[cnt]);
                         }
                         else
                         {
                             printf("%06d %06ld\n", cnt, hist[cnt]);
                         }
                     }
                 }

            }
            else
            {
                printf("Not all slaves reached operational state.\n");
                ec_readstate();
                for (i = 1; i <= ec_slavecount; i++)
                {
                    if (ec_slave[i].state != EC_STATE_OPERATIONAL)
                    {
                        printf("Slave %d State=0x%2.2x StatusCode=0x%4.4x : %s\n",
                               i, ec_slave[i].state, ec_slave[i].ALstatuscode, ec_ALstatuscode2string(ec_slave[i].ALstatuscode));
                    }
                }
            }
            printf("\nRequest init state for all slaves\n");
            ec_slave[0].state = EC_STATE_INIT;
            ec_writestate(0);
        }
        else
        {
            printf("No slaves found!\n");
        }
        printf("End simple test, close socket\n");
        ec_close();
    }
    else
    {
        printf("No socket connection on %s\nExcecute as root\n", ifname);
    }
}

OSAL_THREAD_FUNC ecatcheck(void *ptr)
{
    int slave;
    (void)ptr;

    while (1)
    {
        if (inOP && ((wkc < expectedWKC) || ec_group[currentgroup].docheckstate))
        {
            if (needlf)
            {
                needlf = FALSE;
                printf("\n");
            }
            ec_group[currentgroup].docheckstate = FALSE;
            ec_readstate();

            for (slave = 1; slave <= ec_slavecount; slave++)
            {
                if ((ec_slave[slave].group == currentgroup) && (ec_slave[slave].state != EC_STATE_OPERATIONAL))
                {
                    ec_group[currentgroup].docheckstate = TRUE;
                    if (ec_slave[slave].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR))
                    {
                        printf("ERROR : slave %d is in SAFE_OP + ERROR, attempting ack.\n", slave);
                        ec_slave[slave].state = (EC_STATE_SAFE_OP + EC_STATE_ACK);
                        ec_writestate(slave);
                    }
                    else if (ec_slave[slave].state == EC_STATE_SAFE_OP)
                    {
                        printf("WARNING : slave %d is in SAFE_OP, change to OPERATIONAL.\n", slave);
                        ec_slave[slave].state = EC_STATE_OPERATIONAL;
                        ec_writestate(slave);
                    }
                    else if (ec_slave[slave].state > EC_STATE_NONE)
                    {
                        if (ec_reconfig_slave(slave, EC_TIMEOUTMON))
                        {
                            ec_slave[slave].islost = FALSE;
                            printf("MESSAGE : slave %d reconfigured\n", slave);
                        }
                    }
                    else if (!ec_slave[slave].islost)
                    {
                        ec_statecheck(slave, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
                        if (ec_slave[slave].state == EC_STATE_NONE)
                        {
                            ec_slave[slave].islost = TRUE;
                            printf("ERROR : slave %d lost\n", slave);
                        }
                    }
                }
                if (ec_slave[slave].islost)
                {
                    if (ec_slave[slave].state == EC_STATE_NONE)
                    {
                        if (ec_recover_slave(slave, EC_TIMEOUTMON))
                        {
                            ec_slave[slave].islost = FALSE;
                            printf("MESSAGE : slave %d recovered\n", slave);
                        }
                    }
                    else
                    {
                        ec_slave[slave].islost = FALSE;
                        printf("MESSAGE : slave %d found\n", slave);
                    }
                }
            }
            if (!ec_group[currentgroup].docheckstate)
                printf("OK : all slaves resumed OPERATIONAL.\n");
        }
        osal_usleep(10000);
    }
}

int main(int argc, char *argv[])
{
    printf("SOEM (Simple Open EtherCAT Master)\nCycle test with 1-second histogram update\n");

    /* 1. 메모리 잠금 (Page Fault 방지) */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
    {
        perror("mlockall failed");
        return -1;
    }

    unsigned char dummy[8192];
    memset(dummy, 0, 8192);

    /* 2. 현재 스레드(Main)를 CPU 3번에 고정 */
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(3, &cpus); // CPU 3번 선택

    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpus) != 0)
    {
        perror("pthread_setaffinity_np failed");
        return -1;
    }

    struct sched_param param;
    param.sched_priority = 99; // 최대 99

            /* 3. 스케줄러 정책 설정 (RT Priority) */
            if (sched_setscheduler(0, SCHED_FIFO, &param) == -1)
            {
                perror("sched_setscheduler failed");
                return -1;
            }

            if (argc > 1)
            {
        //        osal_thread_create(&thread1, 128000, &ecatcheck, (void *)&ctime);
                simpletest(argv[1]);
            }
    {
        printf("Usage: cycle_test_2 ifname1\nifname = eth0 for example\n");
    }

    printf("End program\n");
    return (0);
}
