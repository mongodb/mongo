// Copyright (C) 2001-2003
// William E. Kempf
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_THREAD_CONFIG_WEK01032003_HPP
#define BOOST_THREAD_CONFIG_WEK01032003_HPP

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>


#if !defined BOOST_THREAD_VERSION
#define BOOST_THREAD_VERSION 1
#else
#if BOOST_THREAD_VERSION!=1  && BOOST_THREAD_VERSION!=2
#error "BOOST_THREAD_VERSION must be 1 or 2"
#endif
#endif

#if BOOST_WORKAROUND(__BORLANDC__, < 0x600)
#  pragma warn -8008 // Condition always true/false
#  pragma warn -8080 // Identifier declared but never used
#  pragma warn -8057 // Parameter never used
#  pragma warn -8066 // Unreachable code
#endif

#include "platform.hpp"

// provided for backwards compatibility, since this
// macro was used for several releases by mistake.
#if defined(BOOST_THREAD_DYN_DLL)
# define BOOST_THREAD_DYN_LINK
#endif

// compatibility with the rest of Boost's auto-linking code:
#if defined(BOOST_THREAD_DYN_LINK) || defined(BOOST_ALL_DYN_LINK)
# undef  BOOST_THREAD_USE_LIB
# define BOOST_THREAD_USE_DLL
#endif

#if defined(BOOST_THREAD_BUILD_DLL)   //Build dll
#elif defined(BOOST_THREAD_BUILD_LIB) //Build lib
#elif defined(BOOST_THREAD_USE_DLL)   //Use dll
#elif defined(BOOST_THREAD_USE_LIB)   //Use lib
#else //Use default
#   if defined(BOOST_THREAD_PLATFORM_WIN32)
#       if defined(BOOST_MSVC) || defined(BOOST_INTEL_WIN)
            //For compilers supporting auto-tss cleanup
            //with Boost.Threads lib, use Boost.Threads lib
#           define BOOST_THREAD_USE_LIB
#       else
            //For compilers not yet supporting auto-tss cleanup
            //with Boost.Threads lib, use Boost.Threads dll
#           define BOOST_THREAD_USE_DLL
#       endif
#   else
#       define BOOST_THREAD_USE_LIB
#   endif
#endif

#if defined(BOOST_HAS_DECLSPEC)
#   if defined(BOOST_THREAD_BUILD_DLL) //Build dll
#       define BOOST_THREAD_DECL BOOST_SYMBOL_EXPORT
//#       define BOOST_THREAD_DECL __declspec(dllexport)

#   elif defined(BOOST_THREAD_USE_DLL) //Use dll
#       define BOOST_THREAD_DECL BOOST_SYMBOL_IMPORT
//#       define BOOST_THREAD_DECL __declspec(dllimport)
#   else
#       define BOOST_THREAD_DECL
#   endif
#elif (__GNUC__ == 4 && __GNUC_MINOR__ >= 1) || (__GNUC__ > 4)
#  define BOOST_THREAD_DECL BOOST_SYMBOL_VISIBLE

#else
#   define BOOST_THREAD_DECL
#endif // BOOST_HAS_DECLSPEC

//
// Automatically link to the correct build variant where possible.
//
#if !defined(BOOST_ALL_NO_LIB) && !defined(BOOST_THREAD_NO_LIB) && !defined(BOOST_THREAD_BUILD_DLL) && !defined(BOOST_THREAD_BUILD_LIB)
//
// Tell the autolink to link dynamically, this will get undef'ed by auto_link.hpp
// once it's done with it:
//
#if defined(BOOST_THREAD_USE_DLL)
#   define BOOST_DYN_LINK
#endif
//
// Set the name of our library, this will get undef'ed by auto_link.hpp
// once it's done with it:
//
#if defined(BOOST_THREAD_LIB_NAME)
#    define BOOST_LIB_NAME BOOST_THREAD_LIB_NAME
#else
#    define BOOST_LIB_NAME boost_thread
#endif
//
// If we're importing code from a dll, then tell auto_link.hpp about it:
//
// And include the header that does the work:
//
#include <boost/config/auto_link.hpp>
#endif  // auto-linking disabled

#endif // BOOST_THREAD_CONFIG_WEK1032003_HPP

// Change Log:
//   22 Jan 05 Roland Schwarz (speedsnail)
//      Usage of BOOST_HAS_DECLSPEC macro.
//      Default again is static lib usage.
//      BOOST_DYN_LINK only defined when autolink included.
