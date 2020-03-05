#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <stddef.h>

extern void syserr(int bl, const char *fmt, ...);

typedef struct runnable {
  void (*function)(void *, size_t);
  void *arg;
  size_t argsz;
} runnable_t;

typedef struct job_queue_elem {
  runnable_t job;
  void *next;
} job_queue_elem_t;

typedef struct job_queue {
  pthread_mutex_t mutex;
  job_queue_elem_t *front;
  job_queue_elem_t *back;
  unsigned len;
} job_queue_t;

typedef struct thread_pool {
  pthread_mutex_t pool_lock;
  pthread_mutex_t mutex;
  pthread_cond_t notify;
  pthread_t *threads;
  size_t pool_size;
  job_queue_t job_queue;
  volatile int shutdown;
} thread_pool_t;

int thread_pool_init(thread_pool_t *pool, size_t pool_size);

void thread_pool_destroy(thread_pool_t *pool);

int defer(thread_pool_t *pool, runnable_t runnable);

#endif
