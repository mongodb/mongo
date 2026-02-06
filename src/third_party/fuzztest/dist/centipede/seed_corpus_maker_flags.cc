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

#include <string>

#include "absl/flags/flag.h"

ABSL_FLAG(
    std::string, config, "",
    "A silifuzz.ccmp.SeedCorpusConfig proto that describes where and how to "
    "obtain seeding corpus elements. Can be either a verbatim textproto or a "
    "path to a textproto file.\n"
    "`sources.dir_glob`s and `destination.dir_path` can be relative paths: if "
    "so, they will be resolved to absolute ones using either the --config's "
    "parent dir, if --config is a filename, or the current dir otherwise.\n"
    "Furthermore, `destination.dir_path` can be overridden by passing a "
    "non-empty --out_dir.");
ABSL_FLAG(
    std::string, coverage_binary_path, "",
    "The path of the binary from which coverage is to be collected. Can be "
    "just the basename of the binary, but in that case --coverage_binary_hash "
    "must also be provided.");
ABSL_FLAG(
    std::string, coverage_binary_hash, "",
    "If not-empty, this hash is used instead of the actual hash of the "
    "contents of --coverage_binary_path. Use when the binary pointed at by "
    "--coverage_binary_path is not actually available on disk.");
ABSL_FLAG(
    std::string, override_out_dir, "",
    "If non-empty, overrides the `destination.dir_path` field in the resolved "
    "--config protobuf.");
