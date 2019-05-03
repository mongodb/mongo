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

#include <boost/intrusive_ptr.hpp>
#include <experimental/type_traits>
#include <type_traits>
#include <utility>

#include "mongo/db/pipeline/expression.h"

namespace mongo::expression_walker {

// The following types and constexpr values are used to determine if a Walker has a given member
// function at compile-time.

/**
 * PreVisit provides the compiler with a type for a preVisit member function.
 */
template <typename Walker, typename Arg>
using PreVisit = decltype(std::declval<Walker>().preVisit(std::declval<Arg>()));
/**
 * hasPreVisit is a template variable indicating whether such a member function exists for a given
 * Walker type when called on a pointer to our Expression type.
 */
template <typename Walker>
constexpr auto hasPreVisit =
    std::experimental::is_detected<PreVisit, Walker, boost::intrusive_ptr<Expression>&>::value;

/**
 * InVisit provides the compiler with a type for an inVisit member function.
 */
template <typename Walker, typename... Args>
using InVisit = decltype(std::declval<Walker>().inVisit(std::declval<Args>()...));
/**
 * hasBasicInVisit is a template variable indicating whether such a member function exists for a
 * given Walker type when called on a pointer to our Expression type.
 */
template <typename Walker>
constexpr auto hasBasicInVisit =
    std::experimental::is_detected<InVisit, Walker, boost::intrusive_ptr<Expression>&>::value;
/**
 * hasCountingInVisit is a template variable indicating whether such a member function exists for a
 * given Walker type when called on a pointer to our Expression type.
 */
template <typename Walker>
constexpr auto hasCountingInVisit = std::experimental::
    is_detected<InVisit, Walker, unsigned long long, boost::intrusive_ptr<Expression>&>::value;

/**
 * PostVisit provides the compiler with a type for a postVisit member function.
 */
template <typename Walker, typename Arg>
using PostVisit = decltype(std::declval<Walker>().postVisit(std::declval<Arg>()));
/**
 * hasPostVisit is a template variable indicating whether such a member function exists for a given
 * Walker type when called on a pointer to our Expression type.
 */
template <typename Walker>
constexpr auto hasPostVisit =
    std::experimental::is_detected<PostVisit, Walker, boost::intrusive_ptr<Expression>&>::value;

/**
 * Provided with a Walker and an Expression, walk() calls each of the following if they exist:
 * * walker.preVisit() once before walking to each child.
 * * walker.inVisit() between walking to each child. It is called multiple times, once between each
 *   pair of children. walker.inVisit() is skipped if the Expression has fewer than two children.
 * * walker.postVisit() once after walking to each child.
 * Each of the Expression's child Expressions is recursively walked and the same three methods are
 * called for it. Although each of the methods are individually optional, at least one of them must
 * exist. preVisit() and postVisit() may return a pointer to an Expression. If either does, walk()
 * will replace the current Expression with the return value. If no change is needed during a
 * particular call, preVisit() and postVisit() may return their argument.
 */
template <typename Walker>
void walk(Walker& walker, boost::intrusive_ptr<Expression>& expression) {
    static_assert(hasPreVisit<Walker> || hasBasicInVisit<Walker> || hasCountingInVisit<Walker> ||
                      hasPostVisit<Walker>,
                  "Walker must have at least one of the following functions: 'preVisit', "
                  "'inVisit', 'postVisit'.");
    static_assert(!hasBasicInVisit<Walker> || !hasCountingInVisit<Walker>,
                  "Walker must include only one signature for inVisit: inVisit(count, expression) "
                  "or inVisit(expression).");
    if (expression) {
        if
            constexpr(hasPreVisit<Walker>) {
                if
                    constexpr(std::is_void_v<PreVisit<Walker, boost::intrusive_ptr<Expression>&>>)
                        walker.preVisit(expression);
                else
                    expression = walker.preVisit(expression);
            }

        // InVisit needs to be called between every two nodes which requires more complicated
        // branching logic. InVisit is forbidden from replacing its Expression through the return
        // value and must return void since it would break our iteration and be confusing to
        // replace a node while only a portion of its children have been walked.
        if
            constexpr(hasBasicInVisit<Walker>) {
                static_assert(
                    std::is_void_v<InVisit<Walker, boost::intrusive_ptr<Expression>&>>,
                    "Walker::inVisit must return void. Modification is forbidden between walking "
                    "children.");
                auto skippingFirst = true;
                for (auto&& child : expression->getChildren()) {
                    if (skippingFirst)
                        skippingFirst = false;
                    else
                        walker.inVisit(expression);
                    walk(walker, child);
                }
            }
        // If the signature of InVisit includes a count, maintaining it while walking and pass it to
        // the function.
        else if
            constexpr(hasCountingInVisit<Walker>) {
                static_assert(
                    std::is_void_v<
                        InVisit<Walker, unsigned long long, boost::intrusive_ptr<Expression>&>>,
                    "Walker::inVisit must return void. Modification is forbidden between walking "
                    "children.");
                auto count = 0ull;
                for (auto&& child : expression->getChildren()) {
                    if (count != 0ull)
                        walker.inVisit(count, expression);
                    count++;
                    walk(walker, child);
                }
            }
        else {
            for (auto&& child : expression->getChildren())
                walk(walker, child);
        }

        if
            constexpr(hasPostVisit<Walker>) {
                if
                    constexpr(std::is_void_v<PostVisit<Walker, boost::intrusive_ptr<Expression>&>>)
                        walker.postVisit(expression);
                else
                    expression = walker.postVisit(expression);
            }
    }
}

}  // namespace mongo::expression_walker
