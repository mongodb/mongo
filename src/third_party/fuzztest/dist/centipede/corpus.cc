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

#include "./centipede/corpus.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/substitute.h"
#include "./centipede/control_flow.h"
#include "./centipede/coverage.h"
#include "./centipede/execution_metadata.h"
#include "./centipede/feature.h"
#include "./centipede/feature_set.h"
#include "./centipede/util.h"
#include "./common/defs.h"
#include "./common/logging.h"  // IWYU pragma: keep
#include "./common/remote_file.h"
#include "./common/status_macros.h"

namespace fuzztest::internal {

//------------------------------------------------------------------------------
//                                  Corpus
//------------------------------------------------------------------------------

// Returns the weight of `fv` computed using `fs` and `coverage_frontier`.
static size_t ComputeWeight(const FeatureVec &fv, const FeatureSet &fs,
                            const CoverageFrontier &coverage_frontier) {
  size_t weight = fs.ComputeWeight(fv);
  // The following is checking for the cases where PCTable is not present. In
  // such cases, we cannot use any ControlFlow related features.
  if (coverage_frontier.MaxPcIndex() == 0) return weight;
  size_t frontier_weights_sum = 0;
  for (const auto feature : fv) {
    if (!feature_domains::kPCs.Contains(feature)) continue;
    const auto pc_index = ConvertPCFeatureToPcIndex(feature);
    // Avoid checking frontier for out-of-bounds indices.
    // TODO(b/299624088): revisit once dlopen is supported.
    if (pc_index >= coverage_frontier.MaxPcIndex()) continue;
    if (coverage_frontier.PcIndexIsFrontier(pc_index)) {
      frontier_weights_sum += coverage_frontier.FrontierWeight(pc_index);
    }
  }
  return weight * (frontier_weights_sum + 1);  // Multiply by at least 1.
}

std::pair<size_t, size_t> Corpus::MaxAndAvgSize() const {
  if (records_.empty()) return {0, 0};
  size_t max = 0;
  size_t total = 0;
  for (const auto &r : records_) {
    max = std::max(max, r.data.size());
    total += r.data.size();
  }
  return {max, total / records_.size()};
}

size_t Corpus::Prune(const FeatureSet &fs,
                     const CoverageFrontier &coverage_frontier,
                     size_t max_corpus_size, Rng &rng) {
  // TODO(kcc): use coverage_frontier.
  CHECK(max_corpus_size);
  if (records_.size() < 2UL) return 0;
  // Recompute the weights.
  size_t num_zero_weights = 0;
  for (size_t i = 0, n = records_.size(); i < n; ++i) {
    fs.PruneFeaturesAndCountUnseen(records_[i].features);
    auto new_weight =
        ComputeWeight(records_[i].features, fs, coverage_frontier);
    weighted_distribution_.ChangeWeight(i, new_weight);
    if (new_weight == 0) ++num_zero_weights;
  }

  // Remove zero weights and the corresponding corpus record.
  // Also remove some random elements, if the corpus is still too big.
  // The corpus must not be empty, hence target_size is at least 1.
  // It should also be <= max_corpus_size.
  size_t target_size = std::min(
      max_corpus_size, std::max(1UL, records_.size() - num_zero_weights));
  auto subset_to_remove =
      weighted_distribution_.RemoveRandomWeightedSubset(target_size, rng);
  RemoveSubset(subset_to_remove, records_);

  weighted_distribution_.RecomputeInternalState();
  CHECK(!records_.empty());

  // Features may have shrunk from CountUnseenAndPruneFrequentFeatures.
  // Call shrink_to_fit for the features that survived the pruning.
  for (auto &record : records_) {
    record.features.shrink_to_fit();
  }

  num_pruned_ += subset_to_remove.size();
  return subset_to_remove.size();
}

void Corpus::Add(const ByteArray &data, const FeatureVec &fv,
                 const ExecutionMetadata &metadata, const FeatureSet &fs,
                 const CoverageFrontier &coverage_frontier) {
  // TODO(kcc): use coverage_frontier.
  CHECK(!data.empty())
      << "Got request to add empty element to corpus: ignoring";
  CHECK_EQ(records_.size(), weighted_distribution_.size());
  records_.push_back({data, fv, metadata});
  weighted_distribution_.AddWeight(ComputeWeight(fv, fs, coverage_frontier));
}

const CorpusRecord &Corpus::WeightedRandom(size_t random) const {
  return records_[weighted_distribution_.RandomIndex(random)];
}

const CorpusRecord &Corpus::UniformRandom(size_t random) const {
  return records_[random % records_.size()];
}

void Corpus::DumpStatsToFile(const FeatureSet &fs, std::string_view filepath,
                             std::string_view description) {
  auto *file = ValueOrDie(RemoteFileOpen(filepath, "w"));
  CHECK(file != nullptr) << "Failed to open file: " << filepath;
  CHECK_OK(RemoteFileSetWriteBufferSize(file, 100UL * 1024 * 1024));
  static constexpr std::string_view kHeaderStub = R"(# $0
{
  "num_inputs": $1,
  "corpus_stats": [)";
  static constexpr std::string_view kRecordStub = R"($0
    {"size": $1, "frequencies": [$2]})";
  static constexpr std::string_view kFooter = R"(
  ]
}
)";
  const std::string header_str =
      absl::Substitute(kHeaderStub, description, records_.size());
  CHECK_OK(RemoteFileAppend(file, header_str));
  std::string before_record;
  for (const auto &record : records_) {
    std::vector<size_t> frequencies;
    frequencies.reserve(record.features.size());
    for (const auto feature : record.features) {
      frequencies.push_back(fs.Frequency(feature));
    }
    const std::string frequencies_str = absl::StrJoin(frequencies, ", ");
    const std::string record_str = absl::Substitute(
        kRecordStub, before_record, record.data.size(), frequencies_str);
    CHECK_OK(RemoteFileAppend(file, record_str));
    before_record = ",";
  }
  CHECK_OK(RemoteFileAppend(file, std::string{kFooter}));
  CHECK_OK(RemoteFileClose(file));
}

std::string Corpus::MemoryUsageString() const {
  size_t data_size = 0;
  size_t features_size = 0;
  for (const auto &record : records_) {
    data_size += record.data.capacity() * sizeof(record.data[0]);
    features_size += record.features.capacity() * sizeof(record.features[0]);
  }
  return absl::StrCat("d", data_size >> 20, "/f", features_size >> 20);
}

//------------------------------------------------------------------------------
//                          WeightedDistribution
//------------------------------------------------------------------------------

void WeightedDistribution::AddWeight(uint64_t weight) {
  CHECK_EQ(weights_.size(), cumulative_weights_.size());
  weights_.push_back(weight);
  if (cumulative_weights_.empty()) {
    cumulative_weights_.push_back(weight);
  } else {
    cumulative_weights_.push_back(cumulative_weights_.back() + weight);
  }
}

void WeightedDistribution::ChangeWeight(size_t idx, uint64_t new_weight) {
  CHECK_LT(idx, size());
  weights_[idx] = new_weight;
  cumulative_weights_valid_ = false;
}

__attribute__((noinline))  // to see it in profile.
void WeightedDistribution::RecomputeInternalState() {
  uint64_t partial_sum = 0;
  for (size_t i = 0, n = size(); i < n; i++) {
    partial_sum += weights_[i];
    cumulative_weights_[i] = partial_sum;
  }
  cumulative_weights_valid_ = true;
}

__attribute__((noinline))  // to see it in profile.
size_t
WeightedDistribution::RandomIndex(size_t random) const {
  CHECK(!weights_.empty());
  CHECK(cumulative_weights_valid_);
  uint64_t sum_of_all_weights = cumulative_weights_.back();
  if (sum_of_all_weights == 0)
    return random % size();  // can't do much else here.
  random = random % sum_of_all_weights;
  auto it = std::upper_bound(cumulative_weights_.begin(),
                             cumulative_weights_.end(), random);
  CHECK(it != cumulative_weights_.end());
  return it - cumulative_weights_.begin();
}

uint64_t WeightedDistribution::PopBack() {
  uint64_t result = weights_.back();
  weights_.pop_back();
  cumulative_weights_.pop_back();
  return result;
}

//------------------------------------------------------------------------------
//                            CoverageFrontier
//------------------------------------------------------------------------------

size_t CoverageFrontier::Compute(const Corpus &corpus) {
  return Compute(corpus.Records());
}

size_t CoverageFrontier::Compute(
    const std::vector<CorpusRecord> &corpus_records) {
  // Initialize the vectors.
  std::fill(frontier_.begin(), frontier_.end(), false);
  std::fill(frontier_weight_.begin(), frontier_weight_.end(), 0);

  // A vector of covered indices in pc_table. Needed for Coverage object.
  PCIndexVec covered_pcs;
  for (const auto &record : corpus_records) {
    for (auto feature : record.features) {
      if (!feature_domains::kPCs.Contains(feature)) continue;
      size_t idx = ConvertPCFeatureToPcIndex(feature);
      if (idx >= binary_info_.pc_table.size()) continue;
      covered_pcs.push_back(idx);
      frontier_[idx] = true;
    }
  }

  Coverage coverage(binary_info_.pc_table, covered_pcs);

  num_functions_in_frontier_ = 0;
  IteratePcTableFunctions(binary_info_.pc_table, [this, &coverage](size_t beg,
                                                                   size_t end) {
    auto frontier_begin = frontier_.begin() + beg;
    auto frontier_end = frontier_.begin() + end;
    size_t cov_size_in_this_func =
        std::count(frontier_begin, frontier_end, true);

    if (cov_size_in_this_func > 0 && cov_size_in_this_func < end - beg)
      ++num_functions_in_frontier_;

    // Reset the frontier_ entries.
    std::fill(frontier_begin, frontier_end, false);

    // Iterate over BBs in the function and check the coverage statue.
    for (size_t i = beg; i < end; ++i) {
      // If the current pc is not covered, it cannot be a frontier.
      if (!coverage.BlockIsCovered(i)) continue;

      auto pc = binary_info_.pc_table[i].pc;

      // Current pc is covered, look for a non-covered successor.
      for (auto successor : binary_info_.control_flow_graph.GetSuccessors(pc)) {
        // Successor pc may not be in PCTable because of pruning.
        if (!binary_info_.control_flow_graph.IsInPcTable(successor)) continue;

        auto successor_idx =
            binary_info_.control_flow_graph.GetPcIndex(successor);

        // This successor is covered, skip it.
        if (coverage.BlockIsCovered(successor_idx)) continue;

        // Now we have a frontier, compute the weight.
        frontier_[i] = true;

        // Calculate frontier weight.
        // Here we use reachability and coverage to identify all reachable and
        // non-covered BBs from successor, and then use all functions called
        // in those BBs.
        for (auto reachable_bb :
             binary_info_.control_flow_graph.LazyGetReachabilityForPc(
                 successor)) {
          if (!binary_info_.control_flow_graph.IsInPcTable(reachable_bb) ||
              coverage.BlockIsCovered(
                  binary_info_.control_flow_graph.GetPcIndex(reachable_bb))) {
            // This reachable BB is already either processed and added or
            // covered via a different path -- not interesting!
            continue;
          }
          frontier_weight_[i] += ComputeFrontierWeight(
              coverage, binary_info_.control_flow_graph,
              binary_info_.call_graph.GetBasicBlockCallees(reachable_bb));
        }
      }
    }
  });

  return num_functions_in_frontier_;
}

}  // namespace fuzztest::internal
