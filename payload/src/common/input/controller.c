// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/input/controller.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PBS_INPUT_RELEASE_MS
#define PBS_INPUT_RELEASE_MS 250
#endif
#ifndef PBS_INPUT_DISCONNECT_MS
#define PBS_INPUT_DISCONNECT_MS 2000
#endif
static struct {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  const pbs_input_backend *backend;
  void *opaque;
  pthread_t thread;
  int running, thread_valid, owned, active, pending, neutral, seen_sequence;
  int cleanup_required, last_rc;
  uint32_t sequence;
  uint64_t received, packets;
  char token[PBS_INPUT_TOKEN_SIZE];
  pbs_input_state state;
} input = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
static const pbs_input_state released = {.lx = 128, .ly = 128, .rx = 128, .ry = 128};
static uint64_t clock_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int owns(const char *token) {
  if (!token || strlen(token) != 32) {
    return 0;
  }
  unsigned different = 0;
  for (unsigned i = 0; i < 32; i++) {
    different |= (unsigned char)token[i] ^ (unsigned char)input.token[i];
  }
  return !different;
}
/* Called with input.lock held. Never delete a device owned by another session. */
static int release_owned(void) {
  input.active = input.pending = 0;
  if (!input.owned) {
    return 0;
  }
  (void)input.backend->write(input.opaque, &released);
  int rc = input.backend->disconnect(input.opaque);
  input.last_rc = rc;
  input.cleanup_required = rc != 0;
  if (!rc) {
    input.owned = 0;
    memset(input.token, 0, sizeof input.token);
  }
  return rc;
}
static void *input_thread(void *unused) {
  (void)unused;
  pthread_mutex_lock(&input.lock);
  while (input.running) {
    if (input.active) {
      uint64_t age = clock_ms() - input.received;
      if (age >= PBS_INPUT_DISCONNECT_MS) {
        release_owned();
      } else if (age >= PBS_INPUT_RELEASE_MS && !input.neutral) {
        int rc = input.backend->write(input.opaque, &released);
        input.last_rc = rc;
        input.neutral = 1;
        input.pending = 0;
        if (rc) {
          release_owned();
          input.last_rc = rc;
        }
      } else if (input.pending) {
        int rc = input.backend->write(input.opaque, &input.state);
        input.last_rc = rc;
        input.pending = 0;
        input.neutral = 0;
        if (rc) {
          release_owned();
          input.last_rc = rc;
        }
      }
    }
    if (!input.running) {
      break;
    }
    if (!input.active) {
      pthread_cond_wait(&input.changed, &input.lock);
    } else {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += 8000000;
      if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&input.changed, &input.lock, &deadline);
    }
  }
  if (!input.cleanup_required) {
    release_owned();
  }
  pthread_mutex_unlock(&input.lock);
  return NULL;
}
void pbs_input_configure(const pbs_input_backend *backend, void *opaque) {
  pthread_mutex_lock(&input.lock);
  if (!input.thread_valid && !input.owned) {
    input.backend = backend;
    input.opaque = opaque;
  }
  pthread_mutex_unlock(&input.lock);
}
int pbs_input_start(void) {
  int rc = 0;
  pthread_mutex_lock(&input.lock);
  if (input.backend && !input.thread_valid) {
    input.running = 1;
    rc = pthread_create(&input.thread, NULL, input_thread, NULL);
    if (rc) {
      input.running = 0;
    } else {
      input.thread_valid = 1;
    }
  }
  pthread_mutex_unlock(&input.lock);
  return rc;
}
void pbs_input_stop(void) {
  pthread_mutex_lock(&input.lock);
  input.running = 0;
  pthread_cond_broadcast(&input.changed);
  pthread_mutex_unlock(&input.lock);
}
void pbs_input_wait(void) {
  if (input.thread_valid) {
    pthread_join(input.thread, NULL);
    input.thread_valid = 0;
  }
}
static int new_token(char token[PBS_INPUT_TOKEN_SIZE]) {
  unsigned char random[16];
  size_t used = 0;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  while (used < sizeof random) {
    ssize_t count = read(fd, random + used, sizeof random - used);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      close(fd);
      return -1;
    }
    used += (size_t)count;
  }
  close(fd);
  const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 16; i++) {
    token[i * 2] = hex[random[i] >> 4];
    token[i * 2 + 1] = hex[random[i] & 15];
  }
  token[32] = 0;
  return 0;
}
int pbs_input_connect(char token[PBS_INPUT_TOKEN_SIZE]) {
  int code = 200;
  pthread_mutex_lock(&input.lock);
  if (!input.backend) {
    code = 501;
  } else if (!input.running || input.owned || input.cleanup_required) {
    code = 409;
  } else if (new_token(input.token)) {
    code = 503;
  } else {
    /* Even a partially successful backend acquisition must be unwound. */
    input.owned = 1;
    int rc = input.backend->connect(input.opaque);
    if (!rc) {
      rc = input.backend->write(input.opaque, &released);
    }
    input.last_rc = rc;
    if (rc) {
      release_owned();
      input.last_rc = rc;
      code = 503;
    } else {
      input.active = 1;
      input.received = clock_ms();
      input.neutral = 1;
      input.pending = input.seen_sequence = 0;
      input.packets = 0;
      memcpy(token, input.token, PBS_INPUT_TOKEN_SIZE);
      pthread_cond_broadcast(&input.changed);
    }
  }
  pthread_mutex_unlock(&input.lock);
  return code;
}
int pbs_input_submit(const char *token, uint32_t sequence, const pbs_input_state *state) {
  if (!state || (state->buttons & ~PBS_INPUT_BUTTONS)) {
    return 400;
  }
  int code = 200;
  pthread_mutex_lock(&input.lock);
  if (!owns(token)) {
    code = 403;
  } else if (!input.active || !input.running) {
    code = 409;
  } else if (input.seen_sequence && (int32_t)(sequence - input.sequence) <= 0) {
    code = 409;
  } else {
    input.sequence = sequence;
    input.seen_sequence = 1;
    input.state = *state;
    input.received = clock_ms();
    input.packets++;
    input.pending = 1;
    pthread_cond_broadcast(&input.changed);
  }
  pthread_mutex_unlock(&input.lock);
  return code;
}
int pbs_input_disconnect(const char *token) {
  pthread_mutex_lock(&input.lock);
  int code = !owns(token) ? 403 : release_owned() ? 503 : 200;
  pthread_mutex_unlock(&input.lock);
  return code;
}
void pbs_input_get_status(pbs_input_status *status) {
  pthread_mutex_lock(&input.lock);
  *status = (pbs_input_status){.supported = input.backend != NULL,
                               .connected = input.active,
                               .cleanup_required = input.cleanup_required,
                               .last_rc = input.last_rc,
                               .packets = input.packets};
  pthread_mutex_unlock(&input.lock);
}
int pbs_input_parse_state(const char *text, uint32_t *sequence, pbs_input_state *state) {
  uint32_t values[8];
  if (!text || !sequence || !state) {
    return -1;
  }
  for (unsigned i = 0; i < 8; i++) {
    if (*text < '0' || *text > '9') {
      return -1;
    }
    errno = 0;
    char *end;
    unsigned long long v = strtoull(text, &end, 10);
    if (errno || v > (i < 2 ? UINT32_MAX : 255)) {
      return -1;
    }
    values[i] = (uint32_t)v;
    if (i < 7) {
      if (*end != ',') {
        return -1;
      }
      text = end + 1;
    } else if (*end) {
      return -1;
    }
  }
  if (values[1] & ~PBS_INPUT_BUTTONS) {
    return -1;
  }
  *sequence = values[0];
  *state = (pbs_input_state){.buttons = values[1],
                             .lx = values[2],
                             .ly = values[3],
                             .rx = values[4],
                             .ry = values[5],
                             .l2 = values[6],
                             .r2 = values[7]};
  return 0;
}
