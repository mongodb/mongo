/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* This is the interface that the regexp engine exposes to SpiderMonkey. */

#ifndef regexp_RegExpAPI_h
#define regexp_RegExpAPI_h

#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Range.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

#include "irregexp/RegExpTypes.h"
#include "js/ColumnNumber.h"  // JS::ColumnNumberOneOigin
#include "js/Stack.h"         // JS::NativeStackLimit
#include "vm/RegExpShared.h"

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSTracer;

namespace JS {
class RegExpFlags;
}

namespace v8::internal {
class RegExpStack;
}

namespace js {

class VectorMatchPairs;
class LifoAlloc;

namespace frontend {
class TokenStreamAnyChars;
}

namespace irregexp {

Isolate* CreateIsolate(JSContext* cx);
void TraceIsolate(JSTracer* trc, Isolate* isolate);
void DestroyIsolate(Isolate* isolate);

size_t IsolateSizeOfIncludingThis(Isolate* isolate,
                                  mozilla::MallocSizeOf mallocSizeOf);

bool CheckPatternSyntax(
    js::LifoAlloc& alloc, JS::NativeStackLimit stackLimit,
    frontend::TokenStreamAnyChars& ts,
    const mozilla::Range<const char16_t> chars, JS::RegExpFlags flags,
    mozilla::Maybe<uint32_t> line = mozilla::Nothing(),
    mozilla::Maybe<JS::ColumnNumberOneOrigin> column = mozilla::Nothing());
bool CheckPatternSyntax(JSContext* cx, JS::NativeStackLimit stackLimit,
                        frontend::TokenStreamAnyChars& ts,
                        Handle<JSAtom*> pattern, JS::RegExpFlags flags);

bool CompilePattern(JSContext* cx, MutableHandleRegExpShared re,
                    Handle<JSLinearString*> input,
                    RegExpShared::CodeKind codeKind);

RegExpRunStatus Execute(JSContext* cx, MutableHandleRegExpShared re,
                        Handle<JSLinearString*> input, size_t start,
                        VectorMatchPairs* matches);

RegExpRunStatus ExecuteForFuzzing(JSContext* cx, Handle<JSAtom*> pattern,
                                  Handle<JSLinearString*> input,
                                  JS::RegExpFlags flags, size_t startIndex,
                                  VectorMatchPairs* matches,
                                  RegExpShared::CodeKind codeKind);

bool GrowBacktrackStack(v8::internal::RegExpStack* regexp_stack);

uint32_t CaseInsensitiveCompareNonUnicode(const char16_t* substring1,
                                          const char16_t* substring2,
                                          size_t byteLength);
uint32_t CaseInsensitiveCompareUnicode(const char16_t* substring1,
                                       const char16_t* substring2,
                                       size_t byteLength);
bool IsCharacterInRangeArray(uint32_t c, ByteArrayData* ranges);

#ifdef DEBUG
bool IsolateShouldSimulateInterrupt(Isolate* isolate);
void IsolateSetShouldSimulateInterrupt(Isolate* isolate);
void IsolateClearShouldSimulateInterrupt(Isolate* isolate);
#endif
}  // namespace irregexp
}  // namespace js

#endif /* regexp_RegExpAPI_h */
