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

#include <string.h>

#include <cstdint>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/strings/str_split.h"
#include "absl/synchronization/mutex.h"
#include "./centipede/control_flow.h"
#include "./centipede/feature.h"
#include "./centipede/pc_info.h"
#include "./centipede/symbol_table.h"
#include "./common/remote_file.h"
#include "./common/status_macros.h"

namespace fuzztest::internal {

Coverage::Coverage(const PCTable &pc_table, const PCIndexVec &pci_vec)
    : func_entries_(pc_table.size()),
      fully_covered_funcs_vec_(pc_table.size()),
      covered_pcs_vec_(pc_table.size()) {
  CHECK_LT(pc_table.size(), std::numeric_limits<PCIndex>::max());
  absl::flat_hash_set<PCIndex> covered_pcs(pci_vec.begin(), pci_vec.end());
  // Iterate though all the pc_table entries.
  // The first one is some function's kFuncEntry.
  // Then find the next kFuncEntry or the table end.
  // Everything in between corresponds to the current function.
  // For fully (un)covered functions, add their entry PCIndex
  // to fully_covered_funcs or uncovered_funcs correspondingly.
  // For all others add them to partially_covered_funcs.
  for (size_t this_func = 0; this_func < pc_table.size();) {
    CHECK(pc_table[this_func].has_flag(PCInfo::kFuncEntry));
    func_entries_[this_func] = true;
    // Find next entry.
    size_t next_func = this_func + 1;
    while (next_func < pc_table.size() &&
           !pc_table[next_func].has_flag(PCInfo::kFuncEntry)) {
      next_func++;
    }
    // Collect covered and uncovered indices.
    PartiallyCoveredFunction pcf;
    for (size_t i = this_func; i < next_func; i++) {
      if (covered_pcs.contains(i)) {
        pcf.covered.push_back(i);
        covered_pcs_vec_[i] = true;
      } else {
        pcf.uncovered.push_back(i);
      }
    }
    // Put this function into one of
    // {fully_covered_funcs, uncovered_funcs, partially_covered_funcs}
    size_t num_func_pcs = next_func - this_func;
    if (num_func_pcs == pcf.covered.size()) {
      fully_covered_funcs.push_back(this_func);
      fully_covered_funcs_vec_[this_func] = true;
    } else if (pcf.covered.empty()) {
      uncovered_funcs.push_back(this_func);
    } else {
      CHECK(!pcf.covered.empty());
      CHECK(!pcf.uncovered.empty());
      CHECK_EQ(pcf.covered.size() + pcf.uncovered.size(), num_func_pcs);
      partially_covered_funcs.push_back(pcf);
    }
    // Move to the next function.
    this_func = next_func;
  }
}

void Coverage::DumpReportToFile(const SymbolTable &symbols,
                                std::string_view filepath,
                                std::string_view description) {
  auto *file = ValueOrDie(RemoteFileOpen(filepath, "w"));
  CHECK(file != nullptr) << "Failed to open file: " << filepath;
  CHECK_OK(RemoteFileSetWriteBufferSize(file, 100UL * 1024 * 1024));
  if (!description.empty()) {
    CHECK_OK(RemoteFileAppend(file, "# "));
    CHECK_OK(RemoteFileAppend(file, std::string{description}));
    CHECK_OK(RemoteFileAppend(file, ":\n\n"));
  }
  // Print symbolized function names for all covered functions.
  for (auto pc_index : fully_covered_funcs) {
    CHECK_OK(RemoteFileAppend(file, "FULL: "));
    CHECK_OK(RemoteFileAppend(file, symbols.full_description(pc_index)));
    CHECK_OK(RemoteFileAppend(file, "\n"));
  }
  CHECK_OK(RemoteFileFlush(file));
  // Same for uncovered functions.
  for (auto pc_index : uncovered_funcs) {
    CHECK_OK(RemoteFileAppend(file, "NONE: "));
    CHECK_OK(RemoteFileAppend(file, symbols.full_description(pc_index)));
    CHECK_OK(RemoteFileAppend(file, "\n"));
  }
  CHECK_OK(RemoteFileFlush(file));
  // For every partially covered function, first print its name,
  // then print its covered edges, then uncovered edges.
  for (auto &pcf : partially_covered_funcs) {
    CHECK_OK(RemoteFileAppend(file, "PARTIAL: "));
    CHECK_OK(RemoteFileAppend(file, symbols.full_description(pcf.covered[0])));
    CHECK_OK(RemoteFileAppend(file, "\n"));
    for (auto pc_index : pcf.covered) {
      CHECK_OK(RemoteFileAppend(file, "  + "));
      CHECK_OK(RemoteFileAppend(file, symbols.full_description(pc_index)));
      CHECK_OK(RemoteFileAppend(file, "\n"));
    }
    for (auto pc_index : pcf.uncovered) {
      CHECK_OK(RemoteFileAppend(file, "  - "));
      CHECK_OK(RemoteFileAppend(file, symbols.full_description(pc_index)));
      CHECK_OK(RemoteFileAppend(file, "\n"));
    }
  }
  CHECK_OK(RemoteFileFlush(file));
  CHECK_OK(RemoteFileClose(file));
}

std::string CoverageLogger::ObserveAndDescribeIfNew(PCIndex pc_index) {
  if (pc_table_.empty()) return "";  // Fast-path return (symbolization is off).
  absl::MutexLock l(&mu_);
  if (!observed_indices_.insert(pc_index).second) return "";
  std::ostringstream os;
  if (pc_index >= pc_table_.size()) {
    os << "FUNC/EDGE index: " << pc_index;
  } else {
    os << (pc_table_[pc_index].has_flag(PCInfo::kFuncEntry) ? "FUNC: "
                                                            : "EDGE: ");
    os << symbols_.full_description(pc_index);
    if (!observed_descriptions_.insert(os.str()).second) return "";
  }
  return os.str();
}

FunctionFilter::FunctionFilter(std::string_view functions_to_filter,
                               const SymbolTable &symbols) {
  // set pcs_[idx] to 1, for any idx that belongs to a filtered function.
  // keep pcs_ empty, if no filtered functions are found in symbols.
  for (auto &func : absl::StrSplit(functions_to_filter, ',')) {
    for (size_t idx = 0, n = symbols.size(); idx < n; ++idx) {
      if (func == symbols.func(idx)) {
        if (pcs_.empty()) {
          pcs_.resize(n);
        }
        pcs_[idx] = 1;
      }
    }
  }
}

bool FunctionFilter::filter(const FeatureVec &features) const {
  if (pcs_.empty()) return true;
  for (auto feature : features) {
    if (!feature_domains::kPCs.Contains(feature)) continue;
    size_t idx = ConvertPCFeatureToPcIndex(feature);
    // idx should normally be within the range. Ignore it if it's not.
    if (idx >= pcs_.size()) continue;
    if (pcs_[idx]) return true;
  }
  return false;
}

static uint8_t SelectMultiplierByCoverageKind(uint8_t uncovered_knob,
                                              uint8_t partially_covered_knob,
                                              uint8_t fully_covered_knob,
                                              PCIndex callee_idx,
                                              const Coverage &coverage) {
  if (coverage.FunctionIsFullyCovered(callee_idx)) return fully_covered_knob;
  if (coverage.BlockIsCovered(callee_idx)) return partially_covered_knob;
  return uncovered_knob;
}

uint32_t ComputeFrontierWeight(const Coverage &coverage,
                               const ControlFlowGraph &cfg,
                               const std::vector<uintptr_t> &callees) {
  // Multiplication factors for different coverage types.
  // TODO(ussuri): replace with actual knobs (cl/486229527).
  uint8_t uncovered_knob = 153;         // ~ (255 * 0.6)
  uint8_t partially_covered_knob = 77;  // ~ (255 * 0.3)
  uint8_t fully_covered_knob = 25;      // ~ (255 * 0.1)

  uint32_t weight = 0;
  for (auto callee : callees) {
    // TODO(ussuri): Figure out a better way for determining the complexity
    // of indirect callee. For now using cyclomatic_comp = 1, and factor of
    // non-covered callee.
    if (callee == -1ULL) {
      weight += uncovered_knob;
      continue;
    }
    // This function's body is not in this DSO, like library functions. For now
    // skipping it as we have no coverage kind (Fully/Partially covered or
    // uncovered) and no complexity for it.
    if (!cfg.IsInPcTable(callee)) continue;

    // Retrieve cyclomatic complexity
    auto cyclomatic_comp = cfg.GetCyclomaticComplexity(callee);
    // Determine knob based on callee coverage kind.
    auto callee_idx = cfg.GetPcIndex(callee);
    CHECK(cfg.BlockIsFunctionEntry(callee_idx));
    auto coverage_multiplier = SelectMultiplierByCoverageKind(
        uncovered_knob, partially_covered_knob, fully_covered_knob, callee_idx,
        coverage);

    weight += coverage_multiplier * cyclomatic_comp;
  }
  return weight;
}

}  // namespace fuzztest::internal
