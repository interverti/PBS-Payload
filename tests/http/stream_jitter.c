// SPDX-License-Identifier: GPL-3.0-or-later
/* Hold one send while capture publishes ten frames: all still fit the ring. */
#include "common/http/server.h"
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int held, released;
static void hold_packet(void) {
  held = 1;
  pthread_cond_broadcast(&changed);
  while (!released) pthread_cond_wait(&changed, &lock);
}
ssize_t __real_send(int fd, const void *data, size_t size, int flags);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags) {
  pthread_mutex_lock(&lock);
  if (size == 14 && !held) {
    hold_packet();
  }
  pthread_mutex_unlock(&lock);
  return __real_send(fd, data, size, flags);
}
ssize_t __real_sendto(int fd,const void *data,size_t size,int flags,const struct sockaddr *addr,socklen_t addrlen);
ssize_t __wrap_sendto(int fd,const void *data,size_t size,int flags,const struct sockaddr *addr,socklen_t addrlen) {
  const unsigned char *p=data;
  pthread_mutex_lock(&lock);
  if(size>12 && (p[12]&31)==5 && !held)hold_packet();
  pthread_mutex_unlock(&lock);
  return __real_sendto(fd,data,size,flags,addr,addrlen);
}
static void exact(int fd, void *data, size_t size) {
  unsigned char *p = data;
  while (size) {
    ssize_t n = recv(fd,p,size,0);
    assert(n > 0);
    p += n; size -= (size_t)n;
  }
}
int main(int argc,char **argv) {
  int rtp=argc>1 && !strcmp(argv[1],"--rtp");
  const unsigned char sps[]={0x67,0x4d,0x40,0x28},pps[]={0x68,0xee};
  unsigned char frame[]={0,0,0,1,0x65,0x88};
  assert(!pbs_h264_server_start(18141,1280,720,sps,sizeof sps,pps,sizeof pps));
  int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
  struct timeval timeout={.tv_sec=3};
  assert(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout));
  struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(18141),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(!connect(fd,(struct sockaddr *)&addr,sizeof addr));
  int udp=-1;
  if(rtp){
    udp=socket(AF_INET,SOCK_DGRAM,0);assert(udp>=0);
    struct sockaddr_in dest=addr;dest.sin_port=htons(18142);
    assert(!bind(udp,(struct sockaddr *)&dest,sizeof dest));
    assert(!setsockopt(udp,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout));
  }
  const char *request=rtp?"GET /rtp/video?port=18142 HTTP/1.1\r\nHost: localhost\r\n\r\n":"GET /frames HTTP/1.1\r\nHost: localhost\r\n\r\n";
  assert(send(fd,request,strlen(request),0)==(ssize_t)strlen(request));
  char header[512]={0};size_t used=0;
  do {assert(used+1<sizeof header);exact(fd,header+used++,1);} while(!strstr(header,"\r\n\r\n"));
  assert(!pbs_h264_publish(frame,sizeof frame,0));
  pthread_mutex_lock(&lock);
  while(!held)pthread_cond_wait(&changed,&lock);
  pthread_mutex_unlock(&lock);
  (void)pbs_h264_take_keyframe_request();
  frame[4]=0x41;
  for(unsigned i=1;i<=10;i++)assert(!pbs_h264_publish(frame,sizeof frame,i*1500));
  pthread_mutex_lock(&lock);released=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);
  for(unsigned i=0;i<=10;i++) {
    if(rtp){
      unsigned char packet[128];ssize_t n;
      do{n=recv(udp,packet,sizeof packet,0);assert(n>12);}while((packet[12]&31)==7 || (packet[12]&31)==8);
      uint32_t pts;memcpy(&pts,packet+4,4);
      assert(ntohl(pts)==i*1500 && (packet[12]&31)==(i?1:5));
      continue;
    }
    uint32_t packet[2];unsigned char payload[6];
    exact(fd,packet,sizeof packet);
    assert(ntohl(packet[0])==sizeof payload && ntohl(packet[1])==i*1500);
    exact(fd,payload,sizeof payload);
    assert((payload[4]&31)==(i?1:5));
  }
  assert(!pbs_h264_take_keyframe_request());
  if(udp>=0)close(udp);
  close(fd);pbs_h264_server_stop();pbs_h264_server_wait();
  puts(rtp?"RTP short socket stall: ten queued frames retained without spurious IDR":"HTTP short socket stall: ten queued frames retained without spurious IDR");
  return 0;
}
