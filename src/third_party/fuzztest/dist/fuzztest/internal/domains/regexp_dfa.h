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

#ifndef FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_REGEXP_DFA_H_
#define FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_REGEXP_DFA_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/random/bit_gen_ref.h"
#include "absl/random/discrete_distribution.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "re2/re2.h"

namespace fuzztest::internal {

// Represents the deterministic finite automaton (DFA) of a regular expression.
class RegexpDFA {
 public:
  struct State {
    bool is_end_state() const { return next.empty(); }
    // The special character `256` (kEndOfString) indicates the end of the input
    // string.
    struct StateTransition {
      std::vector<std::int16_t> chars_to_match;
      int next_state_id;
    };
    std::vector<StateTransition> next;
    // The weight represents the probability of an edge being chosen during
    // random walk. The larger the weight, the higher chance the edge gets
    // picked. An edge transitioning to a node that is more likely to reach the
    // end state has larger weight than that doesn't.
    absl::discrete_distribution<int> edge_weight_distribution;
  };

  // `edge_index` is the index for the outgoing edge in State::next
  struct Edge {
    int from_state_id;
    int edge_index;
  };

  using Path = std::vector<Edge>;

  static RegexpDFA Create(absl::string_view regexp);

  std::string GenerateString(absl::BitGenRef prng);

  // Randomly walk from the state of `from_state_id` to any state of
  // `to_state_ids` or an end state.
  Path FindPath(int from_state_id,
                const std::vector<std::optional<int>>& to_state_ids,
                absl::BitGenRef prng);

  // Randomly DFS from the state of `from_state_id` to the state of
  // `to_state_id`, trying to find a path of length less than, if no, equal to
  // `length`. If we have multiple such paths, randomly pick one of them. Since
  // this (nearly) fully explores all the paths, there is no big difference in
  // the efficiency. And we prefer DFS to BFS for better readability.
  Path FindPathWithinLengthDFS(int from_state_id, int to_state_id, int length,
                               absl::BitGenRef prng);

  absl::StatusOr<Path> StringToPath(absl::string_view s) const;

  absl::StatusOr<std::string> PathToString(const Path& path) const;

  size_t state_count() const { return states_.size(); }

  int end_state_id() const { return end_state_id_; }

 private:
  RegexpDFA() = default;

  // Given a state and the next input character, try to match the character and
  // return the index in State::next. Return `nullopt` if the matching fails.
  std::optional<int> NextState(const State& cur_state,
                               const std::vector<std::int16_t>& input_chars,
                               size_t& cur_index) const;
  static std::unique_ptr<re2::Prog> CompileRegexp(absl::string_view regexp);
  void BuildEntireDFA(std::unique_ptr<re2::Prog> compiled_regexp);

  // Assign weights (the probability of being picked during random walk)
  // for edges of the DFA so that very long strings are less likely.  All the
  // edge weights of a state sums to 1.  A node is "safe" if it has an high
  // chance (currently at least 50%, defined by `kProbToSafeNode`) to reach
  // closer to the end state. We separate the edges into safe and unsafe ones.
  // The safe edges transition to safe nodes. First we mark every end
  // states/nodes as safe nodes. Next we start BFS from the safe nodes. For each
  // node to be handled during the exploration, we assign
  // `kProbToSafeNode/num_safe_edges` to each safe edge and `(1 -
  // kProbToSafeNode)/num_of_unsafe_edges` to the unsafe ones. After that we can
  // mark the node as safe because it has now at least 50% chance to go to
  // another safe node, which is closer to the end nodes.
  void ComputeEdgeWeights();

  // Compress the DFA so that every state except the end states have at least
  // two outgoing states. With this condition, every non-ending states are good
  // candidates for mutation.
  void CompressStates();

  // We need a special character representing "end of string". This is necessary
  // to make sure that we have exact matches: i.e., that we always reach end
  // states with the "end of string".
  static constexpr std::int16_t kEndOfString = 256;

  std::vector<State> states_;
  int end_state_id_;
};

}  // namespace fuzztest::internal

#endif  // FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_REGEXP_DFA_H_
