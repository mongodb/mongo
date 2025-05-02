// Copyright (C) 2007, 2008 Steven Watanabe, Joseph Gauterin, Niels Dekker
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// For more information, see http://www.boost.org


#ifndef BOOST_CORE_SWAP_HPP
#define BOOST_CORE_SWAP_HPP

// Note: the implementation of this utility contains various workarounds:
// - boost::swap has two template arguments, instead of one, to
// avoid ambiguity when swapping objects of a Boost type that does
// not have its own boost::swap overload.

#include <boost/core/enable_if.hpp>
#include <boost/config.hpp>
#include <boost/config/header_deprecated.hpp>
#include <boost/core/invoke_swap.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

BOOST_HEADER_DEPRECATED("boost/core/invoke_swap.hpp")

namespace boost
{
  template<class T1, class T2>
  BOOST_GPU_ENABLED
  BOOST_DEPRECATED("This function is deprecated, use boost::core::invoke_swap instead.")
  inline typename enable_if_c< !boost_swap_impl::is_const<T1>::value && !boost_swap_impl::is_const<T2>::value >::type
  swap(T1& left, T2& right)
  {
    boost::core::invoke_swap(left, right);
  }
}

#endif // BOOST_CORE_SWAP_HPP
