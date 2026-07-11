// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

/**
 * Defines an `enum class ENUM_` populated by `LIST_`.
 * Also defines an associated function `toStringData(ENUM_)`.
 *
 * Example:
 *   Define an enum class mongo::MyColors.
 *
 *       namespace mongo {
 *       #define MYCOLORS_TABLE(X) \
 *            X(red)               \
 *            X(green)             \
 *            X(blue)
 *
 *       QUERY_UTIL_NAMED_ENUM_DEFINE(MyColors, MYCOLORS_TABLE)
 *       #undef MYCOLORS_TABLE
 *       }  // namespace mongo
 *
 *   Its elements are MyColors::red, MyColors::green, and MyColors::blue. We
 *   also define an associated toStringData(MyColors) function which returns
 *   the unqualified value names "red", "green", "blue" as constexpr
 *   std::string_view. The array of unqualified std::string_view names is accessible via
 *   the arr_ field; in the example above, this would be MyColors_EnumString::arr_.
 */

#define QUERY_UTIL_NAMED_ENUM_DEFINE(ENUM_, LIST_)                                                 \
    namespace ENUM_##EnumString {                                                                  \
        inline constexpr std::string_view arr_[] = {LIST_(QUERY_UTIL_NAMED_ENUM_INTERNAL_X_STR_)}; \
    }                                                                                              \
    enum class ENUM_ { LIST_(QUERY_UTIL_NAMED_ENUM_INTERNAL_X_) };                                 \
    constexpr std::string_view toStringData(ENUM_ v_) {                                            \
        return ENUM_##EnumString::arr_[static_cast<size_t>(v_)];                                   \
    }
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_(x) x,
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_STR_(x) #x,
