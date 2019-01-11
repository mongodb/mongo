/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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
class SplayTree
{
    struct Node {
        T item;
        Node* left;
        Node* right;
        Node* parent;

        explicit Node(const T& item)
          : item(item), left(nullptr), right(nullptr), parent(nullptr)
        {}
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
      : alloc(alloc), root(nullptr), freeList(nullptr)
#ifdef DEBUG
      , enableCheckCoherency(true)
#endif
    {}

    void setAllocator(LifoAlloc* alloc) {
        this->alloc = alloc;
    }

    void disableCheckCoherency() {
#ifdef DEBUG
        enableCheckCoherency = false;
#endif
    }

    bool empty() const {
        return !root;
    }

    T* maybeLookup(const T& v)
    {
        if (!root)
            return nullptr;
        Node* last = lookup(v);
        return (C::compare(v, last->item) == 0) ? &(last->item) : nullptr;
    }

    bool contains(const T& v, T* res)
    {
        if (!root)
            return false;
        Node* last = lookup(v);
        splay(last);
        checkCoherency(root, nullptr);
        if (C::compare(v, last->item) == 0) {
            *res = last->item;
            return true;
        }
        return false;
    }

    MOZ_MUST_USE bool insert(const T& v)
    {
        Node* element = allocateNode(v);
        if (!element)
            return false;

        if (!root) {
            root = element;
            return true;
        }
        Node* last = lookup(v);
        int cmp = C::compare(v, last->item);

        // Don't tolerate duplicate elements.
        MOZ_ASSERT(cmp);

        Node*& parentPointer = (cmp < 0) ? last->left : last->right;
        MOZ_ASSERT(!parentPointer);
        parentPointer = element;
        element->parent = last;

        splay(element);
        checkCoherency(root, nullptr);
        return true;
    }

    void remove(const T& v)
    {
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
            while (swap->right)
                swap = swap->right;
            swapChild = swap->left;
        } else if (root->right) {
            swap = root->right;
            while (swap->left)
                swap = swap->left;
            swapChild = swap->right;
        } else {
            freeNode(root);
            root = nullptr;
            return;
        }

        // The selected node has at most one child, in swapChild. Detach it
        // from the subtree by replacing it with that child.
        if (swap == swap->parent->left)
            swap->parent->left = swapChild;
        else
            swap->parent->right = swapChild;
        if (swapChild)
            swapChild->parent = swap->parent;

        root->item = swap->item;
        freeNode(swap);

        checkCoherency(root, nullptr);
    }

    template <class Op>
    void forEach(Op op)
    {
        forEachInner<Op>(op, root);
    }

  private:

    Node* lookup(const T& v)
    {
        MOZ_ASSERT(root);
        Node* node = root;
        Node* parent;
        do {
            parent = node;
            int c = C::compare(v, node->item);
            if (c == 0)
                return node;
            else if (c < 0)
                node = node->left;
            else
                node = node->right;
        } while (node);
        return parent;
    }

    Node* allocateNode(const T& v)
    {
        Node* node = freeList;
        if (node) {
            freeList = node->left;
            new(node) Node(v);
            return node;
        }
        return alloc->new_<Node>(v);
    }

    void freeNode(Node* node)
    {
        node->left = freeList;
        freeList = node;
    }

    void splay(Node* node)
    {
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

    void rotate(Node* node)
    {
        // Rearrange nodes so that node becomes the parent of its current
        // parent, while preserving the sortedness of the tree.
        Node* parent = node->parent;
        if (parent->left == node) {
            //     x          y
            //   y  c  ==>  a  x
            //  a b           b c
            parent->left = node->right;
            if (node->right)
                node->right->parent = parent;
            node->right = parent;
        } else {
            MOZ_ASSERT(parent->right == node);
            //   x             y
            //  a  y   ==>   x  c
            //    b c       a b
            parent->right = node->left;
            if (node->left)
                node->left->parent = parent;
            node->left = parent;
        }
        node->parent = parent->parent;
        parent->parent = node;
        if (Node* grandparent = node->parent) {
            if (grandparent->left == parent)
                grandparent->left = node;
            else
                grandparent->right = node;
        } else {
            root = node;
        }
    }

    template <class Op>
    void forEachInner(Op op, Node* node)
    {
        if (!node)
            return;

        forEachInner<Op>(op, node->left);
        op(node->item);
        forEachInner<Op>(op, node->right);
    }

    Node* checkCoherency(Node* node, Node* minimum)
    {
#ifdef DEBUG
        if (!enableCheckCoherency)
            return nullptr;
        if (!node) {
            MOZ_ASSERT(!root);
            return nullptr;
        }
        MOZ_ASSERT_IF(!node->parent, node == root);
        MOZ_ASSERT_IF(minimum, C::compare(minimum->item, node->item) < 0);
        if (node->left) {
            MOZ_ASSERT(node->left->parent == node);
            Node* leftMaximum = checkCoherency(node->left, minimum);
            MOZ_ASSERT(C::compare(leftMaximum->item, node->item) < 0);
        }
        if (node->right) {
            MOZ_ASSERT(node->right->parent == node);
            return checkCoherency(node->right, node);
        }
        return node;
#else
        return nullptr;
#endif
    }
};

}  /* namespace js */

#endif /* ds_SplayTree_h */
