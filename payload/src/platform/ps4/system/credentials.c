// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/system/credentials.h"

#include <ps4/kernel.h>
#include <unistd.h>

static int elevated;
static int have_backup;
static uid_t saved_uid, saved_ruid, saved_svuid;
static gid_t saved_rgid, saved_svgid;
static intptr_t saved_prison, saved_rootdir, saved_jaildir;

int pbs_ps4_cred_elevate(FILE *log) {
  const pid_t pid = getpid();
  if (elevated) return 0;
  if (!have_backup) {
    saved_uid = getuid();
    saved_ruid = getuid();
    saved_svuid = getuid();
    saved_rgid = getgid();
    saved_svgid = getgid();
    saved_prison = kernel_get_ucred_prison(pid);
    saved_rootdir = kernel_get_proc_rootdir(pid);
    saved_jaildir = kernel_get_proc_jaildir(pid);
    have_backup = 1;
  }
  if (log)
    fprintf(log,
            "cred: elevate pid=%d uid=%u gid=%u prison=%p rootdir=%p jaildir=%p\n",
            (int)pid, (unsigned)saved_uid, (unsigned)saved_rgid,
            (void *)saved_prison, (void *)saved_rootdir, (void *)saved_jaildir);
  int failures = 0;
  failures += kernel_set_ucred_uid(pid, 0) != 0;
  failures += kernel_set_ucred_ruid(pid, 0) != 0;
  failures += kernel_set_ucred_svuid(pid, 0) != 0;
  failures += kernel_set_ucred_rgid(pid, 0) != 0;
  failures += kernel_set_ucred_svgid(pid, 0) != 0;
  if (KERNEL_ADDRESS_PRISON0)
    failures += kernel_set_ucred_prison(pid, KERNEL_ADDRESS_PRISON0) != 0;
  if (KERNEL_ADDRESS_ROOTVNODE) {
    failures += kernel_set_proc_rootdir(pid, KERNEL_ADDRESS_ROOTVNODE) != 0;
    failures += kernel_set_proc_jaildir(pid, KERNEL_ADDRESS_ROOTVNODE) != 0;
  }
  if (failures) {
    if (log)
      fprintf(log, "cred: elevate incomplete failures=%d (identity left as is)\n",
              failures);
    return -1;
  }
  elevated = 1;
  if (log) fprintf(log, "cred: ptrace identity adopted\n");
  return 0;
}

int pbs_ps4_cred_restore(FILE *log) {
  if (!elevated || !have_backup) return 0;
  const pid_t pid = getpid();
  int failures = 0;
  failures += kernel_set_ucred_uid(pid, saved_uid) != 0;
  failures += kernel_set_ucred_ruid(pid, saved_ruid) != 0;
  failures += kernel_set_ucred_svuid(pid, saved_svuid) != 0;
  failures += kernel_set_ucred_rgid(pid, saved_rgid) != 0;
  failures += kernel_set_ucred_svgid(pid, saved_svgid) != 0;
  if (saved_prison)
    failures += kernel_set_ucred_prison(pid, saved_prison) != 0;
  if (saved_rootdir) failures += kernel_set_proc_rootdir(pid, saved_rootdir) != 0;
  if (saved_jaildir) failures += kernel_set_proc_jaildir(pid, saved_jaildir) != 0;
  elevated = 0;
  if (log)
    fprintf(log, "cred: restore failures=%d\n", failures);
  return failures ? -1 : 0;
}
