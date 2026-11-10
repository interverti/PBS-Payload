// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/request_template.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void store_u64(unsigned char *bytes, uint64_t value) {
  memcpy(bytes, &value, sizeof value);
}

static void store_u32(unsigned char *bytes, uint32_t value) {
  memcpy(bytes, &value, sizeof value);
}

int main(void) {
  static const uint32_t firmware[] = {
    0x03200004u, 0x04000042u, 0x04510001u, 0x05000040u,
    0x05100023u, 0x05500008u, 0x06020004u, 0x07200005u,
    0x08400005u, 0x09200005u, 0x09400008u, 0x09600004u,
    0x10010000u, 0x10600006u, 0x11200005u, 0x11400005u,
    0x11600005u, 0x12000043u, 0x12400009u, 0x12700001u,
    0x13000040u, 0x13200006u, 0x13420000u, 0x13600007u,
  };
  for (size_t i = 0; i < sizeof firmware / sizeof *firmware; ++i) {
    const pbs_ps5_offset_profile *profile =
        pbs_ps5_find_offsets(firmware[i]);
    assert(profile);
    const pbs_ps5_remoteplay_offsets *rp = &profile->remoteplay;
    unsigned char request[0x400] = {0};
    store_u64(request, profile->avcap2.request_size);
    store_u64(request + 8, 0x28);
    store_u64(request + rp->rate.offset, rp->rate.size);
    store_u64(request + rp->rate.offset + 8, rp->rate.descriptor_size);
    store_u64(request + rp->image.offset, rp->image.size);
    store_u64(request + rp->image.offset + 8, rp->image.descriptor_size);
    store_u64(request + rp->paste_offset, 0x88);
    store_u32(request + rp->mask_mode_offset, 2);
    store_u32(request + rp->width_offset, 1920);
    store_u32(request + rp->height_offset, 1080);
    store_u32(request + 0x28, 12);
    store_u32(request + 0x14, 2);
    store_u32(request + 0x18, 4);
    store_u32(request + 0x1c, 1);
    assert(pbs_ps5_native_request_matches(
        request, profile->avcap2.request_size, profile));
    assert(pbs_ps5_native_request_matches_expected(
        request, profile->avcap2.request_size, profile,
        1920, 1080, 12, 2, 4, 1));
    assert(!pbs_ps5_native_request_matches_expected(
        request, profile->avcap2.request_size, profile,
        2560, 1440, 11, 2, 5, 1));
    request[rp->paste_offset] = 0;
    assert(!pbs_ps5_native_request_matches(
        request, profile->avcap2.request_size, profile));
  }
  const pbs_ps5_offset_profile *profile = pbs_ps5_find_offsets(0x05500008u);
  unsigned char built[0x400] = {0}, native[0x400] = {0};
  assert(profile && !pbs_ps5_apply_request_initializers(
      built, profile->avcap2.request_size, &profile->remoteplay));
  memcpy(native, built, profile->avcap2.request_size);
  native[0x30] ^= 1; /* constructor-owned */
  native[0x14] ^= 1; /* runtime-owned */
  native[0x40] ^= 1; /* neither catalogue */
  FILE *report = tmpfile();
  assert(report);
  pbs_ps5_dump_native_request_diff(report, built, native,
                                    profile->avcap2.request_size, profile);
  assert(!fflush(report));
  rewind(report);
  char text[4096] = {0};
  assert(fread(text, 1, sizeof text - 1, report) > 0);
  fclose(report);
  assert(strstr(text, "bytes=3 initializer=1 runtime=1 residual=1"));
  assert(strstr(text, "+0x030..0x030 initializer"));
  assert(strstr(text, "+0x014..0x014 runtime"));
  assert(strstr(text, "+0x040..0x040 residual"));
  puts("PS5 live native request signatures verified for all 24 exact profiles");
  return 0;
}
