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

#include "mongo/platform/compiler.h"
#include "mongo/platform/source_location.h"
#include "mongo/util/debug_util.h"

#include <string>

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
// IWYU pragma: private, include "mongo/util/assert_util.h"
// IWYU pragma: friend "mongo/util/assert_util.h"
// IWYU pragma: friend "mongo/base/checked_cast.h"
// IWYU pragma: friend "mongo/base/status.h"
// IWYU pragma: friend "mongo/base/status_with.h"
// IWYU pragma: friend "mongo/base/string_data.h"
// IWYU pragma: friend "mongo/util/intrusive_counter.h"

namespace mongo {

#ifdef MONGO_SOURCE_LOCATION_HAVE_STD
MONGO_COMPILER_NORETURN void invariantFailed(const char* expr,
                                             WrappedStdSourceLocation loc) noexcept;
#endif

MONGO_COMPILER_NORETURN void invariantFailed(const char* expr,
                                             SyntheticSourceLocation loc) noexcept;

#ifdef MONGO_SOURCE_LOCATION_HAVE_STD
MONGO_COMPILER_NORETURN void invariantFailedWithMsg(const char* expr,
                                                    const std::string& msg,
                                                    WrappedStdSourceLocation loc) noexcept;
#endif
MONGO_COMPILER_NORETURN void invariantFailedWithMsg(const char* expr,
                                                    const std::string& msg,
                                                    SyntheticSourceLocation loc) noexcept;

// This overload is our legacy invariant, which just takes a condition to test.
//
// ex)   invariant(!condition);
//
//       Invariant failure !condition some/file.cpp 528
//
#define MONGO_invariant_1(Expression) \
    ::mongo::invariantWithLocation((Expression), #Expression, MONGO_SOURCE_LOCATION())

template <typename T>
constexpr void invariantWithLocation(const T& testOK,
                                     const char* expr,
                                     SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!testOK)) {
        ::mongo::invariantFailed(expr, loc);
    }
}


// This invariant overload accepts a condition and a message, to be logged if the condition is
// false.
//
// ex)   invariant(!condition, "hello!");
//
//       Invariant failure !condition "hello!" some/file.cpp 528
//
#define MONGO_invariant_2(Expression, contextExpr)                                     \
    ::mongo::invariantWithContextAndLocation((Expression),                             \
                                             #Expression,                              \
                                             [&] { return std::string{contextExpr}; }, \
                                             MONGO_SOURCE_LOCATION())

template <typename T, typename ContextExpr>
constexpr void invariantWithContextAndLocation(const T& testOK,
                                               const char* expr,
                                               ContextExpr&& contextExpr,
                                               SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!testOK)) {
        ::mongo::invariantFailedWithMsg(expr, contextExpr(), loc);
    }
}

#define MONGO_invariant_EXPAND(x) x /**< MSVC workaround */
#define MONGO_invariant_PICK(_1, _2, x, ...) x
#define invariant(...)      \
    MONGO_invariant_EXPAND( \
        MONGO_invariant_PICK(__VA_ARGS__, MONGO_invariant_2, MONGO_invariant_1)(__VA_ARGS__))

// Behaves like invariant in debug builds and is compiled out in release. Use for checks, which can
// potentially be slow or on a critical path.
#define MONGO_dassert(...) \
    if (kDebugBuild)       \
    invariant(__VA_ARGS__)

#define dassert MONGO_dassert

}  // namespace mongo
