// SPDX-License-Identifier: GPL-3.0-or-later
/* The owner banner is optional branding. Without it the route must answer 404
 * so the viewer drops the corner logo instead of showing a broken image. */
#include "common/http/server.h"
#include "banner.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int main(void) {
  const unsigned char sps[] = {0x67, 0x4d, 0x40, 0x28}, pps[] = {0x68, 0xee};
  assert(!pbs_h264_server_start(18095, 1280, 720, sps, sizeof sps, pps, sizeof pps));
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  struct timeval timeout = {.tv_sec = 3};
  assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout));
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(18095),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(!connect(fd, (struct sockaddr *)&addr, sizeof addr));
  const char request[] = "GET /banner.png HTTP/1.1\r\nHost: localhost\r\n\r\n";
  assert(send(fd, request, sizeof request - 1, 0) == sizeof request - 1);
  char response[512] = {0};
  size_t used = 0;
  ssize_t n;
  while ((n = recv(fd, response + used, sizeof response - 1 - used, 0)) > 0) {
    used += (size_t)n;
    if (strstr(response, "\r\n\r\n")) break;
  }
  close(fd);
  pbs_h264_server_stop();
  pbs_h264_server_wait();
  assert(used > 0);
  if (PBS_BANNER_LEN == 0) {
    assert(strstr(response, "404 Not Found"));
    puts("HTTP banner: no branding embedded, /banner.png answers 404 so the viewer shows no logo");
  } else {
    assert(strstr(response, "200 OK") && strstr(response, "image/png"));
    puts("HTTP banner: embedded branding is served as image/png");
  }
  return 0;
}
