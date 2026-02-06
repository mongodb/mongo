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

#ifndef THIRD_PARTY_CENTIPEDE_ENVIRONMENT_H_
#define THIRD_PARTY_CENTIPEDE_ENVIRONMENT_H_

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "absl/time/time.h"
#include "./centipede/feature.h"
#include "./centipede/knobs.h"
#include "./fuzztest/internal/configuration.h"

namespace fuzztest::internal {

// Fuzzing environment controlling the behavior of
// CentipedeMain(). Centipede binaries are creating Environment instances using
// the flags defined in environment_flags.cc, while other users can use
// CentipedeMain() as a library function without importing the flags.
struct Environment {
#define CENTIPEDE_FLAG(TYPE, NAME, DEFAULT, _DESC) TYPE NAME = DEFAULT;
#include "./centipede/centipede_flags.inc"
#undef CENTIPEDE_FLAG

  // Command line-related fields -----------------------------------------------

  std::string exec_name;          // copied from argv[0]
  std::vector<std::string> args;  // copied from argv[1:].
  std::string binary_name;        // Name of `coverage_binary`, w/o directories.
  bool has_input_wildcards = false;  // Set to true iff `binary` contains "@@".

  // Experiment-related settings -----------------------------------------------

  std::string experiment_name;   // Set by `UpdateForExperiment`.
  std::string experiment_flags;  // Set by `UpdateForExperiment`.

  // Other ---------------------------------------------------------------------

  Knobs knobs;  // Read from a file by `ReadKnobsFileIfSpecified`, see knobs.h.

  // Defines internal logging level. Set to zero to reduce logging in tests.
  // TODO(ussuri): Retire in favor of VLOGs?
  size_t log_level = 1;

  // Path to a file with PCs. This file is created and the field is set in
  // `CentipedeMain()` once per process if trace_pc instrumentation is detected.
  std::string pcs_file_path;

  // APIs ----------------------------------------------------------------------

  // Returns an instance of the environment with default values.
  static const Environment& Default();

  // Should certain actions be performed ---------------------------------------

  // Returns true if we want to log features as symbols in this shard.
  bool LogFeaturesInThisShard() const {
    return my_shard_index < log_features_shards;
  }
  // Returns true if we want to generate the corpus telemetry files (coverage
  // report, corpus stats, etc.) in this shard.
  bool DumpCorpusTelemetryInThisShard() const;
  // Returns true if we want to generate the resource usage report in this
  // shard. See the related RUsageTelemetryScope().
  bool DumpRUsageTelemetryInThisShard() const;
  // Returns true if we want to generate the telemetry files (coverage report,
  // the corpus stats, etc.) after processing `batch_index`-th batch.
  bool DumpTelemetryForThisBatch(size_t batch_index) const;
  // Returns a bitmask indicating which domains Centipede should discard.
  std::bitset<feature_domains::kNumDomains> MakeDomainDiscardMask() const;

  // Experiment-related functions ----------------------------------------------

  // Updates `this` according to the `--experiment` flag.
  // The `--experiment` flag, if not empty, has this form:
  //   foo=1,2,3:bar=10,20
  // where foo and bar are some of the flag names supported for experimentation,
  // see `SetFlag()`.
  // `--experiment` defines the flag values to be set differently in different
  // shards. E.g. in this case,
  //   shard 0 will have {foo=1,bar=10},
  //   shard 1 will have {foo=1,bar=20},
  //   ...
  //   shard 3 will have {foo=2,bar=10},
  //   ...
  //   shard 5 will have {foo=2,bar=30},
  // and so on.
  //
  // CHECK-fails if the `--experiment` flag is not well-formed,
  // or if num_threads is not a multiple of the number of flag combinations
  // (which is 6 in this example).
  //
  // Sets load_other_shard_frequency=0 (experiments should be independent).
  //
  // Sets this->experiment_name to a string like "E01",
  // which means "value #0 is used for foo and value #1 is used for bar".
  void UpdateForExperiment();

  // Sets flag 'name' to `value` for an experiment. CHECK-fails on
  // invalid name/value combination. Used in `UpdateForExperiment()`.
  void SetFlagForExperiment(std::string_view name, std::string_view value);

  // Other ---------------------------------------------------------------------

  // Reads `knobs` from `knobs_file`. Does nothing if the `knobs_file` is empty.
  void ReadKnobsFileIfSpecified();
  // Updates `this` with `config` obtained from the target binary. CHECK-fails
  // if the fields are non-default and inconsistent with the corresponding
  // values in `config`.
  void UpdateWithTargetConfig(const fuzztest::internal::Configuration& config);
  // If `timeout_per_batch` is `val`, computes it as a function of
  // `timeout_per_input` and `batch_size` and updates it. Otherwise, leaves it
  // unchanged.
  void UpdateTimeoutPerBatchIfEqualTo(size_t val);
  // If `binary_hash` is empty, updates it using the file in `coverage_binary`.
  void UpdateBinaryHashIfEmpty();

  std::vector<std::string> CreateFlags() const;
};

}  // namespace fuzztest::internal

#endif  // THIRD_PARTY_CENTIPEDE_ENVIRONMENT_H_
