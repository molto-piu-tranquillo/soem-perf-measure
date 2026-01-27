/* perf_measure.h */
#ifndef PERF_MEASURE_H
#define PERF_MEASURE_H

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

#define HIST_MIN 1
#define HIST_MAX 1000

// 시간 측정용 구조체 (컨텍스트)
typedef struct
{
   char name[32];               // 측정 이름 (예: "Cycle Time")
   uint64_t hist[HIST_MAX + 2]; // 히스토그램 데이터
   uint64_t last_print_ns;      // 마지막 출력 시간
   uint64_t start_ns;           // 시작 시간 임시 저장
   int count;                   // 현재 사이클 카운트 (출력용)
} PerfMeasure;

// 현재 시간(ns) 반환 함수
static inline uint64_t pm_now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

// 초기화 함수
static inline void pm_init(PerfMeasure *pm, const char *name)
{
   strncpy(pm->name, name, 31);
   memset(pm->hist, 0, sizeof(pm->hist));
   pm->last_print_ns = pm_now_ns();
   pm->count = 0;
}

// 측정 시작 (Start)
static inline void pm_start(PerfMeasure *pm)
{
   pm->start_ns = pm_now_ns();
}

// 측정 종료 및 히스토그램 업데이트 (End)
// 1초가 지났으면 자동으로 결과 출력
static inline void pm_end(PerfMeasure *pm)
{
   uint64_t end_ns = pm_now_ns();
   uint64_t period_us = (end_ns - pm->start_ns) / 1000; // us 단위 변환

   pm->count++;

   // 히스토그램 데이터 적재
   if (period_us >= HIST_MIN)
   {
      if (period_us <= HIST_MAX)
      {
         pm->hist[period_us]++;
      }
      else
      {
         pm->hist[HIST_MAX + 1]++;
      }
   }

   // 1초마다 출력 로직
//   if ((end_ns - pm->last_print_ns) >= 1000000000)
//   {
//      printf("\n=== Cycle %d ===\n", pm->count);
//      for (int k = HIST_MIN; k <= HIST_MAX + 1; k++)
//      {
//         if (pm->hist[k] != 0)
//         {
//            if (k == HIST_MAX + 1)
//               printf("%d+ %06ld\n", HIST_MAX, pm->hist[k]);
//            else
//               printf("%06d %06ld\n", k, pm->hist[k]);
//         }
//      }
//      pm->last_print_ns = end_ns; // 마지막 출력 시간 갱신
//
//      // (선택사항) 출력 후 초기화하고 싶으면 아래 주석 해제
//      // memset(pm->hist, 0, sizeof(pm->hist));
//   }
}

#endif // PERF_MEASURE_H