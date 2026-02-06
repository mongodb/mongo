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

// Implementation of remote_file.h for the local file system using pure Standard
// Library APIs.

#if !defined(_MSC_VER) && !defined(__ANDROID__) && !defined(__Fuchsia__)
#include <glob.h>
#define FUZZTEST_HAS_OSS_GLOB
#endif  // !defined(_MSC_VER) && !defined(__ANDROID__) && !defined(__Fuchsia__)

#if defined(_MSC_VER)
#include <windows.h>
#endif  // defined(_MSC_VER)

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <string_view>
#include <system_error>  // NOLINT
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "./common/defs.h"
#include "./common/logging.h"
#include "./common/remote_file.h"
#include "./common/status_macros.h"
#ifndef CENTIPEDE_DISABLE_RIEGELI
#include "riegeli/bytes/fd_reader.h"
#include "riegeli/bytes/fd_writer.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/writer.h"
#endif  // CENTIPEDE_DISABLE_RIEGELI

namespace fuzztest::internal {
namespace {

class LocalRemoteFile : public RemoteFile {
 public:
  static absl::StatusOr<LocalRemoteFile *> Create(std::string path,
                                                  std::string_view mode) {
    FILE *file = std::fopen(path.c_str(), mode.data());
    if (file == nullptr) {
      return absl::UnknownError(absl::StrCat(
          "fopen() failed, path: ", path, ", errno: ", std::strerror(errno)));
    }
    return new LocalRemoteFile{std::move(path), file};
  }

  ~LocalRemoteFile() {
    CHECK(file_ == nullptr) << "Dtor called before Close(): " << VV(path_);
  }

  // Movable but not copyable.
  LocalRemoteFile(const LocalRemoteFile &) = delete;
  LocalRemoteFile &operator=(const LocalRemoteFile &) = delete;
  LocalRemoteFile(LocalRemoteFile &&) = default;
  LocalRemoteFile &operator=(LocalRemoteFile &&) = default;

  absl::Status SetWriteBufSize(size_t size) {
    if (write_buf_ != nullptr) {
      return absl::FailedPreconditionError("SetWriteBufCapacity called twice");
    }
    write_buf_ = std::make_unique<char[]>(size);
    if (std::setvbuf(file_, write_buf_.get(), _IOFBF, size) != 0) {
      return absl::UnknownError(
          absl::StrCat("std::setvbuf failed, path: ", path_,
                       ", errno: ", std::strerror(errno)));
    }
    return absl::OkStatus();
  }

  absl::Status Write(const ByteArray &ba) {
    static constexpr auto elt_size = sizeof(ba[0]);
    const auto elts_to_write = ba.size();
    const auto elts_written =
        std::fwrite(ba.data(), elt_size, elts_to_write, file_);
    if (elts_written != elts_to_write) {
      return absl::UnknownError(absl::StrCat(
          "fwrite() wrote less elements that expected, wrote: ", elts_written,
          ", expected: ", elts_to_write, ", path: ", path_));
    }
    return absl::OkStatus();
  }

  absl::Status Flush() {
    if (std::fflush(file_) != 0) {
      return absl::UnknownError("fflush() failed");
    }
    return absl::OkStatus();
  }

  absl::Status Read(ByteArray &ba) {
    // Compute the file size as a difference between the end and start offsets.
    if (std::fseek(file_, 0, SEEK_END), 0 != 0) {
      return absl::UnknownError(absl::StrCat("fseek() failed on path: ", path_,
                                             ": ", std::strerror(errno)));
    }
    const auto file_size = std::ftell(file_);
    if (std::fseek(file_, 0, SEEK_SET), 0) {
      return absl::UnknownError(absl::StrCat("fseek() failed on path: ", path_,
                                             ": ", std::strerror(errno)));
    }
    static constexpr auto elt_size = sizeof(ba[0]);
    CHECK_EQ(file_size % elt_size, 0)
        << VV(file_size) << VV(elt_size) << VV(path_);
    if (file_size % elt_size != 0) {
      return absl::FailedPreconditionError(
          absl::StrCat("Attempting to read a file with inconsistent element (",
                       elt_size, ") and file size (", file_size, "): ", path_));
    }
    const auto elts_to_read = file_size / elt_size;
    ba.resize(elts_to_read);
    const auto elts_read = std::fread(ba.data(), elt_size, elts_to_read, file_);
    if (elts_read != elts_to_read) {
      return absl::UnknownError(absl::StrCat(
          "fread() read less elements that expected, wrote: ", elts_read,
          ", expected: ", elts_to_read, ", path: ", path_));
    }
    return absl::OkStatus();
  }

  absl::Status Close() {
    if (std::fclose(file_) != 0) {
      return absl::UnknownError(absl::StrCat("fclose() failed on path: ", path_,
                                             ": ", std::strerror(errno)));
    }
    file_ = nullptr;
    write_buf_ = nullptr;
    return absl::OkStatus();
  }

 private:
  LocalRemoteFile(std::string path, FILE *file)
      : path_{std::move(path)}, file_{file} {}

  std::string path_;
  FILE *file_;
  std::unique_ptr<char[]> write_buf_;
};

}  // namespace

#if defined(FUZZTEST_STUB_STD_FILESYSTEM)

absl::Status RemoteMkdir(std::string_view path) {
  LOG(FATAL) << "Filesystem API not supported in iOS/MacOS";
}

bool RemotePathExists(std::string_view path) {
  LOG(FATAL) << "Filesystem API not supported in iOS/MacOS";
}

bool RemotePathIsDirectory(std::string_view path) {
  LOG(FATAL) << "Filesystem API not supported in iOS/MacOS";
}

absl::StatusOr<std::vector<std::string>> RemoteListFiles(std::string_view path,
                                                         bool recursively) {
  LOG(FATAL) << "Filesystem API not supported in iOS/MacOS";
}

absl::Status RemoteFileRename(std::string_view from, std::string_view to) {
  LOG(FATAL) << "Filesystem API not supported in iOS/MacOS";
}

absl::Status RemoteFileCopy(std::string_view from, std::string_view to) {
  LOG(FATAL) << "Filesystem API not supported in iOS/MacOS";
}

absl::Status RemotePathTouchExistingFile(std::string_view path) {
  LOG(FATAL) << "Filesystem API not supported in iOS/MacOS";
}

absl::Status RemotePathDelete(std::string_view path, bool recursively) {
  LOG(FATAL) << "Filesystem API not supported in iOS/MacOS";
}

#else

absl::Status RemoteMkdir(std::string_view path) {
  if (path.empty()) {
    return absl::InvalidArgumentError("Unable to RemoteMkdir() an empty path");
  }
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) {
    return absl::UnknownError(
        absl::StrCat("create_directories() failed, path: ", std::string(path),
                     ", error: ", error.message()));
  }
  return absl::OkStatus();
}

bool RemotePathExists(std::string_view path) {
  return std::filesystem::exists(path);
}

bool RemotePathIsDirectory(std::string_view path) {
  return std::filesystem::is_directory(path);
}

absl::StatusOr<std::vector<std::string>> RemoteListFiles(std::string_view path,
                                                         bool recursively) {
  if (!std::filesystem::exists(path)) return std::vector<std::string>();
  auto list_files = [](auto dir_iter) {
    std::vector<std::string> ret;
    for (const auto &entry : dir_iter) {
      if (entry.is_directory()) continue;
      // On Windows, there's no implicit conversion from `std::filesystem::path`
      // to `std::string`.
      ret.push_back(entry.path().string());
    }
    return ret;
  };
  return recursively
             ? list_files(std::filesystem::recursive_directory_iterator(path))
             : list_files(std::filesystem::directory_iterator(path));
}

absl::Status RemoteFileRename(std::string_view from, std::string_view to) {
  std::error_code error;
  std::filesystem::rename(from, to, error);
  if (error) {
    return absl::UnknownError(
        absl::StrCat("filesystem::rename() failed, from: ", std::string(from),
                     ", to: ", std::string(to), ", error: ", error.message()));
  }
  return absl::OkStatus();
}

absl::Status RemoteFileCopy(std::string_view from, std::string_view to) {
  std::error_code error;
  std::filesystem::copy(
      from, to, std::filesystem::copy_options::overwrite_existing, error);
  if (error) {
    return absl::UnknownError(
        absl::StrCat("filesystem::copy() failed, from: ", std::string(from),
                     ", to: ", std::string(to), ", error: ", error.message()));
  }
  return absl::OkStatus();
}

absl::Status RemotePathTouchExistingFile(std::string_view path) {
  if (!RemotePathExists(path)) {
    return absl::InvalidArgumentError(
        absl::StrCat("path: ", std::string(path), " does not exist."));
  }

#if defined(_MSC_VER)
  HANDLE file = CreateFileA(path.data(), GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    return absl::InternalError(absl::StrCat("Failed to open ", path, "."));
  }
  SYSTEMTIME st;
  FILETIME mtime;
  GetSystemTime(&st);
  SystemTimeToFileTime(&st, &mtime);
  if (SetFileTime(file, nullptr, nullptr, &mtime)) {
    return absl::InternalError(absl::StrCat("Failed to set mtime for ", path));
  }
  CloseHandle(file);
#else
  if (0 != utimes(path.data(), nullptr)) {
    return absl::InternalError(absl::StrCat("Failed to set mtime for ", path,
                                            " (errno ", errno, ")."));
  }
#endif
  return absl::OkStatus();
}

absl::Status RemotePathDelete(std::string_view path, bool recursively) {
  std::error_code error;
  if (recursively) {
    std::filesystem::remove_all(path, error);
  } else {
    std::filesystem::remove(path, error);
  }
  if (error) {
    return absl::UnknownError(
        absl::StrCat("filesystem::remove() or remove_all() failed, path: ",
                     std::string(path), ", error: ", error.message()));
  }
  return absl::OkStatus();
}

#endif  // defined(FUZZTEST_STUB_STD_FILESYSTEM)

// TODO(ussuri): For now, simulate the old behavior, where a failure to open
//  a file returned nullptr. Adjust the clients to expect non-null and use a
//  normal ctor with a CHECK instead of `Create()` here instead.
absl::StatusOr<RemoteFile *> RemoteFileOpen(std::string_view path,
                                            const char *mode) {
  return LocalRemoteFile::Create(std::string(path), mode);
}

absl::Status RemoteFileClose(RemoteFile *absl_nonnull f) {
  auto *file = static_cast<LocalRemoteFile *>(f);
  RETURN_IF_NOT_OK(file->Close());
  delete file;
  return absl::OkStatus();
}

absl::Status RemoteFileSetWriteBufferSize(RemoteFile *absl_nonnull f,
                                          size_t size) {
  return static_cast<LocalRemoteFile *>(f)->SetWriteBufSize(size);
}

absl::Status RemoteFileAppend(RemoteFile *absl_nonnull f, const ByteArray &ba) {
  return static_cast<LocalRemoteFile *>(f)->Write(ba);
}

absl::Status RemoteFileFlush(RemoteFile *absl_nonnull f) {
  return static_cast<LocalRemoteFile *>(f)->Flush();
}

absl::Status RemoteFileRead(RemoteFile *absl_nonnull f, ByteArray &ba) {
  return static_cast<LocalRemoteFile *>(f)->Read(ba);
}

absl::StatusOr<int64_t> RemoteFileGetSize(std::string_view path) {
  FILE *f = std::fopen(path.data(), "r");
  if (f == nullptr) {
    return absl::UnknownError(
        absl::StrCat("fopen() failed, path: ", std::string(path),
                     ", errno: ", std::strerror(errno)));
  }
  if (std::fseek(f, 0, SEEK_END) != 0) {
    return absl::UnknownError(
        absl::StrCat("fseek() failed, path: ", std::string(path),
                     ", errno: ", std::strerror(errno)));
  }
  const auto sz = std::ftell(f);
  if (sz == -1L) {
    return absl::UnknownError(
        absl::StrCat("ftell() failed, path: ", std::string(path),
                     ", errno: ", std::strerror(errno)));
  }
  std::fclose(f);
  return sz;
}

namespace {

#if defined(FUZZTEST_HAS_OSS_GLOB)
int HandleGlobError(const char *epath, int eerrno) {
  if (eerrno == ENOENT) return 0;
  LOG(FATAL) << "Error while globbing path: " << VV(epath) << VV(eerrno);
  return -1;
}
#endif  // defined(FUZZTEST_HAS_OSS_GLOB)

}  // namespace

absl::Status RemoteGlobMatch(std::string_view glob,
                             std::vector<std::string> &matches) {
#if defined(FUZZTEST_HAS_OSS_GLOB)
  // See `man glob.3`.
  ::glob_t glob_ret = {};
  if (int ret = ::glob(std::string{glob}.c_str(), GLOB_TILDE, HandleGlobError,
                       &glob_ret);
      ret != 0) {
    if (ret == GLOB_NOMATCH) {
      return absl::NotFoundError(absl::StrCat(
          "glob() returned NOMATCH for pattern: ", std::string(glob)));
    }
    return absl::UnknownError(absl::StrCat(
        "glob() failed, pattern: ", std::string(glob), ", returned: ", ret));
  }
  for (int i = 0; i < glob_ret.gl_pathc; ++i) {
    matches.emplace_back(glob_ret.gl_pathv[i]);
  }
  ::globfree(&glob_ret);
  return absl::OkStatus();
#else
  return absl::UnimplementedError(
      absl::StrCat(__func__, "() is not supported on this platform"));
#endif  // defined(FUZZTEST_HAS_OSS_GLOB)
}

#ifndef CENTIPEDE_DISABLE_RIEGELI
absl::StatusOr<std::unique_ptr<riegeli::Reader>> CreateRiegeliFileReader(
    std::string_view file_path) {
  auto ret = std::make_unique<riegeli::FdReader<>>(file_path);
  RETURN_IF_NOT_OK(ret->status());
  return ret;
}

absl::StatusOr<std::unique_ptr<riegeli::Writer>> CreateRiegeliFileWriter(
    std::string_view file_path, bool append) {
  auto ret = std::make_unique<riegeli::FdWriter<>>(
      file_path, riegeli::FdWriterBase::Options().set_append(append));
  RETURN_IF_NOT_OK(ret->status());
  return ret;
}
#endif  // CENTIPEDE_DISABLE_RIEGELI

}  // namespace fuzztest::internal
