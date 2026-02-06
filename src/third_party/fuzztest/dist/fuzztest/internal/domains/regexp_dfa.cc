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

#include "./fuzztest/internal/domains/regexp_dfa.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/discrete_distribution.h"
#include "absl/random/distributions.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "./fuzztest/internal/logging.h"
#include "re2/prog.h"
#include "re2/regexp.h"

namespace fuzztest::internal {

RegexpDFA RegexpDFA::Create(absl::string_view regexp) {
  RegexpDFA dfa;
  dfa.BuildEntireDFA(CompileRegexp(regexp));
  dfa.CompressStates();
  dfa.ComputeEdgeWeights();
  return dfa;
}

std::string RegexpDFA::GenerateString(absl::BitGenRef prng) {
  std::string result;
  State* state = &states_.front();
  while (!state->is_end_state()) {
    // Pick a random next state by weight.
    int rand_index = state->edge_weight_distribution(prng);
    const auto& [fragment, next_id] = state->next[rand_index];
    result.insert(result.end(), fragment.begin(), fragment.end());
    state = &states_[next_id];
    if (state->is_end_state()) {
      FUZZTEST_INTERNAL_CHECK(
          fragment.back() == kEndOfString,
          "The last character leading to the end state should be EOS!");
      result.pop_back();
      break;
    }
  }
  return result;
}

RegexpDFA::Path RegexpDFA::FindPath(
    int from_state_id, const std::vector<std::optional<int>>& to_state_ids,
    absl::BitGenRef prng) {
  Path path;
  int cur_state_id = from_state_id;
  State* cur_state = &states_[cur_state_id];
  FUZZTEST_INTERNAL_CHECK(!cur_state->is_end_state(),
                          "Cannot start a DFA path from an end state!");
  while (true) {
    // Pick a random next state.
    int offset = cur_state->edge_weight_distribution(prng);
    path.push_back({cur_state_id, offset});
    const auto& [unused_next_fragment, next_state_id] = cur_state->next[offset];
    cur_state_id = next_state_id;
    cur_state = &states_[cur_state_id];
    // Reached an end state or found a state in the original path?
    if (cur_state->is_end_state() || to_state_ids[cur_state_id].has_value()) {
      path.push_back({cur_state_id, -1});
      break;
    }
  }
  return path;
}

RegexpDFA::Path RegexpDFA::FindPathWithinLengthDFS(int from_state_id,
                                                   int to_state_id, int length,
                                                   absl::BitGenRef prng) {
  // Each state maintains an edge and a counter for each possible length. The
  // edge is the last edge in the path from `from_state` to the state and can
  // be used to reconstruct the path. And the counter is the number of paths
  // to the state, which can be used for reservoir sampling.
  struct LastEdgeAndCounter {
    std::optional<Edge> edge;
    int counter;
  };
  std::vector<std::vector<LastEdgeAndCounter>> last_edges_and_counters(
      states_.size(), std::vector<LastEdgeAndCounter>(length + 1));

  // Randomness for DFS. Instead of always starting to explore from edge index
  // 0, we start with a different random offset for each state.
  std::vector<int> rand_edge_offsets;
  rand_edge_offsets.reserve(states_.size());
  for (const State& state : states_) {
    rand_edge_offsets.push_back(
        absl::Uniform<int>(prng, 0u, state.next.size()));
  }

  std::vector<Edge> stack{Edge{from_state_id, 0}};
  do {
    auto [current_state_id, edge_index] = stack.back();
    if (edge_index == states_[current_state_id].next.size()) {
      stack.pop_back();
      continue;
    }
    ++stack.back().edge_index;
    const int current_path_length = static_cast<int>(stack.size());
    const int real_edge_index =
        (edge_index + rand_edge_offsets[current_state_id]) %
        static_cast<int>(states_[current_state_id].next.size());
    const int next_state_id =
        states_[current_state_id].next[real_edge_index].next_state_id;
    const int n_path_of_current_length =
        ++last_edges_and_counters[next_state_id][current_path_length].counter;
    // Reservoir Sampling.
    if (absl::Bernoulli(prng, 1.0 / n_path_of_current_length)) {
      last_edges_and_counters[next_state_id][current_path_length].edge =
          Edge{current_state_id, real_edge_index};
    }
    if (n_path_of_current_length == 1 && current_path_length != length) {
      stack.push_back(Edge{next_state_id, 0});
    }
  } while (!stack.empty());

  std::vector<int> candidate_lens;
  for (int len = 1; len <= length; ++len) {
    if (last_edges_and_counters[to_state_id][len].edge.has_value()) {
      candidate_lens.push_back(len);
    }
  }
  FUZZTEST_INTERNAL_CHECK(!candidate_lens.empty(), "Cannot find a path!");

  int state_id = to_state_id;
  Path result;
  for (int len =
           candidate_lens[absl::Uniform<int>(prng, 0, candidate_lens.size())];
       len > 0; --len) {
    result.push_back(*(last_edges_and_counters[state_id][len].edge));
    state_id = result.back().from_state_id;
  }
  FUZZTEST_INTERNAL_CHECK(state_id == from_state_id,
                          "Cannot find a path from from_state");
  std::reverse(result.begin(), result.end());
  return result;
}

absl::StatusOr<RegexpDFA::Path> RegexpDFA::StringToPath(
    absl::string_view s) const {
  RegexpDFA::Path path;
  int state_id = 0;
  std::vector<std::int16_t> characters;
  characters.reserve(s.size());
  for (auto c : s) {
    characters.push_back(static_cast<uint8_t>(c));
  }
  characters.push_back(kEndOfString);
  size_t cur_index = 0;
  while (cur_index < characters.size()) {
    std::optional<int> edge_index =
        NextState(states_[state_id], characters, cur_index);
    if (!edge_index.has_value()) {
      return absl::InternalError("Error while matching a string with a DFA.");
    }
    path.push_back({state_id, *edge_index});
    state_id = states_[state_id].next[*edge_index].next_state_id;
  }
  if (!states_[state_id].is_end_state()) {
    return absl::InvalidArgumentError(
        "Didn't reach an end state while matching a string with a DFA.");
  }
  if (cur_index != characters.size() || path.empty()) {
    return absl::InternalError(
        "Impossible case while matching a string with a DFA!");
  }
  return path;
}

absl::StatusOr<std::string> RegexpDFA::PathToString(
    const RegexpDFA::Path& path) const {
  std::string result;
  std::optional<int> next_state_id;
  for (const auto& [from_state_id, edge_index] : path) {
    if (from_state_id >= states_.size() ||
        edge_index >= states_[from_state_id].next.size() ||
        (next_state_id.has_value() && next_state_id != from_state_id)) {
      return absl::InvalidArgumentError("Invalid DFA path.");
    }
    const State::StateTransition& transition =
        states_[from_state_id].next[edge_index];
    next_state_id = transition.next_state_id;
    const std::vector<std::int16_t>& fragment = transition.chars_to_match;
    result.insert(result.end(), fragment.begin(), fragment.end());
    if (next_state_id == end_state_id_) {
      FUZZTEST_INTERNAL_CHECK(
          fragment.back() == kEndOfString,
          "The last character leading to the end state should be EOS!");
      result.pop_back();
    }
  }
  if (next_state_id != end_state_id_) {
    return absl::InternalError("DFA path doesn't end in the end state.");
  }
  return result;
}

std::optional<int> RegexpDFA::NextState(
    const State& cur_state, const std::vector<std::int16_t>& input_chars,
    size_t& cur_index) const {
  auto iter = std::lower_bound(
      cur_state.next.begin(), cur_state.next.end(), input_chars,
      [cur_index](const auto& transition,
                  const std::vector<std::int16_t>& chars) {
        const size_t compare_size = std::min(transition.chars_to_match.size(),
                                             chars.size() - cur_index);
        FUZZTEST_INTERNAL_CHECK(compare_size != 0, "Nothing to compare!");
        for (size_t i = 0; i < compare_size; ++i) {
          if (transition.chars_to_match[i] == chars[cur_index + i]) continue;
          return transition.chars_to_match[i] < chars[cur_index + i];
        }
        return false;
      });
  if (iter == cur_state.next.end() ||
      iter->chars_to_match.size() > input_chars.size() - cur_index) {
    return std::nullopt;
  }
  for (size_t i = 0; i < iter->chars_to_match.size(); ++i) {
    if (iter->chars_to_match[i] != input_chars[cur_index + i]) {
      return std::nullopt;
    }
  }

  cur_index += iter->chars_to_match.size();
  return static_cast<int>(std::distance(cur_state.next.begin(), iter));
}

std::unique_ptr<re2::Prog> RegexpDFA::CompileRegexp(absl::string_view regexp) {
  // Build the RegexpDFA for only full match.
  std::string full_text_regexp(regexp);
  if (regexp.empty() || regexp[0] != '^')
    full_text_regexp = "^" + full_text_regexp;
  if (full_text_regexp.back() != '$') full_text_regexp += "$";

  re2::Regexp* re =
      re2::Regexp::Parse(full_text_regexp, re2::Regexp::LikePerl, nullptr);

  // Is the regexp valid?
  FUZZTEST_INTERNAL_CHECK_PRECONDITION(re != nullptr,
                                       "Invalid RE2 regular expression.");
  re2::Prog* prog = re->CompileToProg(0);
  FUZZTEST_INTERNAL_CHECK(prog != nullptr, "RE2 compilation failed!");
  re->Decref();
  return std::unique_ptr<re2::Prog>(prog);
}

void RegexpDFA::BuildEntireDFA(std::unique_ptr<re2::Prog> compiled_regexp) {
  // Transition table for the states.
  std::vector<std::vector<int>> transition_table;
  // Whether the state is a end state.
  std::vector<bool> end_vec;

  // Full match has no effect on re2::Prog::BuildEntireDFA.
  int state_n = compiled_regexp->BuildEntireDFA(
      re2::Prog::kFirstMatch, [&](const int* next, bool match) {
        FUZZTEST_INTERNAL_CHECK_PRECONDITION(
            next != nullptr,
            "The memory budget for building the state machine (DFA) for the "
            "given regular expression has been exhausted. "
            "You might try to reduce the number of states by using more "
            "specific character classes (e.g., [[:alpha:]] instead of `.`, "
            "i.e., any character) in your regular expression, or wait until "
            "the issue is fixed.");

        transition_table.emplace_back(
            next, next + compiled_regexp->bytemap_range() + 1);
        end_vec.push_back(match);
      });

  states_.resize(state_n);

  constexpr int kDeadState = -1;
  // Construct our own DFA graph.
  for (int i = 0; i < state_n; ++i) {
    std::vector<int>& transition_vec = transition_table[i];
    State& state = states_[i];

    for (int j = 0; j + 1 < transition_vec.size(); ++j) {
      // If `transition_vec[bytemap_idx] == state_id` at state `s`, it means
      // that given a character `c` whose bytemap index is `bytemap_idx`,
      // `s` will transition into state with id `state_id`. The bytemap index
      // that equals `bytemap_range()` means the end of the string.

      int next_state_id = transition_vec[j];
      if (next_state_id == kDeadState) continue;

      for (std::int16_t c = 0; c < 256; ++c) {
        if (j == compiled_regexp->bytemap()[c]) {
          state.next.push_back({{c}, next_state_id});
        }
      }
    }
    if (int final_state_id = transition_vec.back();
        final_state_id != kDeadState) {
      state.next.push_back({{kEndOfString}, final_state_id});
    }

    // Sort State::next so that we can use binary search in matching.
    std::sort(
        state.next.begin(), state.next.end(),
        [](const State::StateTransition& a, const State::StateTransition& b) {
          return a.chars_to_match.front() < b.chars_to_match.front();
        });
    if (end_vec[i]) end_state_id_ = i;
    FUZZTEST_INTERNAL_CHECK((!end_vec[i] || state.next.empty()),
                            "An end state must have no outgoing edges!");
  }
}

void RegexpDFA::ComputeEdgeWeights() {
  constexpr double kProbToSafeNode = 0.5;
  // A graph to record the predecessor states for every state.
  std::vector<std::vector<bool>> is_predecessor(
      states_.size(), std::vector<bool>(states_.size(), false));
  for (int i = 0; i < states_.size(); ++i) {
    for (auto& transition : states_[i].next) {
      is_predecessor[transition.next_state_id][i] = true;
    }
  }

  std::vector<bool> is_safe_node(states_.size(), false);
  // Starting from the end state, BFS to make all the state safe.
  std::queue<int> q;
  q.push(end_state_id_);
  do {
    int state_id = q.front();
    q.pop();
    if (is_safe_node[state_id]) continue;
    for (int j = 0; j < states_.size(); ++j) {
      if (is_predecessor[state_id][j]) q.push(j);
    }
    State& state = states_[state_id];
    std::vector<int> edge_to_safe_nodes;
    std::vector<int> edge_to_unsafe_nodes;
    for (int j = 0; j < state.next.size(); ++j) {
      if (is_safe_node[state.next[j].next_state_id]) {
        edge_to_safe_nodes.push_back(j);
      } else {
        edge_to_unsafe_nodes.push_back(j);
      }
    }
    FUZZTEST_INTERNAL_CHECK(
        state_id == end_state_id_ || !edge_to_safe_nodes.empty(),
        "A non-end node must have at least one safe edge");
    double probability_to_safe_node =
        edge_to_unsafe_nodes.empty() ? 1 : kProbToSafeNode;

    // Distribute `probability_to_safe_node` evenly to every edge that leads
    // to a safe node. Also distribute 1-`probability_to_safe_node` to the
    // unsafe edges.
    std::vector<double> edge_weights(state.next.size());
    for (int edge_index : edge_to_safe_nodes) {
      edge_weights[edge_index] = probability_to_safe_node /
                                 static_cast<double>(edge_to_safe_nodes.size());
    }
    for (int edge_index : edge_to_unsafe_nodes) {
      edge_weights[edge_index] =
          (1.0 - probability_to_safe_node) /
          static_cast<double>(edge_to_unsafe_nodes.size());
    }
    is_safe_node[state_id] = true;
    state.edge_weight_distribution = absl::discrete_distribution<int>(
        edge_weights.begin(), edge_weights.end());
  } while (!q.empty());
}

// Compress a state if the state has only one outgoing state. For example, if we
// have S0-(a)->S1-(b)->S2, then we can compress S1 to S0 and get S0-(ab)->S2.
// We do so by setting S0's outgoing edge to S2 and append S1's matching
// characters to S0's edge.
void RegexpDFA::CompressStates() {
  std::vector<bool> is_dead_state(states_.size(), false);

  // Skip the start state as it should never be compressed.
  for (size_t i = 1; i < states_.size(); ++i) {
    State& state = states_[i];
    if (state.next.size() != 1) continue;
    const auto& [chars_to_match, next_state_id] = state.next[0];
    FUZZTEST_INTERNAL_CHECK(
        next_state_id != i,
        "A self-loop state should have at least two outgoing edges.");
    for (size_t j = 0; j < states_.size(); ++j) {
      for (auto& [next_chars_to_match, state_id] : states_[j].next) {
        if (state_id == i) {
          next_chars_to_match.insert(next_chars_to_match.end(),
                                     chars_to_match.begin(),
                                     chars_to_match.end());
          state_id = next_state_id;
        }
      }
    }
    is_dead_state[i] = true;
  }

  // Remove dead states.
  absl::flat_hash_map<int, int> state_id_map;
  int live_state_num = 0;

  for (int i = 0; i < states_.size(); ++i) {
    if (is_dead_state[i]) continue;
    state_id_map[i] = live_state_num;
    if (states_[i].is_end_state()) end_state_id_ = live_state_num;
    if (live_state_num != i) states_[live_state_num] = std::move(states_[i]);
    ++live_state_num;
  }

  states_.resize(live_state_num);

  // Fix the indexes in `states`.
  for (State& state : states_) {
    for (auto& transition_edge : state.next) {
      transition_edge.next_state_id =
          state_id_map[transition_edge.next_state_id];
    }
  }
}

}  // namespace fuzztest::internal
