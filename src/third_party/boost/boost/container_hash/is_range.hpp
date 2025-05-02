// Copyright 2017 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_HASH_IS_RANGE_HPP_INCLUDED
#define BOOST_HASH_IS_RANGE_HPP_INCLUDED

#include <iterator>
#include <type_traits>

namespace boost
{

namespace hash_detail
{

template<class T> struct iterator_traits: std::iterator_traits<T> {};
template<> struct iterator_traits< void* > {};
template<> struct iterator_traits< void const* > {};

template<class T, class It>
    std::integral_constant< bool, !std::is_same<typename std::remove_cv<T>::type, typename iterator_traits<It>::value_type>::value >
        is_range_check( It first, It last );

template<class T> decltype( is_range_check<T>( std::declval<T const&>().begin(), std::declval<T const&>().end() ) ) is_range_( int );
template<class T> std::false_type is_range_( ... );

} // namespace hash_detail

namespace container_hash
{

template<class T> struct is_range: decltype( hash_detail::is_range_<T>( 0 ) )
{
};

} // namespace container_hash

} // namespace boost

#endif // #ifndef BOOST_HASH_IS_RANGE_HPP_INCLUDED
