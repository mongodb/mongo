/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Typed array, ArrayBuffer, and DataView creation, predicate, and accessor
 * functions.
 */

#ifndef js_experimental_TypedData_h
#define js_experimental_TypedData_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_CRASH
#include "mozilla/Casting.h"     // mozilla::AssertedCast

#include <stddef.h>  // size_t
#include <stdint.h>  // {,u}int8_t, {,u}int16_t, {,u}int32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Object.h"      // JS::GetClass, JS::GetPrivate, JS::GetReservedSlot
#include "js/RootingAPI.h"  // JS::Handle
#include "js/ScalarType.h"  // js::Scalar::Type

struct JSClass;
class JS_PUBLIC_API JSObject;

namespace JS {

class JS_PUBLIC_API AutoRequireNoGC;

}  // namespace JS

/*
 * Create a new typed array with nelements elements.
 *
 * These functions (except the WithBuffer variants) fill in the array with
 * zeros.
 */

extern JS_PUBLIC_API JSObject* JS_NewInt8Array(JSContext* cx, size_t nelements);
extern JS_PUBLIC_API JSObject* JS_NewUint8Array(JSContext* cx,
                                                size_t nelements);
extern JS_PUBLIC_API JSObject* JS_NewUint8ClampedArray(JSContext* cx,
                                                       size_t nelements);
extern JS_PUBLIC_API JSObject* JS_NewInt16Array(JSContext* cx,
                                                size_t nelements);
extern JS_PUBLIC_API JSObject* JS_NewUint16Array(JSContext* cx,
                                                 size_t nelements);
extern JS_PUBLIC_API JSObject* JS_NewInt32Array(JSContext* cx,
                                                size_t nelements);
extern JS_PUBLIC_API JSObject* JS_NewUint32Array(JSContext* cx,
                                                 size_t nelements);
extern JS_PUBLIC_API JSObject* JS_NewFloat32Array(JSContext* cx,
                                                  size_t nelements);
extern JS_PUBLIC_API JSObject* JS_NewFloat64Array(JSContext* cx,
                                                  size_t nelements);

/*
 * Create a new typed array and copy in values from the given object. The
 * object is used as if it were an array; that is, the new array (if
 * successfully created) will have length given by array.length, and its
 * elements will be those specified by array[0], array[1], and so on, after
 * conversion to the typed array element type.
 */

extern JS_PUBLIC_API JSObject* JS_NewInt8ArrayFromArray(
    JSContext* cx, JS::Handle<JSObject*> array);
extern JS_PUBLIC_API JSObject* JS_NewUint8ArrayFromArray(
    JSContext* cx, JS::Handle<JSObject*> array);
extern JS_PUBLIC_API JSObject* JS_NewUint8ClampedArrayFromArray(
    JSContext* cx, JS::Handle<JSObject*> array);
extern JS_PUBLIC_API JSObject* JS_NewInt16ArrayFromArray(
    JSContext* cx, JS::Handle<JSObject*> array);
extern JS_PUBLIC_API JSObject* JS_NewUint16ArrayFromArray(
    JSContext* cx, JS::Handle<JSObject*> array);
extern JS_PUBLIC_API JSObject* JS_NewInt32ArrayFromArray(
    JSContext* cx, JS::Handle<JSObject*> array);
extern JS_PUBLIC_API JSObject* JS_NewUint32ArrayFromArray(
    JSContext* cx, JS::Handle<JSObject*> array);
extern JS_PUBLIC_API JSObject* JS_NewFloat32ArrayFromArray(
    JSContext* cx, JS::Handle<JSObject*> array);
extern JS_PUBLIC_API JSObject* JS_NewFloat64ArrayFromArray(
    JSContext* cx, JS::Handle<JSObject*> array);

/*
 * Create a new typed array using the given ArrayBuffer or
 * SharedArrayBuffer for storage.  The length value is optional; if -1
 * is passed, enough elements to use up the remainder of the byte
 * array is used as the default value.
 */

extern JS_PUBLIC_API JSObject* JS_NewInt8ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewUint8ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewUint8ClampedArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewInt16ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewUint16ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewInt32ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewUint32ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewBigInt64ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewBigUint64ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewFloat32ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);
extern JS_PUBLIC_API JSObject* JS_NewFloat64ArrayWithBuffer(
    JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset,
    int64_t length);

/**
 * Check whether obj supports JS_GetTypedArray* APIs. Note that this may return
 * false if a security wrapper is encountered that denies the unwrapping. If
 * this test or one of the JS_Is*Array tests succeeds, then it is safe to call
 * the various accessor JSAPI calls defined below.
 */
extern JS_PUBLIC_API bool JS_IsTypedArrayObject(JSObject* obj);

/**
 * Check whether obj supports JS_GetArrayBufferView* APIs. Note that this may
 * return false if a security wrapper is encountered that denies the
 * unwrapping. If this test or one of the more specific tests succeeds, then it
 * is safe to call the various ArrayBufferView accessor JSAPI calls defined
 * below.
 */
extern JS_PUBLIC_API bool JS_IsArrayBufferViewObject(JSObject* obj);

/*
 * Test for specific typed array types (ArrayBufferView subtypes)
 */

extern JS_PUBLIC_API bool JS_IsInt8Array(JSObject* obj);
extern JS_PUBLIC_API bool JS_IsUint8Array(JSObject* obj);
extern JS_PUBLIC_API bool JS_IsUint8ClampedArray(JSObject* obj);
extern JS_PUBLIC_API bool JS_IsInt16Array(JSObject* obj);
extern JS_PUBLIC_API bool JS_IsUint16Array(JSObject* obj);
extern JS_PUBLIC_API bool JS_IsInt32Array(JSObject* obj);
extern JS_PUBLIC_API bool JS_IsUint32Array(JSObject* obj);
extern JS_PUBLIC_API bool JS_IsFloat32Array(JSObject* obj);
extern JS_PUBLIC_API bool JS_IsFloat64Array(JSObject* obj);

/**
 * Return the isShared flag of a typed array, which denotes whether
 * the underlying buffer is a SharedArrayBuffer.
 *
 * |obj| must have passed a JS_IsTypedArrayObject/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is a typed array or a wrapper of
 * a typed array, and the unwrapping will succeed.
 */
extern JS_PUBLIC_API bool JS_GetTypedArraySharedness(JSObject* obj);

/*
 * Test for specific typed array types (ArrayBufferView subtypes) and return
 * the unwrapped object if so, else nullptr.  Never throws.
 */

namespace js {

extern JS_PUBLIC_API JSObject* UnwrapInt8Array(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapUint8Array(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapUint8ClampedArray(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapInt16Array(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapUint16Array(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapInt32Array(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapUint32Array(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapBigInt64Array(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapBigUint64Array(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapFloat32Array(JSObject* obj);
extern JS_PUBLIC_API JSObject* UnwrapFloat64Array(JSObject* obj);

extern JS_PUBLIC_API JSObject* UnwrapArrayBufferView(JSObject* obj);

extern JS_PUBLIC_API JSObject* UnwrapReadableStream(JSObject* obj);

namespace detail {

extern JS_PUBLIC_DATA const JSClass* const Int8ArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const Uint8ArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const Uint8ClampedArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const Int16ArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const Uint16ArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const Int32ArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const Uint32ArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const BigInt64ArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const BigUint64ArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const Float32ArrayClassPtr;
extern JS_PUBLIC_DATA const JSClass* const Float64ArrayClassPtr;

const size_t TypedArrayLengthSlot = 1;

}  // namespace detail

#define JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Type, type)                    \
  inline void Get##Type##ArrayLengthAndData(                              \
      JSObject* obj, size_t* length, bool* isSharedMemory, type** data) { \
    MOZ_ASSERT(JS::GetClass(obj) == detail::Type##ArrayClassPtr);         \
    const JS::Value& lenSlot =                                            \
        JS::GetReservedSlot(obj, detail::TypedArrayLengthSlot);           \
    *length = size_t(lenSlot.toPrivate());                                \
    *isSharedMemory = JS_GetTypedArraySharedness(obj);                    \
    *data = static_cast<type*>(JS::GetPrivate(obj));                      \
  }

JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Int8, int8_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Uint8, uint8_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Uint8Clamped, uint8_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Int16, int16_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Uint16, uint16_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Int32, int32_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Uint32, uint32_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Float32, float)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Float64, double)

#undef JS_DEFINE_DATA_AND_LENGTH_ACCESSOR

// This one isn't inlined because it's rather tricky (by dint of having to deal
// with a dozen-plus classes and varying slot layouts.
extern JS_PUBLIC_API void GetArrayBufferViewLengthAndData(JSObject* obj,
                                                          size_t* length,
                                                          bool* isSharedMemory,
                                                          uint8_t** data);

}  // namespace js

/*
 * Unwrap Typed arrays all at once. Return nullptr without throwing if the
 * object cannot be viewed as the correct typed array, or the typed array
 * object on success, filling both outparameters.
 */
extern JS_PUBLIC_API JSObject* JS_GetObjectAsInt8Array(JSObject* obj,
                                                       size_t* length,
                                                       bool* isSharedMemory,
                                                       int8_t** data);
extern JS_PUBLIC_API JSObject* JS_GetObjectAsUint8Array(JSObject* obj,
                                                        size_t* length,
                                                        bool* isSharedMemory,
                                                        uint8_t** data);
extern JS_PUBLIC_API JSObject* JS_GetObjectAsUint8ClampedArray(
    JSObject* obj, size_t* length, bool* isSharedMemory, uint8_t** data);
extern JS_PUBLIC_API JSObject* JS_GetObjectAsInt16Array(JSObject* obj,
                                                        size_t* length,
                                                        bool* isSharedMemory,
                                                        int16_t** data);
extern JS_PUBLIC_API JSObject* JS_GetObjectAsUint16Array(JSObject* obj,
                                                         size_t* length,
                                                         bool* isSharedMemory,
                                                         uint16_t** data);
extern JS_PUBLIC_API JSObject* JS_GetObjectAsInt32Array(JSObject* obj,
                                                        size_t* length,
                                                        bool* isSharedMemory,
                                                        int32_t** data);
extern JS_PUBLIC_API JSObject* JS_GetObjectAsUint32Array(JSObject* obj,
                                                         size_t* length,
                                                         bool* isSharedMemory,
                                                         uint32_t** data);
extern JS_PUBLIC_API JSObject* JS_GetObjectAsFloat32Array(JSObject* obj,
                                                          size_t* length,
                                                          bool* isSharedMemory,
                                                          float** data);
extern JS_PUBLIC_API JSObject* JS_GetObjectAsFloat64Array(JSObject* obj,
                                                          size_t* length,
                                                          bool* isSharedMemory,
                                                          double** data);
extern JS_PUBLIC_API JSObject* JS_GetObjectAsArrayBufferView(
    JSObject* obj, size_t* length, bool* isSharedMemory, uint8_t** data);

/*
 * Get the type of elements in a typed array, or MaxTypedArrayViewType if a
 * DataView.
 *
 * |obj| must have passed a JS_IsArrayBufferView/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is an ArrayBufferView or a
 * wrapper of an ArrayBufferView, and the unwrapping will succeed.
 */
extern JS_PUBLIC_API js::Scalar::Type JS_GetArrayBufferViewType(JSObject* obj);

/**
 * Return the number of elements in a typed array.
 *
 * |obj| must have passed a JS_IsTypedArrayObject/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is a typed array or a wrapper of
 * a typed array, and the unwrapping will succeed.
 */
extern JS_PUBLIC_API size_t JS_GetTypedArrayLength(JSObject* obj);

/**
 * Return the byte offset from the start of an ArrayBuffer to the start of a
 * typed array view.
 *
 * |obj| must have passed a JS_IsTypedArrayObject/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is a typed array or a wrapper of
 * a typed array, and the unwrapping will succeed.
 */
extern JS_PUBLIC_API size_t JS_GetTypedArrayByteOffset(JSObject* obj);

/**
 * Return the byte length of a typed array.
 *
 * |obj| must have passed a JS_IsTypedArrayObject/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is a typed array or a wrapper of
 * a typed array, and the unwrapping will succeed.
 */
extern JS_PUBLIC_API size_t JS_GetTypedArrayByteLength(JSObject* obj);

/**
 * More generic name for JS_GetTypedArrayByteLength to cover DataViews as well
 */
extern JS_PUBLIC_API size_t JS_GetArrayBufferViewByteLength(JSObject* obj);

/**
 * More generic name for JS_GetTypedArrayByteOffset to cover DataViews as well
 */
extern JS_PUBLIC_API size_t JS_GetArrayBufferViewByteOffset(JSObject* obj);

/*
 * Return a pointer to the start of the data referenced by a typed array. The
 * data is still owned by the typed array, and should not be modified on
 * another thread. Furthermore, the pointer can become invalid on GC (if the
 * data is small and fits inside the array's GC header), so callers must take
 * care not to hold on across anything that could GC.
 *
 * |obj| must have passed a JS_Is*Array test, or somehow be known that it would
 * pass such a test: it is a typed array or a wrapper of a typed array, and the
 * unwrapping will succeed.
 *
 * |*isSharedMemory| will be set to true if the typed array maps a
 * SharedArrayBuffer, otherwise to false.
 */

extern JS_PUBLIC_API int8_t* JS_GetInt8ArrayData(JSObject* obj,
                                                 bool* isSharedMemory,
                                                 const JS::AutoRequireNoGC&);
extern JS_PUBLIC_API uint8_t* JS_GetUint8ArrayData(JSObject* obj,
                                                   bool* isSharedMemory,
                                                   const JS::AutoRequireNoGC&);
extern JS_PUBLIC_API uint8_t* JS_GetUint8ClampedArrayData(
    JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&);
extern JS_PUBLIC_API int16_t* JS_GetInt16ArrayData(JSObject* obj,
                                                   bool* isSharedMemory,
                                                   const JS::AutoRequireNoGC&);
extern JS_PUBLIC_API uint16_t* JS_GetUint16ArrayData(
    JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&);
extern JS_PUBLIC_API int32_t* JS_GetInt32ArrayData(JSObject* obj,
                                                   bool* isSharedMemory,
                                                   const JS::AutoRequireNoGC&);
extern JS_PUBLIC_API uint32_t* JS_GetUint32ArrayData(
    JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&);
extern JS_PUBLIC_API float* JS_GetFloat32ArrayData(JSObject* obj,
                                                   bool* isSharedMemory,
                                                   const JS::AutoRequireNoGC&);
extern JS_PUBLIC_API double* JS_GetFloat64ArrayData(JSObject* obj,
                                                    bool* isSharedMemory,
                                                    const JS::AutoRequireNoGC&);

/**
 * Same as above, but for any kind of ArrayBufferView. Prefer the type-specific
 * versions when possible.
 */
extern JS_PUBLIC_API void* JS_GetArrayBufferViewData(
    JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&);

/**
 * Return a "fixed" pointer (one that will not move during a GC) to the
 * ArrayBufferView's data. Note that this will not keep the object alive; the
 * holding object should be rooted or traced. If the view is storing the data
 * inline, this will copy the data to the provided buffer, returning nullptr if
 * bufSize is inadequate.
 *
 * Avoid using this unless necessary. JS_GetArrayBufferViewData is simpler and
 * more efficient because it requires the caller to ensure that a GC will not
 * occur and thus does not need to handle movable data.
 */
extern JS_PUBLIC_API uint8_t* JS_GetArrayBufferViewFixedData(JSObject* obj,
                                                             uint8_t* buffer,
                                                             size_t bufSize);

/**
 * If the bufSize passed to JS_GetArrayBufferViewFixedData is at least this
 * many bytes, then any copied data is guaranteed to fit into the provided
 * buffer.
 */
extern JS_PUBLIC_API size_t JS_MaxMovableTypedArraySize();

/**
 * Return the ArrayBuffer or SharedArrayBuffer underlying an ArrayBufferView.
 * This may return a detached buffer.  |obj| must be an object that would
 * return true for JS_IsArrayBufferViewObject().
 */
extern JS_PUBLIC_API JSObject* JS_GetArrayBufferViewBuffer(
    JSContext* cx, JS::Handle<JSObject*> obj, bool* isSharedMemory);

/**
 * Create a new DataView using the given buffer for storage. The given buffer
 * must be an ArrayBuffer or SharedArrayBuffer (or a cross-compartment wrapper
 * of either type), and the offset and length must fit within the bounds of the
 * buffer. Currently, nullptr will be returned and an exception will be thrown
 * if these conditions do not hold, but do not depend on that behavior.
 */
JS_PUBLIC_API JSObject* JS_NewDataView(JSContext* cx,
                                       JS::Handle<JSObject*> buffer,
                                       size_t byteOffset, size_t byteLength);

namespace JS {

/*
 * Returns whether the passed array buffer view is 'large': its byteLength >= 2
 * GB. See also SetLargeArrayBuffersEnabled.
 *
 * |obj| must pass a JS_IsArrayBufferViewObject test.
 */
JS_PUBLIC_API bool IsLargeArrayBufferView(JSObject* obj);

}  // namespace JS

#endif  // js_experimental_TypedData_h
