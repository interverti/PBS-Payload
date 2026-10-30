// SPDX-License-Identifier: GPL-3.0-or-later
// Local-only browser fixture: real HTTP server with synthetic Annex B video.
#include "common/http/server.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stop;
static void stopping(int sig){(void)sig;stop=1;}
int main(int argc,char **argv){
  assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);rewind(f);
  assert(size>0&&size<16*1024*1024);unsigned char *data=malloc((size_t)size);assert(data&&fread(data,1,(size_t)size,f)==(size_t)size);fclose(f);
  size_t offsets[256],count=0,sps_at=0,sps_len=0,pps_at=0,pps_len=0,last=0;int last_type=0;
  for(size_t i=0;i+4<(size_t)size;i++)if(!data[i]&&!data[i+1]&&(data[i+2]==1||(!data[i+2]&&data[i+3]==1))){
    size_t prefix=data[i+2]==1?3:4;
    if(last_type==7)sps_len=i-last;
    if(last_type==8)pps_len=i-last;
    int type=data[i+prefix]&31;last=i+prefix;last_type=type;
    if(type==7)sps_at=last;
    if(type==8)pps_at=last;
    if(type==9){assert(count<255);offsets[count++]=i;}i+=prefix-1;
  }
  assert(count&&sps_len&&pps_len);offsets[count]=(size_t)size;
  signal(SIGTERM,stopping);signal(SIGINT,stopping);
  pbs_h264_enable_profiles();pbs_audio_configure("pcm_f32le",2,48000);
  assert(!pbs_h264_server_start(18101,1280,720,data+sps_at,sps_len,data+pps_at,pps_len));
  puts("Local dashboard fixture ready on port 18101");fflush(stdout);
  unsigned index=0;float pcm[1600]={0};
  while(!stop&&pbs_h264_server_running()){
    int requested=0;
    if(pbs_h264_take_profile_request(&requested))pbs_h264_profile_failed();
    (void)pbs_h264_take_restart_request();
    size_t n=index%count;assert(!pbs_h264_publish(data+offsets[n],offsets[n+1]-offsets[n],index*1500u));
    assert(!pbs_audio_publish(pcm,sizeof pcm,index*1500u));index++;
    struct timespec delay={0,16666667};nanosleep(&delay,NULL);
  }
  pbs_h264_server_stop();pbs_h264_server_wait();free(data);return 0;
}
