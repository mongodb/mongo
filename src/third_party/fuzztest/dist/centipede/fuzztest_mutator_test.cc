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

#include "./centipede/fuzztest_mutator.h"

#include <cstddef>
#include <optional>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_join.h"
#include "./centipede/execution_metadata.h"
#include "./centipede/knobs.h"
#include "./centipede/mutation_input.h"
#include "./common/defs.h"

namespace fuzztest::internal {

namespace {

using ::testing::AllOf;
using ::testing::Each;
using ::testing::IsSupersetOf;
using ::testing::Le;
using ::testing::SizeIs;
using ::testing::Values;

TEST(FuzzTestMutator, DifferentRngSeedsLeadToDifferentMutantSequences) {
  const Knobs knobs;
  FuzzTestMutator mutator[2]{FuzzTestMutator(knobs, /*seed=*/1),
                             FuzzTestMutator(knobs, /*seed=*/2)};

  std::vector<ByteArray> res[2];
  for (size_t i = 0; i < 2; i++) {
    ByteArray data = {0};
    std::vector<MutationInputRef> mutation_inputs = {{data}};
    constexpr size_t kMutantSequenceLength = 100;
    for (size_t iter = 0; iter < kMutantSequenceLength; iter++) {
      const std::vector<ByteArray> mutants =
          mutator[i].MutateMany(mutation_inputs, 1);
      ASSERT_EQ(mutants.size(), 1);
      res[i].push_back(mutants[0]);
    }
  }
  EXPECT_NE(res[0], res[1]);
}

TEST(FuzzTestMutator, MutateManyWorksWithInputsLargerThanMaxLen) {
  constexpr size_t kMaxLen = 4;
  const Knobs knobs;
  FuzzTestMutator mutator(knobs, /*seed=*/1);
  EXPECT_TRUE(mutator.set_max_len(kMaxLen));
  constexpr size_t kNumMutantsToGenerate = 10000;
  const std::vector<ByteArray> mutants = mutator.MutateMany(
      {
          {/*data=*/{0, 1, 2, 3, 4, 5, 6, 7}},
          {/*data=*/{0}},
          {/*data=*/{0, 1}},
          {/*data=*/{0, 1, 2}},
          {/*data=*/{0, 1, 2, 3}},
      },
      kNumMutantsToGenerate);

  EXPECT_THAT(mutants,
              AllOf(SizeIs(kNumMutantsToGenerate), Each(SizeIs(Le(kMaxLen)))));
}

TEST(FuzzTestMutator, CrossOverInsertsDataFromOtherInputs) {
  const Knobs knobs;
  FuzzTestMutator mutator(knobs, /*seed=*/1);
  constexpr size_t kNumMutantsToGenerate = 100000;
  const std::vector<ByteArray> mutants = mutator.MutateMany(
      {
          {/*data=*/{0, 1, 2, 3}},
          {/*data=*/{4, 5, 6, 7}},
      },
      kNumMutantsToGenerate);

  EXPECT_THAT(mutants, IsSupersetOf(std::vector<ByteArray>{
                           // The entire other input
                           {4, 5, 6, 7, 0, 1, 2, 3},
                           {0, 1, 4, 5, 6, 7, 2, 3},
                           {0, 1, 2, 3, 4, 5, 6, 7},
                           // The prefix of other input
                           {4, 5, 6, 0, 1, 2, 3},
                           {0, 1, 4, 5, 6, 2, 3},
                           {0, 1, 2, 3, 4, 5, 6},
                           // The suffix of other input
                           {5, 6, 7, 0, 1, 2, 3},
                           {0, 1, 5, 6, 7, 2, 3},
                           {0, 1, 2, 3, 5, 6, 7},
                           // The middle of other input
                           {5, 6, 0, 1, 2, 3},
                           {0, 1, 5, 6, 2, 3},
                           {0, 1, 2, 3, 5, 6},
                       }));
}

TEST(FuzzTestMutator, CrossOverOverwritesDataFromOtherInputs) {
  const Knobs knobs;
  FuzzTestMutator mutator(knobs, /*seed=*/1);
  constexpr size_t kNumMutantsToGenerate = 100000;
  const std::vector<ByteArray> mutants = mutator.MutateMany(
      {
          {/*data=*/{0, 1, 2, 3, 4, 5, 6, 7}},
          {/*data=*/{100, 101, 102, 103}},
      },
      kNumMutantsToGenerate);

  EXPECT_THAT(mutants, IsSupersetOf(std::vector<ByteArray>{
                           // The entire other input
                           {100, 101, 102, 103, 4, 5, 6, 7},
                           {0, 1, 100, 101, 102, 103, 6, 7},
                           {0, 1, 2, 3, 100, 101, 102, 103},
                           // The prefix of other input
                           {100, 101, 102, 3, 4, 5, 6, 7},
                           {0, 1, 2, 100, 101, 102, 6, 7},
                           {0, 1, 2, 3, 4, 100, 101, 102},
                           // The suffix of other input
                           {101, 102, 103, 3, 4, 5, 6, 7},
                           {0, 1, 2, 101, 102, 103, 6, 7},
                           {0, 1, 2, 3, 4, 101, 102, 103},
                           // The middle of other input
                           {101, 102, 2, 3, 4, 5, 6, 7},
                           {0, 1, 2, 101, 102, 5, 6, 7},
                           {0, 1, 2, 3, 4, 5, 101, 102},
                       }));
}

// Test parameter containing the mutation settings and the expectations of a
// single mutation step.
struct MutationStepTestParameter {
  // The input to be mutated.
  ByteArray seed_input;
  // The set of mutants to be expected by mutating `seed_input`.
  absl::flat_hash_set<ByteArray> expected_mutants;
  // The set of mutants not supposed to be seen by mutating `seed_input`.
  absl::flat_hash_set<ByteArray> unexpected_mutants;
  // The max length of the mutants. If unset, will not set the limit.
  std::optional<size_t> max_len;
  // The mutation dictionary.
  std::vector<ByteArray> dictionary;
  // The comparison data following the format of ExecutionMetadata::cmp_data.
  ByteArray cmp_data;
  // The minimum number of iterations regardless of whether all mutants in
  // `expected_mutants` are found or not.
  size_t min_num_iterations = 1000;
  // The maximum number of iterations to try before all mutants in
  // `expected_mutants` are found.
  size_t max_num_iterations = 100000000;
};

class MutationStepTest
    : public testing::TestWithParam<MutationStepTestParameter> {};

TEST_P(MutationStepTest, GeneratesExpectedMutantsAndAvoidsUnexpectedMutants) {
  const Knobs knobs;
  FuzzTestMutator mutator(knobs, /*seed=*/1);
  ASSERT_LE(GetParam().min_num_iterations, GetParam().max_num_iterations);
  if (GetParam().max_len.has_value())
    EXPECT_TRUE(mutator.set_max_len(*GetParam().max_len));
  mutator.AddToDictionary(GetParam().dictionary);
  absl::flat_hash_set<ByteArray> unmatched_expected_mutants =
      GetParam().expected_mutants;
  const auto& unexpected_mutants = GetParam().unexpected_mutants;
  ExecutionMetadata metadata;
  metadata.cmp_data = GetParam().cmp_data;
  const std::vector<MutationInputRef> inputs = {
      {/*data=*/GetParam().seed_input, /*metadata=*/&metadata}};
  for (size_t i = 0; i < GetParam().max_num_iterations; i++) {
    const std::vector<ByteArray> mutants = mutator.MutateMany(inputs, 1);
    ASSERT_EQ(mutants.size(), 1);
    const auto& mutant = mutants[0];
    EXPECT_FALSE(unexpected_mutants.contains(mutant))
        << "Unexpected mutant: {" << absl::StrJoin(mutant, ",") << "}";
    unmatched_expected_mutants.erase(mutant);
    if (unmatched_expected_mutants.empty() &&
        i >= GetParam().min_num_iterations)
      break;
  }
  EXPECT_TRUE(unmatched_expected_mutants.empty());
}

INSTANTIATE_TEST_SUITE_P(InsertByteUpToMaxLen, MutationStepTest, Values([] {
                           MutationStepTestParameter params;
                           params.seed_input = {0, 1, 2};
                           params.expected_mutants = {
                               {0, 1, 2, 3},
                               {0, 3, 1, 2},
                               {3, 0, 1, 2},
                           };
                           params.unexpected_mutants = {
                               {0, 1, 2, 3, 4},
                               {0, 3, 4, 1, 2},
                               {3, 4, 0, 1, 2},
                           };
                           params.max_len = 4;
                           return params;
                         }()));

INSTANTIATE_TEST_SUITE_P(OverwriteFromDictionary, MutationStepTest, Values([] {
                           MutationStepTestParameter params;
                           params.seed_input = {1, 2, 3, 4, 5};
                           params.expected_mutants = {
                               {1, 2, 7, 8, 9},  {1, 7, 8, 9, 5},
                               {7, 8, 9, 4, 5},  {1, 2, 3, 0, 6},
                               {1, 2, 0, 6, 5},  {1, 0, 6, 4, 5},
                               {0, 6, 3, 4, 5},  {42, 2, 3, 4, 5},
                               {1, 42, 3, 4, 5}, {1, 2, 42, 4, 5},
                               {1, 2, 3, 42, 5}, {1, 2, 3, 4, 42},
                           };
                           params.dictionary = {
                               {7, 8, 9},
                               {0, 6},
                               {42},
                           };
                           return params;
                         }()));

INSTANTIATE_TEST_SUITE_P(OverwriteFromCmpDictionary, MutationStepTest,
                         Values([] {
                           MutationStepTestParameter params;
                           params.seed_input = {1, 2, 40, 50, 60};
                           params.expected_mutants = {
                               {3, 4, 40, 50, 60},
                               {1, 2, 10, 20, 30},
                           };
                           params.cmp_data = {2,            // size
                                              1,  2,        // lhs
                                              3,  4,        // rhs
                                              3,            // size
                                              10, 20, 30,   // lhs
                                              40, 50, 60};  // rhs
                           return params;
                         }()));

INSTANTIATE_TEST_SUITE_P(InsertFromDictionary, MutationStepTest, Values([] {
                           MutationStepTestParameter params;
                           params.seed_input = {1, 2, 3};
                           params.expected_mutants = {
                               {1, 2, 3, 4, 5},    {1, 2, 4, 5, 3},
                               {1, 4, 5, 2, 3},    {4, 5, 1, 2, 3},
                               {1, 2, 3, 6, 7, 8}, {1, 2, 6, 7, 8, 3},
                               {1, 6, 7, 8, 2, 3}, {6, 7, 8, 1, 2, 3},
                           };
                           params.dictionary = {
                               {4, 5},
                               {6, 7, 8},
                           };
                           return params;
                         }()));

INSTANTIATE_TEST_SUITE_P(InsertFromCmpDictionary, MutationStepTest, Values([] {
                           MutationStepTestParameter params;
                           params.seed_input = {1, 2, 3};
                           params.expected_mutants = {
                               {1, 2, 3, 4, 5},    {1, 2, 4, 5, 3},
                               {1, 4, 5, 2, 3},    {4, 5, 1, 2, 3},
                               {1, 2, 3, 6, 7, 8}, {1, 2, 6, 7, 8, 3},
                               {1, 6, 7, 8, 2, 3}, {6, 7, 8, 1, 2, 3},
                           };
                           params.cmp_data = {2,         // size
                                              4, 5,      // lhs
                                              4, 5,      // rhs
                                              3,         // size
                                              6, 7, 8,   // lhs
                                              6, 7, 8};  // rhs
                           return params;
                         }()));

INSTANTIATE_TEST_SUITE_P(SkipsLongCmpEntry, MutationStepTest, Values([] {
                           MutationStepTestParameter params;
                           params.seed_input = {0};
                           params.expected_mutants = {
                               {0, 1, 2, 3, 4},
                           };
                           params.unexpected_mutants = {
                               {0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
                                11, 12, 13, 14, 15, 16, 17, 18, 19, 20},
                           };
                           params.cmp_data = {
                               20,  // size
                               1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                               11, 12, 13, 14, 15, 16, 17, 18, 19, 20,  // lhs
                               1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                               11, 12, 13, 14, 15, 16, 17, 18, 19, 20,  // rhs
                               4,                                       // size
                               1,  2,  3,  4,                           // lhs
                               1,  2,  3,  4};                          // rhs
                           return params;
                         }()));

}  // namespace

}  // namespace fuzztest::internal
