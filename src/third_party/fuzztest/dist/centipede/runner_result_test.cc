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

#include "./centipede/runner_result.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "./centipede/execution_metadata.h"
#include "./centipede/feature.h"
#include "./centipede/shared_memory_blob_sequence.h"
#include "./common/defs.h"
#include "./common/test_util.h"

namespace fuzztest::internal {
namespace {

using ::testing::ElementsAre;

TEST(ExecutionResult, WriteThenRead) {
  auto buffer = std::make_unique<uint8_t[]>(1000);
  BlobSequence blobseq(buffer.get(), 1000);
  BatchResult batch_result;

  // Imitate execution of two inputs.
  FeatureVec v1{1, 2, 3};
  FeatureVec v2{5, 6, 7, 8};
  ExecutionMetadata metadata;
  metadata.AppendCmpEntry({1, 2, 3}, {4, 5, 6});
  ExecutionResult::Stats stats1;
  stats1.peak_rss_mb = 10;
  ExecutionResult::Stats stats2;
  stats2.peak_rss_mb = 20;
  // First input.
  EXPECT_TRUE(BatchResult::WriteInputBegin(blobseq));
  EXPECT_TRUE(BatchResult::WriteOneFeatureVec(v1.data(), v1.size(), blobseq));
  // Write stats after features. The order should not matter.
  EXPECT_TRUE(BatchResult::WriteStats(stats1, blobseq));
  // Done.
  EXPECT_TRUE(BatchResult::WriteInputEnd(blobseq));

  // Second input.
  EXPECT_TRUE(BatchResult::WriteInputBegin(blobseq));
  // Write stats before features.
  EXPECT_TRUE(BatchResult::WriteStats(stats2, blobseq));
  EXPECT_TRUE(BatchResult::WriteOneFeatureVec(v2.data(), v2.size(), blobseq));
  // Write CMP traces.
  EXPECT_TRUE(BatchResult::WriteMetadata(metadata, blobseq));
  // Done.
  EXPECT_TRUE(BatchResult::WriteInputEnd(blobseq));

  // Ensure we've read them.
  blobseq.Reset();
  batch_result.ClearAndResize(2);
  EXPECT_TRUE(batch_result.Read(blobseq));
  EXPECT_EQ(batch_result.results().size(), 2);
  EXPECT_EQ(batch_result.results()[0].features(), v1);
  EXPECT_EQ(batch_result.results()[0].stats(), stats1);
  EXPECT_EQ(batch_result.results()[1].features(), v2);
  EXPECT_EQ(batch_result.results()[1].stats(), stats2);
  EXPECT_THAT(batch_result.results()[1].metadata().cmp_data,
              ElementsAre(3,        // size
                          1, 2, 3,  // cmp0
                          4, 5, 6   // cmp1
                          ));

  // If there are fewer ExecutionResult-s than expected everything should work.
  blobseq.Reset();
  batch_result.ClearAndResize(3);
  EXPECT_TRUE(batch_result.Read(blobseq));
  EXPECT_EQ(batch_result.results().size(), 3);
  EXPECT_EQ(batch_result.results()[0].features(), v1);
  EXPECT_EQ(batch_result.results()[1].features(), v2);
  EXPECT_EQ(batch_result.results()[2].features(), FeatureVec{});

  // If there are too many ExecutionResult-s, Read() should fail.
  // This should not happen in normal operation.
  blobseq.Reset();
  batch_result.ClearAndResize(1);
  EXPECT_FALSE(batch_result.Read(blobseq));
}

TEST(ExecutionResult, WriteIntoFileThenRead) {
  const std::string temp_file = GetTestTempDir(test_info_->name()) / "tmp.txt";
  std::ofstream output_stream(temp_file, std::ios::out);
  ASSERT_TRUE(output_stream.is_open());

  // Imitate execution of two inputs.
  FeatureVec v1{1, 2, 3};
  FeatureVec v2{5, 6, 7, 8};
  ExecutionResult::Stats stats1;
  stats1.peak_rss_mb = 10;
  ExecutionResult::Stats stats2;
  stats2.peak_rss_mb = 20;
  ExecutionMetadata metadata;
  metadata.AppendCmpEntry({1, 2, 3}, {4, 5, 6});

  std::vector<uint8_t> buffer1(1000);
  BlobSequence blobseq1(buffer1.data(), buffer1.size());
  // First input.
  ASSERT_TRUE(BatchResult::WriteInputBegin(blobseq1));
  ASSERT_TRUE(BatchResult::WriteOneFeatureVec(v1.data(), v1.size(), blobseq1));
  // Write stats after features. The order should not matter.
  ASSERT_TRUE(BatchResult::WriteStats(stats1, blobseq1));
  // Done.
  ASSERT_TRUE(BatchResult::WriteInputEnd(blobseq1));

  output_stream.write(reinterpret_cast<char*>(buffer1.data()),
                      blobseq1.offset());

  std::vector<uint8_t> buffer2(1000);
  BlobSequence blobseq2(buffer2.data(), buffer2.size());
  // Second input.
  ASSERT_TRUE(BatchResult::WriteInputBegin(blobseq2));
  // Write stats before features.
  ASSERT_TRUE(BatchResult::WriteStats(stats2, blobseq2));
  ASSERT_TRUE(BatchResult::WriteOneFeatureVec(v2.data(), v2.size(), blobseq2));
  // Write CMP traces.
  EXPECT_TRUE(BatchResult::WriteMetadata(metadata, blobseq2));
  // Done.
  ASSERT_TRUE(BatchResult::WriteInputEnd(blobseq2));

  output_stream.write(reinterpret_cast<char*>(buffer2.data()),
                      blobseq2.offset());

  output_stream.close();

  std::ifstream input_stream(temp_file);
  std::string content((std::istreambuf_iterator<char>(input_stream)),
                      (std::istreambuf_iterator<char>()));
  BlobSequence blobseq(reinterpret_cast<uint8_t*>(content.data()),
                       content.size());
  BatchResult batch_result;
  batch_result.ClearAndResize(2);
  ASSERT_TRUE(batch_result.Read(blobseq));
  EXPECT_EQ(batch_result.num_outputs_read(), 2);
  EXPECT_EQ(batch_result.results()[0].features(), v1);
  EXPECT_EQ(batch_result.results()[1].features(), v2);
  EXPECT_EQ(batch_result.results()[0].stats(), stats1);
  EXPECT_EQ(batch_result.results()[1].stats(), stats2);
  EXPECT_THAT(batch_result.results()[1].metadata().cmp_data,
              ElementsAre(3,        // size
                          1, 2, 3,  // cmp0
                          4, 5, 6   // cmp1
                          ));
}

TEST(ExecutionResult, IdentifiesSetupFailure) {
  BatchResult batch_result;
  batch_result.exit_code() = EXIT_FAILURE;
  batch_result.failure_description() = "SETUP FAILURE: something went wrong";

  EXPECT_TRUE(batch_result.IsSetupFailure());
}

TEST(MutationResult, WriteThenRead) {
  std::array<uint8_t, 1000> buffer;
  BlobSequence blobseq(buffer.data(), buffer.size());

  // Write a mutation result.
  ASSERT_TRUE(MutationResult::WriteHasCustomMutator(true, blobseq));
  ASSERT_TRUE(MutationResult::WriteMutant({1, 2, 3}, blobseq));
  ASSERT_TRUE(MutationResult::WriteMutant({4, 5, 6}, blobseq));
  ASSERT_TRUE(MutationResult::WriteMutant({7, 8, 9}, blobseq));
  blobseq.Reset();

  MutationResult mutation_result;
  ASSERT_TRUE(mutation_result.Read(3, blobseq));

  EXPECT_TRUE(mutation_result.has_custom_mutator());
  EXPECT_THAT(
      mutation_result.mutants(),
      ElementsAre(ByteArray{1, 2, 3}, ByteArray{4, 5, 6}, ByteArray{7, 8, 9}));
}

TEST(ExecutionResult, ReadResultSucceedsOnlyWithInputBegin) {
  auto buffer = std::make_unique<uint8_t[]>(1000);
  BlobSequence blobseq(buffer.get(), 1000);
  BatchResult batch_result;

  EXPECT_TRUE(BatchResult::WriteInputBegin(blobseq));
  EXPECT_TRUE(BatchResult::WriteOneFeatureVec({}, 0, blobseq));
  EXPECT_TRUE(BatchResult::WriteInputEnd(blobseq));
  blobseq.Reset();
  batch_result.ClearAndResize(1);
  EXPECT_TRUE(batch_result.Read(blobseq));

  blobseq.Reset();
  EXPECT_TRUE(BatchResult::WriteOneFeatureVec({}, 0, blobseq));
  EXPECT_TRUE(BatchResult::WriteInputEnd(blobseq));

  blobseq.Reset();
  batch_result.ClearAndResize(1);
  EXPECT_FALSE(batch_result.Read(blobseq));
}

TEST(ExecutionResult, ReadDispatcher32BitFeatures) {
  auto buffer = std::make_unique<uint8_t[]>(1000);
  BlobSequence blobseq(buffer.get(), 1000);
  BatchResult batch_result;

  std::vector<uint32_t> dispatcher_features = {0, 1, 0x7fffffff, 0xffffffff};

  EXPECT_TRUE(BatchResult::WriteInputBegin(blobseq));
  EXPECT_TRUE(BatchResult::WriteDispatcher32BitFeatures(
      dispatcher_features.data(), dispatcher_features.size(), blobseq));
  EXPECT_TRUE(BatchResult::WriteInputEnd(blobseq));
  blobseq.Reset();
  batch_result.ClearAndResize(1);
  EXPECT_TRUE(batch_result.Read(blobseq));

  ASSERT_EQ(batch_result.num_outputs_read(), 1);
  EXPECT_THAT(batch_result.results()[0].features(),
              ElementsAre(feature_domains::kUserDomains[0].ConvertToMe(0),
                          feature_domains::kUserDomains[0].ConvertToMe(1),
                          feature_domains::kUserDomains[15].ConvertToMe(
                              feature_domains::Domain::kDomainSize - 1),
                          feature_domains::kUserDomains[15].ConvertToMe(
                              feature_domains::Domain::kDomainSize - 1)));
}

TEST(ExecutionResult, KeepArbitraryBytesFromMetadata) {
  auto buffer = std::make_unique<uint8_t[]>(1000);
  BlobSequence blobseq(buffer.get(), 1000);
  BatchResult batch_result;

  ByteArray bytes = {13, 14, 15, 16};

  EXPECT_TRUE(BatchResult::WriteInputBegin(blobseq));
  EXPECT_TRUE(BatchResult::WriteMetadata(bytes, blobseq));
  EXPECT_TRUE(BatchResult::WriteInputEnd(blobseq));
  blobseq.Reset();
  batch_result.ClearAndResize(1);
  EXPECT_TRUE(batch_result.Read(blobseq));

  ASSERT_EQ(batch_result.num_outputs_read(), 1);
  EXPECT_EQ(batch_result.results()[0].metadata().cmp_data, bytes);
  // `ForEachEntry()` should fail but not crash.
  EXPECT_FALSE(batch_result.results()[0].metadata().ForEachCmpEntry(
      [](ByteSpan, ByteSpan) {}));
}

}  // namespace
}  // namespace fuzztest::internal
