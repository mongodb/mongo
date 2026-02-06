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

#ifndef FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_IN_REGEXP_IMPL_H_
#define FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_IN_REGEXP_IMPL_H_

#include <optional>
#include <string>
#include <string_view>

#include "absl/random/bit_gen_ref.h"
#include "absl/status/status.h"
#include "./fuzztest/internal/domains/domain_base.h"
#include "./fuzztest/internal/domains/regexp_dfa.h"
#include "./fuzztest/internal/serialization.h"
#include "./fuzztest/internal/type_support.h"

namespace fuzztest::internal {

class InRegexpImpl
    : public domain_implementor::DomainBase<InRegexpImpl, std::string,
                                            RegexpDFA::Path> {
 public:
  explicit InRegexpImpl(std::string_view regex_str);

  RegexpDFA::Path Init(absl::BitGenRef prng);

  // Strategy: Parse the input string into a path in the DFA. Pick a node in the
  // path and random walk from the node until we reach an end state or go back
  // to the original path.
  void Mutate(RegexpDFA::Path& path, absl::BitGenRef prng,
              const domain_implementor::MutationMetadata&, bool only_shrink);

  StringPrinter GetPrinter() const;

  value_type GetValue(const corpus_type& v) const;

  std::optional<corpus_type> FromValue(const value_type& v) const;

  std::optional<corpus_type> ParseCorpus(const IRObject& obj) const;

  IRObject SerializeCorpus(const corpus_type& path) const;

  absl::Status ValidateCorpusValue(const corpus_type& corpus_value) const;

 private:
  void ValidatePathRoundtrip(const RegexpDFA::Path& path) const;

  // Remove a random loop in the DFA path and return the string from the
  // modified path. A loop is a subpath that starts and ends with the same
  // state.
  bool ShrinkByRemoveLoop(absl::BitGenRef prng, RegexpDFA::Path& path);

  // Randomly pick a subpath and try to replace it with a shorter one. As this
  // might fail we keep trying until success or the maximum number of trials is
  // reached.
  bool ShrinkByFindShorterSubPath(absl::BitGenRef prng, RegexpDFA::Path& path);

  std::string regex_str_;
  RegexpDFA dfa_;
};

}  // namespace fuzztest::internal

#endif  // FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_IN_REGEXP_IMPL_H_
