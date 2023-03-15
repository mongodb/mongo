// Copyright 2020 The Abseil Authors.
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
#include "absl/strings/internal/cord_internal.h"

#include <atomic>
#include <cassert>
#include <memory>

#include "absl/container/inlined_vector.h"
#include "absl/strings/internal/cord_rep_btree.h"
#include "absl/strings/internal/cord_rep_flat.h"
#include "absl/strings/internal/cord_rep_ring.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {

ABSL_CONST_INIT std::atomic<bool> cord_btree_enabled(kCordEnableBtreeDefault);
ABSL_CONST_INIT std::atomic<bool> cord_ring_buffer_enabled(
    kCordEnableRingBufferDefault);
ABSL_CONST_INIT std::atomic<bool> shallow_subcords_enabled(
    kCordShallowSubcordsDefault);
ABSL_CONST_INIT std::atomic<bool> cord_btree_exhaustive_validation(false);

void CordRep::Destroy(CordRep* rep) {
  assert(rep != nullptr);

  absl::InlinedVector<CordRep*, Constants::kInlinedVectorSize> pending;
  while (true) {
    assert(!rep->refcount.IsImmortal());
    if (rep->tag == CONCAT) {
      CordRepConcat* rep_concat = rep->concat();
      CordRep* right = rep_concat->right;
      if (!right->refcount.Decrement()) {
        pending.push_back(right);
      }
      CordRep* left = rep_concat->left;
      delete rep_concat;
      rep = nullptr;
      if (!left->refcount.Decrement()) {
        rep = left;
        continue;
      }
    } else if (rep->tag == BTREE) {
      CordRepBtree::Destroy(rep->btree());
      rep = nullptr;
    } else if (rep->tag == RING) {
      CordRepRing::Destroy(rep->ring());
      rep = nullptr;
    } else if (rep->tag == EXTERNAL) {
      CordRepExternal::Delete(rep);
      rep = nullptr;
    } else if (rep->tag == SUBSTRING) {
      CordRepSubstring* rep_substring = rep->substring();
      CordRep* child = rep_substring->child;
      delete rep_substring;
      rep = nullptr;
      if (!child->refcount.Decrement()) {
        rep = child;
        continue;
      }
    } else {
      CordRepFlat::Delete(rep);
      rep = nullptr;
    }

    if (!pending.empty()) {
      rep = pending.back();
      pending.pop_back();
    } else {
      break;
    }
  }
}

}  // namespace cord_internal
ABSL_NAMESPACE_END
}  // namespace absl
