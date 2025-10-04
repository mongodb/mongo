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

#include <stdint.h>
#include <stdlib.h>

#include <memory>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/strings/str_format.h"
#include "tcmalloc/selsan/selsan.h"

namespace tcmalloc::tcmalloc_internal::selsan {
namespace {

class Test : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!IsEnabled()) {
      GTEST_SKIP() << "SelSan is not enabled";
    }
  }

  template <typename T>
  T* mistag(void* p) {
    return reinterpret_cast<T*>((reinterpret_cast<uintptr_t>(p) + (1ul << 57)) &
                                ~(1ul << 63));
  }
};

TEST_F(Test, Format) {
  uint64_t var = 0;
  auto Warning = [&](size_t size, bool write) {
    return absl::StrFormat(
        "WARNING: SelSan: %s tag-mismatch at addr 0x[0-9a-f]+ ptr/mem "
        "tag:(1|2)/0 "
        "size:%zu",
        write ? "write" : "read", size);
  };
  ABSL_ATTRIBUTE_UNUSED volatile uint64_t sink;
  EXPECT_DEATH(mistag<uint8_t>(&var)[0] = 1, Warning(1, true));
  EXPECT_DEATH(mistag<uint16_t>(&var)[0] = 1, Warning(2, true));
  EXPECT_DEATH(mistag<uint32_t>(&var)[0] = 1, Warning(4, true));
  EXPECT_DEATH(mistag<uint64_t>(&var)[0] = 1, Warning(8, true));
  EXPECT_DEATH(sink = mistag<uint8_t>(&var)[0], Warning(1, false));
  EXPECT_DEATH(sink = mistag<uint16_t>(&var)[0], Warning(2, false));
  EXPECT_DEATH(sink = mistag<uint32_t>(&var)[0], Warning(4, false));
  EXPECT_DEATH(sink = mistag<uint64_t>(&var)[0], Warning(8, false));
}

TEST_F(Test, Heap) {
  auto Warning = [&](size_t size, size_t offset) {
    return absl::StrFormat(
        "WARNING: SelSan: write tag-mismatch at addr 0x[0-9a-f]+ ptr/mem "
        "tag:[0-9]+/[0-9]+ size:1\n"
        "Heap object 0x[0-9a-f]+-0x[0-9a-f]+ \\(size %zu, offset %zd\\)\n",
        size, offset);
  };
  std::unique_ptr<char[]> obj(new char[16]);
  EXPECT_DEATH(mistag<uint8_t>(obj.get() + 0)[0] = 1, Warning(16, 0));
  EXPECT_DEATH(mistag<uint8_t>(obj.get() + 1)[0] = 1, Warning(16, 1));
  EXPECT_DEATH(mistag<uint8_t>(obj.get() + 15)[0] = 1, Warning(16, 15));
  obj.reset(new char[60]);
  EXPECT_DEATH(mistag<uint8_t>(obj.get() + 0)[0] = 1, Warning(64, 0));
  EXPECT_DEATH(mistag<uint8_t>(obj.get() + 10)[0] = 1, Warning(64, 10));
  EXPECT_DEATH(mistag<uint8_t>(obj.get() + 63)[0] = 1, Warning(64, 63));
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal::selsan
