/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS Array interface. */

#ifndef builtin_Array_h
#define builtin_Array_h

#include "mozilla/Attributes.h"

#include "vm/JSObject.h"

namespace js {

enum class ArraySortResult : uint32_t;

namespace jit {
class TrampolineNativeFrameLayout;
}

class ArrayObject;

MOZ_ALWAYS_INLINE bool IdIsIndex(jsid id, uint32_t* indexp) {
  if (id.isInt()) {
    int32_t i = id.toInt();
    MOZ_ASSERT(i >= 0);
    *indexp = uint32_t(i);
    return true;
  }

  if (MOZ_UNLIKELY(!id.isAtom())) {
    return false;
  }

  JSAtom* atom = id.toAtom();
  return atom->isIndex(indexp);
}

// The methods below only create dense boxed arrays.

// Create a dense array with no capacity allocated, length set to 0, in the
// normal (i.e. non-tenured) heap.
extern ArrayObject* NewDenseEmptyArray(JSContext* cx);

// Create a dense array with no capacity allocated, length set to 0, in the
// tenured heap.
extern ArrayObject* NewTenuredDenseEmptyArray(JSContext* cx);

// Create a dense array with a set length, but without allocating space for the
// contents. This is useful, e.g., when accepting length from the user.
extern ArrayObject* NewDenseUnallocatedArray(
    JSContext* cx, uint32_t length, NewObjectKind newKind = GenericObject);

// Create a dense array with length and capacity == 'length', initialized length
// set to 0.
extern ArrayObject* NewDenseFullyAllocatedArray(
    JSContext* cx, uint32_t length, NewObjectKind newKind = GenericObject,
    gc::AllocSite* site = nullptr);

// Create a dense array with length == 'length', initialized length set to 0,
// and capacity == 'length' clamped to EagerAllocationMaxLength.
extern ArrayObject* NewDensePartlyAllocatedArray(
    JSContext* cx, uint32_t length, NewObjectKind newKind = GenericObject,
    gc::AllocSite* site = nullptr);

// Like NewDensePartlyAllocatedArray, but the array will have |proto| as
// prototype (or Array.prototype if |proto| is nullptr).
extern ArrayObject* NewDensePartlyAllocatedArrayWithProto(JSContext* cx,
                                                          uint32_t length,
                                                          HandleObject proto);

// Create a dense array from the given array values, which must be rooted.
extern ArrayObject* NewDenseCopiedArray(JSContext* cx, uint32_t length,
                                        const Value* values,
                                        NewObjectKind newKind = GenericObject);

// Create a dense array from the given (linear)string values, which must be
// rooted
extern ArrayObject* NewDenseCopiedArray(JSContext* cx, uint32_t length,
                                        JSLinearString** values,
                                        NewObjectKind newKind = GenericObject);

// Like NewDenseCopiedArray, but the array will have |proto| as prototype (or
// Array.prototype if |proto| is nullptr).
extern ArrayObject* NewDenseCopiedArrayWithProto(JSContext* cx, uint32_t length,
                                                 const Value* values,
                                                 HandleObject proto);

// Create a dense array with the given shape and length.
extern ArrayObject* NewDenseFullyAllocatedArrayWithShape(
    JSContext* cx, uint32_t length, Handle<SharedShape*> shape);

extern ArrayObject* NewArrayWithShape(JSContext* cx, uint32_t length,
                                      Handle<Shape*> shape);

extern bool ToLength(JSContext* cx, HandleValue v, uint64_t* out);

extern bool GetLengthProperty(JSContext* cx, HandleObject obj,
                              uint64_t* lengthp);

extern bool SetLengthProperty(JSContext* cx, HandleObject obj, uint32_t length);

/*
 * Copy 'length' elements from aobj to vp.
 *
 * This function assumes 'length' is effectively the result of calling
 * GetLengthProperty on aobj. vp must point to rooted memory.
 */
extern bool GetElements(JSContext* cx, HandleObject aobj, uint32_t length,
                        js::Value* vp);

/* Natives exposed for optimization by the interpreter and JITs. */

extern bool array_includes(JSContext* cx, unsigned argc, js::Value* vp);
extern bool array_indexOf(JSContext* cx, unsigned argc, js::Value* vp);
extern bool array_lastIndexOf(JSContext* cx, unsigned argc, js::Value* vp);
extern bool array_pop(JSContext* cx, unsigned argc, js::Value* vp);
extern bool array_join(JSContext* cx, unsigned argc, js::Value* vp);
extern bool array_sort(JSContext* cx, unsigned argc, js::Value* vp);

extern void ArrayShiftMoveElements(ArrayObject* arr);

extern JSObject* ArraySliceDense(JSContext* cx, HandleObject obj, int32_t begin,
                                 int32_t end, HandleObject result);

extern JSObject* ArgumentsSliceDense(JSContext* cx, HandleObject obj,
                                     int32_t begin, int32_t end,
                                     HandleObject result);

extern ArrayObject* NewArrayWithNullProto(JSContext* cx);

/*
 * Append the given (non-hole) value to the end of an array.  The array must be
 * a newborn array -- that is, one which has not been exposed to script for
 * arbitrary manipulation.  (This method optimizes on the assumption that
 * extending the array to accommodate the element will never make the array
 * sparse, which requires that the array be completely filled.)
 */
extern bool NewbornArrayPush(JSContext* cx, HandleObject obj, const Value& v);

extern ArrayObject* ArrayConstructorOneArg(JSContext* cx,
                                           Handle<ArrayObject*> templateObject,
                                           int32_t lengthInt,
                                           gc::AllocSite* site);

#ifdef DEBUG
extern bool ArrayInfo(JSContext* cx, unsigned argc, Value* vp);
#endif

/* Array constructor native. Exposed only so the JIT can know its address. */
extern bool ArrayConstructor(JSContext* cx, unsigned argc, Value* vp);

// Like Array constructor, but doesn't perform GetPrototypeFromConstructor.
extern bool array_construct(JSContext* cx, unsigned argc, Value* vp);

extern JSString* ArrayToSource(JSContext* cx, HandleObject obj);

extern bool IsCrossRealmArrayConstructor(JSContext* cx, JSObject* obj,
                                         bool* result);

extern bool ObjectMayHaveExtraIndexedOwnProperties(JSObject* obj);

extern bool ObjectMayHaveExtraIndexedProperties(JSObject* obj);

extern bool PrototypeMayHaveIndexedProperties(NativeObject* obj);

// JS::IsArray has multiple overloads, use js::IsArrayFromJit to disambiguate.
extern bool IsArrayFromJit(JSContext* cx, HandleObject obj, bool* isArray);

extern bool ArrayLengthGetter(JSContext* cx, HandleObject obj, HandleId id,
                              MutableHandleValue vp);

extern bool ArrayLengthSetter(JSContext* cx, HandleObject obj, HandleId id,
                              HandleValue v, ObjectOpResult& result);

extern ArraySortResult ArraySortFromJit(
    JSContext* cx, jit::TrampolineNativeFrameLayout* frame);

bool IsArrayConstructor(const JSObject* obj);

bool intrinsic_CanOptimizeArraySpecies(JSContext* cx, unsigned argc, Value* vp);

} /* namespace js */

#endif /* builtin_Array_h */
