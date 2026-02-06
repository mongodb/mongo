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

#include "./centipede/byte_array_mutator.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "./centipede/execution_metadata.h"
#include "./centipede/knobs.h"
#include "./centipede/mutation_input.h"
#include "./centipede/runner_cmp_trace.h"
#include "./common/defs.h"

namespace fuzztest::internal {

// Tests that when alignment is not 1 byte, adding bytes to an input will result
// in a size-aligned mutant (even if the input is not size-aligned).
//
// Note: This test cannot be in an anonymous namespace due to the FRIEND_TEST in
// ByteArrayMutator.
TEST(ByteArrayMutator, RoundUpToAddCorrectly) {
  Knobs knobs;
  ByteArrayMutator mutator(knobs, /*seed=*/1);
  EXPECT_TRUE(mutator.set_size_alignment(4));

  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/0, /*to_add=*/0), 0);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/4, /*to_add=*/0), 0);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/4, /*to_add=*/3), 4);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/5, /*to_add=*/0), 3);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/5, /*to_add=*/2), 3);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/5, /*to_add=*/18), 19);

  // Check that max length is also respected.
  EXPECT_TRUE(mutator.set_max_len(12));

  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/5, /*to_add=*/0), 3);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/5, /*to_add=*/2), 3);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/5, /*to_add=*/18), 7);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/11, /*to_add=*/5), 1);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/12, /*to_add=*/5), 0);
  EXPECT_EQ(mutator.RoundUpToAdd(/*curr_size=*/13, /*to_add=*/5), 0);
}

// Tests that when alignment is not 1 byte, removing bytes from an input will
// result in a size-aligned mutant (even if the input is not size-aligned).
//
// Note: This test cannot be in an anonymous namespace due to the FRIEND_TEST in
// ByteArrayMutator.
TEST(ByteArrayMutator, RoundDownToRemoveCorrectly) {
  Knobs knobs;
  ByteArrayMutator mutator(knobs, /*seed=*/1);
  EXPECT_TRUE(mutator.set_size_alignment(4));

  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/0, /*to_remove=*/0), 0);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/0, /*to_remove=*/1), 0);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/1, /*to_remove=*/0), 0);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/1, /*to_remove=*/1), 0);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/4, /*to_remove=*/0), 0);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/4, /*to_remove=*/3), 0);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/5, /*to_remove=*/0), 1);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/5, /*to_remove=*/2), 1);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/7, /*to_remove=*/2), 3);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/23, /*to_remove=*/4), 7);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/23, /*to_remove=*/20), 19);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/23, /*to_remove=*/24), 19);

  // Check that max length is also respected.
  EXPECT_TRUE(mutator.set_max_len(12));

  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/7, /*to_remove=*/2), 3);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/23, /*to_remove=*/4), 11);
  EXPECT_EQ(mutator.RoundDownToRemove(/*curr_size=*/23, /*to_remove=*/20), 19);
}

namespace {

TEST(DictEntry, DictEntry) {
  uint8_t bytes[17] = {0, 1,  2,  3,  4,  5,  6,  7, 8,
                       9, 10, 11, 12, 13, 14, 15, 16};
  DictEntry a_0_10({bytes + 0, 10});
  DictEntry a_0_4({bytes + 0, 4});
  DictEntry a_1_8({bytes + 1, 8});

  EXPECT_LT(a_0_4, a_0_10);
  EXPECT_LT(a_0_10, a_1_8);
  EXPECT_EQ(memcmp(a_0_10.begin(), bytes, a_0_10.end() - a_0_10.begin()), 0);

  EXPECT_DEATH({ DictEntry a_0_10({bytes, 17}); }, "");
}

TEST(CmpDictionary, CmpDictionary) {
  CmpDictionary dict;
  ExecutionMetadata metadata{/*cmp_data=*/{
      2,               // size
      1,  2,           // a
      3,  4,           // b
      3,               // size
      5,  6,  7,       // a
      8,  9,  10,      // b
      4,               // size
      11, 12, 13, 14,  // a
      15, 16, 17, 18,  // b
      3,               // size
      20, 21, 22,      // a
      15, 16, 17,      // b
      3,               // size
      15, 16, 20,      // a
      30, 40, 50,      // b
  }};
  EXPECT_TRUE(dict.SetFromMetadata(metadata));

  using S = ByteSpan;

  std::vector<ByteSpan> suggestions;
  suggestions.reserve(5);

  dict.SuggestReplacement({42, 43}, suggestions);
  EXPECT_TRUE(suggestions.empty());

  dict.SuggestReplacement({1, 2, 3}, suggestions);
  EXPECT_THAT(suggestions, testing::ElementsAre(S({3, 4})));

  dict.SuggestReplacement({5, 6, 7, 8}, suggestions);
  EXPECT_THAT(suggestions, testing::ElementsAre(S({8, 9, 10})));

  dict.SuggestReplacement({15, 16, 17, 18, 0, 0}, suggestions);
  EXPECT_THAT(suggestions, testing::UnorderedElementsAre(S({11, 12, 13, 14}),
                                                         S({20, 21, 22})));

  dict.SuggestReplacement({15, 16, 20}, suggestions);
  EXPECT_THAT(suggestions, testing::UnorderedElementsAre(S({30, 40, 50})));

  // check that we don't exceed capacity.
  std::vector<ByteSpan> capacity1;
  capacity1.reserve(1);
  EXPECT_EQ(capacity1.capacity(), 1);
  dict.SuggestReplacement({15, 16, 17, 18, 0, 0}, capacity1);
  EXPECT_EQ(capacity1.size(), 1);
  EXPECT_EQ(capacity1.capacity(), 1);
}

TEST(CmpDictionary, CmpDictionaryIsCompatibleWithCmpTrace) {
  CmpTrace<0, 13> traceN;
  traceN.Clear();
  constexpr uint8_t long_array[20] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                                      10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  traceN.Capture(20, long_array, long_array);  // will be trimmed to 16.

  ExecutionMetadata metadata;
  bool append_failed = false;
  int count = 0;
  traceN.ForEachNonZero(
      [&](uint8_t size, const uint8_t *v0, const uint8_t *v1) {
        if (!metadata.AppendCmpEntry({v0, size}, {v1, size}))
          append_failed = true;
        count++;
      });
  EXPECT_FALSE(append_failed);
  EXPECT_EQ(1, count);

  CmpDictionary dictionary;
  EXPECT_TRUE(dictionary.SetFromMetadata(metadata));
  EXPECT_EQ(2, dictionary.size());
}

// Tests that two mutators seeded with different rng seeds produce different
// results.
TEST(ByteArrayMutator, Randomness) {
  Knobs knobs;
  ByteArrayMutator mutator[2]{{knobs, 1}, {knobs, 2}};

  std::vector<ByteArray> res[2];
  for (size_t i = 0; i < 2; i++) {
    ByteArray seed = {0};
    // Just run a few iterations.
    for (size_t iter = 0; iter < 100; iter++) {
      mutator[i].Mutate(seed);
      res[i].push_back(seed);
    }
  }
  EXPECT_NE(res[0], res[1]);
}

// Tests that max length is always a multiple of size alignment.
TEST(ByteArrayMutator, CheckSizeAlignmentWithMaxLength) {
  Knobs knobs;
  ByteArrayMutator mutator(knobs, /*seed=*/1);

  EXPECT_TRUE(mutator.set_size_alignment(1000));
  EXPECT_TRUE(mutator.set_size_alignment(4));
  EXPECT_TRUE(mutator.set_max_len(4));
  EXPECT_TRUE(mutator.set_max_len(16));
  EXPECT_FALSE(mutator.set_max_len(2));
  EXPECT_FALSE(mutator.set_max_len(10));

  EXPECT_TRUE(mutator.set_size_alignment(8));
  EXPECT_FALSE(mutator.set_size_alignment(12));
  EXPECT_FALSE(mutator.set_size_alignment(15));
}

// Tests a callback `fn`: mutations of `seed` are expected to eventually
// match all of `expected_mutants`, but never any of `unexpected_mutants`.
// Mutators that do a single-step can be tested for `unexpected_mutants`,
// while for more complicated mutators `unexpected_mutants` should be empty.
void TestMutatorFn(ByteArrayMutator::Fn fn, const ByteArray &seed,
                   const std::vector<ByteArray> &expected_mutants,
                   const std::vector<ByteArray> &unexpected_mutants,
                   size_t size_alignment = 1,
                   size_t max_len = std::numeric_limits<size_t>::max(),
                   const std::vector<ByteArray> &dictionary = {},
                   ByteSpan cmp_data = {}, size_t num_iterations = 100000000) {
  Knobs knobs;
  ByteArrayMutator mutator(knobs, 1);
  EXPECT_TRUE(mutator.set_size_alignment(size_alignment));
  EXPECT_TRUE(mutator.set_max_len(max_len));
  mutator.AddToDictionary(dictionary);
  mutator.SetMetadata({/*cmp_data=*/{cmp_data.begin(), cmp_data.end()}});
  absl::flat_hash_set<ByteArray> expected(expected_mutants.begin(),
                                          expected_mutants.end());
  absl::flat_hash_set<ByteArray> unexpected(unexpected_mutants.begin(),
                                            unexpected_mutants.end());
  ByteArray mutant;  // create outside the loop to avoid malloc in the loop.
  for (size_t i = 0; i < num_iterations; i++) {
    mutant = seed;
    (mutator.*fn)(mutant);
    expected.erase(mutant);
    if (expected.empty()) break;
    EXPECT_FALSE(unexpected.contains(mutant));
  }
  EXPECT_TRUE(expected.empty());
}

TEST(ByteArrayMutator, ChangeByte) {
  TestMutatorFn(&ByteArrayMutator::ChangeByte, {1, 2, 3},
                /*expected_mutants=*/
                {
                    {1, 2, 4},
                    {42, 2, 3},
                    {1, 66, 3},
                },
                /*unexpected_mutants=*/
                {
                    {9, 9, 3},
                    {1, 8, 8},
                    {7, 2, 7},
                });
}

TEST(ByteArrayMutator, FlipBit) {
  TestMutatorFn(&ByteArrayMutator::FlipBit, {0, 7, 10},
                /*expected_mutants=*/
                {
                    {1, 7, 10},
                    {0, 6, 10},
                    {0, 7, 11},
                },
                /*unexpected_mutants=*/
                {
                    {1, 6, 10},
                    {0, 6, 11},
                });
}

TEST(ByteArrayMutator, SwapBytes) {
  TestMutatorFn(&ByteArrayMutator::SwapBytes, {0, 1, 2},
                /*expected_mutants=*/
                {
                    {0, 2, 1},
                    {1, 0, 2},
                    {2, 1, 0},
                },
                /*unexpected_mutants=*/
                {
                    {2, 0, 1},
                });
}

TEST(ByteArrayMutator, InsertBytes) {
  TestMutatorFn(&ByteArrayMutator::InsertBytes, {0, 1, 2},
                /*expected_mutants=*/
                {
                    {0, 1, 2, 3},
                    {0, 3, 1, 2},
                    {3, 0, 1, 2},
                    {0, 1, 2, 3, 4},
                    {0, 3, 4, 1, 2},
                    {3, 4, 0, 1, 2},
                },
                /*unexpected_mutants=*/
                {
                    {0, 1},
                    {0, 1, 2},
                    {0, 3, 1, 4, 2},
                });
}

TEST(ByteArrayMutator, InsertBytesWithAlignment) {
  TestMutatorFn(&ByteArrayMutator::InsertBytes, {0, 1, 2},
                /*expected_mutants=*/
                {
                    {0, 1, 2, 3},
                    {0, 3, 1, 2},
                    {3, 0, 1, 2},
                },
                /*unexpected_mutants=*/
                {
                    {0, 1},
                    {0, 1, 2},
                    {0, 1, 2, 3, 4},
                    {0, 3, 1, 4, 2},
                    {0, 3, 4, 1, 2},
                    {3, 4, 0, 1, 2},
                },
                /*size_alignment=*/4);
}

TEST(ByteArrayMutator, InsertBytesWithMaxLen) {
  TestMutatorFn(&ByteArrayMutator::InsertBytes, {0, 1, 2},
                /*expected_mutants=*/
                {
                    {0, 1, 2, 3},
                    {0, 3, 1, 2},
                    {3, 0, 1, 2},
                },
                /*unexpected_mutants=*/
                {
                    {0, 1, 2, 3, 4},
                    {0, 3, 4, 1, 2},
                    {3, 4, 0, 1, 2},
                },
                /*size_alignment=*/1,
                /*max_len=*/4);
}

// Currently, same as for InsertBytes. Will change in future as we add more
// mutators.
TEST(ByteArrayMutator, MutateIncreaseSize) {
  TestMutatorFn(&ByteArrayMutator::MutateIncreaseSize, {0, 1, 2},
                /*expected_mutants=*/
                {
                    {0, 1, 2, 3},
                    {0, 3, 1, 2},
                    {3, 0, 1, 2},
                    {0, 1, 2, 3, 4},
                    {0, 3, 4, 1, 2},
                    {3, 4, 0, 1, 2},
                },
                /*unexpected_mutants=*/
                {
                    {0, 1},
                    {0, 3, 1, 4, 2},
                });
}

TEST(ByteArrayMutator, MutateIncreaseSizeWithAlignment) {
  TestMutatorFn(&ByteArrayMutator::MutateIncreaseSize, {0, 1, 2},
                /*expected_mutants=*/
                {
                    {0, 1, 2, 3},
                    {0, 3, 1, 2},
                    {3, 0, 1, 2},
                },
                /*unexpected_mutants=*/
                {
                    {0, 1},
                    {0, 1, 2, 3, 4},
                    {0, 3, 1, 4, 2},
                    {0, 3, 4, 1, 2},
                    {3, 4, 0, 1, 2},
                },
                /*size_alignment=*/4);
}

TEST(ByteArrayMutator, EraseBytes) {
  TestMutatorFn(&ByteArrayMutator::EraseBytes, {0, 1, 2, 3},
                /*expected_mutants=*/
                {
                    {0, 1, 2},
                    {0, 1, 3},
                    {0, 2, 3},
                    {1, 2, 3},
                    {0, 1},
                    {0, 3},
                    {2, 3},
                },
                /*unexpected_mutants=*/
                {
                    {0},
                    {1},
                    {2},
                });
}

TEST(ByteArrayMutator, EraseBytesWithAlignment) {
  TestMutatorFn(&ByteArrayMutator::EraseBytes, {0, 1, 2, 3},
                /*expected_mutants=*/
                {
                    {0, 1, 2, 3},
                },
                /*unexpected_mutants=*/
                {
                    {0, 1, 2},
                    {0, 1, 3},
                    {0, 2, 3},
                    {1, 2, 3},
                    {0, 1},
                    {0, 3},
                    {2, 3},
                    {0},
                    {1},
                    {2},
                },
                /*size_alignment=*/4);
  TestMutatorFn(&ByteArrayMutator::EraseBytes, {0, 1, 2, 3, 4},
                /*expected_mutants=*/
                {
                    {0, 1, 2, 3},
                    {1, 2, 3, 4},
                    {0, 1, 3, 4},
                },
                /*unexpected_mutants=*/
                {
                    {0, 1, 2, 3, 4},
                    {0, 1, 2},
                    {0, 1, 3},
                    {0, 2, 3},
                    {1, 2, 3},
                    {0, 1},
                    {0},
                },
                /*size_alignment=*/4);
}

// Currently, same as EraseBytes. Will change in future as we add more mutators.
TEST(ByteArrayMutator, MutateDecreaseSize) {
  TestMutatorFn(&ByteArrayMutator::MutateDecreaseSize, {0, 1, 2, 3},
                /*expected_mutants=*/
                {
                    {0, 1, 2},
                    {0, 1, 3},
                    {0, 2, 3},
                    {1, 2, 3},
                    {0, 1},
                    {0, 3},
                    {2, 3},
                },
                /*unexpected_mutants=*/
                {
                    {0},
                    {1},
                    {2},
                });
}

TEST(ByteArrayMutator, MutateDecreaseSizeWithAlignment) {
  TestMutatorFn(&ByteArrayMutator::MutateDecreaseSize, {0, 1, 2, 3},
                /*expected_mutants=*/
                {
                    {0, 1, 2, 3},
                },
                /*unexpected_mutants=*/
                {
                    {0, 1, 2},
                    {0, 1, 3},
                    {0, 2, 3},
                    {1, 2, 3},
                    {0, 1},
                    {0, 3},
                    {2, 3},
                    {0},
                    {1},
                    {2},
                },
                /*size_alignment=*/4);
  TestMutatorFn(&ByteArrayMutator::MutateDecreaseSize, {0, 1, 2, 3, 4},
                /*expected_mutants=*/
                {
                    {0, 1, 2, 3},
                    {1, 2, 3, 4},
                    {0, 1, 3, 4},
                },
                /*unexpected_mutants=*/
                {
                    {0, 1, 2, 3, 4},
                    {0, 1, 2},
                    {0, 1, 3},
                    {0, 2, 3},
                    {1, 2, 3},
                    {0, 1},
                    {0},
                },
                /*size_alignment=*/4);
}

TEST(ByteArrayMutator, MutateDecreaseSizeWithAlignmentAndMaxLen) {
  TestMutatorFn(&ByteArrayMutator::MutateDecreaseSize, {0, 1, 2, 3},
                /*expected_mutants=*/
                {
                    {0, 1},
                    {2, 3},
                },
                /*unexpected_mutants=*/
                {
                    {0},
                    {1},
                    {2},
                    {1, 2},
                    {0, 1, 2},
                },
                /*size_alignment=*/2,
                /*max_len=*/2);
}

// Tests that MutateSameSize will eventually produce all possible mutants of
// size 1 and 2. Also tests some of the 3-byte mutants.
TEST(ByteArrayMutator, MutateSameSize) {
  Knobs knobs;
  ByteArrayMutator mutator(knobs, 1);
  for (size_t size = 1; size <= 2; size++) {
    ByteArray data(size);
    absl::flat_hash_set<ByteArray> set;
    size_t expected_set_size = 1 << (8 * size);
    for (size_t iter = 0; iter < 2000000ULL; iter++) {
      mutator.MutateSameSize(data);
      EXPECT_EQ(data.size(), size);
      set.insert(data);
      if (set.size() == expected_set_size) break;
    }
    EXPECT_EQ(expected_set_size, set.size());
  }

  // One step of MutateSameSize may generate any mutant that can be generated by
  // one step of its submutants. No mutant of other length may appear.
  const std::vector<ByteArray> kUnexpectedMutants = {
      {1, 2},
      {1, 2, 3, 4},
  };
  TestMutatorFn(&ByteArrayMutator::MutateSameSize, {1, 2, 3},
                /*expected_mutants=*/
                {
                    {1, 2, 4},
                    {42, 2, 3},
                    {1, 66, 3},
                },
                kUnexpectedMutants);
  TestMutatorFn(&ByteArrayMutator::MutateSameSize, {0, 7, 10},
                /*expected_mutants=*/
                {
                    {1, 7, 10},
                    {0, 6, 10},
                    {0, 7, 11},
                },
                kUnexpectedMutants);
  TestMutatorFn(&ByteArrayMutator::MutateSameSize, {0, 1, 2},
                /*expected_mutants=*/
                {
                    {0, 2, 1},
                    {1, 0, 2},
                    {2, 1, 0},
                },
                kUnexpectedMutants);
}

TEST(ByteArrayMutator, Mutate) {
  TestMutatorFn(&ByteArrayMutator::Mutate, {1, 2, 3},
                /*expected_mutants=*/
                {
                    {1, 2, 4},
                    {1, 2},
                    {1, 2, 3, 4},
                },
                /*unexpected_mutants=*/
                {
                    {},
                });
}

TEST(ByteArrayMutator, OverwriteFromDictionary) {
  TestMutatorFn(&ByteArrayMutator::OverwriteFromDictionary, {1, 2, 3, 4, 5},
                /*expected_mutants=*/
                {
                    {1, 2, 7, 8, 9},
                    {1, 7, 8, 9, 5},
                    {7, 8, 9, 4, 5},
                    {1, 2, 3, 0, 6},
                    {1, 2, 0, 6, 5},
                    {1, 0, 6, 4, 5},
                    {0, 6, 3, 4, 5},
                    {42, 2, 3, 4, 5},
                    {1, 42, 3, 4, 5},
                    {1, 2, 42, 4, 5},
                    {1, 2, 3, 42, 5},
                    {1, 2, 3, 4, 42},
                },
                /*unexpected_mutants=*/
                {
                    {1, 2, 3, 7, 8},
                    {8, 9, 3, 4, 5},
                    {6, 2, 3, 4, 5},
                    {1, 2, 3, 4, 0},
                    {42, 42, 3, 4, 5},
                },
                /*size_alignment=*/1,
                /*max_len=*/std::numeric_limits<size_t>::max(),
                /*dictionary=*/
                {
                    {7, 8, 9},
                    {0, 6},
                    {42},
                });
}

TEST(ByteArrayMutator, OverwriteFromCmpDictionary) {
  TestMutatorFn(&ByteArrayMutator::OverwriteFromCmpDictionary,
                {1, 2, 40, 50, 60},
                /*expected_mutants=*/
                {
                    {3, 4, 40, 50, 60},
                    {1, 2, 10, 20, 30},
                },
                /*unexpected_mutants=*/
                {
                    {3, 4, 10, 20, 30},
                },
                /*size_alignment=*/1,
                /*max_len=*/std::numeric_limits<size_t>::max(),
                /*dictionary=*/
                {},
                /*cmp_data=*/
                {/*args1*/ 2, 1, 2, 3, 4, /*args2*/ 3, 10, 20, 30, 40, 50, 60});
}

TEST(ByteArrayMutator, OverwriteFromCmpDictionaryAndSkipLongEntry) {
  TestMutatorFn(
      &ByteArrayMutator::OverwriteFromCmpDictionary,
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19},
      /*expected_mutants=*/
      {{100, 101, 102, 103, 4,  5,  6,  7,  8,  9,
        10,  11,  12,  13,  14, 15, 16, 17, 18, 19}},
      /*unexpected_mutants=*/
      {{100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
        110, 111, 112, 113, 114, 115, 116, 117, 118, 119}},
      /*size_alignment=*/1,
      /*max_len=*/std::numeric_limits<size_t>::max(),
      /*dictionary=*/
      {},
      /*cmp_data=*/
      {/*size*/ 20, /*lhs*/ 0, 1,   2,   3,   4,           5,
       6,           7,         8,   9,   10,  11,          12,
       13,          14,        15,  16,  17,  18,          19,
       /*rhs*/ 100, 101,       102, 103, 104, 105,         106,
       107,         108,       109, 110, 111, 112,         113,
       114,         115,       116, 117, 118, 119,
       /*size*/ 4,  /*lhs*/ 0, 1,   2,   3,   /*rhs*/ 100, 101,
       102,         103});
}

TEST(ByteArrayMutator, InsertFromDictionary) {
  TestMutatorFn(&ByteArrayMutator::InsertFromDictionary, {1, 2, 3},
                /*expected_mutants=*/
                {
                    {1, 2, 3, 4, 5},
                    {1, 2, 4, 5, 3},
                    {1, 4, 5, 2, 3},
                    {4, 5, 1, 2, 3},
                    {1, 2, 3, 6, 7, 8},
                    {1, 2, 6, 7, 8, 3},
                    {1, 6, 7, 8, 2, 3},
                    {6, 7, 8, 1, 2, 3},
                },
                /*unexpected_mutants=*/
                {
                    {1, 2, 3, 7, 8},
                    {7, 8, 1, 2, 3},
                },
                /*size_alignment=*/1,
                /*max_len=*/std::numeric_limits<size_t>::max(),
                /*dictionary=*/
                {
                    {4, 5},
                    {6, 7, 8},
                });
}

// Tests CrossOver* mutations.
// With CrossOver, no random values are involved, only random offsets,
// and so we can test for all possible expected mutants.
void TestCrossOver(void (ByteArrayMutator::*fn)(ByteArray &, const ByteArray &),
                   const ByteArray &seed, const ByteArray &other,
                   const std::vector<ByteArray> &all_possible_mutants,
                   size_t size_alignment = 1) {
  Knobs knobs;
  ByteArrayMutator mutator(knobs, 1);
  EXPECT_TRUE(mutator.set_size_alignment(size_alignment));
  absl::flat_hash_set<ByteArray> expected(all_possible_mutants.begin(),
                                          all_possible_mutants.end());
  absl::flat_hash_set<ByteArray> found;
  const int kNumIter = 10000;
  // Run for some number of iterations, make sure we saw all expected mutations
  // and nothing else.
  for (int i = 0; i < kNumIter; i++) {
    ByteArray mutant = seed;
    (mutator.*fn)(mutant, other);
    EXPECT_EQ(expected.count(mutant), 1);
    found.insert(mutant);
  }
  EXPECT_EQ(expected, found);
}

TEST(ByteArrayMutator, CrossOverInsert) {
  TestCrossOver(&ByteArrayMutator::CrossOverInsert, {1}, {2},
                /*all_possible_mutants=*/
                {
                    {1, 2},
                    {2, 1},
                });
  TestCrossOver(&ByteArrayMutator::CrossOverInsert, {1, 2}, {3},
                /*all_possible_mutants=*/
                {
                    {1, 2, 3},
                    {1, 3, 2},
                    {3, 1, 2},
                });
  TestCrossOver(&ByteArrayMutator::CrossOverInsert, {1}, {2, 3},
                /*all_possible_mutants=*/
                {
                    {1, 2, 3},
                    {2, 3, 1},
                    {2, 1},
                    {1, 2},
                    {3, 1},
                    {1, 3},
                });
  TestCrossOver(&ByteArrayMutator::CrossOverInsert, {1, 2}, {3, 4},
                /*all_possible_mutants=*/
                {
                    {1, 2, 3, 4},
                    {1, 3, 4, 2},
                    {3, 4, 1, 2},
                    {1, 2, 3},
                    {1, 3, 2},
                    {3, 1, 2},
                    {1, 2, 4},
                    {1, 4, 2},
                    {4, 1, 2},
                });
}

TEST(ByteArrayMutator, CrossOverInsertWithAlignment) {
  TestCrossOver(&ByteArrayMutator::CrossOverInsert, {1}, {2},
                /*all_possible_mutants=*/
                {
                    {1},
                },
                /*size_alignment=*/4);
  TestCrossOver(&ByteArrayMutator::CrossOverInsert, {1, 2}, {3, 4},
                /*all_possible_mutants=*/
                {
                    {1, 2, 3, 4},
                    {1, 3, 4, 2},
                    {3, 4, 1, 2},
                },
                /*size_alignment=*/4);
  TestCrossOver(&ByteArrayMutator::CrossOverInsert, {1, 2}, {3, 4, 5},
                /*all_possible_mutants=*/
                {
                    {1, 2, 3, 4},
                    {1, 3, 4, 2},
                    {3, 4, 1, 2},
                    {1, 2, 4, 5},
                    {1, 4, 5, 2},
                    {4, 5, 1, 2},
                },
                /*size_alignment=*/4);
  TestCrossOver(&ByteArrayMutator::CrossOverInsert, {1, 2, 3, 4, 5}, {6},
                /*all_possible_mutants=*/
                {
                    {1, 2, 3, 4, 5},
                },
                /*size_alignment=*/4);
  TestCrossOver(&ByteArrayMutator::CrossOverInsert, {1, 2, 3}, {4, 5, 6, 7},
                /*all_possible_mutants=*/
                {
                    {4, 1, 2, 3},
                    {5, 1, 2, 3},
                    {6, 1, 2, 3},
                    {7, 1, 2, 3},
                    {1, 4, 2, 3},
                    {1, 5, 2, 3},
                    {1, 6, 2, 3},
                    {1, 7, 2, 3},
                    {1, 2, 4, 3},
                    {1, 2, 5, 3},
                    {1, 2, 6, 3},
                    {1, 2, 7, 3},
                    {1, 2, 3, 4},
                    {1, 2, 3, 5},
                    {1, 2, 3, 6},
                    {1, 2, 3, 7},
                },
                /*size_alignment=*/4);
}

TEST(ByteArrayMutator, CrossOverOverwrite) {
  TestCrossOver(&ByteArrayMutator::CrossOverOverwrite, {1}, {2},
                /*all_possible_mutants=*/
                {
                    {2},
                });
  TestCrossOver(&ByteArrayMutator::CrossOverOverwrite, {1, 2}, {3},
                /*all_possible_mutants=*/
                {
                    {1, 3},
                    {3, 2},
                });
  TestCrossOver(&ByteArrayMutator::CrossOverOverwrite, {1}, {2, 3},
                /*all_possible_mutants=*/
                {
                    {2},
                    {3},
                });
  TestCrossOver(&ByteArrayMutator::CrossOverOverwrite, {1, 2}, {3, 4},
                /*all_possible_mutants=*/
                {
                    {1, 3},
                    {3, 2},
                    {1, 4},
                    {4, 2},
                });
  TestCrossOver(&ByteArrayMutator::CrossOverOverwrite, {1, 2, 3, 4, 5, 6},
                {7, 8},
                /*all_possible_mutants=*/
                {
                    // overwrite with {7}
                    {7, 2, 3, 4, 5, 6},
                    {1, 7, 3, 4, 5, 6},
                    {1, 2, 7, 4, 5, 6},
                    {1, 2, 3, 7, 5, 6},
                    {1, 2, 3, 4, 7, 6},
                    {1, 2, 3, 4, 5, 7},
                    // overwrite with {8}
                    {8, 2, 3, 4, 5, 6},
                    {1, 8, 3, 4, 5, 6},
                    {1, 2, 8, 4, 5, 6},
                    {1, 2, 3, 8, 5, 6},
                    {1, 2, 3, 4, 8, 6},
                    {1, 2, 3, 4, 5, 8},
                    // overwrite with {7, 8}
                    {7, 8, 3, 4, 5, 6},
                    {1, 7, 8, 4, 5, 6},
                    {1, 2, 7, 8, 5, 6},
                    {1, 2, 3, 7, 8, 6},
                    {1, 2, 3, 4, 7, 8},
                });
}

TEST(ByteArrayMutator, CrossOver) {
  // Most of CrossOver is tested above in CrossOverOverwrite/CrossOverInsert.
  // Here just test one set of inputs to ensure CrossOver calls the other two
  // functions correctly.
  TestCrossOver(&ByteArrayMutator::CrossOver, {1, 2}, {3, 4},
                /*all_possible_mutants=*/
                {
                    // CrossOverInsert
                    {1, 2, 3, 4},
                    {1, 3, 4, 2},
                    {3, 4, 1, 2},
                    {1, 2, 3},
                    {1, 3, 2},
                    {3, 1, 2},
                    {1, 2, 4},
                    {1, 4, 2},
                    {4, 1, 2},
                    // CrossOverOverwrite
                    {1, 3},
                    {3, 2},
                    {1, 4},
                    {4, 2},
                });
}

TEST(ByteArrayMutator, FailedMutations) {
  const int kNumIter = 1000000;
  ByteArray data = {1, 2, 3, 4, 5};
  Knobs knobs;
  ByteArrayMutator mutator(knobs, 1);
  size_t num_failed_erase = 0;
  size_t num_failed_generic = 0;
  for (int i = 0; i < kNumIter; i++) {
    num_failed_erase += !mutator.EraseBytes(data);
    num_failed_generic += !mutator.Mutate(data);
  }
  // EraseBytes() will fail sometimes, but should not fail too often.
  EXPECT_GT(num_failed_erase, 0);
  EXPECT_LT(num_failed_erase, kNumIter / 2);
  // The generic Mutate() should fail very infrequently.
  EXPECT_LT(num_failed_generic, kNumIter / 1000);
}

TEST(ByteArrayMutator, MutateManyWithAlignedInputs) {
  constexpr size_t kSizeAlignment = 4;
  Knobs knobs;
  ByteArrayMutator mutator(knobs, /*seed=*/1);
  EXPECT_TRUE(mutator.set_size_alignment(kSizeAlignment));
  constexpr size_t kNumMutantsToGenerate = 10000;

  // If all inputs are aligned, expect all generated mutants to be aligned.
  const std::vector<ByteArray> aligned_inputs = {
      {0, 1, 2, 3},
      {0, 1, 2, 3, 4, 5, 6, 7},
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
  };
  const std::vector<ByteArray> mutants =
      mutator.MutateMany(GetMutationInputRefsFromDataInputs(aligned_inputs),
                         kNumMutantsToGenerate);
  EXPECT_EQ(mutants.size(), kNumMutantsToGenerate);
  for (const ByteArray &mutant : mutants) {
    EXPECT_EQ(mutant.size() % kSizeAlignment, 0);
  }
}

TEST(ByteArrayMutator, MutateManyWithUnalignedInputs) {
  constexpr size_t kSizeAlignment = 4;
  Knobs knobs;
  ByteArrayMutator mutator(knobs, /*seed=*/1);
  EXPECT_TRUE(mutator.set_size_alignment(kSizeAlignment));
  constexpr size_t kNumMutantsToGenerate = 10000;

  // If there are unaligned inputs, most mutants should be aligned, but the ones
  // that are unaligned should be the same size as the unaligned inputs (as they
  // resulted from mutators that did not change the size of the inputs).
  const std::vector<ByteArray> unaligned_inputs = {
      {0},
      {0, 1},
      {0, 1, 2},
      {0, 1, 2, 3, 4},
      {0, 1, 2, 3, 4, 5},
      {0, 1, 2, 3, 4, 5, 6},
      {0, 1, 2, 3, 4, 5, 6, 7, 8},
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
  };
  const std::vector<ByteArray> mutants =
      mutator.MutateMany(GetMutationInputRefsFromDataInputs(unaligned_inputs),
                         kNumMutantsToGenerate);
  EXPECT_EQ(mutants.size(), kNumMutantsToGenerate);
  for (const ByteArray &mutant : mutants) {
    if (mutant.size() % kSizeAlignment != 0) {
      EXPECT_LE(mutant.size(), 11);
    }
  }
}

TEST(ByteArrayMutator, MutateManyWithMaxLen) {
  constexpr size_t kMaxLen = 4;
  Knobs knobs;
  ByteArrayMutator mutator(knobs, /*seed=*/1);
  EXPECT_TRUE(mutator.set_max_len(kMaxLen));
  constexpr size_t kNumMutantsToGenerate = 10000;

  const std::vector<ByteArray> inputs = {
      {0},
      {0, 1},
      {0, 1, 2},
      {0, 1, 2, 3},
  };
  const std::vector<ByteArray> mutants = mutator.MutateMany(
      GetMutationInputRefsFromDataInputs(inputs), kNumMutantsToGenerate);
  EXPECT_EQ(mutants.size(), kNumMutantsToGenerate);

  for (const ByteArray &mutant : mutants) {
    EXPECT_LE(mutant.size(), kMaxLen);
  }
}

TEST(ByteArrayMutator, MutateManyWithMaxLenWithStartingLargeInput) {
  constexpr size_t kMaxLen = 4;
  Knobs knobs;
  ByteArrayMutator mutator(knobs, /*seed=*/1);
  EXPECT_TRUE(mutator.set_max_len(kMaxLen));
  constexpr size_t kNumMutantsToGenerate = 10000;

  const std::vector<ByteArray> large_input = {
      {0, 1, 2, 3, 4, 5, 6, 7}, {0}, {0, 1}, {0, 1, 2}, {0, 1, 2, 3},
  };
  const std::vector<ByteArray> mutants = mutator.MutateMany(
      GetMutationInputRefsFromDataInputs(large_input), kNumMutantsToGenerate);
  EXPECT_EQ(mutants.size(), kNumMutantsToGenerate);

  for (const ByteArray &mutant : mutants) {
    if (mutant.size() > kMaxLen) {
      // The only mutant larger than max length should be the same large input
      // that mutation originally started with. All other mutants should be
      // within the maximum length specified.
      EXPECT_EQ(mutant, large_input[0]);
    }
  }
}

}  // namespace

}  // namespace fuzztest::internal
