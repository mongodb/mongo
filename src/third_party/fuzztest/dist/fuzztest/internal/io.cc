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

#include <cerrno>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/hash/hash.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "./common/blob_file.h"
#include "./common/defs.h"
#include "./common/remote_file.h"
#include "./fuzztest/internal/logging.h"

namespace fuzztest::internal {

#if defined(FUZZTEST_STUB_STD_FILESYSTEM)

// TODO(lszekeres): Return absl::Status instead of bool for these.

bool WriteFile(absl::string_view path, absl::string_view contents) {
  FUZZTEST_INTERNAL_CHECK(false, "Filesystem API not supported in iOS/MacOS");
}

std::optional<std::string> ReadFile(absl::string_view path) {
  FUZZTEST_INTERNAL_CHECK(false, "Filesystem API not supported in iOS/MacOS");
}

bool IsDirectory(absl::string_view path) {
  FUZZTEST_INTERNAL_CHECK(false, "Filesystem API not supported in iOS/MacOS");
}

bool CreateDirectory(absl::string_view path) {
  FUZZTEST_INTERNAL_CHECK(false, "Filesystem API not supported in iOS/MacOS");
}

std::vector<std::string> ListDirectory(absl::string_view path) {
  FUZZTEST_INTERNAL_CHECK(false, "Filesystem API not supported in iOS/MacOS");
}

std::vector<std::string> ListDirectoryRecursively(absl::string_view path) {
  FUZZTEST_INTERNAL_CHECK(false, "Filesystem API not supported in iOS/MacOS");
}

#else

bool WriteFile(absl::string_view path, absl::string_view contents) {
  const std::filesystem::path fs_path{
      std::string_view{path.data(), path.size()}};

  // Just in case the directory does not currently exist.
  // If it does, this is a noop.
  CreateDirectory(fs_path.parent_path().string());

  std::ofstream file(fs_path);
  file << contents;
  file.close();
  if (!file.good()) {
    absl::FPrintF(GetStderr(), "[!] %s:%d: Error writing %s: (%d) %s\n",
                  __FILE__, __LINE__, path, errno, strerror(errno));
  }
  return !file.fail();
}

std::optional<std::string> ReadFile(absl::string_view path) {
  const std::filesystem::path fs_path{
      std::string_view{path.data(), path.size()}};

  std::ifstream stream(fs_path);
  if (!stream.good()) {
    // Using stderr instead of GetStderr() to avoid initialization-order-fiasco
    // when reading files at static init time with
    // `.WithSeeds(fuzztest::ReadFilesFromDirectory(...))`.
    absl::FPrintF(stderr, "[!] %s:%d: Error reading %s: (%d) %s\n", __FILE__,
                  __LINE__, path, errno, strerror(errno));
    return std::nullopt;
  }
  std::stringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

bool IsDirectory(absl::string_view path) {
  const std::filesystem::path fs_path{
      std::string_view{path.data(), path.size()}};

  return std::filesystem::is_directory(fs_path);
}

bool CreateDirectory(absl::string_view path) {
  const std::filesystem::path fs_path{
      std::string_view{path.data(), path.size()}};

  return std::filesystem::create_directories(fs_path);
}

std::vector<std::string> ListDirectory(absl::string_view path) {
  std::vector<std::string> output_paths;

  const std::filesystem::path fs_path{
      std::string_view{path.data(), path.size()}};
  if (!std::filesystem::is_directory(fs_path)) return output_paths;
  for (const auto& entry : std::filesystem::directory_iterator(fs_path)) {
    output_paths.push_back(entry.path().string());
  }
  return output_paths;
}

std::vector<std::string> ListDirectoryRecursively(absl::string_view path) {
  std::vector<std::string> output_paths;

  const std::filesystem::path fs_path{
      std::string_view{path.data(), path.size()}};
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           fs_path,
           std::filesystem::directory_options::follow_directory_symlink)) {
    output_paths.push_back(entry.path().string());
  }
  return output_paths;
}

#endif  // FUZZTEST_STUB_STD_FILESYSTEM

std::string WriteDataToDir(absl::string_view data, absl::string_view outdir) {
  std::string filename(outdir);
  if (filename.back() != '/') filename += '/';
  absl::StrAppendFormat(&filename, "%016x",
                        absl::Hash<absl::string_view>{}(data));
  if (!WriteFile(filename, data)) return "";
  return filename;
}

std::vector<FilePathAndData> ReadFileOrDirectory(
    absl::string_view file_or_dir) {
  std::vector<FilePathAndData> out;

  const auto try_append_file = [&](std::string path) {
    std::optional<std::string> contents = ReadFile(path);
    if (contents.has_value()) {
      out.push_back(FilePathAndData{std::move(path), *std::move(contents)});
    }
  };
  if (IsDirectory(file_or_dir)) {
    for (const auto& path : ListDirectoryRecursively(file_or_dir)) {
      if (!IsDirectory(path)) {
        try_append_file(path);
      }
    }
  } else {
    try_append_file(std::string(file_or_dir));
  }
  return out;
}

absl::string_view Basename(absl::string_view filename) {
  auto last_slash_pos = filename.find_last_of("/\\");

  return last_slash_pos == absl::string_view::npos
             ? filename
             : filename.substr(last_slash_pos + 1);
}

std::vector<std::tuple<std::string>> ReadFilesFromDirectory(
    absl::string_view dir) {
  std::vector<FilePathAndData> files =
      ReadFileOrDirectory({dir.data(), dir.size()});

  std::vector<std::tuple<std::string>> out;
  out.reserve(files.size());

  for (const FilePathAndData& file : files) {
    out.push_back(std::make_tuple(file.data));
  }

  return out;
}

// TODO(b/348702296): Consider merging with `fuzztest::internal::ReadShard()`.
void ForEachSerializedInput(absl::Span<const std::string> file_paths,
                            absl::FunctionRef<absl::Status(
                                absl::string_view file_path,
                                std::optional<int> blob_idx, std::string input)>
                                consume,
                            absl::Duration timeout) {
  int total_loaded_inputs = 0;
  int total_invalid_inputs = 0;
  const absl::Time start_time = absl::Now();
  for (const std::string& file_path : file_paths) {
    FUZZTEST_INTERNAL_CHECK_PRECONDITION(
        fuzztest::internal::RemotePathExists(file_path), "File path ",
        file_path, " does not exist.");
    FUZZTEST_INTERNAL_CHECK_PRECONDITION(
        !fuzztest::internal::RemotePathIsDirectory(file_path), "File path ",
        file_path, " is a directory.");
    int loaded_inputs_from_file = 0;
    int invalid_inputs_from_file = 0;
    // The reader cannot be reused for multiple files because of the way it
    // handles its internal state. So we instantiate a new reader for each file.
    std::unique_ptr<fuzztest::internal::BlobFileReader> reader =
        fuzztest::internal::DefaultBlobFileReaderFactory();
    if (reader->Open(file_path).ok()) {
      fuzztest::internal::ByteSpan blob;
      for (int blob_idx = 0; reader->Read(blob).ok(); ++blob_idx) {
        if (absl::Now() - start_time > timeout) {
          absl::FPrintF(GetStderr(),
                        "[!] Timeout reached while processing input at index "
                        "%d in file %s.\n",
                        blob_idx, file_path);
          break;
        }
        absl::Status result =
            consume(file_path, blob_idx,
                    std::string(fuzztest::internal::AsStringView(blob)));
        if (result.ok()) {
          ++loaded_inputs_from_file;
        } else {
          ++invalid_inputs_from_file;
          absl::FPrintF(GetStderr(),
                        "[!] Invalid input at index %d in file %s: %s\n",
                        blob_idx, file_path, result.message());
        }
      }
    }
    if (absl::Now() - start_time > timeout) {
      absl::FPrintF(GetStderr(),
                    "[!] Timeout reached while processing input %s.\n",
                    file_path);
      break;
    }
    if (loaded_inputs_from_file + invalid_inputs_from_file > 0) {
      // The file was a blob file and we read some inputs from it.
      absl::FPrintF(
          GetStderr(),
          "[*] Loaded %d inputs and ignored %d invalid inputs from %s.\n",
          loaded_inputs_from_file, invalid_inputs_from_file, file_path);
      total_loaded_inputs += loaded_inputs_from_file;
      total_invalid_inputs += invalid_inputs_from_file;
      continue;
    }
    // The file was not a blob file (or, unlikely, it was an empty blob file);
    // read its contents directly.
    // TODO(b/349115475): Currently, we cannot distinguish between an empty blob
    // file and a file that is not a blob file. Once we can, we should not fall
    // back to reading the file directly if it is an empty blob file.
    std::string contents;
    const absl::Status get_contents_status =
        fuzztest::internal::RemoteFileGetContents(file_path, contents);
    FUZZTEST_INTERNAL_CHECK_PRECONDITION(
        get_contents_status.ok(), "RemoteFileGetContents failed on ", file_path,
        ", status: ", get_contents_status.message());
    absl::Status result = consume(file_path, std::nullopt, std::move(contents));
    if (result.ok()) {
      ++total_loaded_inputs;
    } else {
      ++total_invalid_inputs;
      absl::FPrintF(GetStderr(), "[!] Invalid input file %s: %s\n", file_path,
                    result.message());
    }
  }

  // Print stats if we attempted to load something.
  if (total_loaded_inputs != 0 || total_invalid_inputs != 0) {
    absl::FPrintF(
        GetStderr(),
        "[*] In total, loaded %d inputs and ignored %d invalid inputs.\n",
        total_loaded_inputs, total_invalid_inputs);
  }
}

}  // namespace fuzztest::internal
