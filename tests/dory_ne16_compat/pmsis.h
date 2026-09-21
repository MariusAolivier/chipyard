#ifndef CHIPYARD_DORY_PMSIS_H
#define CHIPYARD_DORY_PMSIS_H

#include <stddef.h>
#include <stdint.h>

void dory_ne16_compat_set_error(int error);

typedef struct {
  int count;
} pi_cl_sem_t;

struct pi_device { uintptr_t data; };
struct pi_cluster_conf { int dummy; };
struct pi_cluster_task { int dummy; };
typedef struct { int dummy; } pi_cl_ram_req_t;
typedef void (*pi_cl_team_entry_t)(void *);
static int dory_ne16_current_core;

static inline pi_cl_sem_t *pi_cl_sem_alloc(void) {
  static pi_cl_sem_t semaphores[16];
  static unsigned int next;
  if (next >= sizeof(semaphores) / sizeof(semaphores[0])) {
    dory_ne16_compat_set_error(-10);
    return &semaphores[0];
  }
  return &semaphores[next++];
}
static inline void pi_cl_sem_free(pi_cl_sem_t *sem) { (void)sem; }
static inline void pi_cl_sem_set(pi_cl_sem_t *sem, int value) {
  if (sem != NULL) sem->count = value;
}
static inline void pi_cl_sem_dec(pi_cl_sem_t *sem) {
  if (sem == NULL || sem->count <= 0) {
    dory_ne16_compat_set_error(-11);
    return;
  }
  --sem->count;
}
static inline void pi_cl_sem_inc(pi_cl_sem_t *sem, int value) {
  if (sem == NULL || value < 0) {
    dory_ne16_compat_set_error(-12);
    return;
  }
  sem->count += value;
}
static inline int pi_core_id(void) { return dory_ne16_current_core; }
static inline void pi_cl_team_fork(int cores, pi_cl_team_entry_t entry,
                                   void *args) {
  if (entry == NULL || cores <= 0) {
    dory_ne16_compat_set_error(-13);
    return;
  }
  for (dory_ne16_current_core = 0; dory_ne16_current_core < cores;
       ++dory_ne16_current_core) {
    entry(args);
  }
  dory_ne16_current_core = 0;
}

#endif
