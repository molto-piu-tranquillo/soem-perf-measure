/** \file
 * \brief Example code for Simple Open EtherCAT master
 *
 * Usage : simple_test [ifname1]
 * ifname is NIC interface, f.e. eth0
 *
 * This is a minimal test.
 *
 * (c)Arthur Ketels 2010 - 2011
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include "ethercat.h"

#define EC_TIMEOUTMON 500

#define HIST_MIN 1
#define HIST_MAX 200
#define TARGET 1000

/*------------USER_DEFINED FUNCTIONS------------*/

uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);

    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

/*----------------------------------------------*/

char IOmap[4096];
OSAL_THREAD_HANDLE thread1; // pthread_t 같은 거
int expectedWKC;            // 기대하는 wirk counter. 이 값 이상이면 정상으로 간주
boolean needlf;
volatile int wkc;       // simpletest thread에서 업데이트, ecatcheck가 읽음
boolean inOP;           // isInOperation?
uint8 currentgroup = 0; // slave의 그룹

uint64_t hist[HIST_MAX + 1] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int isRunning = 0;

void* print_hist(void* arg)
{
    (void)arg;

    while (isRunning)
    {
        sleep(1);
        pthread_mutex_lock(&mutex);
        for (int i = 1; i <= HIST_MAX; i++)
        {
            printf("%06d %06ld\n", i, hist[i]);
        }
        pthread_mutex_unlock(&mutex);
        
    }

    return NULL;
}

void simpletest(char *ifname) // eth0 eth1 ...
{
    int i, j, oloop, iloop, chk;
    needlf = FALSE;
    inOP = FALSE;

    uint64_t prev, start, error, period;

    printf("Starting simple test\n");

    /* initialise SOEM, bind socket to ifname */
    if (ec_init(ifname)) // TCP/IP의 bind() 같은 거인 듯. 성공 시 true 실패 시 false -> master가 eth에 붙었냐?
    {
        printf("ec_init on %s succeeded.\n", ifname); // 마스터가 eth에 붙음. 로그 출력
        /* find and auto-config slaves */

        if (ec_config_init(FALSE) > 0) // int ec_config_init(uint8 usetable); -> 리턴값: 찾은 슬레이브 개수, false는 테이블 기반 off?
        {
            printf("%d slaves found and configured.\n", ec_slavecount); // 말 그대로 발견된 슬레이브 개수 보통 바로 위의 init 리턴값과 동일

            ec_config_map(&IOmap); // PDO(프로세스 데이터 영역)을 IOmap 버퍼에 매핑
            /*
            **PDO: 제어/센서 값처럼 매 ms마다 주고받는 값들.
            반대로, 자주 안 바꾸는 설정값(parameter 등) -> SDO(Service Data Object)

            *출력 PDO: Master -> Slave. 목표 속도, 제어 모드 등
            *입력 PDO: Slave -> Master. 실제 속도, 실제 위치 등
            */

            /*
            **이후에 ec_slave[0].outputs, ec_slave[0].inputs가 IOmap의 특정 위치를 가리키는 포인터로 세팅된다.
            */

            /*
            struct ec_slavet {
               ...
               uint8 *outputs; //출력 PDO
               uint8 *inputs; //입력 PDO
               ...
            }
            */

            ec_configdc(); // 클락(DC: Distrubuted Clock) 동기화

            printf("Slaves mapped, state to SAFE_OP.\n");
            /* wait for all slaves to reach SAFE_OP state */
            ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
            // ec_statecheck(i, state, timeout) -> i번째 slave가 state까지 올라왔는지 timeout만큼 기다려라
            // 이 과정이 끝나면 ec_slave[i].state에 해당 상태가 업데이트됨
            // 리턴값은 해당 슬레이브의 실제 state
            /*
            * i=0이면 0번째 slave가 아니라 모든 slave,

            ** INIT -> PRE_OP -> SAFE_OP -> OP
            * INIT(0x01)
            * PRE_OP(0x02): 설정 및 통신 준비 단계(사이클 돌리기 전)
            * SAFE_OP(0x04): 입력 PDO(슬레이브가 주는 상태 및 센서 값)는 읽을 수 있으나, 출력 PDO(실제 제어 위한 값)는 제한
            * OP(0x08): 출력도 정상 반영되고 주기 제어가 가능한 단계
            */
            // SOEM이 정한 기본 타임아웃 상수보다 4배 더 기다리기

            oloop = ec_slave[0].Obytes; // 전체 out 바이트 수
            if ((oloop == 0) && (ec_slave[0].Obits > 0))
                oloop = 1; // 바이트는 0인데 비트는 있으면, 1바이트 출력
            if (oloop > 8)
                oloop = 8;              // 너무 길면 최대 8바이트만 출력
            iloop = ec_slave[0].Ibytes; // 전체 in 바이트 수
            if ((iloop == 0) && (ec_slave[0].Ibits > 0))
                iloop = 1;
            if (iloop > 8)
                iloop = 8;

            printf("segments : %d : %d %d %d %d\n", ec_group[0].nsegments, ec_group[0].IOsegment[0], ec_group[0].IOsegment[1], ec_group[0].IOsegment[2], ec_group[0].IOsegment[3]);
            // 0번 그룹의 1-4번째 segment -> segment: IOmap의 데이터가 이더캣 datagram에 실리는 단위

            /*
            struct ec_groupt {
            int nsegments; //해당 그룹 세그먼트 몇 개?
            int IOsegment[EC_MAXIOSEGMENTS]; // 일부만 출력

            int outputsWKC;
            int inputsWKC;

            boolean docheckstate;   //해당 그룹의 status check 필요한지 flag
            };
            extern struct ec_groupt ec_group[EC_MAXGROUP];  //구조체 담고있는 배열
            */

            printf("Request operational state for all slaves\n");
            expectedWKC = (ec_group[0].outputsWKC * 2) + ec_group[0].inputsWKC;
            // 입력 관련 datagram 성공 -> inputsWKC가 증가, 출력도 마찬가지. 수식은 그냥 실무 기준
            printf("Calculated workcounter %d\n", expectedWKC);
            ec_slave[0].state = EC_STATE_OPERATIONAL; // 보낼 요청 저장. 아직 요청을 보내진 않음
            /* send one valid process data to make outputs in slaves happy*/
            ec_send_processdata(); // 데이터가 한 번도 안 왔으면 OP 안넘어가려는 경우 있음. 따라서 OP 요청 전에 processdata 한 번 보내줌

            ec_receive_processdata(EC_TIMEOUTRET);
            /* request OP state for all slaves */
            ec_writestate(0); // 전체 슬레이브(0)에게 상태 변경 요청 write
            // 이건 전체 슬레이브의 ec_slave[i].state에 저장된 값을 write 함

            /*
            **ec_send_processdata()
            * 출력 PDO를 이더캣 프레임에 태워서 전송
            * 프레임이 슬레이브를 순서대로 통과하며(on-the-fly), 슬레이브는 본인에게 해당하는 출력PDO(master->slave) 읽고
            동시에 프레임에 자신의 입력PDO(slave -> master)를 써넣는 일련의 과정들이 send 안에서 일어남
            ** ec_receive_processdata()
            * send가 프레임을 내보낼 때, 슬레이브들이 쓴 입력 PDO값은 다시 프레임과 함께 master로 돌아옴.
            * receive는 이 값을 소켓에서 읽어오고, 읽은 PDO값을 IOmap에 반영하고, wkc 값 리턴 (세 가지 역할을 순차적으로 함)
            * timeout 내에 프레임 못 받으면, 통신이상으로 간주
            */
            chk = 200; // 200번 기회 줌
            /* wait for all slaves to reach OP state */
            do
            {
                ec_send_processdata();
                ec_receive_processdata(EC_TIMEOUTRET);         // processdata로 프레임 받으면 입력 PDO(slave -> master) 갱신 후 wkc(work counter)응답 리턴. but timeout 동안만 기다림
                ec_statecheck(0, EC_STATE_OPERATIONAL, 50000); // 아까 라인이랑 같음. 모든 slave(0)가 OP상태인지, 50ms동안 기다림
            } while (chk-- && (ec_slave[0].state != EC_STATE_OPERATIONAL));
            if (ec_slave[0].state == EC_STATE_OPERATIONAL) // 모든 slaves are operational
            {
                printf("Operational state reached for all slaves.\n");
                inOP = TRUE;
                isRunning = 1;
                // uint64_t hist[HIST_MAX + 1] = {0};

                prev = now_ns();
                // next = prev + 1000000000;
                pthread_t hist_thread;
                pthread_create(&hist_thread, NULL, print_hist, NULL);
                // pthread_join(hist_thread, NULL);
                /* cyclic loop */
                for (i = 1; i <= 10000; i++)
                {
                    
                    start = now_ns();

                    period = start - prev;
                    prev = start;

                    error = (period - TARGET) / 1000;
                    // printf("Period: %ld Error: %ld \r", period, error);

                    if (error >= 1 && error <= HIST_MAX)
                    {
                        hist[error]++;
                    }

                    ec_send_processdata();
                    wkc = ec_receive_processdata(EC_TIMEOUTRET); // 응답(입력 PDO) 받아서 IOmap 갱신, wkc 얻기 매 사이클마다 이 2-step은 하는 듯

                    if (wkc >= expectedWKC) // 정상이면
                    {
                        printf("Processdata cycle %4d, WKC %d , O:", i, wkc);

                        for (j = 0; j < oloop; j++) // 해당 output 값 실제로 찍어봄
                        {
                            printf(" %2.2x", *(ec_slave[0].outputs + j)); // 폭, 자리수 2개씩
                        }

                        printf(" I:");
                        for (j = 0; j < iloop; j++)
                        {
                            printf(" %2.2x", *(ec_slave[0].inputs + j));
                        }
                        printf(" T:%" PRId64 "\r", ec_DCtime); // 모든 슬레이브가 공유하는, 네트쿽에서 공용으로 맞춘 시간값. 해당 시각에 나온 값이라는 것을 명시
                        // PRId64는 64비트 정수 찍을 때 발생하는 오류 처리해줌. 64-bit based 플랫폼이 아니면 long값이 64비트 아닐 수도 있으니
                        // 어떤 환경에서는 64비트 값을 %ld로, 어떤 환경에서는 %lld로 알아서 바꿔줌
                        needlf = TRUE;
                    }
                    // osal_usleep(5000);
                }

                inOP = FALSE;
                pthread_join(hist_thread, NULL);
                isRunning = 0;
                // for (int i = 1; i <= HIST_MAX; i++)
                // {
                //     printf("%06d %06ld\n", i, hist[i]);
                // }
            }
            else
            {
                printf("Not all slaves reached operational state.\n");
                ec_readstate(); // 각 슬레이브의 ec_slave[i].state 업데이트
                for (i = 1; i <= ec_slavecount; i++)
                {
                    if (ec_slave[i].state != EC_STATE_OPERATIONAL)
                    {
                        printf("Slave %d State=0x%2.2x StatusCode=0x%4.4x : %s\n",
                               i, ec_slave[i].state, ec_slave[i].ALstatuscode, ec_ALstatuscode2string(ec_slave[i].ALstatuscode));
                        // ec_ALstatuscode2string: ALststuscode는 에러 났을 때 이유 담고 있는 비트. 이를 자연어로 바꿔 알려줌
                    } // inoperational인 slave의 상태 출력
                }
            }
            printf("\nRequest init state for all slaves\n");
            ec_slave[0].state = EC_STATE_INIT; // 이제 전체 slave를 init 상태로 내림
            /* request INIT state for all slaves */
            ec_writestate(0);
        }
        else // if ( ec_config_init(FALSE) > 0 ) 의 분기
        {
            printf("No slaves found!\n");
        }
        printf("End simple test, close socket\n");
        /* stop SOEM, close socket */
        ec_close(); // socket close
    }
    else // if (ec_init(ifname)) 의 분기 (bind 실패)
    {
        printf("No socket connection on %s\nExcecute as root\n", ifname);
    }
}

OSAL_THREAD_FUNC ecatcheck(void *ptr)
{
    int slave;
    (void)ptr; /* Not used */

    while (1)
    {
        if (inOP && ((wkc < expectedWKC) || ec_group[currentgroup].docheckstate)) // 감시 조건: inOperation이고, wkc 충족 못했거나 그룹의 docheckstate flag 1일 때
        {
            if (needlf)
            {
                needlf = FALSE;
                printf("\n");
            }
            /* one ore more slaves are not responding */
            ec_group[currentgroup].docheckstate = FALSE; // flag 바꾸고, 밑에서 슬레이브들 검사해보고 진짜 문제 있으면 true로 올릴 것임.
            ec_readstate();                              // 각 슬레이브의 ec_slave[i].state 업데이트
            /*
            * ec_slavet의 ALStatus, Alstatuscode 읽어와서 구조체 배열에 저장
            * ec_slave[i].state, ec_slave[i].ALstatuscode 이런 식으로

            readstate는 입력PDO를 읽는 게 아니라, 상태 레지스터만 읽는다. ec_receive_processdata와 헷갈리지 말기
            */

            for (slave = 1; slave <= ec_slavecount; slave++)
            {
                if ((ec_slave[slave].group == currentgroup) && (ec_slave[slave].state != EC_STATE_OPERATIONAL))
                // 앞서 ec_readstate()가 저장한 state 값 읽어서 OP 아니면
                {
                    ec_group[currentgroup].docheckstate = TRUE;
                    // 해당 그룹이 체크 대상(slave 구조체가 그룹 정보 담고 있으니)
                    if (ec_slave[slave].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR))
                    // 비트 연산 가능: state(safe_op는 0x04) + ERROR(보통 0x10)해서, safeop중인데 error값 붙으면 0x14 되겠지.
                    // 모종의 에러가 났기에 op로 가지 못하고 아직 safeop에 머물러 있다
                    {
                        printf("ERROR : slave %d is in SAFE_OP + ERROR, attempting ack.\n", slave);
                        ec_slave[slave].state = (EC_STATE_SAFE_OP + EC_STATE_ACK);
                        // 에러 난 거 확인했으니, 확인용 ack 비트 붙여서 state에 저장
                        ec_writestate(slave);
                        // 해당 slave가 저장돼있는 배열의 state 값을 읽어서, 실제로 상태 변경 요청 보내기(safe_op, ack)
                    }
                    else if (ec_slave[slave].state == EC_STATE_SAFE_OP)
                    // 에러는 아닌데 여전히 safeop상태면 (아무 이유 없이 op로 가지 못했으면)
                    {
                        printf("WARNING : slave %d is in SAFE_OP, change to OPERATIONAL.\n", slave);
                        ec_slave[slave].state = EC_STATE_OPERATIONAL;
                        // ec_slave[i].state는 실제 상태를 저장할뿐 아니라, 변경하고자 하는 값을 저장하는 명령 버퍼의 역할도 수행
                        ec_writestate(slave);
                        // 해당 slave가 저장돼있는 배열의 state 값을 읽어서, 실제로 상태 변경 요청 보내기
                    }
                    else if (ec_slave[slave].state > EC_STATE_NONE)
                    // 상태 비트가 0은 아닌데 op도 safeop도, safeop+error 도 아님
                    // EC_STATE_NONE은 아예 통신이 안 되는 것
                    {
                        if (ec_reconfig_slave(slave, EC_TIMEOUTMON))
                        // ex_reconfig_slave는 slave번째 슬레이브를 재설정해서 정상 state로 만든다
                        // 리턴값은 단순 성공/실패가 아니라 성공했을 시 슬레이브의 상태값
                        {
                            ec_slave[slave].islost = FALSE;
                            // 통신은 됐다는 얘기니까 lost는 아님
                            printf("MESSAGE : slave %d reconfigured\n", slave);
                        }
                    }
                    else if (!ec_slave[slave].islost)
                    // 상태 비트가 op도, safeop도, safeop+error도 아닌데 상태 비트가 0보다 크지도 않음 -> 메모리의 상태비트가 0이란 말
                    // 그런데도 해당 슬레이브의 islost값이 false이면, 그냥 ec_slave[].state가 업데이트 안 된 건 아닌가?
                    {
                        /* re-check state */
                        ec_statecheck(slave, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
                        // slave번째 해당 슬레이브가 op 아닌지 타임아웃 걸어 확인
                        // op라면 ec_slave[].state 값을 op로 업데이트
                        if (ec_slave[slave].state == EC_STATE_NONE) // 근데 여전히 none이면
                        {
                            ec_slave[slave].islost = TRUE; // 진짜 통신이 안 되는 것
                            printf("ERROR : slave %d lost\n", slave);
                        }
                    }
                }
                if (ec_slave[slave].islost) // 앞선 분기에서 진짜 lost로 판명난 슬레이브라면
                {
                    if (ec_slave[slave].state == EC_STATE_NONE) // 아직도 상태 저장값이 none인가? 아니면 그 사이에 갑자기 업데이트됐나?
                    {
                        if (ec_recover_slave(slave, EC_TIMEOUTMON)) // recover 성공, 즉 해당 slave의 state값을 뭐로든 설정했음.(네트워크로 복귀)
                        {
                            ec_slave[slave].islost = FALSE;
                            printf("MESSAGE : slave %d recovered\n", slave);
                        }
                    }
                    else // 다시 확인해봤는데 none이 아님. recover 필요 없음
                    {
                        ec_slave[slave].islost = FALSE;
                        printf("MESSAGE : slave %d found\n", slave);
                    }
                }
            }
            if (!ec_group[currentgroup].docheckstate) // for문의 매 loop마다 체크: 해당 group의 checkstate가 올라와있지는 않은가
                printf("OK : all slaves resumed OPERATIONAL.\n");
        }
        osal_usleep(10000); // 무한루프 돌리긴 하는데 10ms 쉬고 다시 감시
    }
}

int main(int argc, char *argv[])
{
    printf("SOEM (Simple Open EtherCAT Master)\nSimple test\n");

    if (argc > 1)
    {
        /* create thread to handle slave error handling in OP */
        //      pthread_create( &thread1, NULL, (void *) &ecatcheck, (void*) &ctime);
        osal_thread_create(&thread1, 128000, &ecatcheck, (void *)&ctime);
        /* start cyclic part */
        simpletest(argv[1]);
    }
    else
    {
        printf("Usage: simple_test ifname1\nifname = eth0 for example\n");
    }

    printf("End program\n");
    return (0);
}
