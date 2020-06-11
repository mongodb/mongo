/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

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
