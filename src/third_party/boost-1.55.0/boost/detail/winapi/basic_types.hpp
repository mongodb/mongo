//  basic_types.hpp  --------------------------------------------------------------//

//  Copyright 2010 Vicente J. Botet Escriba

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt


#ifndef BOOST_DETAIL_WINAPI_BASIC_TYPES_HPP
#define BOOST_DETAIL_WINAPI_BASIC_TYPES_HPP

#include <boost/config.hpp>
#include <cstdarg>
#include <boost/cstdint.hpp>

#if defined( BOOST_USE_WINDOWS_H )
# include <windows.h>
#elif defined( WIN32 ) || defined( _WIN32 ) || defined( __WIN32__ ) ||  defined(__CYGWIN__)
# include <winerror.h>
// @FIXME Which condition must be tested
# ifdef UNDER_CE
#  ifndef WINAPI
#   ifndef _WIN32_WCE_EMULATION
#    define WINAPI  __cdecl     // Note this doesn't match the desktop definition
#   else
#    define WINAPI  __stdcall
#   endif
#  endif
# else
#  ifndef WINAPI
#    define WINAPI  __stdcall
#  endif
# endif
#else
# error "Win32 functions not available"
#endif

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace detail {
namespace winapi {
#if defined( BOOST_USE_WINDOWS_H )
    typedef ::BOOL BOOL_;
    typedef ::WORD WORD_;
    typedef ::DWORD DWORD_;
    typedef ::HANDLE HANDLE_;
    typedef ::LONG LONG_;
    typedef ::LONGLONG LONGLONG_;
    typedef ::ULONG_PTR ULONG_PTR_;
    typedef ::LARGE_INTEGER LARGE_INTEGER_;
    typedef ::PLARGE_INTEGER PLARGE_INTEGER_;
    typedef ::PVOID PVOID_;
    typedef ::LPVOID LPVOID_;
    typedef ::CHAR CHAR_;
    typedef ::LPSTR LPSTR_;
    typedef ::LPCSTR LPCSTR_;
    typedef ::WCHAR WCHAR_;
    typedef ::LPWSTR LPWSTR_;
    typedef ::LPCWSTR LPCWSTR_;
#else
extern "C" {
    typedef int BOOL_;
    typedef unsigned short WORD_;
    typedef unsigned long DWORD_;
    typedef void* HANDLE_;

    typedef long LONG_;

// @FIXME Which condition must be tested
//~ #if !defined(_M_IX86)
//~ #if defined(BOOST_NO_INT64_T)
    //~ typedef double LONGLONG_;
//~ #else
    //~ typedef __int64 LONGLONG_;
//~ #endif
//~ #else
    //~ typedef double LONGLONG_;
//~ #endif
    typedef boost::int64_t LONGLONG_;

// @FIXME Which condition must be tested
# ifdef _WIN64
#if defined(__CYGWIN__)
    typedef unsigned long ULONG_PTR_;
#else
    typedef unsigned __int64 ULONG_PTR_;
#endif
# else
    typedef unsigned long ULONG_PTR_;
# endif

    typedef struct _LARGE_INTEGER {
        LONGLONG_ QuadPart;
    } LARGE_INTEGER_;
    typedef LARGE_INTEGER_ *PLARGE_INTEGER_;

    typedef void *PVOID_;
    typedef void *LPVOID_;
    typedef const void *LPCVOID_;

    typedef char CHAR_;
    typedef CHAR_ *LPSTR_;
    typedef const CHAR_ *LPCSTR_;

    typedef wchar_t WCHAR_;
    typedef WCHAR_ *LPWSTR_;
    typedef const WCHAR_ *LPCWSTR_;
}
#endif
}
}
}
#endif // BOOST_DETAIL_WINAPI_TIME_HPP
