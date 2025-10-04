#ifndef BOOST_COMPAT_TYPE_TRAITS_HPP_INCLUDED
#define BOOST_COMPAT_TYPE_TRAITS_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <type_traits>

namespace boost {
namespace compat {

template<class T> using remove_const_t = typename std::remove_const<T>::type;
template<class T> using remove_cv_t = typename std::remove_cv<T>::type;
template<class T> using remove_reference_t = typename std::remove_reference<T>::type;
template<class T> using remove_cvref_t = remove_cv_t< remove_reference_t<T> >;

template<class T> using decay_t = typename std::decay<T>::type;

template<bool B, class T = void> using enable_if_t = typename std::enable_if<B, T>::type;

template<bool B, class T, class F> using conditional_t = typename std::conditional<B, T, F>::type;

namespace detail {

template<class...> struct make_void
{
    using type = void;
};

} // namespace detail

template<class... T> using void_t = typename detail::make_void<T...>::type;

template<class T> using add_const_t = typename std::add_const<T>::type;

} // namespace compat
} // namespace boost

#endif // BOOST_COMPAT_TYPE_TRAITS_HPP_INCLUDED
