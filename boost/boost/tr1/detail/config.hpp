//  (C) Copyright John Maddock 2005.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


#ifndef BOOST_TR1_DETAIL_CONFIG_HPP_INCLUDED
#  define BOOST_TR1_DETAIL_CONFIG_HPP_INCLUDED

//
// IMPORTANT: we must figure out the basics, such as how to
// forward to the real std lib headers *without* including
// boost/config.hpp or any of the std lib headers.  A classic 
// chicken and the egg problem....
//
// Including <cstddef> at least lets us detect STLport:
//
#include <cstddef>

#  if (defined(__SGI_STL_PORT) || defined(_STLPORT_VERSION)) && !defined(__BORLANDC__)
#     ifdef __SUNPRO_CC
         // can't use <../stlport/name> since some compilers put stlport in a different directory:
#        define BOOST_TR1_STD_HEADER(name) <../stlport4/name>
#     else
#        define BOOST_TR1_STD_HEADER(name) <../stlport/name>
#     endif
#  elif defined(__HP_aCC)
#     define BOOST_TR1_STD_HEADER(name) <../include_std/name>
#  elif defined(__DECCXX)
#     define BOOST_TR1_STD_HEADER(name) <../cxx/name>
#  elif defined(__BORLANDC__) && __BORLANDC__ >= 0x570
#     define BOOST_TR1_STD_HEADER(name) <../include/dinkumware/name>
#  else
#     define BOOST_TR1_STD_HEADER(name) <../include/name>
#  endif

#if defined(__GNUC__) || (!defined(_AIX) && defined(__IBMCPP__)  && __IBMCPP__ >= 800)
#  ifndef BOOST_HAS_INCLUDE_NEXT
#     define BOOST_HAS_INCLUDE_NEXT
#  endif
#endif

// Can't use BOOST_WORKAROUND here, it leads to recursive includes:
#if (defined(__BORLANDC__) && (__BORLANDC__ <= 0x600)) || (defined(_MSC_VER) && (_MSC_VER < 1310))
#  define BOOST_TR1_USE_OLD_TUPLE
#endif

//
// We may be in the middle of parsing boost/config.hpp
// when this header is included, so don't rely on config
// stuff in the rest of this header...
//
// Find our actual std lib:
//
#ifdef BOOST_HAS_INCLUDE_NEXT
#  ifndef BOOST_TR1_NO_RECURSION
#     define BOOST_TR1_NO_RECURSION
#     define BOOST_TR1_NO_CONFIG_RECURSION
#  endif
#  include_next <utility>
#  if (__GNUC__ < 3)
#     include_next <algorithm>
#     include_next <iterator>
#  endif
#  ifdef BOOST_TR1_NO_CONFIG_RECURSION
#     undef BOOST_TR1_NO_CONFIG_RECURSION
#     undef BOOST_TR1_NO_RECURSION
#  endif
#else
#  include BOOST_TR1_STD_HEADER(utility)
#endif

#if defined(__GLIBCXX__) && !defined(BOOST_TR1_PATH)
#  define BOOST_TR1_PATH(name) tr1/name
#endif
#if !defined(BOOST_TR1_PATH)
#  define BOOST_TR1_PATH(name) name
#endif

#define BOOST_TR1_HEADER(name) <BOOST_TR1_PATH(name)>

#ifdef BOOST_HAS_TR1
   // turn on support for everything:
#  define BOOST_HAS_TR1_ARRAY
#  define BOOST_HAS_TR1_COMPLEX_OVERLOADS
#  define BOOST_HAS_TR1_COMPLEX_INVERSE_TRIG
#  define BOOST_HAS_TR1_REFERENCE_WRAPPER
#  define BOOST_HAS_TR1_RESULT_OF
#  define BOOST_HAS_TR1_MEM_FN
#  define BOOST_HAS_TR1_BIND
#  define BOOST_HAS_TR1_FUNCTION
#  define BOOST_HAS_TR1_HASH
#  define BOOST_HAS_TR1_SHARED_PTR
#  define BOOST_HAS_TR1_RANDOM
#  define BOOST_HAS_TR1_REGEX
#  define BOOST_HAS_TR1_TUPLE
#  define BOOST_HAS_TR1_TYPE_TRAITS
#  define BOOST_HAS_TR1_UTILITY
#  define BOOST_HAS_TR1_UNORDERED_MAP
#  define BOOST_HAS_TR1_UNORDERED_SET

#endif

#if defined(__MWERKS__) && (__MWERKS__ >= 0x3205)
//
// Very preliminary MWCW support, may not be right:
//
#  define BOOST_HAS_TR1_SHARED_PTR
#  define BOOST_HAS_TR1_REFERENCE_WRAPPER
#  define BOOST_HAS_TR1_FUNCTION
#  define BOOST_HAS_TR1_TUPLE
#  define BOOST_HAS_TR1_RESULT_OF
#endif

#ifdef BOOST_HAS_GCC_TR1
   // turn on support for everything in gcc 4.0.x:
#  define BOOST_HAS_TR1_ARRAY
//#  define BOOST_HAS_TR1_COMPLEX_OVERLOADS
//#  define BOOST_HAS_TR1_COMPLEX_INVERSE_TRIG
#  define BOOST_HAS_TR1_REFERENCE_WRAPPER
#  define BOOST_HAS_TR1_RESULT_OF
#  define BOOST_HAS_TR1_MEM_FN
#  define BOOST_HAS_TR1_BIND
#  define BOOST_HAS_TR1_FUNCTION
#  define BOOST_HAS_TR1_HASH
#  define BOOST_HAS_TR1_SHARED_PTR
//#  define BOOST_HAS_TR1_RANDOM
//#  define BOOST_HAS_TR1_REGEX
#  define BOOST_HAS_TR1_TUPLE
#  define BOOST_HAS_TR1_TYPE_TRAITS
#  define BOOST_HAS_TR1_UTILITY
#  define BOOST_HAS_TR1_UNORDERED_MAP
#  define BOOST_HAS_TR1_UNORDERED_SET

#endif

#include <boost/config.hpp>

#endif

