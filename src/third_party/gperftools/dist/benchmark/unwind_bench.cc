// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "config.h"

#include "base/basictypes.h"
#include "gperftools/stacktrace.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "sys/ucontext.h"
#if HAVE_LIBUNWIND_H
#include <libunwind.h>
#endif

#include "run_benchmark.h"

extern "C" void getcontext_light(ucontext_t *ctx);

#define MAX_FRAMES 1024
static void *frames[MAX_FRAMES];

enum measure_mode {
  MODE_NOOP,
  MODE_WITH_CONTEXT,
  MODE_WITHOUT_CONTEXT
};

static int ATTRIBUTE_NOINLINE measure_unwind(int maxlevel, int mode) {
  int n;

  if (mode == MODE_NOOP)
    return 0;

  if (mode == MODE_WITH_CONTEXT) {
    ucontext_t uc;
    getcontext_light(&uc);
    n = GetStackTraceWithContext(frames, MAX_FRAMES, 0, &uc);
  } else {
    n = GetStackTrace(frames, MAX_FRAMES, 0);
  }
  if (n < maxlevel) {
    fprintf(stderr, "Expected at least %d frames, got %d\n", maxlevel, n);
    abort();
  }
  return 0;
}

static int ATTRIBUTE_NOINLINE frame_forcer(int rv) {
  return rv;
}

static int ATTRIBUTE_NOINLINE f1(int level, int maxlevel, int mode) {
  if (level == maxlevel)
    return frame_forcer(measure_unwind(maxlevel, mode));
  return frame_forcer(f1(level + 1, maxlevel, mode));
}

static void bench_unwind_no_op(long iterations, uintptr_t param) {
  do {
    f1(0, param, MODE_NOOP);
    iterations -= param;
  } while (iterations > 0);
}

static void bench_unwind_context(long iterations, uintptr_t param) {
  do {
    f1(0, param, MODE_WITH_CONTEXT);
    iterations -= param;
  } while (iterations > 0);
}

static void bench_unwind_no_context(long iterations, uintptr_t param) {
  do {
    f1(0, param, MODE_WITHOUT_CONTEXT);
    iterations -= param;
  } while (iterations > 0);
}

int main(void) {
  report_benchmark("unwind_no_op", bench_unwind_no_op, 100);
  report_benchmark("unwind_context", bench_unwind_context, 100);
  report_benchmark("unwind_no_context", bench_unwind_no_context, 100);

//// TODO: somehow this fails at linking step. Figure out why this is missing
// #if HAVE_LIBUNWIND_H
//   unw_set_caching_policy(unw_local_addr_space, UNW_CACHE_PER_THREAD);
// #endif
  return 0;
}
