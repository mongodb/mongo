#ifndef BOOST_COMPAT_TO_ARRAY_HPP_INCLUDED
#define BOOST_COMPAT_TO_ARRAY_HPP_INCLUDED

// Copyright 2024 Ruben Perez Hidalgo
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/compat/integer_sequence.hpp>
#include <boost/compat/type_traits.hpp>

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace boost {
namespace compat {

namespace detail {

template <class T, std::size_t N, std::size_t... I>
constexpr std::array<remove_cv_t<T>, N> to_array_lvalue(T (&a)[N], index_sequence<I...>)
{
    return {{a[I]...}};
}

template <class T, std::size_t N, std::size_t... I>
constexpr std::array<remove_cv_t<T>, N> to_array_rvalue(T (&&a)[N], index_sequence<I...>)
{
    return {{std::move(a[I])...}};
}

}  // namespace detail

template <class T, std::size_t N>
constexpr std::array<remove_cv_t<T>, N> to_array(T (&a)[N])
{
    static_assert(
        std::is_constructible<remove_cv_t<T>, T&>::value,
        "This overload requires the resulting element type to be constructible from T&"
    );
    static_assert(!std::is_array<T>::value, "to_array does not work for multi-dimensional C arrays");
    return detail::to_array_lvalue(a, make_index_sequence<N>{});
}

template <class T, std::size_t N>
constexpr std::array<remove_cv_t<T>, N> to_array(T (&&a)[N])
{
    static_assert(
        std::is_constructible<remove_cv_t<T>, T&&>::value,
        "This overload requires the resulting element type to be constructible from T&&"
    );
    static_assert(!std::is_array<T>::value, "to_array does not work for multi-dimensional C arrays");
    return detail::to_array_rvalue(static_cast<T(&&)[N]>(a), make_index_sequence<N>{});
}

}  // namespace compat
}  // namespace boost

#endif  // BOOST_COMPAT_TO_ARRAY_HPP_INCLUDED
