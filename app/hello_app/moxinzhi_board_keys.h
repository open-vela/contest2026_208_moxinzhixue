/****************************************************************************
 * Contest 2026 team 208 - Gemini-S1 board key input
 ****************************************************************************/

#ifndef __MOXINZHI_BOARD_KEYS_H
#define __MOXINZHI_BOARD_KEYS_H

#include <stdbool.h>
#include <stdint.h>

#define MZ_BOARD_KEYS_DEVPATH       "/dev/input/event1"

#define MZ_BOARD_KEY_VOLUME_DOWN    0x01u
#define MZ_BOARD_KEY_VOLUME_UP      0x02u
#define MZ_BOARD_KEY_MENU           0x04u
#define MZ_BOARD_KEY_ENTER          0x08u
#define MZ_BOARD_KEY_HOME           0x10u

struct mz_board_key_events_s
{
  int volume_delta;
  bool ptt_press;
  bool ptt_release;
};

struct mz_board_keys_s
{
  int fd;
  uint32_t state;
  uint32_t enter_pressed_ms;
  uint32_t volume_repeat_ms;
  uint32_t volume_key;
  bool ptt_triggered;
};

int mz_board_keys_init(struct mz_board_keys_s *keys);
int mz_board_keys_poll(struct mz_board_keys_s *keys, uint32_t now_ms,
                       struct mz_board_key_events_s *events);
void mz_board_keys_deinit(struct mz_board_keys_s *keys);

#endif /* __MOXINZHI_BOARD_KEYS_H */
