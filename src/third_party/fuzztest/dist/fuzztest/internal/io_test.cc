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

#include "./fuzztest/internal/io.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./common/blob_file.h"
#include "./common/defs.h"
#include "./common/temp_dir.h"
#include "./fuzztest/fuzztest_core.h"

namespace fuzztest::internal {
namespace {

using ::testing::Eq;
using ::testing::FieldsAre;
using ::testing::IsEmpty;
using ::testing::Optional;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

std::string TmpFile(const std::string& name) {
  std::string filename = absl::StrCat(testing::TempDir(), "/", name, "XXXXXX");
  return mktemp(filename.data());
}

// These use a different implementation of read/write.
// Otherwise we are testing the functions with themselves.
std::string TestRead(const std::string& filename) {
  FILE* f = fopen(filename.c_str(), "r");
  char buf[1024]{};
  size_t s = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  return std::string(buf, s);
}

void TestWrite(const std::string& filename, const std::string& contents) {
  FILE* f = fopen(filename.c_str(), "w");
  ASSERT_TRUE(f) << strerror(errno);
  EXPECT_EQ(fwrite(contents.data(), contents.size(), 1, f), 1)
      << strerror(errno);
  EXPECT_EQ(0, fclose(f)) << strerror(errno);
}

TEST(IOTest, WriteFileWorksWhenDirectoryExists) {
  const std::string tmp_name = TmpFile("write_test");
  EXPECT_TRUE(WriteFile(tmp_name, "Payload1"));
  EXPECT_EQ(TestRead(tmp_name), "Payload1");
  std::filesystem::remove(tmp_name);
}

TEST(IOTest, WriteFileWorksWhenDirectoryDoesNotExist) {
  TempDir tmp_dir;
  const std::string tmp_name = tmp_dir.path() / "doesnt_exist" / "file";
  EXPECT_TRUE(WriteFile(tmp_name, "Payload1"));
  EXPECT_EQ(TestRead(tmp_name), "Payload1");
}

TEST(IOTest, WriteDataToDirReturnsWrittenFilePath) {
  TempDir tmp_dir;
  const std::string path = WriteDataToDir("data", tmp_dir.path().c_str());
  EXPECT_THAT(ReadFile(path), Optional(Eq("data")));
}

TEST(IOTest, WriteDataToDirWritesToSameFileOnSameData) {
  TempDir tmp_dir;
  const std::string path = WriteDataToDir("data", tmp_dir.path().c_str());
  EXPECT_THAT(WriteDataToDir("data", tmp_dir.path().c_str()), Eq(path));
  EXPECT_THAT(ReadFile(path), Optional(Eq("data")));
}

TEST(IOTest, ReadFileReturnsNulloptWhenMissing) {
  EXPECT_THAT(ReadFile("/doesnt_exist/file"), Eq(std::nullopt));
  EXPECT_THAT(ReadFileOrDirectory("/doesnt_exist/file"),
              UnorderedElementsAre());
}

TEST(IOTest, ReadFileWorksWhenFileExists) {
  const std::string tmp_name = TmpFile("read_test");
  TestWrite(tmp_name, "Payload2");
  EXPECT_THAT(ReadFile(tmp_name), Optional(Eq("Payload2")));
  EXPECT_THAT(ReadFileOrDirectory(tmp_name),
              UnorderedElementsAre(FieldsAre(tmp_name, "Payload2")));
  std::filesystem::remove(tmp_name);
}

TEST(IOTest, ReadFileOrDirectoryWorks) {
  TempDir tmp_dir;
  EXPECT_THAT(ReadFileOrDirectory(tmp_dir.path().c_str()),
              UnorderedElementsAre());
  const std::string tmp_file_1 = tmp_dir.path() / "file1";
  TestWrite(tmp_file_1, "Payload3.1");
  EXPECT_THAT(ReadFileOrDirectory(tmp_dir.path().c_str()),
              UnorderedElementsAre(FieldsAre(tmp_file_1, "Payload3.1")));
  const std::string tmp_file_2 = tmp_dir.path() / "file2";
  TestWrite(tmp_file_2, "Payload3.2");
  EXPECT_THAT(ReadFileOrDirectory(tmp_dir.path().c_str()),
              UnorderedElementsAre(FieldsAre(tmp_file_1, "Payload3.1"),
                                   FieldsAre(tmp_file_2, "Payload3.2")));
}

TEST(IOTest, ReadFileOrDirectoryWorksRecursively) {
  TempDir tmp_dir;
  const std::string tmp_sub_dir = tmp_dir.path() / "subdir";
  mkdir(tmp_sub_dir.c_str(), 0700);
  const std::string tmp_file_1 = tmp_dir.path() / "file1";
  TestWrite(tmp_file_1, "Payload5.1");
  const std::string tmp_file_2 = absl::StrCat(tmp_sub_dir, "/file2");
  TestWrite(tmp_file_2, "Payload5.2");
  EXPECT_THAT(ReadFileOrDirectory(tmp_dir.path().c_str()),
              UnorderedElementsAre(FieldsAre(tmp_file_1, "Payload5.1"),
                                   FieldsAre(tmp_file_2, "Payload5.2")));
}

TEST(IOTest, ReadFilesFromDirectoryWorks) {
  TempDir tmp_dir;
  EXPECT_THAT(ReadFilesFromDirectory(tmp_dir.path().c_str()),
              UnorderedElementsAre());
  EXPECT_THAT(ReadFilesFromDirectory(tmp_dir.path().c_str()), SizeIs(0));
  const std::string tmp_file_1 = tmp_dir.path() / "file1";
  TestWrite(tmp_file_1, "Payload4.1");
  EXPECT_THAT(ReadFilesFromDirectory(tmp_dir.path().c_str()),
              UnorderedElementsAre(FieldsAre("Payload4.1")));
  EXPECT_THAT(ReadFilesFromDirectory(tmp_dir.path().c_str()), SizeIs(1));
  const std::string tmp_file_2 = tmp_dir.path() / "file2";
  TestWrite(tmp_file_2, "Payload4.2");
  EXPECT_THAT(
      ReadFilesFromDirectory(tmp_dir.path().c_str()),
      UnorderedElementsAre(FieldsAre("Payload4.1"), FieldsAre("Payload4.2")));
  EXPECT_THAT(ReadFilesFromDirectory(tmp_dir.path().c_str()), SizeIs(2));
}

TEST(IOTest, ReadFilesFromDirectoryReturnsEmptyVectorWhenNoFilesInDir) {
  TempDir tmp_dir;
  EXPECT_THAT(ReadFilesFromDirectory(tmp_dir.path().c_str()),
              UnorderedElementsAre());
  EXPECT_THAT(ReadFileOrDirectory(tmp_dir.path().c_str()), SizeIs(0));
}

TEST(IOTest, ReadFilesFromDirectoryReturnsEmptyVectorWhenMissing) {
  EXPECT_THAT(ReadFilesFromDirectory("/doesnt_exist/"), UnorderedElementsAre());
  EXPECT_THAT(ReadFileOrDirectory("/doesnt_exist/"), SizeIs(0));
}

TEST(IOTest, ListDirectoryReturnsPathsInDirectory) {
  TempDir tmp_dir;
  const std::string tmp_file_1 = tmp_dir.path() / "file1";
  TestWrite(tmp_file_1, /*contents=*/"File1");
  const std::string tmp_file_2 = tmp_dir.path() / "file2";
  TestWrite(tmp_file_2, /*contents=*/"File2");
  EXPECT_THAT(ListDirectory(tmp_dir.path().c_str()),
              UnorderedElementsAre(tmp_file_1, tmp_file_2));
}

TEST(IOTest, ListDirectoryReturnsEmptyVectorWhenDirectoryIsEmpty) {
  TempDir tmp_dir;
  EXPECT_THAT(ListDirectory(tmp_dir.path().c_str()), IsEmpty());
}

TEST(IOTest, ListDirectoryReturnsEmptyVectorWhenDirectoryDoesNotExist) {
  EXPECT_THAT(ListDirectory("/doesnt_exist/"), IsEmpty());
}

TEST(ForEachSerializedInputTest, ReadsInputsFromSerializedFilesAndBlobFiles) {
  TempDir tmp_dir;
  const std::string serialized_file = tmp_dir.path() / "serialized_file";
  const std::string blob_file = tmp_dir.path() / "blob_file";
  TestWrite(serialized_file, "Input1");
  std::unique_ptr<fuzztest::internal::BlobFileWriter> writer =
      fuzztest::internal::DefaultBlobFileWriterFactory();
  CHECK(writer->Open(blob_file, "w").ok());
  CHECK(
      writer->Write(fuzztest::internal::AsByteSpan(absl::string_view("Input2")))
          .ok());
  CHECK(
      writer->Write(fuzztest::internal::AsByteSpan(absl::string_view("Input3")))
          .ok());
  CHECK(writer->Close().ok());

  using InputInFile = std::tuple<std::string, std::optional<int>, std::string>;
  std::vector<InputInFile> inputs;
  ForEachSerializedInput({serialized_file, blob_file},
                         [&](absl::string_view file_path,
                             std::optional<int> blob_idx, std::string input) {
                           inputs.emplace_back(std::string(file_path), blob_idx,
                                               std::move(input));
                           return absl::OkStatus();
                         });
  EXPECT_THAT(inputs, UnorderedElementsAre(
                          InputInFile{serialized_file, std::nullopt, "Input1"},
                          InputInFile{blob_file, 0, "Input2"},
                          InputInFile{blob_file, 1, "Input3"}));
}

TEST(ForEachSerializedInputTest, IgnoresUnconsumedInputs) {
  TempDir tmp_dir;
  const std::string file = tmp_dir.path() / "file";
  std::unique_ptr<fuzztest::internal::BlobFileWriter> writer =
      fuzztest::internal::DefaultBlobFileWriterFactory();
  CHECK(writer->Open(file, "w").ok());
  CHECK(
      writer->Write(fuzztest::internal::AsByteSpan(absl::string_view("Ignore")))
          .ok());
  CHECK(
      writer->Write(fuzztest::internal::AsByteSpan(absl::string_view("Accept")))
          .ok());
  CHECK(writer->Close().ok());

  using InputInFile = std::tuple<std::string, std::optional<int>, std::string>;
  std::vector<InputInFile> inputs;
  ForEachSerializedInput(
      {file}, [&](absl::string_view file_path, std::optional<int> blob_idx,
                  std::string input) {
        if (input == "Ignore") return absl::InvalidArgumentError("Ignore");
        inputs.emplace_back(std::string(file_path), blob_idx, std::move(input));
        return absl::OkStatus();
      });
  EXPECT_THAT(inputs, UnorderedElementsAre(InputInFile{file, 1, "Accept"}));
}

TEST(ForEachSerializedInputTest, ReadsInputsUntilTimeout) {
  TempDir tmp_dir;
  std::vector<std::string> serialized_files;
  constexpr int kInputsNum = 1000;
  constexpr absl::Duration kTimeout = absl::Seconds(1);
  for (int i = 0; i < kInputsNum; ++i) {
    serialized_files.push_back(tmp_dir.path() /
                               absl::StrCat("serialized_file", i));
    TestWrite(serialized_files.back(), absl::StrCat("Input", i));
  }
  int inputs_read = 0;
  ForEachSerializedInput(
      serialized_files,
      [&inputs_read, kTimeout](absl::string_view file_path,
                               std::optional<int> blob_idx, std::string input) {
        absl::SleepFor(kTimeout / kInputsNum);
        ++inputs_read;
        return absl::OkStatus();
      },
      kTimeout);
  EXPECT_LT(inputs_read, kInputsNum);
}

TEST(ForEachSerializedInputTest, DiesOnDirectoriesInFilePaths) {
  TempDir tmp_dir;
  const std::string dir = tmp_dir.path() / "dir";
  std::filesystem::create_directory(dir);

  EXPECT_DEATH(ForEachSerializedInput(
                   {dir}, [&](absl::string_view, std::optional<int>,
                              std::string) { return absl::OkStatus(); }),
               "is a directory");
}

TEST(ForEachSerializedInputTest, DiesOnNonExistingFilePaths) {
  EXPECT_DEATH(
      ForEachSerializedInput({"/doesnt_exist/file"},
                             [&](absl::string_view, std::optional<int>,
                                 std::string) { return absl::OkStatus(); }),
      "does not exist");
}

}  // namespace
}  // namespace fuzztest::internal
