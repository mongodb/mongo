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

#include "gtest/gtest.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/environment.h"

namespace tcmalloc {
namespace {

TEST(TCMallocVariant, KnownExperimentVariants) {
  // This test confirms that all experiment parameters passed for testing
  // purposes are known to the binary.  If any are unknown, this indicates a
  // test configuration that can be cleaned up.
  auto env = absl::NullSafeStringView(
      tcmalloc_internal::thread_safe_getenv("BORG_EXPERIMENTS"));
  if (env.empty()) {
    return;
  }
  for (auto name : absl::StrSplit(env, ',')) {
    auto exp = FindExperimentByName(name);
    EXPECT_TRUE(exp && IsExperimentActive(exp.value()));
  }
}

}  // namespace
}  // namespace tcmalloc
