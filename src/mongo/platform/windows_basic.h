// windows_basic.h

/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#if !defined(_WIN32)
#error "windows_basic included but _WIN32 is not defined"
#endif

// Ensure that _WIN32_WINNT is set to something before we include windows.h. For server builds
// both _WIN32_WINNT and NTDDI_VERSION are set as defines on the command line, but we need
// these here for things like client driver builds, where they may not already be set.
#if !defined(_WIN32_WINNT)
// Can't use symbolic versions here, since we may not have seen sdkddkver.h yet.
#if defined(_WIN64)
// 64-bit builds default to Windows Server 2003 support.
#define _WIN32_WINNT 0x0502
#else
// 32-bit builds default to Windows XP support.
#define _WIN32_WINNT 0x0501
#endif
#endif

// As above, but for NTDDI_VERSION. Otherwise, <windows.h> would set our NTDDI_VERSION based on
// _WIN32_WINNT, but not select the service pack revision.
#if !defined(NTDDI_VERSION)
// Can't use symbolic versions here, since we may not have seen sdkddkver.h yet.
#if defined(_WIN64)
// 64-bit builds default to Windows Server 2003 SP 2 support.
#define NTDDI_VERSION 0x05020200
#else
// 32-bit builds default to Windows XP SP 3 support.
#define NTDDI_VERSION 0x05010300
#endif
#endif

// No need to set WINVER, SdkDdkVer.h does that for us, we double check this below.

// for rand_s() usage:
# define _CRT_RAND_S
# ifndef NOMINMAX
#  define NOMINMAX
# endif

// Do not complain that about standard library functions that Windows believes should have
// underscores in front of them, such as unlink().
#define _CRT_NONSTDC_NO_DEPRECATE

// tell windows.h not to include a bunch of headers we don't need:
# define WIN32_LEAN_AND_MEAN

# include <winsock2.h> //this must be included before the first windows.h include
# include <ws2tcpip.h>
# include <wspiapi.h>
# include <windows.h>

// Should come either from the command line, or if not set there, the inclusion of sdkddkver.h
// via windows.h above should set it based in _WIN32_WINNT, which is assuredly set by now.
#if !defined(NTDDI_VERSION)
#error "NTDDI_VERSION is not defined"
#endif

#if !defined(WINVER) || (WINVER != _WIN32_WINNT)
#error "Expected WINVER to have been defined and to equal _WIN32_WINNT"
#endif

#if defined(_WIN64)
#if !defined(NTDDI_WS03SP2) || (NTDDI_VERSION < NTDDI_WS03SP2)
#error "64 bit mongo does not support Windows versions older than Windows Server 2003 SP 2"
#endif
#else
#if !defined(NTDDI_WINXPSP3) || (NTDDI_VERSION < NTDDI_WINXPSP3)
#error "32 bit mongo does not support Windows versions older than XP Service Pack 3"
#endif
#endif
