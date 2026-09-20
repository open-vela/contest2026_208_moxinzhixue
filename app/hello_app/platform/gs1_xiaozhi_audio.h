/****************************************************************************
 * Contest 2026 team 208 - Gemini-S1 XiaoZhi audio adapter
 ****************************************************************************/

#ifndef __GS1_XIAOZHI_AUDIO_H
#define __GS1_XIAOZHI_AUDIO_H

#include <nuttx/config.h>

#include "xiaozhi_audio.h"

struct gs1_xiaozhi_audio_s;

FAR struct gs1_xiaozhi_audio_s *gs1_xiaozhi_audio_create(void);
void gs1_xiaozhi_audio_destroy(FAR struct gs1_xiaozhi_audio_s *audio);
FAR const struct xiaozhi_audio_ops *gs1_xiaozhi_audio_ops(void);
FAR const char *
gs1_xiaozhi_audio_last_error(FAR const struct gs1_xiaozhi_audio_s *audio);

#endif /* __GS1_XIAOZHI_AUDIO_H */
