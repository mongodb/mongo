//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_WORKAROUND_HPP
#define BOOST_CONTAINER_DETAIL_WORKAROUND_HPP

#include <boost/container/detail/config_begin.hpp>

#if    !defined(BOOST_NO_RVALUE_REFERENCES) && !defined(BOOST_NO_VARIADIC_TEMPLATES)\
    && !defined(BOOST_INTERPROCESS_DISABLE_VARIADIC_TMPL)
   #define BOOST_CONTAINER_PERFECT_FORWARDING
#endif

#if defined(BOOST_NO_NOEXCEPT)
   #define BOOST_CONTAINER_NOEXCEPT
   #define BOOST_CONTAINER_NOEXCEPT_IF(x)
#else
   #define BOOST_CONTAINER_NOEXCEPT    noexcept
   #define BOOST_CONTAINER_NOEXCEPT_IF(x) noexcept(x)
#endif

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_DETAIL_WORKAROUND_HPP
