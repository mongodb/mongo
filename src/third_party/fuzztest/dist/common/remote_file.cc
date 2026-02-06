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

// Implementation of the function from remote_file.h that don't directly depend
// a specific file system.

#include "./common/remote_file.h"

#include <string>
#include <string_view>

#include "absl/base/nullability.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "./common/defs.h"
#include "./common/logging.h"
#include "./common/status_macros.h"

namespace fuzztest::internal {

absl::Status RemoteFileAppend(RemoteFile *absl_nonnull f,
                              const std::string &contents) {
  ByteArray contents_ba{contents.cbegin(), contents.cend()};
  return RemoteFileAppend(f, contents_ba);
}

absl::Status RemoteFileRead(RemoteFile *absl_nonnull f, std::string &contents) {
  ByteArray contents_ba;
  RETURN_IF_NOT_OK(RemoteFileRead(f, contents_ba));
  contents.assign(contents_ba.cbegin(), contents_ba.cend());
  return absl::OkStatus();
}

absl::Status RemoteFileSetContents(std::string_view path,
                                   const ByteArray &contents) {
  ASSIGN_OR_RETURN_IF_NOT_OK(RemoteFile * file, RemoteFileOpen(path, "w"));
  if (file == nullptr) {
    return absl::UnknownError(
        "RemoteFileOpen returned an OK status but a nullptr RemoteFile*");
  }
  RETURN_IF_NOT_OK(RemoteFileAppend(file, contents));
  return RemoteFileClose(file);
}

absl::Status RemoteFileSetContents(std::string_view path,
                                   const std::string &contents) {
  ASSIGN_OR_RETURN_IF_NOT_OK(RemoteFile * file, RemoteFileOpen(path, "w"));
  if (file == nullptr) {
    return absl::UnknownError(
        "RemoteFileOpen returned an OK status but a nullptr RemoteFile*");
  }
  RETURN_IF_NOT_OK(RemoteFileAppend(file, contents));
  RETURN_IF_NOT_OK(RemoteFileClose(file));
  return absl::OkStatus();
}

absl::Status RemoteFileGetContents(std::string_view path, ByteArray &contents) {
  ASSIGN_OR_RETURN_IF_NOT_OK(RemoteFile * file, RemoteFileOpen(path, "r"));
  if (file == nullptr) {
    return absl::UnknownError(
        "RemoteFileOpen returned an OK status but a nullptr RemoteFile*");
  }
  RETURN_IF_NOT_OK(RemoteFileRead(file, contents));
  return RemoteFileClose(file);
}

absl::Status RemoteFileGetContents(std::string_view path,
                                   std::string &contents) {
  ASSIGN_OR_RETURN_IF_NOT_OK(RemoteFile * file, RemoteFileOpen(path, "r"));
  if (file == nullptr) {
    return absl::UnknownError(
        "RemoteFileOpen returned an OK status but a nullptr RemoteFile*");
  }
  RETURN_IF_NOT_OK(RemoteFileRead(file, contents));
  return RemoteFileClose(file);
}

}  // namespace fuzztest::internal
