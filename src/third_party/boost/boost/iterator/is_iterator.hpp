/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file iterator/is_iterator.hpp
 *
 * This header contains definition of the \c is_iterator type trait.
 */

#ifndef BOOST_ITERATOR_IS_ITERATOR_HPP_INCLUDED_
#define BOOST_ITERATOR_IS_ITERATOR_HPP_INCLUDED_

#include <cstddef>
#include <boost/config.hpp>
#include <boost/type_traits/is_complete.hpp>
#include <boost/iterator/detail/type_traits/conjunction.hpp>
#include <boost/iterator/detail/type_traits/negation.hpp>
#if !defined(BOOST_NO_CXX17_ITERATOR_TRAITS)
#include <iterator>
#endif

#include <type_traits>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace iterators {
namespace detail {

// The trait attempts to detect if the T type is an iterator class. Class-type iterators are assumed
// to have the nested type iterator_category. Strictly speaking, this is not required to be the
// case (e.g. a user can specialize iterator_traits for T without defining T::iterator_category).
// Still, this is a good heuristic in practice, and we can't do anything better anyway.
// Since C++17 we can test for iterator_traits<T>::iterator_category presence instead as it is
// required to be only present for iterators.
namespace has_iterator_category_detail {

typedef char yes_type;
struct no_type { char padding[2]; };

template< typename T >
yes_type check(
#if !defined(BOOST_NO_CXX17_ITERATOR_TRAITS)
    typename std::iterator_traits< T >::iterator_category*
#else
    typename T::iterator_category*
#endif
);
template< typename >
no_type check(...);

} // namespace has_iterator_category_detail

template< typename T >
struct is_iterator_impl :
    public std::integral_constant<
        bool,
        sizeof(has_iterator_category_detail::check<T>(0)) == sizeof(has_iterator_category_detail::yes_type)
    >
{
};

template< typename T >
struct is_iterator_impl< T* > :
    public conjunction<
        boost::is_complete<T>,
        negation< std::is_function< T > >
    >::type
{
};

template< typename T, typename U >
struct is_iterator_impl< T U::* > :
    public std::false_type
{
};

template< typename T >
struct is_iterator_impl<T&> :
    public std::false_type
{
};

template< typename T, std::size_t N >
struct is_iterator_impl< T[N] > :
    public std::false_type
{
};

#if !defined(BOOST_TT_HAS_WORKING_IS_COMPLETE)
template< typename T >
struct is_iterator_impl< T[] > :
    public std::false_type
{
};

template< >
struct is_iterator_impl< void > :
    public std::false_type
{
};

template< >
struct is_iterator_impl< void* > :
    public std::false_type
{
};
#endif // !defined(BOOST_TT_HAS_WORKING_IS_COMPLETE)

} // namespace detail

/*!
 * \brief The type trait detects whether the type \c T is an iterator type.
 *
 * The type trait yields \c true if its argument type \c T, after stripping top level
 * cv qualifiers, is one of the following:
 *
 * - A pointer type, other than a pointer to function, a pointer to a class member,
 *   or a pointer to an incomplete type, including `void`.
 * - A class type for which an iterator category is obtainable. Prior to C++17,
 *   the iterator category must be defined as a public `T::iterator_category` type.
 *   Since C++17, the expression `std::iterator_traits< T >::iterator_category` must
 *   be valid and produce the iterator category type.
 *
 * Otherwise, the type trait yields \c false.
 */
template< typename T >
struct is_iterator : public detail::is_iterator_impl< T >::type {};
template< typename T >
struct is_iterator< const T > : public detail::is_iterator_impl< T >::type {};
template< typename T >
struct is_iterator< volatile T > : public detail::is_iterator_impl< T >::type {};
template< typename T >
struct is_iterator< const volatile T > : public detail::is_iterator_impl< T >::type {};

} // namespace iterators

using iterators::is_iterator;

} // namespace boost

#endif // BOOST_ITERATOR_IS_ITERATOR_HPP_INCLUDED_
