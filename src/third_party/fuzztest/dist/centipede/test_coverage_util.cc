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

#include "./centipede/test_coverage_util.h"

#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "absl/log/check.h"
#include "./centipede/corpus.h"
#include "./centipede/environment.h"
#include "./centipede/feature.h"
#include "./centipede/runner_result.h"
#include "./centipede/util.h"
#include "./common/defs.h"

namespace fuzztest::internal {

std::vector<CorpusRecord> RunInputsAndCollectCorpusRecords(
    const Environment &env, const std::vector<std::string> &inputs) {
  TestCallbacks CBs(env);
  std::filesystem::create_directories(TemporaryLocalDirPath());

  // Repackage string inputs into ByteArray inputs.
  std::vector<ByteArray> byte_array_inputs;
  byte_array_inputs.reserve(inputs.size());
  for (auto &string_input : inputs) {
    byte_array_inputs.emplace_back(string_input.begin(), string_input.end());
  }
  BatchResult batch_result;
  // Run.
  CBs.Execute(env.binary, byte_array_inputs, batch_result);

  // Repackage execution results into a vector of CorpusRecords.
  std::vector<CorpusRecord> corpus_records;
  std::vector<ExecutionResult> &execution_results = batch_result.results();
  CHECK_EQ(byte_array_inputs.size(), execution_results.size());

  corpus_records.reserve(byte_array_inputs.size());
  for (int i = 0; i < byte_array_inputs.size(); ++i) {
    corpus_records.push_back({/*data=*/byte_array_inputs[i],
                              /*features=*/execution_results[i].features()});
  }
  return corpus_records;
}

std::vector<FeatureVec> RunInputsAndCollectCoverage(
    const Environment &env, const std::vector<std::string> &inputs) {
  std::vector<CorpusRecord> corpus_records =
      RunInputsAndCollectCorpusRecords(env, inputs);

  // Repackage corpus records into a vector of FeatureVec.
  std::vector<FeatureVec> res;
  res.reserve(corpus_records.size());
  for (const auto &record : corpus_records) {
    res.push_back(record.features);
  }
  return res;
}

}  // namespace fuzztest::internal
