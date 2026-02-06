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

#include "./common/blob_file.h"

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "./common/defs.h"
#include "./common/test_util.h"

namespace fuzztest::internal {
namespace {

std::string TempFilePath() { return TempDir("test").GetFilePath("blob_file"); }

// We may have more than one BlobFile factory.
// Need to test every factory the same way.
class BlobFile : public testing::TestWithParam<bool> {};

// Tests correct way of using a BlobFile, and that files written in both formats
// can be appropriately detected and read.
void TestOneBlobFile(std::unique_ptr<BlobFileReader> (*ReaderFactory)(),
                     std::unique_ptr<BlobFileWriter> (*WriterFactory)(bool),
                     bool riegeli) {
  ByteArray input1{1, 2, 3};
  ByteArray input2{4, 5};
  ByteArray input3{6, 7, 8, 9};
  ByteArray input4{10, 11};
  const auto path = TempFilePath();
  ByteSpan blob;

  // Append two blobs to a file.
  {
    auto appender = WriterFactory(riegeli);
    EXPECT_OK(appender->Open(path, "a"));
    EXPECT_OK(appender->Write(input1));
    EXPECT_OK(appender->Write(input2));
    EXPECT_OK(appender->Close());
  }

  // Read the blobs back.
  {
    auto reader = ReaderFactory();
    EXPECT_OK(reader->Open(path));
    EXPECT_OK(reader->Read(blob));
    EXPECT_EQ(input1, blob);
    EXPECT_OK(reader->Read(blob));
    EXPECT_EQ(input2, blob);
    EXPECT_EQ(reader->Read(blob), absl::OutOfRangeError("no more blobs"));
    EXPECT_OK(reader->Close());
  }

  // Append one more blob to the same file.
  {
    auto appender = WriterFactory(riegeli);
    EXPECT_OK(appender->Open(path, "a"));
    EXPECT_OK(appender->Write(input3));
    EXPECT_OK(appender->Close());
  }

  // Re-read the file, expect to see all 3 blobs.
  {
    auto reader = ReaderFactory();
    EXPECT_OK(reader->Open(path));
    EXPECT_OK(reader->Read(blob));
    EXPECT_EQ(input1, blob);
    EXPECT_OK(reader->Read(blob));
    EXPECT_EQ(input2, blob);
    EXPECT_OK(reader->Read(blob));
    EXPECT_EQ(input3, blob);
    EXPECT_EQ(reader->Read(blob), absl::OutOfRangeError("no more blobs"));
    EXPECT_OK(reader->Close());
  }

  // Overwrite the contents of the file by a new blob.
  {
    auto appender = WriterFactory(riegeli);
    EXPECT_OK(appender->Open(path, "w"));
    EXPECT_OK(appender->Write(input4));
    EXPECT_OK(appender->Close());
  }

  // Re-read the file, expect to see the single new blob.
  {
    auto reader = ReaderFactory();
    EXPECT_OK(reader->Open(path));
    EXPECT_OK(reader->Read(blob));
    EXPECT_EQ(input4, blob);
    EXPECT_EQ(reader->Read(blob), absl::OutOfRangeError("no more blobs"));
    EXPECT_OK(reader->Close());
  }
}

TEST_P(BlobFile, DefaultTest) {
  TestOneBlobFile(&DefaultBlobFileReaderFactory, &DefaultBlobFileWriterFactory,
                  GetParam());
}

// An Open() failure should not interfere with future proper functioning.
TEST_P(BlobFile, AfterFailedOpenTest) {
  auto reader = DefaultBlobFileReaderFactory();
  auto appender = DefaultBlobFileWriterFactory(GetParam());
  const std::string invalid_path = "/DOES/NOT/EXIST";
  const std::string path = TempFilePath();
  ByteArray input{1, 2, 3};

  // Open failure of writer due to file that cannot be created.
  ASSERT_FALSE(appender->Open(invalid_path, "a").ok());
  // Follow that with opening and writing to a file that is expected to work.
  ASSERT_OK(appender->Open(path, "a"));
  ASSERT_OK(appender->Write(input));
  ASSERT_OK(appender->Close());

  // Open failure of reader due to non-existent file.
  ASSERT_FALSE(reader->Open(invalid_path).ok());
  ByteSpan blob;
  // Follow that with reading the already written file and check that contents
  // are as expected.
  ASSERT_OK(reader->Open(path));
  ASSERT_OK(reader->Read(blob));
  EXPECT_EQ(input, blob);
  EXPECT_EQ(reader->Read(blob), absl::OutOfRangeError("no more blobs"));
  EXPECT_OK(reader->Close());
}

TEST_P(BlobFile, CloseReaderAfterFileRemovalTest) {
  auto appender = DefaultBlobFileWriterFactory(GetParam());
  const std::string path = TempFilePath();
  ByteArray input{1};
  ASSERT_OK(appender->Open(path, "a"));
  ASSERT_OK(appender->Write(input));
  ASSERT_OK(appender->Close());

  auto reader = DefaultBlobFileReaderFactory();
  ByteSpan blob;
  ASSERT_OK(reader->Open(path));
  ASSERT_OK(reader->Read(blob));
  TempFilePath();  // Delete the file at `path`
  EXPECT_OK(reader->Close());
}

// Tests incorrect ways of using a BlobFileReader/BlobFileWriter.
void TestIncorrectUsage(std::unique_ptr<BlobFileReader> (*ReaderFactory)(),
                        std::unique_ptr<BlobFileWriter> (*WriterFactory)(bool),
                        bool riegeli) {
  const std::string invalid_path = "/DOES/NOT/EXIST";
  const auto path = TempFilePath();
  auto reader = ReaderFactory();
  auto appender = WriterFactory(riegeli);

  // Open invalid file path.
  EXPECT_FALSE(reader->Open(invalid_path).ok());
  EXPECT_FALSE(appender->Open(invalid_path, "a").ok());
  ByteSpan blob;

  // Write() on objects that are not in a successfuly open state.
  EXPECT_FALSE(appender->Write(blob).ok());
  EXPECT_OK(appender->Open(path, "a"));
  EXPECT_OK(appender->Close());
  EXPECT_FALSE(appender->Write(blob).ok());

  // Read() on objects that are not in a successfully open state.
  EXPECT_FALSE(reader->Read(blob).ok());
  EXPECT_OK(reader->Open(path));
  EXPECT_OK(reader->Close());
  EXPECT_FALSE(reader->Read(blob).ok());
}

TEST_P(BlobFile, IncorrectUsage) {
  TestIncorrectUsage(&DefaultBlobFileReaderFactory,
                     &DefaultBlobFileWriterFactory, GetParam());
}

#ifndef CENTIPEDE_DISABLE_RIEGELI
INSTANTIATE_TEST_SUITE_P(BlobFileTests, BlobFile, ::testing::Bool());
#else
INSTANTIATE_TEST_SUITE_P(BlobFileTests, BlobFile, ::testing::Values(false));
#endif  // CENTIPEDE_DISABLE_RIEGELI

class ReadMultipleFiles
    : public testing::TestWithParam<std::tuple<bool, bool>> {};

// Single reader object can be used to read multiple files, in the same or
// different formats.
TEST_P(ReadMultipleFiles, SingleObjectMultipleFormats) {
  auto reader = DefaultBlobFileReaderFactory();
  auto [first_riegeli, second_riegeli] = GetParam();
  const auto path = TempFilePath();
  ByteArray file1_blob1{1, 2, 3, 4, 5};
  ByteArray file2_blob1{6, 7};
  ByteArray file2_blob2{8, 9};
  ByteSpan blob;

  // Write in first format and read.
  auto writer = DefaultBlobFileWriterFactory(first_riegeli);
  EXPECT_OK(writer->Open(path, "w"));
  EXPECT_OK(writer->Write(file1_blob1));
  EXPECT_OK(writer->Close());
  EXPECT_OK(reader->Open(path));
  EXPECT_OK(reader->Read(blob));
  EXPECT_EQ(file1_blob1, blob);
  EXPECT_EQ(reader->Read(blob), absl::OutOfRangeError("no more blobs"));
  EXPECT_OK(reader->Close());

  // Write in second format and read.
  writer = DefaultBlobFileWriterFactory(second_riegeli);
  EXPECT_OK(writer->Open(path, "w"));
  EXPECT_OK(writer->Write(file2_blob1));
  EXPECT_OK(writer->Write(file2_blob2));
  EXPECT_OK(writer->Close());
  EXPECT_OK(reader->Open(path));
  EXPECT_OK(reader->Read(blob));
  EXPECT_EQ(file2_blob1, blob);
  EXPECT_OK(reader->Read(blob));
  EXPECT_EQ(file2_blob2, blob);
  EXPECT_EQ(reader->Read(blob), absl::OutOfRangeError("no more blobs"));
  EXPECT_OK(reader->Close());
}

#ifndef CENTIPEDE_DISABLE_RIEGELI
INSTANTIATE_TEST_SUITE_P(DefaultBlobFileReaderTests, ReadMultipleFiles,
                         ::testing::Combine(::testing::Bool(),
                                            ::testing::Bool()));
#else
INSTANTIATE_TEST_SUITE_P(DefaultBlobFileReaderTests, ReadMultipleFiles,
                         ::testing::Values(std::tuple{false, false}));
#endif  // CENTIPEDE_DISABLE_RIEGELI

#ifdef CENTIPEDE_DISABLE_RIEGELI
TEST(WriterFactoryDeathTest, FailWhenBuiltWithoutRiegeli) {
  ASSERT_DEATH(DefaultBlobFileWriterFactory(true), "");
}
#endif  // CENTIPEDE_DISABLE_RIEGELI

void Append(ByteArray &to, const ByteArray &from) {
  to.insert(to.end(), from.begin(), from.end());
}

TEST(UtilTest, AppendFile) {
  ByteArray packed;
  ByteArray a{1, 2, 3};
  ByteArray b{3, 4, 5};
  ByteArray c{111, 112, 113, 114, 115};
  Append(packed, PackBytesForAppendFile(a));
  Append(packed, PackBytesForAppendFile(b));
  Append(packed, PackBytesForAppendFile(c));
  std::vector<ByteArray> unpacked;
  UnpackBytesFromAppendFile(packed, &unpacked);
  EXPECT_EQ(a, unpacked[0]);
  EXPECT_EQ(b, unpacked[1]);
  EXPECT_EQ(c, unpacked[2]);
}

}  // namespace
}  // namespace fuzztest::internal
