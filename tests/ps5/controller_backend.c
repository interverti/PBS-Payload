// SPDX-License-Identifier: GPL-3.0-or-later
/* Exercise the native adapter without loading a module or touching a device. */
#define PBS_CONTROLLER_LOG "/dev/null"
#include "platform/ps5/controller.c"
#include <assert.h>

static const pbs_input_backend *configured;
static uint32_t firmware = 0x09400008;
static int init_rc, add_rc, delete_rc, privilege_rc, insert_rc;
static int init_calls, add_calls, delete_calls, privilege_value, missing_export, invalid_id;
static unsigned char inserted[0x80];

uint32_t kernel_get_fw_version(void) {
  return firmware;
}
void pbs_input_configure(const pbs_input_backend *backend, void *opaque) {
  assert(!opaque);
  configured = backend;
}
int kernel_dynlib_handle(pid_t pid, const char *name, uint32_t *handle) {
  assert(pid == getpid() && !strcmp(name, "libScePad.sprx"));
  *handle = 1;
  return 0;
}
int sceKernelLoadStartModule(const char *name, size_t size, const void *args, uint32_t flags,
                             void *options, uint32_t *result) {
  (void)name;
  (void)size;
  (void)args;
  (void)flags;
  (void)options;
  (void)result;
  assert(!"module already present in this mock");
  return -1;
}
static int mock_init(void) {
  init_calls++;
  return init_rc;
}
static int mock_privilege(int value) {
  if (privilege_rc) {
    return privilege_rc;
  }
  privilege_value = value;
  return 0;
}
static int mock_add(const void *request, void *output) {
  const unsigned char *bytes = request;
  add_calls++;
  for (unsigned i = 0; i < 0x40; i++) {
    assert(bytes[i] == (i == 4 ? 1 : 0));
  }
  if (!add_rc && !invalid_id) {
    uint32_t id = 0x14030d;
    memcpy(output, &id, sizeof id);
  }
  return add_rc;
}
static int mock_insert(int device, const void *state) {
  assert(device == 0x14030d);
  memcpy(inserted, state, sizeof inserted);
  return insert_rc;
}
static int mock_delete(int device) {
  assert(device == 0x14030d);
  delete_calls++;
  return delete_rc;
}
intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t module, const char *name) {
  assert(pid == getpid() && module == 1);
  if (missing_export && !strcmp(name, "scePadVirtualDeviceInsertData")) {
    return -1;
  }
  if (!strcmp(name, "scePadInit")) {
    return (intptr_t)mock_init;
  }
  if (!strcmp(name, "scePadSetProcessPrivilege")) {
    return (intptr_t)mock_privilege;
  }
  if (!strcmp(name, "scePadVirtualDeviceAddDevice")) {
    return (intptr_t)mock_add;
  }
  if (!strcmp(name, "scePadVirtualDeviceInsertData")) {
    return (intptr_t)mock_insert;
  }
  if (!strcmp(name, "scePadVirtualDeviceDeleteDevice")) {
    return (intptr_t)mock_delete;
  }
  assert(!"unexpected export");
  return -1;
}
static void reset(void) {
  memset(&pad, 0, sizeof pad);
  init_rc = add_rc = delete_rc = privilege_rc = insert_rc = 0;
  init_calls = add_calls = delete_calls = privilege_value = missing_export = invalid_id = 0;
}
int main(void) {
  static const uint32_t firmwares[] = {0x03200004, 0x04000042, 0x04510001, 0x05000040, 0x05100023,
                                       0x05500008, 0x06020004, 0x07200005, 0x08400005, 0x09200005,
                                       0x09400008, 0x09600004, 0x10010000, 0x10600006, 0x11200005,
                                       0x11400005, 0x11600005, 0x12000043, 0x12400009, 0x12700001,
                                       0x13000040, 0x13200006, 0x13420000, 0x13600007};
  for (unsigned i = 0; i < sizeof firmwares / sizeof firmwares[0]; i++) {
    firmware = firmwares[i];
    configured = NULL;
    pbs_ps5_controller_configure();
    assert(configured);
  }
  firmware = 0x99990000;
  configured = NULL;
  pbs_ps5_controller_configure();
  assert(!configured);
  firmware = 0x09400008;

  reset();
  missing_export = 1;
  assert(connect_pad(NULL) && !init_calls && !add_calls);
  assert(!disconnect_pad(NULL));
  reset();
  init_rc = -7;
  assert(connect_pad(NULL) == -7 && !add_calls);
  assert(!disconnect_pad(NULL));
  reset();
  privilege_rc = -8;
  assert(connect_pad(NULL) == -8 && !add_calls);
  assert(!disconnect_pad(NULL));
  reset();
  add_rc = -9;
  assert(connect_pad(NULL) == -9 && privilege_value == 1);
  assert(!disconnect_pad(NULL) && privilege_value == 0 && !delete_calls);

  reset();
  assert(!connect_pad(NULL) && pad.owned && privilege_value == 1);
  assert(connect_pad(NULL) && add_calls == 1);
  pbs_input_state state = {
      .buttons = 0x4000, .lx = 0, .ly = 255, .rx = 19, .ry = 128, .l2 = 15, .r2 = 254};
  assert(!write_pad(NULL, &state));
  unsigned char expected[0x80] = {0};
  memcpy(expected, &state.buttons, 4);
  expected[4] = 0;
  expected[5] = 255;
  expected[6] = 19;
  expected[7] = 128;
  expected[8] = 15;
  expected[9] = 254;
  float orientation_w = 1.0f;
  memcpy(expected + 0x18, &orientation_w, 4);
  assert(!memcmp(inserted, expected, sizeof expected));
  insert_rc = -10;
  assert(write_pad(NULL, &state) == -10);
  delete_rc = -11;
  assert(disconnect_pad(NULL) == -11 && pad.owned && privilege_value == 1);
  assert(connect_pad(NULL) && add_calls == 1);
  delete_rc = 0;
  assert(!disconnect_pad(NULL) && !pad.owned && privilege_value == 0);
  assert(!disconnect_pad(NULL) && delete_calls == 2);
  assert(!connect_pad(NULL) && init_calls == 1);
  privilege_rc = -12;
  assert(disconnect_pad(NULL) == -12 && !pad.owned && pad.privileged);
  assert(connect_pad(NULL) && add_calls == 2);

  reset();
  invalid_id = 1;
  assert(connect_pad(NULL) && pad.uncertain);
  assert(disconnect_pad(NULL) && !delete_calls && privilege_value == 1);
  assert(connect_pad(NULL) && add_calls == 1);
  puts("PS5 native controller: selector, export probing, state layout and cleanup failures "
       "verified offline");
  return 0;
}
