// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps4_latch_patch.h"
#include "inject.h"
#include <ps4/kernel.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

static struct {
  pid_t pid;
  uint64_t epoch;
  uintptr_t base;
  const pbs_ps4_firmware *fw;
  unsigned owned;
  unsigned budget_active;
} patch;

int pbs_ps4_latch_patch_pending(void) { return patch.owned != 0; }

/* Measured FW11 SceAvCapture idle owner: capture_flags=0x100004. Message bit
 * 0x200 (dword byte 1 bit 1) is the decoder core budget switch OpenVideoEx
 * reads: clear = 24 MiB (0x1800000), set = 54 MiB (0x3600000). */
#define PBS_LATCH_IDLE_FLAGS 0x100004u
#define PBS_LATCH_BUDGET_BIT 0x200u

static int io(pid_t pid, uintptr_t address, void *data, size_t size, int write) {
  /* Same Orbis PT_IO write operation used by the validated call trampoline. */
  struct ptrace_io_desc desc = {.piod_op = write ? PIOD_WRITE_D : PIOD_READ_D,
      .piod_offs = (void *)address, .piod_addr = data, .piod_len = size};
  return ptrace(PT_IO, pid, (caddr_t)&desc, 0) || desc.piod_len != size ? -1 : 0;
}

static int stopped(void) {
  for (unsigned elapsed = 0; elapsed < 250; elapsed += 5) {
    int status = 0;
    pid_t got = waitpid(patch.pid, &status, WNOHANG | WUNTRACED);
    if (got == patch.pid) return WIFSTOPPED(status) ? 0 : -1;
    if (got < 0 && errno != EINTR) return -1;
    usleep(5000);
  }
  return -1;
}

static int put(unsigned index, int restore) {
  const pbs_ps4_text_site *site = &patch.fw->capture_latch_sites[index];
  const unsigned char *bytes = restore ? site->before : site->after;
  unsigned char verify[16];
  uintptr_t address = patch.base + site->offset;
  unsigned changed = 1;
  while (changed < site->size && site->before[changed] == site->after[changed]) ++changed;
  if (changed == site->size ||
      io(patch.pid, address + changed, (void *)(bytes + changed), 1, 1) ||
      io(patch.pid, address, verify, site->size, 0) ||
      memcmp(verify, bytes, site->size)) return -1;
  if (restore) patch.owned &= ~(1u << index);
  return 0;
}

/* The five-byte AND keeps the same opcode and instruction boundaries. Only
 * its immediate byte is written under a process stop; no cave can be active. */
static int rollback(void) {
  for (unsigned i = 0; i < PBS_PS4_LATCH_SITE_COUNT; ++i)
    if ((patch.owned & (1u << i)) && put(i, 1)) return -1;
  return 0;
}

static int exchange(FILE *log, int restore) {
  if (pbs_process_birth_epoch(patch.pid) != patch.epoch ||
      ptrace(PT_ATTACH, patch.pid, NULL, 0)) return -1;
  int result = -1, detached = 0;
  const char *stage = "wait-stop";
  if (stopped()) goto done;
  stage = "identity";
  if (pbs_process_birth_epoch(patch.pid) != patch.epoch) goto done;
  stage = "caps-signature";
  unsigned char bytes[16];
  if (io(patch.pid, patch.base + patch.fw->capture_caps, bytes, sizeof bytes, 0) ||
      memcmp(bytes, patch.fw->capture_caps_signature, sizeof bytes)) goto done;
  stage = "patch-site-signatures";
  for (unsigned i = 0; i < PBS_PS4_LATCH_SITE_COUNT; ++i) {
    const pbs_ps4_text_site *site = &patch.fw->capture_latch_sites[i];
    const unsigned char *expected = restore && (patch.owned & (1u << i)) ? site->after : site->before;
    if (io(patch.pid, patch.base + site->offset, bytes, site->size, 0)) goto done;
    /* A prior failed verification may already have restored this owned site. */
    if (restore && !memcmp(bytes, site->before, site->size)) {
      patch.owned &= ~(1u << i);
    } else if (memcmp(bytes, expected, site->size)) {
      int owned_partial = restore && (patch.owned & (1u << i));
      for (unsigned j = 0; owned_partial && j < site->size; ++j)
        if (bytes[j] != site->before[j] && bytes[j] != site->after[j]) owned_partial = 0;
      if (owned_partial) continue;
      fprintf(log, "Latch patch: foreign bytes at +%x, refusing overwrite\n", site->offset);
      goto done;
    }
  }
  if (restore) {
    stage = "restore-writes";
    result = rollback();
  } else {
    stage = "owner-state";
    uint32_t flags, slots;
    if (io(patch.pid, patch.base + patch.fw->capture_flags, &flags, 4, 0) ||
        io(patch.pid, patch.base + patch.fw->capture_slots, &slots, 4, 0) ||
        flags !=
#if PBS_PS4_NATIVE_SOURCE
            0x300004u || slots) {
#else
            PBS_LATCH_IDLE_FLAGS || slots) {
#endif
      fprintf(log, "Latch patch: admission state differs from the measured idle owner; refused\n");
      goto done;
    }
    const unsigned order[] = {0};
    stage = "install-writes";
    for (unsigned j = 0; j < PBS_PS4_LATCH_SITE_COUNT; ++j) {
      unsigned i = order[j];
      patch.owned |= 1u << i; /* PT_IO can partially write before failing. */
      if (put(i, 0)) {
        if (rollback()) fprintf(log, "Latch patch: rollback incomplete; reboot required if restore keeps failing\n");
        goto done;
      }
    }
    result = 0;
  }
done:
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (!ptrace(PT_DETACH, patch.pid, NULL, 0)) { detached = 1; break; }
    usleep(5000);
  }
  if (!detached) {
    kill(patch.pid, SIGCONT);
    result = -1;
  }
  fprintf(log, "Latch patch: %s rc=%d pending=%x stage=%s (global flags untouched)\n",
          restore ? "restore" : "apply", result, patch.owned, stage);
  return result;
}

/* Shared process-state provision: locate the daemon, validate the module
 * boundaries and cache pid/epoch/base/firmware. The latch variant also
 * validates the five-byte sites; the budget variant may run without any
 * latch apply earlier in the process. */
static int provision_state(const pbs_ps4_firmware *fw, int latch_needed) {
  const pbs_ps4_firmware *known = pbs_ps4_firmware_find(kernel_get_fw_version());
  if (!fw || !known || fw->version != known->version) return -1;
#if PBS_PS4_NATIVE_SOURCE
  /* Standard hardware only: remove the client XOR of the Neo capability,
   * never set the global Neo/budget bit. Only this immediate byte differs.
   * 35 00 02 00 00 -> 35 00 00 00 00 at FW11 RVA9d7d. */
  static pbs_ps4_firmware source_fw;
  source_fw = *fw;
  source_fw.capture_latch_sites[0] = (pbs_ps4_text_site){
    0x9d7d,5,{0x35,0x00,0x02,0x00,0x00},{0x35,0x00,0x00,0x00,0x00}};
  fw = &source_fw;
#endif
  if (latch_needed && patch.owned) return -1;
  pid_t pid = pbs_find_pid("SceAvCapture");
  uint64_t epoch = pid > 0 ? pbs_process_birth_epoch(pid) : 0;
  dynlib_obj_t module = {0};
  if (!epoch || kernel_dynlib_obj(pid, 0, &module) || !module.mapbase ||
      module.mapsize > INTPTR_MAX || module.mapbase > INTPTR_MAX - module.mapsize ||
      fw->capture_flags + 4u > module.mapsize || fw->capture_slots + 4u > module.mapsize ||
      fw->capture_caps + 0x70u > module.mapsize) return -1;
  if (latch_needed)
    for (unsigned i = 0; i < PBS_PS4_LATCH_SITE_COUNT; ++i)
      if (fw->capture_latch_sites[i].size != 5 ||
#if PBS_PS4_NATIVE_SOURCE
          fw->capture_latch_sites[i].before[0] != 0x35 ||
          fw->capture_latch_sites[i].after[0] != 0x35 ||
          fw->capture_latch_sites[i].before[2] != 2 ||
          fw->capture_latch_sites[i].after[2] != 0 ||
#else
          fw->capture_latch_sites[i].before[0] != 0x25 ||
          fw->capture_latch_sites[i].after[0] != 0x25 ||
          memcmp(fw->capture_latch_sites[i].before + 2, fw->capture_latch_sites[i].after + 2, 3) ||
#endif
          fw->capture_latch_sites[i].offset + 16u > module.mapsize) return -1;
  patch.pid = pid; patch.epoch = epoch; patch.base = module.mapbase; patch.fw = fw;
  return 0;
}


#if PBS_PS4_BUDGET_UNLOCK
/* Arm or clear the daemon decoder core-budget bit while the process is
 * stopped. Arm accepts only the measured idle dword or a stale armed
 * leftover from a lost run, which is adopted so this run can clear it.
 * Restore clears only bit 0x200 so any daemon-set bits survive. */
static int budget_exchange(FILE *log, int restore) {
  if (restore ? !patch.budget_active : patch.budget_active) return 0;
  if (pbs_process_birth_epoch(patch.pid) != patch.epoch ||
      ptrace(PT_ATTACH, patch.pid, NULL, 0)) return -1;
  int result = -1, detached = 0;
  const char *stage = "wait-stop";
  uint32_t flags = 0;
  if (stopped()) goto done;
  stage = "identity";
  if (pbs_process_birth_epoch(patch.pid) != patch.epoch) goto done;
  stage = "caps-signature";
  unsigned char bytes[16];
  if (io(patch.pid, patch.base + patch.fw->capture_caps, bytes, sizeof bytes, 0) ||
      memcmp(bytes, patch.fw->capture_caps_signature, sizeof bytes)) goto done;
  stage = "budget-state";
  if (io(patch.pid, patch.base + patch.fw->capture_flags, &flags, 4, 0)) goto done;
  if (restore) {
    flags &= ~PBS_LATCH_BUDGET_BIT;
    stage = "budget-clear";
    if (io(patch.pid, patch.base + patch.fw->capture_flags, &flags, 4, 1) ||
        io(patch.pid, patch.base + patch.fw->capture_flags, &flags, 4, 0) ||
        (flags & PBS_LATCH_BUDGET_BIT)) {
      fprintf(log, "Budget unlock: restore verify failed; armed bit stays pending\n");
      goto done;
    }
    patch.budget_active = 0;
    result = 0;
  } else {
    if (flags == PBS_LATCH_IDLE_FLAGS) {
      stage = "budget-arm";
    } else if (flags == (PBS_LATCH_IDLE_FLAGS | PBS_LATCH_BUDGET_BIT)) {
      fprintf(log, "Budget unlock: armed bit already present; adopting for restore\n");
      stage = "budget-adopt";
    } else {
      fprintf(log, "Budget unlock: capture_flags %08x differ from the measured idle state; refused\n",
              flags);
      goto done;
    }
    patch.budget_active = 1; /* PT_IO can partially write before failing. */
    flags |= PBS_LATCH_BUDGET_BIT;
    if (io(patch.pid, patch.base + patch.fw->capture_flags, &flags, 4, 1) ||
        io(patch.pid, patch.base + patch.fw->capture_flags, &flags, 4, 0) ||
        flags != (PBS_LATCH_IDLE_FLAGS | PBS_LATCH_BUDGET_BIT)) {
      patch.budget_active = 0;
      fprintf(log, "Budget unlock: arm verify failed; a stale armed bit may persist\n");
      goto done;
    }
    result = 0;
  }
done:
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (!ptrace(PT_DETACH, patch.pid, NULL, 0)) { detached = 1; break; }
    usleep(5000);
  }
  if (!detached) {
    kill(patch.pid, SIGCONT);
    result = -1;
  }
  fprintf(log, "Budget unlock: %s rc=%d armed=%d flags=%08x stage=%s\n",
          restore ? "restore" : "arm", result, patch.budget_active ? 1 : 0, flags, stage);
  return result;
}


int pbs_ps4_latch_budget_pending(void) { return patch.budget_active != 0; }

/* Arm or clear the decoder budget bit. Both directions retry twice against
 * transient tracing failures; the armed bit is a soft decoder threshold and
 * a lost owner self-heals on the next adopt-then-clear cycle. Works with or
 * without a prior latch patch; the state provision is self-contained. */
int pbs_ps4_latch_budget_set(FILE *log, unsigned enable) {
  const pbs_ps4_firmware *fw = pbs_ps4_firmware_find(kernel_get_fw_version());
  if (enable ? patch.budget_active != 0 : patch.budget_active == 0) return 0;
  if (!log || (!patch.pid && provision_state(fw, 0))) return -1;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (!budget_exchange(log, !enable)) return 0;
    usleep(5000);
  }
  return -1;
}
#endif

int pbs_ps4_latch_patch_apply(FILE *log, const pbs_ps4_firmware *fw) {
  if (!log || patch.owned) return -1;
  if (provision_state(fw, 1)) return -1;
  return exchange(log, 0);
}

int pbs_ps4_latch_patch_restore(FILE *log) {
  if (!patch.owned) return 0;
  if (!log) return -1;
  /* Retry transient tracing failures only while restoring owned bytes. */
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    if (!exchange(log, 1)) return 0;
    usleep(5000);
  }
  return -1;
}
