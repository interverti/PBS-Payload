// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string.h>

/* Orbis/FreeBSD sockaddr_in: u8 length, u8 family, then network byte order.
 * Keep this explicit even in Linux host tests: Linux uses a u16 family. */
static inline void pbs_ps4_worker_addr(unsigned char addr[16], uint16_t port) {
  memset(addr, 0, 16);
  addr[0] = 16;
  addr[1] = 2; /* AF_INET */
  addr[2] = (unsigned char)(port >> 8);
  addr[3] = (unsigned char)port;
  addr[4] = 127;
  addr[7] = 1;
}
