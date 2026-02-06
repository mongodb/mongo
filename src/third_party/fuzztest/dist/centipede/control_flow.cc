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
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <istream>
#include <iterator>
#include <ostream>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "./centipede/command.h"
#include "./centipede/pc_info.h"
#include "./centipede/util.h"
#include "./common/defs.h"
#include "./common/logging.h"
#include "./common/remote_file.h"

namespace fuzztest::internal {

PCTable ReadPcTableFromFile(std::string_view file_path) {
  ByteArray pc_infos_as_bytes;
  ReadFromLocalFile(file_path, pc_infos_as_bytes);
  CHECK_EQ(pc_infos_as_bytes.size() % sizeof(PCInfo), 0);
  size_t pc_table_size = pc_infos_as_bytes.size() / sizeof(PCInfo);
  const auto *pc_infos = reinterpret_cast<PCInfo *>(pc_infos_as_bytes.data());
  PCTable pc_table{pc_infos, pc_infos + pc_table_size};
  CHECK_EQ(pc_table.size(), pc_table_size);
  return pc_table;
}

PCTable GetPcTableFromBinaryWithTracePC(std::string_view binary_path,
                                        std::string_view objdump_path,
                                        std::string_view tmp_path) {
  const std::string stderr_path = absl::StrCat(tmp_path, ".log");
  Command::Options cmd_options;
  cmd_options.args = {"-d", std::string(binary_path)};
  cmd_options.stdout_file = std::string(tmp_path);
  cmd_options.stderr_file = stderr_path;
  Command cmd{objdump_path, std::move(cmd_options)};
  int exit_code = cmd.Execute();
  if (exit_code != EXIT_SUCCESS) {
    std::string log_text;
    ReadFromLocalFile(stderr_path, log_text);
    LOG(ERROR) << "Failed to use objdump to get PC table; stderr is:";
    for (const auto &line : absl::StrSplit(log_text, '\n')) {
      LOG(ERROR).NoPrefix() << line;
    }
    std::filesystem::remove(tmp_path);
    std::filesystem::remove(stderr_path);
    return {};
  }
  std::filesystem::remove(stderr_path);
  PCTable pc_table;
  std::ifstream in(std::string{tmp_path});
  CHECK(in.good()) << VV(tmp_path);
  bool saw_new_function = false;

  // Read the objdump output, find lines that start a function
  // and lines that have a call to __sanitizer_cov_trace_pc.
  // Reconstruct the PCTable from those.
  for (std::string line; std::getline(in, line);) {
    if (absl::EndsWith(line, ">:")) {  // new function.
      saw_new_function = true;
      continue;
    }
    // On MacOS there is an extra underscope before the symbols, so not sealing
    // the symbol with `<`.
    if (!absl::EndsWith(line, "__sanitizer_cov_trace_pc>") &&
        !absl::EndsWith(line, "__sanitizer_cov_trace_pc@plt>"))
      continue;
    uintptr_t pc = std::stoul(line, nullptr, 16);
    uintptr_t flags = saw_new_function ? PCInfo::kFuncEntry : 0;
    saw_new_function = false;  // next trace_pc will be in the same function.
    pc_table.push_back({pc, flags});
  }
  std::filesystem::remove(tmp_path);
  return pc_table;
}

CFTable ReadCfTable(std::istream &in) {
  const std::string input_string(std::istreambuf_iterator<char>(in), {});
  const ByteArray cf_table_as_bytes(input_string.begin(), input_string.end());
  CHECK_EQ(cf_table_as_bytes.size() % sizeof(CFTable::value_type), 0);
  const size_t cf_table_size =
      cf_table_as_bytes.size() / sizeof(CFTable::value_type);
  const auto *cf_entries =
      reinterpret_cast<const CFTable::value_type *>(cf_table_as_bytes.data());
  return CFTable{cf_entries, cf_entries + cf_table_size};
}

CFTable ReadCfTable(std::string_view file_path) {
  std::string cf_table_contents;
  CHECK_OK(RemoteFileGetContents(file_path, cf_table_contents));
  std::istringstream cf_table_stream(cf_table_contents);
  return ReadCfTable(cf_table_stream);
}

void WriteCfTable(const CFTable &cf_table, std::ostream &out) {
  out.write(reinterpret_cast<const char *>(cf_table.data()),
            sizeof(CFTable::value_type) * cf_table.size());
}

DsoTable ReadDsoTableFromFile(std::string_view file_path) {
  DsoTable result;
  std::string data;
  ReadFromLocalFile(file_path, data);
  for (const auto &line : absl::StrSplit(data, '\n', absl::SkipEmpty())) {
    // Use std::string; there is no std::stoul for std::string_view.
    const std::vector<std::string> tokens =
        absl::StrSplit(line, ' ', absl::SkipEmpty());
    CHECK_EQ(tokens.size(), 2) << VV(line);
    result.push_back(DsoInfo{tokens[0], std::stoul(tokens[1])});
  }
  return result;
}

void ControlFlowGraph::InitializeControlFlowGraph(const CFTable &cf_table,
                                                  const PCTable &pc_table) {
  CHECK(!cf_table.empty());
  func_entries_.resize(pc_table.size());
  reachability_.resize(pc_table.size());

  for (size_t j = 0; j < cf_table.size();) {
    std::vector<uintptr_t> successors;
    auto curr_pc = cf_table[j];
    ++j;

    // Iterate over successors.
    while (cf_table[j]) {
      successors.push_back(cf_table[j]);
      ++j;
    }
    ++j;  // Step over the delimiter.

    // Record the list of successors
    graph_[curr_pc] = std::move(successors);
    // TODO(ussuri): Remove after debugging.
    VLOG(100) << "Added PC: " << curr_pc;

    // Iterate over callees.
    while (cf_table[j]) {
      ++j;
    }
    ++j;  // Step over the delimiter.
    CHECK_LE(j, cf_table.size());
  }
  // Calculate cyclomatic complexity for all functions.
  for (PCIndex i = 0; i < pc_table.size(); ++i) {
    pc_index_map_[pc_table[i].pc] = i;
    if (pc_table[i].has_flag(PCInfo::kFuncEntry)) {
      func_entries_[i] = true;
      uintptr_t func_pc = pc_table[i].pc;
      auto func_comp = ComputeFunctionCyclomaticComplexity(func_pc, *this);
      function_complexities_[func_pc] = func_comp;
    }
  }
}

const std::vector<uintptr_t> &ControlFlowGraph::GetSuccessors(
    uintptr_t basic_block) const {
  auto it = graph_.find(basic_block);
  CHECK(it != graph_.end()) << VV(basic_block);
  return it->second;
}

std::vector<uintptr_t> ControlFlowGraph::ComputeReachabilityForPc(
    uintptr_t pc) const {
  absl::flat_hash_set<uintptr_t> visited_pcs;
  std::queue<uintptr_t> worklist;

  worklist.push(pc);
  while (!worklist.empty()) {
    auto current_pc = worklist.front();
    worklist.pop();
    if (!visited_pcs.insert(current_pc).second) continue;
    for (const auto &successor : graph_.at(current_pc)) {
      if (!exists(successor)) continue;
      worklist.push(successor);
    }
  }
  return {visited_pcs.begin(), visited_pcs.end()};
}

uint32_t ComputeFunctionCyclomaticComplexity(uintptr_t pc,
                                             const ControlFlowGraph &cfg) {
  size_t edge_num = 0, node_num = 0;

  absl::flat_hash_set<uintptr_t> visited_pcs;
  std::queue<uintptr_t> worklist;

  worklist.push(pc);

  while (!worklist.empty()) {
    auto current_pc = worklist.front();
    worklist.pop();
    if (!visited_pcs.insert(current_pc).second) continue;
    ++node_num;
    for (auto &successor : cfg.GetSuccessors(current_pc)) {
      if (!cfg.exists(successor)) continue;
      ++edge_num;
      worklist.push(successor);
    }
  }

  return edge_num - node_num + 2;
}

}  // namespace fuzztest::internal
