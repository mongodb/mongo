//  (C) Copyright John Maddock 2001. 
//  (C) Copyright Jens Maurer 2001 - 2003. 
//  (C) Copyright Peter Dimov 2002. 
//  (C) Copyright Aleksey Gurtovoy 2002 - 2003. 
//  (C) Copyright David Abrahams 2002. 
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version.

//  Sun C++ compiler setup:

#    if __SUNPRO_CC <= 0x500
#      define BOOST_NO_MEMBER_TEMPLATES
#      define BOOST_NO_FUNCTION_TEMPLATE_ORDERING
#    endif

#    if (__SUNPRO_CC <= 0x520)
       //
       // Sunpro 5.2 and earler:
       //
       // although sunpro 5.2 supports the syntax for
       // inline initialization it often gets the value
       // wrong, especially where the value is computed
       // from other constants (J Maddock 6th May 2001)
#      define BOOST_NO_INCLASS_MEMBER_INITIALIZATION

       // Although sunpro 5.2 supports the syntax for
       // partial specialization, it often seems to
       // bind to the wrong specialization.  Better
       // to disable it until suppport becomes more stable
       // (J Maddock 6th May 2001).
#      define BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#    endif

#    if (__SUNPRO_CC <= 0x530) 
       // Requesting debug info (-g) with Boost.Python results
       // in an internal compiler error for "static const"
       // initialized in-class.
       //    >> Assertion:   (../links/dbg_cstabs.cc, line 611)
       //         while processing ../test.cpp at line 0.
       // (Jens Maurer according to Gottfried Ganssauge 04 Mar 2002)
#      define BOOST_NO_INCLASS_MEMBER_INITIALIZATION

       // SunPro 5.3 has better support for partial specialization,
       // but breaks when compiling std::less<shared_ptr<T> >
       // (Jens Maurer 4 Nov 2001).

       // std::less specialization fixed as reported by George
       // Heintzelman; partial specialization re-enabled
       // (Peter Dimov 17 Jan 2002)

//#      define BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

       // integral constant expressions with 64 bit numbers fail
#      define BOOST_NO_INTEGRAL_INT64_T
#    endif

#    if (__SUNPRO_CC < 0x570) 
#      define BOOST_NO_TEMPLATE_TEMPLATES
       // see http://lists.boost.org/MailArchives/boost/msg47184.php
       // and http://lists.boost.org/MailArchives/boost/msg47220.php
#      define BOOST_NO_INCLASS_MEMBER_INITIALIZATION
#      define BOOST_NO_SFINAE
#      define BOOST_NO_ARRAY_TYPE_SPECIALIZATIONS
#    endif
#    if (__SUNPRO_CC <= 0x580) 
#      define BOOST_NO_IS_ABSTRACT
#    endif

#    if (__SUNPRO_CC <= 0x5100)
       // Sun 5.10 may not correctly value-initialize objects of
       // some user defined types, as was reported in April 2010
       // (CR 6947016), and confirmed by Steve Clamage.
       // (Niels Dekker, LKEB, May 2010).
#      define BOOST_NO_COMPLETE_VALUE_INITIALIZATION
#    endif

//
// Dynamic shared object (DSO) and dynamic-link library (DLL) support
//
#if __SUNPRO_CC > 0x500
#  define BOOST_SYMBOL_EXPORT __global
#  define BOOST_SYMBOL_IMPORT __global
#  define BOOST_SYMBOL_VISIBLE __global
#endif



//
// Issues that effect all known versions:
//
#define BOOST_NO_TWO_PHASE_NAME_LOOKUP
#define BOOST_NO_ADL_BARRIER

//
// C++0x features
//
#  define BOOST_HAS_LONG_LONG

#define BOOST_NO_AUTO_DECLARATIONS
#define BOOST_NO_AUTO_MULTIDECLARATIONS
#define BOOST_NO_CHAR16_T
#define BOOST_NO_CHAR32_T
#define BOOST_NO_CONSTEXPR
#define BOOST_NO_DECLTYPE
#define BOOST_NO_DECLTYPE_N3276
#define BOOST_NO_DEFAULTED_FUNCTIONS
#define BOOST_NO_DELETED_FUNCTIONS
#define BOOST_NO_EXPLICIT_CONVERSION_OPERATORS
#define BOOST_NO_EXTERN_TEMPLATE
#define BOOST_NO_FUNCTION_TEMPLATE_DEFAULT_ARGS
#define BOOST_NO_INITIALIZER_LISTS
#define BOOST_NO_LAMBDAS
#define BOOST_NO_NOEXCEPT
#define BOOST_NO_NULLPTR
#define BOOST_NO_RAW_LITERALS
#define BOOST_NO_RVALUE_REFERENCES
#define BOOST_NO_SCOPED_ENUMS
#define BOOST_NO_SFINAE_EXPR
#define BOOST_NO_STATIC_ASSERT
#define BOOST_NO_TEMPLATE_ALIASES
#define BOOST_NO_UNICODE_LITERALS
#define BOOST_NO_VARIADIC_TEMPLATES
#define BOOST_NO_VARIADIC_MACROS
#define BOOST_NO_UNIFIED_INITIALIZATION_SYNTAX

//
// Version
//

#define BOOST_COMPILER "Sun compiler version " BOOST_STRINGIZE(__SUNPRO_CC)

//
// versions check:
// we don't support sunpro prior to version 4:
#if __SUNPRO_CC < 0x400
#error "Compiler not supported or configured - please reconfigure"
#endif
//
// last known and checked version is 0x590:
#if (__SUNPRO_CC > 0x590)
#  if defined(BOOST_ASSERT_CONFIG)
#     error "Unknown compiler version - please run the configure tests and report the results"
#  endif
#endif
