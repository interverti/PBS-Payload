// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PBS_RECORD_DIR
#define PBS_RECORD_DIR "/data/pbs/recordings"
#endif
#ifndef PBS_RECORD_CONFIG
#define PBS_RECORD_CONFIG "/data/pbs/recording-directory"
#endif
#define PBS_STORAGE_PATH 256

static char recording_directory[PBS_STORAGE_PATH] = PBS_RECORD_DIR;
static int recording_storage_loaded;

static int storage_path_valid(const char *path) {
  size_t n = strlen(path);
  if (!n || n >= PBS_STORAGE_PATH || path[0] != '/' || path[n-1] == '/') return 0;
  int root = !strncmp(path,"/data/",6) || !strcmp(path,"/data");
  if (!strncmp(path,"/mnt/usb",8) && path[8]>='0' && path[8]<='7' &&
      (path[9]=='/' || !path[9])) root=1;
  if (!strncmp(path,PBS_RECORD_DIR,strlen(PBS_RECORD_DIR)) &&
      (path[strlen(PBS_RECORD_DIR)]=='/' || !path[strlen(PBS_RECORD_DIR)])) root=1;
  if (!root) return 0;
  for (const char *p=path;*p;p++) {
    if (!isalnum((unsigned char)*p) && !strchr("/_-. ",*p)) return 0;
    if (*p=='/') {
      const char *end=strchr(p+1,'/'); size_t len=end?(size_t)(end-p-1):strlen(p+1);
      if (!len || (len==1 && p[1]=='.') || (len==2 && p[1]=='.' && p[2]=='.')) return 0;
    }
  }
  return 1;
}

/* USB mount roots must already exist. Never create a replacement mountpoint
 * on internal storage, or follow a symlink while creating subdirectories. */
static int storage_prepare(const char *path, int create) {
  if (!storage_path_valid(path)) return -1;
  char part[PBS_STORAGE_PATH]; strcpy(part,path);
  for (size_t i=1;;i++) {
    if (part[i] && part[i]!='/') continue;
    char saved=part[i]; part[i]=0; struct stat st;
    if (lstat(part,&st)) {
      int mount_root=!strncmp(part,"/mnt/usb",8) && strlen(part)==9;
      if (!create || mount_root || mkdir(part,0777) || lstat(part,&st)) return -1;
    }
    /* /data is the console-provided storage alias on some environments. */
    if (!strcmp(part,"/data") && S_ISLNK(st.st_mode) && stat(part,&st)) return -1;
    if (!S_ISDIR(st.st_mode)) return -1;
    part[i]=saved; if (!saved) break;
  }
  return 0;
}

static void storage_load(void) {
  if (recording_storage_loaded) return;
  recording_storage_loaded=1;
  FILE *f=fopen(PBS_RECORD_CONFIG,"rb"); if (!f) return;
  char path[PBS_STORAGE_PATH]; size_t n=fread(path,1,sizeof path-1,f);
  int end=fgetc(f); fclose(f); path[n]=0;
  if (end==EOF && strlen(path)==n && storage_path_valid(path)) strcpy(recording_directory,path);
}

static int storage_save(const char *path) {
  char temporary[512];
  snprintf(temporary,sizeof temporary,"%s.%ld.tmp",PBS_RECORD_CONFIG,(long)getpid());
  int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600); if (fd<0) return -1;
  size_t n=strlen(path); ssize_t written=write(fd,path,n);
  int failed=written!=(ssize_t)n;
  if (close(fd)) failed=1;
  if (!failed && rename(temporary,PBS_RECORD_CONFIG)) failed=1;
  if (failed) unlink(temporary);
  return failed?-1:0;
}

static int storage_decode(const char *encoded,char *decoded,size_t size) {
  size_t n=0;
  for (size_t i=0;encoded[i];i++) {
    unsigned char c=(unsigned char)encoded[i];
    if (c=='%') {
      if (!encoded[i+1] || !encoded[i+2] || !isxdigit((unsigned char)encoded[i+1]) || !isxdigit((unsigned char)encoded[i+2])) return -1;
      char hex[3]={encoded[i+1],encoded[i+2],0}; c=(unsigned char)strtoul(hex,NULL,16); i+=2;
    }
    if (!c || n+1>=size) return -1;
    decoded[n++]=(char)c;
  }
  decoded[n]=0; return 0;
}
