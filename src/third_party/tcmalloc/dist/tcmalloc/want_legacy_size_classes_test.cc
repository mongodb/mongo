// Copyright 2022 The TCMalloc Authors
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

#include <stddef.h>

#include "gtest/gtest.h"
#include "absl/base/macros.h"
#include "tcmalloc/common.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(SizeClassesTest, SmallClasses) {
  // This test needs to validate against the actual SizeMap TCMalloc will use.
  Static::InitIfNecessary();

  if (__STDCPP_DEFAULT_NEW_ALIGNMENT__ > 8)
    GTEST_SKIP() << "Unexpected default new alignment.";

  const size_t kExpectedClasses[] = {0, 8, 16, 24, 32, 40, 48, 56, 64};
  const size_t kExpectedClassesSize = ABSL_ARRAYSIZE(kExpectedClasses);
  ASSERT_LE(kExpectedClassesSize, kNumClasses);
  for (int c = 0; c < kExpectedClassesSize; ++c) {
    EXPECT_EQ(Static::sizemap().class_to_size(c), kExpectedClasses[c]) << c;
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
