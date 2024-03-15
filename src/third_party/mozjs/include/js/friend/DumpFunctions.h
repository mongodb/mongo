/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Functions to print out values during debugging. */

#ifndef js_friend_DumpFunctions_h
#define js_friend_DumpFunctions_h

#include "mozilla/Attributes.h"
#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf

#include <stddef.h>  // size_t
#include <stdio.h>   // FILE

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Printer.h"  // js::GenericPrinter
#include "js/Utility.h"  // JS::UniqueChars

class JS_PUBLIC_API JSAtom;
struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;
class JS_PUBLIC_API JSScript;
class JS_PUBLIC_API JSString;

namespace JS {

class JS_PUBLIC_API BigInt;
class JS_PUBLIC_API PropertyKey;
class JS_PUBLIC_API Value;

}  // namespace JS

namespace js {

class InterpreterFrame;

}  // namespace js

namespace JS {

/** Exposed for DumpJSStack */
extern JS_PUBLIC_API JS::UniqueChars FormatStackDump(JSContext* cx,
                                                     bool showArgs,
                                                     bool showLocals,
                                                     bool showThisProps);

}  // namespace JS

namespace js {

/*
 * These functions are FRIEND_API to help the debugger find them and to support
 * temporarily hacking js::Dump* calls into other code.  Note that there are
 * overloads that do not require the FILE* parameter, which will default to
 * stderr.
 *
 * These functions are no-ops unless built with DEBUG or JS_JITSPEW.
 */

extern JS_PUBLIC_API void DumpString(JSString* str, FILE* fp);

extern JS_PUBLIC_API void DumpAtom(JSAtom* atom, FILE* fp);

extern JS_PUBLIC_API void DumpObject(JSObject* obj, FILE* fp);

extern JS_PUBLIC_API void DumpChars(const char16_t* s, size_t n, FILE* fp);

// DumpBigInt() outputs the value in decimal if it fits within a 64-bit int, and
// otherwise in hex, prefixed with "0x". In both cases the "n" is appended.
extern JS_PUBLIC_API void DumpBigInt(JS::BigInt* bi, FILE* fp);

extern JS_PUBLIC_API void DumpValue(const JS::Value& val, FILE* fp);

extern JS_PUBLIC_API void DumpId(JS::PropertyKey id, FILE* fp);

extern JS_PUBLIC_API bool DumpPC(JSContext* cx, FILE* fp);

extern JS_PUBLIC_API bool DumpScript(JSContext* cx, JSScript* scriptArg,
                                     FILE* fp);

// Versions for use directly in a debugger (default parameters are not handled
// well in gdb; built-in handles like stderr are not handled well in lldb.)
extern JS_PUBLIC_API void DumpString(JSString* str);
extern JS_PUBLIC_API void DumpAtom(JSAtom* atom);
extern JS_PUBLIC_API void DumpObject(JSObject* obj);
extern JS_PUBLIC_API void DumpChars(const char16_t* s, size_t n);
extern JS_PUBLIC_API void DumpBigInt(JS::BigInt* bi);
extern JS_PUBLIC_API void DumpValue(const JS::Value& val);
extern JS_PUBLIC_API void DumpId(JS::PropertyKey id);
extern JS_PUBLIC_API void DumpInterpreterFrame(
    JSContext* cx, InterpreterFrame* start = nullptr);
extern JS_PUBLIC_API bool DumpPC(JSContext* cx);
extern JS_PUBLIC_API bool DumpScript(JSContext* cx, JSScript* scriptArg);

// DumpBacktrace(), unlike the other dump functions, always dumps a backtrace --
// regardless of DEBUG or JS_JITSPEW.

extern JS_PUBLIC_API void DumpBacktrace(JSContext* cx, FILE* fp);

extern JS_PUBLIC_API void DumpBacktrace(JSContext* cx, GenericPrinter& out);

extern JS_PUBLIC_API void DumpBacktrace(JSContext* cx);

enum DumpHeapNurseryBehaviour {
  CollectNurseryBeforeDump,
  IgnoreNurseryObjects
};

/**
 * Dump the complete object graph of heap-allocated things.
 * fp is the file for the dump output.
 */
extern JS_PUBLIC_API void DumpHeap(
    JSContext* cx, FILE* fp, DumpHeapNurseryBehaviour nurseryBehaviour,
    mozilla::MallocSizeOf mallocSizeOf = nullptr);

extern JS_PUBLIC_API void DumpFmt(FILE* fp, const char* fmt, ...)
    MOZ_FORMAT_PRINTF(2, 3);
extern JS_PUBLIC_API void DumpFmt(const char* fmt, ...) MOZ_FORMAT_PRINTF(1, 2);

}  // namespace js

#endif  // js_friend_DumpFunctions_h
