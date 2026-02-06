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

#include "./centipede/shared_memory_blob_sequence.h"

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <thread>  // NOLINT: For thread::get_id() only.
#include <vector>

#include "gtest/gtest.h"

namespace fuzztest::internal {

std::string ShmemName() {
  std::ostringstream oss;
  oss << "/shm_test-" << getpid() << "-" << std::this_thread::get_id();
  return oss.str();
}

// Helper: Blob to std::vector.
static std::vector<uint8_t> Vec(Blob blob) {
  return {blob.data, blob.data + blob.size};
}

// Helper: std::vector to Blob.
static Blob BlobFromVec(const std::vector<uint8_t> &vec, uint64_t tag = 1) {
  return {tag, vec.size(), vec.data()};
}

TEST(BlobSequence, WriteAndReadAnEmptyBlob) {
  std::vector<uint8_t> buffer(1000);
  BlobSequence blobseq1(buffer.data(), buffer.size());
  ASSERT_TRUE(blobseq1.Write(BlobFromVec({}, /*tag=*/1)));
  BlobSequence blobseq2(buffer.data(), blobseq1.offset());
  auto blob = blobseq2.Read();
  EXPECT_EQ(Vec(blob).size(), 0);
  EXPECT_EQ(blob.tag, 1);
}

TEST(BlobSequence, WriteReturnErrorOnOverflow) {
  std::vector<uint8_t> buffer(1000);
  BlobSequence blobseq(buffer.data(), buffer.size());
  const std::vector<uint8_t> should_not_be_accessed = {1, 2, 3};
  ASSERT_FALSE(
      blobseq.Write(Blob{/*tag=*/1, /*size=*/std::numeric_limits<size_t>::max(),
                         should_not_be_accessed.data()}));
}

TEST(BlobSequence, ReadReturnErrorOnNotEnoughBytes) {
  std::vector<uint8_t> buffer(100);
  {
    BlobSequence blobseq(buffer.data(), buffer.size());
    ASSERT_TRUE(blobseq.Write(
        BlobFromVec(std::vector<uint8_t>(/*count=*/50, /*value=*/123))));
  }
  BlobSequence truncated_blobseq(buffer.data(), 40);
  ASSERT_FALSE(truncated_blobseq.Read().IsValid());
}

TEST(BlobSequence, ReadReturnErrorOnSizeOverflow) {
  // It should work on all supported platforms, but this is hacky becuase there
  // is no API to inject fake metadata. But it seems not worthy to add a method
  // just for this.
  std::vector<uint8_t> buffer(/*count=*/100, /*value=*/0xff);
  BlobSequence blobseq(buffer.data(), buffer.size());
  ASSERT_FALSE(blobseq.Read().IsValid());
}

class SharedMemoryBlobSequenceTest
    : public testing::TestWithParam</* use_shm */ bool> {
 public:
  void SetUp() override {
#ifdef __APPLE__
    const bool use_shm = GetParam();
    if (!use_shm) {
      GTEST_SKIP() << "Skipping test that does not use POSIX shmem on MacOS";
    }
#endif  // __APPLE__
  }
};

INSTANTIATE_TEST_SUITE_P(SharedMemoryBlobSequenceParametrizedTest,
                         SharedMemoryBlobSequenceTest,
                         testing::Values(true, false));

TEST_P(SharedMemoryBlobSequenceTest, ParentChild) {
  std::vector<uint8_t> kTestData1 = {1, 2, 3};
  std::vector<uint8_t> kTestData2 = {4, 5, 6, 7};
  std::vector<uint8_t> kTestData3 = {8, 9};
  std::vector<uint8_t> kTestData4 = {'a', 'b', 'c', 'd', 'e'};

  SharedMemoryBlobSequence parent(ShmemName().c_str(), 1000, GetParam());
  // Parent writes data.
  EXPECT_TRUE(parent.Write(BlobFromVec(kTestData1, 123)));
  EXPECT_TRUE(parent.Write(BlobFromVec(kTestData2, 456)));

  // Child created.
  SharedMemoryBlobSequence child(parent.path());
  // Child reads data.
  auto blob1 = child.Read();
  EXPECT_EQ(kTestData1, Vec(blob1));
  EXPECT_EQ(blob1.tag, 123);
  auto blob2 = child.Read();
  EXPECT_EQ(kTestData2, Vec(blob2));
  EXPECT_EQ(blob2.tag, 456);
  EXPECT_FALSE(child.Read().IsValid());

  // Child writes data.
  child.Reset();
  EXPECT_TRUE(child.Write(BlobFromVec(kTestData3)));
  EXPECT_TRUE(child.Write(BlobFromVec(kTestData4)));

  // Parent reads data.
  parent.Reset();
  EXPECT_EQ(kTestData3, Vec(parent.Read()));
  EXPECT_EQ(kTestData4, Vec(parent.Read()));
  EXPECT_FALSE(parent.Read().IsValid());
}

TEST_P(SharedMemoryBlobSequenceTest, CheckForResourceLeaks) {
  const int kNumIters = 1 << 17;  // Some large number of iterations.
  const int kBlobSize = 1 << 30;  // Some large blob size.
  // Create and destroy lots of parent/child blob pairs.
  for (int iter = 0; iter < kNumIters; iter++) {
    SharedMemoryBlobSequence parent(ShmemName().c_str(), kBlobSize, GetParam());
    parent.Write(BlobFromVec({1, 2, 3}));
    SharedMemoryBlobSequence child(parent.path());
    EXPECT_EQ(child.Read().size, 3);
  }
  // Create a parent blob, then create and destroy lots of child blobs.
  SharedMemoryBlobSequence parent(ShmemName().c_str(), kBlobSize, GetParam());
  parent.Write(BlobFromVec({1, 2, 3, 4}));
  for (int iter = 0; iter < kNumIters; iter++) {
    SharedMemoryBlobSequence child(parent.path());
    EXPECT_EQ(child.Read().size, 4);
  }
}

// Tests that Read-after-Write or Write-after-Read w/o Reset crashes.
TEST_P(SharedMemoryBlobSequenceTest, ReadVsWriteWithoutReset) {
  SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 1000, GetParam());
  blobseq.Write(BlobFromVec({1, 2, 3}));
  EXPECT_DEATH(blobseq.Read(), "Had writes after reset");
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 3);
  EXPECT_DEATH(blobseq.Write(BlobFromVec({1, 2, 3, 4})),
               "Had reads after reset");
  blobseq.Reset();
  blobseq.Write(BlobFromVec({1, 2, 3, 4}));
}

// Check cases when SharedMemoryBlobSequence is nearly full.
TEST_P(SharedMemoryBlobSequenceTest, WriteToFullSequence) {
  // Can't create SharedMemoryBlobSequence with sizes < 8.
  EXPECT_DEATH(
      SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 7, GetParam()),
      "Size too small");

  // Allocate a blob sequence with 28 bytes of storage.
  SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 28, GetParam());

  // 17 bytes: 8 bytes size, 8 bytes tag, 1 byte payload.
  EXPECT_TRUE(blobseq.Write(BlobFromVec({1})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 1);
  EXPECT_FALSE(blobseq.Read().IsValid());

  // 20 bytes: 4-byte payload.
  blobseq.Reset();
  EXPECT_TRUE(blobseq.Write(BlobFromVec({1, 2, 3, 4})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 4);
  EXPECT_FALSE(blobseq.Read().IsValid());

  // 23 bytes: 7-byte payload.
  blobseq.Reset();
  EXPECT_TRUE(blobseq.Write(BlobFromVec({1, 2, 3, 4, 5, 6, 7})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 7);
  EXPECT_FALSE(blobseq.Read().IsValid());

  // 28 bytes: 12-byte payload.
  blobseq.Reset();
  EXPECT_TRUE(
      blobseq.Write(BlobFromVec({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 12);
  EXPECT_FALSE(blobseq.Read().IsValid());

  // 13-byte payload - there is not enough space (for 13+8 bytes).
  blobseq.Reset();
  EXPECT_FALSE(
      blobseq.Write(BlobFromVec({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 12);  // State remained the same.

  // 1-, and 2- byte payloads. The last one fails.
  blobseq.Reset();
  EXPECT_TRUE(blobseq.Write(BlobFromVec({1})));
  EXPECT_FALSE(blobseq.Write(BlobFromVec({1, 2})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 1);
  EXPECT_FALSE(blobseq.Read().IsValid());
}

// Test Write-Reset-Write-Read scenario.
TEST_P(SharedMemoryBlobSequenceTest, WriteAfterReset) {
  // Allocate a blob sequence with 28 bytes of storage.
  SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 100, GetParam());
  const std::vector<uint8_t> kFirstWriteData(/*count=*/64, /*value=*/255);
  EXPECT_TRUE(blobseq.Write(BlobFromVec(kFirstWriteData)));
  blobseq.Reset();  // The data in shmem is unchanged.
  const std::vector<uint8_t> kSecondWriteData{42, 43};
  EXPECT_TRUE(blobseq.Write(BlobFromVec(kSecondWriteData)));
  blobseq.Reset();  // The data in shmem is unchanged.
  auto blob1 = blobseq.Read();
  EXPECT_TRUE(blob1.IsValid());
  EXPECT_EQ(Vec(blob1), kSecondWriteData);
  auto blob2 = blobseq.Read();  // must be invalid.
  EXPECT_FALSE(blob2.IsValid());
}

// MacOS does not support releasing the shm memory.
#ifndef __APPLE__
// Test ReleaseSharedMemory and NumBytesUsed.
TEST_P(SharedMemoryBlobSequenceTest, ReleaseSharedMemory) {
  // Allocate a blob sequence with 1M bytes of storage.
  SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 1 << 20, GetParam());
  EXPECT_EQ(blobseq.NumBytesUsed(), 0);
  EXPECT_TRUE(blobseq.Write(BlobFromVec({1, 2, 3, 4})));
  EXPECT_GT(blobseq.NumBytesUsed(), 5);
  blobseq.ReleaseSharedMemory();
  EXPECT_EQ(blobseq.NumBytesUsed(), 0);
  EXPECT_TRUE(blobseq.Write(BlobFromVec({1, 2, 3, 4})));
  EXPECT_GT(blobseq.NumBytesUsed(), 5);
}
#endif

}  // namespace fuzztest::internal
