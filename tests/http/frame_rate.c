// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/http/server.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static void check_fps(int expected) {
  int fd = socket(AF_INET,SOCK_STREAM,0); assert(fd >= 0);
  struct sockaddr_in addr = {.sin_family=AF_INET,.sin_port=htons(18090),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(!connect(fd,(struct sockaddr *)&addr,sizeof addr));
  const char req[]="GET /config.json HTTP/1.1\r\nHost: localhost\r\n\r\n";
  assert(send(fd,req,sizeof req-1,0)==sizeof req-1);
  char response[4096];size_t used=0;ssize_t n;
  while((n=recv(fd,response+used,sizeof response-1-used,0))>0)used+=(size_t)n;
  response[used]=0;close(fd);
  char fps[32];snprintf(fps,sizeof fps,"\"fps\":%d",expected);
  assert(strstr(response,"200 OK") && strstr(response,fps));
}
int main(void) {
  const unsigned char sps[]={0x67,0x4d,0x40,0x28},pps[]={0x68,0xee,0x3c,0x80};
  assert(!pbs_h264_server_start(18090,1280,720,sps,sizeof sps,pps,sizeof pps));check_fps(60);
  assert(!pbs_h264_server_start_at_rate(18090,1920,1080,30,sps,sizeof sps,pps,sizeof pps));check_fps(30);
  assert(pbs_h264_server_start_at_rate(18090,1920,1080,45,sps,sizeof sps,pps,sizeof pps)<0);check_fps(30);
  assert(!pbs_h264_server_start(18090,1280,720,sps,sizeof sps,pps,sizeof pps));check_fps(60);
  pbs_h264_server_stop();pbs_h264_server_wait();
  puts("HTTP config: 30fps, default 60fps and rate reconfiguration verified");return 0;
}
