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

#include "./centipede/corpus_io.h"

#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/types/span.h"
#include "./centipede/corpus.h"
#include "./centipede/feature.h"
#include "./centipede/util.h"
#include "./centipede/workdir.h"
#include "./common/blob_file.h"
#include "./common/defs.h"
#include "./common/test_util.h"

namespace fuzztest::internal {
namespace {

using ::testing::UnorderedElementsAre;

void WriteBlobsToFile(std::string_view blob_file_path,
                      absl::Span<const ByteArray> blobs) {
  auto writer = DefaultBlobFileWriterFactory();
  CHECK_OK(writer->Open(blob_file_path, "w"));
  for (const ByteArray& blob : blobs) {
    CHECK_OK(writer->Write(blob));
  }
  CHECK_OK(writer->Close());
}

std::vector<ByteArray> ReadInputsFromFiles(std::string_view dir) {
  std::vector<ByteArray> inputs;
  for (const auto& file : std::filesystem::directory_iterator(dir)) {
    ByteArray input;
    ReadFromLocalFile(file.path().c_str(), input);
    inputs.push_back(std::move(input));
  }
  return inputs;
}

TEST(ReadShardTest, ReadsInputsAndFeaturesAndCallsCallbackForEachPair) {
  ByteArray data1 = {1, 2, 3};
  ByteArray data2 = {3, 4, 5, 6};
  ByteArray data3 = {7, 8, 9, 10, 11};
  ByteArray data4 = {12, 13, 14};
  ByteArray data5 = {15, 16};
  FeatureVec fv1 = {100, 200, 300};
  FeatureVec fv2 = {300, 400, 500, 600};
  FeatureVec fv3 = {700, 800, 900, 1000, 1100};
  FeatureVec fv4 = {};  // empty.

  std::vector<ByteArray> corpus_blobs;
  corpus_blobs.push_back(data1);
  corpus_blobs.push_back(data2);
  corpus_blobs.push_back(data3);
  corpus_blobs.push_back(data4);
  corpus_blobs.push_back(data5);

  std::vector<ByteArray> features_blobs;
  features_blobs.push_back(PackFeaturesAndHash(data1, fv1));
  features_blobs.push_back(PackFeaturesAndHash(data2, fv2));
  features_blobs.push_back(PackFeaturesAndHash(data3, fv3));
  features_blobs.push_back(PackFeaturesAndHash(data4, fv4));

  TempDir tmp_dir{test_info_->name()};
  std::string corpus_path = tmp_dir.GetFilePath("corpus");
  std::string features_path = tmp_dir.GetFilePath("features");
  WriteBlobsToFile(corpus_path, corpus_blobs);
  WriteBlobsToFile(features_path, features_blobs);

  std::vector<CorpusRecord> res;
  ReadShard(corpus_path, features_path,
            [&res](const ByteArray& input, const FeatureVec& features) {
              res.push_back(CorpusRecord{input, features});
            });

  EXPECT_EQ(res.size(), 5UL);
  EXPECT_EQ(res[0].data, data1);
  EXPECT_EQ(res[1].data, data2);
  EXPECT_EQ(res[2].data, data3);
  EXPECT_EQ(res[3].data, data4);
  EXPECT_EQ(res[4].data, data5);
  EXPECT_EQ(res[0].features, fv1);
  EXPECT_EQ(res[1].features, fv2);
  EXPECT_EQ(res[2].features, fv3);
  EXPECT_EQ(res[3].features, FeatureVec{feature_domains::kNoFeature});
  EXPECT_EQ(res[4].features, FeatureVec());
}

TEST(ExportCorpusTest, ExportsCorpusToIndividualFiles) {
  const std::filesystem::path temp_dir = GetTestTempDir(test_info_->name());
  const std::filesystem::path out_dir = temp_dir / "out_dir";
  CHECK(std::filesystem::create_directory(out_dir));
  const WorkDir workdir{temp_dir.c_str(), "fake_binary_name",
                        "fake_binary_hash", /*my_shard_index=*/0};
  const auto corpus_file_paths = workdir.CorpusFilePaths();
  WriteBlobsToFile(corpus_file_paths.Shard(0), {ByteArray{1, 2}, ByteArray{3}});
  WriteBlobsToFile(corpus_file_paths.Shard(1), {ByteArray{4}, ByteArray{5, 6}});

  ExportCorpus({corpus_file_paths.Shard(0), corpus_file_paths.Shard(1)},
               out_dir.c_str());

  EXPECT_THAT(ReadInputsFromFiles(out_dir.c_str()),
              UnorderedElementsAre(ByteArray{1, 2}, ByteArray{3}, ByteArray{4},
                                   ByteArray{5, 6}));
}

}  // namespace
}  // namespace fuzztest::internal
