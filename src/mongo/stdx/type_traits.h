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

#include <type_traits>

#include "mongo/config.h"

#if defined(MONGO_CONFIG_HAVE_STD_ENABLE_IF_T)

namespace mongo {
namespace stdx {

using ::std::enable_if_t;

}  // namespace stdx
}  // namespace mongo

#else

namespace mongo {
namespace stdx {

template <bool B, class T = void>
using enable_if_t = typename std::enable_if<B, T>::type;

}  // namespace stdx
}  // namespace mongo
#endif

// TODO: Deal with importing this from C++20, when the time comes.
namespace mongo {
namespace stdx {

template <typename T>
struct type_identity {
    using type = T;
};

template <typename T>
using type_identity_t = stdx::type_identity<T>;

}  // namespace stdx
}  // namespace mongo


// TODO: Re-evaluate which of these we need when making the cutover to C++17.

namespace mongo {
namespace stdx {

namespace detail {
template <typename...>
struct make_void {
    using type = void;
};
}  // namespace detail

template <typename... Args>
using void_t = typename detail::make_void<Args...>::type;

template <bool b>
using bool_constant = std::integral_constant<bool, b>;

template <typename T>
struct negation : stdx::bool_constant<!bool(T::value)> {};

template <typename... B>
struct disjunction : std::false_type {};
template <typename B>
struct disjunction<B> : B {};
template <typename B1, typename... B>
struct disjunction<B1, B...> : std::conditional_t<bool(B1::value), B1, stdx::disjunction<B...>> {};

template <typename...>
struct conjunction : std::true_type {};
template <typename B>
struct conjunction<B> : B {};
template <typename B1, typename... B>
struct conjunction<B1, B...> : std::conditional_t<bool(B1::value), stdx::conjunction<B...>, B1> {};

namespace detail {
template <typename Func,
          typename... Args,
          typename = typename std::result_of<Func && (Args && ...)>::type>
auto is_invocable_impl(Func&& func, Args&&... args) -> std::true_type;
auto is_invocable_impl(...) -> std::false_type;
}  // namespace detail

template <typename Func, typename... Args>
struct is_invocable
    : decltype(detail::is_invocable_impl(std::declval<Func>(), std::declval<Args>()...)) {};

namespace detail {

// This helps solve the lack of regular void problem, when passing a 'conversion target' as a
// parameter.

template <typename R,
          typename Func,
          typename... Args,
          typename ComputedResult = typename std::result_of<Func && (Args && ...)>::type>
auto is_invocable_r_impl(stdx::type_identity<R>, Func&& func, Args&&... args) ->
    typename stdx::disjunction<std::is_void<R>,
                               std::is_same<ComputedResult, R>,
                               std::is_convertible<ComputedResult, R>>::type;
auto is_invocable_r_impl(...) -> std::false_type;
}  // namespace detail

template <typename R, typename Func, typename... Args>
struct is_invocable_r
    : decltype(detail::is_invocable_r_impl(
          stdx::type_identity<R>(), std::declval<Func>(), std::declval<Args>()...)) {};

}  // namespace stdx
}  // namespace mongo
