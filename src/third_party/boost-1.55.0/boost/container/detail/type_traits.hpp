//////////////////////////////////////////////////////////////////////////////
// (C) Copyright John Maddock 2000.
// (C) Copyright Ion Gaztanaga 2005-2012.
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
// The alignment_of implementation comes from John Maddock's boost::alignment_of code
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_CONTAINER_DETAIL_TYPE_TRAITS_HPP
#define BOOST_CONTAINER_CONTAINER_DETAIL_TYPE_TRAITS_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include "config_begin.hpp"

#include <boost/move/utility.hpp>

namespace boost {
namespace container {
namespace container_detail {

struct nat{};

template <typename U>
struct LowPriorityConversion
{
   // Convertible from T with user-defined-conversion rank.
   LowPriorityConversion(const U&) { }
};

//boost::alignment_of yields to 10K lines of preprocessed code, so we
//need an alternative
template <typename T> struct alignment_of;

template <typename T>
struct alignment_of_hack
{
    char c;
    T t;
    alignment_of_hack();
};

template <unsigned A, unsigned S>
struct alignment_logic
{
    enum{   value = A < S ? A : S  };
};

template< typename T >
struct alignment_of
{
   enum{ value = alignment_logic
            < sizeof(alignment_of_hack<T>) - sizeof(T)
            , sizeof(T)>::value   };
};

//This is not standard, but should work with all compilers
union max_align
{
   char        char_;
   short       short_;
   int         int_;
   long        long_;
   #ifdef BOOST_HAS_LONG_LONG
   long long   long_long_;
   #endif
   float       float_;
   double      double_;
   long double long_double_;
   void *      void_ptr_;
};

template<class T>
struct remove_reference
{
   typedef T type;
};

template<class T>
struct remove_reference<T&>
{
   typedef T type;
};

#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES

template<class T>
struct remove_reference<T&&>
{
   typedef T type;
};

#else

template<class T>
struct remove_reference< ::boost::rv<T> >
{
   typedef T type;
};

#endif

template<class T>
struct is_reference
{
   enum {  value = false   };
};

template<class T>
struct is_reference<T&>
{
   enum {  value = true   };
};

template<class T>
struct is_pointer
{
   enum {  value = false   };
};

template<class T>
struct is_pointer<T*>
{
   enum {  value = true   };
};

template <typename T>
struct add_reference
{
    typedef T& type;
};

template<class T>
struct add_reference<T&>
{
    typedef T& type;
};

template<>
struct add_reference<void>
{
    typedef nat &type;
};

template<>
struct add_reference<const void>
{
    typedef const nat &type;
};

template <class T>
struct add_const_reference
{  typedef const T &type;   };

template <class T>
struct add_const_reference<T&>
{  typedef T& type;   };

template <typename T, typename U>
struct is_same
{
   typedef char yes_type;
   struct no_type
   {
      char padding[8];
   };

   template <typename V>
   static yes_type is_same_tester(V*, V*);
   static no_type is_same_tester(...);

   static T *t;
   static U *u;

   static const bool value = sizeof(yes_type) == sizeof(is_same_tester(t,u));
};

template<class T>
struct remove_const
{
   typedef T type;
};

template<class T>
struct remove_const< const T>
{
   typedef T type;
};

template<class T>
struct remove_ref_const
{
   typedef typename remove_const< typename remove_reference<T>::type >::type type;
};

} // namespace container_detail
}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_CONTAINER_DETAIL_TYPE_TRAITS_HPP
