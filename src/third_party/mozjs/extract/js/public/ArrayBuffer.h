/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ArrayBuffer functionality. */

#ifndef js_ArrayBuffer_h
#define js_ArrayBuffer_h

#include "mozilla/UniquePtr.h"

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API
#include "js/TypeDecls.h"
#include "js/Utility.h"

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {

class JS_PUBLIC_API AutoRequireNoGC;

// CREATION

/**
 * Create a new ArrayBuffer with the given byte length.
 */
extern JS_PUBLIC_API JSObject* NewArrayBuffer(JSContext* cx, size_t nbytes);

/**
 * Create a new ArrayBuffer with the given |contents|, which may be null only
 * if |nbytes == 0|.  |contents| must be allocated compatible with deallocation
 * by |JS_free|.
 *
 * Care must be taken that |nbytes| bytes of |contents| remain valid for the
 * duration of this call.  In particular, passing the length/pointer of existing
 * typed array or ArrayBuffer data is generally unsafe: if a GC occurs during a
 * call to this function, it could move those contents to a different location
 * and invalidate the provided pointer.
 */
extern JS_PUBLIC_API JSObject* NewArrayBufferWithContents(
    JSContext* cx, size_t nbytes,
    mozilla::UniquePtr<void, JS::FreePolicy> contents);

/**
 * Create a new ArrayBuffer with the given |contents|, which may be null only
 * if |nbytes == 0|.  |contents| must be allocated compatible with deallocation
 * by |JS_free|.
 *
 * Care must be taken that |nbytes| bytes of |contents| remain valid for the
 * duration of this call.  In particular, passing the length/pointer of existing
 * typed array or ArrayBuffer data is generally unsafe: if a GC occurs during a
 * call to this function, it could move those contents to a different location
 * and invalidate the provided pointer.
 */
inline JS_PUBLIC_API JSObject* NewArrayBufferWithContents(
    JSContext* cx, size_t nbytes,
    mozilla::UniquePtr<char[], JS::FreePolicy> contents) {
  // As a convenience, provide an overload for UniquePtr<char[]>.
  mozilla::UniquePtr<void, JS::FreePolicy> ptr{contents.release()};
  return NewArrayBufferWithContents(cx, nbytes, std::move(ptr));
}

/**
 * Create a new ArrayBuffer with the given |contents|, which may be null only
 * if |nbytes == 0|.  |contents| must be allocated compatible with deallocation
 * by |JS_free|.
 *
 * Care must be taken that |nbytes| bytes of |contents| remain valid for the
 * duration of this call.  In particular, passing the length/pointer of existing
 * typed array or ArrayBuffer data is generally unsafe: if a GC occurs during a
 * call to this function, it could move those contents to a different location
 * and invalidate the provided pointer.
 */
inline JS_PUBLIC_API JSObject* NewArrayBufferWithContents(
    JSContext* cx, size_t nbytes,
    mozilla::UniquePtr<uint8_t[], JS::FreePolicy> contents) {
  // As a convenience, provide an overload for UniquePtr<uint8_t[]>.
  mozilla::UniquePtr<void, JS::FreePolicy> ptr{contents.release()};
  return NewArrayBufferWithContents(cx, nbytes, std::move(ptr));
}

/**
 * Marker enum to notify callers that the buffer contents must be freed manually
 * when the ArrayBuffer allocation failed.
 */
enum class NewArrayBufferOutOfMemory { CallerMustFreeMemory };

/**
 * Create a new ArrayBuffer with the given |contents|, which may be null only
 * if |nbytes == 0|.  |contents| must be allocated compatible with deallocation
 * by |JS_free|.
 *
 * !!! IMPORTANT !!!
 * If and only if an ArrayBuffer is successfully created and returned,
 * ownership of |contents| is transferred to the new ArrayBuffer.
 *
 * Care must be taken that |nbytes| bytes of |contents| remain valid for the
 * duration of this call.  In particular, passing the length/pointer of existing
 * typed array or ArrayBuffer data is generally unsafe: if a GC occurs during a
 * call to this function, it could move those contents to a different location
 * and invalidate the provided pointer.
 */
extern JS_PUBLIC_API JSObject* NewArrayBufferWithContents(
    JSContext* cx, size_t nbytes, void* contents, NewArrayBufferOutOfMemory);

/**
 * Create a new ArrayBuffer, whose bytes are set to the values of the bytes in
 * the provided ArrayBuffer.
 *
 * |maybeArrayBuffer| is asserted to be non-null.  An error is thrown if
 * |maybeArrayBuffer| would fail the |IsArrayBufferObject| test given further
 * below or if |maybeArrayBuffer| is detached.
 *
 * |maybeArrayBuffer| may store its contents in any fashion (i.e. it doesn't
 * matter whether |maybeArrayBuffer| was allocated using |JS::NewArrayBuffer|,
 * |JS::NewExternalArrayBuffer|, or any other ArrayBuffer-allocating function).
 *
 * The newly-created ArrayBuffer is effectively creatd as if by
 * |JS::NewArrayBufferWithContents| passing in |maybeArrayBuffer|'s internal
 * data pointer and length, in a manner safe against |maybeArrayBuffer|'s data
 * being moved around by the GC.  In particular, the new ArrayBuffer will not
 * behave like one created for WASM or asm.js, so it *can* be detached.
 */
extern JS_PUBLIC_API JSObject* CopyArrayBuffer(
    JSContext* cx, JS::Handle<JSObject*> maybeArrayBuffer);

using BufferContentsFreeFunc = void (*)(void* contents, void* userData);

/**
 * UniquePtr deleter for external buffer contents.
 */
class JS_PUBLIC_API BufferContentsDeleter {
  BufferContentsFreeFunc freeFunc_ = nullptr;
  void* userData_ = nullptr;

 public:
  MOZ_IMPLICIT BufferContentsDeleter(BufferContentsFreeFunc freeFunc,
                                     void* userData = nullptr)
      : freeFunc_(freeFunc), userData_(userData) {}

  void operator()(void* contents) const { freeFunc_(contents, userData_); }

  BufferContentsFreeFunc freeFunc() const { return freeFunc_; }
  void* userData() const { return userData_; }
};

/**
 * Create a new ArrayBuffer with the given contents. The contents must not be
 * modified by any other code, internal or external.
 *
 * When the ArrayBuffer is ready to be disposed of, `freeFunc(contents,
 * freeUserData)` will be called to release the ArrayBuffer's reference on the
 * contents.
 *
 * `freeFunc()` must not call any JSAPI functions that could cause a garbage
 * collection.
 *
 * The caller must keep the buffer alive until `freeFunc()` is called, or, if
 * `freeFunc` is null, until the JSRuntime is destroyed.
 *
 * The caller must not access the buffer on other threads. The JS engine will
 * not allow the buffer to be transferred to other threads. If you try to
 * transfer an external ArrayBuffer to another thread, the data is copied to a
 * new malloc buffer. `freeFunc()` must be threadsafe, and may be called from
 * any thread.
 *
 * This allows ArrayBuffers to be used with embedder objects that use reference
 * counting, for example. In that case the caller is responsible
 * for incrementing the reference count before passing the contents to this
 * function. This also allows using non-reference-counted contents that must be
 * freed with some function other than free().
 */
extern JS_PUBLIC_API JSObject* NewExternalArrayBuffer(
    JSContext* cx, size_t nbytes,
    mozilla::UniquePtr<void, BufferContentsDeleter> contents);

/**
 * Create a new ArrayBuffer with the given non-null |contents|.
 *
 * Ownership of |contents| remains with the caller: it isn't transferred to the
 * returned ArrayBuffer.  Callers of this function *must* ensure that they
 * perform these two steps, in this order, to properly relinquish ownership of
 * |contents|:
 *
 *   1. Call |JS::DetachArrayBuffer| on the buffer returned by this function.
 *      (|JS::DetachArrayBuffer| is generally fallible, but a call under these
 *      circumstances is guaranteed to succeed.)
 *   2. |contents| may be deallocated or discarded consistent with the manner
 *      in which it was allocated.
 *
 * Do not simply allow the returned buffer to be garbage-collected before
 * deallocating |contents|, because in general there is no way to know *when*
 * an object is fully garbage-collected to the point where this would be safe.
 */
extern JS_PUBLIC_API JSObject* NewArrayBufferWithUserOwnedContents(
    JSContext* cx, size_t nbytes, void* contents);

/**
 * Create a new mapped ArrayBuffer with the given memory mapped contents. It
 * must be legal to free the contents pointer by unmapping it. On success,
 * ownership is transferred to the new mapped ArrayBuffer.
 */
extern JS_PUBLIC_API JSObject* NewMappedArrayBufferWithContents(JSContext* cx,
                                                                size_t nbytes,
                                                                void* contents);

/**
 * Create memory mapped ArrayBuffer contents.
 * Caller must take care of closing fd after calling this function.
 */
extern JS_PUBLIC_API void* CreateMappedArrayBufferContents(int fd,
                                                           size_t offset,
                                                           size_t length);

/**
 * Release the allocated resource of mapped ArrayBuffer contents before the
 * object is created.
 * If a new object has been created by JS::NewMappedArrayBufferWithContents()
 * with this content, then JS::DetachArrayBuffer() should be used instead to
 * release the resource used by the object.
 */
extern JS_PUBLIC_API void ReleaseMappedArrayBufferContents(void* contents,
                                                           size_t length);

// TYPE TESTING

/*
 * Check whether obj supports the JS::GetArrayBuffer* APIs.  Note that this may
 * return false if a security wrapper is encountered that denies the unwrapping.
 * If this test succeeds, then it is safe to call the various predicate and
 * accessor JSAPI calls defined below.
 */
extern JS_PUBLIC_API bool IsArrayBufferObject(JSObject* obj);

// PREDICATES

/**
 * Check whether the obj is a detached ArrayBufferObject. Note that this may
 * return false if a security wrapper is encountered that denies the
 * unwrapping.
 */
extern JS_PUBLIC_API bool IsDetachedArrayBufferObject(JSObject* obj);

/**
 * Check whether the obj is ArrayBufferObject and memory mapped. Note that this
 * may return false if a security wrapper is encountered that denies the
 * unwrapping.
 */
extern JS_PUBLIC_API bool IsMappedArrayBufferObject(JSObject* obj);

/**
 * Return true if the ArrayBuffer |obj| contains any data, i.e. it is not a
 * detached ArrayBuffer.  (ArrayBuffer.prototype is not an ArrayBuffer.)
 *
 * |obj| must have passed a JS::IsArrayBufferObject test, or somehow be known
 * that it would pass such a test: it is an ArrayBuffer or a wrapper of an
 * ArrayBuffer, and the unwrapping will succeed.
 */
extern JS_PUBLIC_API bool ArrayBufferHasData(JSObject* obj);

// ACCESSORS

extern JS_PUBLIC_API JSObject* UnwrapArrayBuffer(JSObject* obj);

/**
 * Attempt to unwrap |obj| as an ArrayBuffer.
 *
 * If |obj| *is* an ArrayBuffer, return it unwrapped and set |*length| and
 * |*data| to weakly refer to the ArrayBuffer's contents.
 *
 * If |obj| isn't an ArrayBuffer, return nullptr and do not modify |*length| or
 * |*data|.
 */
extern JS_PUBLIC_API JSObject* GetObjectAsArrayBuffer(JSObject* obj,
                                                      size_t* length,
                                                      uint8_t** data);

/**
 * Return the available byte length of an ArrayBuffer.
 *
 * |obj| must have passed a JS::IsArrayBufferObject test, or somehow be known
 * that it would pass such a test: it is an ArrayBuffer or a wrapper of an
 * ArrayBuffer, and the unwrapping will succeed.
 */
extern JS_PUBLIC_API size_t GetArrayBufferByteLength(JSObject* obj);

// This one isn't inlined because there are a bunch of different ArrayBuffer
// classes that would have to be individually handled here.
//
// There is an isShared out argument for API consistency (eases use from DOM).
// It will always be set to false.
extern JS_PUBLIC_API void GetArrayBufferLengthAndData(JSObject* obj,
                                                      size_t* length,
                                                      bool* isSharedMemory,
                                                      uint8_t** data);

/**
 * Return a pointer to the start of the data referenced by a typed array. The
 * data is still owned by the typed array, and should not be modified on
 * another thread. Furthermore, the pointer can become invalid on GC (if the
 * data is small and fits inside the array's GC header), so callers must take
 * care not to hold on across anything that could GC.
 *
 * |obj| must have passed a JS::IsArrayBufferObject test, or somehow be known
 * that it would pass such a test: it is an ArrayBuffer or a wrapper of an
 * ArrayBuffer, and the unwrapping will succeed.
 *
 * |*isSharedMemory| is always set to false.  The argument is present to
 * simplify its use from code that also interacts with SharedArrayBuffer.
 */
extern JS_PUBLIC_API uint8_t* GetArrayBufferData(JSObject* obj,
                                                 bool* isSharedMemory,
                                                 const AutoRequireNoGC&);

// MUTATORS

/**
 * Detach an ArrayBuffer, causing all associated views to no longer refer to
 * the ArrayBuffer's original attached memory.
 *
 * This function throws only if it is provided a non-ArrayBuffer object or if
 * the provided ArrayBuffer is a WASM-backed ArrayBuffer or an ArrayBuffer used
 * in asm.js code.
 */
extern JS_PUBLIC_API bool DetachArrayBuffer(JSContext* cx,
                                            Handle<JSObject*> obj);

// Indicates if an object has a defined [[ArrayBufferDetachKey]] internal slot,
// which indicates an ArrayBuffer cannot be detached
extern JS_PUBLIC_API bool HasDefinedArrayBufferDetachKey(JSContext* cx,
                                                         Handle<JSObject*> obj,
                                                         bool* isDefined);

/**
 * Steal the contents of the given ArrayBuffer. The ArrayBuffer has its length
 * set to 0 and its contents array cleared. The caller takes ownership of the
 * return value and must free it or transfer ownership via
 * JS::NewArrayBufferWithContents when done using it.
 */
extern JS_PUBLIC_API void* StealArrayBufferContents(JSContext* cx,
                                                    Handle<JSObject*> obj);

/**
 * Copy data from one array buffer to another.
 *
 * Both fromBuffer and toBuffer must be (possibly wrapped)
 * ArrayBufferObjectMaybeShared.
 *
 * This method may throw if the sizes don't match, or if unwrapping fails.
 *
 * The API for this is modelled on CopyDataBlockBytes from the spec:
 * https://tc39.es/ecma262/#sec-copydatablockbytes
 */
[[nodiscard]] extern JS_PUBLIC_API bool ArrayBufferCopyData(
    JSContext* cx, Handle<JSObject*> toBlock, size_t toIndex,
    Handle<JSObject*> fromBlock, size_t fromIndex, size_t count);

/**
 * Copy data from one array buffer to another.
 *
 * srcBuffer must be a (possibly wrapped) ArrayBufferObjectMaybeShared.
 *
 * This method may throw if unwrapping or allocation fails.
 *
 * The API for this is modelled on CloneArrayBuffer from the spec:
 * https://tc39.es/ecma262/#sec-clonearraybuffer
 */
extern JS_PUBLIC_API JSObject* ArrayBufferClone(JSContext* cx,
                                                Handle<JSObject*> srcBuffer,
                                                size_t srcByteOffset,
                                                size_t srcLength);

}  // namespace JS

#endif /* js_ArrayBuffer_h */
