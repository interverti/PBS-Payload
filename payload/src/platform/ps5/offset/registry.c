// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

extern const pbs_ps5_offset_profile pbs_ps5_offsets_320;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_400;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_602;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_720;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_840;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_920;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_940;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1001;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1060;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1160;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1200;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1240;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1270;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1360;

const pbs_ps5_offset_profile *pbs_ps5_find_offsets(uint32_t firmware) {
  switch (firmware) {
    case 0x03200004u: return &pbs_ps5_offsets_320;
#ifdef PBS_PS5_EXPERIMENTAL_FW400
    case 0x04000042u: return &pbs_ps5_offsets_400;
#endif
    case 0x06020004u: return &pbs_ps5_offsets_602;
    case 0x07200005u: return &pbs_ps5_offsets_720;
    case 0x08400005u: return &pbs_ps5_offsets_840;
    case 0x09200005u: return &pbs_ps5_offsets_920;
    case 0x09400008u: return &pbs_ps5_offsets_940;
    case 0x10010000u: return &pbs_ps5_offsets_1001;
    case 0x10600006u: return &pbs_ps5_offsets_1060;
    case 0x11600005u: return &pbs_ps5_offsets_1160;
    case 0x12000043u: return &pbs_ps5_offsets_1200;
    case 0x12400009u: return &pbs_ps5_offsets_1240;
    case 0x12700001u: return &pbs_ps5_offsets_1270;
    case 0x13600007u: return &pbs_ps5_offsets_1360;
    default:
      /* 13.40 and 13.60 have identical executable Avcap2 segments. The
       * 13.40 SceRemotePlay constructor is not available for comparison. */
      if ((firmware & 0xffff0000u) == 0x13400000u)
        return &pbs_ps5_offsets_1360;
      return 0;
  }
}
