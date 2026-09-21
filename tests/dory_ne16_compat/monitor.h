#ifndef CHIPYARD_DORY_MONITOR_H
#define CHIPYARD_DORY_MONITOR_H

#include "pmsis.h"

typedef struct {
  pi_cl_sem_t *empty;
  pi_cl_sem_t *full;
} Monitor;

static inline int monitor_init(Monitor *monitor, int buffer_size) {
  if (monitor == NULL || buffer_size <= 0) return -1;
  monitor->empty = pi_cl_sem_alloc();
  monitor->full = pi_cl_sem_alloc();
  if (monitor->empty == NULL || monitor->full == NULL) return -1;
  pi_cl_sem_set(monitor->empty, buffer_size);
  pi_cl_sem_set(monitor->full, 0);
  return 0;
}
static inline void monitor_term(Monitor monitor) {
  pi_cl_sem_free(monitor.empty);
  pi_cl_sem_free(monitor.full);
}
static inline void monitor_produce_begin(Monitor monitor) {
  pi_cl_sem_dec(monitor.empty);
}
static inline void monitor_produce_end(Monitor monitor) {
  pi_cl_sem_inc(monitor.full, 1);
}
static inline void monitor_consume_begin(Monitor monitor) {
  pi_cl_sem_dec(monitor.full);
}
static inline void monitor_consume_end(Monitor monitor) {
  pi_cl_sem_inc(monitor.empty, 1);
}

#endif
