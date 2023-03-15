// Copyright 2021 The Abseil Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/strings/internal/cord_rep_consume.h"

#include <array>
#include <utility>

#include "absl/container/inlined_vector.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/internal/cord_internal.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {

namespace {

// Unrefs the provided `substring`, and returns `substring->child`
// Adds or assumes a reference on `substring->child`
CordRep* ClipSubstring(CordRepSubstring* substring) {
  CordRep* child = substring->child;
  if (substring->refcount.IsOne()) {
    delete substring;
  } else {
    CordRep::Ref(child);
    CordRep::Unref(substring);
  }
  return child;
}

// Unrefs the provided `concat`, and returns `{concat->left, concat->right}`
// Adds or assumes a reference on `concat->left` and `concat->right`.
// Returns an array of 2 elements containing the left and right nodes.
std::array<CordRep*, 2> ClipConcat(CordRepConcat* concat) {
  std::array<CordRep*, 2> result{concat->left, concat->right};
  if (concat->refcount.IsOne()) {
    delete concat;
  } else {
    CordRep::Ref(result[0]);
    CordRep::Ref(result[1]);
    CordRep::Unref(concat);
  }
  return result;
}

void Consume(bool forward, CordRep* rep, ConsumeFn consume_fn) {
  size_t offset = 0;
  size_t length = rep->length;
  struct Entry {
    CordRep* rep;
    size_t offset;
    size_t length;
  };
  absl::InlinedVector<Entry, 40> stack;

  for (;;) {
    if (rep->tag == CONCAT) {
      std::array<CordRep*, 2> res = ClipConcat(rep->concat());
      CordRep* left = res[0];
      CordRep* right = res[1];

      if (left->length <= offset) {
        // Don't need left node
        offset -= left->length;
        CordRep::Unref(left);
        rep = right;
        continue;
      }

      size_t length_left = left->length - offset;
      if (length_left >= length) {
        // Don't need right node
        CordRep::Unref(right);
        rep = left;
        continue;
      }

      // Need both nodes
      size_t length_right = length - length_left;
      if (forward) {
        stack.push_back({right, 0, length_right});
        rep = left;
        length = length_left;
      } else {
        stack.push_back({left, offset, length_left});
        rep = right;
        offset = 0;
        length = length_right;
      }
    } else if (rep->tag == SUBSTRING) {
      offset += rep->substring()->start;
      rep = ClipSubstring(rep->substring());
    } else {
      consume_fn(rep, offset, length);
      if (stack.empty()) return;

      rep = stack.back().rep;
      offset = stack.back().offset;
      length = stack.back().length;
      stack.pop_back();
    }
  }
}

}  // namespace

void Consume(CordRep* rep, ConsumeFn consume_fn) {
  return Consume(true, rep, std::move(consume_fn));
}

void ReverseConsume(CordRep* rep, ConsumeFn consume_fn) {
  return Consume(false, rep, std::move(consume_fn));
}

}  // namespace cord_internal
ABSL_NAMESPACE_END
}  // namespace absl
