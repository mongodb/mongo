// Copyright 2000 - 2007 Google Inc.
// All rights reserved.
//
// Author: Sanjay Ghemawat
//
// Portable implementation - just use glibc
//
// Note:  The glibc implementation may cause a call to malloc.
// This can cause a deadlock in HeapProfiler.

#ifndef ABSL_DEBUGGING_INTERNAL_STACKTRACE_GENERIC_INL_H_
#define ABSL_DEBUGGING_INTERNAL_STACKTRACE_GENERIC_INL_H_

#include <execinfo.h>
#include <cstring>

#include "absl/debugging/stacktrace.h"

template <bool IS_STACK_FRAMES, bool IS_WITH_CONTEXT>
static int UnwindImpl(void** result, int* sizes, int max_depth, int skip_count,
                      const void *ucp, int *min_dropped_frames) {
  static_cast<void>(ucp);  // Unused.
  static const int kStackLength = 64;
  void * stack[kStackLength];
  int size;

  size = backtrace(stack, kStackLength);
  skip_count++;  // we want to skip the current frame as well
  int result_count = size - skip_count;
  if (result_count < 0)
    result_count = 0;
  if (result_count > max_depth)
    result_count = max_depth;
  for (int i = 0; i < result_count; i++)
    result[i] = stack[i + skip_count];

  if (IS_STACK_FRAMES) {
    // No implementation for finding out the stack frame sizes yet.
    memset(sizes, 0, sizeof(*sizes) * result_count);
  }
  if (min_dropped_frames != nullptr) {
    if (size - skip_count - max_depth > 0) {
      *min_dropped_frames = size - skip_count - max_depth;
    } else {
      *min_dropped_frames = 0;
    }
  }

  return result_count;
}

namespace absl {
namespace debugging_internal {
bool StackTraceWorksForTest() {
  return true;
}
}  // namespace debugging_internal
}  // namespace absl

#endif  // ABSL_DEBUGGING_INTERNAL_STACKTRACE_GENERIC_INL_H_
