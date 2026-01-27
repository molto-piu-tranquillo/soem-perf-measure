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

char IOmap[4096];
OSAL_THREAD_HANDLE thread1; // pthread_t 같은 거
int expectedWKC;            // 기대하는 wirk counter. 이 값 이상이면 정상으로 간주
boolean needlf;
volatile int wkc;       // simpletest thread에서 업데이트, ecatcheck가 읽음
boolean inOP;           // isInOperation?
uint8 currentgroup = 0; // slave의 그룹

uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);

    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

void simpletest(char *ifname) // eth0 eth1 ...
{
    int i, j, oloop, iloop, chk;
    needlf = FALSE;
    inOP = FALSE;

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

            ec_configdc(); // 클락(DC: Distrubuted Clock) 동기화

            printf("Slaves mapped, state to SAFE_OP.\n");
            /* wait for all slaves to reach SAFE_OP state */
            ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

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
                uint64_t hist[1000 + 1] = {0};

                /* cyclic loop */
                for (i = 1; i <= 4000; i++)
                {
                    uint64_t t0 = now_ns();
                    ec_send_processdata();
                    wkc = ec_receive_processdata(EC_TIMEOUTRET);
                    uint64_t t1 = now_ns();

                    uint64_t period = (t1 - t0) / 1000;
                    if (period >= 1 && period <= 1000)
                    {
                        hist[period]++;
                    }

                    if (wkc >= expectedWKC)
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
                        printf(" T:%" PRId64 "\r", ec_DCtime);
                        needlf = TRUE;
                    }
                    osal_usleep(5000);
                }
                inOP = FALSE;
                for (int i = 1; i <= 1000; i++)
                {
                    printf("%06d %06ld\n", i, hist[i]);
                }
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
            ec_readstate();

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
