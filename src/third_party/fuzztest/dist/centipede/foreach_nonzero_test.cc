// Copyright 2023 The Centipede Authors.
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

#include "./centipede/foreach_nonzero.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace fuzztest::internal {
namespace {

void TrivialForEachNonZeroByte(uint8_t *bytes, size_t num_bytes,
                               std::function<void(size_t, uint8_t)> action) {
  for (size_t i = 0; i < num_bytes; i++) {
    uint8_t value = bytes[i];
    if (value) {
      action(i, value);
      bytes[i] = 0;
    }
  }
}

TEST(ForEachNonZeroByte, ProcessesSubArrays) {
  // Some long data with long spans of zeros and a few non-zeros.
  // We will test all sub-arrays of this array.
  const uint8_t test_data[] = {
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  const size_t kTestDataSize = sizeof(test_data);
  uint8_t test_data_copy[kTestDataSize];

  auto CheckResult = [&](size_t offset, size_t size,
                         const std::vector<std::pair<size_t, uint8_t>> &v) {
    for (size_t i = 0; i < kTestDataSize; ++i) {
      if (i >= offset && i < offset + size) {
        EXPECT_EQ(test_data_copy[i], 0);
      } else {
        EXPECT_EQ(test_data_copy[i], test_data[i]);
      }
    }
  };

  for (size_t offset = 0; offset <= kTestDataSize; offset++) {
    for (size_t size = 0; offset + size <= kTestDataSize; size++) {
      std::vector<std::pair<size_t, uint8_t>> v1, v2;
      memcpy(test_data_copy, test_data, kTestDataSize);
      TrivialForEachNonZeroByte(
          test_data_copy + offset, size,
          [&](size_t idx, uint8_t value) { v1.emplace_back(idx, value); });
      CheckResult(offset, size, v1);

      memcpy(test_data_copy, test_data, kTestDataSize);
      ForEachNonZeroByte(
          test_data_copy + offset, size,
          [&](size_t idx, uint8_t value) { v2.emplace_back(idx, value); });
      CheckResult(offset, size, v2);

      EXPECT_EQ(v1, v2);
    }
  }
}

}  // namespace
}  // namespace fuzztest::internal
