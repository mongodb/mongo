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

#include "./fuzztest/internal/domains/in_grammar_impl.h"

#include <cstddef>
#include <type_traits>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "./fuzztest/internal/serialization.h"

namespace fuzztest::internal::grammar {

void GroupElementByASTType(
    ASTNode& astnode,
    absl::flat_hash_map<ASTTypeId, std::vector<ASTNode*>>& groups) {
  groups[astnode.type_id].push_back(&astnode);
  return std::visit(
      [&groups](auto&& arg) {
        if constexpr (std::is_same_v<std::decay_t<decltype(arg)>,
                                     std::vector<ASTNode>>) {
          for (ASTNode& child : arg) {
            GroupElementByASTType(child, groups);
          }
        }
      },
      astnode.children);
}

size_t ASTNode::NodeCount() const {
  return std::visit(
      [](auto&& arg) {
        size_t result = 1;  // count self.
        if constexpr (std::is_same_v<std::decay_t<decltype(arg)>,
                                     std::vector<ASTNode>>) {
          for (const ASTNode& child : arg) {
            result += child.NodeCount();
          }
        }
        return result;
      },
      children);
}

IRObject WrapASTIntoIRObject(const ASTNode& astnode, IRObject parsed_child) {
  IRObject obj;
  auto& subs = obj.MutableSubs();
  subs.push_back(IRObject::FromCorpus(astnode.type_id));
  subs.push_back(IRObject::FromCorpus(astnode.children.index()));
  subs.emplace_back(parsed_child);
  return obj;
}

}  // namespace fuzztest::internal::grammar
