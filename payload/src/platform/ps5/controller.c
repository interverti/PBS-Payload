// SPDX-License-Identifier: GPL-3.0-or-later
/* Native virtual-pad exports replace the legacy prototype's fixed ioctls,
 * profile tables and demo input loop. No target-process injection or offsets. */
#include "platform/ps5/controller.h"
#if PBS_PS5_CONTROLLER
#include "common/input/controller.h"
#include "platform/ps5/offset/profile.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ps5/kernel.h>

int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, uint32_t *);
typedef struct {
  int (*init)(void);
  int (*privilege)(int);
  int (*add)(const void *, void *);
  int (*insert)(int, const void *);
  int (*remove)(int);
  uint32_t device;
  int ready, privileged, owned, uncertain;
} native_pad;
static native_pad pad;
#ifndef PBS_CONTROLLER_LOG
#define PBS_CONTROLLER_LOG "/data/pbs/controller.log"
#endif
static void log_result(const char *operation, int rc) {
  FILE *f = fopen(PBS_CONTROLLER_LOG, "a");
  if (f) {
    fprintf(f, "fw=0x%08x %s rc=0x%08x device=0x%08x owned=%d\n", kernel_get_fw_version(),
            operation, (unsigned)rc, pad.device, pad.owned);
    fclose(f);
  }
}
static int prepare(void) {
  if (pad.ready) {
    return 0;
  }
  uint32_t module = 0;
  if (kernel_dynlib_handle(getpid(), "libScePad.sprx", &module)) {
    int rc = sceKernelLoadStartModule("/system/common/lib/libScePad.sprx", 0, NULL, 0, NULL, NULL);
    log_result("Load libScePad", rc);
    if (rc < 0 || kernel_dynlib_handle(getpid(), "libScePad.sprx", &module)) {
      return -1;
    }
  }
  pad.init = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadInit");
  pad.privilege = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadSetProcessPrivilege");
  pad.add = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadVirtualDeviceAddDevice");
  pad.insert = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadVirtualDeviceInsertData");
  pad.remove = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadVirtualDeviceDeleteDevice");
  if ((intptr_t)pad.init <= 0 || (intptr_t)pad.privilege <= 0 || (intptr_t)pad.add <= 0 ||
      (intptr_t)pad.insert <= 0 || (intptr_t)pad.remove <= 0) {
    log_result("Required virtual-pad exports unavailable", -1);
    return -1;
  }
  int rc = pad.init();
  log_result("scePadInit", rc);
  if (rc) {
    return rc;
  }
  pad.ready = 1;
  return 0;
}
static int connect_pad(void *unused) {
  (void)unused;
  if (pad.uncertain) {
    return -1;
  }
  int rc = prepare();
  if (rc) {
    return rc;
  }
  if (!pad.privileged) {
    rc = pad.privilege(1);
    log_result("SetProcessPrivilege(1)", rc);
    if (rc) {
      return rc;
    }
    pad.privileged = 1;
  }
  /* The virtual device belongs to the payload, not to a viewer session.
   * Reuse it for later sessions instead of creating and deleting one device
   * per connect: repeated Add/Delete exhausts libScePad's device pool. */
  if (pad.owned) {
    return 0;
  }
  /* AddDevice reads the caller's u32 parameter and profile byte at +4.
   * The library selects its firmware-native kernel representation itself. */
  unsigned char request[0x40] = {0}, output[16] = {0};
  request[4] = 1;
  memset(output, 0xff, 4);
  rc = pad.add(request, output);
  if (!rc) {
    memcpy(&pad.device, output, 4);
    if (pad.device == UINT32_MAX) {
      pad.uncertain = 1;
      rc = -1;
    } else {
      pad.owned = 1;
    }
  }
  log_result("VirtualDeviceAddDevice(profile=1)", rc);
  return rc;
}
static int write_pad(void *unused, const pbs_input_state *state) {
  (void)unused;
  if (!pad.owned) {
    return -1;
  }
  /* Stable exported InsertData input: buttons u32, sticks u8[4], triggers
   * u8[2], followed by zeroed motion/touch fields. Native wrapper owns the
   * larger driver packet, including its connection flag. */
  unsigned char data[0x80] = {0};
  memcpy(data, &state->buttons, 4);
  data[4] = state->lx;
  data[5] = state->ly;
  data[6] = state->rx;
  data[7] = state->ry;
  data[8] = state->l2;
  data[9] = state->r2;
  float orientation_w = 1.0f;
  memcpy(data + 0x18, &orientation_w, 4);
  int rc = pad.insert((int)pad.device, data);
  if (rc) {
    log_result("VirtualDeviceInsertData", rc);
  }
  return rc;
}
static int disconnect_pad(void *unused) {
  (void)unused;
  if (pad.uncertain) {
    log_result("Unknown device ID; no blind delete or reconnect", -1);
    return -1;
  }
  /* Session release only: keep the device and process privilege registered
   * until destroy_pad(), so a reconnect never calls AddDevice again. */
  return 0;
}
static int destroy_pad(void *unused) {
  (void)unused;
  if (pad.uncertain) {
    log_result("Unknown device ID; no blind delete or reconnect", -1);
    return -1;
  }
  if (pad.owned) {
    int rc = pad.remove((int)pad.device);
    log_result("VirtualDeviceDeleteDevice", rc);
    if (rc) {
      return rc;
    }
    pad.owned = 0;
    pad.device = 0;
  }
  if (pad.privileged) {
    int rc = pad.privilege(0);
    log_result("SetProcessPrivilege(0)", rc);
    if (rc) {
      return rc;
    }
    pad.privileged = 0;
  }
  pad.ready = 0;
  return 0;
}
void pbs_ps5_controller_configure(void) {
  static const pbs_input_backend backend = {connect_pad, write_pad, disconnect_pad, destroy_pad};
  /* Follow the same exact/family selector as video; no copied firmware list. */
  if (pbs_ps5_find_offsets(kernel_get_fw_version())) {
    pbs_input_configure(&backend, NULL);
  }
}
#endif
