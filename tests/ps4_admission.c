// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps4_admission.h"
#include "ps4_snapshot.h"
#include <ps4/kernel.h>
#include <assert.h>
#include <string.h>
static int mode, snapshots;
enum {OK, NO_MAPPING, SHORT_MAPPING, SIGNATURE_FAIL, READ_FAIL};
pid_t pbs_find_pid(const char *name) { assert(!strcmp(name, "SceAvCapture")); return 47; }
uint64_t pbs_process_birth_epoch(pid_t pid) { assert(pid == 47); return 41; }
int kernel_dynlib_obj(pid_t pid, uint32_t handle, dynlib_obj_t *obj) {
  assert(pid == 47 && handle == 0);
  obj->mapbase = 0x400000; obj->mapsize = mode == SHORT_MAPPING ? 0x1000 : 0x30000;
  return mode == NO_MAPPING ? -1 : 0;
}
int pbs_ps4_read_snapshot(pid_t pid, uint64_t epoch,
                          const pbs_ps4_snapshot_region *regions, size_t count) {
  assert(pid == 47 && epoch == 41 && count == 4); ++snapshots;
  const pbs_ps4_firmware *fw = pbs_ps4_firmware_find(0x11008001);
  assert(regions[0].address == 0x400000 + fw->capture_check);
  assert(regions[1].address == 0x400000 + fw->capture_caps);
  assert(regions[2].address == 0x400000 + fw->capture_flags);
  assert(regions[3].address == 0x400000 + fw->capture_slots);
  memcpy(regions[0].destination, fw->capture_check_signature, 16);
  memcpy(regions[1].destination, fw->capture_caps_signature, 16);
  *(uint32_t *)regions[2].destination = 0x100004;
  *(uint32_t *)regions[3].destination = 0;
  if (mode == SIGNATURE_FAIL) ((unsigned char *)regions[0].destination)[0] ^= 1;
  return mode == READ_FAIL ? -1 : 0;
}
int main(void) {
  assert(pbs_ps4_admission_caps(0x100000, 2) == 0);
  assert(pbs_ps4_admission_caps(0x100004, 2) == 4);
  assert(pbs_ps4_admission_caps(0x300000, 2) == 0x100000);
  assert(pbs_ps4_admission_caps(0x300080, 2) == 0);
  assert(pbs_ps4_admission_caps(0x300200, 20) == 0);
  assert(pbs_ps4_admission_caps(0x300000, 20) == 0x200);
  assert(pbs_ps4_admission_caps(0x300004,24)==0);
  assert(pbs_ps4_admission_caps(0x200004,24)==0);
  assert(pbs_ps4_admission_caps(0x100004,24)==0x200000);
  assert(pbs_ps4_admission_caps(0, 19) == UINT32_MAX);
  FILE *log = tmpfile(); assert(log);
  const pbs_ps4_firmware *fw = pbs_ps4_firmware_find(0x11008001);
  assert(pbs_ps4_admission_snapshot(log, NULL, "invalid", 2) < 0);
  assert(pbs_ps4_admission_snapshot(log, fw, "invalid", 19) < 0);
  assert(!snapshots);
  for (mode = OK; mode <= READ_FAIL; ++mode)
    assert((pbs_ps4_admission_snapshot(log, fw, "test", 2) == 0) == (mode == OK));
  assert(snapshots == 3);
  fclose(log);
  puts("Admission diagnostics: profile caps, mapping bounds, signatures and read refusal verified");
}
