//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012.
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_ALGORITHMS_HPP
#define BOOST_CONTAINER_DETAIL_ALGORITHMS_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include "config_begin.hpp"
#include <boost/container/detail/workaround.hpp>

#include <boost/type_traits/has_trivial_copy.hpp>
#include <boost/type_traits/has_trivial_assign.hpp>
#include <boost/detail/no_exceptions_support.hpp>

#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/iterators.hpp>


#include <cstring>

namespace boost {
namespace container {

template<class It>
struct is_value_init_construct_iterator
{
   static const bool value = false;
};

template<class U, class D>
struct is_value_init_construct_iterator<value_init_construct_iterator<U, D> >
{
   static const bool value = true;
};

template<class It>
struct is_emplace_iterator
{
   static const bool value = false;
};

template<class U, class EF, class D>
struct is_emplace_iterator<emplace_iterator<U, EF, D> >
{
   static const bool value = true;
};

template<class A, class T, class InpIt>
inline void construct_in_place(A &a, T* dest, InpIt source)
{     boost::container::allocator_traits<A>::construct(a, dest, *source);  }
//#endif

template<class A, class T, class U, class D>
inline void construct_in_place(A &a, T *dest, value_init_construct_iterator<U, D>)
{
   boost::container::allocator_traits<A>::construct(a, dest);
}

template<class A, class T, class U, class D>
inline void construct_in_place(A &a, T *dest, default_init_construct_iterator<U, D>)
{
   boost::container::allocator_traits<A>::construct(a, dest, default_init);
}

template<class A, class T, class U, class EF, class D>
inline void construct_in_place(A &a, T *dest, emplace_iterator<U, EF, D> ei)
{
   ei.construct_in_place(a, dest);
}

}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_DETAIL_ALGORITHMS_HPP

