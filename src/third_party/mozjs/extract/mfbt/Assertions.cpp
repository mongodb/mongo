/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Sprintf.h"

#include <stdarg.h>

MOZ_BEGIN_EXTERN_C

/*
 * The crash reason is defined as a global variable here rather than in the
 * crash reporter itself to make it available to all code, even libraries like
 * JS that don't link with the crash reporter directly. This value will only
 * be consumed if the crash reporter is used by the target application.
 */
MFBT_DATA const char* gMozCrashReason = nullptr;

static char sPrintfCrashReason[sPrintfCrashReasonSize] = {};

// Accesses to this atomic are not included in web replay recordings, so that
// if we crash in an area where recorded events are not allowed the true reason
// for the crash is not obscured by a record/replay error.
static mozilla::Atomic<bool, mozilla::SequentiallyConsistent> sCrashing(false);

MFBT_API MOZ_COLD MOZ_NEVER_INLINE MOZ_FORMAT_PRINTF(1, 2) const
    char* MOZ_CrashPrintf(const char* aFormat, ...) {
  if (!sCrashing.compareExchange(false, true)) {
    // In the unlikely event of a race condition, skip
    // setting the crash reason and just crash safely.
    MOZ_RELEASE_ASSERT(false);
  }
  va_list aArgs;
  va_start(aArgs, aFormat);
  int ret = VsprintfLiteral(sPrintfCrashReason, aFormat, aArgs);
  va_end(aArgs);
  MOZ_RELEASE_ASSERT(
      ret >= 0 && size_t(ret) < sPrintfCrashReasonSize,
      "Could not write the explanation string to the supplied buffer!");
  return sPrintfCrashReason;
}

MOZ_END_EXTERN_C

MFBT_API MOZ_NORETURN MOZ_COLD void mozilla::detail::InvalidArrayIndex_CRASH(
    size_t aIndex, size_t aLength) {
  MOZ_CRASH_UNSAFE_PRINTF("ElementAt(aIndex = %zu, aLength = %zu)", aIndex,
                          aLength);
}
