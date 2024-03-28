// Copyright 2024 The TCMalloc Authors
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

#include <optional>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "tcmalloc/internal/percpu.h"

namespace tcmalloc::tcmalloc_internal::subtle::percpu {
namespace {

ABSL_CONST_INIT std::optional<bool> success;

TEST(PerCpu, IsRegistered) {
  // Verify preinit ran.  Its success should be identical to running it after
  // main has started.
  EXPECT_TRUE(success.has_value());
  EXPECT_EQ(success, IsFast());
}

void register_rseq() { success = IsFast(); }

__attribute__((section(".preinit_array"),
               used)) void (*__local_install_factory_preinit)() = register_rseq;

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal::subtle::percpu
