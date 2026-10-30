// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/http/server.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static char response[8192]; static size_t response_size;
static const unsigned char sps[]={0x67,0x4d,0x40,0x28},pps[]={0x68,0xee,0x3c,0x80};
static const unsigned char idr[]={0,0,0,1,0x65,0x88,0x84,0x21};
static void request(const char *method,const char *path,const char *range,int code) {
  int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
  struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(18096),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(!connect(fd,(struct sockaddr*)&address,sizeof address));
  char req[2048];int n=snprintf(req,sizeof req,"%s %s HTTP/1.1\r\nHost: localhost\r\n%s%s%sContent-Length: 0\r\n\r\n",method,path,range?"Range: ":"",range?range:"",range?"\r\n":"");
  assert(n>0&&(size_t)n<sizeof req);assert(send(fd,req,(size_t)n,0)==n);
  response_size=0;ssize_t got;
  while((got=recv(fd,response+response_size,sizeof response-1-response_size,0))>0)response_size+=(size_t)got;
  response[response_size]=0;close(fd);
  char status[32];snprintf(status,sizeof status,"HTTP/1.1 %d",code);assert(strstr(response,status));
}
int main(void) {
  assert(!pbs_h264_server_start(18096,1280,720,sps,sizeof sps,pps,sizeof pps));
  request("GET","/recording/settings",NULL,200);assert(strstr(response,PBS_RECORD_DIR));
  request("POST","/recording/settings?path=/etc",NULL,400);
  request("POST","/recording/settings?path=" PBS_RECORD_DIR "/%2e%2e/escape",NULL,400);
  request("POST","/recording/settings?path=" PBS_RECORD_DIR "/%00bad",NULL,400);
  request("POST","/recording/settings?path=" PBS_RECORD_DIR "/%22bad",NULL,400);
  request("POST","/recording/settings?path=" PBS_RECORD_DIR "/nested%20folder",NULL,200);
  request("GET","/recording/settings",NULL,200);assert(strstr(response,PBS_RECORD_DIR "/nested folder"));
  FILE *saved=fopen(PBS_RECORD_CONFIG,"rb");assert(saved);char directory[256]={0};assert(fread(directory,1,sizeof directory-1,saved)>0);fclose(saved);
  assert(!strcmp(directory,PBS_RECORD_DIR "/nested folder"));
  request("POST","/record/start",NULL,200);
  request("POST","/recording/settings?path=" PBS_RECORD_DIR,NULL,409);
  assert(!pbs_h264_publish(idr,sizeof idr,0));
  request("GET","/status.json",NULL,200);
  char name[96],route[256];const char *start=strstr(response,"\"recording_name\":\"");assert(start);start+=18;
  const char *end=strchr(start,'"');assert(end&&(size_t)(end-start)<sizeof name);memcpy(name,start,(size_t)(end-start));name[end-start]=0;
  snprintf(route,sizeof route,"/recordings/%s",name);
  request("DELETE",route,NULL,409);
  char info[256];snprintf(info,sizeof info,"/recordings/info?name=%s",name);
  request("GET",info,NULL,409);
  request("POST","/record/stop",NULL,200);
  request("GET",info,NULL,200);assert(strstr(response,"\"fps\":60")&&strstr(response,"\"height\":720")&&strstr(response,"\"frames\":1"));
  request("GET",route,"bytes=0-7",206);
  const char *body=strstr(response,"\r\n\r\n");assert(body);body+=4;
  assert(response_size-(size_t)(body-response)==8);
  assert(!memcmp(body,(unsigned char[]){0,0,0,1,0x67,0x4d,0x40,0x28},8));
  request("GET",route,"bytes=-4",206);body=strstr(response,"\r\n\r\n")+4;assert(!memcmp(body,idr+4,4));
  request("GET",route,"bytes=999999-",416);
  request("GET",route,"bytes=0-1,3-4",416);
  request("GET",route,"bytes=18446744073709551616-",416);
  request("GET",route,"bytes=10-2",416);
  char stale[512];snprintf(stale,sizeof stale,"%s?directory=" PBS_RECORD_DIR,route);
  request("GET",stale,"bytes=0-7",409);
  request("DELETE",stale,NULL,409);
  request("DELETE","/recordings/../escape.h264",NULL,400);
  request("DELETE",route,NULL,200);request("DELETE",route,NULL,404);
  request("GET","/recordings",NULL,200);assert(strstr(response,"\r\n\r\n[]"));
  pbs_h264_server_stop();pbs_h264_server_wait();
  puts("PASS: recording destinations, persistence, active-file conflicts, metadata, byte ranges and deletion");
}
