/****************************************************************************
 * Contest 2026 team 208 - official Xiaozhi pairing CLI
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cloud_client.h"
#include "xiaozhi_provisioning.h"

#define XIAOZHI_PAIR_DEFAULT_TIMEOUT_SECONDS 180
#define XIAOZHI_PAIR_MAX_TIMEOUT_SECONDS 1800
#define XIAOZHI_PAIR_LOOP_MS 100

struct xiaozhi_pair_cli_context
{
  char last_code[XIAOZHI_OTA_VERSION_MAX];
};

static uint64_t xiaozhi_pair_now_ms(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000 +
         (uint64_t)now.tv_nsec / 1000000;
}

static void xiaozhi_pair_usage(const char *program)
{
  printf("Usage: %s [--status] [--reset] [--timeout seconds]\n",
         program);
  printf("  no option   Pair with the official Xiaozhi server\n");
  printf("  --status    Print local pairing state without showing token\n");
  printf("  --reset     Forget cached cloud credentials, then pair again\n");
  printf("\nNo server token or device identity is accepted on the command line.\n");
}

static int xiaozhi_pair_parse_timeout(const char *text,
                                      uint32_t *timeout_ms)
{
  char *end;
  unsigned long seconds;

  errno = 0;
  seconds = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || seconds == 0 ||
      seconds > XIAOZHI_PAIR_MAX_TIMEOUT_SECONDS)
    {
      return -EINVAL;
    }

  *timeout_ms = (uint32_t)seconds * 1000;
  return 0;
}

static void xiaozhi_pair_callback(
    void *user, const struct xiaozhi_pairing_snapshot *snapshot)
{
  struct xiaozhi_pair_cli_context *context = user;

  printf("xiaozhi_pair: state=%s\n",
         xiaozhi_pairing_state_name(snapshot->state));
  if (snapshot->pairing_code[0] != '\0' &&
      strcmp(context->last_code, snapshot->pairing_code) != 0)
    {
      snprintf(context->last_code, sizeof(context->last_code), "%s",
               snapshot->pairing_code);
      printf("xiaozhi_pair: pairing code: %s\n", snapshot->pairing_code);
      if (snapshot->pairing_message[0] != '\0')
        {
          printf("xiaozhi_pair: %s\n", snapshot->pairing_message);
        }
    }

  if (snapshot->state == XIAOZHI_PAIRING_RETRY_WAIT)
    {
      fprintf(stderr, "xiaozhi_pair: temporary error %d: %s\n",
              snapshot->last_error, snapshot->last_error_text);
    }
  else if (snapshot->state == XIAOZHI_PAIRING_ERROR)
    {
      fprintf(stderr, "xiaozhi_pair: failed %d: %s\n",
              snapshot->last_error, snapshot->last_error_text);
    }
}

int main(int argc, char *argv[])
{
  struct xiaozhi_provisioning_config config;
  struct xiaozhi_pairing_snapshot snapshot;
  struct xiaozhi_pair_cli_context context;
  struct xiaozhi_provisioning *provisioning = NULL;
  uint32_t timeout_ms = XIAOZHI_PAIR_DEFAULT_TIMEOUT_SECONDS * 1000;
  uint64_t deadline_ms;
  bool status_only = false;
  bool reset = false;
  bool curl_initialized = false;
  int exit_status = EXIT_FAILURE;
  int ret;
  int i;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
          xiaozhi_pair_usage(argv[0]);
          return EXIT_SUCCESS;
        }
      else if (strcmp(argv[i], "--status") == 0)
        {
          status_only = true;
        }
      else if (strcmp(argv[i], "--reset") == 0)
        {
          reset = true;
        }
      else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
        {
          if (xiaozhi_pair_parse_timeout(argv[++i], &timeout_ms) < 0)
            {
              fprintf(stderr, "xiaozhi_pair: invalid timeout\n");
              return EXIT_FAILURE;
            }
        }
      else
        {
          fprintf(stderr, "xiaozhi_pair: unknown option: %s\n", argv[i]);
          xiaozhi_pair_usage(argv[0]);
          return EXIT_FAILURE;
        }
    }

  memset(&config, 0, sizeof(config));
  memset(&context, 0, sizeof(context));
#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_ALLOW_PLAINTEXT
  config.allow_plaintext = true;
#endif

  provisioning = xiaozhi_provisioning_create(
      &config, xiaozhi_pair_callback, &context);
  if (provisioning == NULL)
    {
      fprintf(stderr, "xiaozhi_pair: invalid provisioning configuration\n");
      goto cleanup;
    }

  ret = xiaozhi_provisioning_start(provisioning, !status_only,
                                   xiaozhi_pair_now_ms());
  if (ret < 0)
    {
      goto cleanup;
    }

  if (reset)
    {
      ret = xiaozhi_provisioning_clear_credentials(provisioning);
      if (ret < 0)
        {
          fprintf(stderr, "xiaozhi_pair: unable to clear credentials: %d\n",
                  ret);
          goto cleanup;
        }

      ret = xiaozhi_provisioning_start(provisioning, true,
                                       xiaozhi_pair_now_ms());
      if (ret < 0)
        {
          goto cleanup;
        }
    }

  if (xiaozhi_provisioning_get_snapshot(provisioning, &snapshot) < 0)
    {
      goto cleanup;
    }

  printf("xiaozhi_pair: device-id=%s client-id=%s cached=%u\n",
         snapshot.identity.device_id, snapshot.identity.client_id,
         (unsigned int)snapshot.has_credentials);
  if (status_only)
    {
      printf("xiaozhi_pair: local state=%s\n",
             xiaozhi_pairing_state_name(snapshot.state));
      exit_status = EXIT_SUCCESS;
      goto cleanup;
    }

  ret = cloud_client_global_init();
  if (ret < 0)
    {
      fprintf(stderr, "xiaozhi_pair: curl initialization failed: %d\n", ret);
      goto cleanup;
    }

  curl_initialized = true;
  deadline_ms = xiaozhi_pair_now_ms() + timeout_ms;
  while (xiaozhi_pair_now_ms() < deadline_ms)
    {
      ret = xiaozhi_provisioning_poll(provisioning,
                                      xiaozhi_pair_now_ms());
      if (ret < 0 && ret != -EAGAIN)
        {
          if (xiaozhi_provisioning_get_snapshot(provisioning,
                                                 &snapshot) < 0 ||
              snapshot.state == XIAOZHI_PAIRING_ERROR)
            {
              goto cleanup;
            }
        }

      if (xiaozhi_provisioning_get_snapshot(provisioning, &snapshot) < 0)
        {
          goto cleanup;
        }

      if (snapshot.state == XIAOZHI_PAIRING_READY)
        {
          printf("xiaozhi_pair: paired; WSS credentials stored securely\n");
          exit_status = EXIT_SUCCESS;
          goto cleanup;
        }

      usleep(XIAOZHI_PAIR_LOOP_MS * 1000);
    }

  fprintf(stderr, "xiaozhi_pair: timed out waiting for pairing\n");

cleanup:
  xiaozhi_provisioning_destroy(provisioning);
  if (curl_initialized)
    {
      cloud_client_global_cleanup();
    }

  return exit_status;
}
