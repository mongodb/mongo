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

#include "./centipede/coverage.h"

#include <stdio.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "./centipede/binary_info.h"
#include "./centipede/control_flow.h"
#include "./centipede/environment.h"
#include "./centipede/feature.h"
#include "./centipede/pc_info.h"
#include "./centipede/symbol_table.h"
#include "./centipede/test_coverage_util.h"
#include "./centipede/thread_pool.h"
#include "./centipede/util.h"
#include "./common/test_util.h"

namespace fuzztest::internal {
namespace {

// llvm-symbolizer output for a binary with 3 functions:
// A, BB, CCC.
// A and BB have one control flow edge each.
// CCC has 3 edges.
const char *symbolizer_output =
    "A\n"
    "a.cc:1:0\n"
    "\n"
    "BB\n"
    "bb.cc:1:0\n"
    "\n"
    "CCC\n"
    "ccc.cc:1:0\n"
    "\n"
    "CCC\n"
    "ccc.cc:2:0\n"
    "\n"
    "CCC\n"
    "ccc.cc:3:0\n"
    "\n"
    "CCC\n"
    "ccc.cc:3:0\n"  // same as the previous entry
    "\n";

// PCTable that corresponds to symbolizer_output above.
static const PCTable g_pc_table = {
    {100, PCInfo::kFuncEntry},
    {200, PCInfo::kFuncEntry},
    {300, PCInfo::kFuncEntry},
    {400, 0},
    {500, 0},
    {600, 0},
};

// Tests Coverage and SymbolTable together.
TEST(Coverage, SymbolTable) {
  const std::filesystem::path test_dir = GetTestTempDir(test_info_->name());

  // Initialize and test SymbolTable.
  SymbolTable symbols;
  std::istringstream iss(symbolizer_output);
  symbols.ReadFromLLVMSymbolizer(iss);
  EXPECT_EQ(symbols.size(), 6U);
  EXPECT_EQ(symbols.func(1), "BB");
  EXPECT_EQ(symbols.location(2), "ccc.cc:1:0");
  EXPECT_EQ(symbols.full_description(0), "A a.cc:1:0");
  EXPECT_EQ(symbols.full_description(4), "CCC ccc.cc:3:0");

  {
    // Tests coverage output for PCIndexVec = {0, 2},
    // i.e. the covered edges are 'A' and the entry of 'CCC'.
    Coverage cov(g_pc_table, {0, 2});
    cov.DumpReportToFile(symbols, (test_dir / "coverage.txt").string());
    std::string str;
    ReadFromLocalFile((test_dir / "coverage.txt").string(), str);
    EXPECT_THAT(str, testing::HasSubstr("FULL: A a.cc:1:0"));
    EXPECT_THAT(str, testing::HasSubstr("NONE: BB bb.cc:1:0"));
    EXPECT_THAT(str, testing::HasSubstr("PARTIAL: CCC ccc.cc:1:0"));
    EXPECT_THAT(str, testing::HasSubstr("+ CCC ccc.cc:1:0"));
    EXPECT_THAT(str, testing::HasSubstr("- CCC ccc.cc:2:0"));
    EXPECT_THAT(str, testing::HasSubstr("- CCC ccc.cc:3:0"));
  }
  {
    // Same as above, but for PCIndexVec = {1, 2, 3},
    Coverage cov(g_pc_table, {1, 2, 3});
    cov.DumpReportToFile(symbols, (test_dir / "coverage.txt").string());
    std::string str;
    ReadFromLocalFile((test_dir / "coverage.txt").string(), str);
    EXPECT_THAT(str, testing::HasSubstr("FULL: BB bb.cc:1:0"));
    EXPECT_THAT(str, testing::HasSubstr("NONE: A a.cc:1:0"));
    EXPECT_THAT(str, testing::HasSubstr("PARTIAL: CCC ccc.cc:1:0"));
    EXPECT_THAT(str, testing::HasSubstr("+ CCC ccc.cc:1:0"));
    EXPECT_THAT(str, testing::HasSubstr("+ CCC ccc.cc:2:0"));
    EXPECT_THAT(str, testing::HasSubstr("- CCC ccc.cc:3:0"));
  }

  symbols.SetAllToUnknown(2);
  EXPECT_EQ(symbols.size(), 2);
  EXPECT_EQ(symbols.full_description(0), "? ?");
  EXPECT_EQ(symbols.full_description(1), "? ?");
}

TEST(Coverage, CoverageLoad) {
  Coverage cov(g_pc_table, {0, 2, 4, 5});

  EXPECT_TRUE(cov.BlockIsCovered(0));
  EXPECT_FALSE(cov.BlockIsCovered(1));
  EXPECT_TRUE(cov.BlockIsCovered(2));
  EXPECT_FALSE(cov.BlockIsCovered(3));
  EXPECT_TRUE(cov.BlockIsCovered(4));
  EXPECT_TRUE(cov.BlockIsCovered(5));

  EXPECT_TRUE(cov.FunctionIsFullyCovered(0));
  EXPECT_FALSE(cov.FunctionIsFullyCovered(1));
  EXPECT_FALSE(cov.FunctionIsFullyCovered(2));
}

TEST(Coverage, CoverageLogger) {
  SymbolTable symbols;
  std::istringstream iss(symbolizer_output);
  symbols.ReadFromLLVMSymbolizer(iss);
  CoverageLogger logger(g_pc_table, symbols);
  // First time logging pc_index=0.
  EXPECT_EQ(logger.ObserveAndDescribeIfNew(0), "FUNC: A a.cc:1:0");
  // Second time logger pc_index=0.
  EXPECT_EQ(logger.ObserveAndDescribeIfNew(0), "");
  // First time logging pc_index=4.
  EXPECT_EQ(logger.ObserveAndDescribeIfNew(4), "EDGE: CCC ccc.cc:3:0");
  // First time logging pc_index=5, but it produces the same description as
  // pc_index=4, and so the result is empty.
  EXPECT_EQ(logger.ObserveAndDescribeIfNew(5), "");

  // Logging with pc_index out of bounds. Second time gives empty result.
  EXPECT_EQ(logger.ObserveAndDescribeIfNew(42), "FUNC/EDGE index: 42");
  EXPECT_EQ(logger.ObserveAndDescribeIfNew(42), "");

  CoverageLogger concurrently_used_logger(g_pc_table, symbols);
  auto cb = [&]() {
    for (int i = 0; i < 1000; i++) {
      PCIndex pc_index = i % g_pc_table.size();
      logger.ObserveAndDescribeIfNew(pc_index);
    }
  };
  {
    ThreadPool threads{2};
    threads.Schedule(cb);
    threads.Schedule(cb);
  }  // The threads join here.
}

// Returns path to test_fuzz_target.
static std::string GetTargetPath() {
  return GetDataDependencyFilepath("centipede/testing/test_fuzz_target");
}

// Returns path to threaded_fuzz_target.
static std::string GetThreadedTargetPath() {
  return GetDataDependencyFilepath("centipede/testing/threaded_fuzz_target");
}

// Tests coverage collection on test_fuzz_target
// using two inputs that trigger different code paths.
TEST(Coverage, CoverageFeatures) {
  // Prepare the inputs.
  Environment env;
  env.binary = GetTargetPath();
  auto features = RunInputsAndCollectCoverage(env, {"func1", "func2-A"});
  EXPECT_EQ(features.size(), 2);
  EXPECT_NE(features[0], features[1]);
  // Get pc_table and symbols.
  bool uses_legacy_trace_pc_instrumentation = {};
  BinaryInfo binary_info;
  binary_info.InitializeFromSanCovBinary(
      GetTargetPath(), GetObjDumpPath(), GetLLVMSymbolizerPath(),
      GetTestTempDir(test_info_->name()).string());
  const auto &pc_table = binary_info.pc_table;
  EXPECT_FALSE(uses_legacy_trace_pc_instrumentation);
  const SymbolTable &symbols = binary_info.symbols;
  // pc_table and symbols should have the same size.
  EXPECT_EQ(pc_table.size(), symbols.size());
  // Check what's covered.
  // Both inputs should cover LLVMFuzzerTestOneInput.
  // Input[0] should cover SingleEdgeFunc and not MultiEdgeFunc.
  // Input[1] - the other way around.
  for (size_t input_idx = 0; input_idx < 2; input_idx++) {
    size_t llvm_fuzzer_test_one_input_num_edges = 0;
    size_t single_edge_func_num_edges = 0;
    size_t multi_edge_func_num_edges = 0;
    for (auto feature : features[input_idx]) {
      if (!feature_domains::kPCs.Contains(feature)) continue;
      auto pc_index = ConvertPCFeatureToPcIndex(feature);
      single_edge_func_num_edges += symbols.func(pc_index) == "SingleEdgeFunc";
      multi_edge_func_num_edges += symbols.func(pc_index) == "MultiEdgeFunc";
      llvm_fuzzer_test_one_input_num_edges +=
          symbols.func(pc_index) == "LLVMFuzzerTestOneInput";
    }
    EXPECT_GT(llvm_fuzzer_test_one_input_num_edges, 1);
    if (input_idx == 0) {
      // This input calls SingleEdgeFunc, but not MultiEdgeFunc.
      EXPECT_EQ(single_edge_func_num_edges, 1);
      EXPECT_EQ(multi_edge_func_num_edges, 0);
    } else {
      // This input calls MultiEdgeFunc, but not SingleEdgeFunc.
      EXPECT_EQ(single_edge_func_num_edges, 0);
      EXPECT_GT(multi_edge_func_num_edges, 1);
    }
  }
}

static FeatureVec ExtractDomainFeatures(const FeatureVec &features,
                                        const feature_domains::Domain &domain) {
  FeatureVec result;
  for (auto feature : features) {
    if (domain.Contains(feature)) {
      result.push_back(feature);
    }
  }
  return result;
}

// Tests data flow instrumentation and feature collection.
TEST(Coverage, DataFlowFeatures) {
  Environment env;
  env.binary = GetTargetPath();
  auto features_g = RunInputsAndCollectCoverage(env, {"glob1", "glob2"});
  auto features_c = RunInputsAndCollectCoverage(env, {"cons1", "cons2"});
  for (auto &features : {features_g, features_c}) {
    EXPECT_EQ(features.size(), 2);
    // Dataflow features should be different.
    EXPECT_NE(ExtractDomainFeatures(features[0], feature_domains::kDataFlow),
              ExtractDomainFeatures(features[1], feature_domains::kDataFlow));
    // But control flow features should be the same.
    EXPECT_EQ(
        ExtractDomainFeatures(features[0], feature_domains::k8bitCounters),
        ExtractDomainFeatures(features[1], feature_domains::k8bitCounters));
  }
}

// Tests feature collection for counters (--use_counter_features).
TEST(Coverage, CounterFeatures) {
  Environment env;
  env.binary = GetTargetPath();

  // Inputs that generate the same PC coverage but different counters.
  std::vector<std::string> inputs = {"cnt\x01", "cnt\x02", "cnt\x04", "cnt\x08",
                                     "cnt\x10"};
  const size_t n = inputs.size();

  // Run with use_counter_features = true.
  env.use_counter_features = true;
  auto features = RunInputsAndCollectCoverage(env, inputs);
  EXPECT_EQ(features.size(), n);
  // Counter features should be different.
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      EXPECT_NE(
          ExtractDomainFeatures(features[i], feature_domains::k8bitCounters),
          ExtractDomainFeatures(features[j], feature_domains::k8bitCounters));
    }
  }

  // Run with use_counter_features = false.
  env.use_counter_features = false;
  features = RunInputsAndCollectCoverage(env, inputs);
  EXPECT_EQ(features.size(), n);
  // Counter features should be the same now.
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      EXPECT_EQ(
          ExtractDomainFeatures(features[i], feature_domains::k8bitCounters),
          ExtractDomainFeatures(features[j], feature_domains::k8bitCounters));
    }
  }
}

// For each of {ABToCmpModDiff, ABToCmpHamming, ABToCmpDiffLog} verify that
// a) they create all possible values in [0,64)
// b) they don't create any other values.
// c) they are sufficiently different from each other, i.e. not using one of
//    them as coverage signal may reduce the overall quality of signal.
TEST(Coverage, CMPFeatures) {
  absl::flat_hash_set<uintptr_t> moddiff, hamming, difflog;

  // clear all hash sets.
  auto clear = [&]() {
    moddiff.clear();
    hamming.clear();
    difflog.clear();
  };

  // verifies `value` < 64 and returns it.
  auto must_be_6bit = [](uintptr_t value) {
    EXPECT_LT(value, 64);
    return value;
  };

  // inserts a value into all hash sets.
  auto update = [&](uintptr_t a, uintptr_t b) {
    moddiff.insert(must_be_6bit(ABToCmpModDiff(a, b)));
    hamming.insert(must_be_6bit(ABToCmpHamming(a, b)));
    difflog.insert(must_be_6bit(ABToCmpDiffLog(a, b)));
  };

  // Check moddiff.
  clear();
  for (uintptr_t a = 0; a <= 64; ++a) {
    uintptr_t b = 32;
    if (a == b) continue;
    update(a, b);
  }
  EXPECT_EQ(moddiff.size(), 64);
  EXPECT_EQ(hamming.size(), 6);
  EXPECT_EQ(difflog.size(), 6);

  // Check hamming.
  clear();
  for (uintptr_t bits = 0; bits < 64; ++bits) {
    uintptr_t minus_one = -1;
    uintptr_t a = minus_one << bits;
    update(a, 0);
  }
  EXPECT_EQ(moddiff.size(), 6);
  EXPECT_EQ(hamming.size(), 64);
  EXPECT_EQ(difflog.size(), 1);

  // Check difflog.
  clear();
  for (uintptr_t bits = 0; bits < 64; ++bits) {
    uintptr_t a = 1ULL << bits;
    uintptr_t b = 0;
    update(a, b);
  }
  EXPECT_EQ(moddiff.size(), 7);
  EXPECT_EQ(hamming.size(), 1);
  EXPECT_EQ(difflog.size(), 64);
}

// Tests CMP tracing and feature collection.
TEST(Coverage, CMPFeaturesExecute) {
  Environment env;
  env.binary = GetTargetPath();
  auto features =
      RunInputsAndCollectCoverage(env, {"cmpAAAAAAAA", "cmpAAAABBBB"});
  EXPECT_EQ(features.size(), 2);
  // CMP features should be different.
  EXPECT_NE(ExtractDomainFeatures(features[0], feature_domains::kCMPEq),
            ExtractDomainFeatures(features[1], feature_domains::kCMPEq));
  EXPECT_NE(ExtractDomainFeatures(features[0], feature_domains::kCMPModDiff),
            ExtractDomainFeatures(features[1], feature_domains::kCMPModDiff));
  EXPECT_NE(ExtractDomainFeatures(features[0], feature_domains::kCMPHamming),
            ExtractDomainFeatures(features[1], feature_domains::kCMPHamming));
  EXPECT_NE(ExtractDomainFeatures(features[0], feature_domains::kCMPDiffLog),
            ExtractDomainFeatures(features[1], feature_domains::kCMPDiffLog));

  // But control flow features should be the same.
  EXPECT_EQ(ExtractDomainFeatures(features[0], feature_domains::k8bitCounters),
            ExtractDomainFeatures(features[1], feature_domains::k8bitCounters));
}

// Tests memcmp interceptor.
TEST(Coverage, CMPFeaturesFromMemcmp) {
  Environment env;
  env.binary = GetTargetPath();
  auto features =
      RunInputsAndCollectCoverage(env, {"mcmpAAAAAAAA", "mcmpAAAABBBB"});
  EXPECT_EQ(features.size(), 2);
  // CMP features should be different.
  EXPECT_NE(ExtractDomainFeatures(features[0], feature_domains::kCMP),
            ExtractDomainFeatures(features[1], feature_domains::kCMP));
  // But control flow features should be the same.
  EXPECT_EQ(ExtractDomainFeatures(features[0], feature_domains::k8bitCounters),
            ExtractDomainFeatures(features[1], feature_domains::k8bitCounters));
}

TEST(Coverage, PathFeatures) {
  Environment env;
  env.binary = GetTargetPath();
  env.path_level = 10;
  // Inputs "pth123" and "pth321" generate different call sequences but exactly
  // the same edge coverage. This test verifies that we can capture this.
  auto features = RunInputsAndCollectCoverage(env, {"pth123", "pth321"});
  EXPECT_EQ(features.size(), 2);
  // Path features should be different.
  EXPECT_NE(ExtractDomainFeatures(features[0], feature_domains::kBoundedPath),
            ExtractDomainFeatures(features[1], feature_domains::kBoundedPath));
  // But control flow features should be the same.
  EXPECT_EQ(ExtractDomainFeatures(features[0], feature_domains::k8bitCounters),
            ExtractDomainFeatures(features[1], feature_domains::k8bitCounters));
}

TEST(Coverage, FunctionFilter) {
  // Initialize coverage data.
  BinaryInfo binary_info;
  binary_info.InitializeFromSanCovBinary(
      GetTargetPath(), GetObjDumpPath(), GetLLVMSymbolizerPath(),
      GetTestTempDir(test_info_->name()).string());

  const PCTable &pc_table = binary_info.pc_table;
  EXPECT_FALSE(binary_info.uses_legacy_trace_pc_instrumentation);
  const DsoTable dso_table = {{GetTargetPath(), pc_table.size()}};
  SymbolTable symbols;
  symbols.GetSymbolsFromBinary(pc_table, dso_table, GetLLVMSymbolizerPath(),
                               GetTestTempDir(test_info_->name()).string());
  // Empty filter.
  FunctionFilter empty_filter("", symbols);
  EXPECT_EQ(empty_filter.count(), 0);

  // Single-function filter. The function has one PC.
  FunctionFilter sing_edge_func_filter("SingleEdgeFunc", symbols);
  EXPECT_EQ(sing_edge_func_filter.count(), 1);

  // Another single-function filter. This function has several PCs.
  FunctionFilter multi_edge_func_filter("MultiEdgeFunc", symbols);
  EXPECT_GT(multi_edge_func_filter.count(), 1);

  // Two-function-filter.
  FunctionFilter both_func_filter("MultiEdgeFunc,SingleEdgeFunc", symbols);
  EXPECT_GT(both_func_filter.count(), multi_edge_func_filter.count());

  // Collect features from the test target by running 3 different inputs.
  Environment env;
  env.binary = GetTargetPath();
  std::vector<FeatureVec> features =
      RunInputsAndCollectCoverage(env, {"func1", "func2-A", "other"});
  EXPECT_EQ(features.size(), 3);
  auto &single = features[0];
  auto &multi = features[1];
  auto &other = features[2];

  // Check the features against the different filters.
  EXPECT_TRUE(empty_filter.filter(single));
  EXPECT_TRUE(empty_filter.filter(multi));
  EXPECT_TRUE(empty_filter.filter(other));

  EXPECT_TRUE(sing_edge_func_filter.filter(single));
  EXPECT_FALSE(sing_edge_func_filter.filter(multi));
  EXPECT_FALSE(sing_edge_func_filter.filter(other));

  EXPECT_FALSE(multi_edge_func_filter.filter(single));
  EXPECT_TRUE(multi_edge_func_filter.filter(multi));
  EXPECT_FALSE(multi_edge_func_filter.filter(other));

  EXPECT_TRUE(both_func_filter.filter(single));
  EXPECT_TRUE(both_func_filter.filter(multi));
  EXPECT_FALSE(both_func_filter.filter(other));
}

TEST(Coverage, ThreadedTest) {
  Environment env;
  env.path_level = 10;
  env.binary = GetThreadedTargetPath();

  std::vector<FeatureVec> features =
      RunInputsAndCollectCoverage(env, {"f", "fu", "fuz", "fuzz"});
  EXPECT_EQ(features.size(), 4);
  // For several pairs of inputs, check that their features in
  // kPC and kBoundedPath are different.
  for (size_t idx0 = 0; idx0 < 3; ++idx0) {
    for (size_t idx1 = idx0 + 1; idx1 < 4; ++idx1) {
      EXPECT_NE(ExtractDomainFeatures(features[idx0], feature_domains::kPCs),
                ExtractDomainFeatures(features[idx1],
                                      feature_domains::k8bitCounters));
      EXPECT_NE(
          ExtractDomainFeatures(features[idx0], feature_domains::kBoundedPath),
          ExtractDomainFeatures(features[idx1], feature_domains::kBoundedPath));
    }
  }
}

TEST(FrontierWeight, ComputeFrontierWeight) {
  PCTable g_pc_table{{0, PCInfo::kFuncEntry},
                     {1, PCInfo::kFuncEntry},
                     {2, 0},
                     {3, PCInfo::kFuncEntry},
                     {4, PCInfo::kFuncEntry}};
  // A simple CF table, to get cyclomatic complexity of 1 for all functions.
  CFTable g_cf_table{
      0, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0, 0, 4, 0, 0,
  };

  Coverage g_coverage(g_pc_table, {0, 1});
  ControlFlowGraph cfg;
  cfg.InitializeControlFlowGraph(g_cf_table, g_pc_table);

  std::vector<uintptr_t> callees1 = {0, 1, 3, 4};
  std::vector<uintptr_t> callees2 = {0, 1};
  std::vector<uintptr_t> callees3 = {0};
  // PC 99 should have no effect on computed weight.
  std::vector<uintptr_t> callees4 = {1, 3, 99};

  auto weight1 = ComputeFrontierWeight(g_coverage, cfg, callees1);
  ASSERT_EQ(weight1, 408);

  auto weight2 = ComputeFrontierWeight(g_coverage, cfg, callees2);
  ASSERT_EQ(weight2, 102);

  auto weight3 = ComputeFrontierWeight(g_coverage, cfg, callees3);
  ASSERT_EQ(weight3, 25);

  auto weight4 = ComputeFrontierWeight(g_coverage, cfg, callees4);
  ASSERT_EQ(weight4, 230);
}

TEST(FrontierWeightDeath, InvalidCallee) {
  // Makes call to ComputeFrontierWeight with some non-function PCs.
  PCTable g_pc_table{{0, PCInfo::kFuncEntry}, {1, 0}, {2, 0}};
  CFTable g_cf_table{0, 1, 0, 0, 1, 2, 0, 0, 2, 0, 0};
  Coverage g_coverage(g_pc_table, {0, 1});
  ControlFlowGraph cfg;
  cfg.InitializeControlFlowGraph(g_cf_table, g_pc_table);
  EXPECT_DEATH(ComputeFrontierWeight(g_coverage, cfg, {0, 1}), "");
  EXPECT_DEATH(ComputeFrontierWeight(g_coverage, cfg, {1, 2}), "");
}

}  // namespace
}  // namespace fuzztest::internal
