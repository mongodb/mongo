//
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
//

#include "absl/strings/internal/str_format/extension.h"

#include <random>
#include <string>
#include "absl/strings/str_format.h"

#include "gtest/gtest.h"

namespace {

std::string MakeRandomString(size_t len) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis('a', 'z');
  std::string s(len, '0');
  for (char& c : s) {
    c = dis(gen);
  }
  return s;
}

TEST(FormatExtensionTest, SinkAppendSubstring) {
  for (size_t chunk_size : {1, 10, 100, 1000, 10000}) {
    std::string expected, actual;
    absl::str_format_internal::FormatSinkImpl sink(&actual);
    for (size_t chunks = 0; chunks < 10; ++chunks) {
      std::string rand = MakeRandomString(chunk_size);
      expected += rand;
      sink.Append(rand);
    }
    sink.Flush();
    EXPECT_EQ(actual, expected);
  }
}

TEST(FormatExtensionTest, SinkAppendChars) {
  for (size_t chunk_size : {1, 10, 100, 1000, 10000}) {
    std::string expected, actual;
    absl::str_format_internal::FormatSinkImpl sink(&actual);
    for (size_t chunks = 0; chunks < 10; ++chunks) {
      std::string rand = MakeRandomString(1);
      expected.append(chunk_size, rand[0]);
      sink.Append(chunk_size, rand[0]);
    }
    sink.Flush();
    EXPECT_EQ(actual, expected);
  }
}
}  // namespace
