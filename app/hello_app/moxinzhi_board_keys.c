/****************************************************************************
 * Contest 2026 team 208 - Gemini-S1 board key input
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "moxinzhi_board_keys.h"

#define MZ_BOARD_PTT_HOLD_MS          450u
#define MZ_BOARD_VOLUME_HOLD_MS       400u
#define MZ_BOARD_VOLUME_REPEAT_MS     120u
#define MZ_BOARD_VOLUME_STEP          5

static bool mz_board_time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
  return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t mz_board_volume_key(uint32_t state)
{
  uint32_t key = state & (MZ_BOARD_KEY_VOLUME_UP |
                          MZ_BOARD_KEY_VOLUME_DOWN);

  return key == MZ_BOARD_KEY_VOLUME_UP ||
         key == MZ_BOARD_KEY_VOLUME_DOWN ? key : 0;
}

static int mz_board_volume_delta(uint32_t key)
{
  return key == MZ_BOARD_KEY_VOLUME_UP ? MZ_BOARD_VOLUME_STEP :
         key == MZ_BOARD_KEY_VOLUME_DOWN ? -MZ_BOARD_VOLUME_STEP : 0;
}

static void mz_board_keys_set_state(struct mz_board_keys_s *keys,
                                    uint32_t state, uint32_t now_ms,
                                    struct mz_board_key_events_s *events)
{
  uint32_t old_state = keys->state;
  uint32_t volume_key;

  keys->state = state;

  if ((state & MZ_BOARD_KEY_ENTER) != 0 &&
      (old_state & MZ_BOARD_KEY_ENTER) == 0)
    {
      keys->enter_pressed_ms = now_ms;
      keys->ptt_triggered = false;
    }
  else if ((state & MZ_BOARD_KEY_ENTER) == 0 &&
           (old_state & MZ_BOARD_KEY_ENTER) != 0)
    {
      if (keys->ptt_triggered)
        {
          events->ptt_release = true;
        }

      keys->ptt_triggered = false;
    }

  volume_key = mz_board_volume_key(state);
  if (volume_key != keys->volume_key)
    {
      keys->volume_key = volume_key;
      if (volume_key != 0)
        {
          events->volume_delta += mz_board_volume_delta(volume_key);
          keys->volume_repeat_ms = now_ms + MZ_BOARD_VOLUME_HOLD_MS;
        }
    }
}

static int mz_board_keys_read(struct mz_board_keys_s *keys,
                              uint32_t *state, bool *updated)
{
  struct pollfd pfd;
  ssize_t nread;
  int ret;

  *updated = false;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = keys->fd;
  pfd.events = POLLIN;

  ret = poll(&pfd, 1, 0);
  if (ret < 0)
    {
      return errno == EINTR ? 0 : -errno;
    }

  if (ret == 0)
    {
      return 0;
    }

  if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
      return -EIO;
    }

  if ((pfd.revents & POLLIN) == 0)
    {
      return 0;
    }

  nread = read(keys->fd, state, sizeof(*state));
  if (nread < 0)
    {
      return errno == EINTR || errno == EAGAIN ? 0 : -errno;
    }

  if (nread != sizeof(*state))
    {
      return -EIO;
    }

  *updated = true;
  return 0;
}

int mz_board_keys_init(struct mz_board_keys_s *keys)
{
  if (keys == NULL)
    {
      return -EINVAL;
    }

  memset(keys, 0, sizeof(*keys));
  keys->fd = open(MZ_BOARD_KEYS_DEVPATH, O_RDONLY | O_NONBLOCK);
  if (keys->fd < 0)
    {
      return -errno;
    }

  return 0;
}

int mz_board_keys_poll(struct mz_board_keys_s *keys, uint32_t now_ms,
                       struct mz_board_key_events_s *events)
{
  uint32_t state = 0;
  bool updated;
  int ret;

  if (keys == NULL || events == NULL)
    {
      return -EINVAL;
    }

  memset(events, 0, sizeof(*events));
  if (keys->fd < 0)
    {
      return -ENODEV;
    }

  ret = mz_board_keys_read(keys, &state, &updated);
  if (ret < 0)
    {
      if (keys->ptt_triggered)
        {
          events->ptt_release = true;
        }

      close(keys->fd);
      keys->fd = -1;
      keys->state = 0;
      keys->volume_key = 0;
      keys->ptt_triggered = false;
      return ret;
    }

  if (updated && state != keys->state)
    {
      mz_board_keys_set_state(keys, state, now_ms, events);
    }

  if ((keys->state & MZ_BOARD_KEY_ENTER) != 0 &&
      !keys->ptt_triggered &&
      (uint32_t)(now_ms - keys->enter_pressed_ms) >=
        MZ_BOARD_PTT_HOLD_MS)
    {
      keys->ptt_triggered = true;
      events->ptt_press = true;
    }

  if (keys->volume_key != 0 &&
      mz_board_time_reached(now_ms, keys->volume_repeat_ms))
    {
      events->volume_delta += mz_board_volume_delta(keys->volume_key);
      keys->volume_repeat_ms = now_ms + MZ_BOARD_VOLUME_REPEAT_MS;
    }

  return 0;
}

void mz_board_keys_deinit(struct mz_board_keys_s *keys)
{
  if (keys == NULL)
    {
      return;
    }

  if (keys->fd >= 0)
    {
      close(keys->fd);
    }

  memset(keys, 0, sizeof(*keys));
  keys->fd = -1;
}
