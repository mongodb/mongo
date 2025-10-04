/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Array-inl.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/SIMD.h"
#include "mozilla/TextUtils.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "jsfriendapi.h"
#include "jsnum.h"
#include "jstypes.h"

#include "builtin/SelfHostingDefines.h"
#include "ds/Sort.h"
#include "jit/InlinableNatives.h"
#include "jit/TrampolineNatives.h"
#include "js/Class.h"
#include "js/Conversions.h"
#include "js/experimental/JitInfo.h"  // JSJitGetterOp, JSJitInfo
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "util/Poison.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/ArgumentsObject.h"
#include "vm/EqualityOperations.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "vm/ToSource.h"  // js::ValueToSource
#include "vm/TypedArrayObject.h"
#include "vm/WrapperObject.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "vm/TupleType.h"
#endif

#include "builtin/Sorting-inl.h"
#include "vm/ArgumentsObject-inl.h"
#include "vm/ArrayObject-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/IsGivenTypeObject-inl.h"
#include "vm/JSAtomUtils-inl.h"  // PrimitiveValueToId, IndexToId
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::Abs;
using mozilla::CeilingLog2;
using mozilla::CheckedInt;
using mozilla::DebugOnly;
using mozilla::IsAsciiDigit;
using mozilla::Maybe;
using mozilla::SIMD;

using JS::AutoCheckCannotGC;
using JS::IsArrayAnswer;
using JS::ToUint32;

bool js::ObjectMayHaveExtraIndexedOwnProperties(JSObject* obj) {
  if (!obj->is<NativeObject>()) {
    return true;
  }

  if (obj->as<NativeObject>().isIndexed()) {
    return true;
  }

  if (obj->is<TypedArrayObject>()) {
    return true;
  }

  return ClassMayResolveId(*obj->runtimeFromAnyThread()->commonNames,
                           obj->getClass(), PropertyKey::Int(0), obj);
}

bool js::PrototypeMayHaveIndexedProperties(NativeObject* obj) {
  do {
    MOZ_ASSERT(obj->hasStaticPrototype(),
               "dynamic-prototype objects must be non-native");

    JSObject* proto = obj->staticPrototype();
    if (!proto) {
      return false;  // no extra indexed properties found
    }

    if (ObjectMayHaveExtraIndexedOwnProperties(proto)) {
      return true;
    }
    obj = &proto->as<NativeObject>();
    if (obj->getDenseInitializedLength() != 0) {
      return true;
    }
  } while (true);
}

/*
 * Whether obj may have indexed properties anywhere besides its dense
 * elements. This includes other indexed properties in its shape hierarchy, and
 * indexed properties or elements along its prototype chain.
 */
bool js::ObjectMayHaveExtraIndexedProperties(JSObject* obj) {
  MOZ_ASSERT_IF(obj->hasDynamicPrototype(), !obj->is<NativeObject>());

  if (ObjectMayHaveExtraIndexedOwnProperties(obj)) {
    return true;
  }

  return PrototypeMayHaveIndexedProperties(&obj->as<NativeObject>());
}

bool JS::IsArray(JSContext* cx, HandleObject obj, IsArrayAnswer* answer) {
  if (obj->is<ArrayObject>()) {
    *answer = IsArrayAnswer::Array;
    return true;
  }

  if (obj->is<ProxyObject>()) {
    return Proxy::isArray(cx, obj, answer);
  }

  *answer = IsArrayAnswer::NotArray;
  return true;
}

bool JS::IsArray(JSContext* cx, HandleObject obj, bool* isArray) {
  IsArrayAnswer answer;
  if (!IsArray(cx, obj, &answer)) {
    return false;
  }

  if (answer == IsArrayAnswer::RevokedProxy) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  *isArray = answer == IsArrayAnswer::Array;
  return true;
}

bool js::IsArrayFromJit(JSContext* cx, HandleObject obj, bool* isArray) {
  return JS::IsArray(cx, obj, isArray);
}

// ES2017 7.1.15 ToLength.
bool js::ToLength(JSContext* cx, HandleValue v, uint64_t* out) {
  if (v.isInt32()) {
    int32_t i = v.toInt32();
    *out = i < 0 ? 0 : i;
    return true;
  }

  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else {
    if (!ToNumber(cx, v, &d)) {
      return false;
    }
  }

  d = JS::ToInteger(d);
  if (d <= 0.0) {
    *out = 0;
  } else {
    *out = uint64_t(std::min(d, DOUBLE_INTEGRAL_PRECISION_LIMIT - 1));
  }
  return true;
}

bool js::GetLengthProperty(JSContext* cx, HandleObject obj, uint64_t* lengthp) {
  if (obj->is<ArrayObject>()) {
    *lengthp = obj->as<ArrayObject>().length();
    return true;
  }

  if (obj->is<ArgumentsObject>()) {
    ArgumentsObject& argsobj = obj->as<ArgumentsObject>();
    if (!argsobj.hasOverriddenLength()) {
      *lengthp = argsobj.initialLength();
      return true;
    }
  }

  RootedValue value(cx);
  if (!GetProperty(cx, obj, obj, cx->names().length, &value)) {
    return false;
  }

  return ToLength(cx, value, lengthp);
}

// Fast path for array functions where the object is expected to be an array.
static MOZ_ALWAYS_INLINE bool GetLengthPropertyInlined(JSContext* cx,
                                                       HandleObject obj,
                                                       uint64_t* lengthp) {
  if (obj->is<ArrayObject>()) {
    *lengthp = obj->as<ArrayObject>().length();
    return true;
  }

  return GetLengthProperty(cx, obj, lengthp);
}

/*
 * Determine if the id represents an array index.
 *
 * An id is an array index according to ECMA by (15.4):
 *
 * "Array objects give special treatment to a certain class of property names.
 * A property name P (in the form of a string value) is an array index if and
 * only if ToString(ToUint32(P)) is equal to P and ToUint32(P) is not equal
 * to 2^32-1."
 *
 * This means the largest allowed index is actually 2^32-2 (4294967294).
 *
 * In our implementation, it would be sufficient to check for id.isInt32()
 * except that by using signed 31-bit integers we miss the top half of the
 * valid range. This function checks the string representation itself; note
 * that calling a standard conversion routine might allow strings such as
 * "08" or "4.0" as array indices, which they are not.
 *
 */
JS_PUBLIC_API bool js::StringIsArrayIndex(JSLinearString* str,
                                          uint32_t* indexp) {
  if (!str->isIndex(indexp)) {
    return false;
  }
  MOZ_ASSERT(*indexp <= MAX_ARRAY_INDEX);
  return true;
}

JS_PUBLIC_API bool js::StringIsArrayIndex(const char16_t* str, uint32_t length,
                                          uint32_t* indexp) {
  if (length == 0 || length > UINT32_CHAR_BUFFER_LENGTH) {
    return false;
  }
  if (!mozilla::IsAsciiDigit(str[0])) {
    return false;
  }
  if (!CheckStringIsIndex(str, length, indexp)) {
    return false;
  }
  MOZ_ASSERT(*indexp <= MAX_ARRAY_INDEX);
  return true;
}

template <typename T>
static bool ToId(JSContext* cx, T index, MutableHandleId id);

template <>
bool ToId(JSContext* cx, uint32_t index, MutableHandleId id) {
  return IndexToId(cx, index, id);
}

template <>
bool ToId(JSContext* cx, uint64_t index, MutableHandleId id) {
  MOZ_ASSERT(index < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));

  if (index == uint32_t(index)) {
    return IndexToId(cx, uint32_t(index), id);
  }

  Value tmp = DoubleValue(index);
  return PrimitiveValueToId<CanGC>(cx, HandleValue::fromMarkedLocation(&tmp),
                                   id);
}

/*
 * If the property at the given index exists, get its value into |vp| and set
 * |*hole| to false. Otherwise set |*hole| to true and |vp| to Undefined.
 */
template <typename T>
static bool HasAndGetElement(JSContext* cx, HandleObject obj,
                             HandleObject receiver, T index, bool* hole,
                             MutableHandleValue vp) {
  if (obj->is<NativeObject>()) {
    NativeObject* nobj = &obj->as<NativeObject>();
    if (index < nobj->getDenseInitializedLength()) {
      vp.set(nobj->getDenseElement(size_t(index)));
      if (!vp.isMagic(JS_ELEMENTS_HOLE)) {
        *hole = false;
        return true;
      }
    }
    if (nobj->is<ArgumentsObject>() && index <= UINT32_MAX) {
      if (nobj->as<ArgumentsObject>().maybeGetElement(uint32_t(index), vp)) {
        *hole = false;
        return true;
      }
    }
  }

  RootedId id(cx);
  if (!ToId(cx, index, &id)) {
    return false;
  }

  bool found;
  if (!HasProperty(cx, obj, id, &found)) {
    return false;
  }

  if (found) {
    if (!GetProperty(cx, obj, receiver, id, vp)) {
      return false;
    }
  } else {
    vp.setUndefined();
  }
  *hole = !found;
  return true;
}

template <typename T>
static inline bool HasAndGetElement(JSContext* cx, HandleObject obj, T index,
                                    bool* hole, MutableHandleValue vp) {
  return HasAndGetElement(cx, obj, obj, index, hole, vp);
}

bool ElementAdder::append(JSContext* cx, HandleValue v) {
  MOZ_ASSERT(index_ < length_);
  if (resObj_) {
    NativeObject* resObj = &resObj_->as<NativeObject>();
    DenseElementResult result =
        resObj->setOrExtendDenseElements(cx, index_, v.address(), 1);
    if (result == DenseElementResult::Failure) {
      return false;
    }
    if (result == DenseElementResult::Incomplete) {
      if (!DefineDataElement(cx, resObj_, index_, v)) {
        return false;
      }
    }
  } else {
    vp_[index_] = v;
  }
  index_++;
  return true;
}

void ElementAdder::appendHole() {
  MOZ_ASSERT(getBehavior_ == ElementAdder::CheckHasElemPreserveHoles);
  MOZ_ASSERT(index_ < length_);
  if (!resObj_) {
    vp_[index_].setMagic(JS_ELEMENTS_HOLE);
  }
  index_++;
}

bool js::GetElementsWithAdder(JSContext* cx, HandleObject obj,
                              HandleObject receiver, uint32_t begin,
                              uint32_t end, ElementAdder* adder) {
  MOZ_ASSERT(begin <= end);

  RootedValue val(cx);
  for (uint32_t i = begin; i < end; i++) {
    if (adder->getBehavior() == ElementAdder::CheckHasElemPreserveHoles) {
      bool hole;
      if (!HasAndGetElement(cx, obj, receiver, i, &hole, &val)) {
        return false;
      }
      if (hole) {
        adder->appendHole();
        continue;
      }
    } else {
      MOZ_ASSERT(adder->getBehavior() == ElementAdder::GetElement);
      if (!GetElement(cx, obj, receiver, i, &val)) {
        return false;
      }
    }
    if (!adder->append(cx, val)) {
      return false;
    }
  }

  return true;
}

static inline bool IsPackedArrayOrNoExtraIndexedProperties(JSObject* obj,
                                                           uint64_t length) {
  return (IsPackedArray(obj) && obj->as<ArrayObject>().length() == length) ||
         !ObjectMayHaveExtraIndexedProperties(obj);
}

static bool GetDenseElements(NativeObject* aobj, uint32_t length, Value* vp) {
  MOZ_ASSERT(IsPackedArrayOrNoExtraIndexedProperties(aobj, length));

  if (length > aobj->getDenseInitializedLength()) {
    return false;
  }

  for (size_t i = 0; i < length; i++) {
    vp[i] = aobj->getDenseElement(i);

    // No other indexed properties so hole => undefined.
    if (vp[i].isMagic(JS_ELEMENTS_HOLE)) {
      vp[i] = UndefinedValue();
    }
  }

  return true;
}

bool js::GetElements(JSContext* cx, HandleObject aobj, uint32_t length,
                     Value* vp) {
  if (IsPackedArrayOrNoExtraIndexedProperties(aobj, length)) {
    if (GetDenseElements(&aobj->as<NativeObject>(), length, vp)) {
      return true;
    }
  }

  if (aobj->is<ArgumentsObject>()) {
    ArgumentsObject& argsobj = aobj->as<ArgumentsObject>();
    if (!argsobj.hasOverriddenLength()) {
      if (argsobj.maybeGetElements(0, length, vp)) {
        return true;
      }
    }
  }

  if (aobj->is<TypedArrayObject>()) {
    Handle<TypedArrayObject*> typedArray = aobj.as<TypedArrayObject>();
    if (typedArray->length().valueOr(0) == length) {
      return TypedArrayObject::getElements(cx, typedArray, length, vp);
    }
  }

  if (js::GetElementsOp op = aobj->getOpsGetElements()) {
    ElementAdder adder(cx, vp, length, ElementAdder::GetElement);
    return op(cx, aobj, 0, length, &adder);
  }

  for (uint32_t i = 0; i < length; i++) {
    if (!GetElement(cx, aobj, aobj, i,
                    MutableHandleValue::fromMarkedLocation(&vp[i]))) {
      return false;
    }
  }

  return true;
}

static inline bool GetArrayElement(JSContext* cx, HandleObject obj,
                                   uint64_t index, MutableHandleValue vp) {
  if (obj->is<NativeObject>()) {
    NativeObject* nobj = &obj->as<NativeObject>();
    if (index < nobj->getDenseInitializedLength()) {
      vp.set(nobj->getDenseElement(size_t(index)));
      if (!vp.isMagic(JS_ELEMENTS_HOLE)) {
        return true;
      }
    }

    if (nobj->is<ArgumentsObject>() && index <= UINT32_MAX) {
      if (nobj->as<ArgumentsObject>().maybeGetElement(uint32_t(index), vp)) {
        return true;
      }
    }
  }

  RootedId id(cx);
  if (!ToId(cx, index, &id)) {
    return false;
  }
  return GetProperty(cx, obj, obj, id, vp);
}

static inline bool DefineArrayElement(JSContext* cx, HandleObject obj,
                                      uint64_t index, HandleValue value) {
  RootedId id(cx);
  if (!ToId(cx, index, &id)) {
    return false;
  }
  return DefineDataProperty(cx, obj, id, value);
}

// Set the value of the property at the given index to v.
static inline bool SetArrayElement(JSContext* cx, HandleObject obj,
                                   uint64_t index, HandleValue v) {
  RootedId id(cx);
  if (!ToId(cx, index, &id)) {
    return false;
  }

  return SetProperty(cx, obj, id, v);
}

/*
 * Attempt to delete the element |index| from |obj| as if by
 * |obj.[[Delete]](index)|.
 *
 * If an error occurs while attempting to delete the element (that is, the call
 * to [[Delete]] threw), return false.
 *
 * Otherwise call result.succeed() or result.fail() to indicate whether the
 * deletion attempt succeeded (that is, whether the call to [[Delete]] returned
 * true or false).  (Deletes generally fail only when the property is
 * non-configurable, but proxies may implement different semantics.)
 */
static bool DeleteArrayElement(JSContext* cx, HandleObject obj, uint64_t index,
                               ObjectOpResult& result) {
  if (obj->is<ArrayObject>() && !obj->as<NativeObject>().isIndexed() &&
      !obj->as<NativeObject>().denseElementsAreSealed()) {
    ArrayObject* aobj = &obj->as<ArrayObject>();
    if (index <= UINT32_MAX) {
      uint32_t idx = uint32_t(index);
      if (idx < aobj->getDenseInitializedLength()) {
        if (idx + 1 == aobj->getDenseInitializedLength()) {
          aobj->setDenseInitializedLengthMaybeNonExtensible(cx, idx);
        } else {
          aobj->setDenseElementHole(idx);
        }
        if (!SuppressDeletedElement(cx, obj, idx)) {
          return false;
        }
      }
    }

    return result.succeed();
  }

  RootedId id(cx);
  if (!ToId(cx, index, &id)) {
    return false;
  }
  return DeleteProperty(cx, obj, id, result);
}

/* ES6 draft rev 32 (2 Febr 2015) 7.3.7 */
static bool DeletePropertyOrThrow(JSContext* cx, HandleObject obj,
                                  uint64_t index) {
  ObjectOpResult success;
  if (!DeleteArrayElement(cx, obj, index, success)) {
    return false;
  }
  if (!success) {
    RootedId id(cx);
    if (!ToId(cx, index, &id)) {
      return false;
    }
    return success.reportError(cx, obj, id);
  }
  return true;
}

static bool DeletePropertiesOrThrow(JSContext* cx, HandleObject obj,
                                    uint64_t len, uint64_t finalLength) {
  if (obj->is<ArrayObject>() && !obj->as<NativeObject>().isIndexed() &&
      !obj->as<NativeObject>().denseElementsAreSealed()) {
    if (len <= UINT32_MAX) {
      // Skip forward to the initialized elements of this array.
      len = std::min(uint32_t(len),
                     obj->as<ArrayObject>().getDenseInitializedLength());
    }
  }

  for (uint64_t k = len; k > finalLength; k--) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    if (!DeletePropertyOrThrow(cx, obj, k - 1)) {
      return false;
    }
  }
  return true;
}

static bool SetArrayLengthProperty(JSContext* cx, Handle<ArrayObject*> obj,
                                   HandleValue value) {
  RootedId id(cx, NameToId(cx->names().length));
  ObjectOpResult result;
  if (obj->lengthIsWritable()) {
    Rooted<PropertyDescriptor> desc(
        cx, PropertyDescriptor::Data(value, JS::PropertyAttribute::Writable));
    if (!ArraySetLength(cx, obj, id, desc, result)) {
      return false;
    }
  } else {
    MOZ_ALWAYS_TRUE(result.fail(JSMSG_READ_ONLY));
  }
  return result.checkStrict(cx, obj, id);
}

static bool SetLengthProperty(JSContext* cx, HandleObject obj,
                              uint64_t length) {
  MOZ_ASSERT(length < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));

  RootedValue v(cx, NumberValue(length));
  if (obj->is<ArrayObject>()) {
    return SetArrayLengthProperty(cx, obj.as<ArrayObject>(), v);
  }
  return SetProperty(cx, obj, cx->names().length, v);
}

bool js::SetLengthProperty(JSContext* cx, HandleObject obj, uint32_t length) {
  RootedValue v(cx, NumberValue(length));
  if (obj->is<ArrayObject>()) {
    return SetArrayLengthProperty(cx, obj.as<ArrayObject>(), v);
  }
  return SetProperty(cx, obj, cx->names().length, v);
}

bool js::ArrayLengthGetter(JSContext* cx, HandleObject obj, HandleId id,
                           MutableHandleValue vp) {
  MOZ_ASSERT(id == NameToId(cx->names().length));

  vp.setNumber(obj->as<ArrayObject>().length());
  return true;
}

bool js::ArrayLengthSetter(JSContext* cx, HandleObject obj, HandleId id,
                           HandleValue v, ObjectOpResult& result) {
  MOZ_ASSERT(id == NameToId(cx->names().length));

  Handle<ArrayObject*> arr = obj.as<ArrayObject>();
  MOZ_ASSERT(arr->lengthIsWritable(),
             "setter shouldn't be called if property is non-writable");

  Rooted<PropertyDescriptor> desc(
      cx, PropertyDescriptor::Data(v, JS::PropertyAttribute::Writable));
  return ArraySetLength(cx, arr, id, desc, result);
}

struct ReverseIndexComparator {
  bool operator()(const uint32_t& a, const uint32_t& b, bool* lessOrEqualp) {
    MOZ_ASSERT(a != b, "how'd we get duplicate indexes?");
    *lessOrEqualp = b <= a;
    return true;
  }
};

/* ES6 draft rev 34 (2015 Feb 20) 9.4.2.4 ArraySetLength */
bool js::ArraySetLength(JSContext* cx, Handle<ArrayObject*> arr, HandleId id,
                        Handle<PropertyDescriptor> desc,
                        ObjectOpResult& result) {
  MOZ_ASSERT(id == NameToId(cx->names().length));
  MOZ_ASSERT(desc.isDataDescriptor() || desc.isGenericDescriptor());

  // Step 1.
  uint32_t newLen;
  if (!desc.hasValue()) {
    // The spec has us calling OrdinaryDefineOwnProperty if
    // Desc.[[Value]] is absent, but our implementation is so different that
    // this is impossible. Instead, set newLen to the current length and
    // proceed to step 9.
    newLen = arr->length();
  } else {
    // Step 2 is irrelevant in our implementation.

    // Step 3.
    if (!ToUint32(cx, desc.value(), &newLen)) {
      return false;
    }

    // Step 4.
    double d;
    if (!ToNumber(cx, desc.value(), &d)) {
      return false;
    }

    // Step 5.
    if (d != newLen) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_ARRAY_LENGTH);
      return false;
    }

    // Steps 6-8 are irrelevant in our implementation.
  }

  // Steps 9-11.
  bool lengthIsWritable = arr->lengthIsWritable();
#ifdef DEBUG
  {
    mozilla::Maybe<PropertyInfo> lengthProp = arr->lookupPure(id);
    MOZ_ASSERT(lengthProp.isSome());
    MOZ_ASSERT(lengthProp->writable() == lengthIsWritable);
  }
#endif
  uint32_t oldLen = arr->length();

  // Part of steps 1.a, 12.a, and 16: Fail if we're being asked to change
  // enumerability or configurability, or otherwise break the object
  // invariants. (ES6 checks these by calling OrdinaryDefineOwnProperty, but
  // in SM, the array length property is hardly ordinary.)
  if ((desc.hasConfigurable() && desc.configurable()) ||
      (desc.hasEnumerable() && desc.enumerable()) ||
      (!lengthIsWritable && desc.hasWritable() && desc.writable())) {
    return result.fail(JSMSG_CANT_REDEFINE_PROP);
  }

  // Steps 12-13 for arrays with non-writable length.
  if (!lengthIsWritable) {
    if (newLen == oldLen) {
      return result.succeed();
    }

    return result.fail(JSMSG_CANT_REDEFINE_ARRAY_LENGTH);
  }

  // Step 19.
  bool succeeded = true;
  do {
    // The initialized length and capacity of an array only need updating
    // when non-hole elements are added or removed, which doesn't happen
    // when array length stays the same or increases.
    if (newLen >= oldLen) {
      break;
    }

    // Attempt to propagate dense-element optimization tricks, if possible,
    // and avoid the generic (and accordingly slow) deletion code below.
    // We can only do this if there are only densely-indexed elements.
    // Once there's a sparse indexed element, there's no good way to know,
    // save by enumerating all the properties to find it.  But we *have* to
    // know in case that sparse indexed element is non-configurable, as
    // that element must prevent any deletions below it.  Bug 586842 should
    // fix this inefficiency by moving indexed storage to be entirely
    // separate from non-indexed storage.
    // A second reason for this optimization to be invalid is an active
    // for..in iteration over the array. Keys deleted before being reached
    // during the iteration must not be visited, and suppressing them here
    // would be too costly.
    // This optimization is also invalid when there are sealed
    // (non-configurable) elements.
    if (!arr->isIndexed() && !arr->denseElementsMaybeInIteration() &&
        !arr->denseElementsAreSealed()) {
      uint32_t oldCapacity = arr->getDenseCapacity();
      uint32_t oldInitializedLength = arr->getDenseInitializedLength();
      MOZ_ASSERT(oldCapacity >= oldInitializedLength);
      if (oldInitializedLength > newLen) {
        arr->setDenseInitializedLengthMaybeNonExtensible(cx, newLen);
      }
      if (oldCapacity > newLen) {
        if (arr->isExtensible()) {
          arr->shrinkElements(cx, newLen);
        } else {
          MOZ_ASSERT(arr->getDenseInitializedLength() ==
                     arr->getDenseCapacity());
        }
      }

      // We've done the work of deleting any dense elements needing
      // deletion, and there are no sparse elements.  Thus we can skip
      // straight to defining the length.
      break;
    }

    // Step 15.
    //
    // Attempt to delete all elements above the new length, from greatest
    // to least.  If any of these deletions fails, we're supposed to define
    // the length to one greater than the index that couldn't be deleted,
    // *with the property attributes specified*.  This might convert the
    // length to be not the value specified, yet non-writable.  (You may be
    // forgiven for thinking these are interesting semantics.)  Example:
    //
    //   var arr =
    //     Object.defineProperty([0, 1, 2, 3], 1, { writable: false });
    //   Object.defineProperty(arr, "length",
    //                         { value: 0, writable: false });
    //
    // will convert |arr| to an array of non-writable length two, then
    // throw a TypeError.
    //
    // We implement this behavior, in the relevant lops below, by setting
    // |succeeded| to false.  Then we exit the loop, define the length
    // appropriately, and only then throw a TypeError, if necessary.
    uint32_t gap = oldLen - newLen;
    const uint32_t RemoveElementsFastLimit = 1 << 24;
    if (gap < RemoveElementsFastLimit) {
      // If we're removing a relatively small number of elements, just do
      // it exactly by the spec.
      while (newLen < oldLen) {
        // Step 15a.
        oldLen--;

        // Steps 15b-d.
        ObjectOpResult deleteSucceeded;
        if (!DeleteElement(cx, arr, oldLen, deleteSucceeded)) {
          return false;
        }
        if (!deleteSucceeded) {
          newLen = oldLen + 1;
          succeeded = false;
          break;
        }
      }
    } else {
      // If we're removing a large number of elements from an array
      // that's probably sparse, try a different tack.  Get all the own
      // property names, sift out the indexes in the deletion range into
      // a vector, sort the vector greatest to least, then delete the
      // indexes greatest to least using that vector.  See bug 322135.
      //
      // This heuristic's kind of a huge guess -- "large number of
      // elements" and "probably sparse" are completely unprincipled
      // predictions.  In the long run, bug 586842 will support the right
      // fix: store sparse elements in a sorted data structure that
      // permits fast in-reverse-order traversal and concurrent removals.

      Vector<uint32_t> indexes(cx);
      {
        RootedIdVector props(cx);
        if (!GetPropertyKeys(cx, arr, JSITER_OWNONLY | JSITER_HIDDEN, &props)) {
          return false;
        }

        for (size_t i = 0; i < props.length(); i++) {
          if (!CheckForInterrupt(cx)) {
            return false;
          }

          uint32_t index;
          if (!IdIsIndex(props[i], &index)) {
            continue;
          }

          if (index >= newLen && index < oldLen) {
            if (!indexes.append(index)) {
              return false;
            }
          }
        }
      }

      uint32_t count = indexes.length();
      {
        // We should use radix sort to be O(n), but this is uncommon
        // enough that we'll punt til someone complains.
        Vector<uint32_t> scratch(cx);
        if (!scratch.resize(count)) {
          return false;
        }
        MOZ_ALWAYS_TRUE(MergeSort(indexes.begin(), count, scratch.begin(),
                                  ReverseIndexComparator()));
      }

      uint32_t index = UINT32_MAX;
      for (uint32_t i = 0; i < count; i++) {
        MOZ_ASSERT(indexes[i] < index, "indexes should never repeat");
        index = indexes[i];

        // Steps 15b-d.
        ObjectOpResult deleteSucceeded;
        if (!DeleteElement(cx, arr, index, deleteSucceeded)) {
          return false;
        }
        if (!deleteSucceeded) {
          newLen = index + 1;
          succeeded = false;
          break;
        }
      }
    }
  } while (false);

  // Update array length. Technically we should have been doing this
  // throughout the loop, in step 19.d.iii.
  arr->setLength(newLen);

  // Step 20.
  if (desc.hasWritable() && !desc.writable()) {
    Maybe<PropertyInfo> lengthProp = arr->lookup(cx, id);
    MOZ_ASSERT(lengthProp.isSome());
    MOZ_ASSERT(lengthProp->isCustomDataProperty());
    PropertyFlags flags = lengthProp->flags();
    flags.clearFlag(PropertyFlag::Writable);
    if (!NativeObject::changeCustomDataPropAttributes(cx, arr, id, flags)) {
      return false;
    }
  }

  // All operations past here until the |!succeeded| code must be infallible,
  // so that all element fields remain properly synchronized.

  // Trim the initialized length, if needed, to preserve the <= length
  // invariant.  (Capacity was already reduced during element deletion, if
  // necessary.)
  ObjectElements* header = arr->getElementsHeader();
  header->initializedLength = std::min(header->initializedLength, newLen);

  if (!arr->isExtensible()) {
    arr->shrinkCapacityToInitializedLength(cx);
  }

  if (desc.hasWritable() && !desc.writable()) {
    arr->setNonWritableLength(cx);
  }

  if (!succeeded) {
    return result.fail(JSMSG_CANT_TRUNCATE_ARRAY);
  }

  return result.succeed();
}

static bool array_addProperty(JSContext* cx, HandleObject obj, HandleId id,
                              HandleValue v) {
  ArrayObject* arr = &obj->as<ArrayObject>();

  uint32_t index;
  if (!IdIsIndex(id, &index)) {
    return true;
  }

  uint32_t length = arr->length();
  if (index >= length) {
    MOZ_ASSERT(arr->lengthIsWritable(),
               "how'd this element get added if length is non-writable?");
    arr->setLength(index + 1);
  }
  return true;
}

static SharedShape* AddLengthProperty(JSContext* cx,
                                      Handle<SharedShape*> shape) {
  // Add the 'length' property for a newly created array shape.

  MOZ_ASSERT(shape->propMapLength() == 0);
  MOZ_ASSERT(shape->getObjectClass() == &ArrayObject::class_);

  RootedId lengthId(cx, NameToId(cx->names().length));
  constexpr PropertyFlags flags = {PropertyFlag::CustomDataProperty,
                                   PropertyFlag::Writable};

  Rooted<SharedPropMap*> map(cx, shape->propMap());
  uint32_t mapLength = shape->propMapLength();
  ObjectFlags objectFlags = shape->objectFlags();

  if (!SharedPropMap::addCustomDataProperty(cx, &ArrayObject::class_, &map,
                                            &mapLength, lengthId, flags,
                                            &objectFlags)) {
    return nullptr;
  }

  return SharedShape::getPropMapShape(cx, shape->base(), shape->numFixedSlots(),
                                      map, mapLength, objectFlags);
}

bool js::IsArrayConstructor(const JSObject* obj) {
  // Note: this also returns true for cross-realm Array constructors in the
  // same compartment.
  return IsNativeFunction(obj, ArrayConstructor);
}

static bool IsArrayConstructor(const Value& v) {
  return v.isObject() && IsArrayConstructor(&v.toObject());
}

bool js::IsCrossRealmArrayConstructor(JSContext* cx, JSObject* obj,
                                      bool* result) {
  if (obj->is<WrapperObject>()) {
    obj = CheckedUnwrapDynamic(obj, cx);
    if (!obj) {
      ReportAccessDenied(cx);
      return false;
    }
  }

  *result =
      IsArrayConstructor(obj) && obj->as<JSFunction>().realm() != cx->realm();
  return true;
}

// Returns true iff we know for -sure- that it is definitely safe to use the
// realm's array constructor.
//
// This function is conservative as it may return false for cases which
// ultimately do use the array constructor.
static MOZ_ALWAYS_INLINE bool IsArraySpecies(JSContext* cx,
                                             HandleObject origArray) {
  if (MOZ_UNLIKELY(origArray->is<ProxyObject>())) {
    if (origArray->getClass()->isDOMClass()) {
#ifdef DEBUG
      // We assume DOM proxies never return true for IsArray.
      IsArrayAnswer answer;
      MOZ_ASSERT(Proxy::isArray(cx, origArray, &answer));
      MOZ_ASSERT(answer == IsArrayAnswer::NotArray);
#endif
      return true;
    }
    return false;
  }

  // 9.4.2.3 Step 4. Non-array objects always use the default constructor.
  if (!origArray->is<ArrayObject>()) {
    return true;
  }

  if (cx->realm()->arraySpeciesLookup.tryOptimizeArray(
          cx, &origArray->as<ArrayObject>())) {
    return true;
  }

  Value ctor;
  if (!GetPropertyPure(cx, origArray, NameToId(cx->names().constructor),
                       &ctor)) {
    return false;
  }

  if (!IsArrayConstructor(ctor)) {
    return ctor.isUndefined();
  }

  // 9.4.2.3 Step 6.c. Use the current realm's constructor if |ctor| is a
  // cross-realm Array constructor.
  if (cx->realm() != ctor.toObject().as<JSFunction>().realm()) {
    return true;
  }

  jsid speciesId = PropertyKey::Symbol(cx->wellKnownSymbols().species);
  JSFunction* getter;
  if (!GetGetterPure(cx, &ctor.toObject(), speciesId, &getter)) {
    return false;
  }

  if (!getter) {
    return false;
  }

  return IsSelfHostedFunctionWithName(getter, cx->names().dollar_ArraySpecies_);
}

static bool ArraySpeciesCreate(JSContext* cx, HandleObject origArray,
                               uint64_t length, MutableHandleObject arr) {
  MOZ_ASSERT(length < DOUBLE_INTEGRAL_PRECISION_LIMIT);

  FixedInvokeArgs<2> args(cx);

  args[0].setObject(*origArray);
  args[1].set(NumberValue(length));

  RootedValue rval(cx);
  if (!CallSelfHostedFunction(cx, cx->names().ArraySpeciesCreate,
                              UndefinedHandleValue, args, &rval)) {
    return false;
  }

  MOZ_ASSERT(rval.isObject());
  arr.set(&rval.toObject());
  return true;
}

JSString* js::ArrayToSource(JSContext* cx, HandleObject obj) {
  AutoCycleDetector detector(cx, obj);
  if (!detector.init()) {
    return nullptr;
  }

  JSStringBuilder sb(cx);

  if (detector.foundCycle()) {
    if (!sb.append("[]")) {
      return nullptr;
    }
    return sb.finishString();
  }

  if (!sb.append('[')) {
    return nullptr;
  }

  uint64_t length;
  if (!GetLengthPropertyInlined(cx, obj, &length)) {
    return nullptr;
  }

  RootedValue elt(cx);
  for (uint64_t index = 0; index < length; index++) {
    bool hole;
    if (!CheckForInterrupt(cx) ||
        !HasAndGetElement(cx, obj, index, &hole, &elt)) {
      return nullptr;
    }

    /* Get element's character string. */
    JSString* str;
    if (hole) {
      str = cx->runtime()->emptyString;
    } else {
      str = ValueToSource(cx, elt);
      if (!str) {
        return nullptr;
      }
    }

    /* Append element to buffer. */
    if (!sb.append(str)) {
      return nullptr;
    }
    if (index + 1 != length) {
      if (!sb.append(", ")) {
        return nullptr;
      }
    } else if (hole) {
      if (!sb.append(',')) {
        return nullptr;
      }
    }
  }

  /* Finalize the buffer. */
  if (!sb.append(']')) {
    return nullptr;
  }

  return sb.finishString();
}

static bool array_toSource(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "toSource");
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.thisv().isObject()) {
    ReportIncompatible(cx, args);
    return false;
  }

  Rooted<JSObject*> obj(cx, &args.thisv().toObject());

  JSString* str = ArrayToSource(cx, obj);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

template <typename SeparatorOp>
static bool ArrayJoinDenseKernel(JSContext* cx, SeparatorOp sepOp,
                                 Handle<NativeObject*> obj, uint64_t length,
                                 StringBuffer& sb, uint32_t* numProcessed) {
  // This loop handles all elements up to initializedLength. If
  // length > initLength we rely on the second loop to add the
  // other elements.
  MOZ_ASSERT(*numProcessed == 0);
  uint64_t initLength =
      std::min<uint64_t>(obj->getDenseInitializedLength(), length);
  MOZ_ASSERT(initLength <= UINT32_MAX,
             "initialized length shouldn't exceed UINT32_MAX");
  uint32_t initLengthClamped = uint32_t(initLength);
  while (*numProcessed < initLengthClamped) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    // Step 7.b.
    Value elem = obj->getDenseElement(*numProcessed);

    // Steps 7.c-d.
    if (elem.isString()) {
      if (!sb.append(elem.toString())) {
        return false;
      }
    } else if (elem.isNumber()) {
      if (!NumberValueToStringBuffer(elem, sb)) {
        return false;
      }
    } else if (elem.isBoolean()) {
      if (!BooleanToStringBuffer(elem.toBoolean(), sb)) {
        return false;
      }
    } else if (elem.isObject() || elem.isSymbol()) {
      /*
       * Object stringifying could modify the initialized length or make
       * the array sparse. Delegate it to a separate loop to keep this
       * one tight.
       *
       * Symbol stringifying is a TypeError, so into the slow path
       * with those as well.
       */
      break;
    } else if (elem.isBigInt()) {
      // ToString(bigint) doesn't access bigint.toString or
      // anything like that, so it can't mutate the array we're
      // walking through, so it *could* be handled here. We don't
      // do so yet for reasons of initial-implementation economy.
      break;
    } else {
      MOZ_ASSERT(elem.isMagic(JS_ELEMENTS_HOLE) || elem.isNullOrUndefined());
    }

    // Steps 7.a, 7.e.
    if (++(*numProcessed) != length && !sepOp(sb)) {
      return false;
    }
  }

  return true;
}

template <typename SeparatorOp>
static bool ArrayJoinKernel(JSContext* cx, SeparatorOp sepOp, HandleObject obj,
                            uint64_t length, StringBuffer& sb) {
  // Step 6.
  uint32_t numProcessed = 0;

  if (IsPackedArrayOrNoExtraIndexedProperties(obj, length)) {
    if (!ArrayJoinDenseKernel<SeparatorOp>(cx, sepOp, obj.as<NativeObject>(),
                                           length, sb, &numProcessed)) {
      return false;
    }
  }

  // Step 7.
  if (numProcessed != length) {
    RootedValue v(cx);
    for (uint64_t i = numProcessed; i < length;) {
      if (!CheckForInterrupt(cx)) {
        return false;
      }

      // Step 7.b.
      if (!GetArrayElement(cx, obj, i, &v)) {
        return false;
      }

      // Steps 7.c-d.
      if (!v.isNullOrUndefined()) {
        if (!ValueToStringBuffer(cx, v, sb)) {
          return false;
        }
      }

      // Steps 7.a, 7.e.
      if (++i != length && !sepOp(sb)) {
        return false;
      }
    }
  }

  return true;
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.13 Array.prototype.join ( separator )
bool js::array_join(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "join");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  AutoCycleDetector detector(cx, obj);
  if (!detector.init()) {
    return false;
  }

  if (detector.foundCycle()) {
    args.rval().setString(cx->names().empty_);
    return true;
  }

  // Step 2.
  uint64_t length;
  if (!GetLengthPropertyInlined(cx, obj, &length)) {
    return false;
  }

  // Steps 3-4.
  Rooted<JSLinearString*> sepstr(cx);
  if (args.hasDefined(0)) {
    JSString* s = ToString<CanGC>(cx, args[0]);
    if (!s) {
      return false;
    }
    sepstr = s->ensureLinear(cx);
    if (!sepstr) {
      return false;
    }
  } else {
    sepstr = cx->names().comma_;
  }

  // Steps 5-8 (When the length is zero, directly return the empty string).
  if (length == 0) {
    args.rval().setString(cx->emptyString());
    return true;
  }

  // An optimized version of a special case of steps 5-8: when length==1 and
  // the 0th element is a string, ToString() of that element is a no-op and
  // so it can be immediately returned as the result.
  if (length == 1 && obj->is<NativeObject>()) {
    NativeObject* nobj = &obj->as<NativeObject>();
    if (nobj->getDenseInitializedLength() == 1) {
      Value elem0 = nobj->getDenseElement(0);
      if (elem0.isString()) {
        args.rval().set(elem0);
        return true;
      }
    }
  }

  // Step 5.
  JSStringBuilder sb(cx);
  if (sepstr->hasTwoByteChars() && !sb.ensureTwoByteChars()) {
    return false;
  }

  // The separator will be added |length - 1| times, reserve space for that
  // so that we don't have to unnecessarily grow the buffer.
  size_t seplen = sepstr->length();
  if (seplen > 0) {
    if (length > UINT32_MAX) {
      ReportAllocationOverflow(cx);
      return false;
    }
    CheckedInt<uint32_t> res =
        CheckedInt<uint32_t>(seplen) * (uint32_t(length) - 1);
    if (!res.isValid()) {
      ReportAllocationOverflow(cx);
      return false;
    }

    if (!sb.reserve(res.value())) {
      return false;
    }
  }

  // Various optimized versions of steps 6-7.
  if (seplen == 0) {
    auto sepOp = [](StringBuffer&) { return true; };
    if (!ArrayJoinKernel(cx, sepOp, obj, length, sb)) {
      return false;
    }
  } else if (seplen == 1) {
    char16_t c = sepstr->latin1OrTwoByteChar(0);
    if (c <= JSString::MAX_LATIN1_CHAR) {
      Latin1Char l1char = Latin1Char(c);
      auto sepOp = [l1char](StringBuffer& sb) { return sb.append(l1char); };
      if (!ArrayJoinKernel(cx, sepOp, obj, length, sb)) {
        return false;
      }
    } else {
      auto sepOp = [c](StringBuffer& sb) { return sb.append(c); };
      if (!ArrayJoinKernel(cx, sepOp, obj, length, sb)) {
        return false;
      }
    }
  } else {
    Handle<JSLinearString*> sepHandle = sepstr;
    auto sepOp = [sepHandle](StringBuffer& sb) { return sb.append(sepHandle); };
    if (!ArrayJoinKernel(cx, sepOp, obj, length, sb)) {
      return false;
    }
  }

  // Step 8.
  JSString* str = sb.finishString();
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

// ES2017 draft rev f8a9be8ea4bd97237d176907a1e3080dce20c68f
// 22.1.3.27 Array.prototype.toLocaleString ([ reserved1 [ , reserved2 ] ])
// ES2017 Intl draft rev 78bbe7d1095f5ff3760ac4017ed366026e4cb276
// 13.4.1 Array.prototype.toLocaleString ([ locales [ , options ]])
static bool array_toLocaleString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype",
                                        "toLocaleString");

  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Avoid calling into self-hosted code if the array is empty.
  if (obj->is<ArrayObject>() && obj->as<ArrayObject>().length() == 0) {
    args.rval().setString(cx->names().empty_);
    return true;
  }

  AutoCycleDetector detector(cx, obj);
  if (!detector.init()) {
    return false;
  }

  if (detector.foundCycle()) {
    args.rval().setString(cx->names().empty_);
    return true;
  }

  FixedInvokeArgs<2> args2(cx);

  args2[0].set(args.get(0));
  args2[1].set(args.get(1));

  // Steps 2-10.
  RootedValue thisv(cx, ObjectValue(*obj));
  return CallSelfHostedFunction(cx, cx->names().ArrayToLocaleString, thisv,
                                args2, args.rval());
}

/* vector must point to rooted memory. */
static bool SetArrayElements(JSContext* cx, HandleObject obj, uint64_t start,
                             uint32_t count, const Value* vector) {
  MOZ_ASSERT(count <= MAX_ARRAY_INDEX);
  MOZ_ASSERT(start + count < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));

  if (count == 0) {
    return true;
  }

  if (!ObjectMayHaveExtraIndexedProperties(obj) && start <= UINT32_MAX) {
    NativeObject* nobj = &obj->as<NativeObject>();
    DenseElementResult result =
        nobj->setOrExtendDenseElements(cx, uint32_t(start), vector, count);
    if (result != DenseElementResult::Incomplete) {
      return result == DenseElementResult::Success;
    }
  }

  RootedId id(cx);
  const Value* end = vector + count;
  while (vector < end) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    if (!ToId(cx, start++, &id)) {
      return false;
    }

    if (!SetProperty(cx, obj, id, HandleValue::fromMarkedLocation(vector++))) {
      return false;
    }
  }

  return true;
}

static DenseElementResult ArrayReverseDenseKernel(JSContext* cx,
                                                  Handle<NativeObject*> obj,
                                                  uint32_t length) {
  MOZ_ASSERT(length > 1);

  // If there are no elements, we're done.
  if (obj->getDenseInitializedLength() == 0) {
    return DenseElementResult::Success;
  }

  if (!obj->isExtensible()) {
    return DenseElementResult::Incomplete;
  }

  if (!IsPackedArray(obj)) {
    /*
     * It's actually surprisingly complicated to reverse an array due
     * to the orthogonality of array length and array capacity while
     * handling leading and trailing holes correctly.  Reversing seems
     * less likely to be a common operation than other array
     * mass-mutation methods, so for now just take a probably-small
     * memory hit (in the absence of too many holes in the array at
     * its start) and ensure that the capacity is sufficient to hold
     * all the elements in the array if it were full.
     */
    DenseElementResult result = obj->ensureDenseElements(cx, length, 0);
    if (result != DenseElementResult::Success) {
      return result;
    }

    /* Fill out the array's initialized length to its proper length. */
    obj->ensureDenseInitializedLength(length, 0);
  }

  if (!obj->denseElementsMaybeInIteration() &&
      !cx->zone()->needsIncrementalBarrier()) {
    obj->reverseDenseElementsNoPreBarrier(length);
    return DenseElementResult::Success;
  }

  auto setElementMaybeHole = [](JSContext* cx, Handle<NativeObject*> obj,
                                uint32_t index, const Value& val) {
    if (MOZ_LIKELY(!val.isMagic(JS_ELEMENTS_HOLE))) {
      obj->setDenseElement(index, val);
      return true;
    }

    obj->setDenseElementHole(index);
    return SuppressDeletedProperty(cx, obj, PropertyKey::Int(index));
  };

  RootedValue origlo(cx), orighi(cx);

  uint32_t lo = 0, hi = length - 1;
  for (; lo < hi; lo++, hi--) {
    origlo = obj->getDenseElement(lo);
    orighi = obj->getDenseElement(hi);
    if (!setElementMaybeHole(cx, obj, lo, orighi)) {
      return DenseElementResult::Failure;
    }
    if (!setElementMaybeHole(cx, obj, hi, origlo)) {
      return DenseElementResult::Failure;
    }
  }

  return DenseElementResult::Success;
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.21 Array.prototype.reverse ( )
static bool array_reverse(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "reverse");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2.
  uint64_t len;
  if (!GetLengthPropertyInlined(cx, obj, &len)) {
    return false;
  }

  // An empty array or an array with length 1 is already reversed.
  if (len <= 1) {
    args.rval().setObject(*obj);
    return true;
  }

  if (IsPackedArrayOrNoExtraIndexedProperties(obj, len) && len <= UINT32_MAX) {
    DenseElementResult result =
        ArrayReverseDenseKernel(cx, obj.as<NativeObject>(), uint32_t(len));
    if (result != DenseElementResult::Incomplete) {
      /*
       * Per ECMA-262, don't update the length of the array, even if the new
       * array has trailing holes (and thus the original array began with
       * holes).
       */
      args.rval().setObject(*obj);
      return result == DenseElementResult::Success;
    }
  }

  // Steps 3-5.
  RootedValue lowval(cx), hival(cx);
  for (uint64_t i = 0, half = len / 2; i < half; i++) {
    bool hole, hole2;
    if (!CheckForInterrupt(cx) ||
        !HasAndGetElement(cx, obj, i, &hole, &lowval) ||
        !HasAndGetElement(cx, obj, len - i - 1, &hole2, &hival)) {
      return false;
    }

    if (!hole && !hole2) {
      if (!SetArrayElement(cx, obj, i, hival)) {
        return false;
      }
      if (!SetArrayElement(cx, obj, len - i - 1, lowval)) {
        return false;
      }
    } else if (hole && !hole2) {
      if (!SetArrayElement(cx, obj, i, hival)) {
        return false;
      }
      if (!DeletePropertyOrThrow(cx, obj, len - i - 1)) {
        return false;
      }
    } else if (!hole && hole2) {
      if (!DeletePropertyOrThrow(cx, obj, i)) {
        return false;
      }
      if (!SetArrayElement(cx, obj, len - i - 1, lowval)) {
        return false;
      }
    } else {
      // No action required.
    }
  }

  // Step 6.
  args.rval().setObject(*obj);
  return true;
}

static inline bool CompareStringValues(JSContext* cx, const Value& a,
                                       const Value& b, bool* lessOrEqualp) {
  if (!CheckForInterrupt(cx)) {
    return false;
  }

  JSString* astr = a.toString();
  JSString* bstr = b.toString();
  int32_t result;
  if (!CompareStrings(cx, astr, bstr, &result)) {
    return false;
  }

  *lessOrEqualp = (result <= 0);
  return true;
}

static const uint64_t powersOf10[] = {
    1,       10,       100,       1000,       10000,           100000,
    1000000, 10000000, 100000000, 1000000000, 1000000000000ULL};

static inline unsigned NumDigitsBase10(uint32_t n) {
  /*
   * This is just floor_log10(n) + 1
   * Algorithm taken from
   * http://graphics.stanford.edu/~seander/bithacks.html#IntegerLog10
   */
  uint32_t log2 = CeilingLog2(n);
  uint32_t t = log2 * 1233 >> 12;
  return t - (n < powersOf10[t]) + 1;
}

static inline bool CompareLexicographicInt32(const Value& a, const Value& b,
                                             bool* lessOrEqualp) {
  int32_t aint = a.toInt32();
  int32_t bint = b.toInt32();

  /*
   * If both numbers are equal ... trivial
   * If only one of both is negative --> arithmetic comparison as char code
   * of '-' is always less than any other digit
   * If both numbers are negative convert them to positive and continue
   * handling ...
   */
  if (aint == bint) {
    *lessOrEqualp = true;
  } else if ((aint < 0) && (bint >= 0)) {
    *lessOrEqualp = true;
  } else if ((aint >= 0) && (bint < 0)) {
    *lessOrEqualp = false;
  } else {
    uint32_t auint = Abs(aint);
    uint32_t buint = Abs(bint);

    /*
     *  ... get number of digits of both integers.
     * If they have the same number of digits --> arithmetic comparison.
     * If digits_a > digits_b: a < b*10e(digits_a - digits_b).
     * If digits_b > digits_a: a*10e(digits_b - digits_a) <= b.
     */
    unsigned digitsa = NumDigitsBase10(auint);
    unsigned digitsb = NumDigitsBase10(buint);
    if (digitsa == digitsb) {
      *lessOrEqualp = (auint <= buint);
    } else if (digitsa > digitsb) {
      MOZ_ASSERT((digitsa - digitsb) < std::size(powersOf10));
      *lessOrEqualp =
          (uint64_t(auint) < uint64_t(buint) * powersOf10[digitsa - digitsb]);
    } else { /* if (digitsb > digitsa) */
      MOZ_ASSERT((digitsb - digitsa) < std::size(powersOf10));
      *lessOrEqualp =
          (uint64_t(auint) * powersOf10[digitsb - digitsa] <= uint64_t(buint));
    }
  }

  return true;
}

template <typename Char1, typename Char2>
static inline bool CompareSubStringValues(JSContext* cx, const Char1* s1,
                                          size_t len1, const Char2* s2,
                                          size_t len2, bool* lessOrEqualp) {
  if (!CheckForInterrupt(cx)) {
    return false;
  }

  if (!s1 || !s2) {
    return false;
  }

  int32_t result = CompareChars(s1, len1, s2, len2);
  *lessOrEqualp = (result <= 0);
  return true;
}

namespace {

struct SortComparatorStrings {
  JSContext* const cx;

  explicit SortComparatorStrings(JSContext* cx) : cx(cx) {}

  bool operator()(const Value& a, const Value& b, bool* lessOrEqualp) {
    return CompareStringValues(cx, a, b, lessOrEqualp);
  }
};

struct SortComparatorLexicographicInt32 {
  bool operator()(const Value& a, const Value& b, bool* lessOrEqualp) {
    return CompareLexicographicInt32(a, b, lessOrEqualp);
  }
};

struct StringifiedElement {
  size_t charsBegin;
  size_t charsEnd;
  size_t elementIndex;
};

struct SortComparatorStringifiedElements {
  JSContext* const cx;
  const StringBuffer& sb;

  SortComparatorStringifiedElements(JSContext* cx, const StringBuffer& sb)
      : cx(cx), sb(sb) {}

  bool operator()(const StringifiedElement& a, const StringifiedElement& b,
                  bool* lessOrEqualp) {
    size_t lenA = a.charsEnd - a.charsBegin;
    size_t lenB = b.charsEnd - b.charsBegin;

    if (sb.isUnderlyingBufferLatin1()) {
      return CompareSubStringValues(cx, sb.rawLatin1Begin() + a.charsBegin,
                                    lenA, sb.rawLatin1Begin() + b.charsBegin,
                                    lenB, lessOrEqualp);
    }

    return CompareSubStringValues(cx, sb.rawTwoByteBegin() + a.charsBegin, lenA,
                                  sb.rawTwoByteBegin() + b.charsBegin, lenB,
                                  lessOrEqualp);
  }
};

struct NumericElement {
  double dv;
  size_t elementIndex;
};

static bool ComparatorNumericLeftMinusRight(const NumericElement& a,
                                            const NumericElement& b,
                                            bool* lessOrEqualp) {
  *lessOrEqualp = std::isunordered(a.dv, b.dv) || (a.dv <= b.dv);
  return true;
}

static bool ComparatorNumericRightMinusLeft(const NumericElement& a,
                                            const NumericElement& b,
                                            bool* lessOrEqualp) {
  *lessOrEqualp = std::isunordered(a.dv, b.dv) || (b.dv <= a.dv);
  return true;
}

using ComparatorNumeric = bool (*)(const NumericElement&, const NumericElement&,
                                   bool*);

static const ComparatorNumeric SortComparatorNumerics[] = {
    nullptr, nullptr, ComparatorNumericLeftMinusRight,
    ComparatorNumericRightMinusLeft};

static bool ComparatorInt32LeftMinusRight(const Value& a, const Value& b,
                                          bool* lessOrEqualp) {
  *lessOrEqualp = (a.toInt32() <= b.toInt32());
  return true;
}

static bool ComparatorInt32RightMinusLeft(const Value& a, const Value& b,
                                          bool* lessOrEqualp) {
  *lessOrEqualp = (b.toInt32() <= a.toInt32());
  return true;
}

using ComparatorInt32 = bool (*)(const Value&, const Value&, bool*);

static const ComparatorInt32 SortComparatorInt32s[] = {
    nullptr, nullptr, ComparatorInt32LeftMinusRight,
    ComparatorInt32RightMinusLeft};

// Note: Values for this enum must match up with SortComparatorNumerics
// and SortComparatorInt32s.
enum ComparatorMatchResult {
  Match_Failure = 0,
  Match_None,
  Match_LeftMinusRight,
  Match_RightMinusLeft
};

}  // namespace

/*
 * Specialize behavior for comparator functions with particular common bytecode
 * patterns: namely, |return x - y| and |return y - x|.
 */
static ComparatorMatchResult MatchNumericComparator(JSContext* cx,
                                                    JSObject* obj) {
  if (!obj->is<JSFunction>()) {
    return Match_None;
  }

  RootedFunction fun(cx, &obj->as<JSFunction>());
  if (!fun->isInterpreted() || fun->isClassConstructor()) {
    return Match_None;
  }

  JSScript* script = JSFunction::getOrCreateScript(cx, fun);
  if (!script) {
    return Match_Failure;
  }

  jsbytecode* pc = script->code();

  uint16_t arg0, arg1;
  if (JSOp(*pc) != JSOp::GetArg) {
    return Match_None;
  }
  arg0 = GET_ARGNO(pc);
  pc += JSOpLength_GetArg;

  if (JSOp(*pc) != JSOp::GetArg) {
    return Match_None;
  }
  arg1 = GET_ARGNO(pc);
  pc += JSOpLength_GetArg;

  if (JSOp(*pc) != JSOp::Sub) {
    return Match_None;
  }
  pc += JSOpLength_Sub;

  if (JSOp(*pc) != JSOp::Return) {
    return Match_None;
  }

  if (arg0 == 0 && arg1 == 1) {
    return Match_LeftMinusRight;
  }

  if (arg0 == 1 && arg1 == 0) {
    return Match_RightMinusLeft;
  }

  return Match_None;
}

template <typename K, typename C>
static inline bool MergeSortByKey(K keys, size_t len, K scratch, C comparator,
                                  MutableHandle<GCVector<Value>> vec) {
  MOZ_ASSERT(vec.length() >= len);

  /* Sort keys. */
  if (!MergeSort(keys, len, scratch, comparator)) {
    return false;
  }

  /*
   * Reorder vec by keys in-place, going element by element.  When an out-of-
   * place element is encountered, move that element to its proper position,
   * displacing whatever element was at *that* point to its proper position,
   * and so on until an element must be moved to the current position.
   *
   * At each outer iteration all elements up to |i| are sorted.  If
   * necessary each inner iteration moves some number of unsorted elements
   * (including |i|) directly to sorted position.  Thus on completion |*vec|
   * is sorted, and out-of-position elements have moved once.  Complexity is
   * Θ(len) + O(len) == O(2*len), with each element visited at most twice.
   */
  for (size_t i = 0; i < len; i++) {
    size_t j = keys[i].elementIndex;
    if (i == j) {
      continue;  // fixed point
    }

    MOZ_ASSERT(j > i, "Everything less than |i| should be in the right place!");
    Value tv = vec[j];
    do {
      size_t k = keys[j].elementIndex;
      keys[j].elementIndex = j;
      vec[j].set(vec[k]);
      j = k;
    } while (j != i);

    // We could assert the loop invariant that |i == keys[i].elementIndex|
    // here if we synced |keys[i].elementIndex|.  But doing so would render
    // the assertion vacuous, so don't bother, even in debug builds.
    vec[i].set(tv);
  }

  return true;
}

/*
 * Sort Values as strings.
 *
 * To minimize #conversions, SortLexicographically() first converts all Values
 * to strings at once, then sorts the elements by these cached strings.
 */
static bool SortLexicographically(JSContext* cx,
                                  MutableHandle<GCVector<Value>> vec,
                                  size_t len) {
  MOZ_ASSERT(vec.length() >= len);

  StringBuffer sb(cx);
  Vector<StringifiedElement, 0, TempAllocPolicy> strElements(cx);

  /* MergeSort uses the upper half as scratch space. */
  if (!strElements.resize(2 * len)) {
    return false;
  }

  /* Convert Values to strings. */
  size_t cursor = 0;
  for (size_t i = 0; i < len; i++) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    if (!ValueToStringBuffer(cx, vec[i], sb)) {
      return false;
    }

    strElements[i] = {cursor, sb.length(), i};
    cursor = sb.length();
  }

  /* Sort Values in vec alphabetically. */
  return MergeSortByKey(strElements.begin(), len, strElements.begin() + len,
                        SortComparatorStringifiedElements(cx, sb), vec);
}

/*
 * Sort Values as numbers.
 *
 * To minimize #conversions, SortNumerically first converts all Values to
 * numerics at once, then sorts the elements by these cached numerics.
 */
static bool SortNumerically(JSContext* cx, MutableHandle<GCVector<Value>> vec,
                            size_t len, ComparatorMatchResult comp) {
  MOZ_ASSERT(vec.length() >= len);

  Vector<NumericElement, 0, TempAllocPolicy> numElements(cx);

  /* MergeSort uses the upper half as scratch space. */
  if (!numElements.resize(2 * len)) {
    return false;
  }

  /* Convert Values to numerics. */
  for (size_t i = 0; i < len; i++) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    double dv;
    if (!ToNumber(cx, vec[i], &dv)) {
      return false;
    }

    numElements[i] = {dv, i};
  }

  /* Sort Values in vec numerically. */
  return MergeSortByKey(numElements.begin(), len, numElements.begin() + len,
                        SortComparatorNumerics[comp], vec);
}

static bool FillWithUndefined(JSContext* cx, HandleObject obj, uint32_t start,
                              uint32_t count) {
  MOZ_ASSERT(start < start + count,
             "count > 0 and start + count doesn't overflow");

  do {
    if (ObjectMayHaveExtraIndexedProperties(obj)) {
      break;
    }

    NativeObject* nobj = &obj->as<NativeObject>();
    if (!nobj->isExtensible()) {
      break;
    }

    if (obj->is<ArrayObject>() && !obj->as<ArrayObject>().lengthIsWritable() &&
        start + count >= obj->as<ArrayObject>().length()) {
      break;
    }

    DenseElementResult result = nobj->ensureDenseElements(cx, start, count);
    if (result != DenseElementResult::Success) {
      if (result == DenseElementResult::Failure) {
        return false;
      }
      MOZ_ASSERT(result == DenseElementResult::Incomplete);
      break;
    }

    if (obj->is<ArrayObject>() &&
        start + count >= obj->as<ArrayObject>().length()) {
      obj->as<ArrayObject>().setLength(start + count);
    }

    for (uint32_t i = 0; i < count; i++) {
      nobj->setDenseElement(start + i, UndefinedHandleValue);
    }

    return true;
  } while (false);

  for (uint32_t i = 0; i < count; i++) {
    if (!CheckForInterrupt(cx) ||
        !SetArrayElement(cx, obj, start + i, UndefinedHandleValue)) {
      return false;
    }
  }

  return true;
}

static bool ArraySortWithoutComparator(JSContext* cx, Handle<JSObject*> obj,
                                       uint64_t length,
                                       ComparatorMatchResult comp) {
  MOZ_ASSERT(length > 1);

  if (length > UINT32_MAX) {
    ReportAllocationOverflow(cx);
    return false;
  }
  uint32_t len = uint32_t(length);

  /*
   * We need a temporary array of 2 * len Value to hold the array elements
   * and the scratch space for merge sort. Check that its size does not
   * overflow size_t, which would allow for indexing beyond the end of the
   * malloc'd vector.
   */
#if JS_BITS_PER_WORD == 32
  if (size_t(len) > size_t(-1) / (2 * sizeof(Value))) {
    ReportAllocationOverflow(cx);
    return false;
  }
#endif

  size_t n, undefs;
  {
    Rooted<GCVector<Value>> vec(cx, GCVector<Value>(cx));
    if (!vec.reserve(2 * size_t(len))) {
      return false;
    }

    /*
     * By ECMA 262, 15.4.4.11, a property that does not exist (which we
     * call a "hole") is always greater than an existing property with
     * value undefined and that is always greater than any other property.
     * Thus to sort holes and undefs we simply count them, sort the rest
     * of elements, append undefs after them and then make holes after
     * undefs.
     */
    undefs = 0;
    bool allStrings = true;
    bool allInts = true;
    RootedValue v(cx);
    if (IsPackedArray(obj)) {
      Handle<ArrayObject*> array = obj.as<ArrayObject>();

      for (uint32_t i = 0; i < len; i++) {
        if (!CheckForInterrupt(cx)) {
          return false;
        }

        v.set(array->getDenseElement(i));
        MOZ_ASSERT(!v.isMagic(JS_ELEMENTS_HOLE));
        if (v.isUndefined()) {
          ++undefs;
          continue;
        }
        vec.infallibleAppend(v);
        allStrings = allStrings && v.isString();
        allInts = allInts && v.isInt32();
      }
    } else {
      for (uint32_t i = 0; i < len; i++) {
        if (!CheckForInterrupt(cx)) {
          return false;
        }

        bool hole;
        if (!HasAndGetElement(cx, obj, i, &hole, &v)) {
          return false;
        }
        if (hole) {
          continue;
        }
        if (v.isUndefined()) {
          ++undefs;
          continue;
        }
        vec.infallibleAppend(v);
        allStrings = allStrings && v.isString();
        allInts = allInts && v.isInt32();
      }
    }

    /*
     * If the array only contains holes, we're done.  But if it contains
     * undefs, those must be sorted to the front of the array.
     */
    n = vec.length();
    if (n == 0 && undefs == 0) {
      return true;
    }

    /* Here len == n + undefs + number_of_holes. */
    if (comp == Match_None) {
      /*
       * Sort using the default comparator converting all elements to
       * strings.
       */
      if (allStrings) {
        MOZ_ALWAYS_TRUE(vec.resize(n * 2));
        if (!MergeSort(vec.begin(), n, vec.begin() + n,
                       SortComparatorStrings(cx))) {
          return false;
        }
      } else if (allInts) {
        MOZ_ALWAYS_TRUE(vec.resize(n * 2));
        if (!MergeSort(vec.begin(), n, vec.begin() + n,
                       SortComparatorLexicographicInt32())) {
          return false;
        }
      } else {
        if (!SortLexicographically(cx, &vec, n)) {
          return false;
        }
      }
    } else {
      if (allInts) {
        MOZ_ALWAYS_TRUE(vec.resize(n * 2));
        if (!MergeSort(vec.begin(), n, vec.begin() + n,
                       SortComparatorInt32s[comp])) {
          return false;
        }
      } else {
        if (!SortNumerically(cx, &vec, n, comp)) {
          return false;
        }
      }
    }

    if (!SetArrayElements(cx, obj, 0, uint32_t(n), vec.begin())) {
      return false;
    }
  }

  /* Set undefs that sorted after the rest of elements. */
  if (undefs > 0) {
    if (!FillWithUndefined(cx, obj, n, undefs)) {
      return false;
    }
    n += undefs;
  }

  /* Re-create any holes that sorted to the end of the array. */
  for (uint32_t i = n; i < len; i++) {
    if (!CheckForInterrupt(cx) || !DeletePropertyOrThrow(cx, obj, i)) {
      return false;
    }
  }
  return true;
}

// This function handles sorting without a comparator function (or with a
// trivial comparator function that we can pattern match) by calling
// ArraySortWithoutComparator.
//
// If there's a non-trivial comparator function, it initializes the
// ArraySortData struct for ArraySortData::sortArrayWithComparator. This
// function must be called next to perform the sorting.
//
// This is separate from ArraySortData::sortArrayWithComparator because it lets
// the compiler generate better code for ArraySortData::sortArrayWithComparator.
//
// https://tc39.es/ecma262/#sec-array.prototype.sort
// 23.1.3.30 Array.prototype.sort ( comparefn )
static MOZ_ALWAYS_INLINE bool ArraySortPrologue(JSContext* cx,
                                                Handle<Value> thisv,
                                                Handle<Value> comparefn,
                                                ArraySortData* d, bool* done) {
  // Step 1.
  if (MOZ_UNLIKELY(!comparefn.isUndefined() && !IsCallable(comparefn))) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_SORT_ARG);
    return false;
  }

  // Step 2.
  Rooted<JSObject*> obj(cx, ToObject(cx, thisv));
  if (!obj) {
    return false;
  }

  // Step 3.
  uint64_t length;
  if (MOZ_UNLIKELY(!GetLengthPropertyInlined(cx, obj, &length))) {
    return false;
  }

  // Arrays with less than two elements remain unchanged when sorted.
  if (length <= 1) {
    d->setReturnValue(obj);
    *done = true;
    return true;
  }

  // Use a fast path if there's no comparator or if the comparator is a function
  // that we can pattern match.
  do {
    ComparatorMatchResult comp = Match_None;
    if (comparefn.isObject()) {
      comp = MatchNumericComparator(cx, &comparefn.toObject());
      if (comp == Match_Failure) {
        return false;
      }
      if (comp == Match_None) {
        // Pattern matching failed.
        break;
      }
    }
    if (!ArraySortWithoutComparator(cx, obj, length, comp)) {
      return false;
    }
    d->setReturnValue(obj);
    *done = true;
    return true;
  } while (false);

  // Ensure length * 2 (used below) doesn't overflow UINT32_MAX.
  if (MOZ_UNLIKELY(length > UINT32_MAX / 2)) {
    ReportAllocationOverflow(cx);
    return false;
  }
  uint32_t len = uint32_t(length);

  // Merge sort requires extra scratch space.
  bool needsScratchSpace = len > ArraySortData::InsertionSortMaxLength;

  Rooted<ArraySortData::ValueVector> vec(cx);
  if (MOZ_UNLIKELY(!vec.reserve(needsScratchSpace ? (2 * len) : len))) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Append elements to the vector. Skip holes.
  if (IsPackedArray(obj)) {
    Handle<ArrayObject*> array = obj.as<ArrayObject>();
    const Value* elements = array->getDenseElements();
    vec.infallibleAppend(elements, len);
  } else {
    RootedValue v(cx);
    for (uint32_t i = 0; i < len; i++) {
      if (!CheckForInterrupt(cx)) {
        return false;
      }

      bool hole;
      if (!HasAndGetElement(cx, obj, i, &hole, &v)) {
        return false;
      }
      if (hole) {
        continue;
      }
      vec.infallibleAppend(v);
    }
    // If there are only holes, the object is already sorted.
    if (vec.empty()) {
      d->setReturnValue(obj);
      *done = true;
      return true;
    }
  }

  uint32_t denseLen = vec.length();
  if (needsScratchSpace) {
    MOZ_ALWAYS_TRUE(vec.resize(denseLen * 2));
  }
  d->init(obj, &comparefn.toObject(), std::move(vec.get()), len, denseLen);

  // Continue in ArraySortData::sortArrayWithComparator.
  MOZ_ASSERT(!*done);
  return true;
}

ArraySortResult js::CallComparatorSlow(ArraySortData* d, const Value& x,
                                       const Value& y) {
  JSContext* cx = d->cx();
  FixedInvokeArgs<2> callArgs(cx);
  callArgs[0].set(x);
  callArgs[1].set(y);
  Rooted<Value> comparefn(cx, ObjectValue(*d->comparator()));
  Rooted<Value> rval(cx);
  if (!js::Call(cx, comparefn, UndefinedHandleValue, callArgs, &rval)) {
    return ArraySortResult::Failure;
  }
  d->setComparatorReturnValue(rval);
  return ArraySortResult::Done;
}

// static
ArraySortResult ArraySortData::sortArrayWithComparator(ArraySortData* d) {
  ArraySortResult result = sortWithComparatorShared<ArraySortKind::Array>(d);
  if (result != ArraySortResult::Done) {
    return result;
  }

  // Copy elements to the array.
  JSContext* cx = d->cx();
  Rooted<JSObject*> obj(cx, d->obj_);
  if (!SetArrayElements(cx, obj, 0, d->denseLen, d->list)) {
    return ArraySortResult::Failure;
  }

  // Re-create any holes that sorted to the end of the array.
  for (uint32_t i = d->denseLen; i < d->length; i++) {
    if (!CheckForInterrupt(cx) || !DeletePropertyOrThrow(cx, obj, i)) {
      return ArraySortResult::Failure;
    }
  }

  d->freeMallocData();
  d->setReturnValue(obj);
  return ArraySortResult::Done;
}

// https://tc39.es/ecma262/#sec-array.prototype.sort
// 23.1.3.30 Array.prototype.sort ( comparefn )
bool js::array_sort(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "sort");
  CallArgs args = CallArgsFromVp(argc, vp);

  // If we have a comparator argument, use the JIT trampoline implementation
  // instead. This avoids a performance cliff (especially with large arrays)
  // because C++ => JIT calls are much slower than Trampoline => JIT calls.
  if (args.hasDefined(0) && jit::IsBaselineInterpreterEnabled()) {
    return CallTrampolineNativeJitCode(cx, jit::TrampolineNative::ArraySort,
                                       args);
  }

  Rooted<ArraySortData> data(cx, cx);

  // On all return paths other than ArraySortData::sortArrayWithComparator
  // returning Done, we call freeMallocData to not fail debug assertions. This
  // matches the JIT trampoline where we can't rely on C++ destructors.
  auto freeData =
      mozilla::MakeScopeExit([&]() { data.get().freeMallocData(); });

  bool done = false;
  if (!ArraySortPrologue(cx, args.thisv(), args.get(0), data.address(),
                         &done)) {
    return false;
  }
  if (done) {
    args.rval().set(data.get().returnValue());
    return true;
  }

  FixedInvokeArgs<2> callArgs(cx);
  Rooted<Value> rval(cx);

  while (true) {
    ArraySortResult res =
        ArraySortData::sortArrayWithComparator(data.address());
    switch (res) {
      case ArraySortResult::Failure:
        return false;

      case ArraySortResult::Done:
        freeData.release();
        args.rval().set(data.get().returnValue());
        return true;

      case ArraySortResult::CallJS:
      case ArraySortResult::CallJSSameRealmNoRectifier:
        MOZ_ASSERT(data.get().comparatorThisValue().isUndefined());
        MOZ_ASSERT(&args[0].toObject() == data.get().comparator());
        callArgs[0].set(data.get().comparatorArg(0));
        callArgs[1].set(data.get().comparatorArg(1));
        if (!js::Call(cx, args[0], UndefinedHandleValue, callArgs, &rval)) {
          return false;
        }
        data.get().setComparatorReturnValue(rval);
        break;
    }
  }
}

ArraySortResult js::ArraySortFromJit(JSContext* cx,
                                     jit::TrampolineNativeFrameLayout* frame) {
  // Initialize the ArraySortData class stored in the trampoline frame.
  void* dataUninit = frame->getFrameData<ArraySortData>();
  auto* data = new (dataUninit) ArraySortData(cx);

  Rooted<Value> thisv(cx, frame->thisv());
  Rooted<Value> comparefn(cx);
  if (frame->numActualArgs() > 0) {
    comparefn = frame->actualArgs()[0];
  }

  bool done = false;
  if (!ArraySortPrologue(cx, thisv, comparefn, data, &done)) {
    return ArraySortResult::Failure;
  }
  if (done) {
    data->freeMallocData();
    return ArraySortResult::Done;
  }

  return ArraySortData::sortArrayWithComparator(data);
}

void ArraySortData::trace(JSTracer* trc) {
  TraceNullableRoot(trc, &comparator_, "comparator_");
  TraceRoot(trc, &thisv, "thisv");
  TraceRoot(trc, &callArgs[0], "callArgs0");
  TraceRoot(trc, &callArgs[1], "callArgs1");
  vec.trace(trc);
  TraceRoot(trc, &item, "item");
  TraceNullableRoot(trc, &obj_, "obj");
}

bool js::NewbornArrayPush(JSContext* cx, HandleObject obj, const Value& v) {
  Handle<ArrayObject*> arr = obj.as<ArrayObject>();

  MOZ_ASSERT(!v.isMagic());
  MOZ_ASSERT(arr->lengthIsWritable());

  uint32_t length = arr->length();
  MOZ_ASSERT(length <= arr->getDenseCapacity());

  if (!arr->ensureElements(cx, length + 1)) {
    return false;
  }

  arr->setDenseInitializedLength(length + 1);
  arr->setLength(length + 1);
  arr->initDenseElement(length, v);
  return true;
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.18 Array.prototype.push ( ...items )
static bool array_push(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "push");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2.
  uint64_t length;
  if (!GetLengthPropertyInlined(cx, obj, &length)) {
    return false;
  }

  if (!ObjectMayHaveExtraIndexedProperties(obj) && length <= UINT32_MAX) {
    DenseElementResult result =
        obj->as<NativeObject>().setOrExtendDenseElements(
            cx, uint32_t(length), args.array(), args.length());
    if (result != DenseElementResult::Incomplete) {
      if (result == DenseElementResult::Failure) {
        return false;
      }

      uint32_t newlength = uint32_t(length) + args.length();
      args.rval().setNumber(newlength);

      // setOrExtendDenseElements takes care of updating the length for
      // arrays. Handle updates to the length of non-arrays here.
      if (!obj->is<ArrayObject>()) {
        MOZ_ASSERT(obj->is<NativeObject>());
        return SetLengthProperty(cx, obj, newlength);
      }

      return true;
    }
  }

  // Step 5.
  uint64_t newlength = length + args.length();
  if (newlength >= uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TOO_LONG_ARRAY);
    return false;
  }

  // Steps 3-6.
  if (!SetArrayElements(cx, obj, length, args.length(), args.array())) {
    return false;
  }

  // Steps 7-8.
  args.rval().setNumber(double(newlength));
  return SetLengthProperty(cx, obj, newlength);
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.17 Array.prototype.pop ( )
bool js::array_pop(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "pop");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2.
  uint64_t index;
  if (!GetLengthPropertyInlined(cx, obj, &index)) {
    return false;
  }

  // Steps 3-4.
  if (index == 0) {
    // Step 3.b.
    args.rval().setUndefined();
  } else {
    // Steps 4.a-b.
    index--;

    // Steps 4.c, 4.f.
    if (!GetArrayElement(cx, obj, index, args.rval())) {
      return false;
    }

    // Steps 4.d.
    if (!DeletePropertyOrThrow(cx, obj, index)) {
      return false;
    }
  }

  // Steps 3.a, 4.e.
  return SetLengthProperty(cx, obj, index);
}

void js::ArrayShiftMoveElements(ArrayObject* arr) {
  AutoUnsafeCallWithABI unsafe;
  MOZ_ASSERT(arr->isExtensible());
  MOZ_ASSERT(arr->lengthIsWritable());
  MOZ_ASSERT(IsPackedArray(arr));
  MOZ_ASSERT(!arr->denseElementsHaveMaybeInIterationFlag());

  size_t initlen = arr->getDenseInitializedLength();
  MOZ_ASSERT(initlen > 0);

  if (!arr->tryShiftDenseElements(1)) {
    arr->moveDenseElements(0, 1, initlen - 1);
    arr->setDenseInitializedLength(initlen - 1);
  }

  MOZ_ASSERT(arr->getDenseInitializedLength() == initlen - 1);
  arr->setLength(initlen - 1);
}

static inline void SetInitializedLength(JSContext* cx, NativeObject* obj,
                                        size_t initlen) {
  MOZ_ASSERT(obj->isExtensible());

  size_t oldInitlen = obj->getDenseInitializedLength();
  obj->setDenseInitializedLength(initlen);
  if (initlen < oldInitlen) {
    obj->shrinkElements(cx, initlen);
  }
}

static DenseElementResult ArrayShiftDenseKernel(JSContext* cx, HandleObject obj,
                                                MutableHandleValue rval) {
  if (!IsPackedArray(obj) && ObjectMayHaveExtraIndexedProperties(obj)) {
    return DenseElementResult::Incomplete;
  }

  Handle<NativeObject*> nobj = obj.as<NativeObject>();
  if (nobj->denseElementsMaybeInIteration()) {
    return DenseElementResult::Incomplete;
  }

  if (!nobj->isExtensible()) {
    return DenseElementResult::Incomplete;
  }

  size_t initlen = nobj->getDenseInitializedLength();
  if (initlen == 0) {
    return DenseElementResult::Incomplete;
  }

  rval.set(nobj->getDenseElement(0));
  if (rval.isMagic(JS_ELEMENTS_HOLE)) {
    rval.setUndefined();
  }

  if (nobj->tryShiftDenseElements(1)) {
    return DenseElementResult::Success;
  }

  nobj->moveDenseElements(0, 1, initlen - 1);

  SetInitializedLength(cx, nobj, initlen - 1);
  return DenseElementResult::Success;
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.22 Array.prototype.shift ( )
static bool array_shift(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "shift");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2.
  uint64_t len;
  if (!GetLengthPropertyInlined(cx, obj, &len)) {
    return false;
  }

  // Step 3.
  if (len == 0) {
    // Step 3.a.
    if (!SetLengthProperty(cx, obj, uint32_t(0))) {
      return false;
    }

    // Step 3.b.
    args.rval().setUndefined();
    return true;
  }

  uint64_t newlen = len - 1;

  /* Fast paths. */
  uint64_t startIndex;
  DenseElementResult result = ArrayShiftDenseKernel(cx, obj, args.rval());
  if (result != DenseElementResult::Incomplete) {
    if (result == DenseElementResult::Failure) {
      return false;
    }

    if (len <= UINT32_MAX) {
      return SetLengthProperty(cx, obj, newlen);
    }

    startIndex = UINT32_MAX - 1;
  } else {
    // Steps 4, 9.
    if (!GetElement(cx, obj, 0, args.rval())) {
      return false;
    }

    startIndex = 0;
  }

  // Steps 5-6.
  RootedValue value(cx);
  for (uint64_t i = startIndex; i < newlen; i++) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }
    bool hole;
    if (!HasAndGetElement(cx, obj, i + 1, &hole, &value)) {
      return false;
    }
    if (hole) {
      if (!DeletePropertyOrThrow(cx, obj, i)) {
        return false;
      }
    } else {
      if (!SetArrayElement(cx, obj, i, value)) {
        return false;
      }
    }
  }

  // Step 7.
  if (!DeletePropertyOrThrow(cx, obj, newlen)) {
    return false;
  }

  // Step 8.
  return SetLengthProperty(cx, obj, newlen);
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.29 Array.prototype.unshift ( ...items )
static bool array_unshift(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "unshift");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2.
  uint64_t length;
  if (!GetLengthPropertyInlined(cx, obj, &length)) {
    return false;
  }

  // Steps 3-4.
  if (args.length() > 0) {
    bool optimized = false;
    do {
      if (length > UINT32_MAX) {
        break;
      }
      if (ObjectMayHaveExtraIndexedProperties(obj)) {
        break;
      }
      NativeObject* nobj = &obj->as<NativeObject>();
      if (nobj->denseElementsMaybeInIteration()) {
        break;
      }
      if (!nobj->isExtensible()) {
        break;
      }
      if (nobj->is<ArrayObject>() &&
          !nobj->as<ArrayObject>().lengthIsWritable()) {
        break;
      }
      if (!nobj->tryUnshiftDenseElements(args.length())) {
        DenseElementResult result =
            nobj->ensureDenseElements(cx, uint32_t(length), args.length());
        if (result != DenseElementResult::Success) {
          if (result == DenseElementResult::Failure) {
            return false;
          }
          MOZ_ASSERT(result == DenseElementResult::Incomplete);
          break;
        }
        if (length > 0) {
          nobj->moveDenseElements(args.length(), 0, uint32_t(length));
        }
      }
      for (uint32_t i = 0; i < args.length(); i++) {
        nobj->setDenseElement(i, args[i]);
      }
      optimized = true;
    } while (false);

    if (!optimized) {
      if (length > 0) {
        uint64_t last = length;
        uint64_t upperIndex = last + args.length();

        // Step 4.a.
        if (upperIndex >= uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT)) {
          JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                    JSMSG_TOO_LONG_ARRAY);
          return false;
        }

        // Steps 4.b-c.
        RootedValue value(cx);
        do {
          --last;
          --upperIndex;
          if (!CheckForInterrupt(cx)) {
            return false;
          }
          bool hole;
          if (!HasAndGetElement(cx, obj, last, &hole, &value)) {
            return false;
          }
          if (hole) {
            if (!DeletePropertyOrThrow(cx, obj, upperIndex)) {
              return false;
            }
          } else {
            if (!SetArrayElement(cx, obj, upperIndex, value)) {
              return false;
            }
          }
        } while (last != 0);
      }

      // Steps 4.d-f.
      /* Copy from args to the bottom of the array. */
      if (!SetArrayElements(cx, obj, 0, args.length(), args.array())) {
        return false;
      }
    }
  }

  // Step 5.
  uint64_t newlength = length + args.length();
  if (!SetLengthProperty(cx, obj, newlength)) {
    return false;
  }

  // Step 6.
  /* Follow Perl by returning the new array length. */
  args.rval().setNumber(double(newlength));
  return true;
}

enum class ArrayAccess { Read, Write };

/*
 * Returns true if this is a dense array whose properties ending at |endIndex|
 * (exclusive) may be accessed (get, set, delete) directly through its
 * contiguous vector of elements without fear of getters, setters, etc. along
 * the prototype chain, or of enumerators requiring notification of
 * modifications.
 */
template <ArrayAccess Access>
static bool CanOptimizeForDenseStorage(HandleObject arr, uint64_t endIndex) {
  /* If the desired properties overflow dense storage, we can't optimize. */
  if (endIndex > UINT32_MAX) {
    return false;
  }

  if (Access == ArrayAccess::Read) {
    /*
     * Dense storage read access is possible for any packed array as long
     * as we only access properties within the initialized length. In all
     * other cases we need to ensure there are no other indexed properties
     * on this object or on the prototype chain. Callers are required to
     * clamp the read length, so it doesn't exceed the initialized length.
     */
    if (IsPackedArray(arr) &&
        endIndex <= arr->as<ArrayObject>().getDenseInitializedLength()) {
      return true;
    }
    return !ObjectMayHaveExtraIndexedProperties(arr);
  }

  /* There's no optimizing possible if it's not an array. */
  if (!arr->is<ArrayObject>()) {
    return false;
  }

  /* If the length is non-writable, always pick the slow path */
  if (!arr->as<ArrayObject>().lengthIsWritable()) {
    return false;
  }

  /* Also pick the slow path if the object is non-extensible. */
  if (!arr->as<ArrayObject>().isExtensible()) {
    return false;
  }

  /* Also pick the slow path if the object is being iterated over. */
  if (arr->as<ArrayObject>().denseElementsMaybeInIteration()) {
    return false;
  }

  /* Or we attempt to write to indices outside the initialized length. */
  if (endIndex > arr->as<ArrayObject>().getDenseInitializedLength()) {
    return false;
  }

  /*
   * Now watch out for getters and setters along the prototype chain or in
   * other indexed properties on the object. Packed arrays don't have any
   * other indexed properties by definition.
   */
  return IsPackedArray(arr) || !ObjectMayHaveExtraIndexedProperties(arr);
}

static ArrayObject* CopyDenseArrayElements(JSContext* cx,
                                           Handle<NativeObject*> obj,
                                           uint32_t begin, uint32_t count) {
  size_t initlen = obj->getDenseInitializedLength();
  MOZ_ASSERT(initlen <= UINT32_MAX,
             "initialized length shouldn't exceed UINT32_MAX");
  uint32_t newlength = 0;
  if (initlen > begin) {
    newlength = std::min<uint32_t>(initlen - begin, count);
  }

  ArrayObject* narr = NewDenseFullyAllocatedArray(cx, newlength);
  if (!narr) {
    return nullptr;
  }

  MOZ_ASSERT(count >= narr->length());
  narr->setLength(count);

  if (newlength > 0) {
    narr->initDenseElements(obj, begin, newlength);
  }

  return narr;
}

static bool CopyArrayElements(JSContext* cx, HandleObject obj, uint64_t begin,
                              uint64_t count, Handle<ArrayObject*> result) {
  MOZ_ASSERT(result->length() == count);

  uint64_t startIndex = 0;
  RootedValue value(cx);

  // Use dense storage for new indexed properties where possible.
  {
    uint32_t index = 0;
    uint32_t limit = std::min<uint32_t>(count, PropertyKey::IntMax);
    for (; index < limit; index++) {
      bool hole;
      if (!CheckForInterrupt(cx) ||
          !HasAndGetElement(cx, obj, begin + index, &hole, &value)) {
        return false;
      }

      if (!hole) {
        DenseElementResult edResult = result->ensureDenseElements(cx, index, 1);
        if (edResult != DenseElementResult::Success) {
          if (edResult == DenseElementResult::Failure) {
            return false;
          }

          MOZ_ASSERT(edResult == DenseElementResult::Incomplete);
          if (!DefineDataElement(cx, result, index, value)) {
            return false;
          }

          break;
        }
        result->setDenseElement(index, value);
      }
    }
    startIndex = index + 1;
  }

  // Copy any remaining elements.
  for (uint64_t i = startIndex; i < count; i++) {
    bool hole;
    if (!CheckForInterrupt(cx) ||
        !HasAndGetElement(cx, obj, begin + i, &hole, &value)) {
      return false;
    }

    if (!hole && !DefineArrayElement(cx, result, i, value)) {
      return false;
    }
  }
  return true;
}

// Helpers for array_splice_impl() and array_to_spliced()
//
// Initialize variables common to splice() and toSpliced():
// - GetActualStart() returns the index at which to start deleting elements.
// - GetItemCount() returns the number of new elements being added.
// - GetActualDeleteCount() returns the number of elements being deleted.
static bool GetActualStart(JSContext* cx, HandleValue start, uint64_t len,
                           uint64_t* result) {
  MOZ_ASSERT(len < DOUBLE_INTEGRAL_PRECISION_LIMIT);

  // Steps from proposal: https://github.com/tc39/proposal-change-array-by-copy
  // Array.prototype.toSpliced()

  // Step 3. Let relativeStart be ? ToIntegerOrInfinity(start).
  double relativeStart;
  if (!ToInteger(cx, start, &relativeStart)) {
    return false;
  }

  // Steps 4-5. If relativeStart is -∞, let actualStart be 0.
  // Else if relativeStart < 0, let actualStart be max(len + relativeStart, 0).
  if (relativeStart < 0) {
    *result = uint64_t(std::max(double(len) + relativeStart, 0.0));
  } else {
    // Step 6. Else, let actualStart be min(relativeStart, len).
    *result = uint64_t(std::min(relativeStart, double(len)));
  }
  return true;
}

static uint32_t GetItemCount(const CallArgs& args) {
  if (args.length() < 2) {
    return 0;
  }
  return (args.length() - 2);
}

static bool GetActualDeleteCount(JSContext* cx, const CallArgs& args,
                                 HandleObject obj, uint64_t len,
                                 uint64_t actualStart, uint32_t insertCount,
                                 uint64_t* actualDeleteCount) {
  MOZ_ASSERT(len < DOUBLE_INTEGRAL_PRECISION_LIMIT);
  MOZ_ASSERT(actualStart <= len);
  MOZ_ASSERT(insertCount == GetItemCount(args));

  // Steps from proposal: https://github.com/tc39/proposal-change-array-by-copy
  // Array.prototype.toSpliced()

  if (args.length() < 1) {
    // Step 8. If start is not present, then let actualDeleteCount be 0.
    *actualDeleteCount = 0;
  } else if (args.length() < 2) {
    // Step 9. Else if deleteCount is not present, then let actualDeleteCount be
    // len - actualStart.
    *actualDeleteCount = len - actualStart;
  } else {
    // Step 10.a. Else, let dc be toIntegerOrInfinity(deleteCount).
    double deleteCount;
    if (!ToInteger(cx, args.get(1), &deleteCount)) {
      return false;
    }

    // Step 10.b. Let actualDeleteCount be the result of clamping dc between 0
    // and len - actualStart.
    *actualDeleteCount = uint64_t(
        std::min(std::max(0.0, deleteCount), double(len - actualStart)));
    MOZ_ASSERT(*actualDeleteCount <= len);

    // Step 11. Let newLen be len + insertCount - actualDeleteCount.
    // Step 12. If newLen > 2^53 - 1, throw a TypeError exception.
    if (len + uint64_t(insertCount) - *actualDeleteCount >=
        uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TOO_LONG_ARRAY);
      return false;
    }
  }
  MOZ_ASSERT(actualStart + *actualDeleteCount <= len);

  return true;
}

static bool array_splice_impl(JSContext* cx, unsigned argc, Value* vp,
                              bool returnValueIsUsed) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "splice");
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  /* Step 2. */
  uint64_t len;
  if (!GetLengthPropertyInlined(cx, obj, &len)) {
    return false;
  }

  /* Steps 3-6. */
  /* actualStart is the index after which elements will be
     deleted and/or new elements will be added */
  uint64_t actualStart;
  if (!GetActualStart(cx, args.get(0), len, &actualStart)) {
    return false;
  }

  /* Steps 7-10.*/
  /* itemCount is the number of elements being added */
  uint32_t itemCount = GetItemCount(args);

  /* actualDeleteCount is the number of elements being deleted */
  uint64_t actualDeleteCount;
  if (!GetActualDeleteCount(cx, args, obj, len, actualStart, itemCount,
                            &actualDeleteCount)) {
    return false;
  }

  RootedObject arr(cx);
  if (IsArraySpecies(cx, obj)) {
    if (actualDeleteCount > UINT32_MAX) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_ARRAY_LENGTH);
      return false;
    }
    uint32_t count = uint32_t(actualDeleteCount);

    if (CanOptimizeForDenseStorage<ArrayAccess::Read>(obj,
                                                      actualStart + count)) {
      MOZ_ASSERT(actualStart <= UINT32_MAX,
                 "if actualStart + count <= UINT32_MAX, then actualStart <= "
                 "UINT32_MAX");
      if (returnValueIsUsed) {
        /* Steps 11-13. */
        arr = CopyDenseArrayElements(cx, obj.as<NativeObject>(),
                                     uint32_t(actualStart), count);
        if (!arr) {
          return false;
        }
      }
    } else {
      /* Step 11. */
      arr = NewDenseFullyAllocatedArray(cx, count);
      if (!arr) {
        return false;
      }

      /* Steps 12-13. */
      if (!CopyArrayElements(cx, obj, actualStart, count,
                             arr.as<ArrayObject>())) {
        return false;
      }
    }
  } else {
    /* Step 11. */
    if (!ArraySpeciesCreate(cx, obj, actualDeleteCount, &arr)) {
      return false;
    }

    /* Steps 12-13. */
    RootedValue fromValue(cx);
    for (uint64_t k = 0; k < actualDeleteCount; k++) {
      if (!CheckForInterrupt(cx)) {
        return false;
      }

      /* Steps 13.b, 13.c.i. */
      bool hole;
      if (!HasAndGetElement(cx, obj, actualStart + k, &hole, &fromValue)) {
        return false;
      }

      /* Step 13.c. */
      if (!hole) {
        /* Step 13.c.ii. */
        if (!DefineArrayElement(cx, arr, k, fromValue)) {
          return false;
        }
      }
    }

    /* Step 14. */
    if (!SetLengthProperty(cx, arr, actualDeleteCount)) {
      return false;
    }
  }

  /* Step 15. */
  uint64_t finalLength = len - actualDeleteCount + itemCount;

  if (itemCount < actualDeleteCount) {
    /* Step 16: the array is being shrunk. */
    uint64_t sourceIndex = actualStart + actualDeleteCount;
    uint64_t targetIndex = actualStart + itemCount;

    if (CanOptimizeForDenseStorage<ArrayAccess::Write>(obj, len)) {
      MOZ_ASSERT(sourceIndex <= len && targetIndex <= len && len <= UINT32_MAX,
                 "sourceIndex and targetIndex are uint32 array indices");
      MOZ_ASSERT(finalLength < len, "finalLength is strictly less than len");
      MOZ_ASSERT(obj->is<NativeObject>());

      /* Step 16.b. */
      Handle<ArrayObject*> arr = obj.as<ArrayObject>();
      if (targetIndex != 0 || !arr->tryShiftDenseElements(sourceIndex)) {
        arr->moveDenseElements(uint32_t(targetIndex), uint32_t(sourceIndex),
                               uint32_t(len - sourceIndex));
      }

      /* Steps 20. */
      SetInitializedLength(cx, arr, finalLength);
    } else {
      /*
       * This is all very slow if the length is very large. We don't yet
       * have the ability to iterate in sorted order, so we just do the
       * pessimistic thing and let CheckForInterrupt handle the
       * fallout.
       */

      /* Step 16. */
      RootedValue fromValue(cx);
      for (uint64_t from = sourceIndex, to = targetIndex; from < len;
           from++, to++) {
        /* Steps 15.b.i-ii (implicit). */

        if (!CheckForInterrupt(cx)) {
          return false;
        }

        /* Steps 16.b.iii-v */
        bool hole;
        if (!HasAndGetElement(cx, obj, from, &hole, &fromValue)) {
          return false;
        }

        if (hole) {
          if (!DeletePropertyOrThrow(cx, obj, to)) {
            return false;
          }
        } else {
          if (!SetArrayElement(cx, obj, to, fromValue)) {
            return false;
          }
        }
      }

      /* Step 16d. */
      if (!DeletePropertiesOrThrow(cx, obj, len, finalLength)) {
        return false;
      }
    }
  } else if (itemCount > actualDeleteCount) {
    MOZ_ASSERT(actualDeleteCount <= UINT32_MAX);
    uint32_t deleteCount = uint32_t(actualDeleteCount);

    /* Step 17. */

    // Fast path for when we can simply extend and move the dense elements.
    auto extendElements = [len, itemCount, deleteCount](JSContext* cx,
                                                        HandleObject obj) {
      if (!obj->is<ArrayObject>()) {
        return DenseElementResult::Incomplete;
      }
      if (len > UINT32_MAX) {
        return DenseElementResult::Incomplete;
      }

      // Ensure there are no getters/setters or other extra indexed properties.
      if (ObjectMayHaveExtraIndexedProperties(obj)) {
        return DenseElementResult::Incomplete;
      }

      // Watch out for arrays with non-writable length or non-extensible arrays.
      // In these cases `splice` may have to throw an exception so we let the
      // slow path handle it. We also have to ensure we maintain the
      // |capacity <= initializedLength| invariant for such objects. See
      // NativeObject::shrinkCapacityToInitializedLength.
      Handle<ArrayObject*> arr = obj.as<ArrayObject>();
      if (!arr->lengthIsWritable() || !arr->isExtensible()) {
        return DenseElementResult::Incomplete;
      }

      // Also use the slow path if there might be an active for-in iterator so
      // that we don't have to worry about suppressing deleted properties.
      if (arr->denseElementsMaybeInIteration()) {
        return DenseElementResult::Incomplete;
      }

      return arr->ensureDenseElements(cx, uint32_t(len),
                                      itemCount - deleteCount);
    };

    DenseElementResult res = extendElements(cx, obj);
    if (res == DenseElementResult::Failure) {
      return false;
    }
    if (res == DenseElementResult::Success) {
      MOZ_ASSERT(finalLength <= UINT32_MAX);
      MOZ_ASSERT((actualStart + actualDeleteCount) <= len && len <= UINT32_MAX,
                 "start and deleteCount are uint32 array indices");
      MOZ_ASSERT(actualStart + itemCount <= UINT32_MAX,
                 "can't overflow because |len - actualDeleteCount + itemCount "
                 "<= UINT32_MAX| "
                 "and |actualStart <= len - actualDeleteCount| are both true");
      uint32_t start = uint32_t(actualStart);
      uint32_t length = uint32_t(len);

      Handle<ArrayObject*> arr = obj.as<ArrayObject>();
      arr->moveDenseElements(start + itemCount, start + deleteCount,
                             length - (start + deleteCount));

      /* Step 20. */
      SetInitializedLength(cx, arr, finalLength);
    } else {
      MOZ_ASSERT(res == DenseElementResult::Incomplete);

      RootedValue fromValue(cx);
      for (uint64_t k = len - actualDeleteCount; k > actualStart; k--) {
        if (!CheckForInterrupt(cx)) {
          return false;
        }

        /* Step 17.b.i. */
        uint64_t from = k + actualDeleteCount - 1;

        /* Step 17.b.ii. */
        uint64_t to = k + itemCount - 1;

        /* Steps 17.b.iii, 17.b.iv.1. */
        bool hole;
        if (!HasAndGetElement(cx, obj, from, &hole, &fromValue)) {
          return false;
        }

        /* Steps 17.b.iv. */
        if (hole) {
          /* Step 17.b.v.1. */
          if (!DeletePropertyOrThrow(cx, obj, to)) {
            return false;
          }
        } else {
          /* Step 17.b.iv.2. */
          if (!SetArrayElement(cx, obj, to, fromValue)) {
            return false;
          }
        }
      }
    }
  }

  Value* items = args.array() + 2;

  /* Steps 18-19. */
  if (!SetArrayElements(cx, obj, actualStart, itemCount, items)) {
    return false;
  }

  /* Step 20. */
  if (!SetLengthProperty(cx, obj, finalLength)) {
    return false;
  }

  /* Step 21. */
  if (returnValueIsUsed) {
    args.rval().setObject(*arr);
  }

  return true;
}

/* ES 2016 draft Mar 25, 2016 22.1.3.26. */
static bool array_splice(JSContext* cx, unsigned argc, Value* vp) {
  return array_splice_impl(cx, argc, vp, true);
}

static bool array_splice_noRetVal(JSContext* cx, unsigned argc, Value* vp) {
  return array_splice_impl(cx, argc, vp, false);
}

static void CopyDenseElementsFillHoles(ArrayObject* arr, NativeObject* nobj,
                                       uint32_t length) {
  // Ensure |arr| is an empty array with sufficient capacity.
  MOZ_ASSERT(arr->getDenseInitializedLength() == 0);
  MOZ_ASSERT(arr->getDenseCapacity() >= length);
  MOZ_ASSERT(length > 0);

  uint32_t count = std::min(nobj->getDenseInitializedLength(), length);

  if (count > 0) {
    if (nobj->denseElementsArePacked()) {
      // Copy all dense elements when no holes are present.
      arr->initDenseElements(nobj, 0, count);
    } else {
      arr->setDenseInitializedLength(count);

      // Handle each element separately to filter out holes.
      for (uint32_t i = 0; i < count; i++) {
        Value val = nobj->getDenseElement(i);
        if (val.isMagic(JS_ELEMENTS_HOLE)) {
          val = UndefinedValue();
        }
        arr->initDenseElement(i, val);
      }
    }
  }

  // Fill trailing holes with undefined.
  if (count < length) {
    arr->setDenseInitializedLength(length);

    for (uint32_t i = count; i < length; i++) {
      arr->initDenseElement(i, UndefinedValue());
    }
  }

  // Ensure |length| elements have been copied and no holes are present.
  MOZ_ASSERT(arr->getDenseInitializedLength() == length);
  MOZ_ASSERT(arr->denseElementsArePacked());
}

// https://github.com/tc39/proposal-change-array-by-copy
// Array.prototype.toSpliced()
static bool array_toSpliced(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "toSpliced");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. Let O be ? ToObject(this value).
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2. Let len be ? LengthOfArrayLike(O).
  uint64_t len;
  if (!GetLengthPropertyInlined(cx, obj, &len)) {
    return false;
  }

  // Steps 3-6.
  // |actualStart| is the index after which elements will be deleted and/or
  // new elements will be added
  uint64_t actualStart;
  if (!GetActualStart(cx, args.get(0), len, &actualStart)) {
    return false;
  }
  MOZ_ASSERT(actualStart <= len);

  // Step 7. Let insertCount be the number of elements in items.
  uint32_t insertCount = GetItemCount(args);

  // Steps 8-10.
  // actualDeleteCount is the number of elements being deleted
  uint64_t actualDeleteCount;
  if (!GetActualDeleteCount(cx, args, obj, len, actualStart, insertCount,
                            &actualDeleteCount)) {
    return false;
  }
  MOZ_ASSERT(actualStart + actualDeleteCount <= len);

  // Step 11. Let newLen be len + insertCount - actualDeleteCount.
  uint64_t newLen = len + insertCount - actualDeleteCount;

  // Step 12 handled by GetActualDeleteCount().
  MOZ_ASSERT(newLen < DOUBLE_INTEGRAL_PRECISION_LIMIT);
  MOZ_ASSERT(actualStart <= newLen,
             "if |actualStart + actualDeleteCount <= len| and "
             "|newLen = len + insertCount - actualDeleteCount|, then "
             "|actualStart <= newLen|");

  // ArrayCreate, step 1.
  if (newLen > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }

  // Step 13. Let A be ? ArrayCreate(𝔽(newLen)).
  Rooted<ArrayObject*> arr(cx,
                           NewDensePartlyAllocatedArray(cx, uint32_t(newLen)));
  if (!arr) {
    return false;
  }

  // Steps 14-19 optimized for dense elements.
  if (CanOptimizeForDenseStorage<ArrayAccess::Read>(obj, len)) {
    MOZ_ASSERT(len <= UINT32_MAX);
    MOZ_ASSERT(actualDeleteCount <= UINT32_MAX,
               "if |actualStart + actualDeleteCount <= len| and "
               "|len <= UINT32_MAX|, then |actualDeleteCount <= UINT32_MAX|");

    uint32_t length = uint32_t(len);
    uint32_t newLength = uint32_t(newLen);
    uint32_t start = uint32_t(actualStart);
    uint32_t deleteCount = uint32_t(actualDeleteCount);

    auto nobj = obj.as<NativeObject>();

    ArrayObject* arr = NewDenseFullyAllocatedArray(cx, newLength);
    if (!arr) {
      return false;
    }
    arr->setLength(newLength);

    // Below code doesn't handle the case when the storage has to grow,
    // therefore the capacity must fit for at least |newLength| elements.
    MOZ_ASSERT(arr->getDenseCapacity() >= newLength);

    if (deleteCount == 0 && insertCount == 0) {
      // Copy the array when we don't have to remove or insert any elements.
      if (newLength > 0) {
        CopyDenseElementsFillHoles(arr, nobj, newLength);
      }
    } else {
      // Copy nobj[0..start] to arr[0..start].
      if (start > 0) {
        CopyDenseElementsFillHoles(arr, nobj, start);
      }

      // Insert |items| into arr[start..(start + insertCount)].
      if (insertCount > 0) {
        auto items = HandleValueArray::subarray(args, 2, insertCount);

        // Prefer |initDenseElements| because it's faster.
        if (arr->getDenseInitializedLength() == 0) {
          arr->initDenseElements(items.begin(), items.length());
        } else {
          arr->ensureDenseInitializedLength(start, items.length());
          arr->copyDenseElements(start, items.begin(), items.length());
        }
      }

      uint32_t fromIndex = start + deleteCount;
      uint32_t toIndex = start + insertCount;
      MOZ_ASSERT((length - fromIndex) == (newLength - toIndex),
                 "Copies all remaining elements to the end");

      // Copy nobj[(start + deleteCount)..length] to
      // arr[(start + insertCount)..newLength].
      if (fromIndex < length) {
        uint32_t end = std::min(length, nobj->getDenseInitializedLength());
        if (fromIndex < end) {
          uint32_t count = end - fromIndex;
          if (nobj->denseElementsArePacked()) {
            // Copy all dense elements when no holes are present.
            const Value* src = nobj->getDenseElements() + fromIndex;
            arr->ensureDenseInitializedLength(toIndex, count);
            arr->copyDenseElements(toIndex, src, count);
            fromIndex += count;
            toIndex += count;
          } else {
            arr->setDenseInitializedLength(toIndex + count);

            // Handle each element separately to filter out holes.
            for (uint32_t i = 0; i < count; i++) {
              Value val = nobj->getDenseElement(fromIndex++);
              if (val.isMagic(JS_ELEMENTS_HOLE)) {
                val = UndefinedValue();
              }
              arr->initDenseElement(toIndex++, val);
            }
          }
        }

        arr->setDenseInitializedLength(newLength);

        // Fill trailing holes with undefined.
        while (fromIndex < length) {
          arr->initDenseElement(toIndex++, UndefinedValue());
          fromIndex++;
        }
      }

      MOZ_ASSERT(fromIndex == length);
      MOZ_ASSERT(toIndex == newLength);
    }

    // Ensure the result array is packed and has the correct length.
    MOZ_ASSERT(IsPackedArray(arr));
    MOZ_ASSERT(arr->length() == newLength);

    args.rval().setObject(*arr);
    return true;
  }

  // Copy everything before start

  // Step 14. Let i be 0.
  uint32_t i = 0;

  // Step 15. Let r be actualStart + actualDeleteCount.
  uint64_t r = actualStart + actualDeleteCount;

  // Step 16. Repeat while i < actualStart,
  RootedValue iValue(cx);
  while (i < uint32_t(actualStart)) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    // Skip Step 16.a. Let Pi be ! ToString(𝔽(i)).

    // Step 16.b. Let iValue be ? Get(O, Pi).
    if (!GetArrayElement(cx, obj, i, &iValue)) {
      return false;
    }

    // Step 16.c. Perform ! CreateDataPropertyOrThrow(A, Pi, iValue).
    if (!DefineArrayElement(cx, arr, i, iValue)) {
      return false;
    }

    // Step 16.d. Set i to i + 1.
    i++;
  }

  // Result array now contains all elements before start.

  // Copy new items
  if (insertCount > 0) {
    HandleValueArray items = HandleValueArray::subarray(args, 2, insertCount);

    // Fast-path to copy all items in one go.
    DenseElementResult result =
        arr->setOrExtendDenseElements(cx, i, items.begin(), items.length());
    if (result == DenseElementResult::Failure) {
      return false;
    }

    if (result == DenseElementResult::Success) {
      i += items.length();
    } else {
      MOZ_ASSERT(result == DenseElementResult::Incomplete);

      // Step 17. For each element E of items, do
      for (size_t j = 0; j < items.length(); j++) {
        if (!CheckForInterrupt(cx)) {
          return false;
        }

        // Skip Step 17.a. Let Pi be ! ToString(𝔽(i)).

        // Step 17.b. Perform ! CreateDataPropertyOrThrow(A, Pi, E).
        if (!DefineArrayElement(cx, arr, i, items[j])) {
          return false;
        }

        // Step 17.c. Set i to i + 1.
        i++;
      }
    }
  }

  // Copy items after new items
  // Step 18. Repeat, while i < newLen,
  RootedValue fromValue(cx);
  while (i < uint32_t(newLen)) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    // Skip Step 18.a. Let Pi be ! ToString(𝔽(i)).
    // Skip Step 18.b. Let from be ! ToString(𝔽(r)).

    // Step 18.c. Let fromValue be ? Get(O, from). */
    if (!GetArrayElement(cx, obj, r, &fromValue)) {
      return false;
    }

    // Step 18.d. Perform ! CreateDataPropertyOrThrow(A, Pi, fromValue).
    if (!DefineArrayElement(cx, arr, i, fromValue)) {
      return false;
    }

    // Step 18.e. Set i to i + 1.
    i++;

    // Step 18.f. Set r to r + 1.
    r++;
  }

  // Step 19. Return A.
  args.rval().setObject(*arr);
  return true;
}

// https://github.com/tc39/proposal-change-array-by-copy
// Array.prototype.with()
static bool array_with(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "with");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. Let O be ? ToObject(this value).
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2. Let len be ? LengthOfArrayLike(O).
  uint64_t len;
  if (!GetLengthPropertyInlined(cx, obj, &len)) {
    return false;
  }

  // Step 3. Let relativeIndex be ? ToIntegerOrInfinity(index).
  double relativeIndex;
  if (!ToInteger(cx, args.get(0), &relativeIndex)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
    return false;
  }

  // Step 4. If relativeIndex >= 0, let actualIndex be relativeIndex.
  double actualIndex = relativeIndex;
  if (actualIndex < 0) {
    // Step 5. Else, let actualIndex be len + relativeIndex.
    actualIndex = double(len) + actualIndex;
  }

  // Step 6. If actualIndex >= len or actualIndex < 0, throw a RangeError
  // exception.
  if (actualIndex < 0 || actualIndex >= double(len)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
    return false;
  }

  // ArrayCreate, step 1.
  if (len > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }
  uint32_t length = uint32_t(len);

  MOZ_ASSERT(length > 0);
  MOZ_ASSERT(0 <= actualIndex && actualIndex < UINT32_MAX);

  // Steps 7-10 optimized for dense elements.
  if (CanOptimizeForDenseStorage<ArrayAccess::Read>(obj, length)) {
    auto nobj = obj.as<NativeObject>();

    ArrayObject* arr = NewDenseFullyAllocatedArray(cx, length);
    if (!arr) {
      return false;
    }
    arr->setLength(length);

    CopyDenseElementsFillHoles(arr, nobj, length);

    // Replace the value at |actualIndex|.
    arr->setDenseElement(uint32_t(actualIndex), args.get(1));

    // Ensure the result array is packed and has the correct length.
    MOZ_ASSERT(IsPackedArray(arr));
    MOZ_ASSERT(arr->length() == length);

    args.rval().setObject(*arr);
    return true;
  }

  // Step 7. Let A be ? ArrayCreate(𝔽(len)).
  RootedObject arr(cx, NewDensePartlyAllocatedArray(cx, length));
  if (!arr) {
    return false;
  }

  // Steps 8-9. Let k be 0; Repeat, while k < len,
  RootedValue fromValue(cx);
  for (uint32_t k = 0; k < length; k++) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    // Skip Step 9.a. Let Pk be ! ToString(𝔽(k)).

    // Step 9.b. If k is actualIndex, let fromValue be value.
    if (k == uint32_t(actualIndex)) {
      fromValue = args.get(1);
    } else {
      // Step 9.c. Else, let fromValue be ? Get(O, 𝔽(k)).
      if (!GetArrayElement(cx, obj, k, &fromValue)) {
        return false;
      }
    }

    // Step 9.d. Perform ! CreateDataPropertyOrThrow(A, 𝔽(k), fromValue).
    if (!DefineArrayElement(cx, arr, k, fromValue)) {
      return false;
    }
  }

  // Step 10. Return A.
  args.rval().setObject(*arr);
  return true;
}

struct SortComparatorIndexes {
  bool operator()(uint32_t a, uint32_t b, bool* lessOrEqualp) {
    *lessOrEqualp = (a <= b);
    return true;
  }
};

// Returns all indexed properties in the range [begin, end) found on |obj| or
// its proto chain. This function does not handle proxies, objects with
// resolve/lookupProperty hooks or indexed getters, as those can introduce
// new properties. In those cases, *success is set to |false|.
static bool GetIndexedPropertiesInRange(JSContext* cx, HandleObject obj,
                                        uint64_t begin, uint64_t end,
                                        Vector<uint32_t>& indexes,
                                        bool* success) {
  *success = false;

  // TODO: Add IdIsIndex with support for large indices.
  if (end > UINT32_MAX) {
    return true;
  }
  MOZ_ASSERT(begin <= UINT32_MAX);

  // First, look for proxies or class hooks that can introduce extra
  // properties.
  JSObject* pobj = obj;
  do {
    if (!pobj->is<NativeObject>() || pobj->getClass()->getResolve() ||
        pobj->getOpsLookupProperty()) {
      return true;
    }
  } while ((pobj = pobj->staticPrototype()));

  // Collect indexed property names.
  pobj = obj;
  do {
    // Append dense elements.
    NativeObject* nativeObj = &pobj->as<NativeObject>();
    uint32_t initLen = nativeObj->getDenseInitializedLength();
    for (uint32_t i = begin; i < initLen && i < end; i++) {
      if (nativeObj->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE)) {
        continue;
      }
      if (!indexes.append(i)) {
        return false;
      }
    }

    // Append typed array elements.
    if (nativeObj->is<TypedArrayObject>()) {
      size_t len = nativeObj->as<TypedArrayObject>().length().valueOr(0);
      for (uint32_t i = begin; i < len && i < end; i++) {
        if (!indexes.append(i)) {
          return false;
        }
      }
    }

    // Append sparse elements.
    if (nativeObj->isIndexed()) {
      ShapePropertyIter<NoGC> iter(nativeObj->shape());
      for (; !iter.done(); iter++) {
        jsid id = iter->key();
        uint32_t i;
        if (!IdIsIndex(id, &i)) {
          continue;
        }

        if (!(begin <= i && i < end)) {
          continue;
        }

        // Watch out for getters, they can add new properties.
        if (!iter->isDataProperty()) {
          return true;
        }

        if (!indexes.append(i)) {
          return false;
        }
      }
    }
  } while ((pobj = pobj->staticPrototype()));

  // Sort the indexes.
  Vector<uint32_t> tmp(cx);
  size_t n = indexes.length();
  if (!tmp.resize(n)) {
    return false;
  }
  if (!MergeSort(indexes.begin(), n, tmp.begin(), SortComparatorIndexes())) {
    return false;
  }

  // Remove duplicates.
  if (!indexes.empty()) {
    uint32_t last = 0;
    for (size_t i = 1, len = indexes.length(); i < len; i++) {
      uint32_t elem = indexes[i];
      if (indexes[last] != elem) {
        last++;
        indexes[last] = elem;
      }
    }
    if (!indexes.resize(last + 1)) {
      return false;
    }
  }

  *success = true;
  return true;
}

static bool SliceSparse(JSContext* cx, HandleObject obj, uint64_t begin,
                        uint64_t end, Handle<ArrayObject*> result) {
  MOZ_ASSERT(begin <= end);

  Vector<uint32_t> indexes(cx);
  bool success;
  if (!GetIndexedPropertiesInRange(cx, obj, begin, end, indexes, &success)) {
    return false;
  }

  if (!success) {
    return CopyArrayElements(cx, obj, begin, end - begin, result);
  }

  MOZ_ASSERT(end <= UINT32_MAX,
             "indices larger than UINT32_MAX should be rejected by "
             "GetIndexedPropertiesInRange");

  RootedValue value(cx);
  for (uint32_t index : indexes) {
    MOZ_ASSERT(begin <= index && index < end);

    bool hole;
    if (!HasAndGetElement(cx, obj, index, &hole, &value)) {
      return false;
    }

    if (!hole &&
        !DefineDataElement(cx, result, index - uint32_t(begin), value)) {
      return false;
    }
  }

  return true;
}

static JSObject* SliceArguments(JSContext* cx, Handle<ArgumentsObject*> argsobj,
                                uint32_t begin, uint32_t count) {
  MOZ_ASSERT(!argsobj->hasOverriddenLength() &&
             !argsobj->hasOverriddenElement());
  MOZ_ASSERT(begin + count <= argsobj->initialLength());

  ArrayObject* result = NewDenseFullyAllocatedArray(cx, count);
  if (!result) {
    return nullptr;
  }
  result->setDenseInitializedLength(count);

  for (uint32_t index = 0; index < count; index++) {
    const Value& v = argsobj->element(begin + index);
    result->initDenseElement(index, v);
  }
  return result;
}

template <typename T, typename ArrayLength>
static inline ArrayLength NormalizeSliceTerm(T value, ArrayLength length) {
  if (value < 0) {
    value += length;
    if (value < 0) {
      return 0;
    }
  } else if (double(value) > double(length)) {
    return length;
  }
  return ArrayLength(value);
}

static bool ArraySliceOrdinary(JSContext* cx, HandleObject obj, uint64_t begin,
                               uint64_t end, MutableHandleValue rval) {
  if (begin > end) {
    begin = end;
  }

  if ((end - begin) > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }
  uint32_t count = uint32_t(end - begin);

  if (CanOptimizeForDenseStorage<ArrayAccess::Read>(obj, end)) {
    MOZ_ASSERT(begin <= UINT32_MAX,
               "if end <= UINT32_MAX, then begin <= UINT32_MAX");
    JSObject* narr = CopyDenseArrayElements(cx, obj.as<NativeObject>(),
                                            uint32_t(begin), count);
    if (!narr) {
      return false;
    }

    rval.setObject(*narr);
    return true;
  }

  if (obj->is<ArgumentsObject>()) {
    Handle<ArgumentsObject*> argsobj = obj.as<ArgumentsObject>();
    if (!argsobj->hasOverriddenLength() && !argsobj->hasOverriddenElement()) {
      MOZ_ASSERT(begin <= UINT32_MAX, "begin is limited by |argsobj|'s length");
      JSObject* narr = SliceArguments(cx, argsobj, uint32_t(begin), count);
      if (!narr) {
        return false;
      }

      rval.setObject(*narr);
      return true;
    }
  }

  Rooted<ArrayObject*> narr(cx, NewDensePartlyAllocatedArray(cx, count));
  if (!narr) {
    return false;
  }

  if (end <= UINT32_MAX) {
    if (js::GetElementsOp op = obj->getOpsGetElements()) {
      ElementAdder adder(cx, narr, count,
                         ElementAdder::CheckHasElemPreserveHoles);
      if (!op(cx, obj, uint32_t(begin), uint32_t(end), &adder)) {
        return false;
      }

      rval.setObject(*narr);
      return true;
    }
  }

  if (obj->is<NativeObject>() && obj->as<NativeObject>().isIndexed() &&
      count > 1000) {
    if (!SliceSparse(cx, obj, begin, end, narr)) {
      return false;
    }
  } else {
    if (!CopyArrayElements(cx, obj, begin, count, narr)) {
      return false;
    }
  }

  rval.setObject(*narr);
  return true;
}

/* ES 2016 draft Mar 25, 2016 22.1.3.23. */
static bool array_slice(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "slice");
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  /* Step 2. */
  uint64_t length;
  if (!GetLengthPropertyInlined(cx, obj, &length)) {
    return false;
  }

  uint64_t k = 0;
  uint64_t final = length;
  if (args.length() > 0) {
    double d;
    /* Step 3. */
    if (!ToInteger(cx, args[0], &d)) {
      return false;
    }

    /* Step 4. */
    k = NormalizeSliceTerm(d, length);

    if (args.hasDefined(1)) {
      /* Step 5. */
      if (!ToInteger(cx, args[1], &d)) {
        return false;
      }

      /* Step 6. */
      final = NormalizeSliceTerm(d, length);
    }
  }

  if (IsArraySpecies(cx, obj)) {
    /* Steps 7-12: Optimized for ordinary array. */
    return ArraySliceOrdinary(cx, obj, k, final, args.rval());
  }

  /* Step 7. */
  uint64_t count = final > k ? final - k : 0;

  /* Step 8. */
  RootedObject arr(cx);
  if (!ArraySpeciesCreate(cx, obj, count, &arr)) {
    return false;
  }

  /* Step 9. */
  uint64_t n = 0;

  /* Step 10. */
  RootedValue kValue(cx);
  while (k < final) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    /* Steps 10.a-b, and 10.c.i. */
    bool kNotPresent;
    if (!HasAndGetElement(cx, obj, k, &kNotPresent, &kValue)) {
      return false;
    }

    /* Step 10.c. */
    if (!kNotPresent) {
      /* Steps 10.c.ii. */
      if (!DefineArrayElement(cx, arr, n, kValue)) {
        return false;
      }
    }
    /* Step 10.d. */
    k++;

    /* Step 10.e. */
    n++;
  }

  /* Step 11. */
  if (!SetLengthProperty(cx, arr, n)) {
    return false;
  }

  /* Step 12. */
  args.rval().setObject(*arr);
  return true;
}

static bool ArraySliceDenseKernel(JSContext* cx, ArrayObject* arr,
                                  int32_t beginArg, int32_t endArg,
                                  ArrayObject* result) {
  uint32_t length = arr->length();

  uint32_t begin = NormalizeSliceTerm(beginArg, length);
  uint32_t end = NormalizeSliceTerm(endArg, length);

  if (begin > end) {
    begin = end;
  }

  uint32_t count = end - begin;
  size_t initlen = arr->getDenseInitializedLength();
  if (initlen > begin) {
    uint32_t newlength = std::min<uint32_t>(initlen - begin, count);
    if (newlength > 0) {
      if (!result->ensureElements(cx, newlength)) {
        return false;
      }
      result->initDenseElements(arr, begin, newlength);
    }
  }

  MOZ_ASSERT(count >= result->length());
  result->setLength(count);

  return true;
}

JSObject* js::ArraySliceDense(JSContext* cx, HandleObject obj, int32_t begin,
                              int32_t end, HandleObject result) {
  MOZ_ASSERT(IsPackedArray(obj));

  if (result && IsArraySpecies(cx, obj)) {
    if (!ArraySliceDenseKernel(cx, &obj->as<ArrayObject>(), begin, end,
                               &result->as<ArrayObject>())) {
      return nullptr;
    }
    return result;
  }

  // Slower path if the JIT wasn't able to allocate an object inline.
  JS::RootedValueArray<4> argv(cx);
  argv[0].setUndefined();
  argv[1].setObject(*obj);
  argv[2].setInt32(begin);
  argv[3].setInt32(end);
  if (!array_slice(cx, 2, argv.begin())) {
    return nullptr;
  }
  return &argv[0].toObject();
}

JSObject* js::ArgumentsSliceDense(JSContext* cx, HandleObject obj,
                                  int32_t begin, int32_t end,
                                  HandleObject result) {
  MOZ_ASSERT(obj->is<ArgumentsObject>());
  MOZ_ASSERT(IsArraySpecies(cx, obj));

  Handle<ArgumentsObject*> argsobj = obj.as<ArgumentsObject>();
  MOZ_ASSERT(!argsobj->hasOverriddenLength());
  MOZ_ASSERT(!argsobj->hasOverriddenElement());

  uint32_t length = argsobj->initialLength();
  uint32_t actualBegin = NormalizeSliceTerm(begin, length);
  uint32_t actualEnd = NormalizeSliceTerm(end, length);

  if (actualBegin > actualEnd) {
    actualBegin = actualEnd;
  }
  uint32_t count = actualEnd - actualBegin;

  if (result) {
    Handle<ArrayObject*> resArray = result.as<ArrayObject>();
    MOZ_ASSERT(resArray->getDenseInitializedLength() == 0);
    MOZ_ASSERT(resArray->length() == 0);

    if (count > 0) {
      if (!resArray->ensureElements(cx, count)) {
        return nullptr;
      }
      resArray->setDenseInitializedLength(count);
      resArray->setLength(count);

      for (uint32_t index = 0; index < count; index++) {
        const Value& v = argsobj->element(actualBegin + index);
        resArray->initDenseElement(index, v);
      }
    }

    return resArray;
  }

  // Slower path if the JIT wasn't able to allocate an object inline.
  return SliceArguments(cx, argsobj, actualBegin, count);
}

static bool array_isArray(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array", "isArray");
  CallArgs args = CallArgsFromVp(argc, vp);

  bool isArray = false;
  if (args.get(0).isObject()) {
    RootedObject obj(cx, &args[0].toObject());
    if (!IsArray(cx, obj, &isArray)) {
      return false;
    }
  }
  args.rval().setBoolean(isArray);
  return true;
}

static bool ArrayFromCallArgs(JSContext* cx, CallArgs& args,
                              HandleObject proto = nullptr) {
  ArrayObject* obj =
      NewDenseCopiedArrayWithProto(cx, args.length(), args.array(), proto);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool array_of(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array", "of");
  CallArgs args = CallArgsFromVp(argc, vp);

  bool isArrayConstructor =
      IsArrayConstructor(args.thisv()) &&
      args.thisv().toObject().nonCCWRealm() == cx->realm();

  if (isArrayConstructor || !IsConstructor(args.thisv())) {
    // isArrayConstructor will usually be true in practice. This is the most
    // common path.
    return ArrayFromCallArgs(cx, args);
  }

  if (!ReportUsageCounter(cx, nullptr, SUBCLASSING_ARRAY,
                          SUBCLASSING_TYPE_II)) {
    return false;
  }

  // Step 4.
  RootedObject obj(cx);
  {
    FixedConstructArgs<1> cargs(cx);

    cargs[0].setNumber(args.length());

    if (!Construct(cx, args.thisv(), cargs, args.thisv(), &obj)) {
      return false;
    }
  }

  // Step 8.
  for (unsigned k = 0; k < args.length(); k++) {
    if (!DefineDataElement(cx, obj, k, args[k])) {
      return false;
    }
  }

  // Steps 9-10.
  if (!SetLengthProperty(cx, obj, args.length())) {
    return false;
  }

  // Step 11.
  args.rval().setObject(*obj);
  return true;
}

static const JSJitInfo array_splice_info = {
    {(JSJitGetterOp)array_splice_noRetVal},
    {0}, /* unused */
    {0}, /* unused */
    JSJitInfo::IgnoresReturnValueNative,
    JSJitInfo::AliasEverything,
    JSVAL_TYPE_UNDEFINED,
};

enum class SearchKind {
  // Specializes SearchElementDense for Array.prototype.indexOf/lastIndexOf.
  // This means hole values are ignored and StrictlyEqual semantics are used.
  IndexOf,
  // Specializes SearchElementDense for Array.prototype.includes.
  // This means hole values are treated as |undefined| and SameValueZero
  // semantics are used.
  Includes,
};

template <SearchKind Kind, typename Iter>
static bool SearchElementDense(JSContext* cx, HandleValue val, Iter iterator,
                               MutableHandleValue rval) {
  // We assume here and in the iterator lambdas that nothing can trigger GC or
  // move dense elements.
  AutoCheckCannotGC nogc;

  // Fast path for string values.
  if (val.isString()) {
    JSLinearString* str = val.toString()->ensureLinear(cx);
    if (!str) {
      return false;
    }
    const uint32_t strLen = str->length();
    auto cmp = [str, strLen](JSContext* cx, const Value& element, bool* equal) {
      if (!element.isString() || element.toString()->length() != strLen) {
        *equal = false;
        return true;
      }
      JSLinearString* s = element.toString()->ensureLinear(cx);
      if (!s) {
        return false;
      }
      *equal = EqualStrings(str, s);
      return true;
    };
    return iterator(cx, cmp, rval);
  }

  // Fast path for numbers.
  if (val.isNumber()) {
    double dval = val.toNumber();
    // For |includes|, two NaN values are considered equal, so we use a
    // different implementation for NaN.
    if (Kind == SearchKind::Includes && std::isnan(dval)) {
      auto cmp = [](JSContext*, const Value& element, bool* equal) {
        *equal = (element.isDouble() && std::isnan(element.toDouble()));
        return true;
      };
      return iterator(cx, cmp, rval);
    }
    auto cmp = [dval](JSContext*, const Value& element, bool* equal) {
      *equal = (element.isNumber() && element.toNumber() == dval);
      return true;
    };
    return iterator(cx, cmp, rval);
  }

  // Fast path for values where we can use a simple bitwise comparison.
  if (CanUseBitwiseCompareForStrictlyEqual(val)) {
    // For |includes| we need to treat hole values as |undefined| so we use a
    // different path if searching for |undefined|.
    if (Kind == SearchKind::Includes && val.isUndefined()) {
      auto cmp = [](JSContext*, const Value& element, bool* equal) {
        *equal = (element.isUndefined() || element.isMagic(JS_ELEMENTS_HOLE));
        return true;
      };
      return iterator(cx, cmp, rval);
    }
    uint64_t bits = val.asRawBits();
    auto cmp = [bits](JSContext*, const Value& element, bool* equal) {
      *equal = (bits == element.asRawBits());
      return true;
    };
    return iterator(cx, cmp, rval);
  }

  MOZ_ASSERT(val.isBigInt() ||
             IF_RECORD_TUPLE(val.isExtendedPrimitive(), false));

  // Generic implementation for the remaining types.
  RootedValue elementRoot(cx);
  auto cmp = [val, &elementRoot](JSContext* cx, const Value& element,
                                 bool* equal) {
    if (MOZ_UNLIKELY(element.isMagic(JS_ELEMENTS_HOLE))) {
      // |includes| treats holes as |undefined|, but |undefined| is already
      // handled above. For |indexOf| we have to ignore holes.
      *equal = false;
      return true;
    }
    // Note: |includes| uses SameValueZero, but that checks for NaN and then
    // calls StrictlyEqual. Since we already handled NaN above, we can call
    // StrictlyEqual directly.
    MOZ_ASSERT(!val.isNumber());
    elementRoot = element;
    return StrictlyEqual(cx, val, elementRoot, equal);
  };
  return iterator(cx, cmp, rval);
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.1.3.14 Array.prototype.indexOf ( searchElement [ , fromIndex ] )
bool js::array_indexOf(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "indexOf");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2.
  uint64_t len;
  if (!GetLengthPropertyInlined(cx, obj, &len)) {
    return false;
  }

  // Step 3.
  if (len == 0) {
    args.rval().setInt32(-1);
    return true;
  }

  // Steps 4-8.
  uint64_t k = 0;
  if (args.length() > 1) {
    double n;
    if (!ToInteger(cx, args[1], &n)) {
      return false;
    }

    // Step 6.
    if (n >= double(len)) {
      args.rval().setInt32(-1);
      return true;
    }

    // Steps 7-8.
    if (n >= 0) {
      k = uint64_t(n);
    } else {
      double d = double(len) + n;
      if (d >= 0) {
        k = uint64_t(d);
      }
    }
  }

  MOZ_ASSERT(k < len);

  HandleValue searchElement = args.get(0);

  // Steps 9 and 10 optimized for dense elements.
  if (CanOptimizeForDenseStorage<ArrayAccess::Read>(obj, len)) {
    MOZ_ASSERT(len <= UINT32_MAX);

    NativeObject* nobj = &obj->as<NativeObject>();
    uint32_t start = uint32_t(k);
    uint32_t length =
        std::min(nobj->getDenseInitializedLength(), uint32_t(len));
    const Value* elements = nobj->getDenseElements();

    if (CanUseBitwiseCompareForStrictlyEqual(searchElement) && length > start) {
      const uint64_t* elementsAsBits =
          reinterpret_cast<const uint64_t*>(elements);
      const uint64_t* res = SIMD::memchr64(
          elementsAsBits + start, searchElement.asRawBits(), length - start);
      if (res) {
        args.rval().setInt32(static_cast<int32_t>(res - elementsAsBits));
      } else {
        args.rval().setInt32(-1);
      }
      return true;
    }

    auto iterator = [elements, start, length](JSContext* cx, auto cmp,
                                              MutableHandleValue rval) {
      static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT <= INT32_MAX,
                    "code assumes dense index fits in Int32Value");
      for (uint32_t i = start; i < length; i++) {
        bool equal;
        if (MOZ_UNLIKELY(!cmp(cx, elements[i], &equal))) {
          return false;
        }
        if (equal) {
          rval.setInt32(int32_t(i));
          return true;
        }
      }
      rval.setInt32(-1);
      return true;
    };
    return SearchElementDense<SearchKind::IndexOf>(cx, searchElement, iterator,
                                                   args.rval());
  }

  // Step 9.
  RootedValue v(cx);
  for (; k < len; k++) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    bool hole;
    if (!HasAndGetElement(cx, obj, k, &hole, &v)) {
      return false;
    }
    if (hole) {
      continue;
    }

    bool equal;
    if (!StrictlyEqual(cx, v, searchElement, &equal)) {
      return false;
    }
    if (equal) {
      args.rval().setNumber(k);
      return true;
    }
  }

  // Step 10.
  args.rval().setInt32(-1);
  return true;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.1.3.17 Array.prototype.lastIndexOf ( searchElement [ , fromIndex ] )
bool js::array_lastIndexOf(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "lastIndexOf");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2.
  uint64_t len;
  if (!GetLengthPropertyInlined(cx, obj, &len)) {
    return false;
  }

  // Step 3.
  if (len == 0) {
    args.rval().setInt32(-1);
    return true;
  }

  // Steps 4-6.
  uint64_t k = len - 1;
  if (args.length() > 1) {
    double n;
    if (!ToInteger(cx, args[1], &n)) {
      return false;
    }

    // Steps 5-6.
    if (n < 0) {
      double d = double(len) + n;
      if (d < 0) {
        args.rval().setInt32(-1);
        return true;
      }
      k = uint64_t(d);
    } else if (n < double(k)) {
      k = uint64_t(n);
    }
  }

  MOZ_ASSERT(k < len);

  HandleValue searchElement = args.get(0);

  // Steps 7 and 8 optimized for dense elements.
  if (CanOptimizeForDenseStorage<ArrayAccess::Read>(obj, k + 1)) {
    MOZ_ASSERT(k <= UINT32_MAX);

    NativeObject* nobj = &obj->as<NativeObject>();
    uint32_t initLen = nobj->getDenseInitializedLength();
    if (initLen == 0) {
      args.rval().setInt32(-1);
      return true;
    }

    uint32_t end = std::min(uint32_t(k), initLen - 1);
    const Value* elements = nobj->getDenseElements();

    auto iterator = [elements, end](JSContext* cx, auto cmp,
                                    MutableHandleValue rval) {
      static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT <= INT32_MAX,
                    "code assumes dense index fits in int32_t");
      for (int32_t i = int32_t(end); i >= 0; i--) {
        bool equal;
        if (MOZ_UNLIKELY(!cmp(cx, elements[i], &equal))) {
          return false;
        }
        if (equal) {
          rval.setInt32(int32_t(i));
          return true;
        }
      }
      rval.setInt32(-1);
      return true;
    };
    return SearchElementDense<SearchKind::IndexOf>(cx, searchElement, iterator,
                                                   args.rval());
  }

  // Step 7.
  RootedValue v(cx);
  for (int64_t i = int64_t(k); i >= 0; i--) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    bool hole;
    if (!HasAndGetElement(cx, obj, uint64_t(i), &hole, &v)) {
      return false;
    }
    if (hole) {
      continue;
    }

    bool equal;
    if (!StrictlyEqual(cx, v, searchElement, &equal)) {
      return false;
    }
    if (equal) {
      args.rval().setNumber(uint64_t(i));
      return true;
    }
  }

  // Step 8.
  args.rval().setInt32(-1);
  return true;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.1.3.13 Array.prototype.includes ( searchElement [ , fromIndex ] )
bool js::array_includes(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "includes");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2.
  uint64_t len;
  if (!GetLengthPropertyInlined(cx, obj, &len)) {
    return false;
  }

  // Step 3.
  if (len == 0) {
    args.rval().setBoolean(false);
    return true;
  }

  // Steps 4-7.
  uint64_t k = 0;
  if (args.length() > 1) {
    double n;
    if (!ToInteger(cx, args[1], &n)) {
      return false;
    }

    if (n >= double(len)) {
      args.rval().setBoolean(false);
      return true;
    }

    // Steps 6-7.
    if (n >= 0) {
      k = uint64_t(n);
    } else {
      double d = double(len) + n;
      if (d >= 0) {
        k = uint64_t(d);
      }
    }
  }

  MOZ_ASSERT(k < len);

  HandleValue searchElement = args.get(0);

  // Steps 8 and 9 optimized for dense elements.
  if (CanOptimizeForDenseStorage<ArrayAccess::Read>(obj, len)) {
    MOZ_ASSERT(len <= UINT32_MAX);

    NativeObject* nobj = &obj->as<NativeObject>();
    uint32_t start = uint32_t(k);
    uint32_t length =
        std::min(nobj->getDenseInitializedLength(), uint32_t(len));
    const Value* elements = nobj->getDenseElements();

    // Trailing holes are treated as |undefined|.
    if (uint32_t(len) > length && searchElement.isUndefined()) {
      // |undefined| is strictly equal only to |undefined|.
      args.rval().setBoolean(true);
      return true;
    }

    // For |includes| we need to treat hole values as |undefined| so we use a
    // different path if searching for |undefined|.
    if (CanUseBitwiseCompareForStrictlyEqual(searchElement) &&
        !searchElement.isUndefined() && length > start) {
      if (SIMD::memchr64(reinterpret_cast<const uint64_t*>(elements) + start,
                         searchElement.asRawBits(), length - start)) {
        args.rval().setBoolean(true);
      } else {
        args.rval().setBoolean(false);
      }
      return true;
    }

    auto iterator = [elements, start, length](JSContext* cx, auto cmp,
                                              MutableHandleValue rval) {
      for (uint32_t i = start; i < length; i++) {
        bool equal;
        if (MOZ_UNLIKELY(!cmp(cx, elements[i], &equal))) {
          return false;
        }
        if (equal) {
          rval.setBoolean(true);
          return true;
        }
      }
      rval.setBoolean(false);
      return true;
    };
    return SearchElementDense<SearchKind::Includes>(cx, searchElement, iterator,
                                                    args.rval());
  }

  // Step 8.
  RootedValue v(cx);
  for (; k < len; k++) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    if (!GetArrayElement(cx, obj, k, &v)) {
      return false;
    }

    bool equal;
    if (!SameValueZero(cx, v, searchElement, &equal)) {
      return false;
    }
    if (equal) {
      args.rval().setBoolean(true);
      return true;
    }
  }

  // Step 9.
  args.rval().setBoolean(false);
  return true;
}

// ES2024 draft 23.1.3.2.1 IsConcatSpreadable
static bool IsConcatSpreadable(JSContext* cx, HandleValue v, bool* spreadable) {
  // Step 1.
  if (!v.isObject()) {
    *spreadable = false;
    return true;
  }

  // Step 2.
  JS::Symbol* sym = cx->wellKnownSymbols().isConcatSpreadable;
  JSObject* holder;
  if (MOZ_UNLIKELY(
          MaybeHasInterestingSymbolProperty(cx, &v.toObject(), sym, &holder))) {
    RootedValue res(cx);
    RootedObject obj(cx, holder);
    Rooted<PropertyKey> key(cx, PropertyKey::Symbol(sym));
    if (!GetProperty(cx, obj, v, key, &res)) {
      return false;
    }
    // Step 3.
    if (!res.isUndefined()) {
      *spreadable = ToBoolean(res);
      return true;
    }
  }

  // Step 4.
  if (MOZ_LIKELY(v.toObject().is<ArrayObject>())) {
    *spreadable = true;
    return true;
  }
  RootedObject obj(cx, &v.toObject());
  bool isArray;
  if (!JS::IsArray(cx, obj, &isArray)) {
    return false;
  }
  *spreadable = isArray;
  return true;
}

// Returns true if the object may have an @@isConcatSpreadable property.
static bool MaybeHasIsConcatSpreadable(JSContext* cx, JSObject* obj) {
  JS::Symbol* sym = cx->wellKnownSymbols().isConcatSpreadable;
  JSObject* holder;
  return MaybeHasInterestingSymbolProperty(cx, obj, sym, &holder);
}

static bool TryOptimizePackedArrayConcat(JSContext* cx, CallArgs& args,
                                         Handle<JSObject*> obj,
                                         bool* optimized) {
  // Fast path for the following cases:
  //
  // (1) packedArray.concat(): copy the array's elements.
  // (2) packedArray.concat(packedArray): concatenate two packed arrays.
  // (3) packedArray.concat(value): copy and append a single non-array value.
  //
  // These cases account for almost all calls to Array.prototype.concat in
  // Speedometer 3.

  *optimized = false;

  if (args.length() > 1) {
    return true;
  }

  // The `this` object must be a packed array without @@isConcatSpreadable.
  // @@isConcatSpreadable is uncommon and requires a property lookup and more
  // complicated code, so we let the slow path handle it.
  if (!IsPackedArray(obj)) {
    return true;
  }
  if (MaybeHasIsConcatSpreadable(cx, obj)) {
    return true;
  }

  Handle<ArrayObject*> thisArr = obj.as<ArrayObject>();
  uint32_t thisLen = thisArr->length();

  if (args.length() == 0) {
    // Case (1). Copy the packed array.
    ArrayObject* arr = NewDenseFullyAllocatedArray(cx, thisLen);
    if (!arr) {
      return false;
    }
    arr->initDenseElements(thisArr->getDenseElements(), thisLen);
    args.rval().setObject(*arr);
    *optimized = true;
    return true;
  }

  MOZ_ASSERT(args.length() == 1);

  // If the argument is an object, it must not have an @@isConcatSpreadable
  // property.
  if (args[0].isObject() &&
      MaybeHasIsConcatSpreadable(cx, &args[0].toObject())) {
    return true;
  }

  MOZ_ASSERT_IF(args[0].isObject(), args[0].toObject().is<NativeObject>());

  // Case (3). Copy and append a single value if the argument is not an array.
  if (!args[0].isObject() || !args[0].toObject().is<ArrayObject>()) {
    ArrayObject* arr = NewDenseFullyAllocatedArray(cx, thisLen + 1);
    if (!arr) {
      return false;
    }
    arr->initDenseElements(thisArr->getDenseElements(), thisLen);

    arr->ensureDenseInitializedLength(thisLen, 1);
    arr->initDenseElement(thisLen, args[0]);

    args.rval().setObject(*arr);
    *optimized = true;
    return true;
  }

  // Case (2). Concatenate two packed arrays.
  if (!IsPackedArray(&args[0].toObject())) {
    return true;
  }

  uint32_t argLen = args[0].toObject().as<ArrayObject>().length();

  // Compute the array length. This can't overflow because both arrays are
  // packed.
  static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT < INT32_MAX);
  MOZ_ASSERT(thisLen <= NativeObject::MAX_DENSE_ELEMENTS_COUNT);
  MOZ_ASSERT(argLen <= NativeObject::MAX_DENSE_ELEMENTS_COUNT);
  uint32_t totalLen = thisLen + argLen;

  ArrayObject* arr = NewDenseFullyAllocatedArray(cx, totalLen);
  if (!arr) {
    return false;
  }
  arr->initDenseElements(thisArr->getDenseElements(), thisLen);

  ArrayObject* argArr = &args[0].toObject().as<ArrayObject>();
  arr->ensureDenseInitializedLength(thisLen, argLen);
  arr->initDenseElementRange(thisLen, argArr, argLen);

  args.rval().setObject(*arr);
  *optimized = true;
  return true;
}

static bool array_concat(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Array.prototype", "concat");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  bool isArraySpecies = IsArraySpecies(cx, obj);

  // Fast path for the most common cases.
  if (isArraySpecies) {
    bool optimized;
    if (!TryOptimizePackedArrayConcat(cx, args, obj, &optimized)) {
      return false;
    }
    if (optimized) {
      return true;
    }
  }

  // Step 2.
  RootedObject arr(cx);
  if (isArraySpecies) {
    arr = NewDenseEmptyArray(cx);
    if (!arr) {
      return false;
    }
  } else {
    if (!ArraySpeciesCreate(cx, obj, 0, &arr)) {
      return false;
    }
  }

  // Step 3.
  uint64_t n = 0;

  // Step 4 (handled implicitly with nextArg and CallArgs).
  uint32_t nextArg = 0;

  // Step 5.
  RootedValue v(cx, ObjectValue(*obj));
  while (true) {
    // Step 5.a.
    bool spreadable;
    if (!IsConcatSpreadable(cx, v, &spreadable)) {
      return false;
    }
    // Step 5.b.
    if (spreadable) {
      // Step 5.b.i.
      obj = &v.toObject();
      uint64_t len;
      if (!GetLengthPropertyInlined(cx, obj, &len)) {
        return false;
      }

      // Step 5.b.ii.
      if (n + len > uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT) - 1) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TOO_LONG_ARRAY);
        return false;
      }

      // Step 5.b.iii.
      uint64_t k = 0;

      // Step 5.b.iv.

      // Try a fast path for copying dense elements directly.
      bool optimized = false;
      if (len > 0 && isArraySpecies &&
          CanOptimizeForDenseStorage<ArrayAccess::Read>(obj, len) &&
          n + len <= NativeObject::MAX_DENSE_ELEMENTS_COUNT) {
        NativeObject* nobj = &obj->as<NativeObject>();
        ArrayObject* resArr = &arr->as<ArrayObject>();
        uint32_t count =
            std::min(uint32_t(len), nobj->getDenseInitializedLength());

        DenseElementResult res = resArr->ensureDenseElements(cx, n, count);
        if (res == DenseElementResult::Failure) {
          return false;
        }
        if (res == DenseElementResult::Success) {
          resArr->initDenseElementRange(n, nobj, count);
          n += len;
          optimized = true;
        } else {
          MOZ_ASSERT(res == DenseElementResult::Incomplete);
        }
      }

      if (!optimized) {
        // Step 5.b.iv.
        while (k < len) {
          if (!CheckForInterrupt(cx)) {
            return false;
          }

          // Step 5.b.iv.2.
          bool hole;
          if (!HasAndGetElement(cx, obj, k, &hole, &v)) {
            return false;
          }
          if (!hole) {
            // Step 5.b.iv.3.
            if (!DefineArrayElement(cx, arr, n, v)) {
              return false;
            }
          }

          // Step 5.b.iv.4.
          n++;

          // Step 5.b.iv.5.
          k++;
        }
      }
    } else {
      // Step 5.c.ii.
      if (n >= uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT) - 1) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TOO_LONG_ARRAY);
        return false;
      }

      // Step 5.c.iii.
      if (!DefineArrayElement(cx, arr, n, v)) {
        return false;
      }

      // Step 5.c.iv.
      n++;
    }

    // Move on to the next argument.
    if (nextArg == args.length()) {
      break;
    }
    v = args[nextArg];
    nextArg++;
  }

  // Step 6.
  if (!SetLengthProperty(cx, arr, n)) {
    return false;
  }

  // Step 7.
  args.rval().setObject(*arr);
  return true;
}

static const JSFunctionSpec array_methods[] = {
    JS_FN("toSource", array_toSource, 0, 0),
    JS_SELF_HOSTED_FN("toString", "ArrayToString", 0, 0),
    JS_FN("toLocaleString", array_toLocaleString, 0, 0),

    /* Perl-ish methods. */
    JS_INLINABLE_FN("join", array_join, 1, 0, ArrayJoin),
    JS_FN("reverse", array_reverse, 0, 0),
    JS_TRAMPOLINE_FN("sort", array_sort, 1, 0, ArraySort),
    JS_INLINABLE_FN("push", array_push, 1, 0, ArrayPush),
    JS_INLINABLE_FN("pop", array_pop, 0, 0, ArrayPop),
    JS_INLINABLE_FN("shift", array_shift, 0, 0, ArrayShift),
    JS_FN("unshift", array_unshift, 1, 0),
    JS_FNINFO("splice", array_splice, &array_splice_info, 2, 0),

    /* Pythonic sequence methods. */
    JS_FN("concat", array_concat, 1, 0),
    JS_INLINABLE_FN("slice", array_slice, 2, 0, ArraySlice),

    JS_FN("lastIndexOf", array_lastIndexOf, 1, 0),
    JS_FN("indexOf", array_indexOf, 1, 0),
    JS_SELF_HOSTED_FN("forEach", "ArrayForEach", 1, 0),
    JS_SELF_HOSTED_FN("map", "ArrayMap", 1, 0),
    JS_SELF_HOSTED_FN("filter", "ArrayFilter", 1, 0),
    JS_SELF_HOSTED_FN("reduce", "ArrayReduce", 1, 0),
    JS_SELF_HOSTED_FN("reduceRight", "ArrayReduceRight", 1, 0),
    JS_SELF_HOSTED_FN("some", "ArraySome", 1, 0),
    JS_SELF_HOSTED_FN("every", "ArrayEvery", 1, 0),

    /* ES6 additions */
    JS_SELF_HOSTED_FN("find", "ArrayFind", 1, 0),
    JS_SELF_HOSTED_FN("findIndex", "ArrayFindIndex", 1, 0),
    JS_SELF_HOSTED_FN("copyWithin", "ArrayCopyWithin", 3, 0),

    JS_SELF_HOSTED_FN("fill", "ArrayFill", 3, 0),

    JS_SELF_HOSTED_SYM_FN(iterator, "$ArrayValues", 0, 0),
    JS_SELF_HOSTED_FN("entries", "ArrayEntries", 0, 0),
    JS_SELF_HOSTED_FN("keys", "ArrayKeys", 0, 0),
    JS_SELF_HOSTED_FN("values", "$ArrayValues", 0, 0),

    /* ES7 additions */
    JS_FN("includes", array_includes, 1, 0),

    /* ES2020 */
    JS_SELF_HOSTED_FN("flatMap", "ArrayFlatMap", 1, 0),
    JS_SELF_HOSTED_FN("flat", "ArrayFlat", 0, 0),

    /* Proposal */
    JS_SELF_HOSTED_FN("at", "ArrayAt", 1, 0),
    JS_SELF_HOSTED_FN("findLast", "ArrayFindLast", 1, 0),
    JS_SELF_HOSTED_FN("findLastIndex", "ArrayFindLastIndex", 1, 0),

    JS_SELF_HOSTED_FN("toReversed", "ArrayToReversed", 0, 0),
    JS_SELF_HOSTED_FN("toSorted", "ArrayToSorted", 1, 0),
    JS_FN("toSpliced", array_toSpliced, 2, 0), JS_FN("with", array_with, 2, 0),

    JS_FS_END};

static const JSFunctionSpec array_static_methods[] = {
    JS_INLINABLE_FN("isArray", array_isArray, 1, 0, ArrayIsArray),
    JS_SELF_HOSTED_FN("from", "ArrayFrom", 3, 0),
    JS_SELF_HOSTED_FN("fromAsync", "ArrayFromAsync", 3, 0),
    JS_FN("of", array_of, 0, 0),

    JS_FS_END};

const JSPropertySpec array_static_props[] = {
    JS_SELF_HOSTED_SYM_GET(species, "$ArraySpecies", 0), JS_PS_END};

static inline bool ArrayConstructorImpl(JSContext* cx, CallArgs& args,
                                        bool isConstructor) {
  RootedObject proto(cx);
  if (isConstructor) {
    if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Array, &proto)) {
      return false;
    }
  }

  if (args.length() != 1 || !args[0].isNumber()) {
    return ArrayFromCallArgs(cx, args, proto);
  }

  uint32_t length;
  if (args[0].isInt32()) {
    int32_t i = args[0].toInt32();
    if (i < 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_ARRAY_LENGTH);
      return false;
    }
    length = uint32_t(i);
  } else {
    double d = args[0].toDouble();
    length = ToUint32(d);
    if (d != double(length)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_ARRAY_LENGTH);
      return false;
    }
  }

  ArrayObject* obj = NewDensePartlyAllocatedArrayWithProto(cx, length, proto);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/* ES5 15.4.2 */
bool js::ArrayConstructor(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "Array");
  CallArgs args = CallArgsFromVp(argc, vp);
  return ArrayConstructorImpl(cx, args, /* isConstructor = */ true);
}

bool js::array_construct(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "Array");
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(!args.isConstructing());
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isNumber());
  return ArrayConstructorImpl(cx, args, /* isConstructor = */ false);
}

ArrayObject* js::ArrayConstructorOneArg(JSContext* cx,
                                        Handle<ArrayObject*> templateObject,
                                        int32_t lengthInt) {
  // JIT code can call this with a template object from a different realm when
  // calling another realm's Array constructor.
  Maybe<AutoRealm> ar;
  if (cx->realm() != templateObject->realm()) {
    MOZ_ASSERT(cx->compartment() == templateObject->compartment());
    ar.emplace(cx, templateObject);
  }

  if (lengthInt < 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return nullptr;
  }

  uint32_t length = uint32_t(lengthInt);
  ArrayObject* res = NewDensePartlyAllocatedArray(cx, length);
  MOZ_ASSERT_IF(res, res->realm() == templateObject->realm());
  return res;
}

/*
 * Array allocation functions.
 */

static inline bool EnsureNewArrayElements(JSContext* cx, ArrayObject* obj,
                                          uint32_t length) {
  /*
   * If ensureElements creates dynamically allocated slots, then having
   * fixedSlots is a waste.
   */
  DebugOnly<uint32_t> cap = obj->getDenseCapacity();

  if (!obj->ensureElements(cx, length)) {
    return false;
  }

  MOZ_ASSERT_IF(cap, !obj->hasDynamicElements());

  return true;
}

template <uint32_t maxLength>
static MOZ_ALWAYS_INLINE ArrayObject* NewArrayWithShape(
    JSContext* cx, Handle<SharedShape*> shape, uint32_t length,
    NewObjectKind newKind, gc::AllocSite* site = nullptr) {
  // The shape must already have the |length| property defined on it.
  MOZ_ASSERT(shape->propMapLength() == 1);
  MOZ_ASSERT(shape->lastProperty().key() == NameToId(cx->names().length));

  gc::AllocKind allocKind = GuessArrayGCKind(length);
  MOZ_ASSERT(CanChangeToBackgroundAllocKind(allocKind, &ArrayObject::class_));
  allocKind = ForegroundToBackgroundAllocKind(allocKind);

  MOZ_ASSERT(shape->slotSpan() == 0);
  constexpr uint32_t slotSpan = 0;

  AutoSetNewObjectMetadata metadata(cx);
  ArrayObject* arr = ArrayObject::create(
      cx, allocKind, GetInitialHeap(newKind, &ArrayObject::class_, site), shape,
      length, slotSpan, metadata);
  if (!arr) {
    return nullptr;
  }

  if (maxLength > 0 &&
      !EnsureNewArrayElements(cx, arr, std::min(maxLength, length))) {
    return nullptr;
  }

  probes::CreateObject(cx, arr);
  return arr;
}

static SharedShape* GetArrayShapeWithProto(JSContext* cx, HandleObject proto) {
  // Get a shape with zero fixed slots, because arrays store the ObjectElements
  // header inline.
  Rooted<SharedShape*> shape(
      cx, SharedShape::getInitialShape(cx, &ArrayObject::class_, cx->realm(),
                                       TaggedProto(proto), /* nfixed = */ 0));
  if (!shape) {
    return nullptr;
  }

  // Add the |length| property and use the new shape as initial shape for new
  // arrays.
  if (shape->propMapLength() == 0) {
    shape = AddLengthProperty(cx, shape);
    if (!shape) {
      return nullptr;
    }
    SharedShape::insertInitialShape(cx, shape);
  } else {
    MOZ_ASSERT(shape->propMapLength() == 1);
    MOZ_ASSERT(shape->lastProperty().key() == NameToId(cx->names().length));
  }

  return shape;
}

SharedShape* GlobalObject::createArrayShapeWithDefaultProto(JSContext* cx) {
  MOZ_ASSERT(!cx->global()->data().arrayShapeWithDefaultProto);

  RootedObject proto(cx,
                     GlobalObject::getOrCreateArrayPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  SharedShape* shape = GetArrayShapeWithProto(cx, proto);
  if (!shape) {
    return nullptr;
  }

  cx->global()->data().arrayShapeWithDefaultProto.init(shape);
  return shape;
}

template <uint32_t maxLength>
static MOZ_ALWAYS_INLINE ArrayObject* NewArray(JSContext* cx, uint32_t length,
                                               NewObjectKind newKind,
                                               gc::AllocSite* site = nullptr) {
  Rooted<SharedShape*> shape(cx,
                             GlobalObject::getArrayShapeWithDefaultProto(cx));
  if (!shape) {
    return nullptr;
  }

  return NewArrayWithShape<maxLength>(cx, shape, length, newKind, site);
}

template <uint32_t maxLength>
static MOZ_ALWAYS_INLINE ArrayObject* NewArrayWithProto(JSContext* cx,
                                                        uint32_t length,
                                                        HandleObject proto,
                                                        NewObjectKind newKind) {
  Rooted<SharedShape*> shape(cx);
  if (!proto || proto == cx->global()->maybeGetArrayPrototype()) {
    shape = GlobalObject::getArrayShapeWithDefaultProto(cx);
  } else {
    shape = GetArrayShapeWithProto(cx, proto);
  }
  if (!shape) {
    return nullptr;
  }

  return NewArrayWithShape<maxLength>(cx, shape, length, newKind, nullptr);
}

static JSObject* CreateArrayPrototype(JSContext* cx, JSProtoKey key) {
  MOZ_ASSERT(key == JSProto_Array);
  RootedObject proto(cx, &cx->global()->getObjectPrototype());
  return NewArrayWithProto<0>(cx, 0, proto, TenuredObject);
}

static bool array_proto_finish(JSContext* cx, JS::HandleObject ctor,
                               JS::HandleObject proto) {
  // Add Array.prototype[@@unscopables]. ECMA-262 draft (2016 Mar 19) 22.1.3.32.
  RootedObject unscopables(cx,
                           NewPlainObjectWithProto(cx, nullptr, TenuredObject));
  if (!unscopables) {
    return false;
  }

  RootedValue value(cx, BooleanValue(true));
  if (!DefineDataProperty(cx, unscopables, cx->names().at, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().copyWithin, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().entries, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().fill, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().find, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().findIndex, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().findLast, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().findLastIndex, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().flat, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().flatMap, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().includes, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().keys, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().toReversed, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().toSorted, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().toSpliced, value) ||
      !DefineDataProperty(cx, unscopables, cx->names().values, value)) {
    return false;
  }

  RootedId id(cx, PropertyKey::Symbol(cx->wellKnownSymbols().unscopables));
  value.setObject(*unscopables);
  if (!DefineDataProperty(cx, proto, id, value, JSPROP_READONLY)) {
    return false;
  }

  // Mark Array prototype as having fuse property (@iterator for example).
  return JSObject::setHasFuseProperty(cx, proto);
}

static const JSClassOps ArrayObjectClassOps = {
    array_addProperty,  // addProperty
    nullptr,            // delProperty
    nullptr,            // enumerate
    nullptr,            // newEnumerate
    nullptr,            // resolve
    nullptr,            // mayResolve
    nullptr,            // finalize
    nullptr,            // call
    nullptr,            // construct
    nullptr,            // trace
};

static const ClassSpec ArrayObjectClassSpec = {
    GenericCreateConstructor<ArrayConstructor, 1, gc::AllocKind::FUNCTION,
                             &jit::JitInfo_Array>,
    CreateArrayPrototype,
    array_static_methods,
    array_static_props,
    array_methods,
    nullptr,
    array_proto_finish};

const JSClass ArrayObject::class_ = {
    "Array",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Array) | JSCLASS_DELAY_METADATA_BUILDER,
    &ArrayObjectClassOps, &ArrayObjectClassSpec};

ArrayObject* js::NewDenseEmptyArray(JSContext* cx) {
  return NewArray<0>(cx, 0, GenericObject);
}

ArrayObject* js::NewTenuredDenseEmptyArray(JSContext* cx) {
  return NewArray<0>(cx, 0, TenuredObject);
}

ArrayObject* js::NewDenseFullyAllocatedArray(
    JSContext* cx, uint32_t length, NewObjectKind newKind /* = GenericObject */,
    gc::AllocSite* site /* = nullptr */) {
  return NewArray<UINT32_MAX>(cx, length, newKind, site);
}

ArrayObject* js::NewDensePartlyAllocatedArray(
    JSContext* cx, uint32_t length,
    NewObjectKind newKind /* = GenericObject */) {
  return NewArray<ArrayObject::EagerAllocationMaxLength>(cx, length, newKind);
}

ArrayObject* js::NewDensePartlyAllocatedArrayWithProto(JSContext* cx,
                                                       uint32_t length,
                                                       HandleObject proto) {
  return NewArrayWithProto<ArrayObject::EagerAllocationMaxLength>(
      cx, length, proto, GenericObject);
}

ArrayObject* js::NewDenseUnallocatedArray(
    JSContext* cx, uint32_t length,
    NewObjectKind newKind /* = GenericObject */) {
  return NewArray<0>(cx, length, newKind);
}

// values must point at already-rooted Value objects
ArrayObject* js::NewDenseCopiedArray(
    JSContext* cx, uint32_t length, const Value* values,
    NewObjectKind newKind /* = GenericObject */) {
  ArrayObject* arr = NewArray<UINT32_MAX>(cx, length, newKind);
  if (!arr) {
    return nullptr;
  }

  arr->initDenseElements(values, length);
  return arr;
}

// values must point at already-rooted Value objects
ArrayObject* js::NewDenseCopiedArray(
    JSContext* cx, uint32_t length, JSLinearString** values,
    NewObjectKind newKind /* = GenericObject */) {
  ArrayObject* arr = NewArray<UINT32_MAX>(cx, length, newKind);
  if (!arr) {
    return nullptr;
  }

  arr->initDenseElements(values, length);
  return arr;
}

ArrayObject* js::NewDenseCopiedArrayWithProto(JSContext* cx, uint32_t length,
                                              const Value* values,
                                              HandleObject proto) {
  ArrayObject* arr =
      NewArrayWithProto<UINT32_MAX>(cx, length, proto, GenericObject);
  if (!arr) {
    return nullptr;
  }

  arr->initDenseElements(values, length);
  return arr;
}

ArrayObject* js::NewDenseFullyAllocatedArrayWithShape(
    JSContext* cx, uint32_t length, Handle<SharedShape*> shape) {
  AutoSetNewObjectMetadata metadata(cx);
  gc::AllocKind allocKind = GuessArrayGCKind(length);
  MOZ_ASSERT(CanChangeToBackgroundAllocKind(allocKind, &ArrayObject::class_));
  allocKind = ForegroundToBackgroundAllocKind(allocKind);

  gc::Heap heap = GetInitialHeap(GenericObject, &ArrayObject::class_);
  ArrayObject* arr = ArrayObject::create(cx, allocKind, heap, shape, length,
                                         shape->slotSpan(), metadata);
  if (!arr) {
    return nullptr;
  }

  if (!EnsureNewArrayElements(cx, arr, length)) {
    return nullptr;
  }

  probes::CreateObject(cx, arr);

  return arr;
}

// TODO(no-TI): clean up.
ArrayObject* js::NewArrayWithShape(JSContext* cx, uint32_t length,
                                   Handle<Shape*> shape) {
  // Ion can call this with a shape from a different realm when calling
  // another realm's Array constructor.
  Maybe<AutoRealm> ar;
  if (cx->realm() != shape->realm()) {
    MOZ_ASSERT(cx->compartment() == shape->compartment());
    ar.emplace(cx, shape);
  }

  return NewDenseFullyAllocatedArray(cx, length);
}

#ifdef DEBUG
bool js::ArrayInfo(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject obj(cx);

  for (unsigned i = 0; i < args.length(); i++) {
    HandleValue arg = args[i];

    UniqueChars bytes =
        DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, arg, nullptr);
    if (!bytes) {
      return false;
    }
    if (arg.isPrimitive() || !(obj = arg.toObjectOrNull())->is<ArrayObject>()) {
      fprintf(stderr, "%s: not array\n", bytes.get());
      continue;
    }
    fprintf(stderr, "%s: (len %u", bytes.get(),
            obj->as<ArrayObject>().length());
    fprintf(stderr, ", capacity %u", obj->as<ArrayObject>().getDenseCapacity());
    fputs(")\n", stderr);
  }

  args.rval().setUndefined();
  return true;
}
#endif

void js::ArraySpeciesLookup::initialize(JSContext* cx) {
  MOZ_ASSERT(state_ == State::Uninitialized);

  // Get the canonical Array.prototype.
  NativeObject* arrayProto = cx->global()->maybeGetArrayPrototype();

  // Leave the cache uninitialized if the Array class itself is not yet
  // initialized.
  if (!arrayProto) {
    return;
  }

  // Get the canonical Array constructor. The Array constructor must be
  // initialized if Array.prototype is initialized.
  JSObject& arrayCtorObject = cx->global()->getConstructor(JSProto_Array);
  JSFunction* arrayCtor = &arrayCtorObject.as<JSFunction>();

  // Shortcut returns below means Array[@@species] will never be
  // optimizable, set to disabled now, and clear it later when we succeed.
  state_ = State::Disabled;

  // Look up Array.prototype.constructor and ensure it's a data property.
  Maybe<PropertyInfo> ctorProp =
      arrayProto->lookup(cx, NameToId(cx->names().constructor));
  if (ctorProp.isNothing() || !ctorProp->isDataProperty()) {
    return;
  }

  // Get the referred value, and ensure it holds the canonical Array
  // constructor.
  JSFunction* ctorFun;
  if (!IsFunctionObject(arrayProto->getSlot(ctorProp->slot()), &ctorFun)) {
    return;
  }
  if (ctorFun != arrayCtor) {
    return;
  }

  // Look up the '@@species' value on Array
  Maybe<PropertyInfo> speciesProp = arrayCtor->lookup(
      cx, PropertyKey::Symbol(cx->wellKnownSymbols().species));
  if (speciesProp.isNothing() || !arrayCtor->hasGetter(*speciesProp)) {
    return;
  }

  // Get the referred value, ensure it holds the canonical Array[@@species]
  // function.
  uint32_t speciesGetterSlot = speciesProp->slot();
  JSObject* speciesGetter = arrayCtor->getGetter(speciesGetterSlot);
  if (!speciesGetter || !speciesGetter->is<JSFunction>()) {
    return;
  }
  JSFunction* speciesFun = &speciesGetter->as<JSFunction>();
  if (!IsSelfHostedFunctionWithName(speciesFun,
                                    cx->names().dollar_ArraySpecies_)) {
    return;
  }

  // Store raw pointers below. This is okay to do here, because all objects
  // are in the tenured heap.
  MOZ_ASSERT(!IsInsideNursery(arrayProto));
  MOZ_ASSERT(!IsInsideNursery(arrayCtor));
  MOZ_ASSERT(!IsInsideNursery(arrayCtor->shape()));
  MOZ_ASSERT(!IsInsideNursery(speciesFun));
  MOZ_ASSERT(!IsInsideNursery(arrayProto->shape()));

  state_ = State::Initialized;
  arrayProto_ = arrayProto;
  arrayConstructor_ = arrayCtor;
  arrayConstructorShape_ = arrayCtor->shape();
  arraySpeciesGetterSlot_ = speciesGetterSlot;
  canonicalSpeciesFunc_ = speciesFun;
  arrayProtoShape_ = arrayProto->shape();
  arrayProtoConstructorSlot_ = ctorProp->slot();
}

void js::ArraySpeciesLookup::reset() {
  AlwaysPoison(this, JS_RESET_VALUE_PATTERN, sizeof(*this),
               MemCheckKind::MakeUndefined);
  state_ = State::Uninitialized;
}

bool js::ArraySpeciesLookup::isArrayStateStillSane() {
  MOZ_ASSERT(state_ == State::Initialized);

  // Ensure that Array.prototype still has the expected shape.
  if (arrayProto_->shape() != arrayProtoShape_) {
    return false;
  }

  // Ensure that Array.prototype.constructor contains the canonical Array
  // constructor function.
  if (arrayProto_->getSlot(arrayProtoConstructorSlot_) !=
      ObjectValue(*arrayConstructor_)) {
    return false;
  }

  // Ensure that Array still has the expected shape.
  if (arrayConstructor_->shape() != arrayConstructorShape_) {
    return false;
  }

  // Ensure the species getter contains the canonical @@species function.
  JSObject* getter = arrayConstructor_->getGetter(arraySpeciesGetterSlot_);
  return getter == canonicalSpeciesFunc_;
}

bool js::ArraySpeciesLookup::tryOptimizeArray(JSContext* cx,
                                              ArrayObject* array) {
  if (state_ == State::Uninitialized) {
    // If the cache is not initialized, initialize it.
    initialize(cx);
  } else if (state_ == State::Initialized && !isArrayStateStillSane()) {
    // Otherwise, if the array state is no longer sane, reinitialize.
    reset();
    initialize(cx);
  }

  // If the cache is disabled or still uninitialized, don't bother trying to
  // optimize.
  if (state_ != State::Initialized) {
    return false;
  }

  // By the time we get here, we should have a sane array state.
  MOZ_ASSERT(isArrayStateStillSane());

  // Ensure |array|'s prototype is the actual Array.prototype.
  if (array->staticPrototype() != arrayProto_) {
    return false;
  }

  // Ensure the array does not define an own "constructor" property which may
  // shadow `Array.prototype.constructor`.

  // Most arrays don't define any additional own properties beside their
  // "length" property. If "length" is the last property, it must be the only
  // property, because it's non-configurable.
  MOZ_ASSERT(array->shape()->propMapLength() > 0);
  PropertyKey lengthKey = NameToId(cx->names().length);
  if (MOZ_LIKELY(array->getLastProperty().key() == lengthKey)) {
    MOZ_ASSERT(array->shape()->propMapLength() == 1, "Expected one property");
    return true;
  }

  // Fail if the array has an own "constructor" property.
  uint32_t index;
  if (array->shape()->lookup(cx, NameToId(cx->names().constructor), &index)) {
    return false;
  }

  return true;
}

JS_PUBLIC_API JSObject* JS::NewArrayObject(JSContext* cx,
                                           const HandleValueArray& contents) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(contents);

  return NewDenseCopiedArray(cx, contents.length(), contents.begin());
}

JS_PUBLIC_API JSObject* JS::NewArrayObject(JSContext* cx, size_t length) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return NewDenseFullyAllocatedArray(cx, length);
}

JS_PUBLIC_API bool JS::IsArrayObject(JSContext* cx, Handle<JSObject*> obj,
                                     bool* isArray) {
  return IsGivenTypeObject(cx, obj, ESClass::Array, isArray);
}

JS_PUBLIC_API bool JS::IsArrayObject(JSContext* cx, Handle<Value> value,
                                     bool* isArray) {
  if (!value.isObject()) {
    *isArray = false;
    return true;
  }

  Rooted<JSObject*> obj(cx, &value.toObject());
  return IsArrayObject(cx, obj, isArray);
}

JS_PUBLIC_API bool JS::GetArrayLength(JSContext* cx, Handle<JSObject*> obj,
                                      uint32_t* lengthp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  uint64_t len = 0;
  if (!GetLengthProperty(cx, obj, &len)) {
    return false;
  }

  if (len > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }

  *lengthp = uint32_t(len);
  return true;
}

JS_PUBLIC_API bool JS::SetArrayLength(JSContext* cx, Handle<JSObject*> obj,
                                      uint32_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  return SetLengthProperty(cx, obj, length);
}

ArrayObject* js::NewArrayWithNullProto(JSContext* cx) {
  Rooted<SharedShape*> shape(cx, GetArrayShapeWithProto(cx, nullptr));
  if (!shape) {
    return nullptr;
  }

  uint32_t length = 0;
  return ::NewArrayWithShape<0>(cx, shape, length, GenericObject);
}
