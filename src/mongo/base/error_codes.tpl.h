// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class Status;
class DBException;

// ErrorExtraInfo subclasses:
//#for $ec in $codes:
//#if $ec.extra
//#set $extra_ns = $ec.extra_ns
//#if $extra_ns
namespace $extra_ns {
//#end if
class $ec.extra_class;
//#if $extra_ns
}  // namespace $extra_ns
//#end if
//#end if
//#end for

enum class ErrorCategory {
    //#for $cat in $categories
    ${cat.name},
    //#end for
};

std::string toString(ErrorCategory cat);
std::ostream& operator<<(std::ostream& os, ErrorCategory cat);

/**
 * This is a generated class containing a table of error codes and their corresponding error
 * strings. The class is derived from the definitions in src/mongo/base/error_codes.yml file and the
 * src/mongo/base/error_codes.tpl.h template.
 *
 * Do not update this file directly. Update src/mongo/base/error_codes.yml instead.
 */
class ErrorCodes {
public:
    // Explicitly 32-bits wide so that non-symbolic values,
    // like uassert codes, are valid.
    enum Error : std::int32_t {
        //#for $ec in $codes
        $ec.name = $ec.code,
        //#end for
        MaxError
    };

    static std::string errorString(Error err);

    /**
     * Parses an Error from its "name".  Returns UnknownError if "name" is unrecognized.
     *
     * NOTE: Also returns UnknownError for the string "UnknownError".
     */
    static Error fromString(std::string_view name);

    /**
     * Reuses a unique numeric code in a way that suppresses the duplicate code detection. This
     * should only be used when testing error cases to ensure that the code under test fails in the
     * right place. It should NOT be used in non-test code to either make a new error site (use
     * ErrorCodes::Error(CODE) for that) or to see if a specific failure case occurred (use named
     * codes for that).
     */
    static Error duplicateCodeForTest(int code) {
        return static_cast<Error>(code);
    }

    /**
     * Generic predicate to test if a given error code is in a category.
     *
     * This version is intended to simplify forwarding by Status and DBException. Non-generic
     * callers should just use the specific isCategoryName() methods instead.
     */
    template <ErrorCategory category>
    static bool isA(Error code);

    template <ErrorCategory category, typename ErrorContainer>
    static bool isA(const ErrorContainer& object);

    //#for $cat in $categories
    static bool is${cat.name}(Error code);
    template <typename ErrorContainer>
    static bool is${cat.name}(const ErrorContainer& object);

    //#end for
    static bool canHaveExtraInfo(Error code);
    static bool mustHaveExtraInfo(Error code);
};

std::ostream& operator<<(std::ostream& stream, ErrorCodes::Error code);

template <ErrorCategory category, typename ErrorContainer>
inline bool ErrorCodes::isA(const ErrorContainer& object) {
    return isA<category>(object.code());
}

//#for $cat in $categories
// Category function declarations for "${cat.name}"
template <>
bool ErrorCodes::isA<ErrorCategory::$cat.name>(Error code);

inline bool ErrorCodes::is${cat.name}(Error code) {
    return isA<ErrorCategory::$cat.name>(code);
}

template <typename ErrorContainer>
inline bool ErrorCodes::is${cat.name}(const ErrorContainer& object) {
    return isA<ErrorCategory::$cat.name>(object.code());
}

//#end for
/**
 * This namespace contains implementation details for our error handling code and should not be used
 * directly in general code.
 */
namespace error_details {

template <int32_t code>
constexpr bool isNamedCode = false;
//#for $ec in $codes
template <>
constexpr inline bool isNamedCode<ErrorCodes::$ec.name> = true;
//#end for

[[MONGO_MOD_NEEDS_REPLACEMENT]] MONGO_COMPILER_NORETURN void throwExceptionForStatus(
    const Status& status);

//
// ErrorCategoriesFor
//

template <ErrorCategory... categories>
struct CategoryList;

template <ErrorCodes::Error code>
struct ErrorCategoriesForImpl {
    using type = CategoryList<>;
};

//#for $ec in $codes:
//#if $ec.categories
template <>
struct ErrorCategoriesForImpl<ErrorCodes::$ec.name> {
    using type = CategoryList<
        //#for $i, $cat in enumerate($ec.categories)
        //#set $comma = '' if i == len($ec.categories) - 1 else ', '
        ErrorCategory::$cat$comma
        //#end for
        >;
};
//#end if
//#end for

template <ErrorCodes::Error code>
using ErrorCategoriesFor = typename ErrorCategoriesForImpl<code>::type;

//
// ErrorExtraInfoFor
//

template <ErrorCodes::Error code>
struct ErrorExtraInfoForImpl {};

//#for $code in $codes
//#if $code.extra
template <>
struct ErrorExtraInfoForImpl<ErrorCodes::$code.name> {
    using type = $code.extra;
};

//#end if
//#end for

template <ErrorCodes::Error code>
using ErrorExtraInfoFor = typename ErrorExtraInfoForImpl<code>::type;

}  // namespace error_details

[[MONGO_MOD_PUBLIC]] std::span<const ErrorCodes::Error> allErrorCodes_forTest();

}  // namespace mongo

template <>
struct fmt::formatter<mongo::ErrorCodes::Error> : fmt::formatter<std::string> {
    auto format(const mongo::ErrorCodes::Error& err, fmt::format_context& ctx) const {
        return fmt::formatter<std::string>::format(mongo::ErrorCodes::errorString(err), ctx);
    }
};

template <>
struct fmt::formatter<mongo::ErrorCategory> : fmt::formatter<std::string> {
    auto format(const mongo::ErrorCategory& cat, fmt::format_context& ctx) const {
        return fmt::formatter<std::string>::format(mongo::toString(cat), ctx);
    }
};
