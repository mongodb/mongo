// Copyright 2022 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_HASH_IS_DESCRIBED_CLASS_HPP_INCLUDED
#define BOOST_HASH_IS_DESCRIBED_CLASS_HPP_INCLUDED

#include <boost/describe/bases.hpp>
#include <boost/describe/members.hpp>
#include <type_traits>

namespace boost
{
namespace container_hash
{

#if defined(BOOST_DESCRIBE_CXX11)

template<class T> struct is_described_class: std::integral_constant<bool,
    describe::has_describe_bases<T>::value &&
    describe::has_describe_members<T>::value &&
    !std::is_union<T>::value>
{
};

#else

template<class T> struct is_described_class: std::false_type
{
};

#endif

} // namespace container_hash
} // namespace boost

#endif // #ifndef BOOST_HASH_IS_DESCRIBED_CLASS_HPP_INCLUDED
