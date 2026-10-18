// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/ps4/system/firmware.h"
#include <stdio.h>
/* Diagnostic-only image dump of the SceAvCapture daemon, one attached stop
 * per run, PT_IO reads in shrinking windows with skip-on-unreadable. The
 * file lands under the FTP-visible capture dir for offline reversal. */
int pbs_ps4_daemon_dump(FILE *log, const pbs_ps4_firmware *fw, const char *path);
