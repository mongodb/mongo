// Copyright 2017 The Abseil Authors.
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

#include "absl/strings/internal/resize_uninitialized.h"

#include "gtest/gtest.h"

namespace {

int resize_call_count = 0;

struct resizable_string {
  void resize(size_t) { resize_call_count += 1; }
};

int resize_default_init_call_count = 0;

struct resize_default_init_string {
  void resize(size_t) { resize_call_count += 1; }
  void __resize_default_init(size_t) { resize_default_init_call_count += 1; }
};

TEST(ResizeUninit, WithAndWithout) {
  resize_call_count = 0;
  resize_default_init_call_count = 0;
  {
    resizable_string rs;

    EXPECT_EQ(resize_call_count, 0);
    EXPECT_EQ(resize_default_init_call_count, 0);
    EXPECT_FALSE(
        absl::strings_internal::STLStringSupportsNontrashingResize(&rs));
    EXPECT_EQ(resize_call_count, 0);
    EXPECT_EQ(resize_default_init_call_count, 0);
    absl::strings_internal::STLStringResizeUninitialized(&rs, 237);
    EXPECT_EQ(resize_call_count, 1);
    EXPECT_EQ(resize_default_init_call_count, 0);
  }

  resize_call_count = 0;
  resize_default_init_call_count = 0;
  {
    resize_default_init_string rus;

    EXPECT_EQ(resize_call_count, 0);
    EXPECT_EQ(resize_default_init_call_count, 0);
    EXPECT_TRUE(
        absl::strings_internal::STLStringSupportsNontrashingResize(&rus));
    EXPECT_EQ(resize_call_count, 0);
    EXPECT_EQ(resize_default_init_call_count, 0);
    absl::strings_internal::STLStringResizeUninitialized(&rus, 237);
    EXPECT_EQ(resize_call_count, 0);
    EXPECT_EQ(resize_default_init_call_count, 1);
  }
}

}  // namespace
