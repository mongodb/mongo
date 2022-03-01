/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ScopeExit.h"

#include "jsapi.h"

#include "fuzz-tests/tests.h"
#include "irregexp/RegExpAPI.h"
#include "vm/Interpreter.h"
#include "vm/JSAtom.h"
#include "vm/MatchPairs.h"

#include "vm/JSContext-inl.h"

using namespace JS;
using namespace js;

extern JS::PersistentRootedObject gGlobal;
extern JSContext* gCx;

static int testRegExpInit(int* argc, char*** argv) { return 0; }

static int testRegExpFuzz(const uint8_t* buf, size_t size) {
  auto gcGuard = mozilla::MakeScopeExit([&] {
    JS::PrepareForFullGC(gCx);
    JS::NonIncrementalGC(gCx, JS::GCOptions::Normal, JS::GCReason::API);
  });

  const uint32_t HEADER_LEN = 2;
  if (size <= HEADER_LEN) {
    return 0;
  }

  uint8_t rawFlags = buf[0];
  int32_t patternLength = buf[1];

  const uint32_t startIndex = 0;

  RegExpFlags flags(rawFlags & RegExpFlag::AllFlags);

  int32_t inputLength = size - HEADER_LEN - patternLength;

  const char* patternChars = reinterpret_cast<const char*>(buf + HEADER_LEN);

  const char* inputChars;
  if (inputLength < 0) {
    patternLength = size - HEADER_LEN;

    bool useUnicodeInput = (buf[1] & 1) == 0;
    inputChars = useUnicodeInput ? "Привет мир" : "Hello\nworld!";
    inputLength = strlen(inputChars);
  } else {
    inputChars = patternChars + patternLength;
  }

  RootedAtom pattern(gCx, AtomizeUTF8Chars(gCx, patternChars, patternLength));
  if (!pattern) {
    ReportOutOfMemory(gCx);
    return 0;
  }
  RootedAtom input(gCx, AtomizeUTF8Chars(gCx, inputChars, inputLength));
  if (!input) {
    ReportOutOfMemory(gCx);
    return 0;
  }

  VectorMatchPairs interpretedMatches;
  VectorMatchPairs compiledMatches;

  RegExpRunStatus iStatus = irregexp::ExecuteForFuzzing(
      gCx, pattern, input, flags, startIndex, &interpretedMatches,
      RegExpShared::CodeKind::Bytecode);
  if (iStatus == RegExpRunStatus_Error) {
    if (gCx->isThrowingOverRecursed()) {
      return 0;
    }
    gCx->clearPendingException();
  }
  RegExpRunStatus cStatus = irregexp::ExecuteForFuzzing(
      gCx, pattern, input, flags, startIndex, &compiledMatches,
      RegExpShared::CodeKind::Jitcode);
  if (cStatus == RegExpRunStatus_Error) {
    if (gCx->isThrowingOverRecursed()) {
      return 0;
    }
    gCx->clearPendingException();
  }

  // Use release asserts to enable fuzzing on non-debug builds.
  MOZ_RELEASE_ASSERT(iStatus == cStatus);
  if (iStatus == RegExpRunStatus_Success) {
    MOZ_RELEASE_ASSERT(interpretedMatches.pairCount() ==
                       compiledMatches.pairCount());
    for (uint32_t i = 0; i < interpretedMatches.pairCount(); i++) {
      MOZ_RELEASE_ASSERT(
          interpretedMatches[i].start == compiledMatches[i].start &&
          interpretedMatches[i].limit == compiledMatches[i].limit);
    }
  }
  return 0;
}

MOZ_FUZZING_INTERFACE_RAW(testRegExpInit, /* init function */
                          testRegExpFuzz, /* fuzzing function */
                          RegExp          /* module name */
);
