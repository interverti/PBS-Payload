// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>

#ifdef PBS_PS4
/* Adopt the ps4debug jailbreak identity on the payload's own process so that
 * PT_ATTACH is accepted: uid/ruid/svuid/rgid/svgid = 0, prison0 and
 * rootvnode for proc rootdir/jaildir. authid and caps are deliberately left
 * alone (forcing them on a GoldHEN shared loader still yields EPERM).
 * Idempotent; logs one line per attempt when log is not NULL. */
int pbs_ps4_cred_elevate(FILE *log);
/* Put the original identity back; safe to call when nothing was changed. */
int pbs_ps4_cred_restore(FILE *log);
#else
static inline int pbs_ps4_cred_elevate(FILE *log) { (void)log; return 0; }
static inline int pbs_ps4_cred_restore(FILE *log) { (void)log; return 0; }
#endif
