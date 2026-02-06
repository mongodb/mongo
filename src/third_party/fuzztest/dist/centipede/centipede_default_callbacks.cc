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

#include "./centipede/centipede_default_callbacks.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "./centipede/centipede_callbacks.h"
#include "./centipede/environment.h"
#include "./centipede/mutation_input.h"
#include "./centipede/runner_result.h"
#include "./centipede/stop.h"
#include "./common/defs.h"
#include "./common/logging.h"  // IWYU pragma: keep

namespace fuzztest::internal {

CentipedeDefaultCallbacks::CentipedeDefaultCallbacks(const Environment &env)
    : CentipedeCallbacks(env) {
  for (const auto &dictionary_path : env_.dictionary) {
    LoadDictionary(dictionary_path);
  }

  if (env_.has_input_wildcards) {
    LOG(INFO) << "Disabling custom mutator for standalone target";
    custom_mutator_is_usable_ = false;
  }
}

bool CentipedeDefaultCallbacks::Execute(std::string_view binary,
                                        const std::vector<ByteArray> &inputs,
                                        BatchResult &batch_result) {
  return ExecuteCentipedeSancovBinaryWithShmem(binary, inputs, batch_result) ==
         0;
}

size_t CentipedeDefaultCallbacks::GetSeeds(size_t num_seeds,
                                           std::vector<ByteArray> &seeds) {
  seeds.resize(num_seeds);
  if (GetSeedsViaExternalBinary(env_.binary, num_seeds, seeds)) {
    return num_seeds;
  }
  return CentipedeCallbacks::GetSeeds(num_seeds, seeds);
}

absl::StatusOr<std::string>
CentipedeDefaultCallbacks::GetSerializedTargetConfig() {
  std::string serialized_target_config;
  if (GetSerializedTargetConfigViaExternalBinary(env_.binary,
                                                 serialized_target_config)) {
    return serialized_target_config;
  }
  return absl::InternalError(
      "Failed to get serialized configuration from the target binary.");
}

std::vector<ByteArray> CentipedeDefaultCallbacks::Mutate(
    const std::vector<MutationInputRef> &inputs, size_t num_mutants) {
  if (num_mutants == 0) return {};
  // Try to use the custom mutator if it hasn't been disabled.
  if (custom_mutator_is_usable_.value_or(true)) {
    MutationResult result =
        MutateViaExternalBinary(env_.binary, inputs, num_mutants);
    if (result.exit_code() == EXIT_SUCCESS) {
      if (!custom_mutator_is_usable_.has_value()) {
        custom_mutator_is_usable_ = result.has_custom_mutator();
        if (*custom_mutator_is_usable_) {
          LOG(INFO) << "Custom mutator detected; will use it.";
        } else {
          LOG(INFO) << "Custom mutator not detected; falling back to the "
                       "built-in mutator.";
        }
      }
      if (*custom_mutator_is_usable_) {
        // TODO(b/398261908): Exit with failure instead of crashing.
        CHECK(result.has_custom_mutator())
            << "Test binary no longer has a custom mutator, even though it was "
               "previously detected.";
        if (!result.mutants().empty()) return std::move(result).mutants();
        LOG_FIRST_N(WARNING, 5) << "Custom mutator returned no mutants; will "
                                   "generate some using the built-in mutator.";
      }
    } else if (ShouldStop()) {
      LOG(WARNING) << "Custom mutator failed, but ignored since the stop "
                      "condition it met. Possibly what triggered the stop "
                      "condition also interrupted the mutator.";
      // Returning whatever mutants we got before the failure.
      return std::move(result).mutants();
    } else {
      LOG(ERROR) << "Test binary failed when asked to mutate inputs - exiting.";
      RequestEarlyStop(EXIT_FAILURE);
      return {};
    }
  }
  // Fall back to the internal mutator.
  return CentipedeCallbacks::Mutate(inputs, num_mutants);
}

}  // namespace fuzztest::internal
