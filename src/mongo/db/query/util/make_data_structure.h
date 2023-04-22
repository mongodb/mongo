/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <list>
#include <type_traits>

namespace mongo {

namespace detail {

/**
 * Appends an element with an operator* to the end of a data structure. If the operator* produces
 * the data structure's element type, it will be called first unless the element argument is boolean
 * convertable to false. If this is the case, this function will perform no action. The bool
 * argument gives this function overload priority.
 */
template <typename T, typename DS, typename Arg>
auto pushBackUnlessNone(DS&& ds, Arg&& arg, bool) -> decltype(*arg, void()) {
    if constexpr (std::is_convertible_v<std::decay_t<decltype(*arg)>, T>) {
        if (arg)
            ds.push_back(*std::forward<Arg>(arg));
    } else {
        ds.push_back(std::forward<Arg>(arg));
    }
}

/**
 * Appends an element to the end of a data structure. SFINAE backup for elements without an
 * operator*. The ... arguments casue this function to lose overload priority.
 */
template <typename T, typename DS, typename Arg>
void pushBackUnlessNone(DS&& ds, Arg&& arg, ...) {
    ds.push_back(std::forward<Arg>(arg));
}

/**
 * Helpers for makeVector. Pass along the given type if a type is given for the vector elements, or
 * deduce the type and use that if void.
 */
template <typename T, typename...>
struct vecTypeHelper {
    using type = T;
};

template <typename... Args>
struct vecTypeHelper<void, Args...> {
    using type = typename std::common_type<Args...>::type;
};

template <typename T, typename... Args>
using vecTypeHelperT = typename vecTypeHelper<T, Args...>::type;

}  // namespace detail

/**
 * Create a vector. Unlike an initializer list, this function will allow passing elements by Rvalue
 * reference.
 */
template <typename T = void, typename... Args, typename V = detail::vecTypeHelperT<T, Args...>>
auto makeVector(Args&&... args) {
    std::vector<V> v;
    v.reserve(sizeof...(Args));
    (v.push_back(std::forward<Args>(args)), ...);
    return v;
}

template <typename T, typename U>
void addExprIfNotNull(std::vector<T>& v, U&& expr) {
    if (expr) {
        v.push_back(std::forward<U>(expr));
    }
}

/**
 * Creates a vector of which all elements are not null.
 */
template <typename T = void, typename... Args, typename V = detail::vecTypeHelperT<T, Args...>>
auto makeVectorIfNotNull(Args&&... args) {
    std::vector<V> v;
    (addExprIfNotNull(v, std::forward<Args>(args)), ...);
    return v;
}

/**
 * Create a list. unlike an initializer list, this function will allow passing elements by Rvalue
 * reference. If an argument is dereferenceable (operator*) to produce the new list's element type,
 * the dereferencing will be performed before the argument is passed to the list. If an argument is
 * dereferenceable and boolean convertable to false, it will be skipped.
 */
template <typename T, typename... Args>
auto makeFlattenedList(Args&&... args) {
    std::list<T> l;
    (detail::pushBackUnlessNone<T>(l, std::forward<Args>(args), true), ...);
    return l;
}

}  // namespace mongo
