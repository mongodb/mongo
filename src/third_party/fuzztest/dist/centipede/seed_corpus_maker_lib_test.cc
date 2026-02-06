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

#include "./centipede/seed_corpus_maker_lib.h"

#include <unistd.h>

#include <cmath>
#include <cstddef>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "./centipede/feature.h"
#include "./centipede/workdir.h"
#include "./common/defs.h"
#include "./common/logging.h"  // IWYU pragma: keep
#include "./common/remote_file.h"
#include "./common/test_util.h"

namespace fuzztest::internal {
namespace {

namespace fs = std::filesystem;

using ::testing::IsSubsetOf;
using ::testing::IsSupersetOf;

inline constexpr auto kIdxDigits = WorkDir::kDigitsInShardIndex;

enum ShardType { kNormal, kDistilled };

void VerifyShardsExist(            //
    std::string_view workdir,      //
    std::string_view binary_name,  //
    std::string_view binary_hash,  //
    size_t num_shards,             //
    ShardType shard_type) {
  const WorkDir wd{
      std::string{workdir},
      std::string{binary_name},
      std::string{binary_hash},
      /*my_shard_index=*/0,
  };
  const WorkDir::PathShards corpus_file_paths =
      shard_type == kNormal ? wd.CorpusFilePaths()
                            : wd.DistilledCorpusFilePaths();
  const WorkDir::PathShards features_file_paths =
      shard_type == kNormal ? wd.FeaturesFilePaths()
                            : wd.DistilledFeaturesFilePaths();
  for (int shard = 0; shard < num_shards + 2; ++shard) {
    if (shard < num_shards) {
      ASSERT_TRUE(fs::exists(corpus_file_paths.Shard(shard)))
          << VV(shard) << VV(corpus_file_paths.Shard(shard));
      ASSERT_TRUE(fs::exists(features_file_paths.Shard(shard)))
          << VV(shard) << VV(features_file_paths.Shard(shard));
    } else {
      ASSERT_FALSE(fs::exists(corpus_file_paths.Shard(shard)))
          << VV(shard) << VV(corpus_file_paths.Shard(shard));
      ASSERT_FALSE(fs::exists(features_file_paths.Shard(shard)))
          << VV(shard) << VV(features_file_paths.Shard(shard));
    }
  }
}

void VerifyDumpedConfig(           //
    std::string_view workdir,      //
    std::string_view binary_name,  //
    std::string_view binary_hash) {
  const WorkDir wd{
      std::string{workdir},
      std::string{binary_name},
      std::string{binary_hash},
      /*my_shard_index=*/0,
  };
  // TODO(ussuri): Verify the contents is as expected as well.
  ASSERT_TRUE(fs::exists(fs::path{wd.DebugInfoDirPath()} / "seeding.cfg"))
      << VV(workdir);
}

TEST(SeedCorpusMakerLibTest, RoundTripWriteReadWrite) {
  const fs::path test_dir = GetTestTempDir(test_info_->name());
  chdir(test_dir.c_str());

  const InputAndFeaturesVec kElements = {
      {{0}, {}},
      {{1}, {feature_domains::kNoFeature}},
      {{0, 1}, {0x11, 0x23}},
      {{1, 2, 3}, {0x11, 0x23, 0xfe}},
      {{3, 4, 5, 6}, {0x111, 0x234, 0x345, 0x56}},
      {{5, 6, 7, 9}, {0x1111, 0x2345, 0x3456, 0x5678}},
      {{7, 8, 9, 10, 111}, {0x11111, 0x23456, 0x34567, 0x56789, 0xffaf}},
  };
  constexpr std::string_view kCovBin = "bin";
  constexpr std::string_view kCovHash = "hash";
  constexpr std::string_view kRelDir1 = "dir/foo";
  constexpr std::string_view kRelDir2 = "dir/bar";

  // Test `WriteSeedCorpusElementsToDestination()`. This also creates a seed
  // source for the subsequent tests.
  {
    constexpr size_t kNumShards = 2;
    const SeedCorpusDestination destination = {
        /*dir_path=*/std::string(kRelDir1),
        /*shard_rel_glob=*/absl::StrCat("distilled-", kCovBin, ".*"),
        /*shard_index_digits=*/kIdxDigits,
        /*num_shards=*/kNumShards,
    };
    ASSERT_OK(WriteSeedCorpusElementsToDestination(  //
        kElements, kCovBin, kCovHash, destination));
    const std::string workdir = (test_dir / kRelDir1).c_str();
    ASSERT_NO_FATAL_FAILURE(VerifyShardsExist(  //
        workdir, kCovBin, kCovHash, kNumShards, ShardType::kDistilled));
  }

  // Test that `SampleSeedCorpusElementsFromSource()` correctly reads a
  // subsample of elements from the seed source created by the previous step.
  {
    for (const float fraction : {1.0, 0.5, 0.2}) {
      SeedCorpusSource source;
      source.dir_glob = std::string(kRelDir1);
      source.num_recent_dirs = 2;
      source.shard_rel_glob = absl::StrCat("distilled-", kCovBin, ".*");
      source.sampled_fraction_or_count = fraction;

      InputAndFeaturesVec elements;
      ASSERT_OK(SampleSeedCorpusElementsFromSource(  //
          source, kCovBin, kCovHash, elements));
      // NOTE: 1.0 has a precise double representation, so `==` is fine.
      ASSERT_EQ(elements.size(), std::llrint(kElements.size() * fraction))
          << VV(fraction);
      ASSERT_THAT(elements, IsSubsetOf(kElements)) << VV(fraction);
    }
  }

  // Test that `GenerateSeedCorpusFromConfig()` correctly samples seed elements
  // from the source and writes expected shards to the destination.
  {
    constexpr size_t kNumShards = 3;

    SeedCorpusSource source;
    source.dir_glob = std::string(kRelDir1);
    source.num_recent_dirs = 1;
    source.shard_rel_glob = absl::StrCat("distilled-", kCovBin, ".*");
    source.sampled_fraction_or_count = 1.0f;
    const SeedCorpusConfig config = {
        /*sources=*/{{source}},
        /*destination=*/
        {
            /*dir_path=*/std::string(kRelDir2),
            /*shard_rel_glob=*/"corpus.*",
            /*shard_index_digits=*/kIdxDigits,
            /*num_shards=*/kNumShards,
        },
    };

    {
      ASSERT_OK(GenerateSeedCorpusFromConfig(  //
          config, kCovBin, kCovHash));
      const std::string workdir = (test_dir / kRelDir2).c_str();
      ASSERT_NO_FATAL_FAILURE(VerifyShardsExist(  //
          workdir, kCovBin, kCovHash, kNumShards, ShardType::kNormal));
    }
  }
}

TEST(SeedCorpusMakerLibTest, LoadsBothIndividualInputsAndShardsFromSource) {
  const fs::path test_dir = GetTestTempDir(test_info_->name());
  chdir(test_dir.c_str());

  const InputAndFeaturesVec kShardedInputs = {
      {{0}, {}},
      {{1}, {feature_domains::kNoFeature}},
      {{0, 1}, {0x11, 0x23}},
  };
  constexpr std::string_view kCovBin = "bin";
  constexpr std::string_view kCovHash = "hash";
  constexpr std::string_view kRelDir = "dir/foo";

  const std::vector<ByteArray> kIndividualInputs = {
      {0, 1, 2},
      {0, 1, 2, 3},
      // Empty input expected to be not in the sample result.
      {}};
  // Write sharded inputs.
  {
    constexpr size_t kNumShards = 2;
    const SeedCorpusDestination destination = {
        /*dir_path=*/std::string(kRelDir),
        /*shard_rel_glob=*/absl::StrCat("distilled-", kCovBin, ".*"),
        /*shard_index_digits=*/kIdxDigits,
        /*num_shards=*/kNumShards,
    };
    CHECK_OK(WriteSeedCorpusElementsToDestination(  //
        kShardedInputs, kCovBin, kCovHash, destination));
    const std::string workdir = (test_dir / kRelDir).c_str();
    ASSERT_NO_FATAL_FAILURE(VerifyShardsExist(  //
        workdir, kCovBin, kCovHash, kNumShards, ShardType::kDistilled));
  }

  // Write individual inputs
  for (int i = 0; i < kIndividualInputs.size(); ++i) {
    const auto path = std::filesystem::path(test_dir) / kRelDir /
                      absl::StrCat("individual_input_", i);
    CHECK_OK(RemoteFileSetContents(path.string(), kIndividualInputs[i]));
  }

  // Test that sharded and individual inputs matches what we wrote.
  {
    InputAndFeaturesVec elements;
    ASSERT_OK(SampleSeedCorpusElementsFromSource(  //
        SeedCorpusSource{
            /*dir_glob=*/std::string(kRelDir),
            /*num_recent_dirs=*/1,
            /*shard_rel_glob=*/absl::StrCat("distilled-", kCovBin, ".*"),
            // Intentionally try to match the shard files and test if they will
            // be read as individual inputs.
            /*individual_input_rel_glob=*/"*",
            /*sampled_fraction_or_count=*/1.0f,
        },
        kCovBin, kCovHash, elements));
    EXPECT_EQ(elements.size(), 5);  // Non-empty inputs
    EXPECT_THAT(elements, IsSupersetOf(kShardedInputs));
    EXPECT_THAT(elements, IsSupersetOf(InputAndFeaturesVec{
                              {{0, 1, 2}, {}},
                              {{0, 1, 2, 3}, {}},
                          }));
  }
}

}  // namespace
}  // namespace fuzztest::internal
