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

#include "./centipede/crash_summary.h"

#include <utility>

#include "absl/strings/str_format.h"
#include "./centipede/util.h"
#include "./common/defs.h"

namespace fuzztest::internal {
namespace {

ExternalCrashReporter external_crash_reporter = nullptr;

}  // namespace

void CrashSummary::AddCrash(Crash crash) {
  crashes_.push_back(std::move(crash));
}

void CrashSummary::Report(absl::FormatRawSink sink) const {
  if (external_crash_reporter != nullptr) {
    external_crash_reporter(*this);
  }
  absl::Format(sink, "=== Summary of detected crashes ===\n\n");
  absl::Format(sink, "Binary ID    : %s\n", binary_id());
  absl::Format(sink, "Fuzz test    : %s\n", fuzz_test());
  absl::Format(sink, "Total crashes: %d\n\n", crashes().size());
  int i = 0;
  for (const Crash& crash : crashes()) {
    absl::Format(sink, "Crash #%d:\n", ++i);
    absl::Format(sink, "  Crash ID   : %s\n", crash.id);
    absl::Format(sink, "  Category   : %s\n", crash.category);
    absl::Format(sink, "  Signature  : %s\n",
                 AsPrintableString(AsByteSpan(crash.signature), 32));
    absl::Format(sink, "  Description: %s\n\n", crash.description);
  }
  absl::Format(sink, "=== End of summary of detected crashes ===\n\n");
}

void SetExternalCrashReporter(ExternalCrashReporter reporter) {
  external_crash_reporter = reporter;
}

}  // namespace fuzztest::internal
