//  (C) Copyright John Maddock 2001-8.
//  (C) Copyright Peter Dimov 2001.
//  (C) Copyright Jens Maurer 2001.
//  (C) Copyright David Abrahams 2002 - 2003.
//  (C) Copyright Aleksey Gurtovoy 2002 - 2003.
//  (C) Copyright Guillaume Melquiond 2002 - 2003.
//  (C) Copyright Beman Dawes 2003.
//  (C) Copyright Martin Wille 2003.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version.

//  Intel compiler setup:

#include "boost/config/compiler/common_edg.hpp"

#if defined(__INTEL_COMPILER)
#  define BOOST_INTEL_CXX_VERSION __INTEL_COMPILER
#elif defined(__ICL)
#  define BOOST_INTEL_CXX_VERSION __ICL
#elif defined(__ICC)
#  define BOOST_INTEL_CXX_VERSION __ICC
#elif defined(__ECC)
#  define BOOST_INTEL_CXX_VERSION __ECC
#endif

// Flags determined by comparing output of 'icpc -dM -E' with and without '-std=c++0x'
#if (!(defined(_WIN32) || defined(_WIN64)) && defined(__STDC_HOSTED__) && (__STDC_HOSTED__ && (BOOST_INTEL_CXX_VERSION <= 1200))) || defined(__GXX_EXPERIMENTAL_CPP0X__)
#  define BOOST_INTEL_STDCXX0X
#endif
#if defined(_MSC_VER) && (_MSC_VER >= 1600)
#  define BOOST_INTEL_STDCXX0X
#endif

#ifdef BOOST_INTEL_STDCXX0X
#define BOOST_COMPILER "Intel C++ C++0x mode version " BOOST_STRINGIZE(BOOST_INTEL_CXX_VERSION)
#else
#define BOOST_COMPILER "Intel C++ version " BOOST_STRINGIZE(BOOST_INTEL_CXX_VERSION)
#endif
#define BOOST_INTEL BOOST_INTEL_CXX_VERSION

#if defined(_WIN32) || defined(_WIN64)
#  define BOOST_INTEL_WIN BOOST_INTEL
#else
#  define BOOST_INTEL_LINUX BOOST_INTEL
#endif

#if (BOOST_INTEL_CXX_VERSION <= 500) && defined(_MSC_VER)
#  define BOOST_NO_EXPLICIT_FUNCTION_TEMPLATE_ARGUMENTS
#  define BOOST_NO_TEMPLATE_TEMPLATES
#endif

#if (BOOST_INTEL_CXX_VERSION <= 600)

#  if defined(_MSC_VER) && (_MSC_VER <= 1300) // added check for <= VC 7 (Peter Dimov)

// Boost libraries assume strong standard conformance unless otherwise
// indicated by a config macro. As configured by Intel, the EDG front-end
// requires certain compiler options be set to achieve that strong conformance.
// Particularly /Qoption,c,--arg_dep_lookup (reported by Kirk Klobe & Thomas Witt)
// and /Zc:wchar_t,forScope. See boost-root/tools/build/intel-win32-tools.jam for
// details as they apply to particular versions of the compiler. When the
// compiler does not predefine a macro indicating if an option has been set,
// this config file simply assumes the option has been set.
// Thus BOOST_NO_ARGUMENT_DEPENDENT_LOOKUP will not be defined, even if
// the compiler option is not enabled.

#     define BOOST_NO_SWPRINTF
#  endif

// Void returns, 64 bit integrals don't work when emulating VC 6 (Peter Dimov)

#  if defined(_MSC_VER) && (_MSC_VER <= 1200)
#     define BOOST_NO_VOID_RETURNS
#     define BOOST_NO_INTEGRAL_INT64_T
#  endif

#endif

#if (BOOST_INTEL_CXX_VERSION <= 710) && defined(_WIN32)
#  define BOOST_NO_POINTER_TO_MEMBER_TEMPLATE_PARAMETERS
#endif

// See http://aspn.activestate.com/ASPN/Mail/Message/boost/1614864
#if BOOST_INTEL_CXX_VERSION < 600
#  define BOOST_NO_INTRINSIC_WCHAR_T
#else
// We should test the macro _WCHAR_T_DEFINED to check if the compiler
// supports wchar_t natively. *BUT* there is a problem here: the standard
// headers define this macro if they typedef wchar_t. Anyway, we're lucky
// because they define it without a value, while Intel C++ defines it
// to 1. So we can check its value to see if the macro was defined natively
// or not.
// Under UNIX, the situation is exactly the same, but the macro _WCHAR_T
// is used instead.
#  if ((_WCHAR_T_DEFINED + 0) == 0) && ((_WCHAR_T + 0) == 0)
#    define BOOST_NO_INTRINSIC_WCHAR_T
#  endif
#endif

#if defined(__GNUC__) && !defined(BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL)
//
// Figure out when Intel is emulating this gcc bug
// (All Intel versions prior to 9.0.26, and versions
// later than that if they are set up to emulate gcc 3.2
// or earlier):
//
#  if ((__GNUC__ == 3) && (__GNUC_MINOR__ <= 2)) || (BOOST_INTEL < 900) || (__INTEL_COMPILER_BUILD_DATE < 20050912)
#     define BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL
#  endif
#endif
#if (defined(__GNUC__) && (__GNUC__ < 4)) || defined(_WIN32) || (BOOST_INTEL_CXX_VERSION <= 1200)
// GCC or VC emulation:
#define BOOST_NO_TWO_PHASE_NAME_LOOKUP
#endif
//
// Verify that we have actually got BOOST_NO_INTRINSIC_WCHAR_T
// set correctly, if we don't do this now, we will get errors later
// in type_traits code among other things, getting this correct
// for the Intel compiler is actually remarkably fragile and tricky:
//
#ifdef __cplusplus
#if defined(BOOST_NO_INTRINSIC_WCHAR_T)
#include <cwchar>
template< typename T > struct assert_no_intrinsic_wchar_t;
template<> struct assert_no_intrinsic_wchar_t<wchar_t> { typedef void type; };
// if you see an error here then you need to unset BOOST_NO_INTRINSIC_WCHAR_T
// where it is defined above:
typedef assert_no_intrinsic_wchar_t<unsigned short>::type assert_no_intrinsic_wchar_t_;
#else
template< typename T > struct assert_intrinsic_wchar_t;
template<> struct assert_intrinsic_wchar_t<wchar_t> {};
// if you see an error here then define BOOST_NO_INTRINSIC_WCHAR_T on the command line:
template<> struct assert_intrinsic_wchar_t<unsigned short> {};
#endif
#endif

#if defined(_MSC_VER) && (_MSC_VER+0 >= 1000)
#  if _MSC_VER >= 1200
#     define BOOST_HAS_MS_INT64
#  endif
#  define BOOST_NO_SWPRINTF
#  define BOOST_NO_TWO_PHASE_NAME_LOOKUP
#elif defined(_WIN32)
#  define BOOST_DISABLE_WIN32
#endif

// I checked version 6.0 build 020312Z, it implements the NRVO.
// Correct this as you find out which version of the compiler
// implemented the NRVO first.  (Daniel Frey)
#if (BOOST_INTEL_CXX_VERSION >= 600)
#  define BOOST_HAS_NRVO
#endif

//
// versions check:
// we don't support Intel prior to version 5.0:
#if BOOST_INTEL_CXX_VERSION < 500
#  error "Compiler not supported or configured - please reconfigure"
#endif

// Intel on MacOS requires
#if defined(__APPLE__) && defined(__INTEL_COMPILER)
#  define BOOST_NO_TWO_PHASE_NAME_LOOKUP
#endif

// Intel on Altix Itanium
#if defined(__itanium__) && defined(__INTEL_COMPILER)
#  define BOOST_NO_TWO_PHASE_NAME_LOOKUP
#endif

//
// An attempt to value-initialize a pointer-to-member may trigger an
// internal error on Intel <= 11.1 (last checked version), as was 
// reported by John Maddock, Intel support issue 589832, May 2010.
// Moreover, according to test results from Huang-Vista-x86_32_intel,
// intel-vc9-win-11.1 may leave a non-POD array uninitialized, in some 
// cases when it should be value-initialized.
// (Niels Dekker, LKEB, May 2010)
// Apparently Intel 12.1 (compiler version number 9999 !!) has the same issue (compiler regression).
#if defined(__INTEL_COMPILER)
#  if (__INTEL_COMPILER <= 1110) || (__INTEL_COMPILER == 9999)
#    define BOOST_NO_COMPLETE_VALUE_INITIALIZATION
#  endif
#endif

//
// Dynamic shared object (DSO) and dynamic-link library (DLL) support
//
#if defined(__GNUC__) && (__GNUC__ >= 4)
#  define BOOST_SYMBOL_EXPORT __attribute__((visibility("default")))
#  define BOOST_SYMBOL_IMPORT
#  define BOOST_SYMBOL_VISIBLE __attribute__((visibility("default")))
#endif
//
// C++0x features
//     - ICC added static_assert in 11.0 (first version with C++0x support)
//
#if defined(BOOST_INTEL_STDCXX0X)
#  undef  BOOST_NO_STATIC_ASSERT
//
// These pass our test cases, but aren't officially supported according to:
// http://software.intel.com/en-us/articles/c0x-features-supported-by-intel-c-compiler/
//
//#  undef  BOOST_NO_LAMBDAS
//#  undef  BOOST_NO_DECLTYPE
//#  undef  BOOST_NO_AUTO_DECLARATIONS
//#  undef  BOOST_NO_AUTO_MULTIDECLARATIONS
#endif

#if defined(BOOST_INTEL_STDCXX0X) && (BOOST_INTEL_CXX_VERSION >= 1200)
//#  undef  BOOST_NO_RVALUE_REFERENCES // Enabling this breaks Filesystem and Exception libraries
//#  undef  BOOST_NO_SCOPED_ENUMS  // doesn't really work!!
#  undef  BOOST_NO_DELETED_FUNCTIONS
#  undef  BOOST_NO_DEFAULTED_FUNCTIONS
#  undef  BOOST_NO_LAMBDAS
#  undef  BOOST_NO_DECLTYPE
#  undef  BOOST_NO_AUTO_DECLARATIONS
#  undef  BOOST_NO_AUTO_MULTIDECLARATIONS
#endif

// icl Version 12.1.0.233 Build 20110811 and possibly some other builds
// had an incorrect __INTEL_COMPILER value of 9999. Intel say this has been fixed. 
#if defined(BOOST_INTEL_STDCXX0X) && (BOOST_INTEL_CXX_VERSION > 1200)
#  undef  BOOST_NO_FUNCTION_TEMPLATE_DEFAULT_ARGS
#  undef  BOOST_NO_NULLPTR
#  undef  BOOST_NO_RVALUE_REFERENCES
#  undef  BOOST_NO_SFINAE_EXPR
#  undef  BOOST_NO_TEMPLATE_ALIASES
#  undef  BOOST_NO_VARIADIC_TEMPLATES

// http://software.intel.com/en-us/articles/c0x-features-supported-by-intel-c-compiler/
// continues to list scoped enum support as "Partial" 
//#  undef  BOOST_NO_SCOPED_ENUMS 
#endif

#if defined(_MSC_VER) && (_MSC_VER <= 1700)
//
// Although the Intel compiler is capable of supporting these, it appears not to in MSVC compatibility mode:
//
#  define  BOOST_NO_INITIALIZER_LISTS
#  define  BOOST_NO_VARIADIC_TEMPLATES
#  define  BOOST_NO_DELETED_FUNCTIONS
#  define  BOOST_NO_DEFAULTED_FUNCTIONS
#  define  BOOST_NO_TEMPLATE_ALIASES
#endif

#if (BOOST_INTEL_CXX_VERSION < 1200)
//
// fenv.h appears not to work with Intel prior to 12.0:
//
#  define BOOST_NO_FENV_H
#endif

//
// last known and checked version:
#if (BOOST_INTEL_CXX_VERSION > 1200)
#  if defined(BOOST_ASSERT_CONFIG)
#     error "Unknown compiler version - please run the configure tests and report the results"
#  elif defined(_MSC_VER)
//
//      We don't emit this warning any more, since we have so few
//      defect macros set anyway (just the one).
//
//#     pragma message("Unknown compiler version - please run the configure tests and report the results")
#  endif
#endif

