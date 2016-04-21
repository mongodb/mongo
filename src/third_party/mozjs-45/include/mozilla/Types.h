/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* mfbt foundational types and macros. */

#ifndef mozilla_Types_h
#define mozilla_Types_h

/*
 * This header must be valid C and C++, includable by code embedding either
 * SpiderMonkey or Gecko.
 */

/* Expose all <stdint.h> types and size_t. */
#include <stddef.h>
#include <stdint.h>

/* Implement compiler and linker macros needed for APIs. */

/*
 * MOZ_EXPORT is used to declare and define a symbol or type which is externally
 * visible to users of the current library.  It encapsulates various decorations
 * needed to properly export the method's symbol.
 *
 *   api.h:
 *     extern MOZ_EXPORT int MeaningOfLife(void);
 *     extern MOZ_EXPORT int LuggageCombination;
 *
 *   api.c:
 *     int MeaningOfLife(void) { return 42; }
 *     int LuggageCombination = 12345;
 *
 * If you are merely sharing a method across files, just use plain |extern|.
 * These macros are designed for use by library interfaces -- not for normal
 * methods or data used cross-file.
 */
// MONGOD MODIFICATION - SERVER-20311
// Disable all visibility hints since we are statically linking on Windows.
// This prevents mongod.exe and other binaries from exporting these functions.
#if 0 //defined(WIN32)
#  define MOZ_EXPORT   __declspec(dllexport)
#else /* Unix */
#  ifdef HAVE_VISIBILITY_ATTRIBUTE
#    define MOZ_EXPORT       __attribute__((visibility("default")))
#  elif defined(__SUNPRO_C) || defined(__SUNPRO_CC)
#    define MOZ_EXPORT      __global
#  else
#    define MOZ_EXPORT /* nothing */
#  endif
#endif


/*
 * Whereas implementers use MOZ_EXPORT to declare and define library symbols,
 * users use MOZ_IMPORT_API and MOZ_IMPORT_DATA to access them.  Most often the
 * implementer of the library will expose an API macro which expands to either
 * the export or import version of the macro, depending upon the compilation
 * mode.
 */
#ifdef _WIN32
#  if defined(__MWERKS__)
#    define MOZ_IMPORT_API /* nothing */
#  else
#    define MOZ_IMPORT_API __declspec(dllimport)
#  endif
#else
#  define MOZ_IMPORT_API MOZ_EXPORT
#endif

#if defined(_WIN32) && !defined(__MWERKS__)
#  define MOZ_IMPORT_DATA  __declspec(dllimport)
#else
#  define MOZ_IMPORT_DATA  MOZ_EXPORT
#endif

/*
 * Consistent with the above comment, the MFBT_API and MFBT_DATA macros expose
 * export mfbt declarations when building mfbt, and they expose import mfbt
 * declarations when using mfbt.
 */
#if defined(IMPL_MFBT)
#  define MFBT_API     MOZ_EXPORT
#  define MFBT_DATA    MOZ_EXPORT
#else
  /*
   * On linux mozglue is linked in the program and we link libxul.so with
   * -z,defs. Normally that causes the linker to reject undefined references in
   * libxul.so, but as a loophole it allows undefined references to weak
   * symbols. We add the weak attribute to the import version of the MFBT API
   * macros to exploit this.
   */
#  if defined(MOZ_GLUE_IN_PROGRAM) && !defined(MOZILLA_XPCOMRT_API)
#    define MFBT_API   __attribute__((weak)) MOZ_IMPORT_API
#    define MFBT_DATA  __attribute__((weak)) MOZ_IMPORT_DATA
#  else
#    define MFBT_API   MOZ_IMPORT_API
#    define MFBT_DATA  MOZ_IMPORT_DATA
#  endif
#endif

/*
 * C symbols in C++ code must be declared immediately within |extern "C"|
 * blocks.  However, in C code, they need not be declared specially.  This
 * difference is abstracted behind the MOZ_BEGIN_EXTERN_C and MOZ_END_EXTERN_C
 * macros, so that the user need not know whether he is being used in C or C++
 * code.
 *
 *   MOZ_BEGIN_EXTERN_C
 *
 *   extern MOZ_EXPORT int MostRandomNumber(void);
 *   ...other declarations...
 *
 *   MOZ_END_EXTERN_C
 *
 * This said, it is preferable to just use |extern "C"| in C++ header files for
 * its greater clarity.
 */
#ifdef __cplusplus
#  define MOZ_BEGIN_EXTERN_C    extern "C" {
#  define MOZ_END_EXTERN_C      }
#else
#  define MOZ_BEGIN_EXTERN_C
#  define MOZ_END_EXTERN_C
#endif

/*
 * GCC's typeof is available when decltype is not.
 */
#if defined(__GNUC__) && defined(__cplusplus) && \
  !defined(__GXX_EXPERIMENTAL_CXX0X__) && __cplusplus < 201103L
#  define decltype __typeof__
#endif

#endif /* mozilla_Types_h */
