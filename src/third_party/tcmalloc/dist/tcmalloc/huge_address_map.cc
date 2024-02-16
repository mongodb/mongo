// Copyright 2019 The TCMalloc Authors
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

#include "tcmalloc/huge_address_map.h"

#include <stdlib.h>

#include <algorithm>
#include <new>

#include "absl/base/internal/cycleclock.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

const HugeAddressMap::Node* HugeAddressMap::Node::next() const {
  const Node* n = right_;
  if (n) {
    while (n->left_) n = n->left_;
    return n;
  }

  n = parent_;
  const Node* last = this;
  while (n) {
    if (n->left_ == last) return n;
    last = n;
    n = n->parent_;
  }

  return nullptr;
}

HugeAddressMap::Node* HugeAddressMap::Node::next() {
  const Node* n = static_cast<const Node*>(this)->next();
  return const_cast<Node*>(n);
}

void HugeAddressMap::Node::Check(size_t* num_nodes, HugeLength* size) const {
  HugeLength longest = range_.len();
  *num_nodes += 1;
  *size += range_.len();

  if (left_) {
    // tree
    CHECK_CONDITION(left_->range_.start() < range_.start());
    // disjoint
    CHECK_CONDITION(left_->range_.end_addr() < range_.start_addr());
    // well-formed
    CHECK_CONDITION(left_->parent_ == this);
    // heap
    CHECK_CONDITION(left_->prio_ <= prio_);
    left_->Check(num_nodes, size);
    if (left_->longest_ > longest) longest = left_->longest_;
  }

  if (right_) {
    // tree
    CHECK_CONDITION(right_->range_.start() > range_.start());
    // disjoint
    CHECK_CONDITION(right_->range_.start_addr() > range_.end_addr());
    // well-formed
    CHECK_CONDITION(right_->parent_ == this);
    // heap
    CHECK_CONDITION(right_->prio_ <= prio_);
    right_->Check(num_nodes, size);
    if (right_->longest_ > longest) longest = right_->longest_;
  }

  CHECK_CONDITION(longest_ == longest);
}

const HugeAddressMap::Node* HugeAddressMap::first() const {
  const Node* n = root();
  if (!n) return nullptr;
  const Node* left = n->left_;
  while (left) {
    n = left;
    left = n->left_;
  }

  return n;
}

HugeAddressMap::Node* HugeAddressMap::first() {
  const Node* f = static_cast<const HugeAddressMap*>(this)->first();
  return const_cast<Node*>(f);
}

void HugeAddressMap::Check() {
  size_t nodes = 0;
  HugeLength size = NHugePages(0);
  if (root_) {
    CHECK_CONDITION(root_->parent_ == nullptr);
    root_->Check(&nodes, &size);
  }
  CHECK_CONDITION(nodes == nranges());
  CHECK_CONDITION(size == total_mapped());
  CHECK_CONDITION(total_nodes_ == used_nodes_ + freelist_size_);
}

size_t HugeAddressMap::nranges() const { return used_nodes_; }

HugeLength HugeAddressMap::total_mapped() const { return total_size_; }

void HugeAddressMap::Print(Printer* out) const {
  out->printf("HugeAddressMap: treap %zu / %zu nodes used / created\n",
              used_nodes_, total_nodes_);
  const size_t longest = root_ ? root_->longest_.raw_num() : 0;
  out->printf("HugeAddressMap: %zu contiguous hugepages available\n", longest);
}

void HugeAddressMap::PrintInPbtxt(PbtxtRegion* hpaa) const {
  hpaa->PrintI64("num_huge_address_map_treap_nodes_used", used_nodes_);
  hpaa->PrintI64("num_huge_address_map_treap_nodes_created", total_nodes_);
  const size_t longest = root_ ? root_->longest_.in_bytes() : 0;
  hpaa->PrintI64("contiguous_free_bytes", longest);
}

HugeAddressMap::Node* HugeAddressMap::Predecessor(HugePage p) {
  Node* n = root();
  Node* best = nullptr;
  while (n) {
    HugeRange here = n->range_;
    if (here.contains(p)) return n;
    if (p < here.start()) {
      // p comes before here:
      // our predecessor isn't here, nor in the right subtree.
      n = n->left_;
    } else {
      // p comes after here:
      // here is a valid candidate, and the right subtree might have better.
      best = n;
      n = n->right_;
    }
  }

  return best;
}

void HugeAddressMap::Merge(Node* b, HugeRange r, Node* a) {
  auto merge_when = [](HugeRange x, int64_t x_when, HugeRange y,
                       int64_t y_when) {
    // avoid overflow with floating-point
    const size_t x_len = x.len().raw_num();
    const size_t y_len = y.len().raw_num();
    const double x_weight = static_cast<double>(x_len) * x_when;
    const double y_weight = static_cast<double>(y_len) * y_when;
    return static_cast<int64_t>((x_weight + y_weight) / (x_len + y_len));
  };

  int64_t when = absl::base_internal::CycleClock::Now();
  // Two way merges are easy.
  if (a == nullptr) {
    b->when_ = merge_when(b->range_, b->when(), r, when);
    b->range_ = Join(b->range_, r);
    FixLongest(b);
    return;
  } else if (b == nullptr) {
    a->when_ = merge_when(r, when, a->range_, a->when());
    a->range_ = Join(r, a->range_);
    FixLongest(a);
    return;
  }

  // Three way merge: slightly harder.  We must remove one node
  // (arbitrarily picking next).
  HugeRange partial = Join(r, a->range_);
  int64_t partial_when = merge_when(r, when, a->range_, a->when());
  HugeRange full = Join(b->range_, partial);
  int64_t full_when = merge_when(b->range_, b->when(), partial, partial_when);
  // Removing a will reduce total_size_ by that length, but since we're merging
  // we actually don't change lengths at all; undo that.
  total_size_ += a->range_.len();
  Remove(a);
  b->range_ = full;
  b->when_ = full_when;
  FixLongest(b);
}

void HugeAddressMap::Insert(HugeRange r) {
  total_size_ += r.len();
  // First, try to merge if necessary. Note there are three possibilities:
  // we might need to merge before with r, r with after, or all three together.
  Node* before = Predecessor(r.start());
  CHECK_CONDITION(!before || !before->range_.intersects(r));
  Node* after = before ? before->next() : first();
  CHECK_CONDITION(!after || !after->range_.intersects(r));
  if (before && before->range_.precedes(r)) {
    if (after && r.precedes(after->range_)) {
      Merge(before, r, after);
    } else {
      Merge(before, r, nullptr);
    }
    return;
  } else if (after && r.precedes(after->range_)) {
    Merge(nullptr, r, after);
    return;
  }
  CHECK_CONDITION(!before || !before->range_.precedes(r));
  CHECK_CONDITION(!after || !r.precedes(after->range_));
  // No merging possible; just add a new node.
  Node* n = Get(r);
  Node* curr = root();
  Node* parent = nullptr;
  Node** link = &root_;
  // Walk down the tree to our correct location
  while (curr != nullptr && curr->prio_ >= n->prio_) {
    curr->longest_ = std::max(curr->longest_, r.len());
    parent = curr;
    if (curr->range_.start() < r.start()) {
      link = &curr->right_;
      curr = curr->right_;
    } else {
      link = &curr->left_;
      curr = curr->left_;
    }
  }
  *link = n;
  n->parent_ = parent;
  n->left_ = n->right_ = nullptr;
  n->longest_ = r.len();
  if (curr) {
    HugePage p = r.start();
    // We need to split the treap at curr into n's children.
    // This will be two treaps: one less than p, one greater, and has
    // a nice recursive structure.
    Node** less = &n->left_;
    Node* lp = n;
    Node** more = &n->right_;
    Node* mp = n;
    while (curr) {
      if (curr->range_.start() < p) {
        *less = curr;
        curr->parent_ = lp;
        less = &curr->right_;
        lp = curr;
        curr = curr->right_;
      } else {
        *more = curr;
        curr->parent_ = mp;
        more = &curr->left_;
        mp = curr;
        curr = curr->left_;
      }
    }
    *more = *less = nullptr;
    // We ripped apart the tree along these two paths--fix longest pointers.
    FixLongest(lp);
    FixLongest(mp);
  }
}

void HugeAddressMap::Node::FixLongest() {
  const HugeLength l = left_ ? left_->longest_ : NHugePages(0);
  const HugeLength r = right_ ? right_->longest_ : NHugePages(0);
  const HugeLength c = range_.len();
  const HugeLength new_longest = std::max({l, r, c});
  longest_ = new_longest;
}

void HugeAddressMap::FixLongest(HugeAddressMap::Node* n) {
  while (n) {
    n->FixLongest();
    n = n->parent_;
  }
}

void HugeAddressMap::Remove(HugeAddressMap::Node* n) {
  total_size_ -= n->range_.len();
  // We need to merge the left and right children of n into one
  // treap, then glue it into place wherever n was.
  Node** link;
  Node* parent = n->parent_;
  Node* top = n->left_;
  Node* bottom = n->right_;

  const HugeLength child_longest =
      std::max(top ? top->longest_ : NHugePages(0),
               bottom ? bottom->longest_ : NHugePages(0));
  if (!parent) {
    link = &root_;
  } else {
    // Account for the removed child--might change longests.
    // Easiest way: update this subtree to ignore the removed node,
    // then fix the chain of parents.
    n->longest_ = child_longest;
    FixLongest(parent);
    if (parent->range_.start() > n->range_.start()) {
      link = &parent->left_;
    } else {
      link = &parent->right_;
    }
  }

  // A routine op we'll need a lot: given two (possibly null)
  // children, put the root-ier one into top.
  auto reorder_maybe = [](Node** top, Node** bottom) {
    Node *b = *bottom, *t = *top;
    if (b && (!t || t->prio_ < b->prio_)) {
      *bottom = t;
      *top = b;
    }
  };

  reorder_maybe(&top, &bottom);
  // if we have two treaps to merge (top is always non-null if bottom is)
  // Invariant: top, bottom are two valid (longest included)
  // treaps. parent (and all above/elsewhere) have the correct longest
  // values, though parent does not have the correct children (will be the
  // merged value of top and bottom.)
  while (bottom) {
    *link = top;
    top->parent_ = parent;
    // We're merging bottom into top, so top might contain a longer
    // chunk than it thinks.
    top->longest_ = std::max(top->longest_, bottom->longest_);
    parent = top;
    if (bottom->range_.start() < top->range_.start()) {
      link = &top->left_;
      top = top->left_;
    } else {
      link = &top->right_;
      top = top->right_;
    }
    reorder_maybe(&top, &bottom);
  }
  *link = top;
  if (top) top->parent_ = parent;
  Put(n);
}

void HugeAddressMap::Put(Node* n) {
  freelist_size_++;
  used_nodes_--;
  n->left_ = freelist_;
  freelist_ = n;
}

HugeAddressMap::Node* HugeAddressMap::Get(HugeRange r) {
  CHECK_CONDITION((freelist_ == nullptr) == (freelist_size_ == 0));
  used_nodes_++;
  int prio = rand_r(&seed_);
  if (freelist_size_ == 0) {
    total_nodes_++;
    Node* ret = reinterpret_cast<Node*>(meta_(sizeof(Node)));
    return new (ret) Node(r, prio);
  }

  freelist_size_--;
  Node* ret = freelist_;
  freelist_ = ret->left_;
  return new (ret) Node(r, prio);
}

HugeAddressMap::Node::Node(HugeRange r, int prio)
    : range_(r), prio_(prio), when_(absl::base_internal::CycleClock::Now()) {}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
