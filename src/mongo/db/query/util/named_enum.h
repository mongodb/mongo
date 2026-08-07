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
 *   the arr_ field; in the example above, this would be MyColorsEnumString::arr_.
 *
 *   A table entry may instead supply an explicit name as a second argument, for values whose
 *   reported name differs from the C++ enumerator - e.g. an enumerator following the 'k' prefix
 *   convention, or one whose name must match an externally fixed spelling:
 *
 *       #define MYCOLORS_TABLE(X) \
 *            X(kRed, "red")       \
 *            X(kGreen, "green")   \
 *            X(kBlue, "blue")
 *
 *   Both entry forms may be mixed within one table; an entry with no explicit name continues to
 *   report the enumerator itself.
 */

#define QUERY_UTIL_NAMED_ENUM_DEFINE(ENUM_, LIST_)                                                 \
    namespace ENUM_##EnumString {                                                                  \
        inline constexpr std::string_view arr_[] = {LIST_(QUERY_UTIL_NAMED_ENUM_INTERNAL_X_STR_)}; \
    }                                                                                              \
    enum class ENUM_ { LIST_(QUERY_UTIL_NAMED_ENUM_INTERNAL_X_) };                                 \
    constexpr std::string_view toStringData(ENUM_ v_) {                                            \
        return ENUM_##EnumString::arr_[static_cast<size_t>(v_)];                                   \
    }

/**
 * Table entries accept either form, 'X(enumerator)' or 'X(enumerator, "name")', so both the
 * enumerator expansion and the name expansion dispatch on the entry's argument count. SELECT_
 * receives the entry's arguments followed by the two candidate expansions and returns whichever
 * one lands in its third parameter: with one entry argument that is the 1-argument expansion, and
 * with two it is the 2-argument one (which pushes the other candidate into the trailing '...').
 */
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_SELECT_(_1, _2, CHOSEN_, ...) CHOSEN_

/**
 * MSVC's traditional preprocessor passes '__VA_ARGS__' to a nested macro as a single argument
 * rather than as a comma-separated list, which would defeat the dispatch above. Routing each
 * dispatch through this identity macro forces the extra rescan that splits it back apart.
 */
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_EXPAND_(...) __VA_ARGS__

#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_(...)                                     \
    QUERY_UTIL_NAMED_ENUM_INTERNAL_EXPAND_(QUERY_UTIL_NAMED_ENUM_INTERNAL_SELECT_( \
        __VA_ARGS__,                                                               \
        QUERY_UTIL_NAMED_ENUM_INTERNAL_X_NAMED_,                                   \
        QUERY_UTIL_NAMED_ENUM_INTERNAL_X_PLAIN_)(__VA_ARGS__))
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_PLAIN_(x) x,
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_NAMED_(x, name) x,

#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_STR_(...)                                 \
    QUERY_UTIL_NAMED_ENUM_INTERNAL_EXPAND_(QUERY_UTIL_NAMED_ENUM_INTERNAL_SELECT_( \
        __VA_ARGS__,                                                               \
        QUERY_UTIL_NAMED_ENUM_INTERNAL_X_STR_NAMED_,                               \
        QUERY_UTIL_NAMED_ENUM_INTERNAL_X_STR_PLAIN_)(__VA_ARGS__))
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_STR_PLAIN_(x) #x,
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_STR_NAMED_(x, name) name,
