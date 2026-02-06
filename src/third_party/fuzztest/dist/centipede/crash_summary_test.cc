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

#include <string>
#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"

namespace fuzztest::internal {
namespace {

using ::testing::AllOf;
using ::testing::HasSubstr;
using ::testing::Pointee;

class CrashSummaryTest : public testing::Test {
 public:
  ~CrashSummaryTest() {
    if (dumped_summary_ != nullptr) {
      delete dumped_summary_;
      dumped_summary_ = nullptr;
    }
  }

 protected:
  static void DumpCrashSummary(const CrashSummary& summary) {
    CHECK(dumped_summary_ == nullptr);
    dumped_summary_ = new CrashSummary{summary};
  };

  static CrashSummary* dumped_summary_;
};

CrashSummary* CrashSummaryTest::dumped_summary_ = nullptr;

TEST_F(CrashSummaryTest, ReportPrintsSummary) {
  CrashSummary summary("binary_id", "fuzz_test");
  summary.AddCrash({"id1", "category1", "signature1", "description1"});
  summary.AddCrash({"id2", "category2",
                    "Unprintable (\xbe\xef) and very long signature",
                    "description2"});
  std::string output;
  summary.Report(&output);

  EXPECT_THAT(
      output,
      AllOf(HasSubstr("Binary ID    : binary_id"),
            HasSubstr("Fuzz test    : fuzz_test"),
            HasSubstr("Total crashes: 2"),  //
            HasSubstr("Crash ID   : id1"),  //
            HasSubstr("Category   : category1"),
            HasSubstr("Signature  : signature1"),
            HasSubstr("Description: description1"),
            HasSubstr("Crash ID   : id2"),  //
            HasSubstr("Category   : category2"),
            HasSubstr("Signature  : Unprintable (\\xBE\\xEF) and very long s"),
            HasSubstr("Description: description2")));
}

TEST_F(CrashSummaryTest, ReportCallsExternalCrashReporter) {
  CrashSummary summary("binary_id", "fuzz_test");
  summary.AddCrash({"id1", "category1", "signature1", "description1"});
  summary.AddCrash({"id2", "category2", "signature2", "description2"});
  SetExternalCrashReporter(DumpCrashSummary);
  std::string output;
  summary.Report(&output);

  EXPECT_THAT(dumped_summary_, Pointee(summary));
}

}  // namespace
}  // namespace fuzztest::internal
