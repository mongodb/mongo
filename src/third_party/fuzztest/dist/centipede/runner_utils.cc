// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/runner_utils.h"

#include <pthread.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "absl/base/nullability.h"

namespace fuzztest::internal {

void PrintErrorAndExitIf(bool condition, const char* absl_nonnull error) {
  if (!condition) return;
  fprintf(stderr, "error: %s\n", error);
  exit(1);
}

uintptr_t GetCurrentThreadStackRegionLow() {
#ifdef __APPLE__
  pthread_t self = pthread_self();
  const auto stack_addr =
      reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
  const auto stack_size = pthread_get_stacksize_np(self);
  return stack_addr - stack_size;
#else   // __APPLE__
  pthread_attr_t attr = {};
  if (pthread_getattr_np(pthread_self(), &attr) != 0) {
    fprintf(stderr, "Failed to get the pthread attr of the current thread.\n");
    return 0;
  }
  void *stack_addr = nullptr;
  size_t stack_size = 0;
  if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) != 0) {
    fprintf(stderr, "Failed to get the stack region of the current thread.\n");
    pthread_attr_destroy(&attr);
    return 0;
  }
  pthread_attr_destroy(&attr);
  const auto stack_region_low = reinterpret_cast<uintptr_t>(stack_addr);
  RunnerCheck(stack_region_low != 0,
              "the current thread stack region starts from 0 - unexpected!");
  return stack_region_low;
#endif  // __APPLE__
}

}  // namespace fuzztest::internal
