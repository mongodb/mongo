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

#include "./centipede/config_init.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/log_severity.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage_config.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/strings/match.h"
#include "./centipede/config_util.h"

namespace fuzztest::internal {

RuntimeState::RuntimeState(std::vector<std::string> leftover_argv)
    : leftover_argv_(std::move(leftover_argv)) {}

ABSL_ATTRIBUTE_WEAK std::unique_ptr<RuntimeState> InitRuntime(int argc,
                                                              char* argv[]) {
  // NB: The invocation order below is very important. Do not change.
  // Make `LOG(INFO)` to go to stderr by default. Note that an explicit
  // `--stderrthreshold=N` on the command line will override this.
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
  // Make --help print any flags defined by any Centipede source.
  absl::FlagsUsageConfig usage_config;
  usage_config.contains_help_flags = [](std::string_view filename) {
    return absl::StrContains(filename, "centipede");
  };
  absl::SetFlagsUsageConfig(usage_config);
  // Parse the known flags from the command line.
  std::vector<std::string> leftover_argv =
      CastArgv(absl::ParseCommandLine(argc, argv));
  // Initialize the logging system using the just-parsed log-related flags.
  absl::InitializeLog();

  return std::make_unique<RuntimeState>(leftover_argv);
}

}  // namespace fuzztest::internal
