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

#include "./centipede/knobs.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"

namespace fuzztest::internal {

static const KnobId knob0 = Knobs::NewId("kn0");
static const KnobId knob1 = Knobs::NewId("kn1");
static const KnobId knob2 = Knobs::NewId("kn2");
static const KnobId knob3 = Knobs::NewId("kn3");

TEST(Knobs, Name) {
  EXPECT_EQ(Knobs::Name(knob0), "kn0");
  EXPECT_EQ(Knobs::Name(knob1), "kn1");
  EXPECT_EQ(Knobs::Name(knob2), "kn2");
  EXPECT_EQ(Knobs::Name(knob3), "kn3");
}

// Depends on FRIEND_TEST in KnobId - don't rename.
TEST(Knobs, Choose) {
  Knobs knobs;

  EXPECT_EQ(knob0.id(), 0);
  EXPECT_EQ(knob1.id(), 1);
  EXPECT_EQ(knobs.Choose({knob3, knob2, knob1}, 0), knob3);
  EXPECT_EQ(knobs.Choose({knob3, knob2, knob1}, 1), knob2);
  EXPECT_EQ(knobs.Choose({knob3, knob2, knob1}, 2), knob1);

  constexpr size_t kNumIter = 1000000;
  knobs.Set(16);
  absl::flat_hash_map<size_t, size_t> id_to_freq;
  for (size_t iter = 0; iter < kNumIter; ++iter) {
    ++id_to_freq[knobs.Choose({knob3, knob2, knob1}, iter).id()];
  }
  EXPECT_EQ(id_to_freq[knob0.id()], 0);
  EXPECT_GE(id_to_freq[knob1.id()], kNumIter / 4);
  EXPECT_GE(id_to_freq[knob2.id()], kNumIter / 4);
  EXPECT_GE(id_to_freq[knob3.id()], kNumIter / 4);

  knobs.Set({100, 0, 10, 1});
  id_to_freq.clear();
  for (size_t iter = 0; iter < kNumIter; ++iter) {
    ++id_to_freq[knobs.Choose({knob0, knob1, knob2, knob3}, iter).id()];
  }
  EXPECT_EQ(id_to_freq[knob1.id()], 0);
  EXPECT_GT(id_to_freq[knob0.id()], 9 * id_to_freq[knob2.id()]);
  EXPECT_GT(id_to_freq[knob2.id()], 9 * id_to_freq[knob3.id()]);
  EXPECT_GT(id_to_freq[knob3.id()], kNumIter / 200);

  absl::flat_hash_map<std::string, size_t> str_to_freq;
  for (size_t iter = 0; iter < kNumIter; ++iter) {
    ++str_to_freq[knobs.Choose<std::string>({knob0, knob2}, {"AAA", "BBB"},
                                            iter)];
  }
  EXPECT_GT(str_to_freq["AAA"], 9 * str_to_freq["BBB"]);
  EXPECT_GT(str_to_freq["BBB"], kNumIter / 200);
}

TEST(Knobs, GenerateBool) {
  Knobs knobs;
  constexpr size_t kNumIter = 255;
  // Checks the GenerateBool on kNumIter different (fake) random values,
  // verifies the expected number of "true" results.
  auto check = [&](Knobs::value_type knob_value,
                   size_t expected_num_true_results) {
    knobs.Set(knob_value);
    size_t num_true = 0;
    for (size_t fake_random = 0; fake_random < kNumIter; ++fake_random) {
      if (knobs.GenerateBool(knob0, fake_random)) ++num_true;
    }
    EXPECT_EQ(num_true, expected_num_true_results);
  };

  check(0, kNumIter / 2 + 1);  // true half the time
  check(-128, 0);              // Never true
  check(127, kNumIter);        // Always true.
  for (int8_t i = -127; i < 127; i++) {
    // The greater the knob value, the more frequently we see true.
    check(i, 128 + i);
  }
}

TEST(KnobsDeathTest, NewId) {
  auto allocate_too_many_knob_ids = []() {
    for (size_t i = 0; i < Knobs::kNumKnobs; ++i) {
      Knobs::NewId(absl::StrCat("kn", i));
    }
  };
  EXPECT_DEATH(allocate_too_many_knob_ids(),
               "Knobs::NewId: no more IDs left, aborting");
}

TEST(KnobsDeathTest, Choose) {
  Knobs knobs;
  EXPECT_DEATH(knobs.Choose<int>({}, {}, 0), "");
  EXPECT_DEATH(knobs.Choose<int>({knob1, knob2}, {1}, 0), "");
}

}  // namespace fuzztest::internal
