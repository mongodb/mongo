// Copyright 2023 Google LLC
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

#include "./fuzztest/internal/corpus_database.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "./fuzztest/internal/configuration.h"
#include "./fuzztest/internal/io.h"

namespace fuzztest::internal {
namespace {

std::vector<std::string> GetInputs(
    absl::string_view corpus_path_for_test_binary, absl::string_view test_name,
    absl::string_view subdir) {
  if (corpus_path_for_test_binary.empty()) return {};
  return ListDirectory(
      absl::StrCat(corpus_path_for_test_binary, "/", test_name, "/", subdir));
}

}  // namespace

CorpusDatabase::CorpusDatabase(absl::string_view database_path,
                               absl::string_view binary_identifier,
                               bool use_crashing_inputs)
    : corpus_path_for_test_binary_([=] () -> std::string {
        if (database_path.empty()) return "";
        std::string corpus_path_for_test_binary =
            absl::StrCat(database_path, "/", binary_identifier);
        if (!absl::StartsWith(corpus_path_for_test_binary, "/") &&
            std::getenv("TEST_SRCDIR")) {
          corpus_path_for_test_binary = absl::StrCat(
              std::getenv("TEST_SRCDIR"), "/", corpus_path_for_test_binary);
        }
        return corpus_path_for_test_binary;
      }()),
      use_crashing_inputs_(use_crashing_inputs) {}

CorpusDatabase::CorpusDatabase(const Configuration& configuration)
    : CorpusDatabase(configuration.corpus_database,
                     configuration.binary_identifier,
                     /*use_crashing_inputs=*/
                     configuration.reproduce_findings_as_separate_tests) {}

std::vector<std::string> CorpusDatabase::GetRegressionInputs(
    absl::string_view test_name) const {
  return GetInputs(corpus_path_for_test_binary_, test_name, "regression");
}

std::vector<std::string> CorpusDatabase::GetCrashingInputsIfAny(
    absl::string_view test_name) const {
  if (!use_crashing_inputs_) return {};
  return GetInputs(corpus_path_for_test_binary_, test_name, "crashing");
}

std::vector<std::string> CorpusDatabase::GetCoverageInputsIfAny(
    absl::string_view test_name) const {
  return GetInputs(corpus_path_for_test_binary_, test_name, "coverage");
}

}  // namespace fuzztest::internal
