
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_CONFIG_HPP_INCLUDED
#define BOOST_TT_CONFIG_HPP_INCLUDED

#ifndef BOOST_CONFIG_HPP
#include <boost/config.hpp>
#endif

#include <boost/detail/workaround.hpp>

//
// whenever we have a conversion function with elipses
// it needs to be declared __cdecl to suppress compiler
// warnings from MS and Borland compilers (this *must*
// appear before we include is_same.hpp below):
#if defined(BOOST_MSVC) || (defined(__BORLANDC__) && !defined(BOOST_DISABLE_WIN32))
#   define BOOST_TT_DECL __cdecl
#else
#   define BOOST_TT_DECL /**/
#endif

# if (BOOST_WORKAROUND(__MWERKS__, < 0x3000)                         \
    || BOOST_WORKAROUND(BOOST_MSVC, <= 1301)                        \
    || !defined(__EDG_VERSION__) && BOOST_WORKAROUND(__GNUC__, < 3) \
    || BOOST_WORKAROUND(__IBMCPP__, < 600 )                         \
    || BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))       \
    || defined(__ghs)                                               \
    || BOOST_WORKAROUND(__HP_aCC, BOOST_TESTED_AT(53800))           \
    || BOOST_WORKAROUND(MPW_CPLUS, BOOST_TESTED_AT(0x890))          \
    || BOOST_WORKAROUND(__SUNPRO_CC, BOOST_TESTED_AT(0x580)) )      \
    && defined(BOOST_NO_IS_ABSTRACT)

#   define BOOST_TT_NO_CONFORMING_IS_CLASS_IMPLEMENTATION 1

#endif

#ifndef BOOST_TT_NO_CONFORMING_IS_CLASS_IMPLEMENTATION
# define BOOST_TT_HAS_CONFORMING_IS_CLASS_IMPLEMENTATION 1
#endif

//
// Define BOOST_TT_NO_ELLIPSIS_IN_FUNC_TESTING 
// when we can't test for function types with elipsis:
//
#if BOOST_WORKAROUND(__GNUC__, < 3)
#  define BOOST_TT_NO_ELLIPSIS_IN_FUNC_TESTING
#endif

//
// define BOOST_TT_TEST_MS_FUNC_SIGS
// when we want to test __stdcall etc function types with is_function etc
// (Note, does not work with Borland, even though it does support __stdcall etc):
//
#if defined(_MSC_EXTENSIONS) && !defined(__BORLANDC__)
#  define BOOST_TT_TEST_MS_FUNC_SIGS
#endif

//
// define BOOST_TT_NO_CV_FUNC_TEST
// if tests for cv-qualified member functions don't 
// work in is_member_function_pointer
//
#if BOOST_WORKAROUND(__MWERKS__, < 0x3000) || BOOST_WORKAROUND(__IBMCPP__, <= 600)
#  define BOOST_TT_NO_CV_FUNC_TEST
#endif

#endif // BOOST_TT_CONFIG_HPP_INCLUDED


