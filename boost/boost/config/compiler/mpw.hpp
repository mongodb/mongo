//  (C) Copyright John Maddock 2001 - 2002. 
//  (C) Copyright Aleksey Gurtovoy 2002. 
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version.

//  MPW C++ compilers setup:

#   if    defined(__SC__)
#     define BOOST_COMPILER "MPW SCpp version " BOOST_STRINGIZE(__SC__)
#   elif defined(__MRC__)
#     define BOOST_COMPILER "MPW MrCpp version " BOOST_STRINGIZE(__MRC__)
#   else
#     error "Using MPW compiler configuration by mistake.  Please update."
#   endif

//
// MPW 8.90:
//
#if (MPW_CPLUS <= 0x890) || !defined(BOOST_STRICT_CONFIG)
#  define BOOST_NO_CV_SPECIALIZATIONS
#  define BOOST_NO_DEPENDENT_NESTED_DERIVATIONS
#  define BOOST_NO_DEPENDENT_TYPES_IN_TEMPLATE_VALUE_PARAMETERS
#  define BOOST_NO_INCLASS_MEMBER_INITIALIZATION
#  define BOOST_NO_INTRINSIC_WCHAR_T
#  define BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#  define BOOST_NO_USING_TEMPLATE

#  define BOOST_NO_CWCHAR
#  define BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS

#  define BOOST_NO_STD_ALLOCATOR /* actually a bug with const reference overloading */
#endif

//
// versions check:
// we don't support MPW prior to version 8.9:
#if MPW_CPLUS < 0x890
#  error "Compiler not supported or configured - please reconfigure"
#endif
//
// last known and checked version is 0x890:
#if (MPW_CPLUS > 0x890)
#  if defined(BOOST_ASSERT_CONFIG)
#     error "Unknown compiler version - please run the configure tests and report the results"
#  endif
#endif


