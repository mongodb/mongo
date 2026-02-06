// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./fuzztest/internal/domains/in_regexp_impl.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/random/bit_gen_ref.h"
#include "absl/random/distributions.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "./fuzztest/internal/domains/domain_base.h"
#include "./fuzztest/internal/domains/regexp_dfa.h"
#include "./fuzztest/internal/logging.h"
#include "./fuzztest/internal/serialization.h"
#include "./fuzztest/internal/status.h"
#include "./fuzztest/internal/type_support.h"

namespace fuzztest::internal {

InRegexpImpl::InRegexpImpl(std::string_view regex_str)
    : regex_str_(regex_str), dfa_(RegexpDFA::Create(regex_str_)) {}

RegexpDFA::Path InRegexpImpl::Init(absl::BitGenRef prng) {
  if (auto seed = MaybeGetRandomSeed(prng)) return *seed;
  absl::StatusOr<RegexpDFA::Path> path =
      dfa_.StringToPath(dfa_.GenerateString(prng));
  FUZZTEST_INTERNAL_CHECK_PRECONDITION(path.ok(),
                                       "Init should generate valid paths");
  return *path;
}

void InRegexpImpl::Mutate(RegexpDFA::Path& path, absl::BitGenRef prng,
                          const domain_implementor::MutationMetadata&,
                          bool only_shrink) {
  if (only_shrink) {
    // Fast path to remove loop.
    if (absl::Bernoulli(prng, 0.5)) {
      if (ShrinkByRemoveLoop(prng, path)) return;
    }

    if (ShrinkByFindShorterSubPath(prng, path)) return;
    return;
  }

  size_t rand_offset = absl::Uniform(prng, 0u, path.size());
  // Maps states to the path index of their first appearance. We want the
  // mutation to be minimal, so if a state appears multiple times in the path,
  // we only keep the index of its first appearance.
  std::vector<std::optional<int>> sink_states_first_appearance(
      dfa_.state_count());
  for (size_t i = rand_offset; i < path.size(); ++i) {
    int state_id = path[i].from_state_id;
    if (sink_states_first_appearance[state_id].has_value()) continue;
    sink_states_first_appearance[state_id] = i;
  }
  RegexpDFA::Path new_subpath = dfa_.FindPath(
      path[rand_offset].from_state_id, sink_states_first_appearance, prng);
  int to_state_id = new_subpath.back().from_state_id;
  new_subpath.pop_back();

  RegexpDFA::Path new_path;
  for (size_t i = 0; i < rand_offset; ++i) {
    new_path.push_back(path[i]);
  }
  for (size_t i = 0; i < new_subpath.size(); ++i) {
    new_path.push_back(new_subpath[i]);
  }

  // Found a node in the original path, so we append the remaining substring
  // of the original path.
  if (sink_states_first_appearance[to_state_id].has_value()) {
    for (size_t i = *sink_states_first_appearance[to_state_id]; i < path.size();
         ++i) {
      new_path.push_back(path[i]);
    }
  }
  ValidatePathRoundtrip(new_path);
  path = std::move(new_path);
}

StringPrinter InRegexpImpl::GetPrinter() const { return StringPrinter{}; }

InRegexpImpl::value_type InRegexpImpl::GetValue(const corpus_type& v) const {
  absl::StatusOr<std::string> val = dfa_.PathToString(v);
  FUZZTEST_INTERNAL_CHECK(val.ok(), "Corpus is invalid!");
  return *val;
}

std::optional<InRegexpImpl::corpus_type> InRegexpImpl::FromValue(
    const value_type& v) const {
  absl::StatusOr<corpus_type> path = dfa_.StringToPath(v);
  if (!path.ok()) return std::nullopt;
  return *path;
}

std::optional<InRegexpImpl::corpus_type> InRegexpImpl::ParseCorpus(
    const IRObject& obj) const {
  auto subs = obj.Subs();
  if (!subs) return std::nullopt;
  if (subs->size() % 2 != 0) return std::nullopt;
  corpus_type corpus_value;
  for (size_t i = 0; i < subs->size(); i += 2) {
    auto from_state_id = (*subs)[i].ToCorpus<int>();
    auto edge_index = (*subs)[i + 1].ToCorpus<int>();
    if (!from_state_id.has_value() || !edge_index.has_value())
      return std::nullopt;
    corpus_value.push_back(RegexpDFA::Edge{*from_state_id, *edge_index});
  }
  return corpus_value;
}

IRObject InRegexpImpl::SerializeCorpus(const corpus_type& path) const {
  IRObject obj;
  auto& subs = obj.MutableSubs();
  for (const auto& edge : path) {
    subs.push_back(IRObject::FromCorpus(edge.from_state_id));
    subs.push_back(IRObject::FromCorpus(edge.edge_index));
  }
  return obj;
}

absl::Status InRegexpImpl::ValidateCorpusValue(
    const corpus_type& corpus_value) const {
  // Check whether this is a valid path in the DFA.
  absl::StatusOr<std::string> str = dfa_.PathToString(corpus_value);
  if (str.ok()) return absl::OkStatus();
  return Prefix(str.status(), absl::StrCat("Invalid value for InRegexp(\"",
                                           regex_str_, "\")"));
}

void InRegexpImpl::ValidatePathRoundtrip(const RegexpDFA::Path& path) const {
  absl::StatusOr<std::string> str = dfa_.PathToString(path);
  FUZZTEST_INTERNAL_CHECK(str.ok(), "Invalid path in the DFA!");
  absl::StatusOr<RegexpDFA::Path> new_path = dfa_.StringToPath(*str);
  FUZZTEST_INTERNAL_CHECK(new_path.ok(), "Invalid path in the DFA!");
}

bool InRegexpImpl::ShrinkByRemoveLoop(absl::BitGenRef prng,
                                      RegexpDFA::Path& path) {
  std::vector<std::vector<int>> state_appearances(dfa_.state_count());
  for (int i = 0; i < path.size(); ++i) {
    state_appearances[path[i].from_state_id].push_back(i);
  }
  std::vector<int> states_with_loop;
  for (int i = 0; i < dfa_.state_count(); ++i) {
    if (state_appearances[i].size() > 1) states_with_loop.push_back(i);
  }
  if (!states_with_loop.empty()) {
    size_t rand_state_id =
        states_with_loop[absl::Uniform(prng, 0u, states_with_loop.size())];
    std::vector<int>& loop_indexes = state_appearances[rand_state_id];
    size_t loop_start = absl::Uniform(prng, 0u, loop_indexes.size() - 1);
    size_t loop_end = absl::Uniform(prng, loop_start + 1, loop_indexes.size());
    // Delete the detected loop.
    path.erase(path.begin() + loop_indexes[loop_start],
               path.begin() + loop_indexes[loop_end]);
    ValidatePathRoundtrip(path);
    return true;
  }
  return false;
}

bool InRegexpImpl::ShrinkByFindShorterSubPath(absl::BitGenRef prng,
                                              RegexpDFA::Path& path) {
  if (path.size() <= 1) {
    return false;
  }
  constexpr int n_trial = 40;
  constexpr int max_exploration_length = 100;
  for (int i = 0; i < n_trial; ++i) {
    // Pick any state in `path` as the start of the subpath, *except* the one
    // in the last element.
    size_t from_index = absl::Uniform(prng, 0u, path.size() - 1);
    int from_state_id = path[from_index].from_state_id;

    // Pick a state after the "from state" as the end of the subpath.
    size_t to_index, length;
    int to_state_id;
    if (i <= n_trial / 2) {
      // Pick any state in `path` after the "from state" as the end of the
      // subpath; this excludes the "end state".
      to_index = absl::Uniform(
          prng, from_index + 1,
          std::min(from_index + max_exploration_length, path.size()));
      to_state_id = path[to_index].from_state_id;
      length = to_index - from_index;
    } else {
      // If failing too many times, try to find a shorter path to the
      // end_state as a fall back. In this case, to_index isn't the index of
      // a valid element in `path`.
      to_index = path.size();
      to_state_id = dfa_.end_state_id();
      length = to_index - from_index;
    }

    if (length == 1) continue;

    RegexpDFA::Path new_subpath =
        dfa_.FindPathWithinLengthDFS(from_state_id, to_state_id, length, prng);
    // If the size is unchanged, keep trying.
    if (new_subpath.size() == length) continue;

    RegexpDFA::Path new_path(path.begin(), path.begin() + from_index);
    new_path.insert(new_path.end(), new_subpath.begin(), new_subpath.end());
    for (size_t idx = to_index; idx < path.size(); ++idx) {
      new_path.push_back(path[idx]);
    }
    ValidatePathRoundtrip(new_path);
    path = std::move(new_path);
    return true;
  }
  return false;
}

}  // namespace fuzztest::internal
