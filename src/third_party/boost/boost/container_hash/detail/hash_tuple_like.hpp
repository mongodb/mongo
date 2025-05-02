// Copyright 2005-2009 Daniel James.
// Copyright 2021 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_HASH_DETAIL_HASH_TUPLE_LIKE_HPP
#define BOOST_HASH_DETAIL_HASH_TUPLE_LIKE_HPP

#include <boost/container_hash/hash_fwd.hpp>
#include <boost/container_hash/is_tuple_like.hpp>
#include <boost/container_hash/is_range.hpp>
#include <type_traits>
#include <utility>

namespace boost
{
namespace hash_detail
{

template <std::size_t I, typename T>
inline
typename std::enable_if<(I == std::tuple_size<T>::value), void>::type
    hash_combine_tuple_like( std::size_t&, T const& )
{
}

template <std::size_t I, typename T>
inline
typename std::enable_if<(I < std::tuple_size<T>::value), void>::type
    hash_combine_tuple_like( std::size_t& seed, T const& v )
{
    using std::get;
    boost::hash_combine( seed, get<I>( v ) );

    boost::hash_detail::hash_combine_tuple_like<I + 1>( seed, v );
}

template <typename T>
inline std::size_t hash_tuple_like( T const& v )
{
    std::size_t seed = 0;

    boost::hash_detail::hash_combine_tuple_like<0>( seed, v );

    return seed;
}

} // namespace hash_detail

template <class T>
inline
typename std::enable_if<
    container_hash::is_tuple_like<T>::value && !container_hash::is_range<T>::value,
std::size_t>::type
    hash_value( T const& v )
{
    return boost::hash_detail::hash_tuple_like( v );
}

} // namespace boost

#endif // #ifndef BOOST_HASH_DETAIL_HASH_TUPLE_LIKE_HPP
