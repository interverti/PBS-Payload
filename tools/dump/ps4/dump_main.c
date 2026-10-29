// SPDX-License-Identifier: GPL-3.0-or-later
/* Read-only PS4 offset dump payload.
 *
 * Measures the addresses a firmware profile needs and writes them to
 * /data/pbs/ps4/dump.txt (human) and /data/pbs/ps4/offsets.txt (tool input):
 *   - the verified kernel layout from the live image (acmgr stub and the two
 *     ptrace/policy gates), plus a paste-ready initializer;
 *   - the module-relative address of every export the capture path resolves,
 *     per process, so avcap_init_offset and friends come from the console.
 * If the kernel is readable but discovery fails, also saves ELF headers and
 * executable LOADs to kernel-code-<firmware>.bin for offline signature work.
 * It never patches, never attaches, never opens a capture session and never
 * reads a module image: tools/ps4_offsets.py derives the remaining values
 * offline from the module ELFs dumped by the `probe` payload.
 */
#include "dump/ps4/offset_report.h"
#include "dump/ps4/kernel_image.h"
#include "platform/ps4/system/layout.h"
#include <ps4/kernel.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

#define PBS_PS4_DUMP_DIR "/data/pbs/ps4"
#define PBS_PS4_DUMP_MODULES 8u
#define PBS_PS4_DUMP_SYMBOLS 32u
#define PBS_PS4_DUMP_LINE 1024u

static int read_kernel(void *opaque, uint64_t at, void *out, size_t size) {
  (void)opaque;
  return kernel_copyout((intptr_t)at, out, size);
}

static pid_t find_pid(const char *name) {
  int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) || !size || size > 4u * 1024u * 1024u)
    return -1;
  unsigned char *list = malloc(size);
  if (!list) return -1;
  if (sysctl(mib, 4, list, &size, NULL, 0)) {
    free(list);
    return -1;
  }
  pid_t result = -1;
  for (size_t pos = 0; size - pos >= sizeof(struct kinfo_proc);) {
    const struct kinfo_proc *process = (const struct kinfo_proc *)(list + pos);
    if (process->ki_structsize <= 0 ||
        (size_t)process->ki_structsize < sizeof *process ||
        (size_t)process->ki_structsize > size - pos)
      break;
    if (!strcmp(process->ki_comm, name)) {
      result = process->ki_pid;
      break;
    }
    pos += (size_t)process->ki_structsize;
  }
  free(list);
  return result;
}

static void emit(FILE *stream, const char *line, size_t size, int written) {
  if (written < 0 || (size_t)written >= size) {
    fprintf(stream, "truncated line\n");
    return;
  }
  fprintf(stream, "%s\n", line);
}

typedef struct {
  const char *module;
  unsigned handle;
  uintptr_t mapbase;
  uintptr_t mapsize;
  intptr_t entry;
  int ok;
  int printed;
} pbs_ps4_dump_module;

/* Resolve a module's load information. kernel_dynlib_mapbase_addr() is the
 * authoritative base; the dynlib object only refines the span when its layout
 * matches what the platform diagnostics already accept. */
static int module_info(pid_t pid, const char *module, pbs_ps4_dump_module *out) {
  uint32_t handle = 0;
  if (kernel_dynlib_handle(pid, module, &handle)) return -1;
  intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
  if (base <= 0) return -1;
  uintptr_t size = 0;
  dynlib_obj_t object;
  if (!kernel_dynlib_obj(pid, handle, &object) &&
      object.mapbase == (uintptr_t)base && object.mapsize)
    size = object.mapsize;
  out->module = module;
  out->handle = handle;
  out->mapbase = (uintptr_t)base;
  out->mapsize = size;
  out->entry = kernel_dynlib_entry_addr(pid, handle);
  out->ok = 1;
  return 0;
}

static const pbs_ps4_dump_module *module_for(pid_t pid, const char *process,
                                             const char *module,
                                             pbs_ps4_dump_module *modules,
                                             size_t *count, FILE *report) {
  for (size_t i = 0; i < *count; ++i)
    if (!strcmp(modules[i].module, module)) return &modules[i];
  if (*count >= PBS_PS4_DUMP_MODULES) return NULL;
  pbs_ps4_dump_module *slot = &modules[(*count)++];
  memset(slot, 0, sizeof *slot);
  slot->module = module;
  if (module_info(pid, module, slot)) {
    slot->ok = 0;
    fprintf(report, "module process=%s name=%s status=unavailable\n",
            process, module);
  } else {
    char line[PBS_PS4_DUMP_LINE];
    emit(report, line, sizeof line,
         pbs_ps4_dump_module_line(line, sizeof line, process, module,
                                  slot->handle, slot->mapbase, slot->mapsize,
                                  slot->entry));
    slot->printed = 1;
  }
  return slot;
}

static unsigned dump_process(FILE *report, FILE *offsets, const char *name,
                             long long *seen, size_t seen_count) {
  pid_t pid = find_pid(name);
  fprintf(report, "process name=%s pid=%d\n", name, (int)pid);
  if (pid <= 0) {
    fprintf(report, "process name=%s status=unavailable\n", name);
    return 0; /* Missing exports are counted once, after both processes. */
  }
  size_t catalog_count = 0;
  const pbs_ps4_dump_symbol *catalog = pbs_ps4_dump_symbols(&catalog_count);
  pbs_ps4_dump_module modules[PBS_PS4_DUMP_MODULES];
  size_t module_count = 0;
  unsigned failures = 0;
  char line[PBS_PS4_DUMP_LINE];
  for (size_t i = 0; i < catalog_count; ++i) {
    const pbs_ps4_dump_symbol *entry = &catalog[i];
    const pbs_ps4_dump_module *module =
        module_for(pid, name, entry->module, modules, &module_count, report);
    if (!module) continue;
    intptr_t address = 0;
    long long rva = -1;
    if (module->ok) {
      address = kernel_dynlib_dlsym(pid, module->handle, entry->symbol);
      rva = pbs_ps4_dump_rva(address, (intptr_t)module->mapbase,
                             module->mapsize);
    }
    emit(report, line, sizeof line,
         pbs_ps4_dump_symbol_line(line, sizeof line, name, entry->label,
                                  entry->module, address, rva));
    if (i < seen_count) {
      if (rva < 0) {
        /* An unresolved export must never erase a value measured elsewhere. */
        fprintf(offsets, "# %s unresolved in %s\n", entry->label, name);
      } else if (seen[i] < 0) {
        seen[i] = rva;
        emit(offsets, line, sizeof line,
             pbs_ps4_dump_offset_line(line, sizeof line, entry->label, rva));
      } else if (seen[i] != rva) {
        /* Same export, different address: a real anomaly worth grepping. */
        ++failures;
        fprintf(offsets, "# %s differs in %s: 0x%llx (first 0x%llx)\n",
                entry->label, name, rva, seen[i]);
      }
    }
  }
  return failures;
}

static unsigned dump_kernel(FILE *report, FILE *offsets, unsigned firmware) {
  /* The SDK exports the base as a negative intptr_t; `available` is the only
   * supported test. Probing first makes a read failure distinguishable from a
   * pattern mismatch in the report instead of a silent "unavailable". */
  const intptr_t base = KERNEL_ADDRESS_IMAGE_BASE;
  const int available = pbs_ps4_layout_available(base, KERNEL_IMAGE_SIZE);
  unsigned char magic[8] = {0};
  int probe = -1;
  if (available)
    probe = read_kernel(NULL, (uint64_t)base, magic, sizeof magic);
  char line[PBS_PS4_DUMP_LINE];
  emit(report, line, sizeof line,
       pbs_ps4_dump_kernel_probe_line(line, sizeof line, (uint64_t)base,
                                      KERNEL_IMAGE_SIZE, probe, magic,
                                      sizeof magic));
  pbs_ps4_firmware layout;
  memset(&layout, 0, sizeof layout);
  int rc = -1;
  if (available)
    rc = pbs_ps4_layout_kernel(read_kernel, NULL, (uint64_t)base,
                               KERNEL_IMAGE_SIZE, &layout);
  const pbs_ps4_firmware *found = rc ? NULL : &layout;
  emit(report, line, sizeof line,
       pbs_ps4_dump_kernel_line(line, sizeof line, found, firmware));
  if (!found) {
    fprintf(offsets, "kernel_layout=unavailable\n");
    if (available && !probe) {
      char path[160], partial[168];
      snprintf(path,sizeof path,PBS_PS4_DUMP_DIR "/kernel-code-%08x.bin",firmware);
      snprintf(partial,sizeof partial,"%s.part",path);
      FILE *image=fopen(partial,"wb");
      int saved=image ? pbs_ps4_dump_kernel_code(read_kernel,NULL,(uint64_t)base,
                                    KERNEL_IMAGE_SIZE,image,report) : -1;
      if(image && fclose(image))saved=-1;
      if(!saved && rename(partial,path))saved=-1;
      if(saved)unlink(partial);
      fprintf(report,"kernel code status=%s path=%s format=memory-layout-executable-only\n",
              saved?"failed":"saved",path);
    }
    return 1;
  }
  pbs_ps4_dump_offset_line(line, sizeof line, "acmgr_offset",
                           (long long)found->acmgr_offset);
  fprintf(offsets, "%s\n", line);
  pbs_ps4_dump_offset_line(line, sizeof line, "gate0_offset",
                           (long long)found->gates[0].offset);
  fprintf(offsets, "%s\n", line);
  pbs_ps4_dump_offset_line(line, sizeof line, "gate1_offset",
                           (long long)found->gates[1].offset);
  fprintf(offsets, "%s\n", line);
  int written = pbs_ps4_dump_snippet(line, sizeof line, found, firmware);
  if (written < 0 || (size_t)written >= sizeof line) {
    fprintf(report, "kernel snippet truncated\n");
  } else {
    fprintf(report, "kernel snippet:\n%s", line);
  }
  return 0;
}

int main(void) {
  mkdir("/data/pbs", 0777);
  mkdir(PBS_PS4_DUMP_DIR, 0777);
  FILE *report = fopen(PBS_PS4_DUMP_DIR "/dump.txt", "w");
  FILE *offsets = fopen(PBS_PS4_DUMP_DIR "/offsets.txt", "w");
  if (!report || !offsets) {
    if (report) fclose(report);
    if (offsets) fclose(offsets);
    return 1;
  }
  setvbuf(report, NULL, _IONBF, 0);
  setvbuf(offsets, NULL, _IONBF, 0);
  unsigned firmware = kernel_get_fw_version();
  fprintf(report, "PS4-DUMP v1 firmware=0x%08x kernel_base=0x%llx image_size=0x%zx\n",
          firmware, (unsigned long long)KERNEL_ADDRESS_IMAGE_BASE,
          KERNEL_IMAGE_SIZE);
  fprintf(offsets, "# PS4-DUMP v1 firmware=0x%08x\n", firmware);
  unsigned failures = dump_kernel(report, offsets, firmware);
  size_t catalog_count = 0;
  (void)pbs_ps4_dump_symbols(&catalog_count);
  long long seen[PBS_PS4_DUMP_SYMBOLS];
  if (catalog_count > PBS_PS4_DUMP_SYMBOLS) catalog_count = PBS_PS4_DUMP_SYMBOLS;
  for (size_t i = 0; i < catalog_count; ++i) seen[i] = -1;
  failures += dump_process(report, offsets, "SceRemotePlay", seen, catalog_count);
  failures += dump_process(report, offsets, "SceAvCapture", seen, catalog_count);
  unsigned missing=0;
  for(size_t i=0;i<catalog_count;i++)if(seen[i]<0)missing++;
  failures+=missing;
  fprintf(report,"symbols resolved=%zu missing=%u\n",catalog_count-missing,missing);
  fprintf(report, "PS4-DUMP finished failures=%u\n", failures);
  fprintf(offsets, "# failures=%u\n", failures);
  int report_ok = fclose(report);
  int offsets_ok = fclose(offsets);
  return (report_ok || offsets_ok || failures) ? 1 : 0;
}
