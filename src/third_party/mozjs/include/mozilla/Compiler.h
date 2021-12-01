/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Various compiler checks. */

#ifndef mozilla_Compiler_h
#define mozilla_Compiler_h

#define MOZ_IS_GCC 0
#define MOZ_IS_MSVC 0

#if !defined(__clang__) && defined(__GNUC__)

#  undef MOZ_IS_GCC
#  define MOZ_IS_GCC 1
   /*
    * These macros should simplify gcc version checking. For example, to check
    * for gcc 4.7.1 or later, check `#if MOZ_GCC_VERSION_AT_LEAST(4, 7, 1)`.
    */
#  define MOZ_GCC_VERSION_AT_LEAST(major, minor, patchlevel)          \
     ((__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__) \
      >= ((major) * 10000 + (minor) * 100 + (patchlevel)))
#  define MOZ_GCC_VERSION_AT_MOST(major, minor, patchlevel)           \
     ((__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__) \
      <= ((major) * 10000 + (minor) * 100 + (patchlevel)))
#  if !MOZ_GCC_VERSION_AT_LEAST(4, 9, 0)
#    error "mfbt (and Gecko) require at least gcc 4.9 to build."
#  endif

#elif defined(_MSC_VER)

#  undef MOZ_IS_MSVC
#  define MOZ_IS_MSVC 1

#endif

/*
 * The situation with standard libraries is a lot worse than with compilers,
 * particularly as clang and gcc could end up using one of three or so standard
 * libraries, and they may not be up-to-snuff with newer C++11 versions. To
 * detect the library, we're going to include cstddef (which is a small header
 * which will be transitively included by everybody else at some point) to grab
 * the version macros and deduce macros from there.
 */
#ifdef __cplusplus
#  include <cstddef>
#  ifdef _STLPORT_MAJOR
#    define MOZ_USING_STLPORT 1
#    define MOZ_STLPORT_VERSION_AT_LEAST(major, minor, patch) \
       (_STLPORT_VERSION >= ((major) << 8 | (minor) << 4 | (patch)))
#  elif defined(_LIBCPP_VERSION)
   /*
    * libc++, unfortunately, doesn't appear to have useful versioning macros.
    * Hopefully, the recommendations of N3694 with respect to standard libraries
    * will get applied instead and we won't need to worry about version numbers
    * here.
    */
#    define MOZ_USING_LIBCXX 1
#  elif defined(__GLIBCXX__)
#    define MOZ_USING_LIBSTDCXX 1
   /*
    * libstdc++ is also annoying and doesn't give us useful versioning macros
    * for the library. If we're using gcc, then assume that libstdc++ matches
    * the compiler version. If we're using clang, we're going to have to fake
    * major/minor combinations by looking for newly-defined config macros.
    */
#    if MOZ_IS_GCC
#      define MOZ_LIBSTDCXX_VERSION_AT_LEAST(major, minor, patch) \
          MOZ_GCC_VERSION_AT_LEAST(major, minor, patch)
#    elif defined(_GLIBCXX_THROW_OR_ABORT)
#      define MOZ_LIBSTDCXX_VERSION_AT_LEAST(major, minor, patch) \
          ((major) < 4 || ((major) == 4 && (minor) <= 8))
#    elif defined(_GLIBCXX_NOEXCEPT)
#      define MOZ_LIBSTDCXX_VERSION_AT_LEAST(major, minor, patch) \
          ((major) < 4 || ((major) == 4 && (minor) <= 7))
#    elif defined(_GLIBCXX_USE_DEPRECATED)
#      define MOZ_LIBSTDCXX_VERSION_AT_LEAST(major, minor, patch) \
          ((major) < 4 || ((major) == 4 && (minor) <= 6))
#    elif defined(_GLIBCXX_PSEUDO_VISIBILITY)
#      define MOZ_LIBSTDCXX_VERSION_AT_LEAST(major, minor, patch) \
          ((major) < 4 || ((major) == 4 && (minor) <= 5))
#    elif defined(_GLIBCXX_BEGIN_EXTERN_C)
#      define MOZ_LIBSTDCXX_VERSION_AT_LEAST(major, minor, patch) \
          ((major) < 4 || ((major) == 4 && (minor) <= 4))
#    elif defined(_GLIBCXX_VISIBILITY_ATTR)
#      define MOZ_LIBSTDCXX_VERSION_AT_LEAST(major, minor, patch) \
          ((major) < 4 || ((major) == 4 && (minor) <= 3))
#    elif defined(_GLIBCXX_VISIBILITY)
#      define MOZ_LIBSTDCXX_VERSION_AT_LEAST(major, minor, patch) \
          ((major) < 4 || ((major) == 4 && (minor) <= 2))
#    else
#      error "Your version of libstdc++ is unknown to us and is likely too old."
#    endif
#  endif

   // Flesh out the defines for everyone else
#  ifndef MOZ_USING_STLPORT
#    define MOZ_USING_STLPORT 0
#    define MOZ_STLPORT_VERSION_AT_LEAST(major, minor, patch) 0
#  endif
#  ifndef MOZ_USING_LIBCXX
#    define MOZ_USING_LIBCXX 0
#  endif
#  ifndef MOZ_USING_LIBSTDCXX
#    define MOZ_USING_LIBSTDCXX 0
#    define MOZ_LIBSTDCXX_VERSION_AT_LEAST(major, minor, patch) 0
#  endif
#endif /* __cplusplus */

#endif /* mozilla_Compiler_h */
