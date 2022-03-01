/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript API. */

#ifndef js_Context_h
#define js_Context_h

#include "jspubtd.h"
// [SMDOC] Nested Thread Data Structures (JSContext, JSRuntime)
//
// Spidermonkey has two nested data structures for representing threads,
// JSContext and JSRuntime. All JS threads are represented by a context.
// Contexts can contain runtimes. A runtime however is not present for
// all threads. Threads also interact with the GC. See "Nested GC
// DataStructures" for more info.
//
// Context
// -------
// JSContext represents a thread: there must be exactly one JSContext for each
// thread running JS/Wasm.
//
// Internally, helper threads can also have a JSContext. They do not always have
// an active context, but one may be requested by AutoSetHelperThreadContext,
// which activates a pre-allocated JSContext for the duration of its lifetime.
//
// Runtime
// -------
// JSRuntime is very similar to JSContext: each runtime belongs to one context
// (thread), but helper threads don't have their own runtimes (they're shared by
// all runtimes in the process and use the runtime of the task they're
// executing).
//
// Note:
// Locking, contexts, and memory allocation.
//
// It is important that SpiderMonkey be initialized, and the first context
// be created, in a single-threaded fashion.  Otherwise the behavior of the
// library is undefined.
// See:
// https://developer.mozilla.org/en-US/docs/Mozilla/Projects/SpiderMonkey/JSAPI_reference

// Create a new context (and runtime) for this thread.
extern JS_PUBLIC_API JSContext* JS_NewContext(
    uint32_t maxbytes, JSRuntime* parentRuntime = nullptr);

// Destroy a context allocated with JS_NewContext. Must be called on the thread
// that called JS_NewContext.
extern JS_PUBLIC_API void JS_DestroyContext(JSContext* cx);

JS_PUBLIC_API void* JS_GetContextPrivate(JSContext* cx);

JS_PUBLIC_API void JS_SetContextPrivate(JSContext* cx, void* data);

extern JS_PUBLIC_API JSRuntime* JS_GetParentRuntime(JSContext* cx);

extern JS_PUBLIC_API JSRuntime* JS_GetRuntime(JSContext* cx);

extern JS_PUBLIC_API void JS_SetFutexCanWait(JSContext* cx);

namespace js {

void AssertHeapIsIdle();

} /* namespace js */

namespace JS {

/**
 * Asserts (in debug and release builds) that `obj` belongs to the current
 * thread's context.
 */
JS_PUBLIC_API void AssertObjectBelongsToCurrentThread(JSObject* obj);

/**
 * Install a process-wide callback to validate script filenames. The JS engine
 * will invoke this callback for each JS script it parses or XDR decodes.
 *
 * If the callback returns |false|, an exception is thrown and parsing/decoding
 * will be aborted.
 *
 * See also CompileOptions::setSkipFilenameValidation to opt-out of the callback
 * for specific parse jobs.
 */
using FilenameValidationCallback = bool (*)(const char* filename,
                                            bool isSystemRealm);
JS_PUBLIC_API void SetFilenameValidationCallback(FilenameValidationCallback cb);

} /* namespace JS */

#endif  // js_Context_h
