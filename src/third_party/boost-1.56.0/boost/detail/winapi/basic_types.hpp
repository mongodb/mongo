//  basic_types.hpp  --------------------------------------------------------------//

//  Copyright 2010 Vicente J. Botet Escriba

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt


#ifndef BOOST_DETAIL_WINAPI_BASIC_TYPES_HPP
#define BOOST_DETAIL_WINAPI_BASIC_TYPES_HPP

#include <cstdarg>
#include <boost/cstdint.hpp>
#include <boost/detail/winapi/config.hpp>

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
# ifndef NTAPI
#  define NTAPI __stdcall
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
    typedef ::BOOLEAN BOOLEAN_;
    typedef ::PBOOLEAN PBOOLEAN_;
    typedef ::BYTE BYTE_;
    typedef ::WORD WORD_;
    typedef ::DWORD DWORD_;
    typedef ::HANDLE HANDLE_;
    typedef ::HMODULE HMODULE_;
    typedef ::LONG LONG_;
    typedef ::ULONG ULONG_;
    typedef ::LONGLONG LONGLONG_;
    typedef ::ULONGLONG ULONGLONG_;
    typedef ::INT_PTR INT_PTR_;
    typedef ::UINT_PTR UINT_PTR_;
    typedef ::LONG_PTR LONG_PTR_;
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
    typedef unsigned char BYTE_;
    typedef BYTE_ BOOLEAN_;
    typedef BOOLEAN_* PBOOLEAN_;
    typedef unsigned short WORD_;
    typedef unsigned long DWORD_;
    typedef void* HANDLE_;
    typedef void* HMODULE_;

    typedef long LONG_;
    typedef unsigned long ULONG_;

    typedef boost::int64_t LONGLONG_;
    typedef boost::uint64_t ULONGLONG_;

// @FIXME Which condition must be tested
# ifdef _WIN64
#if defined(__CYGWIN__)
    typedef long INT_PTR_;
    typedef unsigned long UINT_PTR_;
    typedef long LONG_PTR_;
    typedef unsigned long ULONG_PTR_;
#else
    typedef __int64 INT_PTR_;
    typedef unsigned __int64 UINT_PTR_;
    typedef __int64 LONG_PTR_;
    typedef unsigned __int64 ULONG_PTR_;
#endif
# else
    typedef int INT_PTR_;
    typedef unsigned int UINT_PTR_;
    typedef long LONG_PTR_;
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

#endif // BOOST_DETAIL_WINAPI_BASIC_TYPES_HPP
