/****************************************************************************
 * Contest 2026 team 208 - KVDB persistence adapter
 ****************************************************************************/

#ifndef __MOXINZHI_STORAGE_H
#define __MOXINZHI_STORAGE_H

#include <nuttx/config.h>

#include <stdint.h>

#include "moxinzhi_model.h"

int mz_storage_load(FAR struct mz_model_meta_s *meta,
                    FAR struct mz_card_s cards[MZ_CARD_CAPACITY]);
int mz_storage_save_meta(FAR const struct mz_model_meta_s *meta);
int mz_storage_save_card(uint8_t slot, FAR struct mz_card_s *card);
int mz_storage_reset(void);
FAR const char *mz_storage_backend_path(void);

#endif /* __MOXINZHI_STORAGE_H */
