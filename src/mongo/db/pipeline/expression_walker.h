/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/stdx/type_traits.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace mongo::expression_walker {

/**
 * A template type which resolves to 'const T*' if 'IsConst' argument is 'true', and to 'T*'
 * otherwise.
 */
template <bool IsConst, typename T>
using MaybeConstPtr = typename std::conditional<IsConst, const T*, T*>::type;

// The following types and constexpr values are used to determine if a Walker has a given member
// function at compile-time.

/**
 * PreVisit provides the compiler with a type for a preVisit member function.
 */
template <typename Walker, typename Arg>
using PreVisit = decltype(std::declval<Walker>().preVisit(std::declval<Arg>()));
/**
 * hasVoidPreVisit is a template variable indicating whether such a void-returning member function
 * exists for a given Walker type when called on a pointer to our Node type.
 */
template <typename Node, typename Walker>
inline constexpr auto hasVoidPreVisit =
    stdx::is_detected_exact_v<void, PreVisit, Walker, MaybeConstPtr<std::is_const_v<Node>, Node>>;
/**
 * hasPtrPreVisit is a template variable indicating whether such a pointer-returning member
 * function exists for a given Walker type when called on a pointer to our Node type.
 */
template <typename Node, typename Walker>
inline constexpr auto hasPtrPreVisit =
    stdx::is_detected_convertible_v<std::unique_ptr<Node>, PreVisit, Walker, Node*>;

/**
 * InVisit provides the compiler with a type for an inVisit member function.
 */
template <typename Walker, typename... Args>
using InVisit = decltype(std::declval<Walker>().inVisit(std::declval<Args>()...));
/**
 * hasBasicInVisit is a template variable indicating whether such a member function exists for a
 * given Walker type when called on a pointer to our Node type.
 */
template <typename Node, typename Walker>
inline constexpr auto hasBasicInVisit =
    stdx::is_detected_v<InVisit, Walker, MaybeConstPtr<std::is_const_v<Node>, Node>>;
/**
 * hasCountingInVisit is a template variable indicating whether such a member function exists for a
 * given Walker type when called on a pointer to our Node type.
 */
template <typename Node, typename Walker>
inline constexpr auto hasCountingInVisit = stdx::
    is_detected_v<InVisit, Walker, unsigned long long, MaybeConstPtr<std::is_const_v<Node>, Node>>;

/**
 * PostVisit provides the compiler with a type for a postVisit member function.
 */
template <typename Walker, typename Arg>
using PostVisit = decltype(std::declval<Walker>().postVisit(std::declval<Arg>()));
/**
 * hasVoidPostVisit is a template variable indicating whether such a void-returning member function
 * exists for a given Walker type when called on a pointer to our Node type.
 */
template <typename Node, typename Walker>
inline constexpr auto hasVoidPostVisit =
    stdx::is_detected_exact_v<void, PostVisit, Walker, MaybeConstPtr<std::is_const_v<Node>, Node>>;
/**
 * hasVoidPostVisit is a template variable indicating whether such a pointer-returning member
 * function exists for a given Walker type when called on a pointer to our Node type.
 */
template <typename Node, typename Walker>
inline constexpr auto hasPtrPostVisit =
    stdx::is_detected_convertible_v<std::unique_ptr<Node>, PostVisit, Walker, Node*>;

/**
 * hasReturningVisit is a template variable indicating whether there is a pointer-returning member
 * function (pre or post) that exists for a given Walker type when called on a pointer to our
 * Node type.
 */
template <typename Node, typename Walker>
inline constexpr auto hasReturningVisit =
    hasPtrPreVisit<Node, Walker> || hasPtrPostVisit<Node, Walker>;

/**
 * Provided with a Walker and a Node, walk() calls each of the following if they exist:
 * * walker.preVisit() once before walking to each child.
 * * walker.inVisit() between walking to each child. It is called multiple times, once between each
 *   pair of children. walker.inVisit() is skipped if the Node has fewer than two children.
 * * walker.postVisit() once after walking to each child.
 * Each of the Node's child Nodes is recursively walked and the same three methods are called for
 * it. Although each of the methods are individually optional, at least one of them must exist.
 *
 * If the caller doesn't intend to modify the tree, then the type of Node should be 'const'.
 *
 * If Node is not const, preVisit() and postVisit() may return a pointer to a Node. If either does,
 * walk() will replace the current Node with the return value. If no change is needed during a
 * particular call, preVisit() and postVisit() may return null. walk() returns a unique_ptr
 * containing a new root node, if it is modified by a value returning preVisit() or postVisit(),
 * nullptr if it is not modified, or void if modification is impossible for the given Walker.
 */
template <typename Node, typename Walker>
auto walk(Node* node, Walker* walker)
    -> std::conditional_t<hasReturningVisit<Node, Walker>, std::unique_ptr<Node>, void> {
    if constexpr (std::is_const_v<Node>) {
        static_assert(
            hasVoidPreVisit<Node, Walker> || hasBasicInVisit<Node, Walker> ||
                hasCountingInVisit<Node, Walker> || hasVoidPostVisit<Node, Walker>,
            "Walker that is const must have at least one of the following functions: 'preVisit', "
            "'inVisit', 'postVisit'.");
    } else {
        static_assert(hasVoidPreVisit<Node, Walker> || hasPtrPreVisit<Node, Walker> ||
                          hasBasicInVisit<Node, Walker> || hasCountingInVisit<Node, Walker> ||
                          hasVoidPostVisit<Node, Walker> || hasPtrPostVisit<Node, Walker>,
                      "Walker that is non-const must have at least one of the following functions: "
                      "'preVisit', 'inVisit', 'postVisit'.");
    }
    static_assert(!hasBasicInVisit<Node, Walker> || !hasCountingInVisit<Node, Walker>,
                  "Walker must include only one signature for inVisit: inVisit(num, node) "
                  "or inVisit(node).");

    // Calls walk on a child node. Then replaces that node if walk returns a non-null value.
    auto walkChild = [&](auto&& child) {
        if constexpr (hasReturningVisit<Node, Walker>) {
            if (auto newChild = walk(child.get(), walker))
                child = newChild.release();
        } else {
            walk(child.get(), walker);
        }
    };

    auto newExpr = std::unique_ptr<Node>{};
    if (node) {
        if constexpr (hasVoidPreVisit<Node, Walker>) {
            walker->preVisit(node);
        } else if constexpr (hasPtrPreVisit<Node, Walker>) {
            newExpr = walker->preVisit(node);
            node = newExpr.get() != nullptr ? newExpr.get() : node;
        }

        // InVisit needs to be called between every two nodes which requires more complicated
        // branching logic. InVisit is forbidden from replacing its node through the return value
        // and must return void since it would break our iteration and be confusing to replace a
        // node while only a portion of its children have been walked.
        if constexpr (hasBasicInVisit<Node, Walker>) {
            static_assert(
                std::is_void_v<InVisit<Walker, Node*>>,
                "Walker::inVisit must return void. Modification is forbidden between walking "
                "children.");
            auto skippingFirst = true;
            for (auto&& child : node->getChildren()) {
                if (skippingFirst)
                    skippingFirst = false;
                else
                    walker->inVisit(node);
                walkChild(child);
            }
        }
        // If the signature of InVisit includes a count, maintaing it while walking and pass it to
        // the function.
        else if constexpr (hasCountingInVisit<Node, Walker>) {
            static_assert(
                std::is_void_v<InVisit<Walker, unsigned long long, Node*>>,
                "Walker::inVisit must return void. Modification is forbidden between walking "
                "children.");
            auto count = 0ull;
            for (auto&& child : node->getChildren()) {
                if (count != 0ull)
                    walker->inVisit(count, node);
                count++;
                walkChild(child);
            }
        } else {
            for (auto&& child : node->getChildren())
                walkChild(child);
        }

        if constexpr (hasVoidPostVisit<Node, Walker>)
            walker->postVisit(node);
        else if constexpr (hasPtrPostVisit<Node, Walker>)
            if (auto postResult = walker->postVisit(node))
                newExpr = std::move(postResult);
    }
    if constexpr (hasReturningVisit<Node, Walker>)
        return newExpr;
}

}  // namespace mongo::expression_walker
