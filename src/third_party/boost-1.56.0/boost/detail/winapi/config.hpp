//  config.hpp  --------------------------------------------------------------//

//  Copyright 2013 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt


#ifndef BOOST_DETAIL_WINAPI_CONFIG_HPP_INCLUDED_
#define BOOST_DETAIL_WINAPI_CONFIG_HPP_INCLUDED_

#include <boost/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

// These constants reflect _WIN32_WINNT_* macros from sdkddkver.h
// See also: http://msdn.microsoft.com/en-us/library/windows/desktop/aa383745%28v=vs.85%29.aspx#setting_winver_or__win32_winnt
#define BOOST_WINAPI_VERSION_NT4 0x0400
#define BOOST_WINAPI_VERSION_WIN2K 0x0500
#define BOOST_WINAPI_VERSION_WINXP 0x0501
#define BOOST_WINAPI_VERSION_WS03 0x0502
#define BOOST_WINAPI_VERSION_WIN6 0x0600
#define BOOST_WINAPI_VERSION_VISTA 0x0600
#define BOOST_WINAPI_VERSION_WS08 0x0600
#define BOOST_WINAPI_VERSION_LONGHORN 0x0600
#define BOOST_WINAPI_VERSION_WIN7 0x0601
#define BOOST_WINAPI_VERSION_WIN8 0x0602
#define BOOST_WINAPI_VERSION_WINBLUE 0x0603

#if !defined(BOOST_USE_WINAPI_VERSION)
#if defined(_WIN32_WINNT)
#define BOOST_USE_WINAPI_VERSION _WIN32_WINNT
#elif defined(WINVER)
#define BOOST_USE_WINAPI_VERSION WINVER
#else
// By default use Windows XP API
#define BOOST_USE_WINAPI_VERSION BOOST_WINAPI_VERSION_WINXP
#endif
#endif

#if defined(BOOST_USE_WINDOWS_H)
// We have to define the version macros so that windows.h provides the necessary symbols
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT BOOST_USE_WINAPI_VERSION
#endif
#if !defined(WINVER)
#define WINVER BOOST_USE_WINAPI_VERSION
#endif
#endif

#endif // BOOST_DETAIL_WINAPI_CONFIG_HPP_INCLUDED_
