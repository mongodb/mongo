//  platform_config.hpp  --------------------------------------------------------------------//

//  Copyright 2020 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

#ifndef BOOST_FILESYSTEM_PLATFORM_CONFIG_HPP_
#define BOOST_FILESYSTEM_PLATFORM_CONFIG_HPP_

//  define 64-bit offset macros BEFORE including boost/config.hpp (see ticket #5355)
#if defined(__ANDROID__) && defined(__ANDROID_API__) && __ANDROID_API__ < 24
// Android fully supports 64-bit file offsets only for API 24 and above.
//
// Trying to define _FILE_OFFSET_BITS=64 for APIs below 24
// leads to compilation failure for one or another reason,
// depending on target Android API level, Android NDK version,
// used STL, order of include paths and more.
// For more information, please see:
// - https://github.com/boostorg/filesystem/issues/65
// - https://github.com/boostorg/filesystem/pull/69
//
// Android NDK developers consider it the expected behavior.
// See their official position here:
// - https://github.com/android-ndk/ndk/issues/501#issuecomment-326447479
// - https://android.googlesource.com/platform/bionic/+/a34817457feee026e8702a1d2dffe9e92b51d7d1/docs/32-bit-abi.md#32_bit-abi-bugs
//
// Thus we do not define _FILE_OFFSET_BITS in such case.
#else
// Defining _FILE_OFFSET_BITS=64 should kick in 64-bit off_t's
// (and thus st_size) on 32-bit systems that provide the Large File
// Support (LFS) interface, such as Linux, Solaris, and IRIX.
//
// At the time of this comment writing (March 2018), on most systems
// _FILE_OFFSET_BITS=64 definition is harmless:
// either the definition is supported and enables 64-bit off_t,
// or the definition is not supported and is ignored, in which case
// off_t does not change its default size for the target system
// (which may be 32-bit or 64-bit already).
// Thus it makes sense to have _FILE_OFFSET_BITS=64 defined by default,
// instead of listing every system that supports the definition.
// Those few systems, on which _FILE_OFFSET_BITS=64 is harmful,
// for example this definition causes compilation failure on those systems,
// should be exempt from defining _FILE_OFFSET_BITS by adding
// an appropriate #elif block above with the appropriate comment.
//
// _FILE_OFFSET_BITS must be defined before any headers are included
// to ensure that the definition is available to all included headers.
// That is required at least on Solaris, and possibly on other
// systems as well.
#define _FILE_OFFSET_BITS 64
#endif

#if defined(__APPLE__) || defined(__MACH__)
// Enable newer ABI on Mac OS 10.5 and later, which is needed for struct stat to have birthtime members
#define _DARWIN_USE_64_BIT_INODE 1
#endif

#ifndef _POSIX_PTHREAD_SEMANTICS
#define _POSIX_PTHREAD_SEMANTICS  // Sun readdir_r() needs this
#endif

#if !defined(_INCLUDE_STDCSOURCE_199901) && (defined(hpux) || defined(_hpux) || defined(__hpux))
// For HP-UX, request that WCHAR_MAX and WCHAR_MIN be defined as macros,
// not casts. See ticket 5048
#define _INCLUDE_STDCSOURCE_199901
#endif

#if defined(_WIN32) || defined(_WIN64) || defined(__WIN32__) || defined(__TOS_WIN__) || defined(__WINDOWS__) ||\
    defined(__CYGWIN__)
// Define target Windows version macros before including any other headers
#include <boost/winapi/config.hpp>
#endif

#ifndef BOOST_SYSTEM_NO_DEPRECATED
#define BOOST_SYSTEM_NO_DEPRECATED
#endif

#include <boost/filesystem/config.hpp>

#endif // BOOST_FILESYSTEM_PLATFORM_CONFIG_HPP_
