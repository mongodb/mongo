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

#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/log/check.h"
#include "./centipede/control_flow.h"
#include "./centipede/pc_info.h"

namespace fuzztest::internal {

void CallGraph::InitializeCallGraph(const CFTable &cf_table,
                                    const PCTable &pc_table) {
  // Find all function entries.
  for (auto pc_info : pc_table) {
    if (pc_info.has_flag(PCInfo::kFuncEntry))
      function_entries_.insert(pc_info.pc);
  }

  uintptr_t current_function_entry = 0;

  for (size_t j = 0; j < cf_table.size();) {
    std::vector<uintptr_t> current_callees;
    auto current_pc = cf_table[j];
    ++j;

    basic_blocks_.insert(current_pc);
    if (IsFunctionEntry(current_pc)) current_function_entry = current_pc;

    // Iterate over successors.
    while (cf_table[j]) {
      ++j;
    }
    ++j;  // Step over the delimeter.

    // Iterate over callees.
    while (cf_table[j]) {
      current_callees.push_back(cf_table[j]);
      ++j;
    }
    ++j;  // Step over the delimeter.
    CHECK_LE(j, cf_table.size());

    if (current_callees.empty()) continue;
    basic_block_callees_[current_pc] = current_callees;
    // Append collected callees to the call graph.
    call_graph_[current_function_entry].insert(
        call_graph_[current_function_entry].end(), current_callees.begin(),
        current_callees.end());
  }
  // This should stay empty.
  CHECK(empty_.empty());
}

}  // namespace fuzztest::internal
