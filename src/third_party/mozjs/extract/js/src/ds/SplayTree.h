/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_SplayTree_h
#define ds_SplayTree_h

#include "ds/LifoAlloc.h"

namespace js {

/*
 * Class which represents a splay tree with nodes allocated from a LifoAlloc.
 * Splay trees are balanced binary search trees for which search, insert and
 * remove are all amortized O(log n).
 *
 * T indicates the type of tree elements, C must have a static
 * compare(const T&, const T&) method ordering the elements. As for LifoAlloc
 * objects, T objects stored in the tree will not be explicitly destroyed.
 */
template <class T, class C>
class SplayTree {
  struct Node {
    T item;
    Node* left;
    Node* right;
    Node* parent;

    explicit Node(const T& item)
        : item(item), left(nullptr), right(nullptr), parent(nullptr) {}
  };

  LifoAlloc* alloc;
  Node* root;
  Node* freeList;

#ifdef DEBUG
  bool enableCheckCoherency;
#endif

  SplayTree(const SplayTree&) = delete;
  SplayTree& operator=(const SplayTree&) = delete;

 public:
  explicit SplayTree(LifoAlloc* alloc = nullptr)
      : alloc(alloc),
        root(nullptr),
        freeList(nullptr)
#ifdef DEBUG
        ,
        enableCheckCoherency(true)
#endif
  {
  }

  void setAllocator(LifoAlloc* alloc) { this->alloc = alloc; }

  void disableCheckCoherency() {
#ifdef DEBUG
    enableCheckCoherency = false;
#endif
  }

  bool empty() const { return !root; }

  T* maybeLookup(const T& v) {
    if (!root) {
      return nullptr;
    }
    Node* last = lookup(v);
    return (C::compare(v, last->item) == 0) ? &(last->item) : nullptr;
  }

  bool contains(const T& v, T* res) {
    if (!root) {
      return false;
    }
    Node* last = lookup(v);
    splay(last);
    checkCoherency();
    if (C::compare(v, last->item) == 0) {
      *res = last->item;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool insert(const T& v) {
    Node* element = allocateNode(v);
    if (!element) {
      return false;
    }

    if (!root) {
      root = element;
      return true;
    }
    Node* last = lookup(v);
    int cmp = C::compare(v, last->item);

    // Don't tolerate duplicate elements.
    MOZ_DIAGNOSTIC_ASSERT(cmp);

    Node*& parentPointer = (cmp < 0) ? last->left : last->right;
    MOZ_ASSERT(!parentPointer);
    parentPointer = element;
    element->parent = last;

    splay(element);
    checkCoherency();
    return true;
  }

  void remove(const T& v) {
    Node* last = lookup(v);
    MOZ_ASSERT(last && C::compare(v, last->item) == 0);

    splay(last);
    MOZ_ASSERT(last == root);

    // Find another node which can be swapped in for the root: either the
    // rightmost child of the root's left, or the leftmost child of the
    // root's right.
    Node* swap;
    Node* swapChild;
    if (root->left) {
      swap = root->left;
      while (swap->right) {
        swap = swap->right;
      }
      swapChild = swap->left;
    } else if (root->right) {
      swap = root->right;
      while (swap->left) {
        swap = swap->left;
      }
      swapChild = swap->right;
    } else {
      freeNode(root);
      root = nullptr;
      return;
    }

    // The selected node has at most one child, in swapChild. Detach it
    // from the subtree by replacing it with that child.
    if (swap == swap->parent->left) {
      swap->parent->left = swapChild;
    } else {
      swap->parent->right = swapChild;
    }
    if (swapChild) {
      swapChild->parent = swap->parent;
    }

    root->item = swap->item;
    freeNode(swap);

    checkCoherency();
  }

  template <class Op>
  void forEach(Op op) {
    forEachInner<Op>(op, root);
  }

 private:
  Node* lookup(const T& v) {
    MOZ_ASSERT(root);
    Node* node = root;
    Node* parent;
    do {
      parent = node;
      int c = C::compare(v, node->item);
      if (c == 0) {
        return node;
      } else if (c < 0) {
        node = node->left;
      } else {
        node = node->right;
      }
    } while (node);
    return parent;
  }

  Node* allocateNode(const T& v) {
    Node* node = freeList;
    if (node) {
      freeList = node->left;
      new (node) Node(v);
      return node;
    }
    return alloc->new_<Node>(v);
  }

  void freeNode(Node* node) {
    node->left = freeList;
    freeList = node;
  }

  void splay(Node* node) {
    // Rotate the element until it is at the root of the tree. Performing
    // the rotations in this fashion preserves the amortized balancing of
    // the tree.
    MOZ_ASSERT(node);
    while (node != root) {
      Node* parent = node->parent;
      if (parent == root) {
        // Zig rotation.
        rotate(node);
        MOZ_ASSERT(node == root);
        return;
      }
      Node* grandparent = parent->parent;
      if ((parent->left == node) == (grandparent->left == parent)) {
        // Zig-zig rotation.
        rotate(parent);
        rotate(node);
      } else {
        // Zig-zag rotation.
        rotate(node);
        rotate(node);
      }
    }
  }

  void rotate(Node* node) {
    // Rearrange nodes so that node becomes the parent of its current
    // parent, while preserving the sortedness of the tree.
    Node* parent = node->parent;
    if (parent->left == node) {
      //     x          y
      //   y  c  ==>  a  x
      //  a b           b c
      parent->left = node->right;
      if (node->right) {
        node->right->parent = parent;
      }
      node->right = parent;
    } else {
      MOZ_ASSERT(parent->right == node);
      //   x             y
      //  a  y   ==>   x  c
      //    b c       a b
      parent->right = node->left;
      if (node->left) {
        node->left->parent = parent;
      }
      node->left = parent;
    }
    node->parent = parent->parent;
    parent->parent = node;
    if (Node* grandparent = node->parent) {
      if (grandparent->left == parent) {
        grandparent->left = node;
      } else {
        grandparent->right = node;
      }
    } else {
      root = node;
    }
  }

  template <class Op>
  void forEachInner(Op op, Node* node) {
    if (!node) {
      return;
    }

    forEachInner<Op>(op, node->left);
    op(node->item);
    forEachInner<Op>(op, node->right);
  }

  void checkCoherency() const {
#ifdef DEBUG
    if (!enableCheckCoherency) {
      return;
    }
    if (!root) {
      return;
    }
    MOZ_ASSERT(root->parent == nullptr);
    const Node* node = root;
    const Node* minimum = nullptr;
    MOZ_ASSERT_IF(node->left, node->left->parent == node);
    MOZ_ASSERT_IF(node->right, node->right->parent == node);

    // This is doing a depth-first search and check that the values are
    // ordered properly.
    while (true) {
      // Go to the left-most child.
      while (node->left) {
        MOZ_ASSERT_IF(node->left, node->left->parent == node);
        MOZ_ASSERT_IF(node->right, node->right->parent == node);
        node = node->left;
      }

      MOZ_ASSERT_IF(minimum, C::compare(minimum->item, node->item) < 0);
      minimum = node;

      if (node->right) {
        // Go once to the right and try again.
        MOZ_ASSERT_IF(node->left, node->left->parent == node);
        MOZ_ASSERT_IF(node->right, node->right->parent == node);
        node = node->right;
      } else {
        // We reached a leaf node, move to the first branch to the right of
        // our current left-most sub-tree.
        MOZ_ASSERT(!node->left && !node->right);
        const Node* prev = nullptr;

        // Visit the parent node, to find the right branch which we have
        // not visited yet. Either we are coming back from the right
        // branch, or we are coming back from the left branch with no
        // right branch to visit.
        while (node->parent) {
          prev = node;
          node = node->parent;

          // If we came back from the left branch, visit the value.
          if (node->left == prev) {
            MOZ_ASSERT_IF(minimum, C::compare(minimum->item, node->item) < 0);
            minimum = node;
          }

          if (node->right != prev && node->right != nullptr) {
            break;
          }
        }

        if (!node->parent) {
          MOZ_ASSERT(node == root);
          // We reached the root node either because we came back from
          // the right hand side, or because the root node had a
          // single child.
          if (node->right == prev || node->right == nullptr) {
            return;
          }
        }

        // Go to the right node which we have not visited yet.
        MOZ_ASSERT(node->right != prev && node->right != nullptr);
        node = node->right;
      }
    }
#endif
  }
};

} /* namespace js */

#endif /* ds_SplayTree_h */
