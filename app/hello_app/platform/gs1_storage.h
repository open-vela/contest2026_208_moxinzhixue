/****************************************************************************
 * Contest 2026 team 208 - persistent storage service
 ****************************************************************************/

#ifndef __GS1_STORAGE_H
#define __GS1_STORAGE_H

#include "gs1_platform.h"

int gs1_storage_probe(FAR struct gs1_storage_status_s *status);
int gs1_storage_query(FAR struct gs1_storage_status_s *status);
int gs1_storage_atomic_write(FAR const char *relative_path,
                             FAR const void *data, size_t size);
int gs1_storage_read(FAR const char *relative_path, FAR void *data,
                     size_t capacity, FAR size_t *size);
int gs1_storage_new_recording(FAR const char *basename,
                              FAR char *part_path, size_t part_size,
                              FAR char *final_path, size_t final_size);
int gs1_storage_publish_recording(FAR const char *part_path,
                                  FAR const char *final_path);
int gs1_storage_discard_recording(FAR const char *part_path);
int gs1_storage_prune_recordings(FAR const char *protected_path);

#endif /* __GS1_STORAGE_H */
