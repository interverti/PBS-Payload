// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string.h>

static int range_number(const char **p,uint64_t *out) {
  uint64_t n=0; const char *start=*p;
  while (**p>='0' && **p<='9') {
    unsigned digit=(unsigned)(*(*p)++-'0');
    if (n>(UINT64_MAX-digit)/10) return -1;
    n=n*10+digit;
  }
  *out=n; return *p==start?-1:0;
}
/* Return 0 for the full file, 1 for a single satisfiable range, -1 otherwise. */
static int recording_range(const char *value,uint64_t size,uint64_t *first,uint64_t *last) {
  *first=0; *last=size?size-1:0;
  if (!value || !*value) return 0;
  if (strncmp(value,"bytes=",6) || !size) return -1;
  const char *p=value+6; uint64_t a,b;
  if (*p=='-') {
    p++; if (range_number(&p,&b) || *p || !b) return -1;
    *first=b>=size?0:size-b; return 1;
  }
  if (range_number(&p,&a) || *p++!='-' || a>=size) return -1;
  if (!*p) b=size-1;
  else if (range_number(&p,&b) || *p || b<a) return -1;
  *first=a; *last=b>=size?size-1:b; return 1;
}
