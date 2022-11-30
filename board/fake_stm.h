// minimal code to fake a panda for tests
#include <stdio.h>

#include "utils.h"

#define CANFD
#define ALLOW_DEBUG
#define PANDA

void print(const char *a) {
  printf(a);
}

void puth(unsigned int i) {
  printf("%u", i);
}

typedef struct {
  uint32_t CNT;
} TIM_TypeDef;

TIM_TypeDef timer;
TIM_TypeDef *MICROSECOND_TIMER = &timer;
uint32_t microsecond_timer_get(void);

uint32_t microsecond_timer_get(void) {
  return MICROSECOND_TIMER->CNT;
}
