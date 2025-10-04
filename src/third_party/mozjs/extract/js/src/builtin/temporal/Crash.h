/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Crash_h
#define builtin_temporal_Crash_h

#include "mozilla/Assertions.h"
#include "mozilla/Compiler.h"

#include <cstdlib>

// Old GCC don't support calling MOZ_CRASH in a constexpr function. Directly
// abort in this case without calling the crash annotation functions.

#if MOZ_IS_GCC
#  if !MOZ_GCC_VERSION_AT_LEAST(9, 1, 0)
#    define JS_CONSTEXPR_CRASH(...) std::abort()
#  else
#    define JS_CONSTEXPR_CRASH(...) MOZ_CRASH(__VA_ARGS__)
#  endif
#else
#  define JS_CONSTEXPR_CRASH(...) MOZ_CRASH(__VA_ARGS__)
#endif

#endif
