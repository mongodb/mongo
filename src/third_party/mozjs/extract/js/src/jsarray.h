/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS Array interface. */

#ifndef jsarray_h
#define jsarray_h

#include "jspubtd.h"

#include "vm/ArrayObject.h"
#include "vm/JSObject.h"

namespace js {
/* 2^32-2, inclusive */
const uint32_t MAX_ARRAY_INDEX = 4294967294u;

MOZ_ALWAYS_INLINE bool
IdIsIndex(jsid id, uint32_t* indexp)
{
    if (JSID_IS_INT(id)) {
        int32_t i = JSID_TO_INT(id);
        MOZ_ASSERT(i >= 0);
        *indexp = (uint32_t)i;
        return true;
    }

    if (MOZ_UNLIKELY(!JSID_IS_STRING(id)))
        return false;

    JSAtom* atom = JSID_TO_ATOM(id);
    if (atom->length() == 0 || !JS7_ISDEC(atom->latin1OrTwoByteChar(0)))
        return false;

    return js::StringIsArrayIndex(atom, indexp);
}

// The methods below only create dense boxed arrays.

/* Create a dense array with no capacity allocated, length set to 0. */
extern ArrayObject * JS_FASTCALL
NewDenseEmptyArray(JSContext* cx, HandleObject proto = nullptr,
                   NewObjectKind newKind = GenericObject);

/*
 * Create a dense array with a set length, but without allocating space for the
 * contents. This is useful, e.g., when accepting length from the user.
 */
extern ArrayObject * JS_FASTCALL
NewDenseUnallocatedArray(JSContext* cx, uint32_t length, HandleObject proto = nullptr,
                         NewObjectKind newKind = GenericObject);

/*
 * Create a dense array with length and capacity == |length|, initialized length set to 0,
 * but with only |EagerAllocationMaxLength| elements allocated.
 */
extern ArrayObject * JS_FASTCALL
NewDensePartlyAllocatedArray(JSContext* cx, uint32_t length, HandleObject proto = nullptr,
                             NewObjectKind newKind = GenericObject);

/* Create a dense array with length and capacity == 'length', initialized length set to 0. */
extern ArrayObject * JS_FASTCALL
NewDenseFullyAllocatedArray(JSContext* cx, uint32_t length, HandleObject proto = nullptr,
                            NewObjectKind newKind = GenericObject);

/* Create a dense array from the given array values, which must be rooted */
extern ArrayObject*
NewDenseCopiedArray(JSContext* cx, uint32_t length, const Value* values,
                    HandleObject proto = nullptr, NewObjectKind newKind = GenericObject);

/* Create a dense array based on templateObject with the given length. */
extern ArrayObject*
NewDenseFullyAllocatedArrayWithTemplate(JSContext* cx, uint32_t length, JSObject* templateObject);

/* Create a dense array with the same copy-on-write elements as another object. */
extern ArrayObject*
NewDenseCopyOnWriteArray(JSContext* cx, HandleArrayObject templateObject, gc::InitialHeap heap);

extern ArrayObject*
NewFullyAllocatedArrayTryUseGroup(JSContext* cx, HandleObjectGroup group, size_t length,
                                  NewObjectKind newKind = GenericObject);

extern ArrayObject*
NewPartlyAllocatedArrayTryUseGroup(JSContext* cx, HandleObjectGroup group, size_t length);

extern ArrayObject*
NewFullyAllocatedArrayTryReuseGroup(JSContext* cx, HandleObject obj, size_t length,
                                    NewObjectKind newKind = GenericObject);

extern ArrayObject*
NewPartlyAllocatedArrayTryReuseGroup(JSContext* cx, HandleObject obj, size_t length);

extern ArrayObject*
NewFullyAllocatedArrayForCallingAllocationSite(JSContext* cx, size_t length,
                                               NewObjectKind newKind = GenericObject);

extern ArrayObject*
NewPartlyAllocatedArrayForCallingAllocationSite(JSContext* cx, size_t length, HandleObject proto);

extern ArrayObject*
NewCopiedArrayTryUseGroup(JSContext* cx, HandleObjectGroup group,
                          const Value* vp, size_t length,
                          NewObjectKind newKind = GenericObject,
                          ShouldUpdateTypes updateTypes = ShouldUpdateTypes::Update);

extern ArrayObject*
NewCopiedArrayForCallingAllocationSite(JSContext* cx, const Value* vp, size_t length,
                                       HandleObject proto = nullptr);

/*
 * Determines whether a write to the given element on |obj| should fail because
 * |obj| is an Array with a non-writable length, and writing that element would
 * increase the length of the array.
 */
extern bool
WouldDefinePastNonwritableLength(HandleNativeObject obj, uint32_t index);

extern bool
GetLengthProperty(JSContext* cx, HandleObject obj, uint32_t* lengthp);

extern bool
SetLengthProperty(JSContext* cx, HandleObject obj, uint32_t length);

/*
 * Copy 'length' elements from aobj to vp.
 *
 * This function assumes 'length' is effectively the result of calling
 * GetLengthProperty on aobj. vp must point to rooted memory.
 */
extern bool
GetElements(JSContext* cx, HandleObject aobj, uint32_t length, js::Value* vp);

/* Natives exposed for optimization by the interpreter and JITs. */

extern bool
intrinsic_ArrayNativeSort(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_push(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_pop(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_join(JSContext* cx, unsigned argc, js::Value* vp);

extern void
ArrayShiftMoveElements(NativeObject* obj);

extern bool
array_shift(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_unshift(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_slice(JSContext* cx, unsigned argc, js::Value* vp);

extern JSObject*
array_slice_dense(JSContext* cx, HandleObject obj, int32_t begin, int32_t end, HandleObject result);

extern bool
array_reverse(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
array_splice(JSContext* cx, unsigned argc, js::Value* vp);

extern const JSJitInfo array_splice_info;

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

#ifdef DEBUG
extern bool
ArrayInfo(JSContext* cx, unsigned argc, Value* vp);
#endif

/* Array constructor native. Exposed only so the JIT can know its address. */
extern bool
ArrayConstructor(JSContext* cx, unsigned argc, Value* vp);

// Like Array constructor, but doesn't perform GetPrototypeFromConstructor.
extern bool
array_construct(JSContext* cx, unsigned argc, Value* vp);

extern bool
IsWrappedArrayConstructor(JSContext* cx, const Value& v, bool* result);

class MOZ_NON_TEMPORARY_CLASS ArraySpeciesLookup final
{
    /*
     * An ArraySpeciesLookup holds the following:
     *
     *  Array.prototype (arrayProto_)
     *      To ensure that the incoming array has the standard proto.
     *
     *  Array.prototype's shape (arrayProtoShape_)
     *      To ensure that Array.prototype has not been modified.
     *
     *  Array (arrayConstructor_)
     *  Array's shape (arrayConstructorShape_)
     *       To ensure that Array has not been modified.
     *
     *  Array.prototype's slot number for constructor (arrayProtoConstructorSlot_)
     *      To quickly retrieve and ensure that the Array constructor
     *      stored in the slot has not changed.
     *
     *  Array's shape for the @@species getter. (arraySpeciesShape_)
     *  Array's canonical value for @@species (canonicalSpeciesFunc_)
     *      To quickly retrieve and ensure that the @@species getter for Array
     *      has not changed.
     */

    // Pointer to canonical Array.prototype and Array.
    NativeObject* arrayProto_;
    NativeObject* arrayConstructor_;

    // Shape of matching Array, and slot containing the @@species
    // property, and the canonical value.
    Shape* arrayConstructorShape_;
#ifdef DEBUG
    Shape* arraySpeciesShape_;
    JSFunction* canonicalSpeciesFunc_;
#endif

    // Shape of matching Array.prototype object, and slot containing the
    // constructor for it.
    Shape* arrayProtoShape_;
    uint32_t arrayProtoConstructorSlot_;

    enum class State : uint8_t {
        // Flags marking the lazy initialization of the above fields.
        Uninitialized,
        Initialized,

        // The disabled flag is set when we don't want to try optimizing
        // anymore because core objects were changed.
        Disabled
    };

    State state_;

    // Initialize the internal fields.
    void initialize(JSContext* cx);

    // Reset the cache.
    void reset();

    // Check if the global array-related objects have not been messed with
    // in a way that would disable this cache.
    bool isArrayStateStillSane();

  public:
    ArraySpeciesLookup() {
        reset();
    }

    // Try to optimize the @@species lookup for an array.
    bool tryOptimizeArray(JSContext* cx, ArrayObject* array);

    // Purge the cache and all info associated with it.
    void purge() {
        if (state_ == State::Initialized)
            reset();
    }
};

} /* namespace js */

#endif /* jsarray_h */
