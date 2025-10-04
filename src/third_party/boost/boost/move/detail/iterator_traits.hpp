//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2014-2014.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/move for documentation.
//
//////////////////////////////////////////////////////////////////////////////

//! \file

#ifndef BOOST_MOVE_DETAIL_ITERATOR_TRAITS_HPP
#define BOOST_MOVE_DETAIL_ITERATOR_TRAITS_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif
#
#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#if (BOOST_CXX_VERSION > 201703L) && defined(__cpp_lib_concepts)

#include <iterator>

#define BOOST_MOVE_CONTIGUOUS_ITERATOR_TAG

namespace boost {
namespace movelib {

   using std::iterator_traits;

   template<class T>
   struct iter_difference
   {
      typedef typename std::iterator_traits<T>::difference_type type;
   };

   template<class T>
   struct iter_value
   {
      typedef typename std::iterator_traits<T>::value_type type;
   };

   template<class T>
   struct iter_category
   {
      typedef typename std::iterator_traits<T>::iterator_category type;
   };

}} //namespace boost::movelib

#else

#include <cstddef>
#include <boost/move/detail/type_traits.hpp>

#include <boost/move/detail/std_ns_begin.hpp>

BOOST_MOVE_STD_NS_BEG

struct input_iterator_tag;
struct forward_iterator_tag;
struct bidirectional_iterator_tag;
struct random_access_iterator_tag;
struct output_iterator_tag;

#if (  (defined(BOOST_GNU_STDLIB) && (__cplusplus > 201703L))\
    || (defined(_LIBCPP_VERSION) && (_LIBCPP_STD_VER > 17))\
    || (defined(_YVALS) && defined(_CPPLIB_VER) && defined(__cpp_lib_concepts))\
    || (__cplusplus >= 202002L)\
    )
#  define BOOST_MOVE_CONTIGUOUS_ITERATOR_TAG
struct contiguous_iterator_tag;

#endif

BOOST_MOVE_STD_NS_END

#include <boost/move/detail/std_ns_end.hpp>

namespace boost{  namespace movelib{

template<class T>
struct iter_difference
{
   typedef typename T::difference_type type;
};

template<class T>
struct iter_difference<T*>
{
   typedef std::ptrdiff_t type;
};

template<class T>
struct iter_value
{
   typedef typename T::value_type type;
};

template<class T>
struct iter_value<T*>
{
   typedef T type;
};

template<class T>
struct iter_value<const T*>
{
   typedef T type;
};

template<class T>
struct iter_category
{
   typedef typename T::iterator_category type;
};


template<class T>
struct iter_category<T*>
{
   typedef std::random_access_iterator_tag type;
};

template<class Iterator>
struct iterator_traits
{
   typedef typename iter_difference<Iterator>::type   difference_type;
   typedef typename iter_value<Iterator>::type        value_type;
   typedef typename Iterator::pointer                 pointer;
   typedef typename Iterator::reference               reference;
   typedef typename iter_category<Iterator>::type     iterator_category;
};

template<class T>
struct iterator_traits<T*>
{
   typedef std::ptrdiff_t                    difference_type;
   typedef T                                 value_type;
   typedef T*                                pointer;
   typedef T&                                reference;
   typedef std::random_access_iterator_tag   iterator_category;
};

template<class T>
struct iterator_traits<const T*>
{
   typedef std::ptrdiff_t                    difference_type;
   typedef T                                 value_type;
   typedef const T*                          pointer;
   typedef const T&                          reference;
   typedef std::random_access_iterator_tag   iterator_category;
};

}} //namespace boost::movelib

#endif   //

#include <boost/move/detail/type_traits.hpp>

namespace boost {
namespace movelib {

template<class T>
struct iter_size
   : boost::move_detail::
      make_unsigned<typename iter_difference<T>::type >
{};

}}  //namespace boost move_detail {

#endif //#ifndef BOOST_MOVE_DETAIL_ITERATOR_TRAITS_HPP
