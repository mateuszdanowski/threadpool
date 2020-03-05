#include "threadpool.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void syserr(int bl, const char *fmt, ...) {
  va_list fmt_args;

  fprintf(stderr, "ERROR: ");

  va_start(fmt_args, fmt);
  vfprintf(stderr, fmt, fmt_args);
  va_end(fmt_args);
  fprintf(stderr, " (%d; %s)\n", bl, strerror(bl));
  exit(1);
}

int job_queue_init(job_queue_t *job_queue) {
  job_queue->front = NULL;
  job_queue->back = NULL;
  job_queue->len = 0;

  if (pthread_mutex_init(&job_queue->mutex, NULL) != 0) {
    free(job_queue);
    return -1;
  }
  return 0;
}

void job_queue_push(job_queue_t *job_queue, job_queue_elem_t *elem) {
  int err;

  if ((err = pthread_mutex_lock(&job_queue->mutex)) != 0) {
    syserr(err, "lock failed");
  }

  if (job_queue->len == 0) {
    job_queue->front = elem;
    job_queue->back = elem;
  } else {
    job_queue->back->next = elem;
    job_queue->back = elem;
  }
  job_queue->len++;

  if ((err = pthread_mutex_unlock(&job_queue->mutex)) != 0) {
    syserr(err, "unlock failed");
  }
}

runnable_t *job_queue_get(job_queue_t *job_queue) {
  int err;

  if ((err = pthread_mutex_lock(&job_queue->mutex)) != 0) {
    syserr(err, "lock failed");
  }

  runnable_t *job;

  if (job_queue->len == 0) {
    job = NULL;
  } else {
    job = (runnable_t *)malloc(sizeof(runnable_t));
    if (job == NULL) {
      if ((err = pthread_mutex_unlock(&job_queue->mutex)) != 0) {
        syserr(err, "unlock failed");
      }
      return NULL;
    }

    job_queue_elem_t *elem = job_queue->front;
    *job = job_queue->front->job;

    job_queue->len--;

    if (job_queue->len == 0) {
      job_queue->front = NULL;
      job_queue->back = NULL;
    } else {
      job_queue->front = job_queue->front->next;
    }

    free(elem);
  }

  if ((err = pthread_mutex_unlock(&job_queue->mutex)) != 0) {
    syserr(err, "unlock failed");
  }
  return job;
}

void job_queue_destroy(job_queue_t *job_queue) {
  int err;

  if ((err = pthread_mutex_lock(&job_queue->mutex)) != 0) {
    syserr(err, "lock failed");
  }

  while (job_queue->len > 0) {
    free(job_queue_get(job_queue));
  }

  job_queue->front = NULL;
  job_queue->back = NULL;
  job_queue->len = 0;

  if ((err = pthread_mutex_unlock(&job_queue->mutex)) != 0) {
    syserr(err, "unlock failed");
  }
}

void *run(void *thread_pool) {
  int err;

  thread_pool_t *pool = (thread_pool_t *)thread_pool;
  runnable_t *job;

  while (1) {
    if ((err = pthread_mutex_lock(&pool->mutex)) != 0) {
      syserr(err, "lock failed");
    }

    while (pool->job_queue.len == 0 && pool->shutdown == 0) {
      if ((err = pthread_cond_wait(&pool->notify, &pool->mutex)) != 0) {
        syserr(err, "cond failed");
      }
    }

    if (pool->shutdown == 1 && pool->job_queue.len == 0) {
      break;
    }

    job = job_queue_get(&pool->job_queue);

    if ((err = pthread_mutex_unlock(&pool->mutex)) != 0) {
      syserr(err, "unlock failed");
    }

    if (job != NULL) {
      (job->function)(job->arg, job->argsz);
      free(job);
    }
  }

  if ((err = pthread_mutex_unlock(&pool->mutex)) != 0) {
    syserr(err, "unlock failed");
  }
  return NULL;
}

int thread_pool_init(thread_pool_t *pool, size_t pool_size) {
  if (pool == NULL) {
    return -1;
  }

  pool->pool_size = pool_size;
  pool->shutdown = 0;

  if (job_queue_init(&pool->job_queue) == -1) {
    free(pool);
    return -1;
  }

  pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * pool_size);
  if (pool->threads == NULL) {
    job_queue_destroy(&pool->job_queue);
    free(pool);
    return -1;
  }

  if (pthread_mutex_init(&pool->mutex, NULL) != 0 ||
      pthread_cond_init(&pool->notify, NULL) != 0 ||
      pthread_mutex_init(&pool->pool_lock, NULL) != 0) {
    job_queue_destroy(&pool->job_queue);
    free(pool->threads);
    free(pool);
    return -1;
  }

  for (size_t i = 0; i < pool->pool_size; i++) {
    if (pthread_create(&pool->threads[i], NULL, run, (void *)pool) != 0) {
      thread_pool_destroy(pool);
      return -1;
    }
  }

  return 0;
}

void thread_pool_destroy(thread_pool_t *pool) {
  if (pool == NULL) {
    return;
  }
  int err;

  if ((err = pthread_mutex_lock(&pool->pool_lock)) != 0) {
    syserr(err, "lock failed");
  }

  pool->shutdown = 1;
  if ((err = pthread_cond_broadcast(&pool->notify)) != 0) {
    syserr(err, "broadcast failed");
  }

  for (size_t i = 0; i < pool->pool_size; i++) {
    if ((err = pthread_join(pool->threads[i], NULL)) != 0) {
      syserr(err, "join failed");
    }
  }

  job_queue_destroy(&pool->job_queue);
  free(pool->threads);

  if ((err = pthread_mutex_unlock(&pool->pool_lock)) != 0) {
    syserr(err, "unlock failed");
  }

  pthread_mutex_destroy(&pool->pool_lock);
  pthread_cond_destroy(&pool->notify);
  pthread_mutex_destroy(&pool->mutex);
}

int defer(thread_pool_t *pool, runnable_t runnable) {
  int err;

  if ((err = pthread_mutex_lock(&pool->pool_lock)) != 0) {
    syserr(err, "lock failed");
  }

  if (pool == NULL || pool->threads == NULL || pool->shutdown == 1) {
    if ((err = pthread_mutex_unlock(&pool->pool_lock)) != 0) {
      syserr(err, "unlock failed");
    }
    return -1;
  }

  job_queue_elem_t *elem = (job_queue_elem_t *)malloc(sizeof(job_queue_elem_t));
  if (elem == NULL) {
    if ((err = pthread_mutex_lock(&pool->pool_lock)) != 0) {
      syserr(err, "unlock failed");
    }
    return -1;
  }
  elem->job = runnable;
  elem->next = NULL;

  job_queue_push(&pool->job_queue, elem);

  if ((err = pthread_mutex_unlock(&pool->pool_lock)) != 0) {
    syserr(err, "unlock failed");
  }
  pthread_cond_broadcast(&pool->notify);

  return 0;
}
