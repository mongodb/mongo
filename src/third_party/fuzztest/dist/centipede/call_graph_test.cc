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

#include "./centipede/call_graph.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "./centipede/control_flow.h"
#include "./centipede/pc_info.h"
#include "./common/logging.h"

namespace fuzztest::internal {
namespace {

using ::testing::Contains;

// Mock CFTable for the cfg of function 1: pcs in parentheses are callees.
// There are there more CFTables for functions 6, 7, 8.
// Function 99 has no CFTable.
//      1
//    /   \
//   /     \
//  2 (99)  3 (6, -1, 8)
//   \     /
//    \   /
//      4 (7)
static const CFTable g_cf_table = {
    1, 2, 3, 0,  0,         // PC 1 has no callee.
    2, 4, 0, 99, 0,         // PC 2 calls 99.
    3, 4, 0, 6,  -1, 8, 0,  // PC 3 calls 6, 8, and has one indirect call.
    4, 0, 7, 0,             // PC 4 calls 7.
    5, 0, 0,                // PC 5 is not in pc_table.
    6, 0, 0,                // PC 6 has no callees.
    7, 0, 0,                // PC 7 has no callees.
    8, 0, 7, 0,             // PC 8 calls 7.
};

// Mock PCTable for the above cfg.
static const PCTable g_pc_table = {
    {1, PCInfo::kFuncEntry},
    {2, 0},
    {3, 0},
    {4, 0},
    {6, PCInfo::kFuncEntry},
    {7, PCInfo::kFuncEntry},
    {8, PCInfo::kFuncEntry},
};

TEST(CallGraphDeathTest, CgNoneExistentPc) {
  CallGraph call_graph;
  call_graph.InitializeCallGraph(g_cf_table, g_pc_table);

  // Check with a non-existent PC to make map::at fail.
  EXPECT_DEATH(call_graph.GetFunctionCallees(666), "");
  EXPECT_DEATH(call_graph.GetBasicBlockCallees(666), "");
}

TEST(CallGraph, BuildCgFromCfTable) {
  CallGraph call_graph;
  call_graph.InitializeCallGraph(g_cf_table, g_pc_table);

  absl::flat_hash_set<uintptr_t> instrumented_pcs;
  for (auto &pc_info : g_pc_table) {
    instrumented_pcs.insert(pc_info.pc);
  }

  // Check callees.
  for (size_t i = 0; i < g_pc_table.size(); ++i) {
    uintptr_t pc = g_pc_table[i].pc;
    if (g_pc_table[i].has_flag(PCInfo::kFuncEntry))
      EXPECT_TRUE(call_graph.IsFunctionEntry(pc));
    else
      EXPECT_FALSE(call_graph.IsFunctionEntry(pc));

    SCOPED_TRACE(testing::Message() << VV(pc));
    if (pc == 1) {
      EXPECT_THAT(call_graph.GetFunctionCallees(pc).size(), 5);
      EXPECT_THAT(call_graph.GetBasicBlockCallees(pc).size(), 0);
    } else if (pc == 2) {
      EXPECT_THAT(call_graph.GetBasicBlockCallees(pc).size(), 1);
    } else if (pc == 3) {
      auto callees = call_graph.GetBasicBlockCallees(pc);
      EXPECT_THAT(callees.size(), 3);
      for (auto &callee_pc : callees) {
        if (callee_pc == -1ULL || !instrumented_pcs.contains(callee_pc))
          continue;  // Indirect call or library function call.
        SCOPED_TRACE(testing::Message() << VV(callee_pc));
        EXPECT_TRUE(call_graph.IsFunctionEntry(callee_pc));
      }
      EXPECT_THAT(callees, Contains(6));
      EXPECT_THAT(callees, Contains(8));

      // Check the number of indirect calls.
      EXPECT_THAT(std::count(callees.begin(), callees.end(), -1ULL), 1);
    } else if (pc == 4) {
      EXPECT_THAT(call_graph.GetBasicBlockCallees(pc).size(), 1);
    } else if (pc == 5) {
      EXPECT_THAT(call_graph.GetFunctionCallees(pc).size(), 0);
    } else if (pc == 6 || pc == 7) {
      EXPECT_THAT(call_graph.GetFunctionCallees(pc).size(), 0);
      EXPECT_THAT(call_graph.GetBasicBlockCallees(pc).size(), 0);
    } else if (pc == 8) {
      EXPECT_THAT(call_graph.GetFunctionCallees(pc).size(), 1);
      EXPECT_THAT(call_graph.GetBasicBlockCallees(pc).size(), 1);
    }
  }
}

}  // namespace

}  // namespace fuzztest::internal
