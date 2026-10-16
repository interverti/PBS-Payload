// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Seregonwar.
#include "h264_rtp.h"
#include <vector>
#include <cstdio>
using Bytes=std::vector<unsigned char>;
static int collect(void* opaque,const unsigned char* bytes,size_t size){
 static_cast<std::vector<Bytes>*>(opaque)->emplace_back(bytes,bytes+size);return 0;
}
int main(){
 Bytes input{0,0,0,1,0x67,1,2,0,0,1,0x65};
 input.insert(input.end(),3000,0x55);
 std::vector<Bytes> packets;uint16_t seq=65535;
 if(pbs_rtp_frame(input.data(),input.size(),90000,0x12345678,&seq,collect,&packets))return 1;
 if(packets.size()!=4||seq!=3)return 2;
 Bytes nal;bool in_fu=false;
 for(size_t i=0;i<packets.size();i++){
  const auto& p=packets[i];
  if(p.size()>1200||p[0]!=0x80||(p[1]&127)!=96)return 3;
  if(bool(p[1]&128)!=(i==packets.size()-1))return 4;
  if(p[4]!=0||p[5]!=1||p[6]!=0x5f||p[7]!=0x90)return 5;
  if(i==0){if(p[12]!=0x67||p.size()!=15)return 6;continue;}
  if((p[12]&31)!=28)return 7;
  if(p[13]&128){if(in_fu)return 8;in_fu=true;nal.push_back((p[12]&0xe0)|(p[13]&31));}
  nal.insert(nal.end(),p.begin()+14,p.end());
  if(p[13]&64)in_fu=false;
 }
 if(in_fu||nal.size()!=3001||nal[0]!=0x65)return 9;
 for(size_t i=1;i<nal.size();i++)if(nal[i]!=0x55)return 10;
 std::puts("RTP single NAL, FU-A, MTU, marker, timestamp and sequence wrap verified");
}
