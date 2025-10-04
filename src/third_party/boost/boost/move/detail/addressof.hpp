//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2014-2015. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_MOVE_DETAIL_ADDRESSOF_HPP
#define BOOST_MOVE_DETAIL_ADDRESSOF_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/move/detail/workaround.hpp>

namespace boost {
namespace move_detail {

#if defined(BOOST_MSVC_FULL_VER) && BOOST_MSVC_FULL_VER >= 190024215
#define BOOST_MOVE_HAS_BUILTIN_ADDRESSOF
#elif defined(BOOST_GCC) && BOOST_GCC >= 70000
#define BOOST_MOVE_HAS_BUILTIN_ADDRESSOF
#elif defined(__has_builtin)
#if __has_builtin(__builtin_addressof)
#define BOOST_MOVE_HAS_BUILTIN_ADDRESSOF
#endif
#endif

#ifdef BOOST_MOVE_HAS_BUILTIN_ADDRESSOF

template<class T>
BOOST_MOVE_FORCEINLINE T *addressof( T & v ) BOOST_NOEXCEPT
{
   return __builtin_addressof(v);
}

#else //BOOST_MOVE_HAS_BUILTIN_ADDRESSOF

template <typename T>
BOOST_MOVE_FORCEINLINE T* addressof(T& obj)
{
   return static_cast<T*>(
      static_cast<void*>(
         const_cast<char*>(
            &reinterpret_cast<const volatile char&>(obj)
   )));
}

#endif   //BOOST_MOVE_HAS_BUILTIN_ADDRESSOF

}  //namespace move_detail {
}  //namespace boost {

#endif   //#ifndef BOOST_MOVE_DETAIL_ADDRESSOF_HPP
