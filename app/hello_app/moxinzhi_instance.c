/****************************************************************************
 * Contest 2026 team 208 - cross-task single instance guard
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "moxinzhi_instance.h"

#define MZ_INSTANCE_LOCK_PATH "/tmp/lvgl_screen_test.lock"
#define MZ_INSTANCE_RETRIES   3

static int mz_instance_read_owner(FAR pid_t *owner)
{
  char buffer[24];
  FAR char *endptr;
  long value;
  ssize_t length;
  int fd;

  fd = open(MZ_INSTANCE_LOCK_PATH, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  length = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);
  if (length <= 0)
    {
      return length < 0 ? -errno : -EAGAIN;
    }

  buffer[length] = '\0';
  errno = 0;
  value = strtol(buffer, &endptr, 10);
  if (errno != 0 || endptr == buffer || value <= 0)
    {
      return -EINVAL;
    }

  *owner = (pid_t)value;
  return 0;
}

int mz_instance_acquire(FAR struct mz_instance_guard_s *guard)
{
  char buffer[24];
  pid_t current;
  pid_t owner;
  ssize_t expected;
  ssize_t written;
  int attempt;
  int ret;

  if (guard == NULL)
    {
      return -EINVAL;
    }

  memset(guard, 0, sizeof(*guard));
  guard->fd = -1;
  current = getpid();

  for (attempt = 0; attempt < MZ_INSTANCE_RETRIES; attempt++)
    {
      guard->fd = open(MZ_INSTANCE_LOCK_PATH,
                       O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (guard->fd >= 0)
        {
          expected = snprintf(buffer, sizeof(buffer), "%ld\n",
                              (long)current);
          written = write(guard->fd, buffer, expected);
          if (written != expected)
            {
              ret = written < 0 ? -errno : -EIO;
              close(guard->fd);
              guard->fd = -1;
              unlink(MZ_INSTANCE_LOCK_PATH);
              return ret;
            }

          fsync(guard->fd);
          guard->owner = current;
          guard->held = true;
          return 0;
        }

      if (errno != EEXIST)
        {
          return -errno;
        }

      ret = mz_instance_read_owner(&owner);
      if (ret < 0)
        {
          /* A creator may be between open() and write().  Never remove an
           * ownerless lock here because doing so could admit two live UIs.
           */

          return -EBUSY;
        }

      if (owner != current &&
          (kill(owner, 0) == 0 || errno == EPERM))
        {
          guard->owner = owner;
          return -EBUSY;
        }

      /* The PID is gone, or this process reused a stale PID. */

      if (unlink(MZ_INSTANCE_LOCK_PATH) < 0 && errno != ENOENT)
        {
          return -errno;
        }
    }

  return -EBUSY;
}

void mz_instance_release(FAR struct mz_instance_guard_s *guard)
{
  pid_t owner;

  if (guard == NULL || !guard->held)
    {
      return;
    }

  if (guard->fd >= 0)
    {
      close(guard->fd);
    }

  if (mz_instance_read_owner(&owner) == 0 && owner == guard->owner)
    {
      unlink(MZ_INSTANCE_LOCK_PATH);
    }

  memset(guard, 0, sizeof(*guard));
  guard->fd = -1;
}
