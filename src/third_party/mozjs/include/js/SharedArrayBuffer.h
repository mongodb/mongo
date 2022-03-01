/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ArrayBuffer functionality. */

#ifndef js_SharedArrayBuffer_h
#define js_SharedArrayBuffer_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {
class JS_PUBLIC_API AutoRequireNoGC;

// CREATION

/**
 * Create a new SharedArrayBuffer with the given byte length.  This
 * may only be called if
 * JS::RealmCreationOptionsRef(cx).getSharedMemoryAndAtomicsEnabled() is
 * true.
 */
extern JS_PUBLIC_API JSObject* NewSharedArrayBuffer(JSContext* cx,
                                                    size_t nbytes);

// TYPE TESTING

/**
 * Check whether obj supports the JS::GetSharedArrayBuffer* APIs.  Note that
 * this may return false if a security wrapper is encountered that denies the
 * unwrapping. If this test succeeds, then it is safe to call the various
 * accessor JSAPI calls defined below.
 */
extern JS_PUBLIC_API bool IsSharedArrayBufferObject(JSObject* obj);

// ACCESSORS

extern JS_PUBLIC_API JSObject* UnwrapSharedArrayBuffer(JSObject* obj);

extern JS_PUBLIC_API size_t GetSharedArrayBufferByteLength(JSObject* obj);

extern JS_PUBLIC_API uint8_t* GetSharedArrayBufferData(JSObject* obj,
                                                       bool* isSharedMemory,
                                                       const AutoRequireNoGC&);

// Ditto for SharedArrayBuffer.
//
// There is an isShared out argument for API consistency (eases use from DOM).
// It will always be set to true.
extern JS_PUBLIC_API void GetSharedArrayBufferLengthAndData(
    JSObject* obj, size_t* length, bool* isSharedMemory, uint8_t** data);

/**
 * Returns true if there are any live SharedArrayBuffer objects, including those
 * for wasm memories, associated with the context.  This is conservative,
 * because it does not run GC.  Some dead objects may not have been collected
 * yet and thus will be thought live.
 */
extern JS_PUBLIC_API bool ContainsSharedArrayBuffer(JSContext* cx);

/**
 * Return the isShared flag of a ArrayBufferView subtypes, which denotes whether
 * the underlying buffer is a SharedArrayBuffer.
 *
 * |obj| must have passed a JS_IsArrayBufferViewObject test, or somehow
 * be known that it would pass such a test: it is a ArrayBufferView subtypes or
 * a wrapper of a ArrayBufferView subtypes, and the unwrapping will succeed.
 */
extern JS_PUBLIC_API bool IsArrayBufferViewShared(JSObject* obj);

}  // namespace JS

#endif /* js_SharedArrayBuffer_h */
