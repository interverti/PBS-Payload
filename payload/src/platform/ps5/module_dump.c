// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/module_dump.h"
#include <ps5/kernel.h>
#include <ps5/mdbg.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#define PBS_MODULE_MAX_BYTES (64u * 1024u * 1024u)
#define PBS_VMMAP_MAX_BYTES (4u * 1024u * 1024u)
#define PBS_REGION_MAX_BYTES (32u * 1024u * 1024u)
#define PBS_MEMORY_TOTAL_MAX_BYTES (64u * 1024u * 1024u)

static const char *const wanted_modules[] = {
  "libSceAvcap2.sprx", "libSceResourceArbitrator.sprx",
  "libSceLibcInternal.sprx", "libkernel_sys.sprx",
  "libSceUserService.sprx", "libSceVencCore.sprx",
  "libSceVencCoreForNeo.sprx", "libSceVideoOut.sprx",
  "libSceVenc.sprx", "libSceAjmi.sprx", "eboot.bin",
  "SceRemotePlay", "SceRemotePlay.elf", "remoteplay.elf"
};

static int wanted_index(const char *path) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  for (size_t i = 0; i < sizeof wanted_modules / sizeof wanted_modules[0]; ++i)
    if (!strcmp(base, wanted_modules[i])) return (int)i;
  return -1;
}

static int read_process(pid_t pid, intptr_t address, void *buffer, size_t length) {
  if (!mdbg_copyout(pid, address, buffer, length)) return 0;
  return kernel_proc_copyout(pid, address, buffer, length);
}

static int copy_module(FILE *report, const char *source, const char *destination,
                       const char *name, const char *origin) {
  FILE *input = fopen(source, "rb");
  if (!input) {
    fprintf(report, "module file dump: %s source=%s open errno=%d\n", name, source, errno);
    return -1;
  }
  struct stat st;
  if (fstat(fileno(input), &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      (uint64_t)st.st_size > PBS_MODULE_MAX_BYTES) {
    fprintf(report, "module file dump: %s source=%s invalid or oversized file\n", name, source);
    fclose(input);
    return -1;
  }
  char partial[PATH_MAX];
  int n = snprintf(partial, sizeof partial, "%s.part", destination);
  if (n < 0 || (size_t)n >= sizeof partial) {
    fclose(input);
    return -1;
  }
  FILE *output = fopen(partial, "wb");
  if (!output) {
    fprintf(report, "module file dump: %s destination open errno=%d\n", name, errno);
    fclose(input);
    return -1;
  }
  unsigned char buffer[16384];
  unsigned char magic[4] = {0};
  uint64_t hash = UINT64_C(14695981039346656037);
  uint64_t total = 0;
  int failed = 0;
  while (!feof(input)) {
    size_t read_count = fread(buffer, 1, sizeof buffer, input);
    if (!read_count) {
      if (ferror(input)) failed = 1;
      break;
    }
    if (!total && read_count >= sizeof magic) memcpy(magic, buffer, sizeof magic);
    if (total + read_count > PBS_MODULE_MAX_BYTES ||
        fwrite(buffer, 1, read_count, output) != read_count) {
      failed = 1;
      break;
    }
    for (size_t i = 0; i < read_count; ++i) {
      hash ^= buffer[i];
      hash *= UINT64_C(1099511628211);
    }
    total += read_count;
  }
  if (fclose(input)) failed = 1;
  if (fclose(output)) failed = 1;
  if (failed || total != (uint64_t)st.st_size || rename(partial, destination)) {
    fprintf(report, "module file dump: %s copy incomplete bytes=%" PRIu64 " errno=%d\n",
            name, total, errno);
    remove(partial);
    return -1;
  }
  fprintf(report, "module file dump: %s origin=%s source=%s saved=%s bytes=%" PRIu64
                  " magic=%02x%02x%02x%02x fnv64=0x%016" PRIx64 "\n",
          name, origin, source, destination, total,
          magic[0], magic[1], magic[2], magic[3], hash);
  return 0;
}

static void dump_code_region(FILE *report, pid_t pid, const char *directory,
                             const char *name, unsigned region_index,
                             uint64_t start, uint64_t end, uint64_t file_offset,
                             int protection, uint64_t *budget) {
  if (!(protection & 4) || end <= start || end - start > PBS_REGION_MAX_BYTES ||
      end - start > *budget || start > INTPTR_MAX || end > INTPTR_MAX) return;
  char destination[PATH_MAX], partial[PATH_MAX];
  int n = snprintf(destination, sizeof destination, "%s/%s.map%02u.bin",
                   directory, name, region_index);
  if (n <= 0 || (size_t)n >= sizeof destination) return;
  n = snprintf(partial, sizeof partial, "%s.part", destination);
  if (n <= 0 || (size_t)n >= sizeof partial) return;
  FILE *output = fopen(partial, "wb");
  if (!output) {
    fprintf(report, "module memory dump: %s region=%u output errno=%d\n",
            name, region_index, errno);
    return;
  }
  unsigned char buffer[16384];
  uint64_t hash = UINT64_C(14695981039346656037);
  uint64_t position = start;
  int failed = 0;
  while (position < end) {
    size_t length = end - position < sizeof buffer ? (size_t)(end - position) : sizeof buffer;
    if (read_process(pid, (intptr_t)position, buffer, length) ||
        fwrite(buffer, 1, length, output) != length) {
      failed = 1;
      break;
    }
    for (size_t j = 0; j < length; ++j) {
      hash ^= buffer[j];
      hash *= UINT64_C(1099511628211);
    }
    position += length;
  }
  if (fclose(output)) failed = 1;
  if (failed || rename(partial, destination)) {
    fprintf(report, "module memory dump: %s region=%u incomplete at=0x%" PRIx64
                    " errno=%d\n", name, region_index, position, errno);
    remove(partial);
    return;
  }
  *budget -= end - start;
  fprintf(report, "module memory dump: %s region=%u start=0x%" PRIx64
                  " end=0x%" PRIx64 " file_offset=0x%" PRIx64
                  " protection=0x%x saved=%s bytes=%" PRIu64
                  " fnv64=0x%016" PRIx64 "\n",
          name, region_index, start, end, file_offset, protection, destination,
          end - start, hash);
}

static size_t mapped_module_span(const void *map, size_t map_size,
                                 intptr_t base, const char *name, size_t limit) {
  if (!map) return 0;
  const unsigned char *cursor = map;
  const unsigned char *end = cursor + map_size;
  uint64_t last = (uint64_t)base;
  while ((size_t)(end - cursor) >= sizeof(int)) {
    int record_size = 0;
    memcpy(&record_size, cursor, sizeof record_size);
    if (record_size < (int)(offsetof(struct kinfo_vmentry, kve_path) + 1) ||
        (size_t)record_size > (size_t)(end - cursor)) break;
    size_t path_size = (size_t)record_size - offsetof(struct kinfo_vmentry, kve_path);
    if (path_size > PATH_MAX) path_size = PATH_MAX;
    const char *path = (const char *)(cursor + offsetof(struct kinfo_vmentry, kve_path));
    uint64_t start = 0, finish = 0;
    memcpy(&start, cursor + offsetof(struct kinfo_vmentry, kve_start), sizeof start);
    memcpy(&finish, cursor + offsetof(struct kinfo_vmentry, kve_end), sizeof finish);
    if (memchr(path, 0, path_size) && path[0] == '/' &&
        wanted_index(path) >= 0 &&
        !strcmp(strrchr(path, '/') + 1, name) &&
        start >= (uint64_t)base && start < (uint64_t)base + limit &&
        finish > start && finish <= (uint64_t)base + limit && finish > last)
      last = finish;
    cursor += record_size;
  }
  return (size_t)(last - (uint64_t)base);
}

/* Same read source as the original PBS dumper: bytes from the loaded process.
 * The SDK supplies the base; VM map entries bound the image without relying on
 * the 9.40-only proc/dynlib structure offsets from the original dumper. */
static void dump_loaded_image(FILE *report, pid_t pid, const char *directory,
                              const char *name, const void *map, size_t map_size,
                              size_t limit) {
  uint32_t handle = 0;
  if (kernel_dynlib_handle(pid, name, &handle)) {
    fprintf(report, "module memory image: %s handle unavailable\n", name);
    return;
  }
  intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
  if (base <= 0) {
    fprintf(report, "module memory image: %s base unavailable\n", name);
    return;
  }
  size_t span = mapped_module_span(map, map_size, base, name, limit);
  int bounded_by_map = span != 0;
  if (!span) span = limit;
  char destination[PATH_MAX], partial[PATH_MAX];
  int n = snprintf(destination, sizeof destination, "%s/%s.%s.bin", directory,
                   name, bounded_by_map ? "memory" : "prefix");
  if (n <= 0 || (size_t)n >= sizeof destination) return;
  n = snprintf(partial, sizeof partial, "%s.part", destination);
  if (n <= 0 || (size_t)n >= sizeof partial) return;
  FILE *output = fopen(partial, "wb");
  if (!output) {
    fprintf(report, "module memory image: %s output errno=%d\n", name, errno);
    return;
  }
  unsigned char page[4096];
  size_t done = 0;
  size_t missing = 0;
  int failed = 0;
  while (done < span) {
    if (read_process(pid, base + (intptr_t)done, page, sizeof page)) {
      if (!bounded_by_map) break;
      memset(page, 0, sizeof page);
      ++missing;
    }
    if (fwrite(page, 1, sizeof page, output) != sizeof page) { failed = 1; break; }
    done += sizeof page;
  }
  if (fclose(output)) failed = 1;
  if (failed || !done || missing == done / sizeof page || rename(partial, destination)) {
    fprintf(report, "module memory image: %s failed bytes=%zu missing_pages=%zu errno=%d\n",
            name, done, missing, errno);
    remove(partial);
    return;
  }
  fprintf(report, "module memory image: %s base=0x%" PRIxPTR
                  " handle=%u saved=%s bytes=%zu span_source=%s missing_pages=%zu\n",
          name, (uintptr_t)base, handle, destination, done,
          bounded_by_map ? "vmmap" : "readable-prefix", missing);
}

void pbs_ps5_dump_module_files(FILE *report, pid_t pid, const char *output_root) {
  if (!report || pid <= 0 || !output_root) return;
  if (mkdir(output_root, 0777) && errno != EEXIST) {
    fprintf(report, "module file dump: cannot create %s errno=%d\n", output_root, errno);
    return;
  }
  char directory[PATH_MAX];
  int created = 0;
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    int n = snprintf(directory, sizeof directory, "%s/ps5-%08x-%ld-pid%d-%u",
                     output_root, kernel_get_fw_version(), (long)time(NULL),
                     (int)pid, attempt);
    if (n < 0 || (size_t)n >= sizeof directory) return;
    if (!mkdir(directory, 0777)) { created = 1; break; }
    if (errno != EEXIST) break;
  }
  if (!created) {
    fprintf(report, "module file dump: cannot create run directory errno=%d\n", errno);
    return;
  }
  fprintf(report, "module file dump: target_pid=%d directory=%s\n", (int)pid, directory);

  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, (int)pid};
  size_t size = 0;
  void *map = NULL;
  if (!sysctl(mib, 4, NULL, &size, NULL, 0) && size && size <= PBS_VMMAP_MAX_BYTES) {
    size_t capacity = size + 16384;
    map = malloc(capacity);
    if (map && sysctl(mib, 4, map, &capacity, NULL, 0)) {
      free(map);
      map = NULL;
    } else if (map) size = capacity;
  }
  if (!map) fprintf(report, "module file dump: VM map unavailable; trying standard module directories\n");

  unsigned char copied[sizeof wanted_modules / sizeof wanted_modules[0]] = {0};
  unsigned region_counts[sizeof wanted_modules / sizeof wanted_modules[0]] = {0};
  uint64_t memory_budget = PBS_MEMORY_TOTAL_MAX_BYTES;
  if (map) {
    const unsigned char *cursor = map;
    const unsigned char *end = cursor + size;
    size_t records = 0, matches = 0;
    while ((size_t)(end - cursor) >= sizeof(int)) {
      int record_size = 0;
      memcpy(&record_size, cursor, sizeof record_size);
      if (record_size < (int)(offsetof(struct kinfo_vmentry, kve_path) + 1) ||
          (size_t)record_size > (size_t)(end - cursor)) break;
      ++records;
      size_t path_size = (size_t)record_size - offsetof(struct kinfo_vmentry, kve_path);
      if (path_size > PATH_MAX) path_size = PATH_MAX;
      const char *path = (const char *)(cursor + offsetof(struct kinfo_vmentry, kve_path));
      const char *nul = memchr(path, 0, path_size);
      if (nul && path[0] == '/') {
        int index = wanted_index(path);
        if (index >= 0) {
          ++matches;
          if (!copied[index]) {
            char destination[PATH_MAX];
            int n = snprintf(destination, sizeof destination, "%s/%s", directory,
                             wanted_modules[index]);
            if (n > 0 && (size_t)n < sizeof destination &&
                !copy_module(report, path, destination, wanted_modules[index], "vmmap"))
              copied[index] = 1;
          }
          if (region_counts[index] < 16) {
            uint64_t start, finish, file_offset;
            int protection;
            memcpy(&start, cursor + offsetof(struct kinfo_vmentry, kve_start), sizeof start);
            memcpy(&finish, cursor + offsetof(struct kinfo_vmentry, kve_end), sizeof finish);
            memcpy(&file_offset, cursor + offsetof(struct kinfo_vmentry, kve_offset), sizeof file_offset);
            memcpy(&protection, cursor + offsetof(struct kinfo_vmentry, kve_protection), sizeof protection);
            dump_code_region(report, pid, directory, wanted_modules[index],
                             region_counts[index]++, start, finish, file_offset,
                             protection, &memory_budget);
          }
        }
      }
      cursor += record_size;
    }
    fprintf(report, "module file dump: VM map records=%zu matching_regions=%zu parsed_bytes=%zu/%zu\n",
            records, matches, (size_t)(cursor - (const unsigned char *)map), size);
  }
  dump_loaded_image(report, pid, directory, "libSceAvcap2.sprx", map, size, 0x400000);
  dump_loaded_image(report, pid, directory, "libSceResourceArbitrator.sprx", map, size, 0x100000);
  dump_loaded_image(report, pid, directory, "libSceVenc.sprx", map, size, 0x400000);
  dump_loaded_image(report, pid, directory, "libSceVencCore.sprx", map, size, 0x400000);
  dump_loaded_image(report, pid, directory, "libSceVencCoreForNeo.sprx", map, size, 0x400000);
  dump_loaded_image(report, pid, directory, "eboot.bin", map, size, 0x800000);
  if (map) free(map);
  const char *const roots[] = {
    "/system/common/lib", "/system/priv/lib", "/system/external/lib", "/system/vsh/lib"
  };
  for (size_t i = 0; i < sizeof wanted_modules / sizeof wanted_modules[0]; ++i) {
    if (copied[i]) continue;
    for (size_t r = 0; r < sizeof roots / sizeof roots[0]; ++r) {
      char source[PATH_MAX], destination[PATH_MAX];
      int a = snprintf(source, sizeof source, "%s/%s", roots[r], wanted_modules[i]);
      int b = snprintf(destination, sizeof destination, "%s/%s", directory,
                       wanted_modules[i]);
      if (a <= 0 || (size_t)a >= sizeof source || b <= 0 ||
          (size_t)b >= sizeof destination) continue;
      if (access(source, R_OK)) continue;
      if (!copy_module(report, source, destination, wanted_modules[i], "fallback")) {
        copied[i] = 1;
        break;
      }
    }
    if (!copied[i]) fprintf(report, "module file dump: %s unavailable\n", wanted_modules[i]);
  }
}
