// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <list>
#include <type_traits>

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

namespace [[MONGO_MOD_PRIVATE]] detail {

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
