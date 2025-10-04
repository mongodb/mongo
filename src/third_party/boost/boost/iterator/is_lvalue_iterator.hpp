// Copyright David Abrahams 2003. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef IS_LVALUE_ITERATOR_DWA2003112_HPP
#define IS_LVALUE_ITERATOR_DWA2003112_HPP

#include <boost/iterator/detail/type_traits/conjunction.hpp>

#include <iterator>
#include <type_traits>

namespace boost {
namespace iterators {
namespace detail {

// Guts of is_lvalue_iterator.  It is the iterator type and
// Value is the iterator's value_type.
template< typename It, typename Value >
struct is_lvalue_iterator_impl :
    public detail::conjunction<
        std::is_convertible< decltype(*std::declval< It& >()), typename std::add_lvalue_reference< Value >::type >,
        std::is_lvalue_reference< decltype(*std::declval< It& >()) >
    >::type
{
};

//
// void specializations to handle std input and output iterators
//
template< typename It >
struct is_lvalue_iterator_impl< It, void > :
    public std::false_type
{
};

template< typename It >
struct is_lvalue_iterator_impl< It, const void > :
    public std::false_type
{
};

template< typename It >
struct is_lvalue_iterator_impl< It, volatile void > :
    public std::false_type
{
};

template< typename It >
struct is_lvalue_iterator_impl< It, const volatile void > :
    public std::false_type
{
};

} // namespace detail

template< typename T >
struct is_lvalue_iterator :
    public iterators::detail::is_lvalue_iterator_impl<
        T,
        typename std::iterator_traits< T >::value_type const
    >::type
{
};

template< typename T >
struct is_non_const_lvalue_iterator :
    public iterators::detail::is_lvalue_iterator_impl<
        T,
        typename std::iterator_traits< T >::value_type
    >::type
{
};

} // namespace iterators

using iterators::is_lvalue_iterator;
using iterators::is_non_const_lvalue_iterator;

} // namespace boost

#endif // IS_LVALUE_ITERATOR_DWA2003112_HPP
