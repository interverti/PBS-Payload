// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/controller.h"
#include "common/input/controller.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  _Static_assert(PBS_PS5_CONTROLLER == 0, "This test exercises a controller-free build");
  pbs_ps5_controller_configure();
  assert(!pbs_input_start());
  pbs_input_status status;
  pbs_input_get_status(&status);
  assert(!status.supported && !status.connected);
  char token[PBS_INPUT_TOKEN_SIZE];
  assert(pbs_input_connect(token) == 501);
  pbs_input_stop();
  pbs_input_wait();
  pbs_input_shutdown();
  puts("PS5 controller excluded: no native backend, registration refused");
  return 0;
}
