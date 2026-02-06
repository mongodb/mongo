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

#include "./centipede/seed_corpus_maker_proto_lib.h"

#include <cstddef>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "./centipede/feature.h"
#include "./centipede/seed_corpus_config.pb.h"
#include "./centipede/seed_corpus_maker_lib.h"
#include "./centipede/workdir.h"
#include "./common/logging.h"  // IWYU pragma: keep
#include "./common/test_util.h"

namespace fuzztest::internal {
namespace {

namespace fs = std::filesystem;

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

TEST(SeedCorpusMakerProtoLibTest, MakesSeedUsingConfigProto) {
  const fs::path test_dir = GetTestTempDir(test_info_->name());
  // `ResolveSeedCorpusConfig()` should use the CWD to resolve relative paths.
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

  // Prepare a seed source for tests.
  {
    const SeedCorpusDestination destination = {
        /*dir_path=*/std::string(kRelDir1),
        /*shard_rel_glob=*/absl::StrCat("distilled-", kCovBin, ".*"),
        /*shard_index_digits=*/kIdxDigits,
        /*num_shards=*/2,
    };
    ASSERT_OK(WriteSeedCorpusElementsToDestination(  //
        kElements, kCovBin, kCovHash, destination));
  }

  // Test that `GenerateSeedCorpusFromConfigProto()` correctly uses the config
  // proto to generate the seed corpus shards in the destination.
  {
    constexpr size_t kNumShards = 3;
    constexpr std::string_view kConfigStr = R"pb(
      sources {
        dir_glob: "./$0"
        shard_rel_glob: "distilled-$1.*"
        num_recent_dirs: 1
        sampled_fraction: 1.0
      }
      destination {
        dir_path: "./$2"
        shard_rel_glob: "corpus.*"
        num_shards: $3
        shard_index_digits: $4
      }
    )pb";

    const std::string config_str = absl::Substitute(  //
        kConfigStr, kRelDir1, kCovBin, kRelDir2, kNumShards, kIdxDigits);

    ASSERT_OK(GenerateSeedCorpusFromConfigProto(  //
        config_str, kCovBin, kCovHash, ""));
    const std::string workdir = (test_dir / kRelDir2).c_str();
    ASSERT_NO_FATAL_FAILURE(VerifyDumpedConfig(workdir, kCovBin, kCovHash));
    ASSERT_NO_FATAL_FAILURE(VerifyShardsExist(  //
        workdir, kCovBin, kCovHash, kNumShards, ShardType::kNormal));
  }
}

}  // namespace
}  // namespace fuzztest::internal
