// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

extern const pbs_ps5_offset_profile pbs_ps5_offsets_920;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_940;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1001;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1160;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1240;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1360;

const pbs_ps5_offset_profile *pbs_ps5_find_offsets(uint32_t firmware) {
  switch (firmware) {
    case 0x09200005u: return &pbs_ps5_offsets_920;
    case 0x09400008u: return &pbs_ps5_offsets_940;
    case 0x10010000u: return &pbs_ps5_offsets_1001;
    case 0x11600005u: return &pbs_ps5_offsets_1160;
    case 0x12400009u: return &pbs_ps5_offsets_1240;
    case 0x13600007u: return &pbs_ps5_offsets_1360;
    default:
      /* 13.40 and 13.60 have identical executable Avcap2 segments. The
       * 13.40 SceRemotePlay constructor is not available for comparison. */
      if ((firmware & 0xffff0000u) == 0x13400000u)
        return &pbs_ps5_offsets_1360;
      return 0;
  }
}
