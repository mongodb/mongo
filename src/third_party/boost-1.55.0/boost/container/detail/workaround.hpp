//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_WORKAROUND_HPP
#define BOOST_CONTAINER_DETAIL_WORKAROUND_HPP

#include <boost/container/detail/config_begin.hpp>

#if    !defined(BOOST_NO_CXX11_RVALUE_REFERENCES) && !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)\
    && !defined(BOOST_INTERPROCESS_DISABLE_VARIADIC_TMPL)
   #define BOOST_CONTAINER_PERFECT_FORWARDING
#endif

#if defined(BOOST_NO_CXX11_NOEXCEPT)
   #if defined(BOOST_MSVC)
      #define BOOST_CONTAINER_NOEXCEPT throw()
   #else
      #define BOOST_CONTAINER_NOEXCEPT
   #endif
   #define BOOST_CONTAINER_NOEXCEPT_IF(x)
#else
   #define BOOST_CONTAINER_NOEXCEPT    noexcept
   #define BOOST_CONTAINER_NOEXCEPT_IF(x) noexcept(x)
#endif

#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) && defined(__GXX_EXPERIMENTAL_CXX0X__)\
    && (__GNUC__*10000 + __GNUC_MINOR__*100 + __GNUC_PATCHLEVEL__ < 40700)
   #define BOOST_CONTAINER_UNIMPLEMENTED_PACK_EXPANSION_TO_FIXED_LIST
#endif

#if !defined(BOOST_FALLTHOUGH)
   #define BOOST_CONTAINER_FALLTHOUGH
#else
   #define BOOST_CONTAINER_FALLTHOUGH BOOST_FALLTHOUGH;
#endif

//Macros for documentation purposes. For code, expands to the argument
#define BOOST_CONTAINER_IMPDEF(TYPE) TYPE
#define BOOST_CONTAINER_SEEDOC(TYPE) TYPE

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_DETAIL_WORKAROUND_HPP
