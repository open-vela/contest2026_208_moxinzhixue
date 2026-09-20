/****************************************************************************
 * Contest 2026 team 208 - pthread/message queue wrapper
 ****************************************************************************/

#ifndef __GS1_THREAD_H
#define __GS1_THREAD_H

#include <nuttx/config.h>

#include <mqueue.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GS1_THREAD_PAYLOAD_MAX 384

typedef int (*gs1_thread_message_fn_t)(FAR void *arg,
                                       FAR const void *payload,
                                       size_t payload_size);
typedef void (*gs1_thread_idle_fn_t)(FAR void *arg);
typedef void (*gs1_thread_stop_fn_t)(FAR void *arg);

struct gs1_thread_callbacks_s
{
  gs1_thread_message_fn_t message;
  gs1_thread_idle_fn_t idle;
  gs1_thread_stop_fn_t stop;
};

struct gs1_thread_queue_s
{
  pthread_t thread;
  mqd_t receive_queue;
  mqd_t send_queue;
  char queue_name[32];
  struct gs1_thread_callbacks_s callbacks;
  FAR void *arg;
  uint32_t idle_interval_ms;
  volatile bool closing;
  bool started;
};

int gs1_thread_start(FAR struct gs1_thread_queue_s *queue,
                     FAR const char *name, size_t stack_size,
                     uint32_t idle_interval_ms,
                     FAR const struct gs1_thread_callbacks_s *callbacks,
                     FAR void *arg);
int gs1_thread_post(FAR struct gs1_thread_queue_s *queue,
                    FAR const void *payload, size_t payload_size);
int gs1_thread_depth(FAR struct gs1_thread_queue_s *queue,
                     FAR uint32_t *depth);
int gs1_thread_stop(FAR struct gs1_thread_queue_s *queue);

#endif /* __GS1_THREAD_H */
