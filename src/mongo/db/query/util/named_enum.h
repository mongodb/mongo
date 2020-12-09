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

#include "mongo/base/string_data.h"

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
 *   StringData.
 */
#define QUERY_UTIL_NAMED_ENUM_DEFINE(ENUM_, LIST_)                                   \
    enum class ENUM_ { LIST_(QUERY_UTIL_NAMED_ENUM_INTERNAL_X_) };                   \
    constexpr StringData toStringData(ENUM_ v_) {                                    \
        constexpr StringData arr_[] = {LIST_(QUERY_UTIL_NAMED_ENUM_INTERNAL_X_SD_)}; \
        return arr_[static_cast<size_t>(v_)];                                        \
    }
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_(x) x,
#define QUERY_UTIL_NAMED_ENUM_INTERNAL_X_SD_(x) #x ""_sd,
