// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <type_traits>

namespace mongo::tree_walker {
/**
 * A template type which resolves to 'const T*' if 'IsConst' argument is 'true', and to 'T*'
 * otherwise.
 */
template <bool IsConst, typename T>
using MaybeConstPtr = typename std::conditional<IsConst, const T*, T*>::type;

/**
 * Provided with a Walker and a Node, walk() calls each of the following:
 * * walker.preVisit() once before walking to each child.
 * * walker.inVisit() between walking to each child. It is called multiple times, once between each
 *   pair of children. walker.inVisit() is skipped if the Node has fewer than two children.
 * * walker.postVisit() once after walking to each child.
 *
 * Each of the Node's children is recursively walked and the same three methods are called for it.
 *
 * The Node type should either provide begin() and end() methods returning an iterator to walk its
 * children, or define begin() and end() functions taking a Node reference which return the
 * iterator.
 *
 * If the caller doesn't intend to modify the tree, then the template argument 'IsConst' should be
 * set to 'true'. In this case the 'node' pointer will be qualified with 'const'.
 */
template <bool IsConst, typename Node, typename Walker>
void walk(MaybeConstPtr<IsConst, Node> node, Walker* walker) {
    if (node) {
        walker->preVisit(node);

        auto count = 0ull;
        for (auto&& child : *node) {
            if (count) {
                walker->inVisit(count, node);
            }
            ++count;
            walk<IsConst, Node, Walker>(&*child, walker);
        }

        walker->postVisit(node);
    }
}
}  // namespace mongo::tree_walker
