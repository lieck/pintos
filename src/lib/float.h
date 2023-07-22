#ifndef __LIB_FLOAT_H
#define __LIB_FLOAT_H

#include <stdint.h>
#define E_VAL 2.718281
#define TOL 0.000002

typedef struct fpu_reg {
  uint16_t control;
  uint16_t unused_0;
  uint16_t status;
  uint16_t unused_1;
  uint16_t tag;
  uint16_t unused_2;
  uint32_t instruction;
  uint16_t code_segment;
  uint16_t unused_3;
  uint32_t operator;
  uint16_t date_segment;
  uint16_t unused_4;
  char st0[10];
  char st1[10];
  char st2[10];
  char st3[10];
  char st4[10];
  char st5[10];
  char st6[10];
  char st7[10];
} fpu_reg_t;

/* store floating-point unit environment after checking error conditions */
static inline void fpu_stenv(fpu_reg_t* buffer) {
  asm("mov %0, %%ebx; fsave (%%ebx)" : : "g"(buffer));
}

/* load floating-point unit environment */
static inline void fpu_ldenv(fpu_reg_t* buffer) {
  asm("mov %0, %%ebx; frstor (%%ebx)" : : "g"(buffer));
}

/* initialize floating-point unit after checking error conditions */
static inline void fpu_init(void) {
  asm volatile("finit");
}

int sys_sum_to_e(int);
double sum_to_e(int);
double abs_val(double);

/* Pushes integer num to the FPU */
static inline void fpu_push(int num) {
  asm volatile("pushl %0; flds (%%esp); addl $4, %%esp" : : "m"(num));
}

/* Pops integer from the FPU */
static inline int fpu_pop(void) {
  int val;
  asm volatile("subl $4, %%esp; fstps (%%esp); mov (%%esp), %0; addl $4, %%esp"
               : "=r"(val)
               :
               : "memory");
  return val;
}

#endif /* lib/debug.h */
