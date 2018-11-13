// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/base/internal/atomic_hook.h"

#include "gtest/gtest.h"
#include "absl/base/attributes.h"

namespace {

int value = 0;
void TestHook(int x) { value = x; }

TEST(AtomicHookTest, NoDefaultFunction) {
  ABSL_CONST_INIT static absl::base_internal::AtomicHook<void(*)(int)> hook;
  value = 0;

  // Test the default DummyFunction.
  EXPECT_TRUE(hook.Load() == nullptr);
  EXPECT_EQ(value, 0);
  hook(1);
  EXPECT_EQ(value, 0);

  // Test a stored hook.
  hook.Store(TestHook);
  EXPECT_TRUE(hook.Load() == TestHook);
  EXPECT_EQ(value, 0);
  hook(1);
  EXPECT_EQ(value, 1);

  // Calling Store() with the same hook should not crash.
  hook.Store(TestHook);
  EXPECT_TRUE(hook.Load() == TestHook);
  EXPECT_EQ(value, 1);
  hook(2);
  EXPECT_EQ(value, 2);
}

TEST(AtomicHookTest, WithDefaultFunction) {
  // Set the default value to TestHook at compile-time.
  ABSL_CONST_INIT static absl::base_internal::AtomicHook<void (*)(int)> hook(
      TestHook);
  value = 0;

  // Test the default value is TestHook.
  EXPECT_TRUE(hook.Load() == TestHook);
  EXPECT_EQ(value, 0);
  hook(1);
  EXPECT_EQ(value, 1);

  // Calling Store() with the same hook should not crash.
  hook.Store(TestHook);
  EXPECT_TRUE(hook.Load() == TestHook);
  EXPECT_EQ(value, 1);
  hook(2);
  EXPECT_EQ(value, 2);
}

}  // namespace
