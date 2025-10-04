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

#include <cstdint>
#include <iosfwd>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

namespace MONGO_MOD_PUB mongo {

class Status;
class DBException;

// ErrorExtraInfo subclasses:
//#for $ec in $codes:
//#if $ec.extra
//#if $ec.extra_ns
namespace $ec.extra_ns {
    //#end if
    class $ec.extra_class;
    //#if $ec.extra_ns
}  // namespace $ec.extra_ns
//#end if
//#end if
//#end for

enum class ErrorCategory {
    //#for $cat in $categories
    ${cat.name},
    //#end for
};

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
    static Error fromString(StringData name);

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

MONGO_MOD_NEEDS_REPLACEMENT MONGO_COMPILER_NORETURN void throwExceptionForStatus(const Status& status);

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

}  // namespace mongo
