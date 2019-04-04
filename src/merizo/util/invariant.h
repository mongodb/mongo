/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include <boost/preprocessor/facilities/overload.hpp>
#include <string>

#include "merizo/platform/compiler.h"
#include "merizo/util/debug_util.h"

namespace merizo {

/**
 * This include exists so that merizo/base/status_with.h can use the invariant macro without causing
 * a circular include chain. It should never be included directly in any other file other than that
 * one (and assert_util.h).
 */

#if !defined(MERIZO_INCLUDE_INVARIANT_H_WHITELISTED)
#error "Include assert_util.h instead of invariant.h."
#endif

MERIZO_COMPILER_NORETURN void invariantFailed(const char* expr,
                                             const char* file,
                                             unsigned line) noexcept;

// This overload is our legacy invariant, which just takes a condition to test.
//
// ex)   invariant(!condition);
//
//       Invariant failure !condition some/file.cpp 528
//
#define MERIZO_invariant_1(Expression) \
    ::merizo::invariantWithLocation((Expression), #Expression, __FILE__, __LINE__)

template <typename T>
inline void invariantWithLocation(const T& testOK,
                                  const char* expr,
                                  const char* file,
                                  unsigned line) {
    if (MERIZO_unlikely(!testOK)) {
        ::merizo::invariantFailed(expr, file, line);
    }
}

MERIZO_COMPILER_NORETURN void invariantFailedWithMsg(const char* expr,
                                                    const std::string& msg,
                                                    const char* file,
                                                    unsigned line) noexcept;

// This invariant overload accepts a condition and a message, to be logged if the condition is
// false.
//
// ex)   invariant(!condition, "hello!");
//
//       Invariant failure !condition "hello!" some/file.cpp 528
//
#define MERIZO_invariant_2(Expression, contextExpr)                                           \
    ::merizo::invariantWithContextAndLocation((Expression),                                   \
                                             #Expression,                                    \
                                             [&]() -> std::string { return (contextExpr); }, \
                                             __FILE__,                                       \
                                             __LINE__)

template <typename T, typename ContextExpr>
inline void invariantWithContextAndLocation(
    const T& testOK, const char* expr, ContextExpr&& contextExpr, const char* file, unsigned line) {
    if (MERIZO_unlikely(!testOK)) {
        ::merizo::invariantFailedWithMsg(expr, contextExpr(), file, line);
    }
}

// This helper macro is necessary to make the __VAR_ARGS__ expansion work properly on MSVC.
#define MERIZO_expand(x) x

#define invariant(...) \
    MERIZO_expand(MERIZO_expand(BOOST_PP_OVERLOAD(MERIZO_invariant_, __VA_ARGS__))(__VA_ARGS__))

// Behaves like invariant in debug builds and is compiled out in release. Use for checks, which can
// potentially be slow or on a critical path.
#define MERIZO_dassert(...) \
    if (kDebugBuild)       \
    invariant(__VA_ARGS__)

#define dassert MERIZO_dassert

}  // namespace merizo
