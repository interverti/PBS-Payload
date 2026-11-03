// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/http/server.h"
#include "common/input/controller.h"
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static pthread_mutex_t mock_lock = PTHREAD_MUTEX_INITIALIZER;
static pbs_input_state last_state;
static int create_count, remove_count, write_error, remove_error;
static char response[4096];
static int create_pad(void *unused) {
  (void)unused;
  create_count++;
  return 0;
}
static int write_pad(void *unused, const pbs_input_state *state) {
  (void)unused;
  pthread_mutex_lock(&mock_lock);
  last_state = *state;
  int rc = write_error;
  pthread_mutex_unlock(&mock_lock);
  return rc;
}
static int remove_pad(void *unused) {
  (void)unused;
  pthread_mutex_lock(&mock_lock);
  remove_count++;
  int rc = remove_error;
  pthread_mutex_unlock(&mock_lock);
  return rc;
}
static void request(const char *method, const char *path, const char *token, const char *body,
                    const char *extra, int code, int fragmented) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  struct sockaddr_in addr = {
      .sin_family = AF_INET, .sin_port = htons(18098), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(!connect(fd, (struct sockaddr *)&addr, sizeof addr));
  char headers[1024];
  int n =
      snprintf(headers, sizeof headers,
               "%s %s HTTP/1.1\r\nHost: localhost\r\n%s%s%s%sContent-Length: %zu\r\n%s\r\n", method,
               path, token ? "X-PBS-Input: 1\r\n" : "", token && *token ? "X-PBS-Controller: " : "",
               token && *token ? token : "", token && *token ? "\r\n" : "", strlen(body), extra);
  assert(send(fd, headers, (size_t)n, 0) == n);
  if (fragmented) {
    usleep(20000);
  }
  if (*body) {
    assert(send(fd, body, strlen(body), 0) == (ssize_t)strlen(body));
  }
  shutdown(fd, SHUT_WR);
  size_t used = 0;
  ssize_t got;
  while ((got = recv(fd, response + used, sizeof response - 1 - used, 0)) > 0) {
    used += (size_t)got;
  }
  response[used] = 0;
  close(fd);
  char expected[40];
  snprintf(expected, sizeof expected, "HTTP/1.1 %d", code);
  if (!strstr(response, expected)) {
    fprintf(stderr, "Expected %s; got %s\n", expected, response);
  }
  assert(strstr(response, expected));
}
static void owner(char token[PBS_INPUT_TOKEN_SIZE]) {
  request("POST", "/input/connect", "", "", "", 200, 0);
  const char *start = strstr(response, "\"token\":\"");
  assert(start);
  memcpy(token, start + 9, 32);
  token[32] = 0;
}
static pbs_input_state snapshot(void) {
  pthread_mutex_lock(&mock_lock);
  pbs_input_state state = last_state;
  pthread_mutex_unlock(&mock_lock);
  return state;
}
int main(void) {
  static const pbs_input_backend backend = {create_pad, write_pad, remove_pad};
  const unsigned char sps[] = {0x67, 0x4d, 0x40, 0x28}, pps[] = {0x68, 0xee, 0x3c, 0x80};
  char token[PBS_INPUT_TOKEN_SIZE], old[PBS_INPUT_TOKEN_SIZE];
  assert(pbs_input_connect(token) == 501);
  pbs_input_configure(&backend, NULL);
  assert(!pbs_h264_server_start(18098, 1920, 1080, sps, sizeof sps, pps, sizeof pps));
  request("GET", "/input/status", NULL, "", "", 200, 0);
  assert(strstr(response, "\"supported\":true") && !strstr(response, "token"));
  request("POST", "/input/connect", NULL, "", "", 403, 0);
  request("POST", "/input/connect", "", "", "X-PBS-Input: 1\r\n", 403, 0);
  request("POST", "/input/connect", "", "", "Transfer-Encoding: chunked\r\n", 400, 0);
  request("POST", "/input/connect", "", "", "Content-Length: 0\r\n", 400, 0);
  request("GET", "/input/connect", NULL, "", "", 405, 0);
  request("POST", "/input/unknown", "", "", "", 404, 0);
  owner(token);
  strcpy(old, token);
  request("POST", "/input/connect", "", "", "", 409, 0);
  request("POST", "/input/state", "00000000000000000000000000000000", "7,16384,128,128,128,128,0,0",
          "", 403, 0);
  request("POST", "/input/state", token, "7,16384,256,128,128,128,0,0", "", 400, 0);
  request("POST", "/input/state", token, "7,1,128,128,128,128,0,0", "", 400, 0);
  request("POST", "/input/state", token, "7,16384,128,128,128,128,0,0,9", "", 400, 0);
  request("POST", "/input/state", token, "7,16384,128,128,128,128,0,0", "", 200, 1);
  usleep(20000);
  assert(snapshot().buttons == 0x4000);
  request("POST", "/input/state", token, "7,0,128,128,128,128,0,0", "", 409, 0);
  request("POST", "/input/state", token, "6,0,128,128,128,128,0,0", "", 409, 0);
  usleep(300000);
  assert(snapshot().buttons == 0 && snapshot().lx == 128);
  request("GET", "/input/status", NULL, "", "", 200, 0);
  assert(strstr(response, "\"connected\":true"));
  request("POST", "/input/state", token, "8,16384,0,255,128,128,255,0", "", 200, 0);
  usleep(2150000);
  request("GET", "/input/status", NULL, "", "", 200, 0);
  assert(strstr(response, "\"connected\":false") && remove_count == 1);
  owner(token);
  assert(strcmp(old, token));
  request("POST", "/input/disconnect", old, "", "", 403, 0);
  pbs_h264_discontinuity();
  request("GET", "/input/status", NULL, "", "", 200, 0);
  assert(strstr(response, "\"connected\":true"));
  request("POST", "/input/state", token, "4294967295,16384,128,128,128,128,0,0", "", 200, 0);
  request("POST", "/input/state", token, "0,0,128,128,128,128,0,0", "", 200, 0);
  request("POST", "/input/disconnect", token, "", "", 200, 0);
  assert(snapshot().buttons == 0 && remove_count == 2);
  owner(token);
  request("POST", "/stop", NULL, "", "", 200, 0);
  pbs_h264_server_wait();
  assert(remove_count == 3);
  assert(!pbs_h264_server_start(18098, 1920, 1080, sps, sizeof sps, pps, sizeof pps));
  owner(token);
  pthread_mutex_lock(&mock_lock);
  write_error = -47;
  pthread_mutex_unlock(&mock_lock);
  request("POST", "/input/state", token, "1,16384,128,128,128,128,0,0", "", 200, 0);
  usleep(20000);
  request("GET", "/input/status", NULL, "", "", 200, 0);
  assert(strstr(response, "\"connected\":false") && strstr(response, "\"last_rc\":-47"));
  pthread_mutex_lock(&mock_lock);
  write_error = 0;
  pthread_mutex_unlock(&mock_lock);
  owner(token);
  pthread_mutex_lock(&mock_lock);
  remove_error = -99;
  int before = remove_count;
  pthread_mutex_unlock(&mock_lock);
  request("POST", "/input/disconnect", token, "", "", 503, 0);
  request("POST", "/input/connect", "", "", "", 409, 0);
  request("GET", "/input/status", NULL, "", "", 200, 0);
  assert(strstr(response, "\"cleanup_required\":true"));
  pbs_h264_server_stop();
  pbs_h264_server_wait();
  assert(remove_count == before + 1);
  assert(create_count == 5);
  puts("Controller HTTP ownership, framing, sequence wrap, watchdog, shutdown and failure cleanup "
       "verified");
  return 0;
}
