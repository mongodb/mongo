#ifndef BOOST_SMART_PTR_DETAIL_SP_TYPE_TRAITS_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_SP_TYPE_TRAITS_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <type_traits>

namespace boost
{
namespace detail
{

// std::is_bounded_array (C++20)

template<class T> struct sp_is_bounded_array: std::false_type
{
};

template<class T, std::size_t N> struct sp_is_bounded_array< T[N] >: std::true_type
{
};

// std::is_unbounded_array (C++20)

template<class T> struct sp_is_unbounded_array: std::false_type
{
};

template<class T> struct sp_is_unbounded_array< T[] >: std::true_type
{
};

// std::type_identity (C++20)

template<class T> struct sp_type_identity
{
    typedef T type;
};

// boost::type_with_alignment

template<std::size_t A> struct sp_type_with_alignment
{
    struct alignas(A) type
    {
        unsigned char padding[ A ];
    };
};

} // namespace detail
} // namespace boost

#endif  // #ifndef BOOST_SMART_PTR_DETAIL_SP_TYPE_TRAITS_HPP_INCLUDED
