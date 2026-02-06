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

#include "./centipede/control_flow.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "./centipede/binary_info.h"
#include "./centipede/pc_info.h"
#include "./centipede/symbol_table.h"
#include "./centipede/thread_pool.h"
#include "./common/logging.h"
#include "./common/test_util.h"

namespace fuzztest::internal {

// Mock CFTable representing the following cfg:
//    1
//  /   \
// 2     3
//  \   /
//    4
// TODO(ussuri): Change PCs to 100, 200 etc, to avoid confusion with PCIndex.
static const CFTable g_cf_table = {1, 2, 3, 0, 0, 2, 4, 0,
                                   0, 3, 4, 0, 0, 4, 0, 0};
static const PCTable g_pc_table = {
    {1, PCInfo::kFuncEntry}, {2, 0}, {3, 0}, {4, 0}};

TEST(ControlFlowGraph, ComputeReachabilityForPc) {
  ControlFlowGraph cfg;
  cfg.InitializeControlFlowGraph(g_cf_table, g_pc_table);
  EXPECT_NE(cfg.size(), 0);

  auto reach1 = cfg.ComputeReachabilityForPc(1);
  auto reach2 = cfg.ComputeReachabilityForPc(2);
  auto reach3 = cfg.ComputeReachabilityForPc(3);
  auto reach4 = cfg.ComputeReachabilityForPc(4);

  EXPECT_THAT(reach1, testing::UnorderedElementsAre(1, 2, 3, 4));
  EXPECT_THAT(reach2, testing::UnorderedElementsAre(2, 4));
  EXPECT_THAT(reach3, testing::UnorderedElementsAre(3, 4));
  EXPECT_THAT(reach4, testing::ElementsAre(4));
}

namespace {

TEST(CFTable, MakeCfgFromCfTable) {
  ControlFlowGraph cfg;
  cfg.InitializeControlFlowGraph(g_cf_table, g_pc_table);
  EXPECT_NE(cfg.size(), 0);

  for (auto &pc : {1, 2, 3, 4}) {
    SCOPED_TRACE(testing::Message() << VV(pc));
    EXPECT_TRUE(cfg.exists(pc));

    // Check that cfg traversal is possible.
    auto successors = cfg.GetSuccessors(pc);
    for (auto &successor : successors) {
      EXPECT_TRUE(cfg.exists(successor));
    }

    EXPECT_THAT(cfg.GetSuccessors(1).size(), 2);
    EXPECT_THAT(cfg.GetSuccessors(2).size(), 1);
    EXPECT_THAT(cfg.GetSuccessors(3).size(), 1);
    EXPECT_TRUE(cfg.GetSuccessors(4).empty());
  }

  CHECK_EQ(cfg.GetPcIndex(1), 0);
  CHECK_EQ(cfg.GetPcIndex(2), 1);
  CHECK_EQ(cfg.GetPcIndex(3), 2);
  CHECK_EQ(cfg.GetPcIndex(4), 3);

  EXPECT_TRUE(cfg.BlockIsFunctionEntry(0));
  EXPECT_FALSE(cfg.BlockIsFunctionEntry(1));
  EXPECT_FALSE(cfg.BlockIsFunctionEntry(2));
  EXPECT_FALSE(cfg.BlockIsFunctionEntry(3));

  CHECK_EQ(cfg.GetCyclomaticComplexity(1), 2);
}

TEST(CFTable, SerializesAndDeserializesCfTable) {
  std::stringstream stream;
  WriteCfTable(g_cf_table, stream);
  const CFTable cf_table = ReadCfTable(stream);
  EXPECT_EQ(cf_table, g_cf_table);
}

TEST(FunctionComplexity, ComputeFuncComplexity) {
  static const CFTable g_cf_table1 = {
      1, 2, 3, 0, 0,  // 1 goes to 2 and 3.
      2, 3, 4, 0, 0,  // 2 goes to 3 and 4.
      3, 1, 4, 0, 0,  // 3 goes to 1 and 4.
      4, 0, 0         // 4 goes nowhere.
  };
  static const CFTable g_cf_table2 = {
      1, 0, 0,  // 1 goes nowhere.
  };
  static const CFTable g_cf_table3 = {
      1, 2, 0, 0,  // 1 goes to 2.
      2, 3, 0, 0,  // 2 goes to 3.
      3, 1, 0, 0,  // 3 goes to 1.
  };
  static const CFTable g_cf_table4 = {
      1, 2, 3, 0, 0,  // 1 goes to 2 and 3.
      2, 3, 4, 0, 0,  // 2 goes to 3 and 4.
      3, 0, 0,        // 3 goes nowhere.
      4, 0, 0         // 4 goes nowhere.
  };

  ControlFlowGraph cfg1;
  cfg1.InitializeControlFlowGraph(g_cf_table1, g_pc_table);
  EXPECT_NE(cfg1.size(), 0);

  ControlFlowGraph cfg2;
  cfg2.InitializeControlFlowGraph(g_cf_table2, g_pc_table);
  EXPECT_NE(cfg2.size(), 0);

  ControlFlowGraph cfg3;
  cfg3.InitializeControlFlowGraph(g_cf_table3, g_pc_table);
  EXPECT_NE(cfg3.size(), 0);

  ControlFlowGraph cfg4;
  cfg4.InitializeControlFlowGraph(g_cf_table4, g_pc_table);
  EXPECT_NE(cfg4.size(), 0);

  EXPECT_EQ(ComputeFunctionCyclomaticComplexity(1, cfg1), 4);
  EXPECT_EQ(ComputeFunctionCyclomaticComplexity(1, cfg2), 1);
  EXPECT_EQ(ComputeFunctionCyclomaticComplexity(1, cfg3), 2);
  EXPECT_EQ(ComputeFunctionCyclomaticComplexity(1, cfg4), 2);
}

TEST(ControlFlowGraph, LazyReachability) {
  ControlFlowGraph cfg;
  cfg.InitializeControlFlowGraph(g_cf_table, g_pc_table);
  EXPECT_NE(cfg.size(), 0);

  auto rt = [&cfg]() {
    for (int i = 0; i < 10; ++i) {
      cfg.LazyGetReachabilityForPc(1);
      cfg.LazyGetReachabilityForPc(2);
      cfg.LazyGetReachabilityForPc(3);
      cfg.LazyGetReachabilityForPc(4);
    }
    const auto &reach1 = cfg.LazyGetReachabilityForPc(1);
    const auto &reach2 = cfg.LazyGetReachabilityForPc(2);
    const auto &reach3 = cfg.LazyGetReachabilityForPc(3);
    const auto &reach4 = cfg.LazyGetReachabilityForPc(4);

    EXPECT_THAT(reach1, testing::UnorderedElementsAre(1, 2, 3, 4));
    EXPECT_THAT(reach2, testing::UnorderedElementsAre(2, 4));
    EXPECT_THAT(reach3, testing::UnorderedElementsAre(3, 4));
    EXPECT_THAT(reach4, testing::ElementsAre(4));
  };

  {
    ThreadPool threads{3};
    threads.Schedule(rt);
    threads.Schedule(rt);
    threads.Schedule(rt);
  }  // The threads join here.
}

// Returns path to test_fuzz_target.
static std::string GetTargetPath() {
  return GetDataDependencyFilepath("centipede/testing/test_fuzz_target");
}

// Returns path to test_fuzz_target_trace_pc.
static std::string GetTracePCTargetPath() {
  return GetDataDependencyFilepath(
      "centipede/testing/test_fuzz_target_trace_pc");
}

// Tests GetCfTableFromBinary() on test_fuzz_target.
TEST(CFTable, GetCfTable) {
  auto target_path = GetTargetPath();
  std::string tmp_path1 = GetTempFilePath(test_info_->name(), 1);
  std::string tmp_path2 = GetTempFilePath(test_info_->name(), 2);

  // Load the cf table.
  BinaryInfo binary_info;
  binary_info.InitializeFromSanCovBinary(
      target_path, GetObjDumpPath(), GetLLVMSymbolizerPath(),
      GetTestTempDir(test_info_->name()).string());
  const auto &cf_table = binary_info.cf_table;
  LOG(INFO) << VV(target_path) << VV(tmp_path1) << VV(cf_table.size());
  if (cf_table.empty()) {
    LOG(INFO) << "__sancov_cfs is empty.";
    // TODO(ussuri): This should be removed once OSS clang supports
    //  control-flow.
    GTEST_SKIP();
  }

  ASSERT_FALSE(
      std::filesystem::exists(tmp_path1.c_str()));  // tmp_path1 was deleted.
  LOG(INFO) << VV(cf_table.size());

  const auto &pc_table = binary_info.pc_table;
  EXPECT_FALSE(binary_info.uses_legacy_trace_pc_instrumentation);
  EXPECT_THAT(pc_table.empty(), false);

  const SymbolTable &symbols = binary_info.symbols;

  absl::flat_hash_map<uintptr_t, size_t> pc_table_index;
  for (size_t i = 0; i < pc_table.size(); i++) {
    pc_table_index[pc_table[i].pc] = i;
  }

  for (size_t j = 0; j < cf_table.size();) {
    auto current_pc = cf_table[j];
    ++j;
    size_t successor_num = 0;
    size_t callee_num = 0;
    size_t icallee_num = 0;

    // Iterate over successors.
    while (cf_table[j]) {
      ++successor_num;
      ++j;
    }
    ++j;  // Step over the delimiter.

    // Iterate over callees.
    while (cf_table[j]) {
      if (cf_table[j] > 0) ++callee_num;
      if (cf_table[j] < 0) ++icallee_num;
      ++j;
    }
    ++j;  // Step over the delimiter.

    // Determine if current_pc is a function entry.
    if (pc_table_index.contains(current_pc)) {
      size_t index = pc_table_index[current_pc];
      if (pc_table[index].has_flag(PCInfo::kFuncEntry)) {
        const std::string_view current_function = symbols.func(index);
        // Check for properties.
        SCOPED_TRACE(testing::Message()
                     << "Checking for " << VV(current_function)
                     << VV(current_pc));
        if (current_function == "SingleEdgeFunc") {
          EXPECT_EQ(successor_num, 0);
          EXPECT_EQ(icallee_num, 0);
          EXPECT_EQ(callee_num, 0);
        } else if (current_function == "MultiEdgeFunc") {
          EXPECT_EQ(successor_num, 2);
          EXPECT_EQ(icallee_num, 0);
          EXPECT_EQ(callee_num, 0);
        } else if (current_function == "IndirectCallFunc") {
          EXPECT_EQ(successor_num, 0);
          EXPECT_EQ(icallee_num, 1);
          EXPECT_EQ(callee_num, 0);
        }
      }
    }
  }
}

static void SymbolizeBinary(std::string_view test_dir,
                            std::string_view target_path, bool use_trace_pc) {
  BinaryInfo binary_info;
  binary_info.InitializeFromSanCovBinary(target_path, GetObjDumpPath(),
                                         GetLLVMSymbolizerPath(), test_dir);
  // Load the pc table.
  const auto &pc_table = binary_info.pc_table;
  // Check that it's not empty.
  EXPECT_NE(pc_table.size(), 0);
  // Check that the first PCInfo corresponds to a kFuncEntry.
  EXPECT_TRUE(pc_table[0].has_flag(PCInfo::kFuncEntry));

  // Test the symbols.
  const SymbolTable &symbols = binary_info.symbols;
  ASSERT_EQ(symbols.size(), pc_table.size());

  bool has_llvm_fuzzer_test_one_input = false;
  size_t single_edge_func_num_edges = 0;
  size_t multi_edge_func_num_edges = 0;
  // Iterate all symbols, verify that we:
  //  * Don't have main (coverage instrumentation is disabled for main).
  //  * Have LLVMFuzzerTestOneInput with the correct location.
  //  * Have one edge for SingleEdgeFunc.
  //  * Have several edges for MultiEdgeFunc.
  for (size_t i = 0; i < symbols.size(); i++) {
    bool is_func_entry = pc_table[i].has_flag(PCInfo::kFuncEntry);
    if (is_func_entry) {
      LOG(INFO) << symbols.full_description(i);
    }
    single_edge_func_num_edges += symbols.func(i) == "SingleEdgeFunc";
    multi_edge_func_num_edges += symbols.func(i) == "MultiEdgeFunc";
    EXPECT_NE(symbols.func(i), "main");
    if (is_func_entry && symbols.func(i) == "LLVMFuzzerTestOneInput") {
      // This is a function entry block for LLVMFuzzerTestOneInput.
      has_llvm_fuzzer_test_one_input = true;
      EXPECT_THAT(
          symbols.location(i),
          testing::HasSubstr("centipede/testing/test_fuzz_target.cc:71"));
    }
  }
  EXPECT_TRUE(has_llvm_fuzzer_test_one_input);
  EXPECT_EQ(single_edge_func_num_edges, 1);
  EXPECT_GT(multi_edge_func_num_edges, 1);
}

// Tests GetPcTableFromBinary() and SymbolTable on test_fuzz_target.
TEST(PCTable, GetPcTableFromBinary_And_SymbolTable_PCTable) {
  EXPECT_NO_FATAL_FAILURE(SymbolizeBinary(
      GetTestTempDir(test_info_->name()).string(), GetTargetPath(),
      /*use_trace_pc=*/false));
}

// Tests GetPcTableFromBinary() and SymbolTable on test_fuzz_target_trace_pc.
TEST(PCTable, GetPcTableFromBinary_And_SymbolTable_TracePC) {
  EXPECT_NO_FATAL_FAILURE(SymbolizeBinary(
      GetTestTempDir(test_info_->name()).string(), GetTracePCTargetPath(),
      /*use_trace_pc=*/true));
}

}  // namespace

}  // namespace fuzztest::internal
