// Copyright David Abrahams 2003.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef ITERATOR_TRAITS_DWA200347_HPP
#define ITERATOR_TRAITS_DWA200347_HPP

#include <iterator>

namespace boost {
namespace iterators {

template< typename Iterator >
using iterator_value_t = typename std::iterator_traits< Iterator >::value_type;

template< typename Iterator >
struct iterator_value
{
    using type = iterator_value_t< Iterator >;
};

template< typename Iterator >
using iterator_reference_t = typename std::iterator_traits< Iterator >::reference;

template< typename Iterator >
struct iterator_reference
{
    using type = iterator_reference_t< Iterator >;
};

template< typename Iterator >
using iterator_pointer_t = typename std::iterator_traits< Iterator >::pointer;

template< typename Iterator >
struct iterator_pointer
{
    using type = iterator_pointer_t< Iterator >;
};

template< typename Iterator >
using iterator_difference_t = typename std::iterator_traits< Iterator >::difference_type;

template< typename Iterator >
struct iterator_difference
{
    using type = iterator_difference_t< Iterator >;
};

template< typename Iterator >
using iterator_category_t = typename std::iterator_traits< Iterator >::iterator_category;

template< typename Iterator >
struct iterator_category
{
    using type = iterator_category_t< Iterator >;
};

} // namespace iterators

using iterators::iterator_value;
using iterators::iterator_reference;
using iterators::iterator_pointer;
using iterators::iterator_difference;
using iterators::iterator_category;

} // namespace boost

#endif // ITERATOR_TRAITS_DWA200347_HPP
