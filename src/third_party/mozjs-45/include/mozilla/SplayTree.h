/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * A sorted tree with optimal access times, where recently-accessed elements
 * are faster to access again.
 */

#ifndef mozilla_SplayTree_h
#define mozilla_SplayTree_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

namespace mozilla {

template<class T, class C>
class SplayTree;

template<typename T>
class SplayTreeNode
{
public:
  template<class A, class B>
  friend class SplayTree;

  SplayTreeNode()
    : mLeft(nullptr)
    , mRight(nullptr)
    , mParent(nullptr)
  {}

private:
  T* mLeft;
  T* mRight;
  T* mParent;
};


/**
 * Class which represents a splay tree.
 * Splay trees are balanced binary search trees for which search, insert and
 * remove are all amortized O(log n), but where accessing a node makes it
 * faster to access that node in the future.
 *
 * T indicates the type of tree elements, Comparator must have a static
 * compare(const T&, const T&) method ordering the elements. The compare
 * method must be free from side effects.
 */
template<typename T, class Comparator>
class SplayTree
{
  T* mRoot;

public:
  MOZ_CONSTEXPR SplayTree()
    : mRoot(nullptr)
  {}

  bool empty() const
  {
    return !mRoot;
  }

  T* find(const T& aValue)
  {
    if (empty()) {
      return nullptr;
    }

    T* last = lookup(aValue);
    splay(last);
    return Comparator::compare(aValue, *last) == 0 ? last : nullptr;
  }

  bool insert(T* aValue)
  {
    MOZ_ASSERT(!find(*aValue), "Duplicate elements are not allowed.");

    if (!mRoot) {
      mRoot = aValue;
      return true;
    }
    T* last = lookup(*aValue);
    int cmp = Comparator::compare(*aValue, *last);

    finishInsertion(last, cmp, aValue);
    return true;
  }

  T* findOrInsert(const T& aValue);

  T* remove(const T& aValue)
  {
    T* last = lookup(aValue);
    MOZ_ASSERT(last, "This tree must contain the element being removed.");
    MOZ_ASSERT(Comparator::compare(aValue, *last) == 0);

    // Splay the tree so that the item to remove is the root.
    splay(last);
    MOZ_ASSERT(last == mRoot);

    // Find another node which can be swapped in for the root: either the
    // rightmost child of the root's left, or the leftmost child of the
    // root's right.
    T* swap;
    T* swapChild;
    if (mRoot->mLeft) {
      swap = mRoot->mLeft;
      while (swap->mRight) {
        swap = swap->mRight;
      }
      swapChild = swap->mLeft;
    } else if (mRoot->mRight) {
      swap = mRoot->mRight;
      while (swap->mLeft) {
        swap = swap->mLeft;
      }
      swapChild = swap->mRight;
    } else {
      T* result = mRoot;
      mRoot = nullptr;
      return result;
    }

    // The selected node has at most one child, in swapChild. Detach it
    // from the subtree by replacing it with that child.
    if (swap == swap->mParent->mLeft) {
      swap->mParent->mLeft = swapChild;
    } else {
      swap->mParent->mRight = swapChild;
    }
    if (swapChild) {
      swapChild->mParent = swap->mParent;
    }

    // Make the selected node the new root.
    mRoot = swap;
    mRoot->mParent = nullptr;
    mRoot->mLeft = last->mLeft;
    mRoot->mRight = last->mRight;
    if (mRoot->mLeft) {
      mRoot->mLeft->mParent = mRoot;
    }
    if (mRoot->mRight) {
      mRoot->mRight->mParent = mRoot;
    }

    return last;
  }

  T* removeMin()
  {
    MOZ_ASSERT(mRoot, "No min to remove!");

    T* min = mRoot;
    while (min->mLeft) {
      min = min->mLeft;
    }
    return remove(*min);
  }

  // For testing purposes only.
  void checkCoherency()
  {
    checkCoherency(mRoot, nullptr);
  }

private:
  /**
   * Returns the node in this comparing equal to |aValue|, or a node just
   * greater or just less than |aValue| if there is no such node.
   */
  T* lookup(const T& aValue)
  {
    MOZ_ASSERT(!empty());

    T* node = mRoot;
    T* parent;
    do {
      parent = node;
      int c = Comparator::compare(aValue, *node);
      if (c == 0) {
        return node;
      } else if (c < 0) {
        node = node->mLeft;
      } else {
        node = node->mRight;
      }
    } while (node);
    return parent;
  }

  T* finishInsertion(T* aLast, int32_t aCmp, T* aNew)
  {
    MOZ_ASSERT(aCmp, "Nodes shouldn't be equal!");

    T** parentPointer = (aCmp < 0) ? &aLast->mLeft : &aLast->mRight;
    MOZ_ASSERT(!*parentPointer);
    *parentPointer = aNew;
    aNew->mParent = aLast;

    splay(aNew);
    return aNew;
  }

  /**
   * Rotate the tree until |node| is at the root of the tree. Performing
   * the rotations in this fashion preserves the amortized balancing of
   * the tree.
   */
  void splay(T* aNode)
  {
    MOZ_ASSERT(aNode);

    while (aNode != mRoot) {
      T* parent = aNode->mParent;
      if (parent == mRoot) {
        // Zig rotation.
        rotate(aNode);
        MOZ_ASSERT(aNode == mRoot);
        return;
      }
      T* grandparent = parent->mParent;
      if ((parent->mLeft == aNode) == (grandparent->mLeft == parent)) {
        // Zig-zig rotation.
        rotate(parent);
        rotate(aNode);
      } else {
        // Zig-zag rotation.
        rotate(aNode);
        rotate(aNode);
      }
    }
  }

  void rotate(T* aNode)
  {
    // Rearrange nodes so that aNode becomes the parent of its current
    // parent, while preserving the sortedness of the tree.
    T* parent = aNode->mParent;
    if (parent->mLeft == aNode) {
      //     x          y
      //   y  c  ==>  a  x
      //  a b           b c
      parent->mLeft = aNode->mRight;
      if (aNode->mRight) {
        aNode->mRight->mParent = parent;
      }
      aNode->mRight = parent;
    } else {
      MOZ_ASSERT(parent->mRight == aNode);
      //   x             y
      //  a  y   ==>   x  c
      //    b c       a b
      parent->mRight = aNode->mLeft;
      if (aNode->mLeft) {
        aNode->mLeft->mParent = parent;
      }
      aNode->mLeft = parent;
    }
    aNode->mParent = parent->mParent;
    parent->mParent = aNode;
    if (T* grandparent = aNode->mParent) {
      if (grandparent->mLeft == parent) {
        grandparent->mLeft = aNode;
      } else {
        grandparent->mRight = aNode;
      }
    } else {
      mRoot = aNode;
    }
  }

  T* checkCoherency(T* aNode, T* aMinimum)
  {
    if (mRoot) {
      MOZ_RELEASE_ASSERT(!mRoot->mParent);
    }
    if (!aNode) {
      MOZ_RELEASE_ASSERT(!mRoot);
      return nullptr;
    }
    if (!aNode->mParent) {
      MOZ_RELEASE_ASSERT(aNode == mRoot);
    }
    if (aMinimum) {
      MOZ_RELEASE_ASSERT(Comparator::compare(*aMinimum, *aNode) < 0);
    }
    if (aNode->mLeft) {
      MOZ_RELEASE_ASSERT(aNode->mLeft->mParent == aNode);
      T* leftMaximum = checkCoherency(aNode->mLeft, aMinimum);
      MOZ_RELEASE_ASSERT(Comparator::compare(*leftMaximum, *aNode) < 0);
    }
    if (aNode->mRight) {
      MOZ_RELEASE_ASSERT(aNode->mRight->mParent == aNode);
      return checkCoherency(aNode->mRight, aNode);
    }
    return aNode;
  }

  SplayTree(const SplayTree&) = delete;
  void operator=(const SplayTree&) = delete;
};

template<typename T, class Comparator>
T*
SplayTree<T, Comparator>::findOrInsert(const T& aValue)
{
  if (!mRoot) {
    mRoot = new T(aValue);
    return mRoot;
  }

  T* last = lookup(aValue);
  int cmp = Comparator::compare(aValue, *last);
  if (!cmp) {
    return last;
  }

  return finishInsertion(last, cmp, new T(aValue));
}

}  /* namespace mozilla */

#endif /* mozilla_SplayTree_h */
