// Copyright 2019 The TCMalloc Authors
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

#include "tcmalloc/internal/environment.h"

#include <stdlib.h>
#include <string.h>

#include <string>

#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(EnvironmentTest, thread_safe_getenv) {
  // Should never be defined at test start
  const char *result, *undefined_env_var = "UTIL_TEST_UNDEFINED_ENV_VAR";

  // Check that we handle an undefined variable and then set it
  ASSERT_TRUE(getenv(undefined_env_var) == nullptr);
  ASSERT_TRUE(thread_safe_getenv(undefined_env_var) == nullptr);
  ASSERT_EQ(setenv(undefined_env_var, "1234567890", 0), 0);
  ASSERT_TRUE(getenv(undefined_env_var) != nullptr);

  // Make sure we can find the new variable
  result = thread_safe_getenv(undefined_env_var);
  ASSERT_TRUE(result != nullptr);
  // ... and that it matches what was set
  EXPECT_EQ(strcmp(result, getenv(undefined_env_var)), 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
