//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2014-2015. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_MOVE_DETAIL_FORCE_CAST_HPP
#define BOOST_MOVE_DETAIL_FORCE_CAST_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/move/detail/workaround.hpp>

namespace boost {
namespace move_detail {


template <typename T>
BOOST_MOVE_FORCEINLINE T force_ptr(const volatile void *p)
{
   return static_cast<T>(const_cast<void*>(p));
}

}  //namespace move_detail {
}  //namespace boost {

#endif   //#ifndef BOOST_MOVE_DETAIL_FORCE_CAST_HPP
