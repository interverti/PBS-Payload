// SPDX-License-Identifier: GPL-3.0-or-later
/* The advertised port may already be owned by a previous payload instance.
 * The listener must walk PBS_H264_PORT_SCAN candidates instead of failing the
 * whole session, and must report the port it actually bound. */
#include "common/http/server.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE_PORT 19030

static int listen_on(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(!bind(fd, (struct sockaddr *)&addr, sizeof addr));
  assert(!listen(fd, 4));
  return fd;
}

int main(void) {
  const unsigned char sps[] = {0x67, 0x4d, 0x40, 0x28}, pps[] = {0x68, 0xee, 0x3c, 0x80};
  int blocker_a = listen_on(BASE_PORT);
  int blocker_b = listen_on(BASE_PORT + 1);

  /* Two occupied candidates: the third one must win. */
  assert(!pbs_h264_server_start(BASE_PORT, 1280, 720, sps, sizeof sps, pps, sizeof pps));
  assert(pbs_h264_server_port() == BASE_PORT + 2);

  /* Reconfiguring a running listener keeps the port it already owns. */
  assert(!pbs_h264_server_start_at_rate(BASE_PORT, 1920, 1080, 30, sps, sizeof sps, pps, sizeof pps));
  assert(pbs_h264_server_port() == BASE_PORT + 2);

  pbs_h264_server_stop();
  pbs_h264_server_wait();
  assert(pbs_h264_server_port() == 0);

  close(blocker_a);
  close(blocker_b);

  /* Once the requested port is free again, it is used again. */
  assert(!pbs_h264_server_start(BASE_PORT, 1280, 720, sps, sizeof sps, pps, sizeof pps));
  assert(pbs_h264_server_port() == BASE_PORT);
  pbs_h264_server_stop();
  pbs_h264_server_wait();

  /* The whole window occupied: the start fails, and errno explains why. */
  int blockers[PBS_H264_PORT_SCAN];
  for (int i = 0; i < PBS_H264_PORT_SCAN; ++i) blockers[i] = listen_on(BASE_PORT + i);
  errno = 0;
  assert(pbs_h264_server_start(BASE_PORT, 1280, 720, sps, sizeof sps, pps, sizeof pps) < 0);
  assert(errno == EADDRINUSE);
  for (int i = 0; i < PBS_H264_PORT_SCAN; ++i) close(blockers[i]);

  puts("HTTP port scan: busy ports are skipped, the bound port is reported, "
       "a full window still fails with EADDRINUSE");
  return 0;
}
