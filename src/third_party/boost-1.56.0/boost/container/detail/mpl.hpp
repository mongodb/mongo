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

#ifndef BOOST_CONTAINER_CONTAINER_DETAIL_MPL_HPP
#define BOOST_CONTAINER_CONTAINER_DETAIL_MPL_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

#include <cstddef>

namespace boost {
namespace container {
namespace container_detail {

template <class T, T val>
struct integral_constant
{
   static const T value = val;
   typedef integral_constant<T,val> type;
};

template< bool C_ >
struct bool_ : integral_constant<bool, C_>
{
   static const bool value = C_;
   operator bool() const { return bool_::value; }
};

typedef bool_<true>        true_;
typedef bool_<false>       false_;

typedef true_  true_type;
typedef false_ false_type;

typedef char yes_type;
struct no_type
{
   char padding[8];
};

template <bool B, class T = void>
struct enable_if_c {
  typedef T type;
};

template <class T>
struct enable_if_c<false, T> {};

template <class Cond, class T = void>
struct enable_if : public enable_if_c<Cond::value, T> {};

template <class Cond, class T = void>
struct disable_if : public enable_if_c<!Cond::value, T> {};

template <bool B, class T = void>
struct disable_if_c : public enable_if_c<!B, T> {};

#if defined(_MSC_VER) && (_MSC_VER >= 1400)

template <class T, class U>
struct is_convertible
{
   static const bool value = __is_convertible_to(T, U);
};

#else

template <class T, class U>
class is_convertible
{
   typedef char true_t;
   class false_t { char dummy[2]; };
   //use any_conversion as first parameter since in MSVC
   //overaligned types can't go through ellipsis
   static false_t dispatch(...);
   static true_t  dispatch(U);
   static T &trigger();
   public:
   static const bool value = sizeof(dispatch(trigger())) == sizeof(true_t);
};

#endif

template<
      bool C
    , typename T1
    , typename T2
    >
struct if_c
{
    typedef T1 type;
};

template<
      typename T1
    , typename T2
    >
struct if_c<false,T1,T2>
{
    typedef T2 type;
};

template<
      typename T1
    , typename T2
    , typename T3
    >
struct if_
{
   typedef typename if_c<0 != T1::value, T2, T3>::type type;
};


template <class Pair>
struct select1st
{
	typedef Pair                        argument_type;
	typedef typename Pair::first_type   result_type;

   template<class OtherPair>
   const typename Pair::first_type& operator()(const OtherPair& x) const
   {  return x.first;   }

   const typename Pair::first_type& operator()(const typename Pair::first_type& x) const
   {  return x;   }
};

// identity is an extension: it is not part of the standard.
template <class T>
struct identity
{
	typedef T   argument_type;
	typedef T   result_type;

   typedef T type;
   const T& operator()(const T& x) const
   { return x; }
};

template<std::size_t S>
struct ls_zeros
{
   static const std::size_t value = (S & std::size_t(1)) ? 0 : (1u + ls_zeros<(S >> 1u)>::value);
};

template<>
struct ls_zeros<0>
{
   static const std::size_t value = 0;
};

template<>
struct ls_zeros<1>
{
   static const std::size_t value = 0;
};

template <typename T> struct unvoid { typedef T type; };
template <> struct unvoid<void> { struct type { }; };
template <> struct unvoid<const void> { struct type { }; };

}  //namespace container_detail {
}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_CONTAINER_DETAIL_MPL_HPP

