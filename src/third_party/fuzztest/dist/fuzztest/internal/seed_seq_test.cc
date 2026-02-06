// Copyright 2022 Google LLC
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

#include "./fuzztest/internal/seed_seq.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/types/span.h"

namespace fuzztest::internal {
namespace {

using ::testing::ElementsAre;
using ::testing::Optional;

TEST(GetFromEnvOrMakeSeedSeqTest, GetsSeedSeqFromEnv) {
  const std::vector<uint32_t> seed_material = {1, 2, 3, 4, 5, 6, 7, 8};
  const std::string encoded_seed_material = EncodeSeedMaterial(seed_material);
  ASSERT_EQ(
      setenv("FUZZ_TEST_SEED_GETS_FROM_ENV", encoded_seed_material.c_str(),
             /*overwrite=*/1),
      0);

  std::stringstream stream;
  std::seed_seq expected_seed_seq =
      std::seed_seq(seed_material.begin(), seed_material.end());
  std::seed_seq seed_seq =
      GetFromEnvOrMakeSeedSeq(stream, "FUZZ_TEST_SEED_GETS_FROM_ENV");

  // Can't compare seed sequences directly, so we compare seeded RNGs.
  EXPECT_EQ(std::mt19937(seed_seq), std::mt19937(expected_seed_seq));
  EXPECT_EQ(stream.str(), absl::StrCat("FUZZ_TEST_SEED_GETS_FROM_ENV=",
                                       encoded_seed_material, "\n"));
}

TEST(GetFromEnvOrMakeSeedSeqTest, MakesAndOutputsSeedSeq) {
  ASSERT_EQ(unsetenv("FUZZ_TEST_SEED_MAKES_AND_OUTPUTS_SEED_SEQ"), 0);

  std::stringstream stream;
  std::seed_seq seed_seq = GetFromEnvOrMakeSeedSeq(
      stream, "FUZZ_TEST_SEED_MAKES_AND_OUTPUTS_SEED_SEQ");
  const std::string output = stream.str();
  absl::string_view output_view = output;
  ASSERT_TRUE(absl::ConsumePrefix(
      &output_view, "FUZZ_TEST_SEED_MAKES_AND_OUTPUTS_SEED_SEQ="));
  ASSERT_TRUE(absl::ConsumeSuffix(&output_view, "\n"));
  std::optional<std::vector<uint32_t>> seed_material =
      DecodeSeedMaterial(output_view);
  ASSERT_TRUE(seed_material.has_value());
  std::seed_seq expected_seed_seq(seed_material->begin(), seed_material->end());

  // Can't compare seed sequences directly, so we compare seeded RNGs.
  EXPECT_EQ(std::mt19937(seed_seq), std::mt19937(expected_seed_seq));
}

TEST(GetFromEnvOrMakeSeedSeqDeathTest, AbortsOnInvalidSeedMaterialEncoding) {
  ASSERT_EQ(setenv("FUZZ_TEST_SEED_ABORTS_ON_INVALID_SEED_MATERIAL_ENCODING",
                   "Exclamation is invalid!", /*overwrite=*/1),
            0);

  std::stringstream stream;
  EXPECT_DEATH_IF_SUPPORTED(
      GetFromEnvOrMakeSeedSeq(
          stream, "FUZZ_TEST_SEED_ABORTS_ON_INVALID_SEED_MATERIAL_ENCODING"),
      "Failed to decode seed material");
}

TEST(EncodeSeedMaterialTest, EncodingFollowedByDecodingIsIdentity) {
  EXPECT_THAT(DecodeSeedMaterial(EncodeSeedMaterial({1})),
              Optional(ElementsAre(1)));
  EXPECT_THAT(DecodeSeedMaterial(EncodeSeedMaterial({1, 2})),
              Optional(ElementsAre(1, 2)));
  EXPECT_THAT(DecodeSeedMaterial(EncodeSeedMaterial({1, 2, 3})),
              Optional(ElementsAre(1, 2, 3)));
  EXPECT_THAT(DecodeSeedMaterial(EncodeSeedMaterial({1, 2, 3, 4})),
              Optional(ElementsAre(1, 2, 3, 4)));
}

TEST(DecodeSeedMaterialTest, FailsToDecodeInvalidSeedMaterial) {
  EXPECT_EQ(DecodeSeedMaterial("Exclamation is invalid!"), std::nullopt);
}

}  // namespace
}  // namespace fuzztest::internal
