// SPDX-License-Identifier: GPL-3.0-or-later
/* Hold one send while capture publishes a burst. A stall shorter than
 * PBS_H264_RTP_MAX_BACKLOG keeps every queued frame; a longer backlog is stale
 * and the sender must skip it and ask capture for a fresh IDR. */
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
static void wait_for_hold(void) {
  pthread_mutex_lock(&lock);
  while (!held) pthread_cond_wait(&changed, &lock);
  pthread_mutex_unlock(&lock);
}
static void release_sender(void) {
  pthread_mutex_lock(&lock);
  released = 1;
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&lock);
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
/* SPS and PPS precede the first IDR of a resync; skip until the slice. */
static void read_rtp_until_idr(int udp, uint32_t expected_pts) {
  unsigned char packet[128];
  for (;;) {
    ssize_t n = recv(udp,packet,sizeof packet,0);
    assert(n > 12);
    if ((packet[12]&31) != 5) continue;
    uint32_t pts;
    memcpy(&pts,packet+4,4);
    assert(ntohl(pts) == expected_pts);
    return;
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

  /* Phase 1: a short stall keeps the queued frames. RTP must stay one frame
   * inside the backlog limit; the stale case is phase 2. */
  assert(!pbs_h264_publish(frame,sizeof frame,0));
  wait_for_hold();
  (void)pbs_h264_take_keyframe_request();
  frame[4]=0x41;
  unsigned queued=rtp?PBS_H264_RTP_MAX_BACKLOG-1:10;
  for(unsigned i=1;i<=queued;i++)assert(!pbs_h264_publish(frame,sizeof frame,i*1500));
  release_sender();
  for(unsigned i=0;i<=queued;i++) {
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

  /* Phase 2: hold a fresh IDR, then queue a backlog at the limit. The stale
   * frames must not be replayed; capture is asked for a new IDR instead. */
  if(rtp){
    pthread_mutex_lock(&lock);held=0;released=0;pthread_mutex_unlock(&lock);
    frame[4]=0x65;
    assert(!pbs_h264_publish(frame,sizeof frame,100000));
    wait_for_hold();
    (void)pbs_h264_take_keyframe_request();
    frame[4]=0x41;
    /* The held IDR is already dequeued, so this leaves exactly the limit in
     * the ring: one frame more than the limit would be delivered. */
    for(unsigned i=1;i<=PBS_H264_RTP_MAX_BACKLOG+1;i++)
      assert(!pbs_h264_publish(frame,sizeof frame,100000+i*1500));
    release_sender();
    read_rtp_until_idr(udp,100000);
    int requested=0;
    for(int i=0;i<20 && !requested;i++){usleep(50000);requested=pbs_h264_take_keyframe_request();}
    assert(requested);
    frame[4]=0x65;
    assert(!pbs_h264_publish(frame,sizeof frame,200000));
    read_rtp_until_idr(udp,200000);
  }
  if(udp>=0)close(udp);
  close(fd);pbs_h264_server_stop();pbs_h264_server_wait();
  puts(rtp?"RTP short socket stall: frames inside the backlog limit are retained, a stale backlog resyncs and requests an IDR":"HTTP short socket stall: ten queued frames retained without spurious IDR");
  return 0;
}
