/* Host prototype for the dedicated worker thread.
 *
 * The test executes the real ps4_worker_entry.S machine code in an RWX
 * mapping of the test process, starts it with a real pthread exactly like the
 * payload will do inside RemotePlay, and drives the same pbs_worker_call /
 * pbs_worker_exit client used on console. This proves the stub, the job ABI,
 * the phase protocol and the self-unmap exit path before any console run.
 *
 * The transport here is a direct mapping; on PS4 the transport is still to be
 * chosen because kernel_copyin/kernel_copyout only address kernel memory. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ps4_worker.h"

extern const unsigned char pbs_worker_code_start[], pbs_worker_code_end[];

static int mem_read(void *opaque, uint64_t address, void *dst, size_t size) {
  (void)opaque;
  memcpy(dst, (const void *)(uintptr_t)address, size);
  return 0;
}

static int mem_write(void *opaque, uint64_t address, const void *src,
                     size_t size) {
  (void)opaque;
  memcpy((void *)(uintptr_t)address, src, size);
  return 0;
}

static const pbs_worker_io mem_io = {mem_read, mem_write, NULL};

static void test_sleep(int usec) { usleep((useconds_t)usec); }

static int64_t add6(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                    uint64_t f) {
  return (int64_t)(a + b + c + d + e + f);
}

static int64_t slow_call(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                         uint64_t e, uint64_t f) {
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  usleep(200000);
  return (int64_t)a;
}

/* Map the stub, publish the job in a page of the same mapping, and start the
 * worker thread with the stub entry. */
static pbs_worker_job *start_worker(void **mapping, size_t *mapping_size,
                                    pthread_t *thread) {
  const size_t code_size = (size_t)(pbs_worker_code_end - pbs_worker_code_start);
  const size_t page = 0x1000;
  void *map = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(map != MAP_FAILED && code_size < page);
  memcpy(map, pbs_worker_code_start, code_size);
  /* The job lives in its own mapping: the thread unmaps only the code page on
   * exit, so the payload can still read phase = 4 afterwards. */
  pbs_worker_job *job = mmap(NULL, page, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(job != MAP_FAILED);
  pbs_worker_job_init(job, (uint64_t)(uintptr_t)&test_sleep,
                      (uint64_t)(uintptr_t)&munmap, (uint64_t)(uintptr_t)map,
                      (uint64_t)page);
  int rc = pthread_create(thread, NULL, (void *(*)(void *))map, job);
  assert(rc == 0);
  *mapping = map;
  *mapping_size = page;
  return job;
}

int main(void) {
  void *map = NULL;
  size_t map_size = 0;
  pthread_t thread;
  pbs_worker_job *job = start_worker(&map, &map_size, &thread);
  pbs_worker_t worker = {&mem_io, (uint64_t)(uintptr_t)job, 2000, 0};
  assert(pbs_worker_wait_ready(&worker) == 0);
  assert(job->ready == 1);

  int64_t result = 0;
  uint64_t args[6] = {1, 2, 3, 4, 5, 6};
  assert(pbs_worker_call(&worker, (uint64_t)(uintptr_t)&add6, args, &result) == 0);
  assert(result == 21);
  uint64_t args2[6] = {100, 200, 300, 400, 500, 600};
  assert(pbs_worker_call(&worker, (uint64_t)(uintptr_t)&add6, args2, &result) ==
         0);
  assert(result == 2100);

  /* Exit: the thread unmaps its own mapping and returns into the pthread
   * caller; the join must succeed. */
  assert(pbs_worker_exit(&worker) == 0);
  assert(pthread_join(thread, NULL) == 0);
  printf("worker: ready, six-argument calls and self-unmap exit verified\n");

  /* Timeout: a call that outlives the budget marks the worker fatal and the
   * stale answer is never accepted as the next result. */
  pthread_t slow_thread;
  void *slow_map = NULL;
  size_t slow_size = 0;
  pbs_worker_job *slow_job = start_worker(&slow_map, &slow_size, &slow_thread);
  pbs_worker_t slow = {&mem_io, (uint64_t)(uintptr_t)slow_job, 20, 0};
  assert(pbs_worker_wait_ready(&slow) == 0);
  uint64_t slow_args[6] = {7, 0, 0, 0, 0, 0};
  assert(pbs_worker_call(&slow, (uint64_t)(uintptr_t)&slow_call, slow_args,
                         &result) == -1);
  assert(slow.fatal == 1);
  assert(pbs_worker_call(&slow, (uint64_t)(uintptr_t)&add6, args, &result) ==
         -1);
  printf("worker: timeout marks the worker fatal and blocks reuse\n");

  (void)slow_thread;
  (void)slow_map;
  (void)slow_size;
  (void)map;
  (void)map_size;
  printf("PS4 worker prototype: real stub executed by a pthread, ABI and "
         "protocol verified\n");
  return 0;
}
