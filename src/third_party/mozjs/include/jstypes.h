/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
** File:                jstypes.h
** Description: Definitions of NSPR's basic types
**
** Prototypes and macros used to make up for deficiencies in ANSI environments
** that we have found.
**
** Since we do not wrap <stdlib.h> and all the other standard headers, authors
** of portable code will not know in general that they need these definitions.
** Instead of requiring these authors to find the dependent uses in their code
** and take the following steps only in those C files, we take steps once here
** for all C files.
**/

#ifndef jstypes_h
#define jstypes_h

#include "mozilla/Casting.h"
#include "mozilla/Types.h"

#include <stddef.h>
#include <stdint.h>

// jstypes.h is (or should be!) included by every file in SpiderMonkey.
// js-config.h also should be included by every file. So include it here.
#include "js-config.h"

/*
 * The linkage of JS API functions differs depending on whether the file is
 * used within the JS library or not. Any source file within the JS
 * interpreter should define EXPORT_JS_API whereas any client of the library
 * should not. STATIC_JS_API is used to build JS as a static library.
 */
#if defined(STATIC_JS_API)
#  define JS_PUBLIC_API
#  define JS_PUBLIC_DATA
#elif defined(EXPORT_JS_API) || defined(STATIC_EXPORTABLE_JS_API)
#  define JS_PUBLIC_API MOZ_EXPORT
#  define JS_PUBLIC_DATA MOZ_EXPORT
#else
#  define JS_PUBLIC_API MOZ_IMPORT_API
#  define JS_PUBLIC_DATA MOZ_IMPORT_DATA
#endif

/***********************************************************************
** MACROS:      JS_BEGIN_MACRO
**              JS_END_MACRO
** DESCRIPTION:
**      Macro body brackets so that macros with compound statement definitions
**      behave syntactically more like functions when called.
***********************************************************************/
#define JS_BEGIN_MACRO do {
#define JS_END_MACRO \
  }                  \
  while (0)

/***********************************************************************
** FUNCTIONS:   Bit
**              BitMask
** DESCRIPTION:
** Bit masking functions.  XXX n must be <= 31 to be portable
***********************************************************************/
namespace js {
constexpr uint32_t Bit(uint32_t n) { return uint32_t(1) << n; }

constexpr uint32_t BitMask(uint32_t n) { return Bit(n) - 1; }
}  // namespace js

/***********************************************************************
** FUNCTIONS:   HowMany
**              RoundUp
**              RoundDown
**              Round
** DESCRIPTION:
**      Commonly used functions for operations on compatible types.
***********************************************************************/
namespace js {
constexpr size_t HowMany(size_t x, size_t y) { return (x + y - 1) / y; }

constexpr size_t RoundUp(size_t x, size_t y) { return HowMany(x, y) * y; }

constexpr size_t RoundDown(size_t x, size_t y) { return (x / y) * y; }

constexpr size_t Round(size_t x, size_t y) { return ((x + y / 2) / y) * y; }
}  // namespace js

#if (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8) || \
    (defined(UINTPTR_MAX) && UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu)
#  define JS_BITS_PER_WORD 64
#else
#  define JS_BITS_PER_WORD 32
#endif

static_assert(sizeof(void*) == 8 ? JS_BITS_PER_WORD == 64
                                 : JS_BITS_PER_WORD == 32,
              "preprocessor and compiler must agree");

/***********************************************************************
** MACROS:      JS_FUNC_TO_DATA_PTR
**              JS_DATA_TO_FUNC_PTR
** DESCRIPTION:
**      Macros to convert between function and data pointers of the same
**      size. Use them like this:
**
**      JSGetterOp nativeGetter;
**      JSObject* scriptedGetter;
**      ...
**      scriptedGetter = JS_FUNC_TO_DATA_PTR(JSObject*, nativeGetter);
**      ...
**      nativeGetter = JS_DATA_TO_FUNC_PTR(JSGetterOp, scriptedGetter);
**
***********************************************************************/

#define JS_FUNC_TO_DATA_PTR(type, fun) (mozilla::BitwiseCast<type>(fun))
#define JS_DATA_TO_FUNC_PTR(type, ptr) (mozilla::BitwiseCast<type>(ptr))

#endif /* jstypes_h */
