//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013.
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_CONTAINER_DETAIL_IS_PAIR_HPP
#define BOOST_CONTAINER_CONTAINER_DETAIL_IS_PAIR_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/detail/std_fwd.hpp>

#if defined(BOOST_MSVC) && (_CPPLIB_VER == 520)
//MSVC 2010 tuple marker
namespace std { namespace tr1 { struct _Nil; }}
#elif defined(BOOST_MSVC) && (_CPPLIB_VER == 540)
//MSVC 2012 tuple marker
namespace std { struct _Nil; }
#endif

namespace boost {
namespace tuples {

struct null_type;

template <
  class T0, class T1, class T2,
  class T3, class T4, class T5,
  class T6, class T7, class T8,
  class T9>
class tuple;

}  //namespace tuples {
}  //namespace boost {

namespace boost {
namespace container {

struct try_emplace_t{};

namespace dtl {

template <class T1, class T2>
struct pair;

template <class T>
struct is_pair
{
   BOOST_STATIC_CONSTEXPR bool value = false;
};

template <class T1, class T2>
struct is_pair< pair<T1, T2> >
{
   BOOST_STATIC_CONSTEXPR bool value = true;
};

template <class T1, class T2>
struct is_pair< std::pair<T1, T2> >
{
   BOOST_STATIC_CONSTEXPR bool value = true;
};

template <class T>
struct is_not_pair
{
   BOOST_STATIC_CONSTEXPR bool value = !is_pair<T>::value;
};

}  //namespace dtl {
}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_CONTAINER_DETAIL_IS_PAIR_HPP
