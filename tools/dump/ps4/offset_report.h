// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* Formatting for the read-only PS4 offset dump payload.
 *
 * Everything here is pure: no console access, no kernel calls, no FILE I/O
 * beyond the buffers the caller provides. dump_main.c supplies the measured
 * addresses; tests/ps4/dump/offset_report.c asserts the text and the
 * module-relative arithmetic on the host. tools/ps4_offsets.py parses the
 * same labels, so they are a stable, documented contract. */
#include <stddef.h>
#include <stdint.h>
#include "platform/ps4/system/firmware.h"

typedef struct {
  const char *label;  /* stable key, e.g. "avcap_init" */
  const char *module; /* basename as the kernel lists it */
  const char *symbol; /* exported symbol the capture path resolves */
} pbs_ps4_dump_symbol;

/* Symbol catalog, NULL-terminated count in *count. */
const pbs_ps4_dump_symbol *pbs_ps4_dump_symbols(size_t *count);

/* Text used when a measurement is unavailable. */
const char *pbs_ps4_dump_unresolved(void);

/* Module-relative address. Returns -1 when the address or the module base is
 * unusable, when the address precedes the base, or when it lands outside a
 * known size. A zero mapsize means "unknown" and only rejects negatives. */
long long pbs_ps4_dump_rva(intptr_t address, intptr_t mapbase, uintptr_t mapsize);

/* "module process=... name=... handle=... base=... size=... entry=..." */
int pbs_ps4_dump_module_line(char *out, size_t size, const char *process,
                             const char *module, unsigned handle,
                             uintptr_t mapbase, uintptr_t mapsize,
                             intptr_t entry);

/* "symbol process=... module=... label=... address=... rva=..." */
int pbs_ps4_dump_symbol_line(char *out, size_t size, const char *process,
                             const char *label, const char *module,
                             intptr_t address, long long rva);

/* "label=0x...", or "label=unresolved". */
int pbs_ps4_dump_offset_line(char *out, size_t size, const char *label,
                             long long rva);

/* "kernel layout status=discovered acmgr=... gate0=... ..." */
int pbs_ps4_dump_kernel_line(char *out, size_t size,
                             const pbs_ps4_firmware *layout, unsigned firmware);

/* "kernel probe base=0x... image=0x... read=0 magic=7f454c46...". Explains why
 * discovery failed: the base, the image size and the first bytes read there. */
int pbs_ps4_dump_kernel_probe_line(char *out, size_t size, uint64_t base,
                                   size_t image_size, int read_rc,
                                   const unsigned char *magic,
                                   size_t magic_size);

/* Paste-ready initializer for the legacy table in system/firmware.h. Emits a
 * comment when *layout is NULL. */
int pbs_ps4_dump_snippet(char *out, size_t size,
                         const pbs_ps4_firmware *layout, unsigned firmware);
