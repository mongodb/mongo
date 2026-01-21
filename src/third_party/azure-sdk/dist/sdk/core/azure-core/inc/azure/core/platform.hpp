// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Platform-specific macros.
 */

// The proper way to check for "UWP/NOT UWP" (*):
// 1. Include this file, so that AZ_PLATFORM_WINDOWS becomes available.
// 2. Include windows.h (**)
// 3. Use "#if !defined(WINAPI_PARTITION_DESKTOP) || WINAPI_PARTITION_DESKTOP" (***) as a condition
// meant to detect as fully-featured Win32 API (i.e. registry access, FileOpen() etc.) = "NOT UWP"
// (4.) "#else" you are on "UWP" (*)
// 5. Some features (i.e. WinHTTP) may have support for some non-traditional-Win32 desktops, yet not
// the entirety of "UWP"(*). For example, WinHTTP would currently compile and work on the following:
// WINAPI_PARTITION_DESKTOP, WINAPI_PARTITION_SYSTEM, WINAPI_PARTITION_GAMES, so more complex
// conditions are possible (****).
//
// --
// (*) - "UWP" is oversimplification, we use it in this comment as an umbrella term to represent all
// the Windows platforms (UWP itself, Phone OS, Windows Store, etc) that do not allow some Win32
// APIs such as registry access.
// (**) - Including windows.h brings up WINAPI_PARTITION_DESKTOP macro (***). The reason we don't
// simply include windows.h in this header and declare some sort of a "AZ_PLATFORM_WINDOWS_UWP"
// macro in this file is that some places do need to include windows.h when WIN32_LEAN_AND_MEAN is
// defined, others do not. So we defer this to each Azure SDK .cpp to do the inclusion as it best
// fits. Plus, theoretically, you may want to check for AZ_PLATFORM_WINDOWS, yet you don't need
// anything from windows.h header.
// (***) - What is happening here: "WINAPI_PARTITION_DESKTOP" may not be defined on some old Windows
// SDKs that existed before a concept of "UWP" came out. Those are your traditional "Full Win32 API"
// platforms (Windows 7?). But IF "WINAPI_PARTITION_DESKTOP" is defined, then it can be
// either 0 or 1 (simply put). For example, if we are being compiled for the "traditional" Windows
// 10 desktop, it has a "Full WIn32 API", it has WINAPI_PARTITION_DESKTOP defined, and
// WINAPI_PARTITION_DESKTOP evaluates to 1. Otherwise, it is a UWP, which also has
// WINAPI_PARTITION_DESKTOP defined, but it evaluates as 0.
// (****) - vcpkg could be limiting the default option, because at the moment it only distinguishes
// between "UWP" and "Not UWP". So, if we have a default option indicating whether to build WinHTTP,
// the best we can do is to enable build by default on (windows&!uwp). We can't remove the "!uwp"
// part, because on some partitions compilation will fail. However, there is always an option for
// the customer to run "vcpkg install azure-core-cpp[winhttp]" manually, and the build attempt will
// be made (even if targeting macOS or Linux).

#pragma once

#if defined(_WIN32)
#define AZ_PLATFORM_WINDOWS
#elif defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__))
#define AZ_PLATFORM_POSIX
#if defined(__APPLE__) || defined(__MACH__)
#define AZ_PLATFORM_MAC
#else
#define AZ_PLATFORM_LINUX
#endif

#endif
