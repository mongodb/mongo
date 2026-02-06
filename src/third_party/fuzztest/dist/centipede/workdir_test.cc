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

#include "./centipede/workdir.h"

#include <array>
#include <string_view>

#include "gtest/gtest.h"
#include "./centipede/environment.h"

namespace fuzztest::internal {

TEST(WorkDirTest, Ctors) {
  const WorkDir wd{
      /*workdir=*/"/dir",
      /*binary_name=*/"bin",
      /*binary_hash=*/"hash",
      /*my_shard_index=*/3,
  };
  Environment env;
  env.workdir = "/dir";
  env.my_shard_index = 3;
  env.binary_name = "bin";
  env.binary_hash = "hash";

  const std::array<WorkDir, 3> other_wds = {
      WorkDir{env},
      WorkDir::FromCorpusShardPath(                    //
          /*corpus_shard_path=*/"/dir/corpus.000003",  //
          /*binary_name=*/"bin",                       //
          /*binary_hash=*/"hash"),
      WorkDir::FromCorpusShardPath(                           //
          /*corpus_shard_path=*/"/dir/distilled-nib.000003",  //
          /*binary_name=*/"bin",                              //
          /*binary_hash=*/"hash"),
  };
  for (int i = 0; i < other_wds.size(); ++i) {
    EXPECT_EQ(other_wds[i], wd) << "Test case " << i;
  }
}

TEST(WorkDirTest, Api) {
  const WorkDir wd{"/dir", "bin", "hash", 3};

  EXPECT_EQ(wd.DebugInfoDirPath(), "/dir/debug");
  EXPECT_EQ(wd.CoverageDirPath(), "/dir/bin-hash");
  EXPECT_EQ(wd.BinaryInfoDirPath(), "/dir/bin-hash/binary-info");

  EXPECT_EQ(wd.CrashReproducerDirPaths().MyShard(), "/dir/crashes.000003");
  EXPECT_EQ(wd.CrashReproducerDirPaths().Shard(7), "/dir/crashes.000007");
  EXPECT_EQ(wd.CrashReproducerDirPaths().AllShardsGlob(), "/dir/crashes.*");
  EXPECT_TRUE(wd.CrashReproducerDirPaths().IsShard("/dir/crashes.000009"));

  EXPECT_EQ(wd.CrashMetadataDirPaths().MyShard(), "/dir/crash-metadata.000003");
  EXPECT_EQ(wd.CrashMetadataDirPaths().Shard(7), "/dir/crash-metadata.000007");
  EXPECT_EQ(wd.CrashMetadataDirPaths().AllShardsGlob(),
            "/dir/crash-metadata.*");
  EXPECT_TRUE(wd.CrashMetadataDirPaths().IsShard("/dir/crash-metadata.000009"));

  EXPECT_EQ(wd.CorpusFilePaths().MyShard(), "/dir/corpus.000003");
  EXPECT_EQ(wd.CorpusFilePaths().Shard(7), "/dir/corpus.000007");
  EXPECT_EQ(wd.CorpusFilePaths().AllShardsGlob(), "/dir/corpus.*");
  EXPECT_TRUE(wd.CorpusFilePaths().IsShard("/dir/corpus.000009"));

  EXPECT_EQ(wd.DistilledCorpusFilePaths().MyShard(),  //
            "/dir/distilled-bin.000003");
  EXPECT_EQ(wd.DistilledCorpusFilePaths().Shard(7),  //
            "/dir/distilled-bin.000007");
  EXPECT_EQ(wd.DistilledCorpusFilePaths().AllShardsGlob(),  //
            "/dir/distilled-bin.*");
  EXPECT_TRUE(wd.DistilledCorpusFilePaths().IsShard(  //
      "/dir/distilled-bin.000009"));

  EXPECT_EQ(wd.FeaturesFilePaths().MyShard(),  //
            "/dir/bin-hash/features.000003");
  EXPECT_EQ(wd.FeaturesFilePaths().Shard(7),  //
            "/dir/bin-hash/features.000007");
  EXPECT_EQ(wd.FeaturesFilePaths().AllShardsGlob(),  //
            "/dir/bin-hash/features.*");
  EXPECT_TRUE(wd.FeaturesFilePaths().IsShard(  //
      "/dir/bin-hash/features.000009"));

  EXPECT_EQ(wd.DistilledFeaturesFilePaths().MyShard(),  //
            "/dir/bin-hash/distilled-features-bin.000003");
  EXPECT_EQ(wd.DistilledFeaturesFilePaths().Shard(7),  //
            "/dir/bin-hash/distilled-features-bin.000007");
  EXPECT_EQ(wd.DistilledFeaturesFilePaths().AllShardsGlob(),  //
            "/dir/bin-hash/distilled-features-bin.*");
  EXPECT_TRUE(wd.DistilledFeaturesFilePaths().IsShard(  //
      "/dir/bin-hash/distilled-features-bin.000009"));

  EXPECT_EQ(wd.CoverageReportPath(),  //
            "/dir/coverage-report-bin.000003.txt");
  EXPECT_EQ(wd.CoverageReportPath("anno"),
            "/dir/coverage-report-bin.000003.anno.txt");
  EXPECT_EQ(wd.SourceBasedCoverageReportPath(),
            "/dir/source-coverage-report-bin.000003");
  EXPECT_EQ(wd.SourceBasedCoverageReportPath("anno"),
            "/dir/source-coverage-report-bin.000003.anno");
  EXPECT_EQ(wd.SourceBasedCoverageRawProfilePath(),
            "/dir/bin-hash/clang_coverage.000003.%m.profraw");
  EXPECT_EQ(wd.SourceBasedCoverageIndexedProfilePath(),
            "/dir/bin-hash/clang_coverage.profdata");
  // TODO(ussuri): Test `EnumerateRawCoverageProfiles()`.

  EXPECT_EQ(wd.CorpusStatsPath(),  //
            "/dir/corpus-stats-bin.000003.json");
  EXPECT_EQ(wd.CorpusStatsPath("anno"),  //
            "/dir/corpus-stats-bin.000003.anno.json");
  EXPECT_EQ(wd.CoverageReportPath(),  //
            "/dir/coverage-report-bin.000003.txt");
  EXPECT_EQ(wd.CoverageReportPath("anno"),  //
            "/dir/coverage-report-bin.000003.anno.txt");
  EXPECT_EQ(wd.FuzzingStatsPath(),  //
            "/dir/fuzzing-stats-bin.000003.csv");
  EXPECT_EQ(wd.FuzzingStatsPath("anno"),  //
            "/dir/fuzzing-stats-bin.000003.anno.csv");
  EXPECT_EQ(wd.RUsageReportPath(),  //
            "/dir/rusage-report-bin.000003.txt");
  EXPECT_EQ(wd.RUsageReportPath("anno"),  //
            "/dir/rusage-report-bin.000003.anno.txt");
  EXPECT_EQ(wd.RUsageReportPath(),  //
            "/dir/rusage-report-bin.000003.txt");
  EXPECT_EQ(wd.RUsageReportPath("anno"),  //
            "/dir/rusage-report-bin.000003.anno.txt");
}

}  // namespace fuzztest::internal
