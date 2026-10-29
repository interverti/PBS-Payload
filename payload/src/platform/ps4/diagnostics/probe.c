#include "platform/ps4/system/firmware.h"
// SPDX-License-Identifier: GPL-3.0-or-later
/* Read-only PS4 capture discovery. Dumps remain private in /data/pbs/ps4. */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>
#include <ps4/kernel.h>

static char dump_dir[128] = "/data/pbs/ps4";

static int dump_self(FILE *log, const char *path) {
  unsigned char head[32];
  int fd = open(path, O_RDONLY), result = -1;
  FILE *out = NULL;
  Elf64_Phdr *ph = NULL;
  if (fd < 0) goto done;
  if (pread(fd, head, sizeof head, 0) != sizeof head) goto done;
  uint32_t magic; uint16_t entries;
  memcpy(&magic, head, 4); memcpy(&entries, head + 24, 2);
  if (magic != 0x1d3d154f || entries > 128) { errno = EINVAL; goto done; }
  off_t elf_offset = 32 + entries * 32;
  Elf64_Ehdr eh;
  if (pread(fd, &eh, sizeof eh, elf_offset) != sizeof eh ||
      memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_phnum > 128 ||
      eh.e_phentsize != sizeof *ph || eh.e_phoff > 0x100000) goto done;
  size_t phsize = eh.e_phnum * sizeof *ph;
  ph = malloc(phsize);
  if (!ph || pread(fd, ph, phsize, elf_offset + eh.e_phoff) != (ssize_t)phsize) goto done;
  char dest[256];
  const char *title = strstr(path, "/NPXS");
  if (title) snprintf(dest, sizeof dest, "%s/%.9s.elf", dump_dir, title + 1);
  else snprintf(dest, sizeof dest, "%s/%s.elf", dump_dir, strrchr(path, '/') + 1);
  out = fopen(dest, "wb");
  if (!out) goto done;
  /* The reconstructed file has program headers, no section table. */
  eh.e_shoff = 0; eh.e_shnum = 0; eh.e_shstrndx = 0;
  if (fwrite(&eh, 1, sizeof eh, out) != sizeof eh ||
      fseeko(out, eh.e_phoff, SEEK_SET) || fwrite(ph, 1, phsize, out) != phsize) goto done;
  for (unsigned i = 0; i < eh.e_phnum; ++i) {
    if (!ph[i].p_filesz || ph[i].p_type == PT_PHDR) continue;
    if (ph[i].p_filesz > 64 * 1024 * 1024 || ph[i].p_offset > 64 * 1024 * 1024) goto done;
    uint64_t entry[4];
    int found = 0;
    for (unsigned j = 0; j < entries; ++j) {
      if (pread(fd, entry, sizeof entry, 32 + j * 32) != sizeof entry) goto done;
      if ((entry[0] & 0x800) && ((entry[0] >> 20) & 0xffff) == i) { found = 1; break; }
    }
    if (!found) continue; /* DYNAMIC/EH_FRAME/RELRO alias other segments. */
    fprintf(log, "  segment %u type=%x size=%llu props=%llx\n", i, ph[i].p_type,
      (unsigned long long)ph[i].p_filesz, (unsigned long long)entry[0]);
    if (!(entry[0] & 0xa)) {
      void *raw = malloc(ph[i].p_filesz);
      if (!raw) goto done;
      int failed = pread(fd, raw, ph[i].p_filesz, entry[1]) != (ssize_t)ph[i].p_filesz ||
        fseeko(out, ph[i].p_offset, SEEK_SET) || fwrite(raw, 1, ph[i].p_filesz, out) != ph[i].p_filesz;
      free(raw);
      if (failed) goto done;
      continue;
    }
    void *mem = mmap(NULL, ph[i].p_filesz, PROT_READ, MAP_PRIVATE | 0x80000,
                     fd, (off_t)((uint64_t)i << 32));
    if (mem == MAP_FAILED) {
      fprintf(log, "  segment %u type=%x mmap errno=%d\n", i, ph[i].p_type, errno);
      goto done;
    }
    int failed = fseeko(out, ph[i].p_offset, SEEK_SET) ||
      fwrite(mem, 1, ph[i].p_filesz, out) != ph[i].p_filesz;
    munmap(mem, ph[i].p_filesz);
    if (failed) goto done;
  }
  result = 0;
done:
  fprintf(log, "dump %s: %s errno=%d\n", path, result ? "FAILED" : "OK", errno);
  if (out && fclose(out)) result = -1;
  free(ph);
  if (fd >= 0) close(fd);
  return result;
}

int main(void) {
  mkdir("/data/pbs", 0777); mkdir("/data/pbs/ps4", 0777);
  unsigned firmware = kernel_get_fw_version();
#ifdef PBS_PS4_ABI_PROBE
  snprintf(dump_dir,sizeof dump_dir,"/data/pbs/ps4/abi-%08x",firmware);
  mkdir(dump_dir,0777);
#endif
  char log_path[160];
  snprintf(log_path,sizeof log_path,"%s/probe.txt",dump_dir);
  FILE *log = fopen(log_path, "w");
  if (!log) return 1;
  setvbuf(log, NULL, _IONBF, 0);
  fprintf(log, "PBS PS4 discovery fw=0x%08x pid=%d\n", firmware, getpid());
#ifdef PBS_PS4_ABI_PROBE
  fprintf(log,"ABI collection: files and process pathnames only; no kernel gate reads/writes, ptrace, remote calls or capture\n");
#else
  const pbs_ps4_firmware *fw = pbs_ps4_firmware_find(kernel_get_fw_version());
  if (fw) {
    const unsigned offsets[] = {fw->acmgr_offset, fw->gates[0].offset, fw->gates[1].offset};
    for (unsigned i = 0; i < 3; ++i) {
      unsigned char code[16];
      int rc = kernel_copyout(KERNEL_ADDRESS_IMAGE_BASE + offsets[i], code, sizeof code);
      fprintf(log, "debug gate +%x rc=%d bytes=", offsets[i], rc);
      if (!rc) for (unsigned j = 0; j < sizeof code; ++j) fprintf(log, "%02x", code[j]);
      fprintf(log, "\n");
    }
  }
#endif
  int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (!sysctl(mib, 4, NULL, &size, NULL, 0)) {
    unsigned char *buf = malloc(size);
    if (buf && !sysctl(mib, 4, buf, &size, NULL, 0)) {
      for (size_t pos = 0; size - pos >= sizeof(struct kinfo_proc);) {
        struct kinfo_proc *ki = (void *)(buf + pos);
        if (ki->ki_structsize <= 0 || (size_t)ki->ki_structsize < sizeof *ki || (size_t)ki->ki_structsize > size - pos) break;
        fprintf(log, "pid=%d name=%s\n", ki->ki_pid, ki->ki_comm);
        if (!strcmp(ki->ki_comm, "SceRemotePlay") || !strcmp(ki->ki_comm, "SceAvCapture")) {
          char path[1024] = {0};
          size_t path_size = sizeof path;
          int pmib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, ki->ki_pid};
          int rc = sysctl(pmib, 4, path, &path_size, NULL, 0);
          fprintf(log, "  pathname rc=%d %s\n", rc, path);
          if (!rc && path[0] == '/') dump_self(log, path);
        }
        pos += ki->ki_structsize;
      }
    }
    free(buf);
  }
  const char *paths[] = {
    "/system/common/lib/libSceAvcap.sprx",
    "/system/common/lib/libkernel_sys.sprx",
    "/system/common/lib/libSceLibcInternal.sprx",
    "/system/common/lib/libSceRemoteplay.sprx",
    "/system/priv/lib/libSceVencCore.sprx",
    "/system/priv/lib/libSceVencCoreForNeo.sprx",
    "/system/sys/SceVencProxy.elf",
    "/system/vsh/SceShellCore.elf"
  };
  int failed = 0;
  for (unsigned i = 0; i < sizeof paths / sizeof *paths; ++i) failed |= dump_self(log, paths[i]);
  fprintf(log, "complete result=%d\n", failed);
  fclose(log);
  return failed ? 1 : 0;
}
