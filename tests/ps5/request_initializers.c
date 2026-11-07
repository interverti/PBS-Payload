// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t load_u64(const unsigned char *bytes) {
  uint64_t value = 0;
  memcpy(&value, bytes, sizeof value);
  return value;
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
    assert(profile && profile->firmware == firmware[i]);
    unsigned char request[0x400] = {0};
    assert(!pbs_ps5_apply_request_initializers(
        request, profile->avcap2.request_size, &profile->remoteplay));
    uint64_t request_size = load_u64(request);
    uint64_t common_size = load_u64(request + 8);
    if (request_size != profile->avcap2.request_size || common_size != 0x28) {
      fprintf(stderr, "FW%08x initializer header=%llx/%llx\n",
              firmware[i], (unsigned long long)request_size,
              (unsigned long long)common_size);
      return 1;
    }
    uint64_t rate_size = load_u64(request + profile->remoteplay.rate.offset);
    uint64_t rate_desc = load_u64(request + profile->remoteplay.rate.offset + 8);
    uint64_t image_size = load_u64(request + profile->remoteplay.image.offset);
    uint64_t image_desc = load_u64(request + profile->remoteplay.image.offset + 8);
    if (rate_size != profile->remoteplay.rate.size ||
        rate_desc != profile->remoteplay.rate.descriptor_size ||
        image_size != profile->remoteplay.image.size ||
        image_desc != profile->remoteplay.image.descriptor_size) {
      fprintf(stderr,
              "FW%08x initializer mismatch rate=%llx/%llx image=%llx/%llx\n",
              firmware[i], (unsigned long long)rate_size,
              (unsigned long long)rate_desc, (unsigned long long)image_size,
              (unsigned long long)image_desc);
      return 1;
    }
  }
  puts("PS5 native request initializers verified for all 24 exact firmware profiles");
  return 0;
}
