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

#include <cstdlib>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <system_error>  // NOLINT

#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "./common/logging.h"

namespace fuzztest::internal {

std::filesystem::path GetTestTempDir(std::string_view subdir) {
  const std::filesystem::path test_tempdir = ::testing::TempDir();
  CHECK(!test_tempdir.empty())
      << "testing::TempDir() is expected to always return non-empty path";
  const auto dir = test_tempdir / subdir;
  if (!std::filesystem::exists(dir)) {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    CHECK(!error) << "Failed to create dir: " VV(dir) << error.message();
  }
  return std::filesystem::canonical(dir);
}

std::string GetTempFilePath(std::string_view subdir, size_t i) {
  return GetTestTempDir(subdir) / absl::StrCat("tmp.", getpid(), ".", i);
}

std::filesystem::path GetTestRunfilesDir() {
  const auto test_srcdir = ::testing::SrcDir();
  CHECK(!test_srcdir.empty())
      << "testing::SrcDir() is expected to always return non-empty path";
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  CHECK(test_workspace != nullptr)
      << "TEST_WORKSPACE envvar is expected to be set by build system";
  auto path = std::filesystem::path{test_srcdir}.append(test_workspace);
  CHECK(std::filesystem::exists(path))  //
      << "No such dir: " << VV(path) << VV(test_srcdir) << VV(test_workspace);
  return path;
}

std::filesystem::path GetDataDependencyFilepath(std::string_view rel_path) {
  const auto runfiles_dir = GetTestRunfilesDir();
  auto path = runfiles_dir;
  path.append(rel_path);
  CHECK(std::filesystem::exists(path))  //
      << "No such path: " << VV(path) << VV(runfiles_dir) << VV(rel_path);
  return path;
}

std::string GetLLVMSymbolizerPath() {
  CHECK_EQ(system("which llvm-symbolizer"), EXIT_SUCCESS)
      << "llvm-symbolizer has to be installed and findable via PATH";
  return "llvm-symbolizer";
}

std::string GetObjDumpPath() {
  CHECK_EQ(system("which objdump"), EXIT_SUCCESS)
      << "objdump has to be installed and findable via PATH";
  return "objdump";
}

void PrependDirToPathEnvvar(std::string_view dir) {
  const std::string new_path_envvar = absl::StrCat(dir, ":", getenv("PATH"));
  setenv("PATH", new_path_envvar.c_str(), /*replace*/ 1);
  LOG(INFO) << "New PATH: " << new_path_envvar;
}

}  // namespace fuzztest::internal
