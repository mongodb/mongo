// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/platform/source_location.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/modules.h"

#include <string>

/**
 * This header is separated from assert_util.h so that the low-level
 * dependencies of assert_util.h (e.g. mongo/base/status_with.h,
 * mongo/base/status.h) can use the `invariant` macro
 * without causing a circular include chain. It should never be included
 * directly in any other files.
 *
 *     [assert_util.h]
 *     |    |
 *     |    v
 *     |    [status.h, etc]
 *     |    |
 *     v    v
 *     [assert_util_core.h]
 */
// IWYU pragma: private, include "mongo/util/assert_util.h"
// IWYU pragma: friend "mongo/util/assert_util.h"
// IWYU pragma: friend "mongo/base/checked_cast.h"
// IWYU pragma: friend "mongo/base/status.h"
// IWYU pragma: friend "mongo/base/status_with.h"
// IWYU pragma: friend "mongo/util/intrusive_counter.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

namespace error_details {
#ifdef MONGO_SOURCE_LOCATION_HAVE_STD
[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] MONGO_COMPILER_NORETURN void invariantFailed(
    const char* expr, WrappedStdSourceLocation loc) noexcept;
#endif

[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] MONGO_COMPILER_NORETURN void invariantFailed(
    const char* expr, SyntheticSourceLocation loc) noexcept;

#ifdef MONGO_SOURCE_LOCATION_HAVE_STD
[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] MONGO_COMPILER_NORETURN void invariantFailedWithMsg(
    const char* expr, const std::string& msg, WrappedStdSourceLocation loc) noexcept;
#endif
[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] MONGO_COMPILER_NORETURN void invariantFailedWithMsg(
    const char* expr, const std::string& msg, SyntheticSourceLocation loc) noexcept;

template <typename T>
[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] constexpr void invariantWithLocation(
    const T& testOK, const char* expr, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!testOK)) {
        ::mongo::error_details::invariantFailed(expr, loc);
    }
}

template <typename T, typename ContextExpr>
[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] constexpr void invariantWithContextAndLocation(
    const T& testOK,
    const char* expr,
    ContextExpr&& contextExpr,
    SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!testOK)) {
        ::mongo::error_details::invariantFailedWithMsg(expr, contextExpr(), loc);
    }
}
}  // namespace error_details

// This overload is our simple invariant, which just takes a condition to test.
//
// ex)   invariant(!condition);
//
//       Invariant failure !condition some/file.cpp 528
//
#define MONGO_invariant_1(Expression)              \
    ::mongo::error_details::invariantWithLocation( \
        (Expression), #Expression, MONGO_SOURCE_LOCATION())

// This invariant overload accepts a condition and a message, to be logged if the condition is
// false.
//
// ex)   invariant(!condition, "hello!");
//
//       Invariant failure !condition "hello!" some/file.cpp 528
//
#define MONGO_invariant_2(Expression, contextExpr)           \
    ::mongo::error_details::invariantWithContextAndLocation( \
        (Expression),                                        \
        #Expression,                                         \
        [&] { return std::string{contextExpr}; },            \
        MONGO_SOURCE_LOCATION())

#define MONGO_invariant_EXPAND(x) x /**< MSVC workaround */
#define MONGO_invariant_PICK(_1, _2, x, ...) x
#define invariant(...)      \
    MONGO_invariant_EXPAND( \
        MONGO_invariant_PICK(__VA_ARGS__, MONGO_invariant_2, MONGO_invariant_1)(__VA_ARGS__))

/** An `invariant` that's only active in debug builds. Use for slow checks. */
#define MONGO_dassert(...)                  \
    do {                                    \
        if constexpr (::mongo::kDebugBuild) \
            invariant(__VA_ARGS__);         \
    } while (false)

#define dassert MONGO_dassert

}  // namespace mongo
