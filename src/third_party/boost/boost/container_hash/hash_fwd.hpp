// Copyright 2005-2009 Daniel James.
// Copyright 2021, 2022 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_FUNCTIONAL_HASH_FWD_HPP
#define BOOST_FUNCTIONAL_HASH_FWD_HPP

#include <cstddef>

namespace boost
{

namespace container_hash
{

template<class T> struct is_range;
template<class T> struct is_contiguous_range;
template<class T> struct is_unordered_range;
template<class T> struct is_described_class;
template<class T> struct is_tuple_like;

} // namespace container_hash

template<class T> struct hash;

template<class T> void hash_combine( std::size_t& seed, T const& v );

template<class It> void hash_range( std::size_t&, It, It );
template<class It> std::size_t hash_range( It, It );

template<class It> void hash_unordered_range( std::size_t&, It, It );
template<class It> std::size_t hash_unordered_range( It, It );

} // namespace boost

#endif // #ifndef BOOST_FUNCTIONAL_HASH_FWD_HPP
