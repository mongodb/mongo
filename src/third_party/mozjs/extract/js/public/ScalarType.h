/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* An enumeration of all possible element types in typed data. */

#ifndef js_ScalarType_h
#define js_ScalarType_h

#include "mozilla/Assertions.h"  // MOZ_CRASH

#include <stddef.h>  // size_t

namespace JS {

namespace Scalar {

// Scalar types that can appear in typed arrays.
// The enum values must be kept in sync with:
//
//  * the TYPEDARRAY_KIND constants
//  * the SCTAG_TYPED_ARRAY constants
//  * JS_FOR_EACH_TYPED_ARRAY
//  * JS_FOR_PROTOTYPES_
//  * JS_FOR_EACH_UNIQUE_SCALAR_TYPE_REPR_CTYPE
//  * JIT compilation
//
// and the existing entries here must not be renumbered, since they are
// necessary for backwards compatibility with structured clones from previous
// versions. (It is fine to add new entries and increment
// MaxTypedArrayViewType, or change anything at or after
// MaxTypedArrayViewType.)
enum Type {
  Int8 = 0,
  Uint8,
  Int16,
  Uint16,
  Int32,
  Uint32,
  Float32,
  Float64,

  /**
   * Special type that is a uint8_t, but assignments are clamped to [0, 256).
   * Treat the raw data type as a uint8_t.
   */
  Uint8Clamped,

  BigInt64,
  BigUint64,

  /**
   * Types that don't have their own TypedArray equivalent, for now.
   * E.g. DataView
   */
  MaxTypedArrayViewType,

  Int64,
  Simd128,
};

static inline size_t byteSize(Type atype) {
  switch (atype) {
    case Int8:
    case Uint8:
    case Uint8Clamped:
      return 1;
    case Int16:
    case Uint16:
      return 2;
    case Int32:
    case Uint32:
    case Float32:
      return 4;
    case Int64:
    case Float64:
    case BigInt64:
    case BigUint64:
      return 8;
    case Simd128:
      return 16;
    case MaxTypedArrayViewType:
      break;
  }
  MOZ_CRASH("invalid scalar type");
}

static inline bool isSignedIntType(Type atype) {
  switch (atype) {
    case Int8:
    case Int16:
    case Int32:
    case Int64:
    case BigInt64:
      return true;
    case Uint8:
    case Uint8Clamped:
    case Uint16:
    case Uint32:
    case Float32:
    case Float64:
    case BigUint64:
    case Simd128:
      return false;
    case MaxTypedArrayViewType:
      break;
  }
  MOZ_CRASH("invalid scalar type");
}

static inline bool isBigIntType(Type atype) {
  switch (atype) {
    case BigInt64:
    case BigUint64:
      return true;
    case Int8:
    case Int16:
    case Int32:
    case Int64:
    case Uint8:
    case Uint8Clamped:
    case Uint16:
    case Uint32:
    case Float32:
    case Float64:
    case Simd128:
      return false;
    case MaxTypedArrayViewType:
      break;
  }
  MOZ_CRASH("invalid scalar type");
}

static inline bool isFloatingType(Type atype) {
  switch (atype) {
    case Int8:
    case Uint8:
    case Uint8Clamped:
    case Int16:
    case Uint16:
    case Int32:
    case Uint32:
    case Int64:
    case BigInt64:
    case BigUint64:
      return false;
    case Float32:
    case Float64:
    case Simd128:
      return true;
    case MaxTypedArrayViewType:
      break;
  }
  MOZ_CRASH("invalid scalar type");
}

static inline const char* name(Type atype) {
  switch (atype) {
    case Int8:
      return "Int8";
    case Uint8:
      return "Uint8";
    case Int16:
      return "Int16";
    case Uint16:
      return "Uint16";
    case Int32:
      return "Int32";
    case Uint32:
      return "Uint32";
    case Float32:
      return "Float32";
    case Float64:
      return "Float64";
    case Uint8Clamped:
      return "Uint8Clamped";
    case BigInt64:
      return "BigInt64";
    case BigUint64:
      return "BigUint64";
    case MaxTypedArrayViewType:
      return "MaxTypedArrayViewType";
    case Int64:
      return "Int64";
    case Simd128:
      return "Simd128";
  }
  MOZ_CRASH("invalid scalar type");
}

static inline const char* byteSizeString(Type atype) {
  switch (atype) {
    case Int8:
    case Uint8:
    case Uint8Clamped:
      return "1";
    case Int16:
    case Uint16:
      return "2";
    case Int32:
    case Uint32:
    case Float32:
      return "4";
    case Int64:
    case Float64:
    case BigInt64:
    case BigUint64:
      return "8";
    case Simd128:
      return "16";
    case MaxTypedArrayViewType:
      break;
  }
  MOZ_CRASH("invalid scalar type");
}

}  // namespace Scalar

}  // namespace JS

namespace js {

// This is aliased in NamespaceImports.h, but that is internal-only and
// inaccessible to Gecko code, which uses this type fairly heavily. Until such
// uses are changed, we need the alias here as well.
namespace Scalar = JS::Scalar;

}  // namespace js

#endif  // js_ScalarType_h
