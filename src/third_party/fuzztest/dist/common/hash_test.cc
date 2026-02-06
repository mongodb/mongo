// Copyright 2022 The Centipede Authors.
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

#include "./common/hash.h"

#include "gtest/gtest.h"

namespace fuzztest::internal {
namespace {

TEST(UtilTest, Hash) {
  // The current implementation of Hash() is sha1.
  // Here we test a couple of inputs against their known sha1 values
  // obtained from the sha1sum command line utility.
  EXPECT_EQ(Hash({'a', 'b', 'c'}), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(Hash({'x', 'y'}), "5f8459982f9f619f4b0d9af2542a2086e56a4bef");
}

}  // namespace
}  // namespace fuzztest::internal
