// Copyright Andrey Semashev 2025.
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_ITERATOR_MIN_CATEGORY_HPP_INCLUDED_
#define BOOST_ITERATOR_MIN_CATEGORY_HPP_INCLUDED_

#include <type_traits>

namespace boost {
namespace iterators {
namespace detail {

template<
    typename T1,
    typename T2,
    bool GreaterEqual = std::is_convertible< T1, T2 >::value,
    bool LessEqual = std::is_convertible< T2, T1 >::value
>
struct min_category_impl
{
    static_assert(GreaterEqual || LessEqual, "Iterator category types must be related through convertibility.");
};

template< typename T1, typename T2 >
struct min_category_impl< T1, T2, true, false >
{
    using type = T2;
};

template< typename T1, typename T2 >
struct min_category_impl< T1, T2, false, true >
{
    using type = T1;
};

template< typename T1, typename T2 >
struct min_category_impl< T1, T2, true, true >
{
    static_assert(std::is_same< T1, T2 >::value, "Iterator category types must be the same when they are equivalent.");
    using type = T1;
};

} // namespace detail

//
// Returns the minimum iterator category type in the list
// or fails to compile if any of the categories are unrelated.
//
template< typename... Categories >
struct min_category;

template< typename T >
struct min_category< T >
{
    using type = T;
};

template< typename T1, typename T2, typename... Tail >
struct min_category< T1, T2, Tail... >
{
    using type = typename min_category<
        typename iterators::detail::min_category_impl< T1, T2 >::type,
        Tail...
    >::type;
};

// Shortcut to slightly optimize compilation speed
template< typename T1, typename T2 >
struct min_category< T1, T2 >
{
    using type = typename iterators::detail::min_category_impl< T1, T2 >::type;
};

template< typename... Categories >
using min_category_t = typename min_category< Categories... >::type;

} // namespace iterators
} // namespace boost

#endif // BOOST_ITERATOR_MIN_CATEGORY_HPP_INCLUDED_
