/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
