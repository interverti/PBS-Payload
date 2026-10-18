// SPDX-License-Identifier: GPL-3.0-or-later
/* A payload must not return while its HTTP workers still execute its code. */
#include "common/http/server.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int connect_client(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(!connect(fd, (struct sockaddr *)&addr, sizeof addr));
  struct timeval timeout = {2, 0};
  assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout));
  return fd;
}
static uint64_t monotonic_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void receive_exact(int fd, void *dest, size_t length) {
  unsigned char *out = dest;
  while (length) {
    ssize_t n = recv(fd, out, length, 0);
    assert(n > 0);
    out += n;
    length -= (size_t)n;
  }
}

static void receive_header(int fd) {
  char head[512] = {0};
  size_t used = 0;
  while (used < sizeof head - 1) {
    receive_exact(fd, &head[used], 1);
    used++;
    if (used >= 4 && !memcmp(head + used - 4, "\r\n\r\n", 4)) break;
  }
  assert(strstr(head, "HTTP/1.1 200 OK") && used < sizeof head - 1);
}

int main(void) {
  const unsigned char sps[] = {0x67, 0x42, 0, 0x1f}, pps[] = {0x68, 0xce};
  for (int iteration = 0; iteration < 2; ++iteration) {
    assert(!pbs_h264_server_start(18089, 1280, 720, sps, sizeof sps, pps, sizeof pps));
    int video = connect_client(18089);
    const char video_request[] = "GET /frames HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(send(video, video_request, sizeof video_request - 1, 0) == sizeof video_request - 1);
    receive_header(video);
    int requested = 0;
    for (int i = 0; i < 200 && !requested; ++i) {
      requested = pbs_h264_take_keyframe_request();
      if (!requested) usleep(1000);
    }
    assert(requested);
    const unsigned char idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x80};
    assert(!pbs_h264_publish(idr, sizeof idr, 90000));
    uint32_t packet[2];
    receive_exact(video, packet, sizeof packet);
    assert(ntohl(packet[0]) == sizeof idr && ntohl(packet[1]) == 90000);
    unsigned char received[sizeof idr];
    receive_exact(video, received, sizeof received);
    assert(!memcmp(received, idr, sizeof idr));
    int control = connect_client(18089);
    const char restart[] = "POST /capture/restart HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
    assert(send(control, restart, sizeof restart - 1, 0) == sizeof restart - 1);
    receive_header(control);
    close(control);
    assert(pbs_h264_take_restart_request() == 1);
    assert(pbs_h264_take_restart_request() == 0);
    assert(pbs_h264_server_running());
    /* A capture reopen invalidates the old media generation, then a fresh
     * reader must receive a new IDR from the reconfigured live server. */
    pbs_h264_discontinuity();
    assert(recv(video, received, sizeof received, 0) <= 0);
    assert(!pbs_h264_server_start(18089, 1280, 720, sps, sizeof sps, pps, sizeof pps));
    int resumed = connect_client(18089);
    assert(send(resumed, video_request, sizeof video_request - 1, 0) == sizeof video_request - 1);
    receive_header(resumed);
    requested = 0;
    for (int i = 0; i < 200 && !requested; ++i) {
      requested = pbs_h264_take_keyframe_request();
      if (!requested) usleep(1000);
    }
    assert(requested);
    assert(!pbs_h264_publish(idr, sizeof idr, 180000));
    receive_exact(resumed, packet, sizeof packet);
    assert(ntohl(packet[0]) == sizeof idr && ntohl(packet[1]) == 180000);
    receive_exact(resumed, received, sizeof received);
    assert(!memcmp(received, idr, sizeof idr));
    int stream = connect_client(18089);
    const char request[] = "GET /audio HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(send(stream, request, sizeof request - 1, 0) == sizeof request - 1);
    char reply[512];
    ssize_t count = recv(stream, reply, sizeof reply - 1, 0);
    assert(count > 0); reply[count] = 0;
    assert(strstr(reply, "200 OK"));
    int partial = connect_client(18089);
    assert(send(partial, "GET /", 5, 0) == 5);
    usleep(20000);
    uint64_t stop_began = monotonic_ms();
    pbs_h264_server_stop();
    pbs_h264_server_wait();
    /* Header recv used to stall native Avcap Stop/Close/Term for at least
     * SO_RCVTIMEO (and indefinitely under slowloris byte trickling). */
    assert(monotonic_ms() - stop_began < 1500);
    assert(!pbs_h264_server_running());
    assert(recv(resumed, reply, sizeof reply, 0) <= 0);
    assert(recv(video, reply, sizeof reply, 0) <= 0);
    assert(recv(stream, reply, sizeof reply, 0) <= 0);
    assert(recv(partial, reply, sizeof reply, 0) <= 0);
    close(video); close(resumed); close(stream); close(partial);
  }
  puts("HTTP frame delivery, shutdown joins clients, and restart verified");
  return 0;
}
