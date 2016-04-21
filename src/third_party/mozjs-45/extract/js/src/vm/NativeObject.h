/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_NativeObject_h
#define vm_NativeObject_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stdint.h>

#include "jsfriendapi.h"
#include "jsobj.h"
#include "NamespaceImports.h"

#include "gc/Barrier.h"
#include "gc/Heap.h"
#include "gc/Marking.h"
#include "js/Value.h"
#include "vm/Shape.h"
#include "vm/String.h"
#include "vm/TypeInference.h"

namespace js {

class Shape;
class TenuringTracer;

/*
 * To really poison a set of values, using 'magic' or 'undefined' isn't good
 * enough since often these will just be ignored by buggy code (see bug 629974)
 * in debug builds and crash in release builds. Instead, we use a safe-for-crash
 * pointer.
 */
static MOZ_ALWAYS_INLINE void
Debug_SetValueRangeToCrashOnTouch(Value* beg, Value* end)
{
#ifdef DEBUG
    for (Value* v = beg; v != end; ++v)
        v->setObject(*reinterpret_cast<JSObject*>(0x42));
#endif
}

static MOZ_ALWAYS_INLINE void
Debug_SetValueRangeToCrashOnTouch(Value* vec, size_t len)
{
#ifdef DEBUG
    Debug_SetValueRangeToCrashOnTouch(vec, vec + len);
#endif
}

static MOZ_ALWAYS_INLINE void
Debug_SetValueRangeToCrashOnTouch(HeapValue* vec, size_t len)
{
#ifdef DEBUG
    Debug_SetValueRangeToCrashOnTouch((Value*) vec, len);
#endif
}

static MOZ_ALWAYS_INLINE void
Debug_SetSlotRangeToCrashOnTouch(HeapSlot* vec, uint32_t len)
{
#ifdef DEBUG
    Debug_SetValueRangeToCrashOnTouch((Value*) vec, len);
#endif
}

static MOZ_ALWAYS_INLINE void
Debug_SetSlotRangeToCrashOnTouch(HeapSlot* begin, HeapSlot* end)
{
#ifdef DEBUG
    Debug_SetValueRangeToCrashOnTouch((Value*) begin, end - begin);
#endif
}

class ArrayObject;

/*
 * ES6 20130308 draft 8.4.2.4 ArraySetLength.
 *
 * |id| must be "length", |attrs| are the attributes to be used for the newly-
 * changed length property, |value| is the value for the new length, and
 * |result| receives an error code if the change is invalid.
 */
extern bool
ArraySetLength(JSContext* cx, Handle<ArrayObject*> obj, HandleId id,
               unsigned attrs, HandleValue value, ObjectOpResult& result);

/*
 * Elements header used for native objects. The elements component of such objects
 * offers an efficient representation for all or some of the indexed properties
 * of the object, using a flat array of Values rather than a shape hierarchy
 * stored in the object's slots. This structure is immediately followed by an
 * array of elements, with the elements member in an object pointing to the
 * beginning of that array (the end of this structure).
 * See below for usage of this structure.
 *
 * The sets of properties represented by an object's elements and slots
 * are disjoint. The elements contain only indexed properties, while the slots
 * can contain both named and indexed properties; any indexes in the slots are
 * distinct from those in the elements. If isIndexed() is false for an object,
 * all indexed properties (if any) are stored in the dense elements.
 *
 * Indexes will be stored in the object's slots instead of its elements in
 * the following case:
 *  - there are more than MIN_SPARSE_INDEX slots total and the load factor
 *    (COUNT / capacity) is less than 0.25
 *  - a property is defined that has non-default property attributes.
 *
 * We track these pieces of metadata for dense elements:
 *  - The length property as a uint32_t, accessible for array objects with
 *    ArrayObject::{length,setLength}().  This is unused for non-arrays.
 *  - The number of element slots (capacity), gettable with
 *    getDenseCapacity().
 *  - The array's initialized length, accessible with
 *    getDenseInitializedLength().
 *
 * Holes in the array are represented by MagicValue(JS_ELEMENTS_HOLE) values.
 * These indicate indexes which are not dense properties of the array. The
 * property may, however, be held by the object's properties.
 *
 * The capacity and length of an object's elements are almost entirely
 * unrelated!  In general the length may be greater than, less than, or equal
 * to the capacity.  The first case occurs with |new Array(100)|.  The length
 * is 100, but the capacity remains 0 (indices below length and above capacity
 * must be treated as holes) until elements between capacity and length are
 * set.  The other two cases are common, depending upon the number of elements
 * in an array and the underlying allocator used for element storage.
 *
 * The only case in which the capacity and length of an object's elements are
 * related is when the object is an array with non-writable length.  In this
 * case the capacity is always less than or equal to the length.  This permits
 * JIT code to optimize away the check for non-writable length when assigning
 * to possibly out-of-range elements: such code already has to check for
 * |index < capacity|, and fallback code checks for non-writable length.
 *
 * The initialized length of an object specifies the number of elements that
 * have been initialized. All elements above the initialized length are
 * holes in the object, and the memory for all elements between the initialized
 * length and capacity is left uninitialized. The initialized length is some
 * value less than or equal to both the object's length and the object's
 * capacity.
 *
 * There is flexibility in exactly the value the initialized length must hold,
 * e.g. if an array has length 5, capacity 10, completely empty, it is valid
 * for the initialized length to be any value between zero and 5, as long as
 * the in memory values below the initialized length have been initialized with
 * a hole value. However, in such cases we want to keep the initialized length
 * as small as possible: if the object is known to have no hole values below
 * its initialized length, then it is "packed" and can be accessed much faster
 * by JIT code.
 *
 * Elements do not track property creation order, so enumerating the elements
 * of an object does not necessarily visit indexes in the order they were
 * created.
 */
class ObjectElements
{
  public:
    enum Flags {
        // Integers written to these elements must be converted to doubles.
        CONVERT_DOUBLE_ELEMENTS     = 0x1,

        // Present only if these elements correspond to an array with
        // non-writable length; never present for non-arrays.
        NONWRITABLE_ARRAY_LENGTH    = 0x2,

        // These elements are shared with another object and must be copied
        // before they can be changed. A pointer to the original owner of the
        // elements, which is immutable, is stored immediately after the
        // elements data. There is one case where elements can be written to
        // before being copied: when setting the CONVERT_DOUBLE_ELEMENTS flag
        // the shared elements may change (from ints to doubles) without
        // making a copy first.
        COPY_ON_WRITE               = 0x4,

        // For TypedArrays only: this TypedArray's storage is mapping shared
        // memory.  This is a static property of the TypedArray, set when it
        // is created and never changed.
        SHARED_MEMORY               = 0x8
    };

  private:
    friend class ::JSObject;
    friend class ArrayObject;
    friend class NativeObject;
    friend class TenuringTracer;

    friend bool js::SetIntegrityLevel(JSContext* cx, HandleObject obj, IntegrityLevel level);

    friend bool
    ArraySetLength(JSContext* cx, Handle<ArrayObject*> obj, HandleId id,
                   unsigned attrs, HandleValue value, ObjectOpResult& result);

    /* See Flags enum above. */
    uint32_t flags;

    /*
     * Number of initialized elements. This is <= the capacity, and for arrays
     * is <= the length. Memory for elements above the initialized length is
     * uninitialized, but values between the initialized length and the proper
     * length are conceptually holes.
     */
    uint32_t initializedLength;

    /* Number of allocated slots. */
    uint32_t capacity;

    /* 'length' property of array objects, unused for other objects. */
    uint32_t length;

    bool shouldConvertDoubleElements() const {
        return flags & CONVERT_DOUBLE_ELEMENTS;
    }
    void setShouldConvertDoubleElements() {
        // Note: allow isCopyOnWrite() here, see comment above.
        flags |= CONVERT_DOUBLE_ELEMENTS;
    }
    void clearShouldConvertDoubleElements() {
        MOZ_ASSERT(!isCopyOnWrite());
        flags &= ~CONVERT_DOUBLE_ELEMENTS;
    }
    bool hasNonwritableArrayLength() const {
        return flags & NONWRITABLE_ARRAY_LENGTH;
    }
    void setNonwritableArrayLength() {
        MOZ_ASSERT(!isCopyOnWrite());
        flags |= NONWRITABLE_ARRAY_LENGTH;
    }
    bool isCopyOnWrite() const {
        return flags & COPY_ON_WRITE;
    }
    void clearCopyOnWrite() {
        MOZ_ASSERT(isCopyOnWrite());
        flags &= ~COPY_ON_WRITE;
    }

  public:
    MOZ_CONSTEXPR ObjectElements(uint32_t capacity, uint32_t length)
      : flags(0), initializedLength(0), capacity(capacity), length(length)
    {}

    enum class SharedMemory {
        IsShared
    };

    MOZ_CONSTEXPR ObjectElements(uint32_t capacity, uint32_t length, SharedMemory shmem)
      : flags(SHARED_MEMORY), initializedLength(0), capacity(capacity), length(length)
    {}

    HeapSlot* elements() {
        return reinterpret_cast<HeapSlot*>(uintptr_t(this) + sizeof(ObjectElements));
    }
    const HeapSlot* elements() const {
        return reinterpret_cast<const HeapSlot*>(uintptr_t(this) + sizeof(ObjectElements));
    }
    static ObjectElements * fromElements(HeapSlot* elems) {
        return reinterpret_cast<ObjectElements*>(uintptr_t(elems) - sizeof(ObjectElements));
    }

    bool isSharedMemory() const {
        return flags & SHARED_MEMORY;
    }

    HeapPtrNativeObject& ownerObject() const {
        MOZ_ASSERT(isCopyOnWrite());
        return *(HeapPtrNativeObject*)(&elements()[initializedLength]);
    }

    static int offsetOfFlags() {
        return int(offsetof(ObjectElements, flags)) - int(sizeof(ObjectElements));
    }
    static int offsetOfInitializedLength() {
        return int(offsetof(ObjectElements, initializedLength)) - int(sizeof(ObjectElements));
    }
    static int offsetOfCapacity() {
        return int(offsetof(ObjectElements, capacity)) - int(sizeof(ObjectElements));
    }
    static int offsetOfLength() {
        return int(offsetof(ObjectElements, length)) - int(sizeof(ObjectElements));
    }

    static bool ConvertElementsToDoubles(JSContext* cx, uintptr_t elements);
    static bool MakeElementsCopyOnWrite(ExclusiveContext* cx, NativeObject* obj);

    // This is enough slots to store an object of this class. See the static
    // assertion below.
    static const size_t VALUES_PER_HEADER = 2;
};

static_assert(ObjectElements::VALUES_PER_HEADER * sizeof(HeapSlot) == sizeof(ObjectElements),
              "ObjectElements doesn't fit in the given number of slots");

/*
 * Shared singletons for objects with no elements.
 * emptyObjectElementsShared is used only for TypedArrays, when the TA
 * maps shared memory.
 */
extern HeapSlot* const emptyObjectElements;
extern HeapSlot* const emptyObjectElementsShared;

struct Class;
class GCMarker;
class Shape;

class NewObjectCache;

#ifdef DEBUG
static inline bool
IsObjectValueInCompartment(Value v, JSCompartment* comp);
#endif

// Operations which change an object's dense elements can either succeed, fail,
// or be unable to complete. For native objects, the latter is used when the
// object's elements must become sparse instead. The enum below is used for
// such operations, and for similar operations on unboxed arrays and methods
// that work on both kinds of objects.
enum class DenseElementResult {
    Failure,
    Success,
    Incomplete
};

/*
 * NativeObject specifies the internal implementation of a native object.
 *
 * Native objects extend the base implementation of an object with storage
 * for the object's named properties and indexed elements.
 *
 * These are stored separately from one another. Objects are followed by a
 * variable-sized array of values for inline storage, which may be used by
 * either properties of native objects (fixed slots), by elements (fixed
 * elements), or by other data for certain kinds of objects, such as
 * ArrayBufferObjects and TypedArrayObjects.
 *
 * Two native objects with the same shape are guaranteed to have the same
 * number of fixed slots.
 *
 * Named property storage can be split between fixed slots and a dynamically
 * allocated array (the slots member). For an object with N fixed slots, shapes
 * with slots [0..N-1] are stored in the fixed slots, and the remainder are
 * stored in the dynamic array. If all properties fit in the fixed slots, the
 * 'slots_' member is nullptr.
 *
 * Elements are indexed via the 'elements_' member. This member can point to
 * either the shared emptyObjectElements and emptyObjectElementsShared singletons,
 * into the inline value array (the address of the third value, to leave room
 * for a ObjectElements header;in this case numFixedSlots() is zero) or to
 * a dynamically allocated array.
 *
 * Slots and elements may both be non-empty. The slots may be either names or
 * indexes; no indexed property will be in both the slots and elements.
 */
class NativeObject : public JSObject
{
  protected:
    // Property layout description and other state.
    HeapPtrShape shape_;

    /* Slots for object properties. */
    js::HeapSlot* slots_;

    /* Slots for object dense elements. */
    js::HeapSlot* elements_;

    friend class ::JSObject;

  private:
    static void staticAsserts() {
        static_assert(sizeof(NativeObject) == sizeof(JSObject_Slots0),
                      "native object size must match GC thing size");
        static_assert(sizeof(NativeObject) == sizeof(shadow::Object),
                      "shadow interface must match actual implementation");
        static_assert(sizeof(NativeObject) % sizeof(Value) == 0,
                      "fixed slots after an object must be aligned");

        static_assert(offsetof(NativeObject, shape_) == offsetof(shadow::Object, shape),
                      "shadow shape must match actual shape");
        static_assert(offsetof(NativeObject, group_) == offsetof(shadow::Object, group),
                      "shadow type must match actual type");
        static_assert(offsetof(NativeObject, slots_) == offsetof(shadow::Object, slots),
                      "shadow slots must match actual slots");
        static_assert(offsetof(NativeObject, elements_) == offsetof(shadow::Object, _1),
                      "shadow placeholder must match actual elements");

        static_assert(MAX_FIXED_SLOTS <= Shape::FIXED_SLOTS_MAX,
                      "verify numFixedSlots() bitfield is big enough");
        static_assert(sizeof(NativeObject) + MAX_FIXED_SLOTS * sizeof(Value) == JSObject::MAX_BYTE_SIZE,
                      "inconsistent maximum object size");
    }

  public:
    Shape* lastProperty() const {
        MOZ_ASSERT(shape_);
        return shape_;
    }

    uint32_t propertyCount() const {
        return lastProperty()->entryCount();
    }

    bool hasShapeTable() const {
        return lastProperty()->hasTable();
    }

    HeapSlotArray getDenseElements() {
        return HeapSlotArray(elements_, !getElementsHeader()->isCopyOnWrite());
    }
    HeapSlotArray getDenseElementsAllowCopyOnWrite() {
        // Backdoor allowing direct access to copy on write elements.
        return HeapSlotArray(elements_, true);
    }
    const Value& getDenseElement(uint32_t idx) {
        MOZ_ASSERT(idx < getDenseInitializedLength());
        return elements_[idx];
    }
    bool containsDenseElement(uint32_t idx) {
        return idx < getDenseInitializedLength() && !elements_[idx].isMagic(JS_ELEMENTS_HOLE);
    }
    uint32_t getDenseInitializedLength() {
        return getElementsHeader()->initializedLength;
    }
    uint32_t getDenseCapacity() const {
        return getElementsHeader()->capacity;
    }

    bool isSharedMemory() const {
        return getElementsHeader()->isSharedMemory();
    }

    // Update the last property, keeping the number of allocated slots in sync
    // with the object's new slot span.
    bool setLastProperty(ExclusiveContext* cx, Shape* shape);

    // As for setLastProperty(), but allows the number of fixed slots to
    // change. This can only be used when fixed slots are being erased from the
    // object, and only when the object will not require dynamic slots to cover
    // the new properties.
    void setLastPropertyShrinkFixedSlots(Shape* shape);

    // As for setLastProperty(), but changes the class associated with the
    // object to a non-native one. This leaves the object with a type and shape
    // that are (temporarily) inconsistent.
    void setLastPropertyMakeNonNative(Shape* shape);

    // As for setLastProperty(), but changes the class associated with the
    // object to a native one. The object's type has already been changed, and
    // this brings the shape into sync with it.
    void setLastPropertyMakeNative(ExclusiveContext* cx, Shape* shape);

    // Newly-created TypedArrays that map a SharedArrayBuffer are
    // marked as shared by giving them an ObjectElements that has the
    // ObjectElements::SHARED_MEMORY flag set.
    void setIsSharedMemory() {
        MOZ_ASSERT(elements_ == emptyObjectElements);
        elements_ = emptyObjectElementsShared;
    }

  protected:
#ifdef DEBUG
    void checkShapeConsistency();
#else
    void checkShapeConsistency() { }
#endif

    Shape*
    replaceWithNewEquivalentShape(ExclusiveContext* cx,
                                  Shape* existingShape, Shape* newShape = nullptr,
                                  bool accessorShape = false);

    /*
     * Remove the last property of an object, provided that it is safe to do so
     * (the shape and previous shape do not carry conflicting information about
     * the object itself).
     */
    inline void removeLastProperty(ExclusiveContext* cx);
    inline bool canRemoveLastProperty();

    /*
     * Update the slot span directly for a dictionary object, and allocate
     * slots to cover the new span if necessary.
     */
    bool setSlotSpan(ExclusiveContext* cx, uint32_t span);

    bool toDictionaryMode(ExclusiveContext* cx);

  private:
    friend class TenuringTracer;

    /*
     * Get internal pointers to the range of values starting at start and
     * running for length.
     */
    void getSlotRangeUnchecked(uint32_t start, uint32_t length,
                               HeapSlot** fixedStart, HeapSlot** fixedEnd,
                               HeapSlot** slotsStart, HeapSlot** slotsEnd)
    {
        MOZ_ASSERT(start + length >= start);

        uint32_t fixed = numFixedSlots();
        if (start < fixed) {
            if (start + length < fixed) {
                *fixedStart = &fixedSlots()[start];
                *fixedEnd = &fixedSlots()[start + length];
                *slotsStart = *slotsEnd = nullptr;
            } else {
                uint32_t localCopy = fixed - start;
                *fixedStart = &fixedSlots()[start];
                *fixedEnd = &fixedSlots()[start + localCopy];
                *slotsStart = &slots_[0];
                *slotsEnd = &slots_[length - localCopy];
            }
        } else {
            *fixedStart = *fixedEnd = nullptr;
            *slotsStart = &slots_[start - fixed];
            *slotsEnd = &slots_[start - fixed + length];
        }
    }

    void getSlotRange(uint32_t start, uint32_t length,
                      HeapSlot** fixedStart, HeapSlot** fixedEnd,
                      HeapSlot** slotsStart, HeapSlot** slotsEnd)
    {
        MOZ_ASSERT(slotInRange(start + length, SENTINEL_ALLOWED));
        getSlotRangeUnchecked(start, length, fixedStart, fixedEnd, slotsStart, slotsEnd);
    }

  protected:
    friend class GCMarker;
    friend class Shape;
    friend class NewObjectCache;

    void invalidateSlotRange(uint32_t start, uint32_t length) {
#ifdef DEBUG
        HeapSlot* fixedStart;
        HeapSlot* fixedEnd;
        HeapSlot* slotsStart;
        HeapSlot* slotsEnd;
        getSlotRange(start, length, &fixedStart, &fixedEnd, &slotsStart, &slotsEnd);
        Debug_SetSlotRangeToCrashOnTouch(fixedStart, fixedEnd);
        Debug_SetSlotRangeToCrashOnTouch(slotsStart, slotsEnd);
#endif /* DEBUG */
    }

    void initializeSlotRange(uint32_t start, uint32_t count);

    /*
     * Initialize a flat array of slots to this object at a start slot.  The
     * caller must ensure that are enough slots.
     */
    void initSlotRange(uint32_t start, const Value* vector, uint32_t length);

    /*
     * Copy a flat array of slots to this object at a start slot. Caller must
     * ensure there are enough slots in this object.
     */
    void copySlotRange(uint32_t start, const Value* vector, uint32_t length);

#ifdef DEBUG
    enum SentinelAllowed {
        SENTINEL_NOT_ALLOWED,
        SENTINEL_ALLOWED
    };

    /*
     * Check that slot is in range for the object's allocated slots.
     * If sentinelAllowed then slot may equal the slot capacity.
     */
    bool slotInRange(uint32_t slot, SentinelAllowed sentinel = SENTINEL_NOT_ALLOWED) const;
#endif

    /*
     * Minimum size for dynamically allocated slots in normal Objects.
     * ArrayObjects don't use this limit and can have a lower slot capacity,
     * since they normally don't have a lot of slots.
     */
    static const uint32_t SLOT_CAPACITY_MIN = 8;

    HeapSlot* fixedSlots() const {
        return reinterpret_cast<HeapSlot*>(uintptr_t(this) + sizeof(NativeObject));
    }

  public:
    bool generateOwnShape(ExclusiveContext* cx, Shape* newShape = nullptr) {
        return replaceWithNewEquivalentShape(cx, lastProperty(), newShape);
    }

    bool shadowingShapeChange(ExclusiveContext* cx, const Shape& shape);
    bool clearFlag(ExclusiveContext* cx, BaseShape::Flag flag);

    // The maximum number of slots in an object.
    // |MAX_SLOTS_COUNT * sizeof(JS::Value)| shouldn't overflow
    // int32_t (see slotsSizeMustNotOverflow).
    static const uint32_t MAX_SLOTS_COUNT = (1 << 28) - 1;

    static void slotsSizeMustNotOverflow() {
        static_assert(NativeObject::MAX_SLOTS_COUNT <= INT32_MAX / sizeof(JS::Value),
                      "every caller of this method requires that a slot "
                      "number (or slot count) count multiplied by "
                      "sizeof(Value) can't overflow uint32_t (and sometimes "
                      "int32_t, too)");
    }

    uint32_t numFixedSlots() const {
        return reinterpret_cast<const shadow::Object*>(this)->numFixedSlots();
    }
    uint32_t numUsedFixedSlots() const {
        uint32_t nslots = lastProperty()->slotSpan(getClass());
        return Min(nslots, numFixedSlots());
    }

    uint32_t slotSpan() const {
        if (inDictionaryMode())
            return lastProperty()->base()->slotSpan();
        return lastProperty()->slotSpan();
    }

    /* Whether a slot is at a fixed offset from this object. */
    bool isFixedSlot(size_t slot) {
        return slot < numFixedSlots();
    }

    /* Index into the dynamic slots array to use for a dynamic slot. */
    size_t dynamicSlotIndex(size_t slot) {
        MOZ_ASSERT(slot >= numFixedSlots());
        return slot - numFixedSlots();
    }

    /*
     * Grow or shrink slots immediately before changing the slot span.
     * The number of allocated slots is not stored explicitly, and changes to
     * the slots must track changes in the slot span.
     */
    bool growSlots(ExclusiveContext* cx, uint32_t oldCount, uint32_t newCount);
    void shrinkSlots(ExclusiveContext* cx, uint32_t oldCount, uint32_t newCount);

    /*
     * This method is static because it's called from JIT code. On OOM, returns
     * false without leaving a pending exception on the context.
     */
    static bool growSlotsDontReportOOM(ExclusiveContext* cx, NativeObject* obj, uint32_t newCount);

    bool hasDynamicSlots() const { return !!slots_; }

    /* Compute dynamicSlotsCount() for this object. */
    uint32_t numDynamicSlots() const {
        return dynamicSlotsCount(numFixedSlots(), slotSpan(), getClass());
    }

    bool empty() const {
        return lastProperty()->isEmptyShape();
    }

    Shape* lookup(ExclusiveContext* cx, jsid id);
    Shape* lookup(ExclusiveContext* cx, PropertyName* name) {
        return lookup(cx, NameToId(name));
    }

    bool contains(ExclusiveContext* cx, jsid id) {
        return lookup(cx, id) != nullptr;
    }
    bool contains(ExclusiveContext* cx, PropertyName* name) {
        return lookup(cx, name) != nullptr;
    }
    bool contains(ExclusiveContext* cx, Shape* shape) {
        return lookup(cx, shape->propid()) == shape;
    }

    bool containsShapeOrElement(ExclusiveContext* cx, jsid id) {
        if (JSID_IS_INT(id) && containsDenseElement(JSID_TO_INT(id)))
            return true;
        return contains(cx, id);
    }

    /* Contextless; can be called from other pure code. */
    Shape* lookupPure(jsid id);
    Shape* lookupPure(PropertyName* name) {
        return lookupPure(NameToId(name));
    }

    bool containsPure(jsid id) {
        return lookupPure(id) != nullptr;
    }
    bool containsPure(PropertyName* name) {
        return containsPure(NameToId(name));
    }
    bool containsPure(Shape* shape) {
        return lookupPure(shape->propid()) == shape;
    }

    /*
     * Allocate and free an object slot.
     *
     * FIXME: bug 593129 -- slot allocation should be done by object methods
     * after calling object-parameter-free shape methods, avoiding coupling
     * logic across the object vs. shape module wall.
     */
    static bool allocSlot(ExclusiveContext* cx, HandleNativeObject obj, uint32_t* slotp);
    void freeSlot(uint32_t slot);

  private:
    static Shape* getChildPropertyOnDictionary(ExclusiveContext* cx, HandleNativeObject obj,
                                               HandleShape parent, MutableHandle<StackShape> child);
    static Shape* getChildProperty(ExclusiveContext* cx, HandleNativeObject obj,
                                   HandleShape parent, MutableHandle<StackShape> child);

  public:
    /* Add a property whose id is not yet in this scope. */
    static Shape* addProperty(ExclusiveContext* cx, HandleNativeObject obj, HandleId id,
                              JSGetterOp getter, JSSetterOp setter,
                              uint32_t slot, unsigned attrs, unsigned flags,
                              bool allowDictionary = true);

    /* Add a data property whose id is not yet in this scope. */
    Shape* addDataProperty(ExclusiveContext* cx,
                           jsid id_, uint32_t slot, unsigned attrs);
    Shape* addDataProperty(ExclusiveContext* cx, HandlePropertyName name,
                           uint32_t slot, unsigned attrs);

    /* Add or overwrite a property for id in this scope. */
    static Shape*
    putProperty(ExclusiveContext* cx, HandleNativeObject obj, HandleId id,
                JSGetterOp getter, JSSetterOp setter,
                uint32_t slot, unsigned attrs,
                unsigned flags);
    static inline Shape*
    putProperty(ExclusiveContext* cx, HandleObject obj, PropertyName* name,
                JSGetterOp getter, JSSetterOp setter,
                uint32_t slot, unsigned attrs,
                unsigned flags);

    /* Change the given property into a sibling with the same id in this scope. */
    static Shape*
    changeProperty(ExclusiveContext* cx, HandleNativeObject obj, HandleShape shape,
                   unsigned attrs, JSGetterOp getter, JSSetterOp setter);

    /* Remove the property named by id from this object. */
    bool removeProperty(ExclusiveContext* cx, jsid id);

    /* Clear the scope, making it empty. */
    static void clear(ExclusiveContext* cx, HandleNativeObject obj);

  protected:
    /*
     * Internal helper that adds a shape not yet mapped by this object.
     *
     * Notes:
     * 1. getter and setter must be normalized based on flags (see jsscope.cpp).
     * 2. Checks for non-extensibility must be done by callers.
     */
    static Shape*
    addPropertyInternal(ExclusiveContext* cx, HandleNativeObject obj, HandleId id,
                        JSGetterOp getter, JSSetterOp setter, uint32_t slot, unsigned attrs,
                        unsigned flags, ShapeTable::Entry* entry, bool allowDictionary);

    bool fillInAfterSwap(JSContext* cx, const Vector<Value>& values, void* priv);

  public:
    // Return true if this object has been converted from shared-immutable
    // prototype-rooted shape storage to dictionary-shapes in a doubly-linked
    // list.
    bool inDictionaryMode() const {
        return lastProperty()->inDictionary();
    }

    const Value& getSlot(uint32_t slot) const {
        MOZ_ASSERT(slotInRange(slot));
        uint32_t fixed = numFixedSlots();
        if (slot < fixed)
            return fixedSlots()[slot];
        return slots_[slot - fixed];
    }

    const HeapSlot* getSlotAddressUnchecked(uint32_t slot) const {
        uint32_t fixed = numFixedSlots();
        if (slot < fixed)
            return fixedSlots() + slot;
        return slots_ + (slot - fixed);
    }

    HeapSlot* getSlotAddressUnchecked(uint32_t slot) {
        uint32_t fixed = numFixedSlots();
        if (slot < fixed)
            return fixedSlots() + slot;
        return slots_ + (slot - fixed);
    }

    HeapSlot* getSlotAddress(uint32_t slot) {
        /*
         * This can be used to get the address of the end of the slots for the
         * object, which may be necessary when fetching zero-length arrays of
         * slots (e.g. for callObjVarArray).
         */
        MOZ_ASSERT(slotInRange(slot, SENTINEL_ALLOWED));
        return getSlotAddressUnchecked(slot);
    }

    const HeapSlot* getSlotAddress(uint32_t slot) const {
        /*
         * This can be used to get the address of the end of the slots for the
         * object, which may be necessary when fetching zero-length arrays of
         * slots (e.g. for callObjVarArray).
         */
        MOZ_ASSERT(slotInRange(slot, SENTINEL_ALLOWED));
        return getSlotAddressUnchecked(slot);
    }

    HeapSlot& getSlotRef(uint32_t slot) {
        MOZ_ASSERT(slotInRange(slot));
        return *getSlotAddress(slot);
    }

    const HeapSlot& getSlotRef(uint32_t slot) const {
        MOZ_ASSERT(slotInRange(slot));
        return *getSlotAddress(slot);
    }

    void setSlot(uint32_t slot, const Value& value) {
        MOZ_ASSERT(slotInRange(slot));
        MOZ_ASSERT(IsObjectValueInCompartment(value, compartment()));
        getSlotRef(slot).set(this, HeapSlot::Slot, slot, value);
    }

    void initSlot(uint32_t slot, const Value& value) {
        MOZ_ASSERT(getSlot(slot).isUndefined());
        MOZ_ASSERT(slotInRange(slot));
        MOZ_ASSERT(IsObjectValueInCompartment(value, compartment()));
        initSlotUnchecked(slot, value);
    }

    void initSlotUnchecked(uint32_t slot, const Value& value) {
        getSlotAddressUnchecked(slot)->init(this, HeapSlot::Slot, slot, value);
    }

    // MAX_FIXED_SLOTS is the biggest number of fixed slots our GC
    // size classes will give an object.
    static const uint32_t MAX_FIXED_SLOTS = 16;

  protected:
    inline bool updateSlotsForSpan(ExclusiveContext* cx, size_t oldSpan, size_t newSpan);

  public:
    /*
     * Trigger the write barrier on a range of slots that will no longer be
     * reachable.
     */
    void prepareSlotRangeForOverwrite(size_t start, size_t end) {
        for (size_t i = start; i < end; i++)
            getSlotAddressUnchecked(i)->HeapSlot::~HeapSlot();
    }

    void prepareElementRangeForOverwrite(size_t start, size_t end) {
        MOZ_ASSERT(end <= getDenseInitializedLength());
        MOZ_ASSERT(!denseElementsAreCopyOnWrite());
        for (size_t i = start; i < end; i++)
            elements_[i].HeapSlot::~HeapSlot();
    }

    static bool rollbackProperties(ExclusiveContext* cx, HandleNativeObject obj,
                                   uint32_t slotSpan);

    inline void setSlotWithType(ExclusiveContext* cx, Shape* shape,
                                const Value& value, bool overwriting = true);

    inline const Value& getReservedSlot(uint32_t index) const {
        MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
        return getSlot(index);
    }

    const HeapSlot& getReservedSlotRef(uint32_t index) const {
        MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
        return getSlotRef(index);
    }

    HeapSlot& getReservedSlotRef(uint32_t index) {
        MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
        return getSlotRef(index);
    }

    void initReservedSlot(uint32_t index, const Value& v) {
        MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
        initSlot(index, v);
    }

    void setReservedSlot(uint32_t index, const Value& v) {
        MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
        setSlot(index, v);
    }

    /* For slots which are known to always be fixed, due to the way they are allocated. */

    HeapSlot& getFixedSlotRef(uint32_t slot) {
        MOZ_ASSERT(slot < numFixedSlots());
        return fixedSlots()[slot];
    }

    const Value& getFixedSlot(uint32_t slot) const {
        MOZ_ASSERT(slot < numFixedSlots());
        return fixedSlots()[slot];
    }

    void setFixedSlot(uint32_t slot, const Value& value) {
        MOZ_ASSERT(slot < numFixedSlots());
        fixedSlots()[slot].set(this, HeapSlot::Slot, slot, value);
    }

    void initFixedSlot(uint32_t slot, const Value& value) {
        MOZ_ASSERT(slot < numFixedSlots());
        fixedSlots()[slot].init(this, HeapSlot::Slot, slot, value);
    }

    /*
     * Get the number of dynamic slots to allocate to cover the properties in
     * an object with the given number of fixed slots and slot span. The slot
     * capacity is not stored explicitly, and the allocated size of the slot
     * array is kept in sync with this count.
     */
    static uint32_t dynamicSlotsCount(uint32_t nfixed, uint32_t span, const Class* clasp);
    static uint32_t dynamicSlotsCount(Shape* shape) {
        return dynamicSlotsCount(shape->numFixedSlots(), shape->slotSpan(), shape->getObjectClass());
    }

    /* Elements accessors. */

    // The maximum size, in sizeof(Value), of the allocation used for an
    // object's dense elements.  (This includes space used to store an
    // ObjectElements instance.)
    // |MAX_DENSE_ELEMENTS_ALLOCATION * sizeof(JS::Value)| shouldn't overflow
    // int32_t (see elementsSizeMustNotOverflow).
    static const uint32_t MAX_DENSE_ELEMENTS_ALLOCATION = (1 << 28) - 1;

    // The maximum number of usable dense elements in an object.
    static const uint32_t MAX_DENSE_ELEMENTS_COUNT =
        MAX_DENSE_ELEMENTS_ALLOCATION - ObjectElements::VALUES_PER_HEADER;

    static void elementsSizeMustNotOverflow() {
        static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT <= INT32_MAX / sizeof(JS::Value),
                      "every caller of this method require that an element "
                      "count multiplied by sizeof(Value) can't overflow "
                      "uint32_t (and sometimes int32_t ,too)");
    }

    ObjectElements * getElementsHeader() const {
        return ObjectElements::fromElements(elements_);
    }

    /* Accessors for elements. */
    bool ensureElements(ExclusiveContext* cx, uint32_t capacity) {
        MOZ_ASSERT(!denseElementsAreCopyOnWrite());
        if (capacity > getDenseCapacity())
            return growElements(cx, capacity);
        return true;
    }

    static bool goodElementsAllocationAmount(ExclusiveContext* cx, uint32_t reqAllocated,
                                             uint32_t length, uint32_t* goodAmount);
    bool growElements(ExclusiveContext* cx, uint32_t newcap);
    void shrinkElements(ExclusiveContext* cx, uint32_t cap);
    void setDynamicElements(ObjectElements* header) {
        MOZ_ASSERT(!hasDynamicElements());
        elements_ = header->elements();
        MOZ_ASSERT(hasDynamicElements());
    }

    static bool CopyElementsForWrite(ExclusiveContext* cx, NativeObject* obj);

    bool maybeCopyElementsForWrite(ExclusiveContext* cx) {
        if (denseElementsAreCopyOnWrite())
            return CopyElementsForWrite(cx, this);
        return true;
    }

  private:
    inline void ensureDenseInitializedLengthNoPackedCheck(ExclusiveContext* cx,
                                                          uint32_t index, uint32_t extra);

    // Run a post write barrier that encompasses multiple contiguous elements in a
    // single step.
    inline void elementsRangeWriteBarrierPost(uint32_t start, uint32_t count) {
        for (size_t i = 0; i < count; i++) {
            const Value& v = elements_[start + i];
            if (v.isObject() && IsInsideNursery(&v.toObject())) {
                JS::shadow::Runtime* shadowRuntime = shadowRuntimeFromMainThread();
                shadowRuntime->gcStoreBufferPtr()->putSlot(this, HeapSlot::Element,
                                                           start + i, count - i);
                return;
            }
        }
    }

  public:
    void setDenseInitializedLength(uint32_t length) {
        MOZ_ASSERT(length <= getDenseCapacity());
        MOZ_ASSERT(!denseElementsAreCopyOnWrite());
        prepareElementRangeForOverwrite(length, getElementsHeader()->initializedLength);
        getElementsHeader()->initializedLength = length;
    }

    inline void ensureDenseInitializedLength(ExclusiveContext* cx,
                                             uint32_t index, uint32_t extra);
    void setDenseElement(uint32_t index, const Value& val) {
        MOZ_ASSERT(index < getDenseInitializedLength());
        MOZ_ASSERT(!denseElementsAreCopyOnWrite());
        elements_[index].set(this, HeapSlot::Element, index, val);
    }

    void initDenseElement(uint32_t index, const Value& val) {
        MOZ_ASSERT(index < getDenseInitializedLength());
        MOZ_ASSERT(!denseElementsAreCopyOnWrite());
        elements_[index].init(this, HeapSlot::Element, index, val);
    }

    void setDenseElementMaybeConvertDouble(uint32_t index, const Value& val) {
        if (val.isInt32() && shouldConvertDoubleElements())
            setDenseElement(index, DoubleValue(val.toInt32()));
        else
            setDenseElement(index, val);
    }

    inline void setDenseElementWithType(ExclusiveContext* cx, uint32_t index,
                                        const Value& val);
    inline void initDenseElementWithType(ExclusiveContext* cx, uint32_t index,
                                         const Value& val);
    inline void setDenseElementHole(ExclusiveContext* cx, uint32_t index);
    static inline void removeDenseElementForSparseIndex(ExclusiveContext* cx,
                                                        HandleNativeObject obj, uint32_t index);

    inline Value getDenseOrTypedArrayElement(uint32_t idx);

    void copyDenseElements(uint32_t dstStart, const Value* src, uint32_t count) {
        MOZ_ASSERT(dstStart + count <= getDenseCapacity());
        MOZ_ASSERT(!denseElementsAreCopyOnWrite());
        if (JS::shadow::Zone::asShadowZone(zone())->needsIncrementalBarrier()) {
            for (uint32_t i = 0; i < count; ++i)
                elements_[dstStart + i].set(this, HeapSlot::Element, dstStart + i, src[i]);
        } else {
            memcpy(&elements_[dstStart], src, count * sizeof(HeapSlot));
            elementsRangeWriteBarrierPost(dstStart, count);
        }
    }

    void initDenseElements(uint32_t dstStart, const Value* src, uint32_t count) {
        MOZ_ASSERT(dstStart + count <= getDenseCapacity());
        MOZ_ASSERT(!denseElementsAreCopyOnWrite());
        memcpy(&elements_[dstStart], src, count * sizeof(HeapSlot));
        elementsRangeWriteBarrierPost(dstStart, count);
    }

    void moveDenseElements(uint32_t dstStart, uint32_t srcStart, uint32_t count) {
        MOZ_ASSERT(dstStart + count <= getDenseCapacity());
        MOZ_ASSERT(srcStart + count <= getDenseInitializedLength());
        MOZ_ASSERT(!denseElementsAreCopyOnWrite());

        /*
         * Using memmove here would skip write barriers. Also, we need to consider
         * an array containing [A, B, C], in the following situation:
         *
         * 1. Incremental GC marks slot 0 of array (i.e., A), then returns to JS code.
         * 2. JS code moves slots 1..2 into slots 0..1, so it contains [B, C, C].
         * 3. Incremental GC finishes by marking slots 1 and 2 (i.e., C).
         *
         * Since normal marking never happens on B, it is very important that the
         * write barrier is invoked here on B, despite the fact that it exists in
         * the array before and after the move.
        */
        if (JS::shadow::Zone::asShadowZone(zone())->needsIncrementalBarrier()) {
            if (dstStart < srcStart) {
                HeapSlot* dst = elements_ + dstStart;
                HeapSlot* src = elements_ + srcStart;
                for (uint32_t i = 0; i < count; i++, dst++, src++)
                    dst->set(this, HeapSlot::Element, dst - elements_, *src);
            } else {
                HeapSlot* dst = elements_ + dstStart + count - 1;
                HeapSlot* src = elements_ + srcStart + count - 1;
                for (uint32_t i = 0; i < count; i++, dst--, src--)
                    dst->set(this, HeapSlot::Element, dst - elements_, *src);
            }
        } else {
            memmove(elements_ + dstStart, elements_ + srcStart, count * sizeof(HeapSlot));
            elementsRangeWriteBarrierPost(dstStart, count);
        }
    }

    void moveDenseElementsNoPreBarrier(uint32_t dstStart, uint32_t srcStart, uint32_t count) {
        MOZ_ASSERT(!shadowZone()->needsIncrementalBarrier());

        MOZ_ASSERT(dstStart + count <= getDenseCapacity());
        MOZ_ASSERT(srcStart + count <= getDenseCapacity());
        MOZ_ASSERT(!denseElementsAreCopyOnWrite());

        memmove(elements_ + dstStart, elements_ + srcStart, count * sizeof(Value));
        elementsRangeWriteBarrierPost(dstStart, count);
    }

    bool shouldConvertDoubleElements() {
        return getElementsHeader()->shouldConvertDoubleElements();
    }

    inline void setShouldConvertDoubleElements();
    inline void clearShouldConvertDoubleElements();

    bool denseElementsAreCopyOnWrite() {
        return getElementsHeader()->isCopyOnWrite();
    }

    /* Packed information for this object's elements. */
    inline bool writeToIndexWouldMarkNotPacked(uint32_t index);
    inline void markDenseElementsNotPacked(ExclusiveContext* cx);

    // Ensures that the object can hold at least index + extra elements. This
    // returns DenseElement_Success on success, DenseElement_Failed on failure
    // to grow the array, or DenseElement_Incomplete when the object is too
    // sparse to grow (this includes the case of index + extra overflow). In
    // the last two cases the object is kept intact.
    inline DenseElementResult ensureDenseElements(ExclusiveContext* cx,
                                                  uint32_t index, uint32_t extra);

    inline DenseElementResult extendDenseElements(ExclusiveContext* cx,
                                                  uint32_t requiredCapacity, uint32_t extra);

    /* Convert a single dense element to a sparse property. */
    static bool sparsifyDenseElement(ExclusiveContext* cx,
                                     HandleNativeObject obj, uint32_t index);

    /* Convert all dense elements to sparse properties. */
    static bool sparsifyDenseElements(ExclusiveContext* cx, HandleNativeObject obj);

    /* Small objects are dense, no matter what. */
    static const uint32_t MIN_SPARSE_INDEX = 1000;

    /*
     * Element storage for an object will be sparse if fewer than 1/8 indexes
     * are filled in.
     */
    static const unsigned SPARSE_DENSITY_RATIO = 8;

    /*
     * Check if after growing the object's elements will be too sparse.
     * newElementsHint is an estimated number of elements to be added.
     */
    bool willBeSparseElements(uint32_t requiredCapacity, uint32_t newElementsHint);

    /*
     * After adding a sparse index to obj, see if it should be converted to use
     * dense elements.
     */
    static DenseElementResult maybeDensifySparseElements(ExclusiveContext* cx,
                                                         HandleNativeObject obj);

    inline HeapSlot* fixedElements() const {
        static_assert(2 * sizeof(Value) == sizeof(ObjectElements),
                      "when elements are stored inline, the first two "
                      "slots will hold the ObjectElements header");
        return &fixedSlots()[2];
    }

#ifdef DEBUG
    bool canHaveNonEmptyElements();
#endif

    void setFixedElements() {
        MOZ_ASSERT(canHaveNonEmptyElements());
        elements_ = fixedElements();
    }

    inline bool hasDynamicElements() const {
        /*
         * Note: for objects with zero fixed slots this could potentially give
         * a spurious 'true' result, if the end of this object is exactly
         * aligned with the end of its arena and dynamic slots are allocated
         * immediately afterwards. Such cases cannot occur for dense arrays
         * (which have at least two fixed slots) and can only result in a leak.
         */
        return !hasEmptyElements() && elements_ != fixedElements();
    }

    inline bool hasFixedElements() const {
        return elements_ == fixedElements();
    }

    inline bool hasEmptyElements() const {
        return elements_ == emptyObjectElements || elements_ == emptyObjectElementsShared;
    }

    /*
     * Get a pointer to the unused data in the object's allocation immediately
     * following this object, for use with objects which allocate a larger size
     * class than they need and store non-elements data inline.
     */
    inline uint8_t* fixedData(size_t nslots) const;

    inline void privateWriteBarrierPre(void** oldval);

    void privateWriteBarrierPost(void** pprivate) {
        gc::Cell** cellp = reinterpret_cast<gc::Cell**>(pprivate);
        MOZ_ASSERT(cellp);
        MOZ_ASSERT(*cellp);
        gc::StoreBuffer* storeBuffer = (*cellp)->storeBuffer();
        if (storeBuffer)
            storeBuffer->putCell(cellp);
    }

    /* Private data accessors. */

    inline void*& privateRef(uint32_t nfixed) const { /* XXX should be private, not protected! */
        /*
         * The private pointer of an object can hold any word sized value.
         * Private pointers are stored immediately after the last fixed slot of
         * the object.
         */
        MOZ_ASSERT(nfixed == numFixedSlots());
        MOZ_ASSERT(hasPrivate());
        HeapSlot* end = &fixedSlots()[nfixed];
        return *reinterpret_cast<void**>(end);
    }

    bool hasPrivate() const {
        return getClass()->hasPrivate();
    }
    void* getPrivate() const {
        return privateRef(numFixedSlots());
    }
    void setPrivate(void* data) {
        void** pprivate = &privateRef(numFixedSlots());
        privateWriteBarrierPre(pprivate);
        *pprivate = data;
    }

    void setPrivateGCThing(gc::Cell* cell) {
        void** pprivate = &privateRef(numFixedSlots());
        privateWriteBarrierPre(pprivate);
        *pprivate = reinterpret_cast<void*>(cell);
        privateWriteBarrierPost(pprivate);
    }

    void setPrivateUnbarriered(void* data) {
        void** pprivate = &privateRef(numFixedSlots());
        *pprivate = data;
    }
    void initPrivate(void* data) {
        privateRef(numFixedSlots()) = data;
    }

    /* Access private data for an object with a known number of fixed slots. */
    inline void* getPrivate(uint32_t nfixed) const {
        return privateRef(nfixed);
    }

    static inline NativeObject*
    copy(ExclusiveContext* cx, gc::AllocKind kind, gc::InitialHeap heap,
         HandleNativeObject templateObject);

    /* JIT Accessors */
    static size_t offsetOfElements() { return offsetof(NativeObject, elements_); }
    static size_t offsetOfFixedElements() {
        return sizeof(NativeObject) + sizeof(ObjectElements);
    }

    static size_t getFixedSlotOffset(size_t slot) {
        return sizeof(NativeObject) + slot * sizeof(Value);
    }
    static size_t getPrivateDataOffset(size_t nfixed) { return getFixedSlotOffset(nfixed); }
    static size_t offsetOfSlots() { return offsetof(NativeObject, slots_); }
};

// Object class for plain native objects created using '{}' object literals,
// 'new Object()', 'Object.create', etc.
class PlainObject : public NativeObject
{
  public:
    static const js::Class class_;
};

inline void
NativeObject::privateWriteBarrierPre(void** oldval)
{
    JS::shadow::Zone* shadowZone = this->shadowZoneFromAnyThread();
    if (shadowZone->needsIncrementalBarrier()) {
        if (*oldval && getClass()->trace)
            getClass()->trace(shadowZone->barrierTracer(), this);
    }
}

#ifdef DEBUG
static inline bool
IsObjectValueInCompartment(Value v, JSCompartment* comp)
{
    if (!v.isObject())
        return true;
    return v.toObject().compartment() == comp;
}
#endif


/*** Standard internal methods *******************************************************************/

/*
 * These functions should follow the algorithms in ES6 draft rev 29 section 9.1
 * ("Ordinary Object Internal Methods"). It's an ongoing project.
 *
 * Many native objects are not "ordinary" in ES6, so these functions also have
 * to serve some of the special needs of Functions (9.2, 9.3, 9.4.1), Arrays
 * (9.4.2), Strings (9.4.3), and so on.
 */

extern bool
NativeDefineProperty(ExclusiveContext* cx, HandleNativeObject obj, HandleId id,
                     Handle<PropertyDescriptor> desc,
                     ObjectOpResult& result);

extern bool
NativeDefineProperty(ExclusiveContext* cx, HandleNativeObject obj, HandleId id, HandleValue value,
                     JSGetterOp getter, JSSetterOp setter, unsigned attrs,
                     ObjectOpResult& result);

extern bool
NativeDefineProperty(ExclusiveContext* cx, HandleNativeObject obj, PropertyName* name,
                     HandleValue value, GetterOp getter, SetterOp setter,
                     unsigned attrs, ObjectOpResult& result);

extern bool
NativeDefineElement(ExclusiveContext* cx, HandleNativeObject obj, uint32_t index, HandleValue value,
                    JSGetterOp getter, JSSetterOp setter, unsigned attrs,
                    ObjectOpResult& result);

/* If the result out-param is omitted, throw on failure. */
extern bool
NativeDefineProperty(ExclusiveContext* cx, HandleNativeObject obj, HandleId id, HandleValue value,
                     JSGetterOp getter, JSSetterOp setter, unsigned attrs);

extern bool
NativeDefineProperty(ExclusiveContext* cx, HandleNativeObject obj, PropertyName* name,
                     HandleValue value, JSGetterOp getter, JSSetterOp setter,
                     unsigned attrs);

extern bool
NativeHasProperty(JSContext* cx, HandleNativeObject obj, HandleId id, bool* foundp);

extern bool
NativeGetProperty(JSContext* cx, HandleNativeObject obj, HandleValue receiver, HandleId id,
                  MutableHandleValue vp);

extern bool
NativeGetPropertyNoGC(JSContext* cx, NativeObject* obj, const Value& receiver, jsid id, Value* vp);

extern bool
NativeGetElement(JSContext* cx, HandleNativeObject obj, HandleValue receiver, uint32_t index,
                 MutableHandleValue vp);

inline bool
NativeGetProperty(JSContext* cx, HandleNativeObject obj, HandleId id, MutableHandleValue vp)
{
    RootedValue receiver(cx, ObjectValue(*obj));
    return NativeGetProperty(cx, obj, receiver, id, vp);
}

inline bool
NativeGetElement(JSContext* cx, HandleNativeObject obj, uint32_t index, MutableHandleValue vp)
{
    RootedValue receiver(cx, ObjectValue(*obj));
    return NativeGetElement(cx, obj, receiver, index, vp);
}

bool
SetPropertyByDefining(JSContext* cx, HandleId id, HandleValue v, HandleValue receiver,
                      ObjectOpResult& result);

bool
SetPropertyOnProto(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                   HandleValue receiver, ObjectOpResult& result);

/*
 * Indicates whether an assignment operation is qualified (`x.y = 0`) or
 * unqualified (`y = 0`). In strict mode, the latter is an error if no such
 * variable already exists.
 *
 * Used as an argument to NativeSetProperty.
 */
enum QualifiedBool {
    Unqualified = 0,
    Qualified = 1
};

extern bool
NativeSetProperty(JSContext* cx, HandleNativeObject obj, HandleId id, HandleValue v,
                  HandleValue receiver, QualifiedBool qualified, ObjectOpResult& result);

extern bool
NativeSetElement(JSContext* cx, HandleNativeObject obj, uint32_t index, HandleValue v,
                 HandleValue receiver, ObjectOpResult& result);

extern bool
NativeDeleteProperty(JSContext* cx, HandleNativeObject obj, HandleId id, ObjectOpResult& result);


/*** SpiderMonkey nonstandard internal methods ***************************************************/

template <AllowGC allowGC>
extern bool
NativeLookupOwnProperty(ExclusiveContext* cx,
                        typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                        typename MaybeRooted<jsid, allowGC>::HandleType id,
                        typename MaybeRooted<Shape*, allowGC>::MutableHandleType propp);

/*
 * Get a property from `receiver`, after having already done a lookup and found
 * the property on a native object `obj`.
 *
 * `shape` must not be null and must not be an implicit dense property. It must
 * be present in obj's shape chain.
 */
extern bool
NativeGetExistingProperty(JSContext* cx, HandleObject receiver, HandleNativeObject obj,
                          HandleShape shape, MutableHandleValue vp);

/* * */

/*
 * If obj has an already-resolved data property for id, return true and
 * store the property value in *vp.
 */
extern bool
HasDataProperty(JSContext* cx, NativeObject* obj, jsid id, Value* vp);

inline bool
HasDataProperty(JSContext* cx, NativeObject* obj, PropertyName* name, Value* vp)
{
    return HasDataProperty(cx, obj, NameToId(name), vp);
}

extern bool
GetPropertyForNameLookup(JSContext* cx, HandleObject obj, HandleId id, MutableHandleValue vp);

} /* namespace js */

template <>
inline bool
JSObject::is<js::NativeObject>() const { return isNative(); }

namespace js {

// Alternate to JSObject::as<NativeObject>() that tolerates null pointers.
inline NativeObject*
MaybeNativeObject(JSObject* obj)
{
    return obj ? &obj->as<NativeObject>() : nullptr;
}

} // namespace js


/*** Inline functions declared in jsobj.h that use the native declarations above *****************/

inline bool
js::HasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp)
{
    if (HasPropertyOp op = obj->getOps()->hasProperty)
        return op(cx, obj, id, foundp);
    return NativeHasProperty(cx, obj.as<NativeObject>(), id, foundp);
}

inline bool
js::GetProperty(JSContext* cx, HandleObject obj, HandleValue receiver, HandleId id,
                MutableHandleValue vp)
{
    if (GetPropertyOp op = obj->getOps()->getProperty)
        return op(cx, obj, receiver, id, vp);
    return NativeGetProperty(cx, obj.as<NativeObject>(), receiver, id, vp);
}

inline bool
js::GetPropertyNoGC(JSContext* cx, JSObject* obj, const Value& receiver, jsid id, Value* vp)
{
    if (obj->getOps()->getProperty)
        return false;
    return NativeGetPropertyNoGC(cx, &obj->as<NativeObject>(), receiver, id, vp);
}

inline bool
js::SetProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                HandleValue receiver, ObjectOpResult& result)
{
    if (obj->getOps()->setProperty)
        return JSObject::nonNativeSetProperty(cx, obj, id, v, receiver, result);
    return NativeSetProperty(cx, obj.as<NativeObject>(), id, v, receiver, Qualified, result);
}

inline bool
js::SetElement(JSContext* cx, HandleObject obj, uint32_t index, HandleValue v,
               HandleValue receiver, ObjectOpResult& result)
{
    if (obj->getOps()->setProperty)
        return JSObject::nonNativeSetElement(cx, obj, index, v, receiver, result);
    return NativeSetElement(cx, obj.as<NativeObject>(), index, v, receiver, result);
}

#endif /* vm_NativeObject_h */
