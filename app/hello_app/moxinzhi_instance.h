/****************************************************************************
 * Contest 2026 team 208 - cross-task single instance guard
 ****************************************************************************/

#ifndef __MOXINZHI_INSTANCE_H
#define __MOXINZHI_INSTANCE_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <sys/types.h>

struct mz_instance_guard_s
{
  int fd;
  pid_t owner;
  bool held;
};

int mz_instance_acquire(FAR struct mz_instance_guard_s *guard);
void mz_instance_release(FAR struct mz_instance_guard_s *guard);

#endif /* __MOXINZHI_INSTANCE_H */
