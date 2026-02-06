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

#include "./common/remote_file.h"

#include <cstdint>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./common/logging.h"
#include "./common/test_util.h"

namespace fuzztest::internal {
namespace {

namespace fs = std::filesystem;

using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

void CreateFileOrDie(std::string_view path, std::string_view contents = "") {
  std::ofstream f{std::string(path)};
  CHECK(f.good()) << VV(path);
  f << contents;
  CHECK(f.good()) << VV(path);
}

TEST(RemoteFile, GetSize) {
  const fs::path temp_dir{GetTestTempDir(test_info_->name())};
  const std::string file_path = temp_dir / "file_01";
  {
    const std::string file_contents1 = "abcd1234";
    CreateFileOrDie(file_path, file_contents1);
    const absl::StatusOr<int64_t> size = RemoteFileGetSize(file_path);
    ASSERT_TRUE(size.status().ok());
    EXPECT_EQ(*size, file_contents1.size());
  }
  {
    const std::string file_contents2 = "efg567";
    ASSERT_TRUE(RemoteFileSetContents(file_path, file_contents2).ok());
    const absl::StatusOr<int64_t> size = RemoteFileGetSize(file_path);
    EXPECT_TRUE(size.status().ok());
    EXPECT_EQ(*size, file_contents2.size());
  }
}

TEST(RemoteMkdir, CreatesMissingParentDirectories) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());
  const std::string dir_path = temp_dir / "a" / "b" / "c";

  ASSERT_TRUE(RemoteMkdir(dir_path).ok());
  EXPECT_TRUE(fs::exists(dir_path));
}

TEST(RemoteListFiles, DoesNotRecurseIntoSubdirectories) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());

  const std::string file1_path = temp_dir / "file_01";
  CreateFileOrDie(file1_path);
  const fs::path dir1_path = temp_dir / "dir_01";
  fs::create_directories(dir1_path);
  const std::string file2_path = dir1_path / "file_02";
  CreateFileOrDie(file2_path);

  const absl::StatusOr<std::vector<std::string>> files =
      RemoteListFiles(temp_dir.string(), /*recursively=*/false);
  ASSERT_TRUE(files.status().ok());
  EXPECT_THAT(*files, UnorderedElementsAre(file1_path));
}

TEST(RemoteListFiles, ListsFilesInRecursiveDirectories) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());

  const std::string file1_path = temp_dir / "file_01";
  CreateFileOrDie(file1_path);
  const std::string file2_path = temp_dir / "file_02";
  CreateFileOrDie(file2_path);

  const fs::path dir1_path = temp_dir / "dir_01";
  fs::create_directories(dir1_path);
  const std::string file3_path = dir1_path / "file_03";
  CreateFileOrDie(file3_path);

  const absl::StatusOr<std::vector<std::string>> files =
      RemoteListFiles(temp_dir.string(), /*recursively=*/true);
  ASSERT_TRUE(files.status().ok());
  EXPECT_THAT(*files, UnorderedElementsAre(file1_path, file2_path, file3_path));
}

TEST(RemoteListFiles, ReturnsAnEmptyResultWhenNoFilesAreFound) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());
  const absl::StatusOr<std::vector<std::string>> files =
      RemoteListFiles(temp_dir.string(), /*recursively=*/false);
  ASSERT_TRUE(files.status().ok());
  EXPECT_THAT(*files, IsEmpty());
}

TEST(RemoteListFiles, ReturnsASingleFileWhenListingAFile) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());

  const std::string file1_path = temp_dir / "file_01";
  CreateFileOrDie(file1_path);

  const absl::StatusOr<std::vector<std::string>> files =
      RemoteListFiles(temp_dir.string(), /*recursively=*/false);
  ASSERT_TRUE(files.status().ok());
  EXPECT_THAT(*files, UnorderedElementsAre(file1_path));
}

TEST(RemoteListFiles, ReturnsAnEmptyVectorWhenPathDoesNotExist) {
  const absl::StatusOr<std::vector<std::string>> files =
      RemoteListFiles("/this/file/path/does/not/exist", /*recursively=*/false);
  ASSERT_TRUE(files.status().ok());
  EXPECT_THAT(*files, IsEmpty());
}

TEST(RemotePathDelete, RecursivelyDeletesAllFilesAndSubdirectories) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());
  const fs::path a_b_c = temp_dir / "a" / "b" / "c";
  CHECK(fs::create_directories(a_b_c)) << VV(a_b_c);
  const std::string file_path = a_b_c / "file";
  CreateFileOrDie(file_path);

  ASSERT_TRUE(RemotePathDelete(temp_dir.string(), /*recursively=*/true).ok());
  EXPECT_FALSE(fs::exists(a_b_c));
}

TEST(RemotePathTouchExistingFile, FailsWhenPathDoesNotExist) {
  EXPECT_FALSE(RemotePathTouchExistingFile("/non/existent/path").ok());
}

TEST(RemotePathTouchExistingFile, UpdatesTheLastModifiedTime) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());
  const std::string file_path = temp_dir / "file";
  CreateFileOrDie(file_path);
  absl::SleepFor(absl::Milliseconds(10));

  const auto start_time = absl::Now();
  absl::SleepFor(absl::Milliseconds(10));
  ASSERT_TRUE(RemotePathTouchExistingFile(file_path).ok());
  absl::SleepFor(absl::Milliseconds(10));
  const auto end_time = absl::Now();

  struct stat st = {0};
  ASSERT_EQ(lstat(file_path.c_str(), &st), 0);
  auto last_modified_time = absl::TimeFromTimespec(st.st_mtim);
  ASSERT_LT(last_modified_time, end_time);
  EXPECT_GT(last_modified_time, start_time);
}

TEST(RemoteGlobMatch, DoesNotRecurseIntoSubdirectories) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());

  const std::string file1_path = temp_dir / "file_01";
  CreateFileOrDie(file1_path);
  const fs::path dir1_path = temp_dir / "dir_01";
  fs::create_directories(dir1_path);
  const std::string file2_path = dir1_path / "file_02";
  CreateFileOrDie(file2_path);

  std::vector<std::string> files;
  const absl::Status status = RemoteGlobMatch((temp_dir / "*").string(), files);
  if (absl::IsUnimplemented(status)) {
    GTEST_SKIP()
        << "Skipping RemoteGlobMatch() tests since it is unimplemented: "
        << status;
  }
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_THAT(files, UnorderedElementsAre(file1_path, dir1_path));
}

TEST(RemoteGlobMatch, ReturnsSinglePathWhenGlobIsExistingPath) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());

  const std::string file1_path = temp_dir / "file_01";
  CreateFileOrDie(file1_path);

  std::vector<std::string> files;
  const absl::Status status = RemoteGlobMatch(temp_dir.string(), files);
  if (absl::IsUnimplemented(status)) {
    GTEST_SKIP()
        << "Skipping RemoteGlobMatch() tests since it is unimplemented: "
        << status;
  }
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_THAT(files, UnorderedElementsAre(temp_dir.string()));
}

TEST(RemoteGlobMatch, ReturnsNotFoundErrorWithEmptyVectorWhenGlobMatchNothing) {
  const fs::path temp_dir = GetTestTempDir(test_info_->name());
  std::vector<std::string> files;
  const absl::Status status = RemoteGlobMatch((temp_dir / "*").string(), files);
  if (absl::IsUnimplemented(status)) {
    GTEST_SKIP()
        << "Skipping RemoteGlobMatch() tests since it is unimplemented: "
        << status;
  }
  ASSERT_TRUE(absl::IsNotFound(status)) << status;
  EXPECT_THAT(files, IsEmpty());
}

TEST(RemoteGlobMatch, ReturnsNotFoundErrorWithEmptyVectorWhenPathDoesNotExist) {
  std::vector<std::string> files;
  const absl::Status status =
      RemoteGlobMatch("/this/file/path/does/not/exist", files);
  if (absl::IsUnimplemented(status)) {
    GTEST_SKIP()
        << "Skipping RemoteGlobMatch() tests since it is unimplemented: "
        << status;
  }
  ASSERT_TRUE(absl::IsNotFound(status)) << status;
  EXPECT_THAT(files, IsEmpty());
}

}  // namespace
}  // namespace fuzztest::internal
