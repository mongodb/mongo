// Copyright 2023 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/internal/allocation_guard.h"

#include <new>

#include "gtest/gtest.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(AllocationGuard, Noncooperative) {
  absl::base_internal::SpinLock lock(absl::kConstInit,
                                     absl::base_internal::SCHEDULE_KERNEL_ONLY);
  AllocationGuardSpinLockHolder h(&lock);
}

TEST(AllocationGuard, CooperativeDeathTest) {
  absl::base_internal::SpinLock lock;

  EXPECT_DEBUG_DEATH({ AllocationGuardSpinLockHolder h(&lock); },
                     "SIGABRT received");
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
