// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/request_template.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t request_u64(const unsigned char *bytes) {
  uint64_t value = 0;
  memcpy(&value, bytes, sizeof value);
  return value;
}

static uint32_t request_u32(const unsigned char *bytes) {
  uint32_t value = 0;
  memcpy(&value, bytes, sizeof value);
  return value;
}

int pbs_ps5_native_request_matches(
    const unsigned char *request, size_t length,
    const pbs_ps5_offset_profile *profile) {
  if (!request || !profile || length < profile->avcap2.request_size)
    return 0;
  const pbs_ps5_avcap2_offsets *avcap = &profile->avcap2;
  const pbs_ps5_remoteplay_offsets *rp = &profile->remoteplay;
  if (avcap->request_size < 0x80 || avcap->request_size > 0x400 ||
      rp->rate.offset + 16 > avcap->request_size ||
      rp->image.offset + 16 > avcap->request_size ||
      rp->paste_offset + 8 > avcap->request_size ||
      rp->mask_mode_offset + 4 > avcap->request_size)
    return 0;
  return request_u64(request) == avcap->request_size &&
         request_u64(request + 8) == 0x28 &&
         request_u64(request + rp->rate.offset) == rp->rate.size &&
         request_u64(request + rp->rate.offset + 8) ==
             rp->rate.descriptor_size &&
         request_u64(request + rp->image.offset) == rp->image.size &&
         request_u64(request + rp->image.offset + 8) ==
             rp->image.descriptor_size &&
         request_u64(request + rp->paste_offset) == 0x88 &&
         request_u32(request + rp->mask_mode_offset) == 2;
}

int pbs_ps5_native_request_matches_expected(
    const unsigned char *request, size_t length,
    const pbs_ps5_offset_profile *profile,
    uint32_t width, uint32_t height, uint32_t feature,
    uint32_t usage, uint32_t resolution, uint32_t fps_mode) {
  if (!pbs_ps5_native_request_matches(request, length, profile)) return 0;
  const pbs_ps5_remoteplay_offsets *rp = &profile->remoteplay;
  return request_u32(request + rp->width_offset) == width &&
         request_u32(request + rp->height_offset) == height &&
         request_u32(request + 0x28) == feature &&
         request_u32(request + 0x14) == usage &&
         request_u32(request + 0x18) == resolution &&
         request_u32(request + 0x1c) == fps_mode;
}

#ifndef PBS_TEST
#include <ps5/kernel.h>
#include <ps5/mdbg.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#define PBS_TEMPLATE_VMMAP_MAX (4u * 1024u * 1024u)
#define PBS_TEMPLATE_CHUNK (64u * 1024u)
#define PBS_TEMPLATE_SCAN_MAX (128u * 1024u * 1024u)

static int request_read(pid_t pid, intptr_t address, void *buffer, size_t size) {
  if (!mdbg_copyout(pid, address, buffer, size)) return 0;
  return kernel_proc_copyout(pid, address, buffer, size);
}

int pbs_ps5_find_native_request(
    pid_t pid, const pbs_ps5_offset_profile *profile,
    uint32_t width, uint32_t height, uint32_t feature,
    uint32_t usage, uint32_t resolution, uint32_t fps_mode,
    unsigned char *request, size_t capacity, FILE *report) {
  if (!profile || !request || capacity < profile->avcap2.request_size)
    return -1;
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, (int)pid};
  size_t map_size = 0;
  if (sysctl(mib, 4, NULL, &map_size, NULL, 0) || !map_size ||
      map_size > PBS_TEMPLATE_VMMAP_MAX) {
    if (report) fprintf(report, "Native request template: VM map unavailable\n");
    return -1;
  }
  size_t map_capacity = map_size + 16384;
  unsigned char *map = malloc(map_capacity);
  unsigned char *chunk = malloc(PBS_TEMPLATE_CHUNK);
  if (!map || !chunk || sysctl(mib, 4, map, &map_capacity, NULL, 0)) {
    free(chunk);
    free(map);
    if (report) fprintf(report, "Native request template: VM map read failed\n");
    return -1;
  }
  map_size = map_capacity;
  const size_t request_size = profile->avcap2.request_size;
  size_t scanned = 0;
  unsigned structural_candidates = 0;
  unsigned candidates = 0;
  int ambiguous = 0;
  uint32_t first_width = 0, first_height = 0, first_feature = 0;
  uint32_t first_usage = 0, first_resolution = 0, first_fps = 0;
  const unsigned char *cursor = map;
  const unsigned char *map_end = map + map_size;
  while ((size_t)(map_end - cursor) >= sizeof(int) &&
         scanned < PBS_TEMPLATE_SCAN_MAX) {
    int record_size = 0;
    memcpy(&record_size, cursor, sizeof record_size);
    if (record_size < (int)(offsetof(struct kinfo_vmentry, kve_path) + 1) ||
        (size_t)record_size > (size_t)(map_end - cursor)) break;
    uint64_t start = 0, finish = 0;
    int protection = 0;
    memcpy(&start, cursor + offsetof(struct kinfo_vmentry, kve_start), sizeof start);
    memcpy(&finish, cursor + offsetof(struct kinfo_vmentry, kve_end), sizeof finish);
    memcpy(&protection,
           cursor + offsetof(struct kinfo_vmentry, kve_protection),
           sizeof protection);
    cursor += record_size;
    if ((protection & 3) != 3 || (protection & 4) || finish <= start ||
        start > INTPTR_MAX || finish > INTPTR_MAX) continue;
    uint64_t position = start;
    while (position < finish && scanned < PBS_TEMPLATE_SCAN_MAX) {
      size_t length = finish - position < PBS_TEMPLATE_CHUNK
                          ? (size_t)(finish - position)
                          : PBS_TEMPLATE_CHUNK;
      if (length < request_size) break;
      if (request_read(pid, (intptr_t)position, chunk, length)) {
        position += length;
        scanned += length;
        continue;
      }
      for (size_t offset = 0; offset + request_size <= length; offset += 8) {
        if (!pbs_ps5_native_request_matches(chunk + offset, request_size,
                                            profile))
          continue;
        ++structural_candidates;
        if (structural_candidates == 1) {
          first_width = request_u32(chunk + offset + profile->remoteplay.width_offset);
          first_height = request_u32(chunk + offset + profile->remoteplay.height_offset);
          first_feature = request_u32(chunk + offset + 0x28);
          first_usage = request_u32(chunk + offset + 0x14);
          first_resolution = request_u32(chunk + offset + 0x18);
          first_fps = request_u32(chunk + offset + 0x1c);
        }
        if (!pbs_ps5_native_request_matches_expected(
                chunk + offset, request_size, profile,
                width, height, feature, usage, resolution, fps_mode))
          continue;
        ++candidates;
        if (candidates == 1)
          memcpy(request, chunk + offset, request_size);
        else if (memcmp(request, chunk + offset, request_size))
          ambiguous = 1;
      }
      scanned += length;
      if (length == PBS_TEMPLATE_CHUNK && length > request_size)
        position += length - request_size + 8;
      else
        position += length;
    }
  }
  free(chunk);
  free(map);
  if (report)
    fprintf(report,
            "Native request template: scanned=%zu structural=%u matched=%u ambiguous=%d first=%ux%u feature=%u usage=%u resolution=%u fps=%u requested=%ux%u/%u/%u/%u/%u\n",
            scanned, structural_candidates, candidates, ambiguous,
            first_width, first_height, first_feature, first_usage,
            first_resolution, first_fps, width, height, feature, usage,
            resolution, fps_mode);
  return candidates && !ambiguous ? 0 : -1;
}
#endif
