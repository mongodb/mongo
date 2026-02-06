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

#include <cstdlib>
#include <filesystem>  // NOLINT
#include <string>

#include "absl/base/nullability.h"
#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "./centipede/config_init.h"
#include "./centipede/seed_corpus_maker_flags.h"
#include "./centipede/seed_corpus_maker_proto_lib.h"
#include "./centipede/util.h"
#include "./common/remote_file.h"

int main(int argc, char** absl_nonnull argv) {
  (void)fuzztest::internal::InitRuntime(argc, argv);

  const std::string config = absl::GetFlag(FLAGS_config);
  QCHECK(!config.empty());
  const std::string override_out_dir = absl::GetFlag(FLAGS_override_out_dir);
  const std::string binary_path = absl::GetFlag(FLAGS_coverage_binary_path);
  const std::string binary_name = std::filesystem::path{binary_path}.filename();
  QCHECK(!binary_name.empty())
      << "--coverage_binary_path yields empty basename";
  std::string binary_hash = absl::GetFlag(FLAGS_coverage_binary_hash);
  if (binary_hash.empty()) {
    QCHECK(fuzztest::internal::RemotePathExists(binary_path))
        << "--coverage_binary_path doesn't exist";
    binary_hash = fuzztest::internal::HashOfFileContents(binary_path);
    LOG(INFO) << "--coverage_binary_hash was not provided: computed "
              << binary_hash
              << " from actual file at --coverage_binary_path=" << binary_path;
  }

  QCHECK_OK(fuzztest::internal::GenerateSeedCorpusFromConfigProto(  //
      config, binary_name, binary_hash, override_out_dir));

  return EXIT_SUCCESS;
}
