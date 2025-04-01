/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The methods 'AvlTreeImpl::insert_worker' and 'AvlTreeImpl::delete_worker',
// and all supporting methods reachable from them, are derived from a public
// domain implementation by Georg Kraml.  The public domain implementation in
// C was translated into Rust and the Rust translation was later translated
// into this C++ implementation.
//
// Unfortunately the relevant web site for the original C version is long
// gone, and can only be found on the Wayback Machine:
//
//   https://web.archive.org/web/20010419134337/
//     http://www.kraml.at/georg/avltree/index.html
//
//   https://web.archive.org/web/20030926063347/
//     http://www.kraml.at/georg/avltree/avlmonolithic.c
//
//   https://web.archive.org/web/20030401124003/http://www.kraml.at/src/howto/
//
// The intermediate Rust translation can be found at
//
// https://github.com/bytecodealliance/regalloc.rs/blob/main/lib/src/avl_tree.rs
//
// For relicensing clearances, see Mozilla bugs 1620332 and 1769261:
//
//   https://bugzilla.mozilla.org/show_bug.cgi?id=1620332
//   https://bugzilla.mozilla.org/show_bug.cgi?id=1769261
//
// All other code in this file originates from Mozilla.

#ifndef ds_AvlTree_h
#define ds_AvlTree_h

#include "mozilla/Attributes.h"
#include "mozilla/Likely.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "ds/LifoAlloc.h"

namespace js {

////////////////////////////////////////////////////////////////////////
//                                                                    //
// AvlTree implementation.  For interface see `class AvlTree` below.  //
//                                                                    //
////////////////////////////////////////////////////////////////////////

// An AVL tree class, with private allocator and node-recycling.  `T` is the
// class of elements in the tree.  `C` must provide a method
//
//   static int compare(const T&, const T&)
//
// to provide a total ordering on values `T` that are put into the tree,
// returning -1 for less-than, 0 for equal, and 1 for greater-than.
//
// `C::compare` does not have to be a total ordering for *all* values of `T`,
// but it must be so for the `T` values in the tree.  Requests to insert
// duplicate `T` values, as determined equal by `C::compare`, are valid but
// will be ignored in this implementation class: the stored data is unchanged.
// The interface class `AvlTree` however will MOZ_CRASH() on such requests.
//
// `T` values stored in the tree will not be explicitly freed or destroyed.
//
// For best cache-friendlyness, try to put the fields of `T` that are read by
// your compare function at the end of `T`.  See comment on `struct Node`
// below.
//
// Some operations require (internally) building a stack of tree nodes from
// the root to some leaf.  The maximum stack size, and hence the maximum tree
// depth, is currently bounded at 48.  The max depth of an AVL tree is roughly
// 1.44 * log2(# nodes), so providing the tree-balancing machinery works
// correctly, the max number of nodes is at least 2^(48 / 1.44), somewhat over
// 2^33 (= 8 G).  On a 32-bit target we'll run out of address space long
// before reaching that.  On a 64-bit target, the minimum imaginable
// sizeof(Node) is 16 (for the two pointers), so a tree with 2^33 nodes would
// occupy at least 2^37 bytes, viz, around 137GB.  So this seems unlikely to
// be a limitation.
//
// All stack-pushing operations are release-asserted to not overflow the stack.

template <class T, class C>
class AvlTreeImpl {
  // This is the implementation of AVL trees.  If you want to know how to use
  // them in your code, don't read this; instead look below at the public
  // interface, that is, `class AvlTree`.
  //
  // All of `AvlTreeImpl`, apart from the iterator code at the bottom, is
  // protected.  Public facilities are provided by child class `AvlTree`.
 protected:
  // Tree node tags.
  enum class Tag {
    Free,   // Node not in use -- is on the freelist.
    None,   // Node in use.  Neither subtree is deeper.
    Left,   // Node in use.  The left subtree is deeper.
    Right,  // Node in use.  The right subtree is deeper.

    Count,  // Not used as an actual tag - should remain last in this list
  };

  // Tree nodes. The tag is stored in the lower bits of the right Node pointer
  // rather than as a separate field. For types T with alignment no more than
  // twice the size of a pointer (ie, most types), this reduces the size of Node
  // and enables them to pack more tightly, reducing memory requirements and
  // improving cache behavior. (See bug 1847616.)
  struct Node {
    T item;
    Node* left;

    // This is the mask to use to extract the tag from `rightAndTag`.
    static constexpr uintptr_t kTagMask = 3;
    static_assert(mozilla::IsPowerOfTwo(kTagMask + 1),
                  "kTagMask must only have a consecutive sequence of its "
                  "lowest bits set");
    static_assert(
        kTagMask >= static_cast<uintptr_t>(Tag::Count) - 1,
        "kTagMask must be sufficient to cover largest value in 'Tag'");

   private:
    uintptr_t rightAndTag;

   public:
    explicit Node(const T& item)
        : item(item),
          left(nullptr),
          rightAndTag(static_cast<uintptr_t>(Tag::None)) {}

    [[nodiscard]] Node* getRight() const {
      return reinterpret_cast<Node*>(rightAndTag & ~kTagMask);
    }
    [[nodiscard]] Tag getTag() const {
      return static_cast<Tag>(rightAndTag & kTagMask);
    }

    void setRight(Node* right) {
      rightAndTag =
          reinterpret_cast<uintptr_t>(right) | static_cast<uintptr_t>(getTag());
    }
    void setTag(const Tag tag) {
      rightAndTag = (rightAndTag & ~kTagMask) | static_cast<uintptr_t>(tag);
    }
    void setRightAndTag(Node* right, const Tag tag) {
      const uintptr_t rightAsUint = reinterpret_cast<uintptr_t>(right);
      rightAndTag = rightAsUint | static_cast<uintptr_t>(tag);
    }
  };

  // Ensure that we can use the needed lower bits of a Node pointer to store the
  // tag.
  static_assert(alignof(Node) >= Node::kTagMask + 1);

  // Once-per-tree components.
  Node* root_;
  Node* freeList_;
  LifoAlloc* alloc_;

  // As a modest but easy optimisation, ::allocateNode will allocate one node
  // at the first call that sees an empty `freeList_`, two on the next such
  // call and four on subsequent such calls.  This has the effect of reducing
  // the number of calls to the underlying allocator `alloc_` by a factor of 4
  // for all but the smallest trees.  It also helps pack more nodes into each
  // cache line.  The limit of 4 exists for three reasons:
  //
  // (1) It gains the majority (75%) of the available benefit from reducing
  // the number of calls to `alloc_`, as the allocation size tends to
  // infinity.
  //
  // (2) Similarly, 4 `struct Node`s will surely be greater than 128 bytes,
  // hence there is minimal chance to use even fewer cache lines by increasing
  // the group size further.  In any case most machines have cache lines of
  // size 64 bytes, not 128.
  //
  // (3) Most importantly, it limits the maximum potentially wasted space,
  // which is the case where a request causes an allocation of N nodes, of
  // which one is used immediately and the N-1 are put on the freelist, but
  // then -- because the tree never grows larger -- are never used.  Given
  // that N=4 here, the worst case lossage is 3 nodes, which seems tolerable.
  uint32_t nextAllocSize_;  // 1, 2 or 4 only

  // The expected maximum tree depth.  See comments above.
  static const size_t MAX_TREE_DEPTH = 48;

  AvlTreeImpl(const AvlTreeImpl&) = delete;
  AvlTreeImpl& operator=(const AvlTreeImpl&) = delete;

  // ---- Preliminaries --------------------------------------- //

  explicit AvlTreeImpl(LifoAlloc* alloc = nullptr)
      : root_(nullptr), freeList_(nullptr), alloc_(alloc), nextAllocSize_(1) {}

  void setAllocator(LifoAlloc* alloc) { alloc_ = alloc; }

  // Put `node` onto the free list, for possible later reuse.
  inline void addToFreeList(Node* node) {
    node->left = freeList_;
    node->setRightAndTag(nullptr, Tag::Free);  // for safety
    freeList_ = node;
  }

  // A safer version of `addToFreeList`.
  inline void freeNode(Node* node) {
    MOZ_ASSERT(node->getTag() != Tag::Free);
    addToFreeList(node);
  }

  // This is the slow path for ::allocateNode below.  Allocate 1, 2 or 4 nodes
  // as a block, return the first one properly initialised, and put the rest
  // on the freelist, in increasing order of address.
  MOZ_NEVER_INLINE Node* allocateNodeOOL(const T& v) {
    switch (nextAllocSize_) {
      case 1: {
        nextAllocSize_ = 2;
        Node* node = alloc_->new_<Node>(v);
        // `node` is either fully initialized, or nullptr on OOM.
        return node;
      }
      case 2: {
        nextAllocSize_ = 4;
        Node* nodes = alloc_->newArrayUninitialized<Node>(2);
        if (!nodes) {
          return nullptr;
        }
        Node* node0 = &nodes[0];
        addToFreeList(&nodes[1]);
        new (node0) Node(v);
        return node0;
      }
      case 4: {
        Node* nodes = alloc_->newArrayUninitialized<Node>(4);
        if (!nodes) {
          return nullptr;
        }
        Node* node0 = &nodes[0];
        addToFreeList(&nodes[3]);
        addToFreeList(&nodes[2]);
        addToFreeList(&nodes[1]);
        new (node0) Node(v);
        return node0;
      }
      default: {
        MOZ_CRASH();
      }
    }
  }

  // Allocate a Node holding `v`, or return nullptr on OOM.  All of the fields
  // are initialized.
  inline Node* allocateNode(const T& v) {
    Node* node = freeList_;
    if (MOZ_LIKELY(node)) {
      MOZ_ASSERT(node->getTag() == Tag::Free);
      freeList_ = node->left;
      new (node) Node(v);
      return node;
    }
    return allocateNodeOOL(v);
  }

  // These exist only transiently, to aid rebalancing.  They indicate whether
  // an insertion/deletion succeeded, whether subsequent rebalancing is
  // needed.
  enum class Result { Error, OK, Balance };

  using NodeAndResult = std::pair<Node*, Result>;

  // Standard AVL single-rotate-left
  Node* rotate_left(Node* old_root) {
    Node* new_root = old_root->getRight();
    old_root->setRight(new_root->left);
    new_root->left = old_root;
    return new_root;
  }

  // Standard AVL single-rotate-right
  Node* rotate_right(Node* old_root) {
    Node* new_root = old_root->left;
    old_root->left = new_root->getRight();
    new_root->setRight(old_root);
    return new_root;
  }

  // ---- Helpers for insertion ------------------------------- //

  // `leftgrown`: a helper function for `insert_worker`
  //
  // Parameters:
  //
  //   root   Root of a tree.  This node's left subtree has just grown due to
  //          item insertion; its "tag" flag needs adjustment, and the local
  //          tree (the subtree of which this node is the root node) may have
  //          become unbalanced.
  //
  // Return values:
  //
  //   The new root of the subtree, plus either:
  //
  //   OK       The local tree could be rebalanced or was balanced from the
  //            start.  The caller, insert_worker, may assume the entire tree
  //            is valid.
  //   or
  //   Balance  The local tree was balanced, but has grown in height.
  //            Do not assume the entire tree is valid.
  //
  // This function has been split into two pieces: `leftgrown`, which is small
  // and hot, and is marked always-inline, and `leftgrown_left`, which handles
  // a more complex and less frequent case, and is marked never-inline.  The
  // intent is to have the common case always inlined without having to deal
  // with the extra register pressure from inlining the less frequent code.
  // The dual function `rightgrown` is split similarly.

  MOZ_NEVER_INLINE Node* leftgrown_left(Node* root) {
    if (root->left->getTag() == Tag::Left) {
      root->setTag(Tag::None);
      root->left->setTag(Tag::None);
      root = rotate_right(root);
    } else {
      switch (root->left->getRight()->getTag()) {
        case Tag::Left:
          root->setTag(Tag::Right);
          root->left->setTag(Tag::None);
          break;
        case Tag::Right:
          root->setTag(Tag::None);
          root->left->setTag(Tag::Left);
          break;
        case Tag::None:
          root->setTag(Tag::None);
          root->left->setTag(Tag::None);
          break;
        case Tag::Free:
        default:
          MOZ_CRASH();
      }
      root->left->getRight()->setTag(Tag::None);
      root->left = rotate_left(root->left);
      root = rotate_right(root);
    }
    return root;
  }

  inline NodeAndResult leftgrown(Node* root) {
    switch (root->getTag()) {
      case Tag::Left:
        return NodeAndResult(leftgrown_left(root), Result::OK);
      case Tag::Right:
        root->setTag(Tag::None);
        return NodeAndResult(root, Result::OK);
      case Tag::None:
        root->setTag(Tag::Left);
        return NodeAndResult(root, Result::Balance);
      case Tag::Free:
      default:
        break;
    }
    MOZ_CRASH();
  }

  // `rightgrown`: a helper function for `insert_worker`.  See `leftgrown` for
  // details.

  MOZ_NEVER_INLINE Node* rightgrown_right(Node* root) {
    if (root->getRight()->getTag() == Tag::Right) {
      root->setTag(Tag::None);
      root->getRight()->setTag(Tag::None);
      root = rotate_left(root);
    } else {
      switch (root->getRight()->left->getTag()) {
        case Tag::Right:
          root->setTag(Tag::Left);
          root->getRight()->setTag(Tag::None);
          break;
        case Tag::Left:
          root->setTag(Tag::None);
          root->getRight()->setTag(Tag::Right);
          break;
        case Tag::None:
          root->setTag(Tag::None);
          root->getRight()->setTag(Tag::None);
          break;
        case Tag::Free:
        default:
          MOZ_CRASH();
      }
      root->getRight()->left->setTag(Tag::None);
      root->setRight(rotate_right(root->getRight()));
      root = rotate_left(root);
    }
    return root;
  }

  inline NodeAndResult rightgrown(Node* root) {
    switch (root->getTag()) {
      case Tag::Left:
        root->setTag(Tag::None);
        return NodeAndResult(root, Result::OK);
      case Tag::Right:
        return NodeAndResult(rightgrown_right(root), Result::OK);
      case Tag::None:
        root->setTag(Tag::Right);
        return NodeAndResult(root, Result::Balance);
      case Tag::Free:
      default:
        break;
    }
    MOZ_CRASH();
  }

  // ---- Insertion ------------------------------------------- //

  // Worker for insertion.  Allocates a node for `v` and inserts it into the
  // tree.  Returns: nullptr for OOM; (Node*)1 if `v` is a duplicate (per
  // `C::compare`), in which case the tree is unchanged; otherwise (successful
  // insertion) the new root.  In the latter case, the new `item` field is
  // initialised from `v`.
  Node* insert_worker(const T& v) {
    // Insertion is a two pass process.  In the first pass, we descend from
    // the root, looking for the place in the tree where the new node will go,
    // and at the same time storing the sequence of visited nodes in a stack.
    // In the second phase we re-ascend the tree, as guided by the stack,
    // rebalancing as we go.
    //
    // Note, we start from `root_`, but that isn't updated at the end.  Instead
    // the new value is returned to the caller, which has to do the update.

    Node* stack[MAX_TREE_DEPTH];
    size_t stackPtr = 0;  // points to next available slot

#define STACK_ENTRY_SET_IS_LEFT(_nodePtr) \
  ((Node*)(uintptr_t(_nodePtr) | uintptr_t(1)))
#define STACK_ENTRY_GET_IS_LEFT(_ent) ((bool)(uintptr_t(_ent) & uintptr_t(1)))
#define STACK_ENTRY_GET_NODE(_ent) ((Node*)(uintptr_t(_ent) & ~uintptr_t(1)))

    // In the first phase, walk down the tree to find the place where the new
    // node should be inserted, recording our path in `stack`.  This loop has
    // a factor-of-2 unrolling (the loop body contains two logical iterations)
    // in order to reduce the overall cost of the stack-overflow check at the
    // bottom.
    Node* node = root_;
    while (true) {
      // First logical iteration
      if (!node) {
        break;
      }
      int cmpRes1 = C::compare(v, node->item);
      if (cmpRes1 < 0) {
        stack[stackPtr++] = STACK_ENTRY_SET_IS_LEFT(node);
        node = node->left;
      } else if (cmpRes1 > 0) {
        stack[stackPtr++] = node;
        node = node->getRight();
      } else {
        // `v` is already in the tree.  Inform the caller, and don't change
        // the tree.
        return (Node*)(uintptr_t(1));
      }
      // Second logical iteration
      if (!node) {
        break;
      }
      int cmpRes2 = C::compare(v, node->item);
      if (cmpRes2 < 0) {
        stack[stackPtr++] = STACK_ENTRY_SET_IS_LEFT(node);
        node = node->left;
      } else if (cmpRes2 > 0) {
        stack[stackPtr++] = node;
        node = node->getRight();
      } else {
        return (Node*)(uintptr_t(1));
      }
      // We're going around again.  Ensure there are at least two available
      // stack slots.
      MOZ_RELEASE_ASSERT(stackPtr < MAX_TREE_DEPTH - 2);
    }
    MOZ_ASSERT(!node);

    // Now allocate the new node.
    Node* new_node = allocateNode(v);
    if (!new_node) {
      return nullptr;  // OOM
    }

    // And unwind the stack, back to the root, rebalancing as we go.  Once get
    // to a place where the new subtree doesn't need to be rebalanced, we can
    // stop this upward scan, because no nodes above it will need to be
    // rebalanced either.
    Node* curr_node = new_node;
    Result curr_node_action = Result::Balance;

    while (stackPtr > 0) {
      Node* parent_node_tagged = stack[--stackPtr];
      Node* parent_node = STACK_ENTRY_GET_NODE(parent_node_tagged);
      if (STACK_ENTRY_GET_IS_LEFT(parent_node_tagged)) {
        parent_node->left = curr_node;
        if (curr_node_action == Result::Balance) {
          auto pair = leftgrown(parent_node);
          curr_node = pair.first;
          curr_node_action = pair.second;
        } else {
          curr_node = parent_node;
          break;
        }
      } else {
        parent_node->setRight(curr_node);
        if (curr_node_action == Result::Balance) {
          auto pair = rightgrown(parent_node);
          curr_node = pair.first;
          curr_node_action = pair.second;
        } else {
          curr_node = parent_node;
          break;
        }
      }
    }

    if (stackPtr > 0) {
      curr_node = STACK_ENTRY_GET_NODE(stack[0]);
    }
    MOZ_ASSERT(curr_node);

#undef STACK_ENTRY_SET_IS_LEFT
#undef STACK_ENTRY_GET_IS_LEFT
#undef STACK_ENTRY_GET_NODE
    return curr_node;
  }

  // ---- Helpers for deletion -------------------------------- //

  // `leftshrunk`: a helper function for `delete_worker` and `findlowest`
  //
  // Parameters:
  //
  //   n  Pointer to a node.  The node's left subtree has just shrunk due to
  //      item removal; its "skew" flag needs adjustment, and the local tree
  //      (the subtree of which this node is the root node) may have become
  //      unbalanced.
  //
  // Return values:
  //
  //   (jseward: apparently some node, but what is it?), plus either:
  //
  //   OK       The parent activation of the delete activation that called
  //            this function may assume the entire tree is valid.
  //
  //   Balance  Do not assume the entire tree is valid.

  NodeAndResult leftshrunk(Node* n) {
    switch (n->getTag()) {
      case Tag::Left: {
        n->setTag(Tag::None);
        return NodeAndResult(n, Result::Balance);
      }
      case Tag::Right: {
        if (n->getRight()->getTag() == Tag::Right) {
          n->setTag(Tag::None);
          n->getRight()->setTag(Tag::None);
          n = rotate_left(n);
          return NodeAndResult(n, Result::Balance);
        } else if (n->getRight()->getTag() == Tag::None) {
          n->setTag(Tag::Right);
          n->getRight()->setTag(Tag::Left);
          n = rotate_left(n);
          return NodeAndResult(n, Result::OK);
        } else {
          switch (n->getRight()->left->getTag()) {
            case Tag::Left:
              n->setTag(Tag::None);
              n->getRight()->setTag(Tag::Right);
              break;
            case Tag::Right:
              n->setTag(Tag::Left);
              n->getRight()->setTag(Tag::None);
              break;
            case Tag::None:
              n->setTag(Tag::None);
              n->getRight()->setTag(Tag::None);
              break;
            case Tag::Free:
            default:
              MOZ_CRASH();
          }
          n->getRight()->left->setTag(Tag::None);
          n->setRight(rotate_right(n->getRight()));
          ;
          n = rotate_left(n);
          return NodeAndResult(n, Result::Balance);
        }
        /*NOTREACHED*/ MOZ_CRASH();
      }
      case Tag::None: {
        n->setTag(Tag::Right);
        return NodeAndResult(n, Result::OK);
      }
      case Tag::Free:
      default: {
        MOZ_CRASH();
      }
    }
    MOZ_CRASH();
  }

  // rightshrunk: a helper function for `delete` and `findhighest`.  See
  // `leftshrunk` for details.

  NodeAndResult rightshrunk(Node* n) {
    switch (n->getTag()) {
      case Tag::Right: {
        n->setTag(Tag::None);
        return NodeAndResult(n, Result::Balance);
      }
      case Tag::Left: {
        if (n->left->getTag() == Tag::Left) {
          n->setTag(Tag::None);
          n->left->setTag(Tag::None);
          n = rotate_right(n);
          return NodeAndResult(n, Result::Balance);
        } else if (n->left->getTag() == Tag::None) {
          n->setTag(Tag::Left);
          n->left->setTag(Tag::Right);
          n = rotate_right(n);
          return NodeAndResult(n, Result::OK);
        } else {
          switch (n->left->getRight()->getTag()) {
            case Tag::Left:
              n->setTag(Tag::Right);
              n->left->setTag(Tag::None);
              break;
            case Tag::Right:
              n->setTag(Tag::None);
              n->left->setTag(Tag::Left);
              break;
            case Tag::None:
              n->setTag(Tag::None);
              n->left->setTag(Tag::None);
              break;
            case Tag::Free:
            default:
              MOZ_CRASH();
          }
          n->left->getRight()->setTag(Tag::None);
          n->left = rotate_left(n->left);
          n = rotate_right(n);
          return NodeAndResult(n, Result::Balance);
        }
        /*NOTREACHED*/ MOZ_CRASH();
      }
      case Tag::None: {
        n->setTag(Tag::Left);
        return NodeAndResult(n, Result::OK);
      }
      case Tag::Free:
      default: {
        MOZ_CRASH();
      }
    }
    MOZ_CRASH();
  }

  // `findhighest`: helper function for `delete_worker`.  It replaces a node
  // with a subtree's greatest (per C::compare) item.
  //
  // Parameters:
  //
  //   target  Pointer to node to be replaced.
  //
  //   n       Address of pointer to subtree.
  //
  // Return value:
  //
  //   Nothing  The target node could not be replaced because the subtree
  //            provided was empty.
  //
  //   Some(Node*,Result)  jseward: it's pretty unclear, but I *think* it
  //                       is a pair that has the same meaning as the
  //                       pair returned by `leftgrown`, as described above.

  mozilla::Maybe<NodeAndResult> findhighest(Node* target, Node* n) {
    if (n == nullptr) {
      return mozilla::Nothing();
    }
    auto res = Result::Balance;
    if (n->getRight() != nullptr) {
      auto fhi = findhighest(target, n->getRight());
      if (fhi.isSome()) {
        n->setRight(fhi.value().first);
        res = fhi.value().second;
        if (res == Result::Balance) {
          auto pair = rightshrunk(n);
          n = pair.first;
          res = pair.second;
        }
        return mozilla::Some(NodeAndResult(n, res));
      } else {
        return mozilla::Nothing();
      }
    }
    target->item = n->item;
    Node* tmp = n;
    n = n->left;
    freeNode(tmp);
    return mozilla::Some(NodeAndResult(n, res));
  }

  // `findhighest`: helper function for `delete_worker`.  It replaces a node
  // with a subtree's greatest (per C::compare) item.  See `findhighest` for
  // details.

  mozilla::Maybe<NodeAndResult> findlowest(Node* target, Node* n) {
    if (n == nullptr) {
      return mozilla::Nothing();
    }
    Result res = Result::Balance;
    if (n->left != nullptr) {
      auto flo = findlowest(target, n->left);
      if (flo.isSome()) {
        n->left = flo.value().first;
        res = flo.value().second;
        if (res == Result::Balance) {
          auto pair = leftshrunk(n);
          n = pair.first;
          res = pair.second;
        }
        return mozilla::Some(NodeAndResult(n, res));
      } else {
        return mozilla::Nothing();
      }
    }
    target->item = n->item;
    Node* tmp = n;
    n = n->getRight();
    freeNode(tmp);
    return mozilla::Some(NodeAndResult(n, res));
  }

  // ---- Deletion -------------------------------------------- //

  // Deletes the node matching `item` from an arbitrary subtree rooted at
  // `node`.  Returns the root of the new subtree (if any), a `Result` that
  // indicates that either, the tree containing `node` does or does not need
  // rebalancing (::Balance, ::OK) or that the item was not found (::Error).

  NodeAndResult delete_worker(Node* node, const T& item) {
    Result tmp = Result::Balance;
    if (node == nullptr) {
      return NodeAndResult(node, Result::Error);
    }

    int cmp_res = C::compare(item, node->item);
    if (cmp_res < 0) {
      auto pair1 = delete_worker(node->left, item);
      node->left = pair1.first;
      tmp = pair1.second;
      if (tmp == Result::Balance) {
        auto pair2 = leftshrunk(node);
        node = pair2.first;
        tmp = pair2.second;
      }
      return NodeAndResult(node, tmp);
    } else if (cmp_res > 0) {
      auto pair1 = delete_worker(node->getRight(), item);
      node->setRight(pair1.first);
      tmp = pair1.second;
      if (tmp == Result::Balance) {
        auto pair2 = rightshrunk(node);
        node = pair2.first;
        tmp = pair2.second;
      }
      return NodeAndResult(node, tmp);
    } else {
      if (node->left != nullptr) {
        auto fhi = findhighest(node, node->left);
        if (fhi.isSome()) {
          node->left = fhi.value().first;
          tmp = fhi.value().second;
          if (tmp == Result::Balance) {
            auto pair = leftshrunk(node);
            node = pair.first;
            tmp = pair.second;
          }
        }
        return NodeAndResult(node, tmp);
      }
      if (node->getRight() != nullptr) {
        auto flo = findlowest(node, node->getRight());
        if (flo.isSome()) {
          node->setRight(flo.value().first);
          tmp = flo.value().second;
          if (tmp == Result::Balance) {
            auto pair = rightshrunk(node);
            node = pair.first;
            tmp = pair.second;
          }
        }
        return NodeAndResult(node, tmp);
      }
      freeNode(node);
      return NodeAndResult(nullptr, Result::Balance);
    }
  }

  // ---- Lookup ---------------------------------------------- //

  // Find the node matching `v`, or return nullptr if not found.
  Node* find_worker(const T& v) const {
    Node* node = root_;
    while (node) {
      int cmpRes = C::compare(v, node->item);
      if (cmpRes < 0) {
        node = node->left;
      } else if (cmpRes > 0) {
        node = node->getRight();
      } else {
        return node;
      }
    }
    return nullptr;
  }

  // ---- Iteration ------------------------------------------- //

 public:
  // This provides iteration forwards over the tree.  You can either iterate
  // over the whole tree or specify a start point.  To iterate over the whole
  // tree:
  //
  //   AvlTree<MyT,MyC> tree;
  //   .. put stuff into `tree` ..
  //
  //   AvlTree<MyT,MyC>::Iter iter(&tree);
  //   while (iter.hasMore) {
  //     MyT item = iter.next();
  //   }
  //
  // Alternatively you can initialize the iterator with some value `startAt`,
  // so that the first value you get is greater than or equal to `startAt`,
  // per `MyC::compare`:
  //
  //   AvlTree<MyT,MyC>::Iter iter(&tree, startAt);
  //
  // Starting the iterator at a particular value requires finding the value in
  // the tree and recording the path to it.  So it's nearly as cheap as a call
  // to `AvlTree::contains` and you can use it as a plausible substitute for
  // `::contains` if you want.
  //
  // Note that `class Iter` is quite large -- around 50 machine words -- so
  // you might want to think twice before allocating instances on the heap.
  class Iter {
    const AvlTreeImpl<T, C>* tree_;
    Node* stack_[MAX_TREE_DEPTH];
    size_t stackPtr_;

    // This sets up the iterator stack so that the first value it produces
    // will be the smallest value that is greater than or equal to `v`.  Note
    // the structural similarity to ::find_worker above.
    //
    // The logic for pushing nodes on the stack looks strange at first.  Once
    // set up, the stack contains a root-to-some-node path, and the
    // top-of-stack value is the next value the iterator will emit.  If the
    // stack becomes empty then the iteration is complete.
    //
    // It's not quite accurate to say that the stack contains a complete
    // root-to-some-node path.  Rather, the stack contains such a path, except
    // it omits nodes at which the path goes to the right child.  Eg:
    //
    //          5
    //     3         8
    //   1   4     7   9
    //
    // If the next item to be emitted is 4, then the stack will be [5, 4] and
    // not [5, 3, 4], because at 3 we go right.  This explains why the
    // `cmpRes > 0` case in `setupIteratorStack` doesn't push an item on the
    // stack.  It also explains why the single-argument `Iter::Iter` below,
    // which sets up for iteration starting at the lowest element, simply
    // calls `visitLeftChildren` to do its work.
    void setupIteratorStack(Node* node, const T& v) {
      // Ensure stackPtr_ is cached in a register, since this function can be
      // hot.
      MOZ_ASSERT(stackPtr_ == 0);
      size_t stackPtr = 0;
      while (node) {
        int cmpRes = C::compare(v, node->item);
        if (cmpRes < 0) {
          stack_[stackPtr++] = node;
          MOZ_RELEASE_ASSERT(stackPtr < MAX_TREE_DEPTH);
          node = node->left;
        } else if (cmpRes > 0) {
          node = node->getRight();
        } else {
          stack_[stackPtr++] = node;
          MOZ_RELEASE_ASSERT(stackPtr < MAX_TREE_DEPTH);
          break;
        }
      }
      stackPtr_ = stackPtr;
    }

    void visitLeftChildren(Node* node) {
      while (true) {
        Node* left = node->left;
        if (left == nullptr) {
          break;
        }
        stack_[stackPtr_++] = left;
        MOZ_RELEASE_ASSERT(stackPtr_ < MAX_TREE_DEPTH);
        node = left;
      }
    }

   public:
    explicit Iter(const AvlTreeImpl<T, C>* tree) {
      tree_ = tree;
      stackPtr_ = 0;
      if (tree->root_ != nullptr) {
        stack_[stackPtr_++] = tree->root_;
        MOZ_RELEASE_ASSERT(stackPtr_ < MAX_TREE_DEPTH);
        visitLeftChildren(tree->root_);
      }
    }
    Iter(const AvlTreeImpl<T, C>* tree, const T& startAt) {
      tree_ = tree;
      stackPtr_ = 0;
      setupIteratorStack(tree_->root_, startAt);
    }
    bool hasMore() const { return stackPtr_ > 0; }
    T next() {
      MOZ_RELEASE_ASSERT(stackPtr_ > 0);
      Node* ret = stack_[--stackPtr_];
      Node* right = ret->getRight();
      if (right != nullptr) {
        stack_[stackPtr_++] = right;
        MOZ_RELEASE_ASSERT(stackPtr_ < MAX_TREE_DEPTH);
        visitLeftChildren(right);
      }
      return ret->item;
    }
  };
};

////////////////////////////////////////////////////////////////////////
//                                                                    //
// AvlTree public interface, for SpiderMonkey.                        //
//                                                                    //
////////////////////////////////////////////////////////////////////////

// This public interface is fairly limited and restrictive.  If you need to
// add more functionality, consider copying code from `class AvlTreeTestIF` in
// js/src/jsapi-tests/testAvlTree.cpp rather than rolling your own.  See
// comments there.

template <class T, class C>
class AvlTree : public AvlTreeImpl<T, C> {
  // Shorthands for names in the implementation (parent) class.
  using Impl = AvlTreeImpl<T, C>;
  using ImplNode = typename AvlTreeImpl<T, C>::Node;
  using ImplResult = typename AvlTreeImpl<T, C>::Result;
  using ImplNodeAndResult = typename AvlTreeImpl<T, C>::NodeAndResult;

 public:
  explicit AvlTree(LifoAlloc* alloc = nullptr) : Impl(alloc) {}

  // You'll need to tell the tree how to allocate nodes, either here or in
  // `AvlTree::AvlTree`.
  void setAllocator(LifoAlloc* alloc) { Impl::setAllocator(alloc); }

  // Is the tree empty?
  bool empty() const { return Impl::root_ == nullptr; }

  // Insert `v` in the tree.  Returns false to indicate OOM.  `v` may not be
  // equal to any existing value in the tree, per `C::compare`; if it is, this
  // routine will MOZ_CRASH().  It would be trivial to change this to replace
  // an existing value instead, if needed.
  [[nodiscard]] bool insert(const T& v) {
    ImplNode* new_root = Impl::insert_worker(v);
    // Take out both unlikely cases with a single comparison.
    if (MOZ_UNLIKELY(uintptr_t(new_root) <= uintptr_t(1))) {
      // OOM (new_root == 0) or duplicate (new_root == 1)
      if (!new_root) {
        // OOM
        return false;
      }
      // Duplicate; tree is unchanged.
      MOZ_CRASH();
    }
    Impl::root_ = new_root;
    return true;
  }

  // Remove `v` from the tree.  `v` must actually be in the tree, per
  // `C::compare`.  If it is not, this routine will MOZ_CRASH().
  // Superficially it looks like we could change it to return without doing
  // anything in that case, if needed, except we'd need to first verify that
  // `delete_worker` doesn't change the tree in that case.
  void remove(const T& v) {
    ImplNodeAndResult pair = Impl::delete_worker(Impl::root_, v);
    ImplNode* new_root = pair.first;
    ImplResult res = pair.second;
    if (MOZ_UNLIKELY(res == ImplResult::Error)) {
      // `v` isn't in the tree.
      MOZ_CRASH();
    } else {
      Impl::root_ = new_root;
    }
  }

  // Determine whether the tree contains `v` and if so return, in `res`, a
  // copy of the stored version.  Note that the determination is done using
  // `C::compare` and you should consider carefully the consequences of
  // passing in `v` for which `C::compare` indicates "equal" for more than one
  // value in the tree.  This is not invalid, but it does mean that you may be
  // returned, via `res`, *any* of the values in the tree that `compare` deems
  // equal to `v`, and which you get is arbitrary -- it depends on which is
  // closest to the root.
  bool contains(const T& v, T* res) const {
    ImplNode* node = Impl::find_worker(v);
    if (node) {
      *res = node->item;
      return true;
    }
    return false;
  }

  // Determine whether the tree contains `v` and if so return the address of
  // the stored version.  The comments on `::contains` about the meaning of
  // `C::compare` apply here too.
  T* maybeLookup(const T& v) {
    ImplNode* node = Impl::find_worker(v);
    if (node) {
      return &(node->item);
    }
    return nullptr;
  }

  // AvlTree::Iter is also public; it's just pass-through from AvlTreeImpl.
  // See documentation above on AvlTree::Iter on how to use it.
};

} /* namespace js */

#endif /* ds_AvlTree_h */
