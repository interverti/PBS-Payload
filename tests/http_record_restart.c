// SPDX-License-Identifier: GPL-3.0-or-later
/* Record across an Avcap re-open. Each new encoder sequence must have its
 * own SPS/PPS directly before its first IDR (no old P frames in between). */
#include "h264_http.h"
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void post(const char *path) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  assert(sock >= 0);
  struct sockaddr_in addr = {.sin_family=AF_INET, .sin_port=htons(18139),
                             .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(!connect(sock, (struct sockaddr *)&addr, sizeof addr));
  char request[256];
  int n = snprintf(request, sizeof request,
                   "POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n", path);
  assert(n > 0 && n < (int)sizeof request);
  assert(send(sock, request, (size_t)n, 0) == n);
  char response[256];
  ssize_t count = recv(sock, response, sizeof response - 1, 0);
  assert(count > 0); response[count] = 0;
  assert(strstr(response, "200 OK"));
  close(sock);
}

static int has_bytes(const unsigned char *buf, size_t len,
                     const unsigned char *sub, size_t sub_len) {
  for (size_t i = 0; i + sub_len <= len; ++i)
    if (!memcmp(buf + i, sub, sub_len)) return 1;
  return 0;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const unsigned char sps1[] = {0x67,0x42,0,0x1f};
  const unsigned char pps1[] = {0x68,0xce};
  const unsigned char sps2[] = {0x67,0x4d,0,0x29};
  const unsigned char pps2[] = {0x68,0xee};
  const unsigned char idr1[] = {0,0,0,1,0x65,0x88,0x11};
  const unsigned char pframe[] = {0,0,0,1,0x41,0x88,0x22};
  const unsigned char idr2[] = {0,0,0,1,0x65,0x88,0x33};
  assert(!pbs_h264_server_start(18139,1280,720,sps1,sizeof sps1,pps1,sizeof pps1));
  post("/record/start");
  assert(!pbs_h264_publish(idr1,sizeof idr1,90000));
  pbs_h264_discontinuity();
  assert(!pbs_h264_server_start(18139,1920,1080,sps2,sizeof sps2,pps2,sizeof pps2));
  assert(!pbs_h264_publish(pframe,sizeof pframe,93000));
  assert(!pbs_h264_publish(idr2,sizeof idr2,96000));
  post("/record/stop");
  pbs_h264_server_stop(); pbs_h264_server_wait();
  DIR *dir = opendir(argv[1]); assert(dir);
  struct dirent *entry; char path[512] = "";
  while ((entry = readdir(dir))) {
    if (!strstr(entry->d_name, ".h264")) continue;
    int n = snprintf(path,sizeof path,"%s/%s",argv[1],entry->d_name);
    assert(n > 0 && n < (int)sizeof path);
    break;
  }
  closedir(dir); assert(path[0]);
  FILE *f = fopen(path,"rb"); assert(f);
  unsigned char sample[4096]; size_t len = fread(sample,1,sizeof sample,f);
  fclose(f);
  const unsigned char first[] = {0,0,0,1,0x67,0x42,0,0x1f,0,0,0,1,0x68,0xce};
  const unsigned char second[] = {0,0,0,1,0x67,0x4d,0,0x29,0,0,0,1,0x68,0xee};
  assert(has_bytes(sample,len,first,sizeof first));
  assert(has_bytes(sample,len,second,sizeof second));
  assert(!has_bytes(sample,len,pframe,sizeof pframe));
  assert(has_bytes(sample,len,idr1,sizeof idr1));
  assert(has_bytes(sample,len,idr2,sizeof idr2));
  puts("HTTP recording retained SPS/PPS and keyframes across encoder restart");
  return 0;
}
