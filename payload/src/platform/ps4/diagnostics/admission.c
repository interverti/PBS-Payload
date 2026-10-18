// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/diagnostics/admission.h"
#include "platform/ps4/diagnostics/snapshot.h"
#include "common/process/inject.h"
#include <ps4/kernel.h>
#include <string.h>
#include <time.h>

int pbs_ps4_admission_snapshot(FILE *log, const pbs_ps4_firmware *fw,
                                const char *stage, unsigned profile) {
  const pbs_ps4_firmware *known = pbs_ps4_firmware_find(kernel_get_fw_version());
  if (!log || !fw || !known || fw->version != known->version ||
      (profile != 2 && profile != 20 && profile != 24)) return -1;
  pid_t pid = pbs_find_pid("SceAvCapture");
  uint64_t epoch = pid > 0 ? pbs_process_birth_epoch(pid) : 0;
  dynlib_obj_t module = {0};
  /* Main executable is handle 0. Refuse an unknown/missing mapping. */
  if (!epoch || kernel_dynlib_obj(pid, 0, &module) || !module.mapbase ||
      module.mapsize > INTPTR_MAX ||
      module.mapbase > INTPTR_MAX - module.mapsize ||
      fw->capture_flags + 4u > module.mapsize ||
      fw->capture_slots + 4u > module.mapsize ||
      fw->capture_check + 16u > module.mapsize ||
      fw->capture_caps + 16u > module.mapsize) {
    fprintf(log, "Admission snapshot %s: process/main mapping unavailable\n", stage);
    return -1;
  }
  uint32_t flags = 0, slots = 0;
  unsigned char check[16], caps_code[16];
  const pbs_ps4_snapshot_region regions[] = {
    {(intptr_t)(module.mapbase + fw->capture_check), check, sizeof check},
    {(intptr_t)(module.mapbase + fw->capture_caps), caps_code, sizeof caps_code},
    {(intptr_t)(module.mapbase + fw->capture_flags), &flags, sizeof flags},
    {(intptr_t)(module.mapbase + fw->capture_slots), &slots, sizeof slots}
  };
  struct timespec before, after;
  clock_gettime(CLOCK_MONOTONIC, &before);
  int rc = pbs_ps4_read_snapshot(pid, epoch, regions, 4);
  clock_gettime(CLOCK_MONOTONIC, &after);
  long long elapsed_us = (after.tv_sec - before.tv_sec) * 1000000LL +
                        (after.tv_nsec - before.tv_nsec) / 1000;
  if (rc || memcmp(check, fw->capture_check_signature, sizeof check) ||
      memcmp(caps_code, fw->capture_caps_signature, sizeof caps_code)) {
    fprintf(log, "Admission snapshot %s: rc=%d signature/read refused elapsed_us=%lld\n",
            stage, rc, elapsed_us);
    return -1;
  }
  uint32_t caps = pbs_ps4_admission_caps(flags, profile);
  unsigned caps_rc = caps ? ((caps & 0x3f0a60) ? 0x80a7000a : 0x80a70008) : 0;
  fprintf(log, "Admission snapshot %s: pid=%d raw_flags=%08x slots=%08x profile=%u caps=%08x caps_stage_rc=%08x elapsed_us=%lld (read-only; later checks unresolved)\n",
          stage, (int)pid, flags, slots, profile, caps, caps_rc, elapsed_us);
  return 0;
}

static uint32_t snapshot_u32(const unsigned char *p, size_t offset) {
  uint32_t value; memcpy(&value, p + offset, sizeof value); return value;
}
static uint64_t snapshot_u64(const unsigned char *p, size_t offset) {
  uint64_t value; memcpy(&value, p + offset, sizeof value); return value;
}
static int snapshot_pointer(uint64_t pointer, size_t size) {
  return pointer >= 0x10000 && !(pointer & 7) &&
      pointer <= INT64_MAX - size;
}
static int encoder_read_region(FILE *log, pid_t pid, uint64_t epoch,
                                  const pbs_ps4_snapshot_region *region,
                                  const char *stage) {
  int rc = pbs_ps4_read_snapshot(pid, epoch, region, 1);
  if (rc) fprintf(log, "Encoder snapshot read failed stage=%s address=%llx bytes=%zu rc=%d\n",
      stage, (unsigned long long)region->address, region->size, rc);
  return rc;
}
int pbs_ps4_encoder_snapshot(FILE *log, const pbs_ps4_firmware *fw,
                              const char *path) {
  const pbs_ps4_firmware *known = pbs_ps4_firmware_find(kernel_get_fw_version());
  if (!log || !path || !fw || !known || fw->version != known->version) return -1;
  pid_t pid = pbs_find_pid("SceAvCapture");
  uint64_t epoch = pid > 0 ? pbs_process_birth_epoch(pid) : 0;
  dynlib_obj_t module = {0};
  if (!epoch || kernel_dynlib_obj(pid, 0, &module) || !module.mapbase ||
      module.mapsize > INTPTR_MAX || module.mapbase > INTPTR_MAX - module.mapsize ||
      fw->capture_object_head + 8u > module.mapsize ||
      fw->capture_profile2_vtable + 16u > module.mapsize) return -1;
  unsigned char service[0x378], core[0xb20], backend[0x1000];
  unsigned char driver[0x1100], codec[0x1000], sequence[0x88];
  uint64_t node = 0;
  pbs_ps4_snapshot_region region = {
    (intptr_t)(module.mapbase + fw->capture_object_head), &node, sizeof node};
  int rc = encoder_read_region(log, pid, epoch, &region, "head");
  if (rc) return rc;
  unsigned steps;
  for (steps = 0; node && steps < 8; ++steps) {
    if (!snapshot_pointer(node, sizeof service)) return -1;
    region = (pbs_ps4_snapshot_region){(intptr_t)node, service, sizeof service};
    if ((rc = encoder_read_region(log, pid, epoch, &region, "service"))) return rc;
    if (snapshot_u64(service, 0) == module.mapbase + fw->capture_profile2_vtable &&
        (snapshot_u32(service, 0x80) == 2 || snapshot_u32(service, 0x80) == 24 ||
         snapshot_u32(service, 0x80) == 19) &&
        (snapshot_u32(service, 0x84) == 2 || snapshot_u32(service, 0x84) == 19
#if PBS_PS4_NATIVE_SOURCE
         || (snapshot_u32(service, 0x80) == 24 && snapshot_u32(service, 0x84) == 24)
#endif
        )) break;
    uint64_t next = snapshot_u64(service, 8);
    if (next == node) return -1;
    node = next;
  }
  if (!node || steps == 8) return -1;
  uint64_t encoder = snapshot_u64(service, 0x188);
  if (!snapshot_pointer(encoder, sizeof core)) {
    fprintf(log, "Encoder snapshot pointer refused stage=core address=%llx\n", (unsigned long long)encoder);
    return -1;
  }
  region = (pbs_ps4_snapshot_region){(intptr_t)encoder, core, sizeof core};
  if ((rc = encoder_read_region(log, pid, epoch, &region, "core"))) return rc;
  uint64_t low = snapshot_u64(core, 0x1e8);
  if (!snapshot_pointer(low, sizeof backend)) {
    fprintf(log, "Encoder snapshot pointer refused stage=backend address=%llx\n", (unsigned long long)low);
    return -1;
  }
  region = (pbs_ps4_snapshot_region){(intptr_t)low, backend, sizeof backend};
  if ((rc = encoder_read_region(log, pid, epoch, &region, "backend"))) return rc;
  uint64_t driver_address = snapshot_u64(backend, 0x4b8);
  if (!snapshot_pointer(driver_address, sizeof driver)) {
    fprintf(log, "Encoder snapshot pointer refused stage=driver address=%llx\n", (unsigned long long)driver_address);
    return -1;
  }
  region = (pbs_ps4_snapshot_region){(intptr_t)driver_address, driver, 0x1000};
  if ((rc = encoder_read_region(log, pid, epoch, &region, "driver"))) return rc;
  region = (pbs_ps4_snapshot_region){(intptr_t)(driver_address + 0x1000),
                                   driver + 0x1000, 0x100};
  if ((rc = encoder_read_region(log, pid, epoch, &region, "driver-tail"))) return rc;
  /* Native e440 follows driver+d50 to the codec context; be50/c150 read
   * its sequence descriptor through +a70. These are diagnostic reads only. */
  uint64_t codec_address = snapshot_u64(driver, 0xd50);
  if (!snapshot_pointer(codec_address, sizeof codec)) {
    fprintf(log, "Encoder snapshot pointer refused stage=codec address=%llx\n", (unsigned long long)codec_address);
    return -1;
  }
  region = (pbs_ps4_snapshot_region){(intptr_t)codec_address, codec, sizeof codec};
  if ((rc = encoder_read_region(log, pid, epoch, &region, "codec"))) return rc;
  uint64_t sequence_address = snapshot_u64(codec, 0xa70);
  if (!snapshot_pointer(sequence_address, sizeof sequence)) {
    fprintf(log, "Encoder snapshot pointer refused stage=sequence address=%llx\n", (unsigned long long)sequence_address);
    return -1;
  }
  region = (pbs_ps4_snapshot_region){(intptr_t)sequence_address, sequence, sizeof sequence};
  if ((rc = encoder_read_region(log, pid, epoch, &region, "sequence"))) return rc;
  /* Recheck the same list node and handle after following the pointers. */
  unsigned char verify[0x190];
  region = (pbs_ps4_snapshot_region){(intptr_t)node, verify, sizeof verify};
  if ((rc = encoder_read_region(log, pid, epoch, &region, "verify"))) return rc;
  if (memcmp(verify, service, 16) || snapshot_u64(verify, 0x188) != encoder)
    return -1;
  FILE *out = fopen(path, "wb");
  if (!out) return -1;
  /* Versioned, self-describing local evidence. Each record is address,
   * byte count (both LE uint64 on Orbis), then that many raw bytes. */
  const unsigned char magic[8] = {'P','B','S','E','S','T','0','2'};
  const struct { uint64_t address, size; const void *bytes; } blocks[] = {
    {node, sizeof service, service}, {encoder, sizeof core, core},
    {low, sizeof backend, backend}, {driver_address, sizeof driver, driver},
    {codec_address, sizeof codec, codec}, {sequence_address, sizeof sequence, sequence}
  };
  int failed = fwrite(magic, 1, sizeof magic, out) != sizeof magic;
  for (size_t i = 0; !failed && i < sizeof blocks / sizeof blocks[0]; ++i) {
    uint64_t header[2] = {blocks[i].address, blocks[i].size};
    failed = fwrite(header, 1, sizeof header, out) != sizeof header ||
        fwrite(blocks[i].bytes, 1, blocks[i].size, out) != blocks[i].size;
  }
  if (fclose(out)) failed = 1;
  fprintf(log, "Encoder snapshot: rc=%d path=%s service=%llx core=%llx backend=%llx "
          "preset=%ux%u pitch=%u core_dimensions=%ux%u core_pitch=%u "
          "backend_state=%u (read-only PBSEST02; six records)\n",
          failed ? -1 : 0, path, (unsigned long long)node,
          (unsigned long long)encoder, (unsigned long long)low,
          snapshot_u32(service, 0x1c8), snapshot_u32(service, 0x1d0),
          snapshot_u32(service, 0x1cc), snapshot_u32(core, 0x50),
          snapshot_u32(core, 0x54), snapshot_u32(core, 0x28),
          snapshot_u32(backend, 0x4c0));
  return failed ? -1 : 0;
}
