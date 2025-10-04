// Copyright 2017 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_HASH_IS_UNORDERED_RANGE_HPP_INCLUDED
#define BOOST_HASH_IS_UNORDERED_RANGE_HPP_INCLUDED

#include <boost/container_hash/is_range.hpp>
#include <type_traits>

namespace boost
{
namespace hash_detail
{

template<class T, class E = std::true_type> struct has_hasher_: std::false_type
{
};

template<class T> struct has_hasher_< T, std::integral_constant< bool,
        std::is_same<typename T::hasher, typename T::hasher>::value
    > >: std::true_type
{
};

} // namespace hash_detail

namespace container_hash
{

template<class T> struct is_unordered_range: std::integral_constant< bool, is_range<T>::value && hash_detail::has_hasher_<T>::value >
{
};

} // namespace container_hash
} // namespace boost

#endif // #ifndef BOOST_HASH_IS_UNORDERED_RANGE_HPP_INCLUDED
