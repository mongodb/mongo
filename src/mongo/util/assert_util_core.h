/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/preprocessor/facilities/overload.hpp>
#include <string>

#include "mongo/platform/compiler.h"
#include "mongo/util/debug_util.h"

/**
 * This header is separated from assert_util.h so that the low-level
 * dependencies of assert_util.h (e.g. mongo/base/status_with.h,
 * mongo/base/status.h, mongo/base/string_data.h) can use the `invariant` macro
 * without causing a circular include chain. It should never be included
 * directly in any other files.
 *
 *     [assert_util.h]
 *     |    |
 *     |    v
 *     |    [string_data.h, etc]
 *     |    |
 *     v    v
 *     [assert_util_core.h]
 */
namespace mongo {

MONGO_COMPILER_NORETURN void invariantFailed(const char* expr,
                                             const char* file,
                                             unsigned line) noexcept;

// This overload is our legacy invariant, which just takes a condition to test.
//
// ex)   invariant(!condition);
//
//       Invariant failure !condition some/file.cpp 528
//
#define MONGO_invariant_1(Expression) \
    ::mongo::invariantWithLocation((Expression), #Expression, __FILE__, __LINE__)

template <typename T>
inline void invariantWithLocation(const T& testOK,
                                  const char* expr,
                                  const char* file,
                                  unsigned line) {
    if (MONGO_unlikely(!testOK)) {
        ::mongo::invariantFailed(expr, file, line);
    }
}

MONGO_COMPILER_NORETURN void invariantFailedWithMsg(const char* expr,
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
#define MONGO_invariant_2(Expression, contextExpr) \
    ::mongo::invariantWithContextAndLocation(      \
        (Expression), #Expression, [&] { return std::string{contextExpr}; }, __FILE__, __LINE__)

template <typename T, typename ContextExpr>
inline void invariantWithContextAndLocation(
    const T& testOK, const char* expr, ContextExpr&& contextExpr, const char* file, unsigned line) {
    if (MONGO_unlikely(!testOK)) {
        ::mongo::invariantFailedWithMsg(expr, contextExpr(), file, line);
    }
}

// This helper macro is necessary to make the __VAR_ARGS__ expansion work properly on MSVC.
#define MONGO_expand(x) x

#define invariant(...) \
    MONGO_expand(MONGO_expand(BOOST_PP_OVERLOAD(MONGO_invariant_, __VA_ARGS__))(__VA_ARGS__))

// Behaves like invariant in debug builds and is compiled out in release. Use for checks, which can
// potentially be slow or on a critical path.
#define MONGO_dassert(...) \
    if (kDebugBuild)       \
    invariant(__VA_ARGS__)

#define dassert MONGO_dassert

constexpr void invariantForConstexprThrower(bool val) {
    enum { AbortException };
    val ? 0 : throw AbortException;
}

constexpr void invariantForConstexpr(bool val) noexcept {
    invariantForConstexprThrower(val);
}

}  // namespace mongo
