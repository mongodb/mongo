// Copyright 2025 The Centipede Authors.
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

#ifndef FUZZTEST_CENTIPEDE_CRASH_SUMMARY_H_
#define FUZZTEST_CENTIPEDE_CRASH_SUMMARY_H_

#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/types/span.h"

namespace fuzztest::internal {

// Accumulates crashes for a single fuzz test and provides a method to report a
// summary of the crashes.
class CrashSummary {
 public:
  struct Crash {
    std::string id;
    std::string category;
    std::string signature;
    std::string description;

    friend bool operator==(const Crash& lhs, const Crash& rhs) {
      return lhs.id == rhs.id && lhs.category == rhs.category &&
             lhs.signature == rhs.signature &&
             lhs.description == rhs.description;
    }
  };

  explicit CrashSummary(std::string_view binary_id, std::string_view fuzz_test)
      : binary_id_(std::string(binary_id)),
        fuzz_test_(std::string(fuzz_test)) {}

  CrashSummary(const CrashSummary&) = default;
  CrashSummary& operator=(const CrashSummary&) = default;
  CrashSummary(CrashSummary&&) = default;
  CrashSummary& operator=(CrashSummary&&) = default;

  // Adds a crash to the summary.
  void AddCrash(Crash crash);

  // Reports a summary of the crashes to `sink`.
  // If an external crash reporter has been set with `SetExternalCrashReporter`,
  // calls it with the stored crashes.
  void Report(absl::FormatRawSink sink) const;

  std::string_view binary_id() const { return binary_id_; }
  std::string_view fuzz_test() const { return fuzz_test_; }
  absl::Span<const Crash> crashes() const { return crashes_; }

  friend bool operator==(const CrashSummary& lhs, const CrashSummary& rhs) {
    return lhs.binary_id_ == rhs.binary_id_ &&
           lhs.fuzz_test_ == rhs.fuzz_test_ && lhs.crashes_ == rhs.crashes_;
  }

 private:
  std::string binary_id_;
  std::string fuzz_test_;
  std::vector<Crash> crashes_;
};

using ExternalCrashReporter = void (*)(const CrashSummary&);

// Sets an external crash reporter that will be called when a `CrashSummary` is
// reported.
void SetExternalCrashReporter(ExternalCrashReporter reporter);

}  // namespace fuzztest::internal

#endif  // FUZZTEST_CENTIPEDE_CRASH_SUMMARY_H_
