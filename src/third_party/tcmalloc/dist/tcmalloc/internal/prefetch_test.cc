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

#include "tcmalloc/internal/prefetch.h"

#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

int number = 42;

TEST(Prefetch, TemporalLocalityNone) {
  PrefetchNta(&number);
  EXPECT_EQ(number, 42);
}

TEST(Prefetch, TemporalLocalityLow) {
  PrefetchT2(&number);
  EXPECT_EQ(number, 42);
}

TEST(Prefetch, TemporalLocalityMedium) {
  PrefetchT1(&number);
  EXPECT_EQ(number, 42);
}

TEST(Prefetch, TemporalLocalityHigh) {
  PrefetchT0(&number);
  EXPECT_EQ(number, 42);
}

TEST(Prefetch, PrefetchForWrite) {
  PrefetchW(&number);
  EXPECT_EQ(number, 42);
}

TEST(Prefetch, WriteTemporalLocalityNone) {
  PrefetchWNta(&number);
  EXPECT_EQ(number, 42);
}

TEST(Prefetch, WriteTemporalLocalityLow) {
  PrefetchWT2(&number);
  EXPECT_EQ(number, 42);
}

TEST(Prefetch, WriteTemporalLocalityMedium) {
  PrefetchWT1(&number);
  EXPECT_EQ(number, 42);
}

TEST(Prefetch, WriteTemporalLocalityHigh) {
  PrefetchWT0(&number);
  EXPECT_EQ(number, 42);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
