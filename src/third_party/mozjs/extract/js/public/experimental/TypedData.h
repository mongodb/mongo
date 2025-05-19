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
#include "mozilla/Span.h"

#include <stddef.h>  // size_t
#include <stdint.h>  // {,u}int8_t, {,u}int16_t, {,u}int32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Object.h"  // JS::GetClass, JS::GetReservedSlot, JS::GetMaybePtrFromReservedSlot
#include "js/RootingAPI.h"  // JS::Handle, JS_DECLARE_IS_HEAP_CONSTRUCTIBLE_TYPE
#include "js/ScalarType.h"  // JS::Scalar::Type
#include "js/Wrapper.h"     // js::CheckedUnwrapStatic

struct JSClass;
class JS_PUBLIC_API JSObject;

namespace JS {

class JS_PUBLIC_API AutoRequireNoGC;

}  // namespace JS

// JS_FOR_EACH_TYPED_ARRAY(MACRO) expands MACRO once for each specific
// typed array subtype (Int8Array, Float64Array, ...), passing arguments
// as MACRO(ExternalT, NativeT, Name) where
//
// ExternalT - externally-exposed element type (eg uint8_t)
//
// NativeT - element type used for the implementation (eg js::uint8_clamped_t)
//   Note that this type is not exposed publicly. Internal files need to
//   #include <vm/Uint8Clamped.h> to see it.
//
// Name - a name usable as both a JS::Scalar::Type value (eg
//   JS::Scalar::Uint8Clamped) or the stem of a full typed array name (eg
//   Uint8ClampedArray)
//
#define JS_FOR_EACH_TYPED_ARRAY(MACRO)            \
  MACRO(int8_t, int8_t, Int8)                     \
  MACRO(uint8_t, uint8_t, Uint8)                  \
  MACRO(int16_t, int16_t, Int16)                  \
  MACRO(uint16_t, uint16_t, Uint16)               \
  MACRO(int32_t, int32_t, Int32)                  \
  MACRO(uint32_t, uint32_t, Uint32)               \
  MACRO(float, float, Float32)                    \
  MACRO(double, double, Float64)                  \
  MACRO(uint8_t, js::uint8_clamped, Uint8Clamped) \
  MACRO(int64_t, int64_t, BigInt64)               \
  MACRO(uint64_t, uint64_t, BigUint64)            \
  MACRO(uint16_t, js::float16, Float16)

/*
 * JS_New(type)Array:
 *
 * Create a new typed array with nelements elements.
 *
 * These functions (except the WithBuffer variants) fill in the array with
 * zeros.
 *
 * JS_New(type)ArrayFromArray:
 *
 * Create a new typed array and copy in values from the given object. The
 * object is used as if it were an array; that is, the new array (if
 * successfully created) will have length given by array.length, and its
 * elements will be those specified by array[0], array[1], and so on, after
 * conversion to the typed array element type.
 *
 * JS_New(type)ArrayWithBuffer:
 *
 * Create a new typed array using the given ArrayBuffer or
 * SharedArrayBuffer for storage.  The length value is optional; if -1
 * is passed, enough elements to use up the remainder of the byte
 * array is used as the default value.
 */

#define DECLARE_TYPED_ARRAY_CREATION_API(ExternalType, NativeType, Name)   \
  extern JS_PUBLIC_API JSObject* JS_New##Name##Array(JSContext* cx,        \
                                                     size_t nelements);    \
  extern JS_PUBLIC_API JSObject* JS_New##Name##ArrayFromArray(             \
      JSContext* cx, JS::Handle<JSObject*> array);                         \
  extern JS_PUBLIC_API JSObject* JS_New##Name##ArrayWithBuffer(            \
      JSContext* cx, JS::Handle<JSObject*> arrayBuffer, size_t byteOffset, \
      int64_t length);

JS_FOR_EACH_TYPED_ARRAY(DECLARE_TYPED_ARRAY_CREATION_API)
#undef DECLARE_TYPED_ARRAY_CREATION_API

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

extern JS_PUBLIC_API JSObject* UnwrapArrayBufferView(JSObject* obj);

extern JS_PUBLIC_API JSObject* UnwrapReadableStream(JSObject* obj);

namespace detail {

constexpr size_t TypedArrayLengthSlot = 1;
constexpr size_t TypedArrayDataSlot = 3;

}  // namespace detail

// This one isn't inlined because it's rather tricky (by dint of having to deal
// with a dozen-plus classes and varying slot layouts.
extern JS_PUBLIC_API void GetArrayBufferViewLengthAndData(JSObject* obj,
                                                          size_t* length,
                                                          bool* isSharedMemory,
                                                          uint8_t** data);

}  // namespace js

/*
 * JS_GetObjectAs(type)Array(JSObject* maybeWrapped, size_t* length, bool*
 * isSharedMemory, element_type** data)
 *
 * Unwrap Typed arrays all at once. Return nullptr without throwing if the
 * object cannot be viewed as the correct typed array, or the typed array
 * object on success, filling both outparameters.
 */
#define DECLARE_GET_OBJECT_AS(ExternalType, NativeType, Name)       \
  extern JS_PUBLIC_API JSObject* JS_GetObjectAs##Name##Array(       \
      JSObject* maybeWrapped, size_t* length, bool* isSharedMemory, \
      ExternalType** data);
JS_FOR_EACH_TYPED_ARRAY(DECLARE_GET_OBJECT_AS)
#undef DECLARE_GET_OBJECT_AS

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
extern JS_PUBLIC_API JS::Scalar::Type JS_GetArrayBufferViewType(JSObject* obj);

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
 * GB.
 *
 * |obj| must pass a JS_IsArrayBufferViewObject test.
 */
JS_PUBLIC_API bool IsLargeArrayBufferView(JSObject* obj);

/*
 * Returns whether the passed array buffer view has a resizable or growable
 * array buffer.
 *
 * |obj| must pass a JS_IsArrayBufferViewObject test.
 */
JS_PUBLIC_API bool IsResizableArrayBufferView(JSObject* obj);

/*
 * Given an ArrayBuffer or view, prevent the length of the underlying
 * ArrayBuffer from changing (with pin=true) until unfrozen (with
 * pin=false). Note that some objects (eg SharedArrayBuffers) cannot change
 * length to begin with, and are treated as always pinned.
 *
 * ArrayBuffers and their views can change length by being detached, or
 * if they are ResizableArrayBuffers or (shared) GrowableArrayBuffers.
 *
 * Returns whether the pinned status changed.
 */
JS_PUBLIC_API bool PinArrayBufferOrViewLength(JSObject* obj, bool pin);

/*
 * Given an ArrayBuffer or view, make sure its contents are not stored inline
 * so that the data is safe for use even if a GC moves the owning object.
 *
 * Note that this by itself does not make it safe to use the data pointer
 * if JS can run or the ArrayBuffer can be detached in any way. Consider using
 * this in conjunction with PinArrayBufferOrViewLength, which will cause any
 * potentially invalidating operations to fail.
 */
JS_PUBLIC_API bool EnsureNonInlineArrayBufferOrView(JSContext* cx,
                                                    JSObject* obj);

namespace detail {

// Map from eg Uint8Clamped -> uint8_t, Uint8 -> uint8_t, or Float64 ->
// double. Used as the DataType within a JS::TypedArray specialization.
template <JS::Scalar::Type ArrayType>
struct ExternalTypeOf {};

#define DEFINE_ELEMENT_TYPES(ExternalT, NativeT, Name) \
  template <>                                          \
  struct ExternalTypeOf<JS::Scalar::Name> {            \
    using Type = ExternalT;                            \
  };
JS_FOR_EACH_TYPED_ARRAY(DEFINE_ELEMENT_TYPES)
#undef DEFINE_ELEMENT_TYPES

template <JS::Scalar::Type ArrayType>
using ExternalTypeOf_t = typename ExternalTypeOf<ArrayType>::Type;

}  // namespace detail

// A class holding a JSObject referring to a buffer of data. Either an
// ArrayBufferObject or some sort of ArrayBufferViewObject (see below).
// Note that this will always hold an unwrapped object.
class JS_PUBLIC_API ArrayBufferOrView {
 public:
  // Typed Arrays will set this to their specific element type.
  // Everything else just claims to expose things as uint8_t*.
  using DataType = uint8_t;

 protected:
  JSObject* obj;

  explicit ArrayBufferOrView(JSObject* unwrapped) : obj(unwrapped) {}

 public:
  // ArrayBufferOrView subclasses will set `obj` to nullptr if wrapping an
  // object of the wrong type. So this allows:
  //
  //   auto view = JS::TypedArray<JS::Scalar::Int8>::fromObject(obj);
  //   if (!view) { ... }
  //
  explicit operator bool() const { return !!obj; }

  // `obj` must be an unwrapped ArrayBuffer or view, or nullptr.
  static inline ArrayBufferOrView fromObject(JSObject* unwrapped);

  // Unwrap an ArrayBuffer or view. Returns ArrayBufferOrView(nullptr) if
  // `maybeWrapped` is the wrong type or fails unwrapping. Never throw.
  static ArrayBufferOrView unwrap(JSObject* maybeWrapped);

  // Allow use as Rooted<JS::ArrayBufferOrView>.
  void trace(JSTracer* trc) {
    if (obj) {
      js::gc::TraceExternalEdge(trc, &obj, "ArrayBufferOrView object");
    }
  }

  bool isDetached() const;
  bool isResizable() const;

  void exposeToActiveJS() const {
    if (obj) {
      js::BarrierMethods<JSObject*>::exposeToJS(obj);
    }
  }

  JSObject* asObject() const {
    exposeToActiveJS();
    return obj;
  }

  JSObject* asObjectUnbarriered() const { return obj; }

  JSObject** addressOfObject() { return &obj; }

  bool operator==(const ArrayBufferOrView& other) const {
    return obj == other.asObjectUnbarriered();
  }
  bool operator!=(const ArrayBufferOrView& other) const {
    return obj != other.asObjectUnbarriered();
  }
};

class JS_PUBLIC_API ArrayBuffer : public ArrayBufferOrView {
  static const JSClass* const FixedLengthUnsharedClass;
  static const JSClass* const ResizableUnsharedClass;
  static const JSClass* const FixedLengthSharedClass;
  static const JSClass* const GrowableSharedClass;

 protected:
  explicit ArrayBuffer(JSObject* unwrapped) : ArrayBufferOrView(unwrapped) {}

 public:
  static ArrayBuffer fromObject(JSObject* unwrapped) {
    if (unwrapped) {
      const JSClass* clasp = GetClass(unwrapped);
      if (clasp == FixedLengthUnsharedClass ||
          clasp == ResizableUnsharedClass || clasp == FixedLengthSharedClass ||
          clasp == GrowableSharedClass) {
        return ArrayBuffer(unwrapped);
      }
    }
    return ArrayBuffer(nullptr);
  }
  static ArrayBuffer unwrap(JSObject* maybeWrapped);

  static ArrayBuffer create(JSContext* cx, size_t nbytes);

  mozilla::Span<uint8_t> getData(bool* isSharedMemory,
                                 const JS::AutoRequireNoGC&);
};

// A view into an ArrayBuffer, either a DataViewObject or a Typed Array variant.
class JS_PUBLIC_API ArrayBufferView : public ArrayBufferOrView {
 protected:
  explicit ArrayBufferView(JSObject* unwrapped)
      : ArrayBufferOrView(unwrapped) {}

 public:
  static inline ArrayBufferView fromObject(JSObject* unwrapped);
  static ArrayBufferView unwrap(JSObject* maybeWrapped) {
    if (!maybeWrapped) {
      return ArrayBufferView(nullptr);
    }
    ArrayBufferView view = fromObject(maybeWrapped);
    if (view) {
      return view;
    }
    return fromObject(js::CheckedUnwrapStatic(maybeWrapped));
  }

  bool isDetached() const;
  bool isResizable() const;

  mozilla::Span<uint8_t> getData(bool* isSharedMemory,
                                 const JS::AutoRequireNoGC&);

  // Must only be called if !isDetached().
  size_t getByteLength(const JS::AutoRequireNoGC&);
};

class JS_PUBLIC_API DataView : public ArrayBufferView {
  static const JSClass* const FixedLengthClassPtr;
  static const JSClass* const ResizableClassPtr;

 protected:
  explicit DataView(JSObject* unwrapped) : ArrayBufferView(unwrapped) {}

 public:
  static DataView fromObject(JSObject* unwrapped) {
    if (unwrapped) {
      const JSClass* clasp = GetClass(unwrapped);
      if (clasp == FixedLengthClassPtr || clasp == ResizableClassPtr) {
        return DataView(unwrapped);
      }
    }
    return DataView(nullptr);
  }

  static DataView unwrap(JSObject* maybeWrapped) {
    if (!maybeWrapped) {
      return DataView(nullptr);
    }
    DataView view = fromObject(maybeWrapped);
    if (view) {
      return view;
    }
    return fromObject(js::CheckedUnwrapStatic(maybeWrapped));
  }
};

// Base type of all Typed Array variants.
class JS_PUBLIC_API TypedArray_base : public ArrayBufferView {
 protected:
  explicit TypedArray_base(JSObject* unwrapped) : ArrayBufferView(unwrapped) {}

  static const JSClass* const fixedLengthClasses;
  static const JSClass* const resizableClasses;

 public:
  static TypedArray_base fromObject(JSObject* unwrapped);

  static TypedArray_base unwrap(JSObject* maybeWrapped) {
    if (!maybeWrapped) {
      return TypedArray_base(nullptr);
    }
    TypedArray_base view = fromObject(maybeWrapped);
    if (view) {
      return view;
    }
    return fromObject(js::CheckedUnwrapStatic(maybeWrapped));
  }
};

template <JS::Scalar::Type TypedArrayElementType>
class JS_PUBLIC_API TypedArray : public TypedArray_base {
  // This cannot be a static data member because on Windows,
  // __declspec(dllexport) causes the class to be instantiated immediately,
  // leading to errors when later explicit specializations of inline member
  // functions are encountered ("error: explicit specialization of 'ClassPtr'
  // after instantiation"). And those inlines need to be defined outside of the
  // class due to order dependencies. This is the only way I could get it to
  // work on both Windows and POSIX.
  static const JSClass* fixedLengthClasp() {
    return &TypedArray_base::fixedLengthClasses[static_cast<int>(
        TypedArrayElementType)];
  }
  static const JSClass* resizableClasp() {
    return &TypedArray_base::resizableClasses[static_cast<int>(
        TypedArrayElementType)];
  }

 protected:
  explicit TypedArray(JSObject* unwrapped) : TypedArray_base(unwrapped) {}

 public:
  using DataType = detail::ExternalTypeOf_t<TypedArrayElementType>;

  static constexpr JS::Scalar::Type Scalar = TypedArrayElementType;

  static TypedArray create(JSContext* cx, size_t nelements);
  static TypedArray fromArray(JSContext* cx, HandleObject other);
  static TypedArray fromBuffer(JSContext* cx, HandleObject arrayBuffer,
                               size_t byteOffset, int64_t length);

  // Return an interface wrapper around `obj`, or around nullptr if `obj` is not
  // an unwrapped typed array of the correct type.
  static TypedArray fromObject(JSObject* unwrapped) {
    if (unwrapped) {
      const JSClass* clasp = GetClass(unwrapped);
      if (clasp == fixedLengthClasp() || clasp == resizableClasp()) {
        return TypedArray(unwrapped);
      }
    }
    return TypedArray(nullptr);
  }

  static TypedArray unwrap(JSObject* maybeWrapped) {
    if (!maybeWrapped) {
      return TypedArray(nullptr);
    }
    TypedArray view = fromObject(maybeWrapped);
    if (view) {
      return view;
    }
    return fromObject(js::CheckedUnwrapStatic(maybeWrapped));
  }

  // Return a pointer to the start of the data referenced by a typed array. The
  // data is still owned by the typed array, and should not be modified on
  // another thread. Furthermore, the pointer can become invalid on GC (if the
  // data is small and fits inside the array's GC header), so callers must take
  // care not to hold on across anything that could GC.
  //
  // |obj| must have passed a JS_Is*Array test, or somehow be known that it
  // would pass such a test: it is a typed array or a wrapper of a typed array,
  // and the unwrapping will succeed.
  //
  // |*isSharedMemory| will be set to true if the typed array maps a
  // SharedArrayBuffer, otherwise to false.
  //
  mozilla::Span<DataType> getData(bool* isSharedMemory,
                                  const JS::AutoRequireNoGC& nogc);
};

ArrayBufferOrView ArrayBufferOrView::fromObject(JSObject* unwrapped) {
  if (ArrayBuffer::fromObject(unwrapped) ||
      ArrayBufferView::fromObject(unwrapped)) {
    return ArrayBufferOrView(unwrapped);
  }
  return ArrayBufferOrView(nullptr);
}

ArrayBufferView ArrayBufferView::fromObject(JSObject* unwrapped) {
  if (TypedArray_base::fromObject(unwrapped) ||
      DataView::fromObject(unwrapped)) {
    return ArrayBufferView(unwrapped);
  }
  return ArrayBufferView(nullptr);
}

} /* namespace JS */

/*
 * JS_Get(type)ArrayData(JSObject* obj,
 *                       bool* isSharedMemory,
 *                       const JS::AutoRequireNoGC&)
 *
 * js::Get(type)ArrayLengthAndData(JSObject* obj,
 *                                 size_t* length,
 *                                 bool* isSharedMemory,
 *                                 const JS::AutoRequireNoGC&)
 *
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

#define JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(ExternalType, NativeType, Name) \
  extern JS_PUBLIC_API ExternalType* JS_Get##Name##ArrayData(              \
      JSObject* maybeWrapped, bool* isSharedMemory,                        \
      const JS::AutoRequireNoGC&);                                         \
                                                                           \
  namespace js {                                                           \
  inline void Get##Name##ArrayLengthAndData(JSObject* unwrapped,           \
                                            size_t* length,                \
                                            bool* isSharedMemory,          \
                                            ExternalType** data) {         \
    MOZ_ASSERT(JS::TypedArray<JS::Scalar::Name>::fromObject(unwrapped));   \
    const JS::Value& lenSlot =                                             \
        JS::GetReservedSlot(unwrapped, detail::TypedArrayLengthSlot);      \
    *length = size_t(lenSlot.toPrivate());                                 \
    *isSharedMemory = JS_GetTypedArraySharedness(unwrapped);               \
    *data = JS::GetMaybePtrFromReservedSlot<ExternalType>(                 \
        unwrapped, detail::TypedArrayDataSlot);                            \
  }                                                                        \
                                                                           \
  JS_PUBLIC_API JSObject* Unwrap##Name##Array(JSObject* maybeWrapped);     \
  } /* namespace js */

JS_FOR_EACH_TYPED_ARRAY(JS_DEFINE_DATA_AND_LENGTH_ACCESSOR)
#undef JS_DEFINE_DATA_AND_LENGTH_ACCESSOR

namespace JS {

#define IMPL_TYPED_ARRAY_CLASS(ExternalType, NativeType, Name)                \
  template <>                                                                 \
  inline JS::TypedArray<JS::Scalar::Name>                                     \
  JS::TypedArray<JS::Scalar::Name>::create(JSContext* cx, size_t nelements) { \
    return fromObject(JS_New##Name##Array(cx, nelements));                    \
  };                                                                          \
                                                                              \
  template <>                                                                 \
  inline JS::TypedArray<JS::Scalar::Name>                                     \
  JS::TypedArray<JS::Scalar::Name>::fromArray(JSContext* cx,                  \
                                              HandleObject other) {           \
    return fromObject(JS_New##Name##ArrayFromArray(cx, other));               \
  };                                                                          \
                                                                              \
  template <>                                                                 \
  inline JS::TypedArray<JS::Scalar::Name>                                     \
  JS::TypedArray<JS::Scalar::Name>::fromBuffer(                               \
      JSContext* cx, HandleObject arrayBuffer, size_t byteOffset,             \
      int64_t length) {                                                       \
    return fromObject(                                                        \
        JS_New##Name##ArrayWithBuffer(cx, arrayBuffer, byteOffset, length));  \
  };

JS_FOR_EACH_TYPED_ARRAY(IMPL_TYPED_ARRAY_CLASS)
#undef IMPL_TYPED_ARRAY_CLASS

// Create simple names like Int8Array, Float32Array, etc.
#define JS_DECLARE_CLASS_ALIAS(ExternalType, NativeType, Name) \
  using Name##Array = TypedArray<js::Scalar::Name>;
JS_FOR_EACH_TYPED_ARRAY(JS_DECLARE_CLASS_ALIAS)
#undef JS_DECLARE_CLASS_ALIAS

}  // namespace JS

namespace js {

template <typename T>
using EnableIfABOVType =
    std::enable_if_t<std::is_base_of_v<JS::ArrayBufferOrView, T>>;

template <typename T, typename Wrapper>
class WrappedPtrOperations<T, Wrapper, EnableIfABOVType<T>> {
  auto get() const { return static_cast<const Wrapper*>(this)->get(); }

 public:
  explicit operator bool() const { return bool(get()); }
  JSObject* asObject() const { return get().asObject(); }
  bool isDetached() const { return get().isDetached(); }
  bool isSharedMemory() const { return get().isSharedMemory(); }

  mozilla::Span<typename T::DataType> getData(bool* isSharedMemory,
                                              const JS::AutoRequireNoGC& nogc) {
    return get().getData(isSharedMemory, nogc);
  }
};

// Allow usage within Heap<T>.
template <typename T>
struct IsHeapConstructibleType<T, EnableIfABOVType<T>> : public std::true_type {
};

template <typename T>
struct BarrierMethods<T, EnableIfABOVType<T>> {
  static gc::Cell* asGCThingOrNull(T view) {
    return reinterpret_cast<gc::Cell*>(view.asObjectUnbarriered());
  }
  static void postWriteBarrier(T* viewp, T prev, T next) {
    BarrierMethods<JSObject*>::postWriteBarrier(viewp->addressOfObject(),
                                                prev.asObjectUnbarriered(),
                                                next.asObjectUnbarriered());
  }
  static void exposeToJS(T view) { view.exposeToActiveJS(); }
  static void readBarrier(T view) {
    JSObject* obj = view.asObjectUnbarriered();
    if (obj) {
      js::gc::IncrementalReadBarrier(JS::GCCellPtr(obj));
    }
  }
};

}  // namespace js

namespace JS {
template <typename T>
struct SafelyInitialized<T, js::EnableIfABOVType<T>> {
  static T create() { return T::fromObject(nullptr); }
};
}  // namespace JS

/*
 * JS_Is(type)Array(JSObject* maybeWrapped)
 *
 * Test for specific typed array types.
 */

#define DECLARE_IS_ARRAY_TEST(_1, _2, Name)                                   \
  inline JS_PUBLIC_API bool JS_Is##Name##Array(JSObject* maybeWrapped) {      \
    return JS::TypedArray<js::Scalar::Name>::unwrap(maybeWrapped).asObject(); \
  }
JS_FOR_EACH_TYPED_ARRAY(DECLARE_IS_ARRAY_TEST)
#undef DECLARE_IS_ARRAY_TEST

#endif  // js_experimental_TypedData_h
