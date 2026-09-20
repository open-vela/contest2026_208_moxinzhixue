/****************************************************************************
 * Contest 2026 team 208 - persistent storage service
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_KVDB
#  include <kvdb.h>
#endif

#include "gs1_storage.h"

#define GS1_RECORDINGS_DIR CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_ROOT \
                           "/recordings"
#define GS1_STORAGE_PROBE_FILE ".storage-probe"
#define GS1_RECORD_SEQUENCE_KEY "persist.gs1.record.sequence"

static bool gs1_storage_relative_path_valid(FAR const char *path)
{
  FAR const char *segment;

  if (path == NULL || path[0] == '\0' || path[0] == '/')
    {
      return false;
    }

  segment = path;
  while (*segment != '\0')
    {
      if ((segment[0] == '.' && segment[1] == '.' &&
           (segment[2] == '/' || segment[2] == '\0')) ||
          (segment > path && segment[-1] == '/' &&
           segment[0] == '.' && segment[1] == '.' &&
           (segment[2] == '/' || segment[2] == '\0')))
        {
          return false;
        }

      segment++;
    }

  return true;
}

static int gs1_storage_join(FAR const char *relative_path,
                            FAR char *path, size_t path_size)
{
  int length;

  if (!gs1_storage_relative_path_valid(relative_path))
    {
      return GS1_ERROR_UNSAFE_PATH;
    }

  length = snprintf(path, path_size, "%s/%s",
                    CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_ROOT,
                    relative_path);
  return length >= 0 && (size_t)length < path_size ? 0 : -ENAMETOOLONG;
}

static int gs1_storage_mkdirs(FAR const char *path)
{
  char copy[GS1_PATH_MAX];
  FAR char *cursor;

  if (path == NULL || strlen(path) >= sizeof(copy))
    {
      return -ENAMETOOLONG;
    }

  snprintf(copy, sizeof(copy), "%s", path);
  for (cursor = copy + 1; *cursor != '\0'; cursor++)
    {
      if (*cursor == '/')
        {
          *cursor = '\0';
          if (mkdir(copy, 0750) < 0 && errno != EEXIST)
            {
              return -errno;
            }

          *cursor = '/';
        }
    }

  if (mkdir(copy, 0750) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return 0;
}

static int gs1_storage_parent(FAR const char *path,
                              FAR char *parent, size_t parent_size)
{
  FAR char *slash;

  if (strlen(path) >= parent_size)
    {
      return -ENAMETOOLONG;
    }

  snprintf(parent, parent_size, "%s", path);
  slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent)
    {
      return -EINVAL;
    }

  *slash = '\0';
  return 0;
}

static int gs1_storage_write_all(int fd, FAR const uint8_t *data,
                                 size_t size)
{
  ssize_t written;

  while (size > 0)
    {
      written = write(fd, data, size);
      if (written < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (written == 0)
        {
          return -EIO;
        }

      data += written;
      size -= written;
    }

  return 0;
}

static bool gs1_storage_suffix(FAR const char *name, FAR const char *suffix)
{
  size_t name_len = strlen(name);
  size_t suffix_len = strlen(suffix);

  return name_len >= suffix_len &&
         strcmp(name + name_len - suffix_len, suffix) == 0;
}

static int gs1_storage_scan(FAR struct gs1_storage_status_s *status,
                            bool clean_parts)
{
  FAR DIR *directory;
  FAR struct dirent *entry;
  struct stat file_stat;
  char path[GS1_PATH_MAX];

  status->recording_count = 0;
  status->recording_bytes = 0;
  directory = opendir(GS1_RECORDINGS_DIR);
  if (directory == NULL)
    {
      return -errno;
    }

  while ((entry = readdir(directory)) != NULL)
    {
      if (entry->d_name[0] == '.')
        {
          continue;
        }

      if (snprintf(path, sizeof(path), "%s/%s", GS1_RECORDINGS_DIR,
                   entry->d_name) >= (int)sizeof(path))
        {
          continue;
        }

      if (stat(path, &file_stat) < 0 || !S_ISREG(file_stat.st_mode))
        {
          continue;
        }

      if (gs1_storage_suffix(entry->d_name, ".part"))
        {
          if (clean_parts)
            {
              unlink(path);
            }

          continue;
        }

      if (gs1_storage_suffix(entry->d_name, ".pcm"))
        {
          status->recording_count++;
          status->recording_bytes += file_stat.st_size;
        }
    }

  closedir(directory);
  return 0;
}

int gs1_storage_atomic_write(FAR const char *relative_path,
                             FAR const void *data, size_t size)
{
  char path[GS1_PATH_MAX];
  char temporary[GS1_PATH_MAX];
  char parent[GS1_PATH_MAX];
  int fd = -1;
  int ret;

  if (data == NULL && size > 0)
    {
      return -EINVAL;
    }

  ret = gs1_storage_join(relative_path, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  ret = gs1_storage_parent(path, parent, sizeof(parent));
  if (ret < 0)
    {
      return ret;
    }

  ret = gs1_storage_mkdirs(parent);
  if (ret < 0)
    {
      return ret;
    }

  if (snprintf(temporary, sizeof(temporary), "%s.tmp.%lx", path,
               (unsigned long)getpid()) >= (int)sizeof(temporary))
    {
      return -ENAMETOOLONG;
    }

  fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  ret = gs1_storage_write_all(fd, data, size);
  if (ret == 0 && fsync(fd) < 0)
    {
      ret = -errno;
    }

  if (close(fd) < 0 && ret == 0)
    {
      ret = -errno;
    }

  if (ret == 0 && rename(temporary, path) < 0)
    {
      ret = -errno;
    }

  if (ret < 0)
    {
      unlink(temporary);
      return ret;
    }

  /* YAFFS rename is the commit point.  sync() also covers the directory
   * entry on targets where directory fsync is unavailable.
   */

  sync();
  return 0;
}

int gs1_storage_read(FAR const char *relative_path, FAR void *data,
                     size_t capacity, FAR size_t *size)
{
  char path[GS1_PATH_MAX];
  FAR uint8_t *output = data;
  size_t total = 0;
  ssize_t count;
  int fd;
  int ret;

  if (data == NULL || size == NULL)
    {
      return -EINVAL;
    }

  ret = gs1_storage_join(relative_path, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  while (total < capacity)
    {
      count = read(fd, output + total, capacity - total);
      if (count < 0)
        {
          ret = errno == EINTR ? 0 : -errno;
          if (errno == EINTR)
            {
              continue;
            }

          close(fd);
          return ret;
        }

      if (count == 0)
        {
          break;
        }

      total += count;
    }

  close(fd);
  *size = total;
  return 0;
}

int gs1_storage_query(FAR struct gs1_storage_status_s *status)
{
  struct statfs filesystem;
  int ret;

  if (status == NULL)
    {
      return -EINVAL;
    }

  snprintf(status->root, sizeof(status->root), "%s",
           CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_ROOT);
  if (statfs(status->root, &filesystem) < 0)
    {
      status->state = GS1_STORAGE_UNAVAILABLE;
      status->writable = false;
      status->last_error = -errno;
      return status->last_error;
    }

  status->free_bytes = (uint64_t)filesystem.f_bavail * filesystem.f_bsize;
  ret = gs1_storage_scan(status, false);
  if (ret < 0)
    {
      status->state = GS1_STORAGE_DEGRADED;
      status->last_error = ret;
      return ret;
    }

  status->state = status->writable ? GS1_STORAGE_READY :
                                     GS1_STORAGE_DEGRADED;
  status->last_error = 0;
  return 0;
}

int gs1_storage_probe(FAR struct gs1_storage_status_s *status)
{
  static const char probe[] = "gemini-s1-storage-probe\n";
  char readback[sizeof(probe)];
  char probe_path[GS1_PATH_MAX];
  size_t size;
  int length;
  int ret;

  if (status == NULL)
    {
      return -EINVAL;
    }

  memset(status, 0, sizeof(*status));
  status->state = GS1_STORAGE_CHECKING;
  snprintf(status->root, sizeof(status->root), "%s",
           CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_ROOT);

  ret = gs1_storage_mkdirs(GS1_RECORDINGS_DIR);
  if (ret == 0)
    {
      ret = gs1_storage_atomic_write(GS1_STORAGE_PROBE_FILE,
                                     probe, sizeof(probe));
    }

  if (ret == 0)
    {
      ret = gs1_storage_read(GS1_STORAGE_PROBE_FILE, readback,
                             sizeof(readback), &size);
    }

  if (ret == 0 && (size != sizeof(probe) ||
                   memcmp(readback, probe, sizeof(probe)) != 0))
    {
      ret = GS1_ERROR_DATA_CORRUPT;
    }

  length = snprintf(probe_path, sizeof(probe_path), "%s/%s", status->root,
                    GS1_STORAGE_PROBE_FILE);
  if (length >= 0 && (size_t)length < sizeof(probe_path))
    {
      unlink(probe_path);
    }

  status->writable = ret == 0;

#ifdef CONFIG_KVDB
  if (ret == 0)
    {
      int64_t sequence;
      int kvdb_ret;

      /* Temporary keys are rejected when KVDB temporary storage is off.
       * Probe the persistent namespace without adding a NAND write to each
       * health check.  A missing sequence is valid on a fresh image.
       */

      kvdb_ret = property_get_int64_with_err(GS1_RECORD_SEQUENCE_KEY,
                                              &sequence);
      status->kvdb_available = kvdb_ret >= 0 || kvdb_ret == -ENOENT ||
                               kvdb_ret == -ENODATA;
    }
#endif

  if (ret == 0)
    {
      ret = gs1_storage_scan(status, true);
    }

  if (ret < 0)
    {
      status->state = GS1_STORAGE_UNAVAILABLE;
      status->last_error = ret;
      return ret;
    }

  gs1_storage_prune_recordings(NULL);
  ret = gs1_storage_query(status);
  status->state = status->writable ? GS1_STORAGE_READY :
                                     GS1_STORAGE_DEGRADED;
  status->last_error = ret;
  return ret;
}

int gs1_storage_new_recording(FAR const char *basename,
                              FAR char *part_path, size_t part_size,
                              FAR char *final_path, size_t final_size)
{
  char safe_name[64];
  uint64_t sequence;
  int64_t stored = 0;
  size_t i;
  int length;

  if (part_path == NULL || final_path == NULL)
    {
      return -EINVAL;
    }

  snprintf(safe_name, sizeof(safe_name), "%s",
           basename != NULL && basename[0] != '\0' ? basename : "recording");
  for (i = 0; safe_name[i] != '\0'; i++)
    {
      if (!((safe_name[i] >= 'a' && safe_name[i] <= 'z') ||
            (safe_name[i] >= 'A' && safe_name[i] <= 'Z') ||
            (safe_name[i] >= '0' && safe_name[i] <= '9') ||
            safe_name[i] == '-' || safe_name[i] == '_'))
        {
          safe_name[i] = '_';
        }
    }

#ifdef CONFIG_KVDB
  if (property_get_int64_with_err(GS1_RECORD_SEQUENCE_KEY, &stored) < 0)
    {
      stored = 0;
    }

  stored++;
  if (property_set_int64(GS1_RECORD_SEQUENCE_KEY, stored) == 0)
    {
      property_commit();
    }
  else
    {
      stored = -1;
    }
#else
  stored = 1;
#endif

  sequence = stored > 0 ? (uint64_t)stored :
                          ((uint64_t)time(NULL) << 16) | getpid();
  length = snprintf(final_path, final_size, "%s/%s_%010llu.pcm",
                    GS1_RECORDINGS_DIR, safe_name,
                    (unsigned long long)sequence);
  if (length < 0 || (size_t)length >= final_size)
    {
      return -ENAMETOOLONG;
    }

  length = snprintf(part_path, part_size, "%s.part", final_path);
  if (length < 0 || (size_t)length >= part_size)
    {
      return -ENAMETOOLONG;
    }

  return 0;
}

int gs1_storage_publish_recording(FAR const char *part_path,
                                  FAR const char *final_path)
{
  if (part_path == NULL || final_path == NULL ||
      strncmp(part_path, GS1_RECORDINGS_DIR "/",
              strlen(GS1_RECORDINGS_DIR) + 1) != 0 ||
      strncmp(final_path, GS1_RECORDINGS_DIR "/",
              strlen(GS1_RECORDINGS_DIR) + 1) != 0)
    {
      return GS1_ERROR_UNSAFE_PATH;
    }

  sync();
  if (rename(part_path, final_path) < 0)
    {
      return -errno;
    }

  sync();
  return gs1_storage_prune_recordings(final_path);
}

int gs1_storage_discard_recording(FAR const char *part_path)
{
  if (part_path == NULL ||
      strncmp(part_path, GS1_RECORDINGS_DIR "/",
              strlen(GS1_RECORDINGS_DIR) + 1) != 0 ||
      !gs1_storage_suffix(part_path, ".part"))
    {
      return GS1_ERROR_UNSAFE_PATH;
    }

  return unlink(part_path) < 0 && errno != ENOENT ? -errno : 0;
}

int gs1_storage_prune_recordings(FAR const char *protected_path)
{
  FAR DIR *directory;
  FAR struct dirent *entry;
  struct stat file_stat;
  char path[GS1_PATH_MAX];
  char oldest[GS1_PATH_MAX];
  time_t oldest_time;
  uint32_t count;
  uint64_t bytes;
  bool found;

  for (;;)
    {
      count = 0;
      bytes = 0;
      found = false;
      oldest[0] = '\0';
      oldest_time = 0;
      directory = opendir(GS1_RECORDINGS_DIR);
      if (directory == NULL)
        {
          return -errno;
        }

      while ((entry = readdir(directory)) != NULL)
        {
          if (!gs1_storage_suffix(entry->d_name, ".pcm") ||
              snprintf(path, sizeof(path), "%s/%s", GS1_RECORDINGS_DIR,
                       entry->d_name) >= (int)sizeof(path) ||
              stat(path, &file_stat) < 0 || !S_ISREG(file_stat.st_mode))
            {
              continue;
            }

          count++;
          bytes += file_stat.st_size;
          if ((protected_path == NULL || strcmp(path, protected_path) != 0) &&
              (!found || file_stat.st_mtime < oldest_time))
            {
              found = true;
              oldest_time = file_stat.st_mtime;
              snprintf(oldest, sizeof(oldest), "%s", path);
            }
        }

      closedir(directory);
      if (count <= CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_RECORD_MAX_FILES &&
          bytes <= CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_RECORD_MAX_BYTES)
        {
          return 0;
        }

      if (!found)
        {
          return -ENOSPC;
        }

      if (unlink(oldest) < 0)
        {
          return -errno;
        }

      sync();
    }
}
