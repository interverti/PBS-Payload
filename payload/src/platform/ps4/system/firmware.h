// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* Private kernel/module layouts belong here, never in transport or lifecycle.
 * A profile is evidence of support, not a guess from a nearby firmware number.
 * Export resolution remains dynamic; all kernel patch bytes are checked before
 * the first write. Add another profile only with matching ABI evidence/tests. */
#include <stdint.h>
#include <stddef.h>

#define PBS_PS4_GATE_COUNT 2u
#define PBS_PS4_LATCH_SITE_COUNT 1u

typedef struct {
  unsigned offset;
  unsigned char before[5], after[5];
  size_t size;
} pbs_ps4_gate;
typedef struct {
  unsigned offset, size;
  unsigned char before[16], after[16];
} pbs_ps4_text_site;

typedef struct pbs_ps4_firmware {
  const char *name;
  uint32_t version, version_mask;
  unsigned acmgr_offset;
  unsigned char acmgr_signature[3];
  pbs_ps4_gate gates[PBS_PS4_GATE_COUNT];
  unsigned avcap_init_offset, avcap_trap_offset;
  unsigned ready_delta, dequeue_delta, consume_delta;
  unsigned ring_state_offset;
  unsigned rp_nn_socket, rp_vtable, rp_data, rp_data_size, rp_ensure_open;
  /* Read-only SceAvCapture diagnostics, never patch targets. */
  unsigned capture_flags, capture_slots, capture_check, capture_caps;
  unsigned capture_object_head, capture_profile2_vtable; /* read-only failure snapshot */
  unsigned char capture_check_signature[16], capture_caps_signature[16];
  pbs_ps4_text_site capture_latch_sites[PBS_PS4_LATCH_SITE_COUNT]; /* experimental Open-only patch */
} pbs_ps4_firmware;

static const pbs_ps4_firmware pbs_ps4_fw_1100 = {
  .name = "PS4 11.00 / Avcap ABI v1",
  .version = 0x11000000u, .version_mask = 0xffff0000u,
  .acmgr_offset = 0x3d0de0,
  .acmgr_signature = {0xb0, 1, 0xc3},
  .gates = {
    {0x384285, {0x77,0x1c}, {0xeb,0x1c}, 2},
    {0x384771, {0x48,0x8b,0x41,0x08,0x44}, {0xe9,0x7c,0x02,0,0}, 5}
  },
  .avcap_init_offset = 0x940, .avcap_trap_offset = 0xbe,
  .ready_delta = 0x18e0 - 0x940,
  .dequeue_delta = 0x1c10 - 0x940, .consume_delta = 0x1db0 - 0x940,
  .ring_state_offset = 8,
  .rp_nn_socket = 0x1fdf60, .rp_vtable = 0x529838,
  .rp_data = 0x538000, .rp_data_size = 0x19000, .rp_ensure_open = 0x2fd250,
  .capture_flags = 0x240a0, .capture_slots = 0x26718,
  .capture_object_head = 0x26900, .capture_profile2_vtable = 0x20ab8,
  .capture_check = 0x7fc0, .capture_caps = 0x9d50,
  .capture_check_signature = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,
                              0x8b,0x07,0x41,0x89,0xf6,0x48,0x89,0xfb},
  .capture_caps_signature = {0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,
                             0xfb,0xe8,0x12,0xdf,0xff,0xff,0x8b,0x8b},
  .capture_latch_sites = {
    /* One immediate byte changes BF -> BB; no opcode/length/control-flow change.
     * Generic profiles can also reach this AND: install only for an idle
     * service owned by our Init, and restore immediately after Open. */
    {0x9d9c, 5, {0x25,0xbf,0xfd,0xff,0xff}, {0x25,0xbb,0xfd,0xff,0xff}}
  }
};

static inline const pbs_ps4_firmware *pbs_ps4_firmware_find(uint32_t version) {
  const pbs_ps4_firmware *p = &pbs_ps4_fw_1100;
  return (version & p->version_mask) == p->version ? p : NULL;
}

static inline intptr_t pbs_ps4_firmware_trap(const pbs_ps4_firmware *p,
                                            intptr_t init) {
  if (!p || init <= (intptr_t)p->avcap_init_offset ||
      init - (intptr_t)p->avcap_init_offset > INTPTR_MAX - p->avcap_trap_offset)
    return 0;
  return init - (intptr_t)p->avcap_init_offset + p->avcap_trap_offset;
}
