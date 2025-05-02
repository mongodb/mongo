#ifndef BOOST_HASH_IS_TUPLE_LIKE_HPP_INCLUDED
#define BOOST_HASH_IS_TUPLE_LIKE_HPP_INCLUDED

// Copyright 2017, 2022 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <type_traits>
#include <utility>

namespace boost
{
namespace hash_detail
{

template<class T, class E = std::true_type> struct is_tuple_like_: std::false_type
{
};

template<class T> struct is_tuple_like_<T, std::integral_constant<bool, std::tuple_size<T>::value == std::tuple_size<T>::value> >: std::true_type
{
};

} // namespace hash_detail

namespace container_hash
{

template<class T> struct is_tuple_like: hash_detail::is_tuple_like_< typename std::remove_cv<T>::type >
{
};

} // namespace container_hash
} // namespace boost

#endif // #ifndef BOOST_HASH_IS_TUPLE_LIKE_HPP_INCLUDED
