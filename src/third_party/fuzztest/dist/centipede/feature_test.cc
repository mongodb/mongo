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

#include "./centipede/feature.h"

#include "gtest/gtest.h"

namespace fuzztest::internal {
namespace {

TEST(Feature, FeatureArray) {
  FeatureArray<3> array;
  EXPECT_EQ(array.size(), 0);
  array.push_back(10);
  EXPECT_EQ(array.size(), 1);
  array.push_back(20);
  EXPECT_EQ(array.size(), 2);
  array.clear();
  EXPECT_EQ(array.size(), 0);
  array.push_back(10);
  array.push_back(20);
  array.push_back(30);
  EXPECT_EQ(array.size(), 3);
  array.push_back(40);  // no space left.
  EXPECT_EQ(array.size(), 3);
  EXPECT_EQ(array.data()[0], 10);
  EXPECT_EQ(array.data()[1], 20);
  EXPECT_EQ(array.data()[2], 30);
}

}  // namespace
}  // namespace fuzztest::internal
