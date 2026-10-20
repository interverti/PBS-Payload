// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/capture/export_reader.h"
#include "platform/ps4/worker/remote.h"
#include <string.h>

#define FRAME_CAPACITY 0x200000u
#define META_OFFSET 0x300u
#define META_SIZE 0x40u

static long reader_call(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  long rc = pbs_inject_call_fast(ctx, fn, args);
  /* A timed-out RPC may still use the buffer. Do not free or issue another
   * call, and propagate quarantine to the shared control owner at teardown. */
  if (rc == PBS_CALL_TIMEOUT) ctx->asserted = 1;
  return rc;
}

int pbs_ps4_export_reader_init(pbs_ps4_export_reader *r, pbs_inject_t *ctx) {
  if (!r || !ctx || !ctx->worker || ctx->asserted || r->buffer ||
      ctx->scratch <= 0 || ctx->scratch_len < META_OFFSET + META_SIZE) return -1;
  r->read = pbs_resolve_in(ctx->pid, "libSceAvcap.sprx", "sceVshAvcapReadVideo");
  r->allocate = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "malloc");
  r->release = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "free");
  if (!r->read || !r->allocate || !r->release) return -1;
  long buffer = reader_call(ctx, r->allocate,
                            (uint64_t[6]){FRAME_CAPACITY,0,0,0,0,0});
  if (buffer <= 0 || ctx->asserted || buffer > INTPTR_MAX - FRAME_CAPACITY)
    return -1;
  r->buffer = (intptr_t)buffer;
  return 0;
}

int pbs_ps4_export_reader_read(pbs_ps4_export_reader *r, pbs_inject_t *ctx,
                             void *handle, void *out, size_t size, void *info,
                             int flags) {
  if (!r || !ctx || !ctx->worker || ctx->asserted || !r->buffer ||
      !handle || !out || !size) return -1;
  unsigned char metadata[META_SIZE] = {0};
  if (pbs_inject_write_at(ctx, ctx->scratch + META_OFFSET,
                          metadata, sizeof metadata)) {
    ctx->asserted = 1;
    return -1;
  }
  size_t capacity = size < FRAME_CAPACITY ? size : FRAME_CAPACITY;
  long rc = reader_call(ctx, r->read, (uint64_t[6]){
      (uint64_t)handle, (uint64_t)r->buffer, capacity,
      (uint64_t)(ctx->scratch + META_OFFSET), (uint32_t)flags | 1u, 0});
  if (rc == PBS_CALL_TIMEOUT || ctx->asserted) return -1;
  int32_t length = (int32_t)(uint32_t)rc;
  if ((uint32_t)length == 0x80a70002u) return 0; /* No packet. */
  if (length <= 0) return length;
  if ((size_t)length > capacity) return -1;
  /* The worker transport caps each COPY, independently of Avcap's frame
   * capacity. Keep large keyframes supported without private ring access. */
  for (size_t offset = 0; offset < (size_t)length;) {
    size_t chunk = (size_t)length - offset;
    if (chunk > PBS_WORKER_COPY_MAX) chunk = PBS_WORKER_COPY_MAX;
    if (pbs_inject_read_at(ctx, r->buffer + (intptr_t)offset,
                           (unsigned char *)out + offset, chunk)) {
      ctx->asserted = 1;
      return -1;
    }
    offset += chunk;
  }
  if (info) {
    if (pbs_inject_read_at(ctx, ctx->scratch + META_OFFSET,
                           metadata, sizeof metadata)) {
      ctx->asserted = 1;
      return -1;
    }
    memcpy(info, metadata, sizeof metadata);
  }
  return length;
}

int pbs_ps4_export_reader_release(pbs_ps4_export_reader *r, pbs_inject_t *ctx) {
  if (!r || !ctx) return -1;
  if (ctx->asserted) return -1;
  if (r->buffer) {
    if (!ctx->worker || !r->release) return -1;
    long rc = reader_call(ctx, r->release,
                          (uint64_t[6]){(uint64_t)r->buffer,0,0,0,0,0});
    if (rc == PBS_CALL_TIMEOUT || ctx->asserted) return -1;
  }
  memset(r, 0, sizeof *r);
  return 0;
}
