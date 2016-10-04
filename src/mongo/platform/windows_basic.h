// windows_basic.h

/*
 *    Copyright 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#if !defined(_WIN32)
#error "windows_basic included but _WIN32 is not defined"
#endif

// "If you define NTDDI_VERSION, you must also define _WIN32_WINNT":
// http://msdn.microsoft.com/en-us/library/windows/desktop/aa383745(v=vs.85).aspx
#if defined(NTDDI_VERSION) && !defined(_WIN32_WINNT)
#error NTDDI_VERSION defined but _WIN32_WINNT is undefined
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
#define _CRT_RAND_S
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Do not complain that about standard library functions that Windows believes should have
// underscores in front of them, such as unlink().
#define _CRT_NONSTDC_NO_DEPRECATE

// tell windows.h not to include a bunch of headers we don't need:
#define WIN32_LEAN_AND_MEAN

// Tell windows.h not to define any NT status codes, so that we can
// get the definitions from ntstatus.h, which has a more complete list.
#define WIN32_NO_STATUS

#include <windows.h>
#include <winsock2.h>  //this must be included before the first windows.h include
#include <ws2tcpip.h>

#undef WIN32_NO_STATUS

// Obtain a definition for the ntstatus type.
#include <winternl.h>

// Add back in the status definitions so that macro expansions for
// things like STILL_ACTIVE and WAIT_OBJECT_O can be resolved (they
// expand to STATUS_ codes).
#include <ntstatus.h>

// Should come either from the command line, or if not set there, the inclusion of sdkddkver.h
// via windows.h above should set it based in _WIN32_WINNT, which is assuredly set by now.
#if !defined(NTDDI_VERSION)
#error "NTDDI_VERSION is not defined"
#endif

#if !defined(WINVER) || (WINVER != _WIN32_WINNT)
#error "Expected WINVER to have been defined and to equal _WIN32_WINNT"
#endif

#if !defined(NTDDI_WINBLUE)
#error "MongoDB requires Windows SDK 8.1 or higher to build"
#endif

#if !defined(NTDDI_VISTA) || NTDDI_VERSION < NTDDI_VISTA
#error "MongoDB does not support Windows versions older than Windows Vista"
#endif
