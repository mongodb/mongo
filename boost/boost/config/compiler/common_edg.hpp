//  (C) Copyright John Maddock 2001 - 2002. 
//  (C) Copyright Jens Maurer 2001. 
//  (C) Copyright David Abrahams 2002. 
//  (C) Copyright Aleksey Gurtovoy 2002. 
//  (C) Copyright Markus Schoepflin 2005.
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version.

//
// Options common to all edg based compilers.
//
// This is included from within the individual compiler mini-configs.

#ifndef  __EDG_VERSION__
#  error This file requires that __EDG_VERSION__ be defined.
#endif

#if (__EDG_VERSION__ <= 238)
#   define BOOST_NO_INTEGRAL_INT64_T
#   define BOOST_NO_SFINAE
#endif

#if (__EDG_VERSION__ <= 240)
#   define BOOST_NO_VOID_RETURNS
#endif

#if (__EDG_VERSION__ <= 241) && !defined(BOOST_NO_ARGUMENT_DEPENDENT_LOOKUP)
#   define BOOST_NO_ARGUMENT_DEPENDENT_LOOKUP
#endif

#if (__EDG_VERSION__ <= 244) && !defined(BOOST_NO_TEMPLATE_TEMPLATES)
#   define BOOST_NO_TEMPLATE_TEMPLATES
#endif 

#if (__EDG_VERSION__ < 300) && !defined(BOOST_NO_IS_ABSTRACT)
#   define BOOST_NO_IS_ABSTRACT
#endif 

#if (__EDG_VERSION__ <= 303) && !defined(BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL)
#   define BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL
#endif 

// See also kai.hpp which checks a Kai-specific symbol for EH
# if !defined(__KCC) && !defined(__EXCEPTIONS)
#     define BOOST_NO_EXCEPTIONS
# endif

# if !defined(__NO_LONG_LONG)
#     define BOOST_HAS_LONG_LONG
# endif

#ifdef c_plusplus
// EDG has "long long" in non-strict mode
// However, some libraries have insufficient "long long" support
// #define BOOST_HAS_LONG_LONG
#endif



