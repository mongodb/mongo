/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2025 Andrey Semashev
 */

#ifndef BOOST_ITERATOR_ENABLE_IF_CONVERTIBLE_HPP_INCLUDED_
#define BOOST_ITERATOR_ENABLE_IF_CONVERTIBLE_HPP_INCLUDED_

#include <type_traits>

namespace boost {
namespace iterators {
namespace detail {

//
// Result type used in enable_if_convertible meta function.
// This can be an incomplete type, as only pointers to
// enable_if_convertible< ... >::type are used.
// We could have used void for this, but conversion to
// void* is just too easy.
//
struct enable_type;

} // namespace detail

//
// enable_if for use in adapted iterators constructors.
//
// In order to provide interoperability between adapted constant and
// mutable iterators, adapted iterators will usually provide templated
// conversion constructors of the following form
//
// template <class BaseIterator>
// class adapted_iterator :
//   public iterator_adaptor< adapted_iterator<Iterator>, Iterator >
// {
// public:
//
//   ...
//
//   template <class OtherIterator>
//   adapted_iterator(
//       OtherIterator const& it
//     , typename enable_if_convertible<OtherIterator, Iterator>::type* = 0);
//
//   ...
// };
//
// enable_if_convertible is used to remove those overloads from the overload
// set that cannot be instantiated. For all practical purposes only overloads
// for constant/mutable interaction will remain. This has the advantage that
// meta functions like boost::is_convertible do not return false positives,
// as they can only look at the signature of the conversion constructor
// and not at the actual instantiation.
//
// enable_if_interoperable can be safely used in user code. It falls back to
// always enabled for compilers that don't support enable_if or is_convertible.
// There is no need for compiler specific workarounds in user code.
//
// The operators implementation relies on boost::is_convertible not returning
// false positives for user/library defined iterator types. See comments
// on operator implementation for consequences.
//
template< typename From, typename To >
struct enable_if_convertible :
    public std::enable_if<
        std::is_convertible< From, To >::value,
        boost::iterators::detail::enable_type
    >
{};

template< typename From, typename To >
using enable_if_convertible_t = typename enable_if_convertible< From, To >::type;

} // namespace iterators

using iterators::enable_if_convertible;

} // namespace boost

#endif // BOOST_ITERATOR_ENABLE_IF_CONVERTIBLE_HPP_INCLUDED_
