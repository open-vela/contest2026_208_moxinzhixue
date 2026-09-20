/****************************************************************************
 * Contest 2026 team 208 - pthread/message queue wrapper
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gs1_thread.h"

struct gs1_thread_envelope_s
{
  bool stop;
  uint16_t size;
  uint8_t payload[GS1_THREAD_PAYLOAD_MAX];
};

static void gs1_thread_deadline(FAR struct timespec *deadline,
                                uint32_t interval_ms)
{
  clock_gettime(CLOCK_REALTIME, deadline);
  deadline->tv_sec += interval_ms / 1000;
  deadline->tv_nsec += (long)(interval_ms % 1000) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L)
    {
      deadline->tv_sec++;
      deadline->tv_nsec -= 1000000000L;
    }
}

static FAR void *gs1_thread_entry(FAR void *arg)
{
  FAR struct gs1_thread_queue_s *queue = arg;
  struct gs1_thread_envelope_s envelope;
  struct timespec deadline;
  ssize_t size;

  for (;;)
    {
      memset(&envelope, 0, sizeof(envelope));
      gs1_thread_deadline(&deadline, queue->idle_interval_ms);
      size = mq_timedreceive(queue->receive_queue,
                             (FAR char *)&envelope,
                             sizeof(envelope), NULL, &deadline);
      if (size < 0)
        {
          if (errno == ETIMEDOUT || errno == EINTR)
            {
              if (queue->callbacks.idle != NULL)
                {
                  queue->callbacks.idle(queue->arg);
                }

              continue;
            }

          break;
        }

      if (envelope.stop)
        {
          break;
        }

      if (envelope.size <= GS1_THREAD_PAYLOAD_MAX &&
          queue->callbacks.message != NULL)
        {
          queue->callbacks.message(queue->arg, envelope.payload,
                                   envelope.size);
        }
    }

  if (queue->callbacks.stop != NULL)
    {
      queue->callbacks.stop(queue->arg);
    }

  return NULL;
}

int gs1_thread_start(FAR struct gs1_thread_queue_s *queue,
                     FAR const char *name, size_t stack_size,
                     uint32_t idle_interval_ms,
                     FAR const struct gs1_thread_callbacks_s *callbacks,
                     FAR void *arg)
{
  struct mq_attr mqattr;
  pthread_attr_t threadattr;
  int ret;

  if (queue == NULL || name == NULL || callbacks == NULL ||
      callbacks->message == NULL || idle_interval_ms == 0)
    {
      return -EINVAL;
    }

  memset(queue, 0, sizeof(*queue));
  queue->receive_queue = (mqd_t)-1;
  queue->send_queue = (mqd_t)-1;
  queue->callbacks = *callbacks;
  queue->arg = arg;
  queue->idle_interval_ms = idle_interval_ms;

  snprintf(queue->queue_name, sizeof(queue->queue_name), "/%s_%lx",
           name, (unsigned long)getpid());
  mq_unlink(queue->queue_name);

  memset(&mqattr, 0, sizeof(mqattr));
  mqattr.mq_maxmsg = 8;
  mqattr.mq_msgsize = sizeof(struct gs1_thread_envelope_s);

  queue->receive_queue = mq_open(queue->queue_name,
                                 O_RDONLY | O_CREAT, 0600, &mqattr);
  if (queue->receive_queue == (mqd_t)-1)
    {
      return -errno;
    }

  queue->send_queue = mq_open(queue->queue_name, O_WRONLY | O_NONBLOCK);
  if (queue->send_queue == (mqd_t)-1)
    {
      ret = -errno;
      mq_close(queue->receive_queue);
      mq_unlink(queue->queue_name);
      return ret;
    }

  pthread_attr_init(&threadattr);
  if (stack_size > 0)
    {
      ret = pthread_attr_setstacksize(&threadattr, stack_size);
      if (ret != 0)
        {
          pthread_attr_destroy(&threadattr);
          mq_close(queue->send_queue);
          mq_close(queue->receive_queue);
          mq_unlink(queue->queue_name);
          return -ret;
        }
    }

  ret = pthread_create(&queue->thread, &threadattr, gs1_thread_entry, queue);
  pthread_attr_destroy(&threadattr);
  if (ret != 0)
    {
      mq_close(queue->send_queue);
      mq_close(queue->receive_queue);
      mq_unlink(queue->queue_name);
      return -ret;
    }

  queue->started = true;
  return 0;
}

int gs1_thread_post(FAR struct gs1_thread_queue_s *queue,
                    FAR const void *payload, size_t payload_size)
{
  struct gs1_thread_envelope_s envelope;

  if (queue == NULL || !queue->started || queue->closing)
    {
      return -ESHUTDOWN;
    }

  if (payload == NULL || payload_size == 0 ||
      payload_size > GS1_THREAD_PAYLOAD_MAX)
    {
      return -EINVAL;
    }

  memset(&envelope, 0, sizeof(envelope));
  envelope.size = payload_size;
  memcpy(envelope.payload, payload, payload_size);
  if (mq_send(queue->send_queue, (FAR const char *)&envelope,
              sizeof(envelope), 0) < 0)
    {
      return errno == EAGAIN ? -EAGAIN : -errno;
    }

  return 0;
}

int gs1_thread_depth(FAR struct gs1_thread_queue_s *queue,
                     FAR uint32_t *depth)
{
  struct mq_attr attr;

  if (queue == NULL || depth == NULL || !queue->started)
    {
      return -EINVAL;
    }

  if (mq_getattr(queue->receive_queue, &attr) < 0)
    {
      return -errno;
    }

  *depth = attr.mq_curmsgs;
  return 0;
}

int gs1_thread_stop(FAR struct gs1_thread_queue_s *queue)
{
  struct gs1_thread_envelope_s envelope;
  int ret;

  if (queue == NULL || !queue->started)
    {
      return 0;
    }

  queue->closing = true;
  memset(&envelope, 0, sizeof(envelope));
  envelope.stop = true;

  do
    {
      ret = mq_send(queue->send_queue, (FAR const char *)&envelope,
                    sizeof(envelope), 0);
      if (ret < 0 && errno == EAGAIN)
        {
          usleep(10000);
        }
    }
  while (ret < 0 && errno == EAGAIN);

  if (ret < 0)
    {
      return -errno;
    }

  ret = pthread_join(queue->thread, NULL);
  mq_close(queue->send_queue);
  mq_close(queue->receive_queue);
  mq_unlink(queue->queue_name);
  queue->started = false;
  return ret == 0 ? 0 : -ret;
}
