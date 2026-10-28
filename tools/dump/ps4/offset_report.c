// SPDX-License-Identifier: GPL-3.0-or-later
#include "dump/ps4/offset_report.h"
#include <stdio.h>
#include <string.h>

/* The capture path resolves exactly these exports; the dump reports their
 * module-relative addresses so a profile can be written without a disassembler.
 * basenames match what kernel_dynlib_handle() expects on PS4 FW 11.00. */
static const pbs_ps4_dump_symbol symbols[] = {
  {"avcap_init",            "libSceAvcap.sprx",         "sceVshAvcapInit"},
  {"avcap_term",            "libSceAvcap.sprx",         "sceVshAvcapTerm"},
  {"avcap_set_static_info", "libSceAvcap.sprx",         "sceVshAvcapSetStaticInfo"},
  {"avcap_open_video",      "libSceAvcap.sprx",         "sceVshAvcapOpenVideoEx"},
  {"avcap_start",           "libSceAvcap.sprx",         "sceVshAvcapStart"},
  {"avcap_stop",            "libSceAvcap.sprx",         "sceVshAvcapStop"},
  {"avcap_close",           "libSceAvcap.sprx",         "sceVshAvcapClose"},
  {"avcap_get_info",        "libSceAvcap.sprx",         "sceVshAvcapGetInfo"},
  {"avcap_get_static_info", "libSceAvcap.sprx",         "sceVshAvcapGetStaticInfo"},
  {"avcap_set_info",        "libSceAvcap.sprx",         "sceVshAvcapSetInfo"},
  {"avcap_read_video",      "libSceAvcap.sprx",         "sceVshAvcapReadVideo"},
  {"libc_malloc",           "libSceLibcInternal.sprx",  "malloc"},
  {"libc_free",             "libSceLibcInternal.sprx",  "free"},
  {"kernel_getpid",         "libkernel_sys.sprx",       "getpid"},
  {"kernel_usleep",         "libkernel_sys.sprx",       "usleep"},
};

const pbs_ps4_dump_symbol *pbs_ps4_dump_symbols(size_t *count) {
  if (count) *count = sizeof symbols / sizeof symbols[0];
  return symbols;
}

const char *pbs_ps4_dump_unresolved(void) {
  return "unresolved";
}

long long pbs_ps4_dump_rva(intptr_t address, intptr_t mapbase, uintptr_t mapsize) {
  if (address <= 0 || mapbase <= 0 || address < mapbase) return -1;
  unsigned long long delta = (unsigned long long)(address - mapbase);
  if (mapsize && delta >= (unsigned long long)mapsize) return -1;
  return (long long)delta;
}

int pbs_ps4_dump_module_line(char *out, size_t size, const char *process,
                             const char *module, unsigned handle,
                             uintptr_t mapbase, uintptr_t mapsize,
                             intptr_t entry) {
  if (!out || !size) return -1;
  return snprintf(out, size,
                  "module process=%s name=%s handle=%u base=0x%llx size=0x%llx entry=0x%llx",
                  process && *process ? process : "?",
                  module && *module ? module : "?",
                  handle,
                  (unsigned long long)mapbase,
                  (unsigned long long)mapsize,
                  (unsigned long long)(entry > 0 ? entry : 0));
}

int pbs_ps4_dump_symbol_line(char *out, size_t size, const char *process,
                             const char *label, const char *module,
                             intptr_t address, long long rva) {
  if (!out || !size) return -1;
  const char *process_name = process && *process ? process : "?";
  const char *label_name = label && *label ? label : "?";
  const char *module_name = module && *module ? module : "?";
  if (rva < 0)
    return snprintf(out, size,
                    "symbol process=%s module=%s label=%s address=0x%llx rva=%s",
                    process_name, module_name, label_name,
                    (unsigned long long)(address > 0 ? address : 0),
                    pbs_ps4_dump_unresolved());
  return snprintf(out, size,
                  "symbol process=%s module=%s label=%s address=0x%llx rva=0x%llx",
                  process_name, module_name, label_name,
                  (unsigned long long)(address > 0 ? address : 0),
                  (unsigned long long)rva);
}

int pbs_ps4_dump_offset_line(char *out, size_t size, const char *label,
                             long long rva) {
  if (!out || !size || !label || !*label) return -1;
  if (rva < 0)
    return snprintf(out, size, "%s=%s", label, pbs_ps4_dump_unresolved());
  return snprintf(out, size, "%s=0x%llx", label, (unsigned long long)rva);
}

/* "0x77,0x1c" without a trailing comma; a zero length renders as "-". */
static void bytes_hex(char *out, size_t size, const unsigned char *bytes,
                      size_t length) {
  size_t used = 0;
  if (length > 16) length = 16;
  out[0] = '\0';
  if (!length) {
    snprintf(out, size, "-");
    return;
  }
  for (size_t i = 0; i < length; ++i) {
    int n = snprintf(out + used, size - used, i ? ",0x%02x" : "0x%02x", bytes[i]);
    if (n < 0 || (size_t)n >= size - used) return;
    used += (size_t)n;
  }
}

/* Compact hex, no separators, for the ELF magic probe. */
static void bytes_plain(char *out, size_t size, const unsigned char *bytes,
                        size_t length) {
  size_t used = 0;
  if (length > 8) length = 8;
  out[0] = '\0';
  for (size_t i = 0; i < length; ++i) {
    int n = snprintf(out + used, size - used, "%02x", bytes[i]);
    if (n < 0 || (size_t)n >= size - used) return;
    used += (size_t)n;
  }
}

int pbs_ps4_dump_kernel_probe_line(char *out, size_t size, uint64_t base,
                                   size_t image_size, int read_rc,
                                   const unsigned char *magic,
                                   size_t magic_size) {
  if (!out || !size) return -1;
  if (read_rc || !magic || !magic_size)
    return snprintf(out, size,
                    "kernel probe base=0x%llx image=0x%zx read=%d magic=unread",
                    (unsigned long long)base, image_size, read_rc);
  char text[2 * 8 + 1];
  bytes_plain(text, sizeof text, magic, magic_size);
  return snprintf(out, size,
                  "kernel probe base=0x%llx image=0x%zx read=%d magic=%s",
                  (unsigned long long)base, image_size, read_rc, text);
}

int pbs_ps4_dump_kernel_line(char *out, size_t size,
                             const pbs_ps4_firmware *layout, unsigned firmware) {
  if (!out || !size) return -1;
  if (!layout)
    return snprintf(out, size,
                    "kernel layout status=unavailable firmware=0x%08x", firmware);
  char before0[32], after0[32], before1[32], after1[32];
  bytes_hex(before0, sizeof before0, layout->gates[0].before, layout->gates[0].size);
  bytes_hex(after0, sizeof after0, layout->gates[0].after, layout->gates[0].size);
  bytes_hex(before1, sizeof before1, layout->gates[1].before, layout->gates[1].size);
  bytes_hex(after1, sizeof after1, layout->gates[1].after, layout->gates[1].size);
  return snprintf(out, size,
                  "kernel layout status=discovered firmware=0x%08x acmgr=0x%x"
                  " gate0=0x%x before0=%s after0=%s size0=%zu"
                  " gate1=0x%x before1=%s after1=%s size1=%zu",
                  firmware, layout->acmgr_offset,
                  layout->gates[0].offset, before0, after0, layout->gates[0].size,
                  layout->gates[1].offset, before1, after1, layout->gates[1].size);
}

int pbs_ps4_dump_snippet(char *out, size_t size,
                         const pbs_ps4_firmware *layout, unsigned firmware) {
  if (!out || !size) return -1;
  if (!layout)
    return snprintf(out, size, "/* runtime layout unavailable; no snippet */\n");
  char before0[32], after0[32], before1[32], after1[32];
  bytes_hex(before0, sizeof before0, layout->gates[0].before, layout->gates[0].size);
  bytes_hex(after0, sizeof after0, layout->gates[0].after, layout->gates[0].size);
  bytes_hex(before1, sizeof before1, layout->gates[1].before, layout->gates[1].size);
  bytes_hex(after1, sizeof after1, layout->gates[1].after, layout->gates[1].size);
  /* version_mask stays the firmware family mask used by the legacy table: the
   * runtime discovery matching one image does not prove other revisions. */
  return snprintf(out, size,
                  "/* Measured on the console; paste into the legacy table in\n"
                  " * system/firmware.h and keep PBS_PS4_DYNAMIC_LAYOUT=1 in\n"
                  " * normal builds. */\n"
                  "  .name = \"PS4 %08x / runtime dump\",\n"
                  "  .version = 0x%08xu, .version_mask = 0xffff0000u,\n"
                  "  .acmgr_offset = 0x%x,\n"
                  "  .acmgr_signature = {0x%02x, 0x%02x, 0x%02x},\n"
                  "  .gates = {\n"
                  "    {0x%x, {%s}, {%s}, %zu},\n"
                  "    {0x%x, {%s}, {%s}, %zu}\n"
                  "  },\n",
                  firmware, firmware,
                  layout->acmgr_offset,
                  layout->acmgr_signature[0], layout->acmgr_signature[1],
                  layout->acmgr_signature[2],
                  layout->gates[0].offset, before0, after0, layout->gates[0].size,
                  layout->gates[1].offset, before1, after1, layout->gates[1].size);
}
