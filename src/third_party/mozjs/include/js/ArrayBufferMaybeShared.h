/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Functions for working with either ArrayBuffer or SharedArrayBuffer objects
 * in agnostic fashion.
 */

#ifndef js_ArrayBufferMaybeShared_h
#define js_ArrayBufferMaybeShared_h

#include <stdint.h>  // uint8_t, uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {

class JS_PUBLIC_API AutoRequireNoGC;

// TYPE TESTING

/**
 * Check whether obj supports the JS::GetArrayBufferMaybeShared* APIs.  Note
 * that this may return false if a security wrapper is encountered that denies
 * the unwrapping. If this test succeeds, then it is safe to call the various
 * predicate and accessor JSAPI calls defined below.
 */
extern JS_PUBLIC_API bool IsArrayBufferObjectMaybeShared(JSObject* obj);

// ACCESSORS

/*
 * Test for ArrayBufferMaybeShared subtypes and return the unwrapped object if
 * so, else nullptr. Never throws.
 */
extern JS_PUBLIC_API JSObject* UnwrapArrayBufferMaybeShared(JSObject* obj);

/**
 * Get the length, sharedness, and data from an ArrayBufferMaybeShared subtypes.
 *
 * The computed length and data pointer may be invalidated by a GC or by an
 * unshared array buffer becoming detached. Callers must take care not to
 * perform any actions that could trigger a GC or result in an unshared array
 * buffer becoming detached. If such actions nonetheless must be performed,
 * callers should perform this call a second time (and sensibly handle results
 * that may be different from those returned the first time). (Sharedness is an
 * immutable characteristic of an array buffer or shared array buffer, so that
 * boolean remains valid across GC or detaching.)
 *
 * |obj| must be an ArrayBufferMaybeShared subtype: an ArrayBuffer or a
 * SharedArrayBuffer.
 *
 * |*length| will be set to bytes in the buffer.
 *
 * |*isSharedMemory| will be set to true if it is a SharedArrayBuffer, otherwise
 * to false.
 *
 * |*data| will be set to a pointer to the bytes in the buffer.
 */
extern JS_PUBLIC_API void GetArrayBufferMaybeSharedLengthAndData(
    JSObject* obj, size_t* length, bool* isSharedMemory, uint8_t** data);

/**
 * Return a pointer to the start of the array buffer's data, and indicate
 * whether the data is from a shared array buffer through an outparam.
 *
 * The returned data pointer may be invalidated by a GC or by an unshared array
 * buffer becoming detached. Callers must take care not to perform any actions
 * that could trigger a GC or result in an unshared array buffer becoming
 * detached. If such actions nonetheless must be performed, callers should
 * perform this call a second time (and sensibly handle results that may be
 * different from those returned the first time). (Sharedness is an immutable
 * characteristic of an array buffer or shared array buffer, so that boolean
 * remains valid across GC or detaching.)
 *
 * |obj| must have passed a JS::IsArrayBufferObjectMaybeShared test, or somehow
 * be known that it would pass such a test: it is an ArrayBuffer or
 * SharedArrayBuffer or a wrapper of an ArrayBuffer/SharedArrayBuffer, and the
 * unwrapping will succeed.
 *
 * |*isSharedMemory| will be set to true if the typed array maps a
 * SharedArrayBuffer, otherwise to false.
 */
extern JS_PUBLIC_API uint8_t* GetArrayBufferMaybeSharedData(
    JSObject* obj, bool* isSharedMemory, const AutoRequireNoGC&);

/**
 * Returns whether the passed array buffer is 'large': its byteLength >= 2 GB.
 * See also SetLargeArrayBuffersEnabled.
 *
 * |obj| must pass a JS::IsArrayBufferObjectMaybeShared test.
 */
extern JS_PUBLIC_API bool IsLargeArrayBufferMaybeShared(JSObject* obj);

}  // namespace JS

#endif /* js_ArrayBufferMaybeShared_h */
