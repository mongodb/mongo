/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS Array interface. */

#ifndef jsarray_h
#define jsarray_h

#include "jsobj.h"
#include "jspubtd.h"

namespace js {
/* 2^32-2, inclusive */
const uint32_t MAX_ARRAY_INDEX = 4294967294u;
}

inline bool
js_IdIsIndex(jsid id, uint32_t* indexp)
{
    if (JSID_IS_INT(id)) {
        int32_t i = JSID_TO_INT(id);
        MOZ_ASSERT(i >= 0);
        *indexp = (uint32_t)i;
        return true;
    }

    if (MOZ_UNLIKELY(!JSID_IS_STRING(id)))
        return false;

    return js::StringIsArrayIndex(JSID_TO_ATOM(id), indexp);
}

extern JSObject*
js_InitArrayClass(JSContext* cx, js::HandleObject obj);

extern bool
js_InitContextBusyArrayTable(JSContext* cx);

namespace js {

class ArrayObject;

/* Create a dense array with no capacity allocated, length set to 0. */
extern ArrayObject * JS_FASTCALL
NewDenseEmptyArray(JSContext* cx, HandleObject proto = NullPtr(),
                   NewObjectKind newKind = GenericObject);

/*
 * Create a dense array with a set length, but without allocating space for the
 * contents. This is useful, e.g., when accepting length from the user.
 */
extern ArrayObject * JS_FASTCALL
NewDenseUnallocatedArray(ExclusiveContext* cx, uint32_t length, HandleObject proto = NullPtr(),
                         NewObjectKind newKind = GenericObject);

/*
 * Create a dense array with length and capacity == |length|, initialized length set to 0,
 * but with only |EagerAllocationMaxLength| elements allocated.
 */
extern ArrayObject * JS_FASTCALL
NewDensePartlyAllocatedArray(ExclusiveContext* cx, uint32_t length, HandleObject proto = NullPtr(),
                             NewObjectKind newKind = GenericObject);

/* Create a dense array with length and capacity == 'length', initialized length set to 0. */
extern ArrayObject * JS_FASTCALL
NewDenseFullyAllocatedArray(ExclusiveContext* cx, uint32_t length, HandleObject proto = NullPtr(),
                            NewObjectKind newKind = GenericObject);

enum AllocatingBehaviour {
    NewArray_Unallocating,
    NewArray_PartlyAllocating,
    NewArray_FullyAllocating
};

/*
 * Create a dense array with a set length, but only allocates space for the
 * contents if the length is not excessive.
 */
extern ArrayObject*
NewDenseArray(ExclusiveContext* cx, uint32_t length, HandleObjectGroup group,
              AllocatingBehaviour allocating);

/* Create a dense array with a copy of the dense array elements in src. */
extern ArrayObject*
NewDenseCopiedArray(JSContext* cx, uint32_t length, HandleArrayObject src,
                    uint32_t elementOffset, HandleObject proto = NullPtr());

/* Create a dense array from the given array values, which must be rooted */
extern ArrayObject*
NewDenseCopiedArray(JSContext* cx, uint32_t length, const Value* values,
                    HandleObject proto = NullPtr(), NewObjectKind newKind = GenericObject);

/* Create a dense array based on templateObject with the given length. */
extern ArrayObject*
NewDenseFullyAllocatedArrayWithTemplate(JSContext* cx, uint32_t length, JSObject* templateObject);

/* Create a dense array with the same copy-on-write elements as another object. */
extern JSObject*
NewDenseCopyOnWriteArray(JSContext* cx, HandleArrayObject templateObject, gc::InitialHeap heap);

/*
 * Determines whether a write to the given element on |obj| should fail because
 * |obj| is an Array with a non-writable length, and writing that element would
 * increase the length of the array.
 */
extern bool
WouldDefinePastNonwritableLength(ExclusiveContext* cx,
                                 HandleObject obj, uint32_t index, bool strict,
                                 bool* definesPast);

/*
 * Canonicalize |vp| to a uint32_t value potentially suitable for use as an
 * array length.
 */
extern bool
CanonicalizeArrayLengthValue(JSContext* cx, HandleValue v, uint32_t* canonicalized);

extern bool
GetLengthProperty(JSContext* cx, HandleObject obj, uint32_t* lengthp);

extern bool
SetLengthProperty(JSContext* cx, HandleObject obj, double length);

extern bool
ObjectMayHaveExtraIndexedProperties(JSObject* obj);

/*
 * Copy 'length' elements from aobj to vp.
 *
 * This function assumes 'length' is effectively the result of calling
 * js_GetLengthProperty on aobj. vp must point to rooted memory.
 */
extern bool
GetElements(JSContext* cx, HandleObject aobj, uint32_t length, js::Value* vp);

/* Natives exposed for optimization by the interpreter and JITs. */

extern bool
array_sort(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_push(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_pop(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_splice(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_splice_impl(JSContext* cx, unsigned argc, js::Value* vp, bool pop);

extern bool
array_concat(JSContext* cx, unsigned argc, js::Value* vp);

template <bool Locale>
JSString*
ArrayJoin(JSContext* cx, HandleObject obj, HandleLinearString sepstr, uint32_t length);

extern bool
array_concat_dense(JSContext* cx, Handle<ArrayObject*> arr1, Handle<ArrayObject*> arr2,
                   Handle<ArrayObject*> result);

bool
array_join(JSContext* cx, unsigned argc, js::Value* vp);

extern JSString*
array_join_impl(JSContext* cx, HandleValue array, HandleString sep);

extern void
ArrayShiftMoveElements(ArrayObject* obj);

extern bool
array_shift(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_unshift(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_slice(JSContext* cx, unsigned argc, js::Value* vp);

/*
 * Append the given (non-hole) value to the end of an array.  The array must be
 * a newborn array -- that is, one which has not been exposed to script for
 * arbitrary manipulation.  (This method optimizes on the assumption that
 * extending the array to accommodate the element will never make the array
 * sparse, which requires that the array be completely filled.)
 */
extern bool
NewbornArrayPush(JSContext* cx, HandleObject obj, const Value& v);

extern ArrayObject*
ArrayConstructorOneArg(JSContext* cx, HandleObjectGroup group, int32_t lengthInt);

} /* namespace js */

#ifdef DEBUG
extern bool
js_ArrayInfo(JSContext* cx, unsigned argc, js::Value* vp);
#endif

/* Array constructor native. Exposed only so the JIT can know its address. */
bool
js_Array(JSContext* cx, unsigned argc, js::Value* vp);

#endif /* jsarray_h */
