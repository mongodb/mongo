/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/variadic/to_seq.hpp>

namespace mongo {
namespace detail {

template <typename... T>
constexpr bool alwaysFalse = false;

#define IDL_ENUM_SIZE(enumName) kNum##enumName

#define IDL_ENUM_SIZE_TEMPLATE_HELPER_PRELUDE(name)        \
    template <typename T>                                  \
    struct name##EnumSizeTemplateHelper {                  \
        static constexpr auto getSize() {                  \
            if constexpr (mongo::detail::alwaysFalse<T>) { \
            }

#define IDL_ENUM_SIZE_TEMPLATE_HELPER_INTERLUDE(r, data, enumName) \
    else if constexpr (std::is_same_v<T, enumName>) {              \
        return IDL_ENUM_SIZE(enumName);                            \
    }

#define IDL_ENUM_SIZE_TEMPLATE_HELPER_POSTLUDE                                                     \
    else {                                                                                         \
        static_assert(                                                                             \
            mongo::detail::alwaysFalse<T>,                                                         \
            "Given template parameter was not listed in DEFINE_IDL_ENUM_SIZE_TEMPLATE_HELPER for " \
            "this function and is therefore not supported.");                                      \
    }                                                                                              \
    }                                                                                              \
    }                                                                                              \
    ;

#define DEFINE_IDL_ENUM_SIZE_TEMPLATE_HELPER(name, ...)                                    \
    IDL_ENUM_SIZE_TEMPLATE_HELPER_PRELUDE(name)                                            \
    BOOST_PP_SEQ_FOR_EACH(                                                                 \
        IDL_ENUM_SIZE_TEMPLATE_HELPER_INTERLUDE, _, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)) \
    IDL_ENUM_SIZE_TEMPLATE_HELPER_POSTLUDE

}  // namespace detail
}  // namespace mongo
