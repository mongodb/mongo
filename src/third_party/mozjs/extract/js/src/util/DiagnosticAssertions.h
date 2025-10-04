/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_DiagnosticAssertions_h
#define util_DiagnosticAssertions_h

#include "mozilla/Assertions.h"
#include "mozilla/Likely.h"

#include "jstypes.h"

/* Crash diagnostics by default in debug and on nightly channel. */
#if defined(DEBUG) || defined(NIGHTLY_BUILD)
#  define JS_CRASH_DIAGNOSTICS 1
#endif

#if defined(JS_DEBUG)
#  define JS_DIAGNOSTICS_ASSERT(expr) MOZ_ASSERT(expr)
#elif defined(JS_CRASH_DIAGNOSTICS)
#  define JS_DIAGNOSTICS_ASSERT(expr)         \
    do {                                      \
      if (MOZ_UNLIKELY(!(expr))) MOZ_CRASH(); \
    } while (0)
#else
#  define JS_DIAGNOSTICS_ASSERT(expr) ((void)0)
#endif

#endif /* util_DiagnosticAssertions_h */
