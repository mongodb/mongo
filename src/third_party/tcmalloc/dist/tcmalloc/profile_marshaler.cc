// Copyright 2021 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/profile_marshaler.h"

#include <string>

#include "google/protobuf/io/gzip_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "tcmalloc/internal/profile_builder.h"

namespace tcmalloc {

// Marshal converts a Profile instance into a gzip-encoded, serialized
// representation suitable for viewing with PProf
// (https://github.com/google/pprof).
absl::StatusOr<std::string> Marshal(const tcmalloc::Profile& profile) {
  auto converted_or = tcmalloc_internal::MakeProfileProto(profile);
  if (!converted_or.ok()) {
    return converted_or.status();
  }

  std::string output;
  google::protobuf::io::StringOutputStream stream(&output);
  google::protobuf::io::GzipOutputStream gzip_stream(&stream);
  if (!(*converted_or)->SerializeToZeroCopyStream(&gzip_stream)) {
    return absl::InternalError("Failed to serialize to gzip stream");
  }
  if (!gzip_stream.Close()) {
    return absl::InternalError("Failed to serialize to gzip stream");
  }
  return output;
}

}  // namespace tcmalloc
