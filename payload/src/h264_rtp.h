// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* RFC 6184 non-interleaved mode: single NAL and FU-A, PT 96, 90 kHz. */
typedef int (*pbs_rtp_send_fn)(void *, const unsigned char *, size_t);
static size_t pbs_nal_start(const unsigned char *p, size_t n, size_t pos, size_t *prefix) {
  for (; pos + 3 <= n; pos++) {
    if (!p[pos] && !p[pos+1]) {
      if (p[pos+2] == 1) { *prefix = 3; return pos; }
      if (pos + 4 <= n && !p[pos+2] && p[pos+3] == 1) { *prefix = 4; return pos; }
    }
  }
  *prefix = 0; return n;
}
static int pbs_rtp_frame(const unsigned char *p, size_t n, uint32_t timestamp,
                        uint32_t ssrc, uint16_t *sequence, pbs_rtp_send_fn send_packet, void *ctx) {
  size_t prefix, pos = pbs_nal_start(p,n,0,&prefix);
  if (pos == n) return -1;
  while (pos < n) {
    size_t begin = pos + prefix, next_prefix;
    size_t end = pbs_nal_start(p,n,begin,&next_prefix);
    if (begin == end) return -1;
    size_t length = end - begin, offset = length > 1188 ? 1 : 0;
    do {
      unsigned char packet[1200] = {0x80,96};
      uint16_t seq = (*sequence)++;
      packet[2]=seq>>8; packet[3]=seq;
      for (int i=0;i<4;i++){packet[4+i]=timestamp>>(24-8*i);packet[8+i]=ssrc>>(24-8*i);}
      size_t count, bytes;
      if (length <= 1188) { memcpy(packet+12,p+begin,length); bytes=12+length; offset=length; }
      else {
        count = length-offset; if(count>1186)count=1186;
        packet[12]=(p[begin]&0xe0)|28;
        packet[13]=(p[begin]&31)|(offset==1?0x80:0)|(offset+count==length?0x40:0);
        memcpy(packet+14,p+begin+offset,count); bytes=14+count; offset+=count;
      }
      if (end == n && offset == length) packet[1] |= 0x80;
      if (send_packet(ctx,packet,bytes)) return -1;
    } while(offset<length);
    pos=end; prefix=next_prefix;
  }
  return 0;
}
