//  (C) Copyright John Maddock 2001 - 2003. 
//  (C) Copyright Bill Kempf 2001. 
//  (C) Copyright Aleksey Gurtovoy 2003. 
//  (C) Copyright Rene Rivera 2005.
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version.

//  Win32 specific config options:

#define BOOST_PLATFORM "Win32"

//  Get the information about the MinGW runtime, i.e. __MINGW32_*VERSION.
#if defined(__MINGW32__)
#  include <_mingw.h>
#endif

#if defined(__GNUC__) && !defined(BOOST_NO_SWPRINTF)
#  define BOOST_NO_SWPRINTF
#endif

#if !defined(__GNUC__) && !defined(BOOST_HAS_DECLSPEC)
#  define BOOST_HAS_DECLSPEC
#endif

#if defined(__MINGW32__) && ((__MINGW32_MAJOR_VERSION > 2) || ((__MINGW32_MAJOR_VERSION == 2) && (__MINGW32_MINOR_VERSION >= 0)))
#  define BOOST_HAS_STDINT_H
#  define __STDC_LIMIT_MACROS
#  define BOOST_HAS_DIRENT_H
#  define BOOST_HAS_UNISTD_H
#endif

//
// Win32 will normally be using native Win32 threads,
// but there is a pthread library avaliable as an option,
// we used to disable this when BOOST_DISABLE_WIN32 was 
// defined but no longer - this should allow some
// files to be compiled in strict mode - while maintaining
// a consistent setting of BOOST_HAS_THREADS across
// all translation units (needed for shared_ptr etc).
//

#ifdef _WIN32_WCE
#  define BOOST_NO_ANSI_APIS
#endif

#ifndef BOOST_HAS_PTHREADS
#  define BOOST_HAS_WINTHREADS
#endif

#ifndef BOOST_DISABLE_WIN32
// WEK: Added
#define BOOST_HAS_FTIME
#define BOOST_WINDOWS 1

#endif
