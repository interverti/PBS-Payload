// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test for the pure offset-report layer of the PS4 dump payload.
 * No console, no kernel: the measured values are supplied here. */
#include "dump/ps4/offset_report.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect(const char *actual, const char *expected, const char *what) {
  if (strcmp(actual, expected)) {
    fprintf(stderr, "%s:\n  actual:   %s\n  expected: %s\n", what, actual,
            expected);
    assert(0);
  }
}

static void catalog_test(void) {
  size_t count = 0;
  const pbs_ps4_dump_symbol *symbols = pbs_ps4_dump_symbols(&count);
  assert(symbols && count == 15);
  assert(pbs_ps4_dump_symbols(NULL) == symbols);
  for (size_t i = 0; i < count; ++i) {
    assert(symbols[i].label && *symbols[i].label);
    assert(symbols[i].symbol && *symbols[i].symbol);
    assert(symbols[i].module);
    assert(!strcmp(symbols[i].module, "libSceAvcap.sprx") ||
           !strcmp(symbols[i].module, "libSceLibcInternal.sprx") ||
           !strcmp(symbols[i].module, "libkernel_sys.sprx"));
    for (size_t k = i + 1; k < count; ++k)
      assert(strcmp(symbols[i].label, symbols[k].label));
  }
  assert(!strcmp(pbs_ps4_dump_unresolved(), "unresolved"));
  printf("offset report: catalog stable (%zu symbols)\n", count);
}

static void rva_test(void) {
  assert(pbs_ps4_dump_rva(0x1000 + 0x940, 0x1000, 0x10000) == 0x940);
  assert(pbs_ps4_dump_rva(0x1000, 0x1000, 0x10) == 0);
  assert(pbs_ps4_dump_rva(0xfff, 0x1000, 0x10) == -1);   /* before the base */
  assert(pbs_ps4_dump_rva(0, 0x1000, 0x10) == -1);       /* unresolved symbol */
  assert(pbs_ps4_dump_rva(0x1000, 0, 0x10) == -1);       /* no module base */
  assert(pbs_ps4_dump_rva(0x1000 + 0x10, 0x1000, 0x10) == -1); /* outside */
  assert(pbs_ps4_dump_rva(0x1000 + 0xf, 0x1000, 0x10) == 0xf);
  /* Unknown span: only the sign is enforced, never a guessed size. */
  assert(pbs_ps4_dump_rva(0x1000 + 0x123456, 0x1000, 0) == 0x123456);
  printf("offset report: rva bounds verified\n");
}

static void line_test(void) {
  char line[512];
  int n = pbs_ps4_dump_module_line(line, sizeof line, "SceRemotePlay",
                                   "libSceAvcap.sprx", 3, 0x7f000000u,
                                   0x300000u, 0x7f000100);
  assert(n > 0 && (size_t)n < sizeof line);
  expect(line,
         "module process=SceRemotePlay name=libSceAvcap.sprx handle=3"
         " base=0x7f000000 size=0x300000 entry=0x7f000100",
         "module line");
  n = pbs_ps4_dump_symbol_line(line, sizeof line, "SceRemotePlay", "avcap_init",
                               "libSceAvcap.sprx", 0x7f000940, 0x940);
  expect(line,
         "symbol process=SceRemotePlay module=libSceAvcap.sprx"
         " label=avcap_init address=0x7f000940 rva=0x940",
         "resolved symbol line");
  n = pbs_ps4_dump_symbol_line(line, sizeof line, "SceRemotePlay", "avcap_init",
                               "libSceAvcap.sprx", 0, -1);
  expect(line,
         "symbol process=SceRemotePlay module=libSceAvcap.sprx"
         " label=avcap_init address=0x0 rva=unresolved",
         "unresolved symbol line");
  assert(n > 0);
  n = pbs_ps4_dump_offset_line(line, sizeof line, "avcap_init", 0x940);
  expect(line, "avcap_init=0x940", "resolved offset line");
  n = pbs_ps4_dump_offset_line(line, sizeof line, "avcap_trap", -1);
  expect(line, "avcap_trap=unresolved", "unresolved offset line");
  assert(n > 0);
  /* Truncation is reported, never hidden. */
  n = pbs_ps4_dump_module_line(line, 8, "SceRemotePlay", "libSceAvcap.sprx", 3,
                               0x7f000000u, 0x300000u, 0x7f000100);
  assert(n >= 8);
  assert(pbs_ps4_dump_offset_line(line, sizeof line, "", 1) == -1);
  printf("offset report: line formats verified\n");
}

static void layout(pbs_ps4_firmware *fw) {
  memset(fw, 0, sizeof *fw);
  fw->name = "test";
  fw->version = 0x11000000u;
  fw->version_mask = 0xffff0000u;
  fw->acmgr_offset = 0x3d0de0;
  fw->acmgr_signature[0] = 0xb0;
  fw->acmgr_signature[1] = 0x01;
  fw->acmgr_signature[2] = 0xc3;
  fw->gates[0].offset = 0x384285;
  fw->gates[0].before[0] = 0x77;
  fw->gates[0].before[1] = 0x1c;
  fw->gates[0].after[0] = 0xeb;
  fw->gates[0].after[1] = 0x1c;
  fw->gates[0].size = 2;
  fw->gates[1].offset = 0x384771;
  fw->gates[1].before[0] = 0x48;
  fw->gates[1].before[1] = 0x8b;
  fw->gates[1].before[2] = 0x41;
  fw->gates[1].before[3] = 0x08;
  fw->gates[1].before[4] = 0x44;
  fw->gates[1].after[0] = 0xe9;
  fw->gates[1].after[1] = 0x7c;
  fw->gates[1].after[2] = 0x02;
  fw->gates[1].after[3] = 0x00;
  fw->gates[1].after[4] = 0x00;
  fw->gates[1].size = 5;
}

static void report_test(void) {
  pbs_ps4_firmware fw;
  layout(&fw);
  char line[1024];
  int n = pbs_ps4_dump_kernel_line(line, sizeof line, &fw, 0x11000000u);
  assert(n > 0 && (size_t)n < sizeof line);
  expect(line,
         "kernel layout status=discovered firmware=0x11000000"
         " acmgr=0x3d0de0 gate0=0x384285 before0=0x77,0x1c after0=0xeb,0x1c"
         " size0=2 gate1=0x384771 before1=0x48,0x8b,0x41,0x08,0x44"
         " after1=0xe9,0x7c,0x02,0x00,0x00 size1=5",
         "kernel layout line");
  n = pbs_ps4_dump_snippet(line, sizeof line, &fw, 0x11000000u);
  assert(n > 0 && (size_t)n < sizeof line);
  expect(line,
         "/* Measured on the console; paste into the legacy table in\n"
         " * system/firmware.h and keep PBS_PS4_DYNAMIC_LAYOUT=1 in\n"
         " * normal builds. */\n"
         "  .name = \"PS4 11000000 / runtime dump\",\n"
         "  .version = 0x11000000u, .version_mask = 0xffff0000u,\n"
         "  .acmgr_offset = 0x3d0de0,\n"
         "  .acmgr_signature = {0xb0, 0x01, 0xc3},\n"
         "  .gates = {\n"
         "    {0x384285, {0x77,0x1c}, {0xeb,0x1c}, 2},\n"
         "    {0x384771, {0x48,0x8b,0x41,0x08,0x44}, {0xe9,0x7c,0x02,0x00,0x00}, 5}\n"
         "  },\n",
         "firmware snippet");
  n = pbs_ps4_dump_snippet(line, sizeof line, NULL, 0x11000000u);
  assert(n > 0);
  expect(line, "/* runtime layout unavailable; no snippet */\n",
         "missing snippet");
  const unsigned char magic[4] = {0x7f, 0x45, 0x4c, 0x46};
  n = pbs_ps4_dump_kernel_probe_line(line, sizeof line, 0xffffffffd152c000ull,
                                     0x2820268u, 0, magic, sizeof magic);
  assert(n > 0 && (size_t)n < sizeof line);
  expect(line,
         "kernel probe base=0xffffffffd152c000 image=0x2820268 read=0"
         " magic=7f454c46",
         "kernel probe line");
  n = pbs_ps4_dump_kernel_probe_line(line, sizeof line, 0xffffffff82200000ull,
                                     0x1000u, -1, magic, sizeof magic);
  assert(n > 0);
  expect(line,
         "kernel probe base=0xffffffff82200000 image=0x1000 read=-1"
         " magic=unread",
         "failed kernel probe line");
  printf("offset report: kernel line and snippet verified\n");
}

int main(void) {
  catalog_test();
  rva_test();
  line_test();
  report_test();
  printf("Offset report formatting verified.\n");
  return 0;
}
