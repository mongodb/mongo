/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_NativeObject_h
#define vm_NativeObject_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <algorithm>
#include <stdint.h>

#include "jsfriendapi.h"
#include "NamespaceImports.h"

#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "gc/MaybeRooted.h"
#include "gc/ZoneAllocator.h"
#include "js/shadow/Object.h"  // JS::shadow::Object
#include "js/shadow/Zone.h"    // JS::shadow::Zone
#include "js/Value.h"
#include "vm/GetterSetter.h"
#include "vm/JSObject.h"
#include "vm/PropertyResult.h"
#include "vm/Shape.h"
#include "vm/StringType.h"

namespace js {

class Shape;
class TenuringTracer;

/*
 * To really poison a set of values, using 'magic' or 'undefined' isn't good
 * enough since often these will just be ignored by buggy code (see bug 629974)
 * in debug builds and crash in release builds. Instead, we use a safe-for-crash
 * pointer.
 */
static MOZ_ALWAYS_INLINE void Debug_SetValueRangeToCrashOnTouch(Value* beg,
                                                                Value* end) {
#ifdef DEBUG
  for (Value* v = beg; v != end; ++v) {
    *v = js::PoisonedObjectValue(0x48);
  }
#endif
}

static MOZ_ALWAYS_INLINE void Debug_SetValueRangeToCrashOnTouch(Value* vec,
                                                                size_t len) {
#ifdef DEBUG
  Debug_SetValueRangeToCrashOnTouch(vec, vec + len);
#endif
}

static MOZ_ALWAYS_INLINE void Debug_SetValueRangeToCrashOnTouch(GCPtrValue* vec,
                                                                size_t len) {
#ifdef DEBUG
  Debug_SetValueRangeToCrashOnTouch((Value*)vec, len);
#endif
}

static MOZ_ALWAYS_INLINE void Debug_SetSlotRangeToCrashOnTouch(HeapSlot* vec,
                                                               uint32_t len) {
#ifdef DEBUG
  Debug_SetValueRangeToCrashOnTouch((Value*)vec, len);
#endif
}

static MOZ_ALWAYS_INLINE void Debug_SetSlotRangeToCrashOnTouch(HeapSlot* begin,
                                                               HeapSlot* end) {
#ifdef DEBUG
  Debug_SetValueRangeToCrashOnTouch((Value*)begin, end - begin);
#endif
}

class ArrayObject;

/*
 * ES6 20130308 draft 8.4.2.4 ArraySetLength.
 *
 * |id| must be "length", |desc| is the new non-accessor descriptor, and
 * |result| receives an error code if the change is invalid.
 */
extern bool ArraySetLength(JSContext* cx, Handle<ArrayObject*> obj, HandleId id,
                           Handle<PropertyDescriptor> desc,
                           ObjectOpResult& result);

/*
 * [SMDOC] NativeObject Elements layout
 *
 * Elements header used for native objects. The elements component of such
 * objects offers an efficient representation for all or some of the indexed
 * properties of the object, using a flat array of Values rather than a shape
 * hierarchy stored in the object's slots. This structure is immediately
 * followed by an array of elements, with the elements member in an object
 * pointing to the beginning of that array (the end of this structure). See
 * below for usage of this structure.
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
 *
 *
 * [SMDOC] NativeObject shifted elements optimization
 *
 * Shifted elements
 * ----------------
 * It's pretty common to use an array as a queue, like this:
 *
 *    while (arr.length > 0)
 *        foo(arr.shift());
 *
 * To ensure we don't get quadratic behavior on this, elements can be 'shifted'
 * in memory. tryShiftDenseElements does this by incrementing elements_ to point
 * to the next element and moving the ObjectElements header in memory (so it's
 * stored where the shifted Value used to be).
 *
 * Shifted elements can be moved when we grow the array, when the array is
 * made non-extensible (for simplicity, shifted elements are not supported on
 * objects that are non-extensible, have copy-on-write elements, or on arrays
 * with non-writable length).
 */
class ObjectElements {
 public:
  enum Flags : uint16_t {
    // (0x1 is unused)

    // Present only if these elements correspond to an array with
    // non-writable length; never present for non-arrays.
    NONWRITABLE_ARRAY_LENGTH = 0x2,

    // (0x4 is unused)

    // For TypedArrays only: this TypedArray's storage is mapping shared
    // memory.  This is a static property of the TypedArray, set when it
    // is created and never changed.
    SHARED_MEMORY = 0x8,

    // These elements are not extensible. If this flag is set, the object's
    // Shape must also have the NotExtensible flag. This exists on
    // ObjectElements in addition to Shape to simplify JIT code.
    NOT_EXTENSIBLE = 0x10,

    // These elements are set to integrity level "sealed". If this flag is
    // set, the NOT_EXTENSIBLE flag must be set as well.
    SEALED = 0x20,

    // These elements are set to integrity level "frozen". If this flag is
    // set, the SEALED flag must be set as well.
    //
    // This flag must only be set if the Shape has the FrozenElements flag.
    // The Shape flag ensures a shape guard can be used to guard against frozen
    // elements. The ObjectElements flag is convenient for JIT code and
    // ObjectElements assertions.
    FROZEN = 0x40,

    // If this flag is not set, the elements are guaranteed to contain no hole
    // values (the JS_ELEMENTS_HOLE MagicValue) in [0, initializedLength).
    NON_PACKED = 0x80,

    // If this flag is not set, there's definitely no for-in iterator that
    // covers these dense elements so elements can be deleted without calling
    // SuppressDeletedProperty. This is used by fast paths for various Array
    // builtins. See also NativeObject::denseElementsMaybeInIteration.
    MAYBE_IN_ITERATION = 0x100,
  };

  // The flags word stores both the flags and the number of shifted elements.
  // Allow shifting 2047 elements before actually moving the elements.
  static const size_t NumShiftedElementsBits = 11;
  static const size_t MaxShiftedElements = (1 << NumShiftedElementsBits) - 1;
  static const size_t NumShiftedElementsShift = 32 - NumShiftedElementsBits;
  static const size_t FlagsMask = (1 << NumShiftedElementsShift) - 1;
  static_assert(MaxShiftedElements == 2047,
                "MaxShiftedElements should match the comment");

 private:
  friend class ::JSObject;
  friend class ArrayObject;
  friend class NativeObject;
  friend class TenuringTracer;

  friend bool js::SetIntegrityLevel(JSContext* cx, HandleObject obj,
                                    IntegrityLevel level);

  friend bool ArraySetLength(JSContext* cx, Handle<ArrayObject*> obj,
                             HandleId id, Handle<PropertyDescriptor> desc,
                             ObjectOpResult& result);

  // The NumShiftedElementsBits high bits of this are used to store the
  // number of shifted elements, the other bits are available for the flags.
  // See Flags enum above.
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

  bool hasNonwritableArrayLength() const {
    return flags & NONWRITABLE_ARRAY_LENGTH;
  }
  void setNonwritableArrayLength() {
    // See ArrayObject::setNonWritableLength.
    MOZ_ASSERT(capacity == initializedLength);
    MOZ_ASSERT(numShiftedElements() == 0);
    flags |= NONWRITABLE_ARRAY_LENGTH;
  }

  void addShiftedElements(uint32_t count) {
    MOZ_ASSERT(count < capacity);
    MOZ_ASSERT(count < initializedLength);
    MOZ_ASSERT(!(
        flags & (NONWRITABLE_ARRAY_LENGTH | NOT_EXTENSIBLE | SEALED | FROZEN)));
    uint32_t numShifted = numShiftedElements() + count;
    MOZ_ASSERT(numShifted <= MaxShiftedElements);
    flags = (numShifted << NumShiftedElementsShift) | (flags & FlagsMask);
    capacity -= count;
    initializedLength -= count;
  }
  void unshiftShiftedElements(uint32_t count) {
    MOZ_ASSERT(count > 0);
    MOZ_ASSERT(!(
        flags & (NONWRITABLE_ARRAY_LENGTH | NOT_EXTENSIBLE | SEALED | FROZEN)));
    uint32_t numShifted = numShiftedElements();
    MOZ_ASSERT(count <= numShifted);
    numShifted -= count;
    flags = (numShifted << NumShiftedElementsShift) | (flags & FlagsMask);
    capacity += count;
    initializedLength += count;
  }
  void clearShiftedElements() {
    flags &= FlagsMask;
    MOZ_ASSERT(numShiftedElements() == 0);
  }

  void markNonPacked() { flags |= NON_PACKED; }

  void markMaybeInIteration() { flags |= MAYBE_IN_ITERATION; }
  bool maybeInIteration() { return flags & MAYBE_IN_ITERATION; }

  void setNotExtensible() {
    MOZ_ASSERT(!isNotExtensible());
    flags |= NOT_EXTENSIBLE;
  }
  bool isNotExtensible() { return flags & NOT_EXTENSIBLE; }

  void seal() {
    MOZ_ASSERT(isNotExtensible());
    MOZ_ASSERT(!isSealed());
    MOZ_ASSERT(!isFrozen());
    flags |= SEALED;
  }
  void freeze() {
    MOZ_ASSERT(isNotExtensible());
    MOZ_ASSERT(isSealed());
    MOZ_ASSERT(!isFrozen());
    flags |= FROZEN;
  }

  bool isFrozen() const { return flags & FROZEN; }

 public:
  constexpr ObjectElements(uint32_t capacity, uint32_t length)
      : flags(0), initializedLength(0), capacity(capacity), length(length) {}

  enum class SharedMemory { IsShared };

  constexpr ObjectElements(uint32_t capacity, uint32_t length,
                           SharedMemory shmem)
      : flags(SHARED_MEMORY),
        initializedLength(0),
        capacity(capacity),
        length(length) {}

  HeapSlot* elements() {
    return reinterpret_cast<HeapSlot*>(uintptr_t(this) +
                                       sizeof(ObjectElements));
  }
  const HeapSlot* elements() const {
    return reinterpret_cast<const HeapSlot*>(uintptr_t(this) +
                                             sizeof(ObjectElements));
  }
  static ObjectElements* fromElements(HeapSlot* elems) {
    return reinterpret_cast<ObjectElements*>(uintptr_t(elems) -
                                             sizeof(ObjectElements));
  }

  bool isSharedMemory() const { return flags & SHARED_MEMORY; }

  static int offsetOfFlags() {
    return int(offsetof(ObjectElements, flags)) - int(sizeof(ObjectElements));
  }
  static int offsetOfInitializedLength() {
    return int(offsetof(ObjectElements, initializedLength)) -
           int(sizeof(ObjectElements));
  }
  static int offsetOfCapacity() {
    return int(offsetof(ObjectElements, capacity)) -
           int(sizeof(ObjectElements));
  }
  static int offsetOfLength() {
    return int(offsetof(ObjectElements, length)) - int(sizeof(ObjectElements));
  }

  static void PrepareForPreventExtensions(JSContext* cx, NativeObject* obj);
  static void PreventExtensions(NativeObject* obj);
  [[nodiscard]] static bool FreezeOrSeal(JSContext* cx, HandleNativeObject obj,
                                         IntegrityLevel level);

  bool isSealed() const { return flags & SEALED; }

  bool isPacked() const { return !(flags & NON_PACKED); }

  JS::PropertyAttributes elementAttributes() const {
    if (isFrozen()) {
      return {JS::PropertyAttribute::Enumerable};
    }
    if (isSealed()) {
      return {JS::PropertyAttribute::Enumerable,
              JS::PropertyAttribute::Writable};
    }
    return {JS::PropertyAttribute::Configurable,
            JS::PropertyAttribute::Enumerable, JS::PropertyAttribute::Writable};
  }

  uint32_t numShiftedElements() const {
    uint32_t numShifted = flags >> NumShiftedElementsShift;
    MOZ_ASSERT_IF(numShifted > 0,
                  !(flags & (NONWRITABLE_ARRAY_LENGTH | NOT_EXTENSIBLE |
                             SEALED | FROZEN)));
    return numShifted;
  }

  uint32_t numAllocatedElements() const {
    return VALUES_PER_HEADER + capacity + numShiftedElements();
  }

  // This is enough slots to store an object of this class. See the static
  // assertion below.
  static const size_t VALUES_PER_HEADER = 2;
};

static_assert(ObjectElements::VALUES_PER_HEADER * sizeof(HeapSlot) ==
                  sizeof(ObjectElements),
              "ObjectElements doesn't fit in the given number of slots");

/*
 * Slots header used for native objects. The header stores the capacity and the
 * slot data follows in memory.
 */
class alignas(HeapSlot) ObjectSlots {
  uint32_t capacity_;
  uint32_t dictionarySlotSpan_;

 public:
  static constexpr size_t VALUES_PER_HEADER = 1;

  static inline size_t allocCount(size_t slotCount) {
    static_assert(sizeof(ObjectSlots) ==
                  ObjectSlots::VALUES_PER_HEADER * sizeof(HeapSlot));
    return slotCount + VALUES_PER_HEADER;
  }

  static inline size_t allocSize(size_t slotCount) {
    return allocCount(slotCount) * sizeof(HeapSlot);
  }

  static ObjectSlots* fromSlots(HeapSlot* slots) {
    MOZ_ASSERT(slots);
    return reinterpret_cast<ObjectSlots*>(uintptr_t(slots) -
                                          sizeof(ObjectSlots));
  }

  static constexpr size_t offsetOfCapacity() {
    return offsetof(ObjectSlots, capacity_);
  }
  static constexpr size_t offsetOfDictionarySlotSpan() {
    return offsetof(ObjectSlots, dictionarySlotSpan_);
  }
  static constexpr size_t offsetOfSlots() { return sizeof(ObjectSlots); }
  static constexpr int32_t offsetOfDictionarySlotSpanFromSlots() {
    return int32_t(offsetOfDictionarySlotSpan()) - int32_t(offsetOfSlots());
  }

  constexpr explicit ObjectSlots(uint32_t capacity, uint32_t dictionarySlotSpan)
      : capacity_(capacity), dictionarySlotSpan_(dictionarySlotSpan) {}

  uint32_t capacity() const { return capacity_; }
  uint32_t dictionarySlotSpan() const { return dictionarySlotSpan_; }

  void setDictionarySlotSpan(uint32_t span) { dictionarySlotSpan_ = span; }

  HeapSlot* slots() const {
    return reinterpret_cast<HeapSlot*>(uintptr_t(this) + sizeof(ObjectSlots));
  }
};

/*
 * Shared singletons for objects with no elements.
 * emptyObjectElementsShared is used only for TypedArrays, when the TA
 * maps shared memory.
 */
extern HeapSlot* const emptyObjectElements;
extern HeapSlot* const emptyObjectElementsShared;

/*
 * Shared singletons for objects with no dynamic slots.
 */
extern HeapSlot* const emptyObjectSlots;
extern HeapSlot* const emptyObjectSlotsForDictionaryObject[];

class AutoCheckShapeConsistency;
class GCMarker;
class Shape;

class NewObjectCache;

// Operations which change an object's dense elements can either succeed, fail,
// or be unable to complete. The latter is used when the object's elements must
// become sparse instead. The enum below is used for such operations.
enum class DenseElementResult { Failure, Success, Incomplete };

/*
 * [SMDOC] NativeObject layout
 *
 * NativeObject specifies the internal implementation of a native object.
 *
 * Native objects use ShapedObject::shape to record property information. Two
 * native objects with the same shape are guaranteed to have the same number of
 * fixed slots.
 *
 * Native objects extend the base implementation of an object with storage for
 * the object's named properties and indexed elements.
 *
 * These are stored separately from one another. Objects are followed by a
 * variable-sized array of values for inline storage, which may be used by
 * either properties of native objects (fixed slots), by elements (fixed
 * elements), or by other data for certain kinds of objects, such as
 * ArrayBufferObjects and TypedArrayObjects.
 *
 * Named property storage can be split between fixed slots and a dynamically
 * allocated array (the slots member). For an object with N fixed slots, shapes
 * with slots [0..N-1] are stored in the fixed slots, and the remainder are
 * stored in the dynamic array. If all properties fit in the fixed slots, the
 * 'slots_' member is nullptr.
 *
 * Elements are indexed via the 'elements_' member. This member can point to
 * either the shared emptyObjectElements and emptyObjectElementsShared
 * singletons, into the inline value array (the address of the third value, to
 * leave room for a ObjectElements header;in this case numFixedSlots() is zero)
 * or to a dynamically allocated array.
 *
 * Slots and elements may both be non-empty. The slots may be either names or
 * indexes; no indexed property will be in both the slots and elements.
 */
class NativeObject : public JSObject {
 protected:
  /* Slots for object properties. */
  js::HeapSlot* slots_;

  /* Slots for object dense elements. */
  js::HeapSlot* elements_;

  friend class ::JSObject;

 private:
  static void staticAsserts() {
    static_assert(sizeof(NativeObject) == sizeof(JSObject_Slots0),
                  "native object size must match GC thing size");
    static_assert(sizeof(NativeObject) == sizeof(JS::shadow::Object),
                  "shadow interface must match actual implementation");
    static_assert(sizeof(NativeObject) % sizeof(Value) == 0,
                  "fixed slots after an object must be aligned");

    static_assert(offsetOfShape() == offsetof(JS::shadow::Object, shape),
                  "shadow type must match actual type");
    static_assert(
        offsetof(NativeObject, slots_) == offsetof(JS::shadow::Object, slots),
        "shadow slots must match actual slots");
    static_assert(
        offsetof(NativeObject, elements_) == offsetof(JS::shadow::Object, _1),
        "shadow placeholder must match actual elements");

    static_assert(MAX_FIXED_SLOTS <= Shape::FIXED_SLOTS_MAX,
                  "verify numFixedSlots() bitfield is big enough");
    static_assert(sizeof(NativeObject) + MAX_FIXED_SLOTS * sizeof(Value) ==
                      JSObject::MAX_BYTE_SIZE,
                  "inconsistent maximum object size");

    // Sanity check NativeObject size is what we expect.
#ifdef JS_64BIT
    static_assert(sizeof(NativeObject) == 3 * sizeof(void*));
#else
    static_assert(sizeof(NativeObject) == 4 * sizeof(void*));
#endif
  }

 public:
  PropertyInfoWithKey getLastProperty() const {
    return shape()->lastProperty();
  }

  HeapSlotArray getDenseElements() const { return HeapSlotArray(elements_); }

  const Value& getDenseElement(uint32_t idx) const {
    MOZ_ASSERT(idx < getDenseInitializedLength());
    return elements_[idx];
  }
  bool containsDenseElement(uint32_t idx) {
    return idx < getDenseInitializedLength() &&
           !elements_[idx].isMagic(JS_ELEMENTS_HOLE);
  }
  uint32_t getDenseInitializedLength() const {
    return getElementsHeader()->initializedLength;
  }
  uint32_t getDenseCapacity() const { return getElementsHeader()->capacity; }

  bool isSharedMemory() const { return getElementsHeader()->isSharedMemory(); }

  // Update the object's shape, keeping the number of allocated slots in sync
  // with the object's new slot span.
  MOZ_ALWAYS_INLINE bool setShapeAndUpdateSlots(JSContext* cx, Shape* newShape);

  // Optimized version of setShapeAndUpdateSlots for adding a single property
  // with a slot.
  MOZ_ALWAYS_INLINE bool setShapeAndUpdateSlotsForNewSlot(JSContext* cx,
                                                          Shape* newShape,
                                                          uint32_t slot);

  MOZ_ALWAYS_INLINE bool canReuseShapeForNewProperties(Shape* newShape) const {
    if (shape()->numFixedSlots() != newShape->numFixedSlots()) {
      return false;
    }
    if (shape()->isDictionary() || newShape->isDictionary()) {
      return false;
    }
    if (shape()->base() != newShape->base()) {
      return false;
    }
    MOZ_ASSERT(shape()->getObjectClass() == newShape->getObjectClass());
    MOZ_ASSERT(shape()->proto() == newShape->proto());
    MOZ_ASSERT(shape()->realm() == newShape->realm());
    return shape()->objectFlags() == newShape->objectFlags();
  }

  // Newly-created TypedArrays that map a SharedArrayBuffer are
  // marked as shared by giving them an ObjectElements that has the
  // ObjectElements::SHARED_MEMORY flag set.
  void setIsSharedMemory() {
    MOZ_ASSERT(elements_ == emptyObjectElements);
    elements_ = emptyObjectElementsShared;
  }

  inline bool isInWholeCellBuffer() const;

  static inline JS::Result<NativeObject*, JS::OOM> create(
      JSContext* cx, gc::AllocKind kind, gc::InitialHeap heap,
      HandleShape shape, gc::AllocSite* site = nullptr);

#ifdef DEBUG
  static void enableShapeConsistencyChecks();
#endif

 protected:
#ifdef DEBUG
  friend class js::AutoCheckShapeConsistency;
  void checkShapeConsistency();
#else
  void checkShapeConsistency() {}
#endif

  /*
   * Update the slot span directly for a dictionary object, and allocate
   * slots to cover the new span if necessary.
   */
  bool ensureSlotsForDictionaryObject(JSContext* cx, uint32_t span);

  [[nodiscard]] static bool toDictionaryMode(JSContext* cx,
                                             HandleNativeObject obj);

 private:
  inline void setEmptyDynamicSlots(uint32_t dictonarySlotSpan);

  inline void setDictionaryModeSlotSpan(uint32_t span);

  friend class TenuringTracer;

  /*
   * Get internal pointers to the range of values from |start| to |end|
   * exclusive.
   */
  void getSlotRangeUnchecked(uint32_t start, uint32_t end,
                             HeapSlot** fixedStart, HeapSlot** fixedEnd,
                             HeapSlot** slotsStart, HeapSlot** slotsEnd) {
    MOZ_ASSERT(end >= start);

    uint32_t fixed = numFixedSlots();
    if (start < fixed) {
      if (end <= fixed) {
        *fixedStart = &fixedSlots()[start];
        *fixedEnd = &fixedSlots()[end];
        *slotsStart = *slotsEnd = nullptr;
      } else {
        *fixedStart = &fixedSlots()[start];
        *fixedEnd = &fixedSlots()[fixed];
        *slotsStart = &slots_[0];
        *slotsEnd = &slots_[end - fixed];
      }
    } else {
      *fixedStart = *fixedEnd = nullptr;
      *slotsStart = &slots_[start - fixed];
      *slotsEnd = &slots_[end - fixed];
    }
  }

  void getSlotRange(uint32_t start, uint32_t end, HeapSlot** fixedStart,
                    HeapSlot** fixedEnd, HeapSlot** slotsStart,
                    HeapSlot** slotsEnd) {
    MOZ_ASSERT(slotInRange(end, SENTINEL_ALLOWED));
    getSlotRangeUnchecked(start, end, fixedStart, fixedEnd, slotsStart,
                          slotsEnd);
  }

 protected:
  friend class DictionaryPropMap;
  friend class GCMarker;
  friend class Shape;
  friend class NewObjectCache;

  void invalidateSlotRange(uint32_t start, uint32_t end) {
#ifdef DEBUG
    HeapSlot* fixedStart;
    HeapSlot* fixedEnd;
    HeapSlot* slotsStart;
    HeapSlot* slotsEnd;
    getSlotRange(start, end, &fixedStart, &fixedEnd, &slotsStart, &slotsEnd);
    Debug_SetSlotRangeToCrashOnTouch(fixedStart, fixedEnd);
    Debug_SetSlotRangeToCrashOnTouch(slotsStart, slotsEnd);
#endif /* DEBUG */
  }

  void initializeSlotRange(uint32_t start, uint32_t end);

  /*
   * Initialize the object's slots from a flat array. The caller must ensure
   * that there are enough slots. This is used during brain transplants.
   */
  void initSlots(const Value* vector, uint32_t length);

#ifdef DEBUG
  enum SentinelAllowed{SENTINEL_NOT_ALLOWED, SENTINEL_ALLOWED};

  /*
   * Check that slot is in range for the object's allocated slots.
   * If sentinelAllowed then slot may equal the slot capacity.
   */
  bool slotInRange(uint32_t slot,
                   SentinelAllowed sentinel = SENTINEL_NOT_ALLOWED) const;

  /*
   * Check whether a slot is a fixed slot.
   */
  bool slotIsFixed(uint32_t slot) const;

  /*
   * Check whether the supplied number of fixed slots is correct.
   */
  bool isNumFixedSlots(uint32_t nfixed) const;
#endif

  /*
   * Minimum size for dynamically allocated slots in normal Objects.
   * ArrayObjects don't use this limit and can have a lower slot capacity,
   * since they normally don't have a lot of slots.
   */
  static const uint32_t SLOT_CAPACITY_MIN = 8 - ObjectSlots::VALUES_PER_HEADER;

  HeapSlot* fixedSlots() const {
    return reinterpret_cast<HeapSlot*>(uintptr_t(this) + sizeof(NativeObject));
  }

 public:
  /* Object allocation may directly initialize slots so this is public. */
  void initSlots(HeapSlot* slots) {
    MOZ_ASSERT(slots);
    slots_ = slots;
  }
  inline void initEmptyDynamicSlots();

  [[nodiscard]] static bool generateNewDictionaryShape(JSContext* cx,
                                                       HandleNativeObject obj);

  [[nodiscard]] static bool reshapeForShadowedProp(JSContext* cx,
                                                   HandleNativeObject obj);

  // The maximum number of slots in an object.
  // |MAX_SLOTS_COUNT * sizeof(JS::Value)| shouldn't overflow
  // int32_t (see slotsSizeMustNotOverflow).
  static const uint32_t MAX_SLOTS_COUNT = (1 << 28) - 1;

  static void slotsSizeMustNotOverflow() {
    static_assert(
        NativeObject::MAX_SLOTS_COUNT <= INT32_MAX / sizeof(JS::Value),
        "every caller of this method requires that a slot "
        "number (or slot count) count multiplied by "
        "sizeof(Value) can't overflow uint32_t (and sometimes "
        "int32_t, too)");
  }

  uint32_t numFixedSlots() const {
    return reinterpret_cast<const JS::shadow::Object*>(this)->numFixedSlots();
  }

  // Get the number of fixed slots when the shape pointer may have been
  // forwarded by a moving GC. You need to use this rather that
  // numFixedSlots() in a trace hook if you access an object that is not the
  // object being traced, since it may have a stale shape pointer.
  inline uint32_t numFixedSlotsMaybeForwarded() const;

  // Get the private when the ObjectGroup pointer may have been forwarded by a
  // moving GC.
  inline void* getPrivateMaybeForwarded() const;

  uint32_t numUsedFixedSlots() const {
    uint32_t nslots = shape()->slotSpan();
    return std::min(nslots, numFixedSlots());
  }

  uint32_t slotSpan() const {
    if (inDictionaryMode()) {
      return dictionaryModeSlotSpan();
    }
    MOZ_ASSERT(getSlotsHeader()->dictionarySlotSpan() == 0);
    return shape()->slotSpan();
  }

  uint32_t dictionaryModeSlotSpan() const {
    MOZ_ASSERT(inDictionaryMode());
    return getSlotsHeader()->dictionarySlotSpan();
  }

  /* Whether a slot is at a fixed offset from this object. */
  bool isFixedSlot(size_t slot) { return slot < numFixedSlots(); }

  /* Index into the dynamic slots array to use for a dynamic slot. */
  size_t dynamicSlotIndex(size_t slot) {
    MOZ_ASSERT(slot >= numFixedSlots());
    return slot - numFixedSlots();
  }

  // Native objects are never proxies. Call isExtensible instead.
  bool nonProxyIsExtensible() const = delete;

  bool isExtensible() const { return !hasFlag(ObjectFlag::NotExtensible); }

  /*
   * Whether there may be indexed properties on this object, excluding any in
   * the object's elements.
   */
  bool isIndexed() const { return hasFlag(ObjectFlag::Indexed); }

  bool hasInterestingSymbol() const {
    return hasFlag(ObjectFlag::HasInterestingSymbol);
  }

  static bool setHadGetterSetterChange(JSContext* cx, HandleNativeObject obj) {
    return setFlag(cx, obj, ObjectFlag::HadGetterSetterChange);
  }
  bool hadGetterSetterChange() const {
    return hasFlag(ObjectFlag::HadGetterSetterChange);
  }

  /*
   * Grow or shrink slots immediately before changing the slot span.
   * The number of allocated slots is not stored explicitly, and changes to
   * the slots must track changes in the slot span.
   */
  bool growSlots(JSContext* cx, uint32_t oldCapacity, uint32_t newCapacity);
  void shrinkSlots(JSContext* cx, uint32_t oldCapacity, uint32_t newCapacity);

  bool allocateSlots(JSContext* cx, uint32_t newCapacity);

  /*
   * This method is static because it's called from JIT code. On OOM, returns
   * false without leaving a pending exception on the context.
   */
  static bool growSlotsPure(JSContext* cx, NativeObject* obj,
                            uint32_t newCapacity);

  /*
   * Like growSlotsPure but for dense elements. This will return
   * false if we failed to allocate a dense element for some reason (OOM, too
   * many dense elements, non-writable array length, etc).
   */
  static bool addDenseElementPure(JSContext* cx, NativeObject* obj);

  bool hasDynamicSlots() const { return getSlotsHeader()->capacity(); }

  /* Compute the number of dynamic slots required for this object. */
  MOZ_ALWAYS_INLINE uint32_t calculateDynamicSlots() const;

  MOZ_ALWAYS_INLINE uint32_t numDynamicSlots() const;

  bool empty() const { return shape()->propMapLength() == 0; }

  mozilla::Maybe<PropertyInfo> lookup(JSContext* cx, jsid id);
  mozilla::Maybe<PropertyInfo> lookup(JSContext* cx, PropertyName* name) {
    return lookup(cx, NameToId(name));
  }

  bool contains(JSContext* cx, jsid id) { return lookup(cx, id).isSome(); }
  bool contains(JSContext* cx, PropertyName* name) {
    return lookup(cx, name).isSome();
  }
  bool contains(JSContext* cx, jsid id, PropertyInfo prop) {
    mozilla::Maybe<PropertyInfo> found = lookup(cx, id);
    return found.isSome() && *found == prop;
  }

  /* Contextless; can be called from other pure code. */
  mozilla::Maybe<PropertyInfo> lookupPure(jsid id);
  mozilla::Maybe<PropertyInfo> lookupPure(PropertyName* name) {
    return lookupPure(NameToId(name));
  }

  bool containsPure(jsid id) { return lookupPure(id).isSome(); }
  bool containsPure(PropertyName* name) { return containsPure(NameToId(name)); }
  bool containsPure(jsid id, PropertyInfo prop) {
    mozilla::Maybe<PropertyInfo> found = lookupPure(id);
    return found.isSome() && *found == prop;
  }

 private:
  /*
   * Allocate and free an object slot.
   *
   * FIXME: bug 593129 -- slot allocation should be done by object methods
   * after calling object-parameter-free shape methods, avoiding coupling
   * logic across the object vs. shape module wall.
   */
  static bool allocDictionarySlot(JSContext* cx, HandleNativeObject obj,
                                  uint32_t* slotp);

  void freeDictionarySlot(uint32_t slot);

  static MOZ_ALWAYS_INLINE bool maybeConvertToDictionaryForAdd(
      JSContext* cx, HandleNativeObject obj);

 public:
  // Add a new property. Must only be used when the |id| is not already present
  // in the object's shape. Checks for non-extensibility must be done by the
  // callers.
  static bool addProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                          PropertyFlags flags, uint32_t* slotOut);

  static bool addProperty(JSContext* cx, HandleNativeObject obj,
                          HandlePropertyName name, PropertyFlags flags,
                          uint32_t* slotOut) {
    RootedId id(cx, NameToId(name));
    return addProperty(cx, obj, id, flags, slotOut);
  }

  static bool addPropertyInReservedSlot(JSContext* cx, HandleNativeObject obj,
                                        HandleId id, uint32_t slot,
                                        PropertyFlags flags);
  static bool addPropertyInReservedSlot(JSContext* cx, HandleNativeObject obj,
                                        HandlePropertyName name, uint32_t slot,
                                        PropertyFlags flags) {
    RootedId id(cx, NameToId(name));
    return addPropertyInReservedSlot(cx, obj, id, slot, flags);
  }

  static bool addCustomDataProperty(JSContext* cx, HandleNativeObject obj,
                                    HandleId id, PropertyFlags flags);

  // Change a property with key |id| in this object. The object must already
  // have a property (stored in the shape tree) with this |id|.
  static bool changeProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                             PropertyFlags flags, uint32_t* slotOut);

  static bool changeCustomDataPropAttributes(JSContext* cx,
                                             HandleNativeObject obj,
                                             HandleId id, PropertyFlags flags);

  // Remove the property named by id from this object.
  static bool removeProperty(JSContext* cx, HandleNativeObject obj,
                             HandleId id);

  static bool freezeOrSealProperties(JSContext* cx, HandleNativeObject obj,
                                     IntegrityLevel level);

 protected:
  static bool changeNumFixedSlotsAfterSwap(JSContext* cx,
                                           HandleNativeObject obj,
                                           uint32_t nfixed);

  [[nodiscard]] static bool fillInAfterSwap(JSContext* cx,
                                            HandleNativeObject obj,
                                            NativeObject* old,
                                            HandleValueVector values,
                                            void* priv);

 public:
  // Return true if this object has been converted from shared-immutable
  // shapes to object-owned dictionary shapes.
  bool inDictionaryMode() const { return shape()->isDictionary(); }

  const Value& getSlot(uint32_t slot) const {
    MOZ_ASSERT(slotInRange(slot));
    uint32_t fixed = numFixedSlots();
    if (slot < fixed) {
      return fixedSlots()[slot];
    }
    return slots_[slot - fixed];
  }

  const HeapSlot* getSlotAddressUnchecked(uint32_t slot) const {
    uint32_t fixed = numFixedSlots();
    if (slot < fixed) {
      return fixedSlots() + slot;
    }
    return slots_ + (slot - fixed);
  }

  HeapSlot* getSlotAddressUnchecked(uint32_t slot) {
    uint32_t fixed = numFixedSlots();
    if (slot < fixed) {
      return fixedSlots() + slot;
    }
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

  MOZ_ALWAYS_INLINE HeapSlot& getSlotRef(uint32_t slot) {
    MOZ_ASSERT(slotInRange(slot));
    return *getSlotAddress(slot);
  }

  MOZ_ALWAYS_INLINE const HeapSlot& getSlotRef(uint32_t slot) const {
    MOZ_ASSERT(slotInRange(slot));
    return *getSlotAddress(slot);
  }

  // Check requirements on values stored to this object.
  MOZ_ALWAYS_INLINE void checkStoredValue(const Value& v) {
    MOZ_ASSERT(IsObjectValueInCompartment(v, compartment()));
    MOZ_ASSERT(AtomIsMarked(zoneFromAnyThread(), v));
    MOZ_ASSERT_IF(v.isMagic() && v.whyMagic() == JS_ELEMENTS_HOLE,
                  !denseElementsArePacked());
  }

  MOZ_ALWAYS_INLINE void setSlot(uint32_t slot, const Value& value) {
    MOZ_ASSERT(slotInRange(slot));
    checkStoredValue(value);
    getSlotRef(slot).set(this, HeapSlot::Slot, slot, value);
  }

  MOZ_ALWAYS_INLINE void initSlot(uint32_t slot, const Value& value) {
    MOZ_ASSERT(getSlot(slot).isUndefined());
    MOZ_ASSERT(slotInRange(slot));
    checkStoredValue(value);
    initSlotUnchecked(slot, value);
  }

  MOZ_ALWAYS_INLINE void initSlotUnchecked(uint32_t slot, const Value& value) {
    getSlotAddressUnchecked(slot)->init(this, HeapSlot::Slot, slot, value);
  }

  // Returns the GetterSetter for an accessor property.
  GetterSetter* getGetterSetter(uint32_t slot) const {
    return getSlot(slot).toGCThing()->as<GetterSetter>();
  }
  GetterSetter* getGetterSetter(PropertyInfo prop) const {
    MOZ_ASSERT(prop.isAccessorProperty());
    return getGetterSetter(prop.slot());
  }

  // Returns the (possibly nullptr) getter or setter object. |prop| and |slot|
  // must be (for) an accessor property.
  JSObject* getGetter(uint32_t slot) const {
    return getGetterSetter(slot)->getter();
  }
  JSObject* getGetter(PropertyInfo prop) const {
    return getGetterSetter(prop)->getter();
  }
  JSObject* getSetter(PropertyInfo prop) const {
    return getGetterSetter(prop)->setter();
  }

  // Returns true if the property has a non-nullptr getter or setter object.
  // |prop| can be any property.
  bool hasGetter(PropertyInfo prop) const {
    return prop.isAccessorProperty() && getGetter(prop);
  }
  bool hasSetter(PropertyInfo prop) const {
    return prop.isAccessorProperty() && getSetter(prop);
  }

  // If the property has a non-nullptr getter/setter, return it as ObjectValue.
  // Else return |undefined|. |prop| must be an accessor property.
  Value getGetterValue(PropertyInfo prop) const {
    MOZ_ASSERT(prop.isAccessorProperty());
    if (JSObject* getterObj = getGetter(prop)) {
      return ObjectValue(*getterObj);
    }
    return UndefinedValue();
  }
  Value getSetterValue(PropertyInfo prop) const {
    MOZ_ASSERT(prop.isAccessorProperty());
    if (JSObject* setterObj = getSetter(prop)) {
      return ObjectValue(*setterObj);
    }
    return UndefinedValue();
  }

  // MAX_FIXED_SLOTS is the biggest number of fixed slots our GC
  // size classes will give an object.
  static constexpr uint32_t MAX_FIXED_SLOTS =
      JS::shadow::Object::MAX_FIXED_SLOTS;

 protected:
  MOZ_ALWAYS_INLINE bool updateSlotsForSpan(JSContext* cx, size_t oldSpan,
                                            size_t newSpan);

 private:
  void prepareElementRangeForOverwrite(size_t start, size_t end) {
    MOZ_ASSERT(end <= getDenseInitializedLength());
    for (size_t i = start; i < end; i++) {
      elements_[i].destroy();
    }
  }

  /*
   * Trigger the write barrier on a range of slots that will no longer be
   * reachable.
   */
  void prepareSlotRangeForOverwrite(size_t start, size_t end) {
    for (size_t i = start; i < end; i++) {
      getSlotAddressUnchecked(i)->destroy();
    }
  }

  inline void shiftDenseElementsUnchecked(uint32_t count);

 public:
  MOZ_ALWAYS_INLINE const Value& getReservedSlot(uint32_t index) const {
    MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
    return getSlot(index);
  }

  MOZ_ALWAYS_INLINE const HeapSlot& getReservedSlotRef(uint32_t index) const {
    MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
    return getSlotRef(index);
  }

  MOZ_ALWAYS_INLINE HeapSlot& getReservedSlotRef(uint32_t index) {
    MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
    return getSlotRef(index);
  }

  MOZ_ALWAYS_INLINE void initReservedSlot(uint32_t index, const Value& v) {
    MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
    initSlot(index, v);
  }

  MOZ_ALWAYS_INLINE void setReservedSlot(uint32_t index, const Value& v) {
    MOZ_ASSERT(index < JSSLOT_FREE(getClass()));
    setSlot(index, v);
  }

  // For slots which are known to always be fixed, due to the way they are
  // allocated.

  HeapSlot& getFixedSlotRef(uint32_t slot) {
    MOZ_ASSERT(slotIsFixed(slot));
    return fixedSlots()[slot];
  }

  const Value& getFixedSlot(uint32_t slot) const {
    MOZ_ASSERT(slotIsFixed(slot));
    return fixedSlots()[slot];
  }

  void setFixedSlot(uint32_t slot, const Value& value) {
    MOZ_ASSERT(slotIsFixed(slot));
    checkStoredValue(value);
    fixedSlots()[slot].set(this, HeapSlot::Slot, slot, value);
  }

  void initFixedSlot(uint32_t slot, const Value& value) {
    MOZ_ASSERT(slotIsFixed(slot));
    checkStoredValue(value);
    fixedSlots()[slot].init(this, HeapSlot::Slot, slot, value);
  }

  /*
   * Calculate the number of dynamic slots to allocate to cover the properties
   * in an object with the given number of fixed slots and slot span.
   */
  static MOZ_ALWAYS_INLINE uint32_t calculateDynamicSlots(uint32_t nfixed,
                                                          uint32_t span,
                                                          const JSClass* clasp);
  static MOZ_ALWAYS_INLINE uint32_t calculateDynamicSlots(Shape* shape);

  ObjectSlots* getSlotsHeader() const { return ObjectSlots::fromSlots(slots_); }

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
    static_assert(
        NativeObject::MAX_DENSE_ELEMENTS_COUNT <= INT32_MAX / sizeof(JS::Value),
        "every caller of this method require that an element "
        "count multiplied by sizeof(Value) can't overflow "
        "uint32_t (and sometimes int32_t ,too)");
  }

  ObjectElements* getElementsHeader() const {
    return ObjectElements::fromElements(elements_);
  }

  // Returns a pointer to the first element, including shifted elements.
  inline HeapSlot* unshiftedElements() const {
    return elements_ - getElementsHeader()->numShiftedElements();
  }

  // Like getElementsHeader, but returns a pointer to the unshifted header.
  // This is mainly useful for free()ing dynamic elements: the pointer
  // returned here is the one we got from malloc.
  void* getUnshiftedElementsHeader() const {
    return ObjectElements::fromElements(unshiftedElements());
  }

  uint32_t unshiftedIndex(uint32_t index) const {
    return index + getElementsHeader()->numShiftedElements();
  }

  /* Accessors for elements. */
  bool ensureElements(JSContext* cx, uint32_t capacity) {
    MOZ_ASSERT(isExtensible());
    if (capacity > getDenseCapacity()) {
      return growElements(cx, capacity);
    }
    return true;
  }

  // Try to shift |count| dense elements, see the "Shifted elements" comment.
  inline bool tryShiftDenseElements(uint32_t count);

  // Try to make space for |count| dense elements at the start of the array.
  bool tryUnshiftDenseElements(uint32_t count);

  // Move the elements header and all shifted elements to the start of the
  // allocated elements space, so that numShiftedElements is 0 afterwards.
  void moveShiftedElements();

  // If this object has many shifted elements call moveShiftedElements.
  void maybeMoveShiftedElements();

  static bool goodElementsAllocationAmount(JSContext* cx, uint32_t reqAllocated,
                                           uint32_t length,
                                           uint32_t* goodAmount);
  bool growElements(JSContext* cx, uint32_t newcap);
  void shrinkElements(JSContext* cx, uint32_t cap);

 private:
  // Run a post write barrier that encompasses multiple contiguous elements in a
  // single step.
  inline void elementsRangePostWriteBarrier(uint32_t start, uint32_t count);

 public:
  void shrinkCapacityToInitializedLength(JSContext* cx);

 private:
  void setDenseInitializedLengthInternal(uint32_t length) {
    MOZ_ASSERT(length <= getDenseCapacity());
    MOZ_ASSERT(!denseElementsAreFrozen());
    prepareElementRangeForOverwrite(length,
                                    getElementsHeader()->initializedLength);
    getElementsHeader()->initializedLength = length;
  }

 public:
  void setDenseInitializedLength(uint32_t length) {
    MOZ_ASSERT(isExtensible());
    setDenseInitializedLengthInternal(length);
  }

  void setDenseInitializedLengthMaybeNonExtensible(JSContext* cx,
                                                   uint32_t length) {
    setDenseInitializedLengthInternal(length);
    if (!isExtensible()) {
      shrinkCapacityToInitializedLength(cx);
    }
  }

  inline void ensureDenseInitializedLength(uint32_t index, uint32_t extra);

  void setDenseElement(uint32_t index, const Value& val) {
    // Note: Streams code can call this for the internal ListObject type with
    // MagicValue(JS_WRITABLESTREAM_CLOSE_RECORD).
    MOZ_ASSERT_IF(val.isMagic(), val.whyMagic() != JS_ELEMENTS_HOLE);
    setDenseElementUnchecked(index, val);
  }

  void initDenseElement(uint32_t index, const Value& val) {
    MOZ_ASSERT(!val.isMagic(JS_ELEMENTS_HOLE));
    initDenseElementUnchecked(index, val);
  }

 private:
  // Note: 'Unchecked' here means we don't assert |val| isn't the hole
  // MagicValue.
  void initDenseElementUnchecked(uint32_t index, const Value& val) {
    MOZ_ASSERT(index < getDenseInitializedLength());
    MOZ_ASSERT(isExtensible());
    checkStoredValue(val);
    elements_[index].init(this, HeapSlot::Element, unshiftedIndex(index), val);
  }
  void setDenseElementUnchecked(uint32_t index, const Value& val) {
    MOZ_ASSERT(index < getDenseInitializedLength());
    MOZ_ASSERT(!denseElementsAreFrozen());
    checkStoredValue(val);
    elements_[index].set(this, HeapSlot::Element, unshiftedIndex(index), val);
  }

  // Mark the dense elements as possibly containing holes.
  inline void markDenseElementsNotPacked();

 public:
  inline void initDenseElementHole(uint32_t index);
  inline void setDenseElementHole(uint32_t index);
  inline void removeDenseElementForSparseIndex(uint32_t index);

  inline void copyDenseElements(uint32_t dstStart, const Value* src,
                                uint32_t count);

  inline void initDenseElements(const Value* src, uint32_t count);
  inline void initDenseElements(NativeObject* src, uint32_t srcStart,
                                uint32_t count);

  // Store the Values in the range [begin, end) as elements of this array.
  //
  // Preconditions: This must be a boring ArrayObject with dense initialized
  // length 0: no shifted elements, no frozen elements, no fixed "length", not
  // indexed, not inextensible, not copy-on-write. Existing capacity is
  // optional.
  //
  // This runs write barriers but does not update types. `end - begin` must
  // return the size of the range, which must be >= 0 and fit in an int32_t.
  template <typename Iter>
  [[nodiscard]] inline bool initDenseElementsFromRange(JSContext* cx,
                                                       Iter begin, Iter end);

  inline void moveDenseElements(uint32_t dstStart, uint32_t srcStart,
                                uint32_t count);
  inline void reverseDenseElementsNoPreBarrier(uint32_t length);

  inline DenseElementResult setOrExtendDenseElements(JSContext* cx,
                                                     uint32_t start,
                                                     const Value* vp,
                                                     uint32_t count);

  bool denseElementsAreSealed() const {
    return getElementsHeader()->isSealed();
  }
  bool denseElementsAreFrozen() const {
    return hasFlag(ObjectFlag::FrozenElements);
  }

  bool denseElementsArePacked() const {
    return getElementsHeader()->isPacked();
  }

  void markDenseElementsMaybeInIteration() {
    getElementsHeader()->markMaybeInIteration();
  }

  // Return whether the object's dense elements might be in the midst of for-in
  // iteration. We rely on this to be able to safely delete or move dense array
  // elements without worrying about updating in-progress iterators.
  // See bug 690622.
  //
  // Note that it's fine to return false if this object is on the prototype of
  // another object: SuppressDeletedProperty only suppresses properties deleted
  // from the iterated object itself.
  inline bool denseElementsHaveMaybeInIterationFlag();
  inline bool denseElementsMaybeInIteration();

  // Ensures that the object can hold at least index + extra elements. This
  // returns DenseElement_Success on success, DenseElement_Failed on failure
  // to grow the array, or DenseElement_Incomplete when the object is too
  // sparse to grow (this includes the case of index + extra overflow). In
  // the last two cases the object is kept intact.
  inline DenseElementResult ensureDenseElements(JSContext* cx, uint32_t index,
                                                uint32_t extra);

  inline DenseElementResult extendDenseElements(JSContext* cx,
                                                uint32_t requiredCapacity,
                                                uint32_t extra);

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
  bool willBeSparseElements(uint32_t requiredCapacity,
                            uint32_t newElementsHint);

  /*
   * After adding a sparse index to obj, see if it should be converted to use
   * dense elements.
   */
  static DenseElementResult maybeDensifySparseElements(JSContext* cx,
                                                       HandleNativeObject obj);
  static bool densifySparseElements(JSContext* cx, HandleNativeObject obj);

  inline HeapSlot* fixedElements() const {
    static_assert(2 * sizeof(Value) == sizeof(ObjectElements),
                  "when elements are stored inline, the first two "
                  "slots will hold the ObjectElements header");
    return &fixedSlots()[2];
  }

#ifdef DEBUG
  bool canHaveNonEmptyElements();
#endif

  void setEmptyElements() { elements_ = emptyObjectElements; }

  void setFixedElements(uint32_t numShifted = 0) {
    MOZ_ASSERT(canHaveNonEmptyElements());
    elements_ = fixedElements() + numShifted;
  }

  inline bool hasDynamicElements() const {
    /*
     * Note: for objects with zero fixed slots this could potentially give
     * a spurious 'true' result, if the end of this object is exactly
     * aligned with the end of its arena and dynamic slots are allocated
     * immediately afterwards. Such cases cannot occur for dense arrays
     * (which have at least two fixed slots) and can only result in a leak.
     */
    return !hasEmptyElements() && !hasFixedElements();
  }

  inline bool hasFixedElements() const {
    return unshiftedElements() == fixedElements();
  }

  inline bool hasEmptyElements() const {
    return elements_ == emptyObjectElements ||
           elements_ == emptyObjectElementsShared;
  }

  /*
   * Get a pointer to the unused data in the object's allocation immediately
   * following this object, for use with objects which allocate a larger size
   * class than they need and store non-elements data inline.
   */
  inline uint8_t* fixedData(size_t nslots) const;

  inline void privatePreWriteBarrier(HeapSlot* pprivate);

  /* Private data accessors. */

 private:
  HeapSlot& privateRef(uint32_t nfixed) const {
    /*
     * The private field of an object is used to hold a pointer by storing it as
     * a PrivateValue(). Private fields are stored immediately after the last
     * fixed slot of the object.
     */
    MOZ_ASSERT(isNumFixedSlots(nfixed));
    MOZ_ASSERT(hasPrivate());
    return fixedSlots()[nfixed];
  }

 public:
  bool hasPrivate() const { return getClass()->hasPrivate(); }

  void* getPrivate() const { return getPrivate(numFixedSlots()); }
  void* getPrivate(uint32_t nfixed) const {
    return privateRef(nfixed).toPrivate();
  }

  void initPrivate(void* data) {
    uint32_t nfixed = numFixedSlots();
    privateRef(nfixed).unbarrieredSet(PrivateValue(data));
  }

  void setPrivate(void* data) { setPrivate(numFixedSlots(), data); }
  void setPrivate(uint32_t nfixed, void* data) {
    HeapSlot* pprivate = &privateRef(nfixed);
    privatePreWriteBarrier(pprivate);
    setPrivateUnbarriered(nfixed, data);
  }

  void setPrivateGCThing(gc::Cell* cell) {
#ifdef DEBUG
    if (IsMarkedBlack(this)) {
      JS::AssertCellIsNotGray(cell);
    }
#endif
    uint32_t nfixed = numFixedSlots();
    HeapSlot* pprivate = &privateRef(nfixed);
    Cell* prev = static_cast<gc::Cell*>(pprivate->toPrivate());
    privatePreWriteBarrier(pprivate);
    setPrivateUnbarriered(nfixed, cell);
    gc::PostWriteBarrierCell(this, prev, cell);
  }

  void setPrivateUnbarriered(void* data) {
    setPrivateUnbarriered(numFixedSlots(), data);
  }
  void setPrivateUnbarriered(uint32_t nfixed, void* data) {
    privateRef(nfixed).unbarrieredSet(PrivateValue(data));
  }

  /* Return the allocKind we would use if we were to tenure this object. */
  inline js::gc::AllocKind allocKindForTenure() const;

  // Native objects are never wrappers, so a native object always has a realm
  // and global.
  JS::Realm* realm() const { return nonCCWRealm(); }
  inline js::GlobalObject& global() const;

  /* JIT Accessors */
  static size_t offsetOfElements() { return offsetof(NativeObject, elements_); }
  static size_t offsetOfFixedElements() {
    return sizeof(NativeObject) + sizeof(ObjectElements);
  }

  static constexpr size_t getFixedSlotOffset(size_t slot) {
    MOZ_ASSERT(slot < MAX_FIXED_SLOTS);
    return sizeof(NativeObject) + slot * sizeof(Value);
  }
  static constexpr size_t getPrivateDataOffset(size_t nfixed) {
    return getFixedSlotOffset(nfixed);
  }
  static constexpr size_t getFixedSlotIndexFromOffset(size_t offset) {
    MOZ_ASSERT(offset >= sizeof(NativeObject));
    offset -= sizeof(NativeObject);
    MOZ_ASSERT(offset % sizeof(Value) == 0);
    MOZ_ASSERT(offset / sizeof(Value) < MAX_FIXED_SLOTS);
    return offset / sizeof(Value);
  }
  static constexpr size_t getDynamicSlotIndexFromOffset(size_t offset) {
    MOZ_ASSERT(offset % sizeof(Value) == 0);
    return offset / sizeof(Value);
  }
  static size_t offsetOfSlots() { return offsetof(NativeObject, slots_); }
};

inline void NativeObject::privatePreWriteBarrier(HeapSlot* pprivate) {
  JS::shadow::Zone* shadowZone = this->shadowZoneFromAnyThread();
  if (shadowZone->needsIncrementalBarrier() && pprivate->get().toPrivate() &&
      getClass()->hasTrace()) {
    getClass()->doTrace(shadowZone->barrierTracer(), this);
  }
}

/*** Standard internal methods **********************************************/

/*
 * These functions should follow the algorithms in ES6 draft rev 29 section 9.1
 * ("Ordinary Object Internal Methods"). It's an ongoing project.
 *
 * Many native objects are not "ordinary" in ES6, so these functions also have
 * to serve some of the special needs of Functions (9.2, 9.3, 9.4.1), Arrays
 * (9.4.2), Strings (9.4.3), and so on.
 */

extern bool NativeDefineProperty(JSContext* cx, HandleNativeObject obj,
                                 HandleId id,
                                 Handle<JS::PropertyDescriptor> desc,
                                 ObjectOpResult& result);

extern bool NativeDefineDataProperty(JSContext* cx, HandleNativeObject obj,
                                     HandleId id, HandleValue value,
                                     unsigned attrs, ObjectOpResult& result);

/* If the result out-param is omitted, throw on failure. */

extern bool NativeDefineAccessorProperty(JSContext* cx, HandleNativeObject obj,
                                         HandleId id, HandleObject getter,
                                         HandleObject setter, unsigned attrs);

extern bool NativeDefineDataProperty(JSContext* cx, HandleNativeObject obj,
                                     HandleId id, HandleValue value,
                                     unsigned attrs);

extern bool NativeDefineDataProperty(JSContext* cx, HandleNativeObject obj,
                                     PropertyName* name, HandleValue value,
                                     unsigned attrs);

extern bool NativeHasProperty(JSContext* cx, HandleNativeObject obj,
                              HandleId id, bool* foundp);

extern bool NativeGetOwnPropertyDescriptor(
    JSContext* cx, HandleNativeObject obj, HandleId id,
    MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc);

extern bool NativeGetProperty(JSContext* cx, HandleNativeObject obj,
                              HandleValue receiver, HandleId id,
                              MutableHandleValue vp);

extern bool NativeGetPropertyNoGC(JSContext* cx, NativeObject* obj,
                                  const Value& receiver, jsid id, Value* vp);

inline bool NativeGetProperty(JSContext* cx, HandleNativeObject obj,
                              HandleId id, MutableHandleValue vp) {
  RootedValue receiver(cx, ObjectValue(*obj));
  return NativeGetProperty(cx, obj, receiver, id, vp);
}

extern bool NativeGetElement(JSContext* cx, HandleNativeObject obj,
                             HandleValue receiver, int32_t index,
                             MutableHandleValue vp);

bool GetSparseElementHelper(JSContext* cx, HandleArrayObject obj,
                            int32_t int_id, MutableHandleValue result);

bool SetPropertyByDefining(JSContext* cx, HandleId id, HandleValue v,
                           HandleValue receiver, ObjectOpResult& result);

bool SetPropertyOnProto(JSContext* cx, HandleObject obj, HandleId id,
                        HandleValue v, HandleValue receiver,
                        ObjectOpResult& result);

bool AddOrUpdateSparseElementHelper(JSContext* cx, HandleArrayObject obj,
                                    int32_t int_id, HandleValue v, bool strict);

/*
 * Indicates whether an assignment operation is qualified (`x.y = 0`) or
 * unqualified (`y = 0`). In strict mode, the latter is an error if no such
 * variable already exists.
 *
 * Used as an argument to NativeSetProperty.
 */
enum QualifiedBool { Unqualified = 0, Qualified = 1 };

template <QualifiedBool Qualified>
extern bool NativeSetProperty(JSContext* cx, HandleNativeObject obj,
                              HandleId id, HandleValue v, HandleValue receiver,
                              ObjectOpResult& result);

extern bool NativeSetElement(JSContext* cx, HandleNativeObject obj,
                             uint32_t index, HandleValue v,
                             HandleValue receiver, ObjectOpResult& result);

extern bool NativeDeleteProperty(JSContext* cx, HandleNativeObject obj,
                                 HandleId id, ObjectOpResult& result);

/*** SpiderMonkey nonstandard internal methods ******************************/

template <AllowGC allowGC>
extern bool NativeLookupOwnProperty(
    JSContext* cx, typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
    typename MaybeRooted<jsid, allowGC>::HandleType id, PropertyResult* propp);

/*
 * Get a property from `receiver`, after having already done a lookup and found
 * the property on a native object `obj`.
 *
 * `prop` must be present in obj's shape.
 */
extern bool NativeGetExistingProperty(JSContext* cx, HandleObject receiver,
                                      HandleNativeObject obj, HandleId id,
                                      PropertyInfo prop, MutableHandleValue vp);

/* * */

extern bool GetNameBoundInEnvironment(JSContext* cx, HandleObject env,
                                      HandleId id, MutableHandleValue vp);

} /* namespace js */

template <>
inline bool JSObject::is<js::NativeObject>() const {
  return getClass()->isNativeObject();
}

namespace js {

// Alternate to JSObject::as<NativeObject>() that tolerates null pointers.
inline NativeObject* MaybeNativeObject(JSObject* obj) {
  return obj ? &obj->as<NativeObject>() : nullptr;
}

// Defined in NativeObject-inl.h.
bool IsPackedArray(JSObject* obj);

// Initialize an object's reserved slot with a private value pointing to
// malloc-allocated memory and associate the memory with the object.
//
// This call should be matched with a call to JSFreeOp::free_/delete_ in the
// object's finalizer to free the memory and update the memory accounting.

inline void InitReservedSlot(NativeObject* obj, uint32_t slot, void* ptr,
                             size_t nbytes, MemoryUse use) {
  AddCellMemory(obj, nbytes, use);
  obj->initReservedSlot(slot, PrivateValue(ptr));
}
template <typename T>
inline void InitReservedSlot(NativeObject* obj, uint32_t slot, T* ptr,
                             MemoryUse use) {
  InitReservedSlot(obj, slot, ptr, sizeof(T), use);
}

// Initialize an object's private slot with a pointer to malloc-allocated memory
// and associate the memory with the object.
//
// This call should be matched with a call to JSFreeOp::free_/delete_ in the
// object's finalizer to free the memory and update the memory accounting.

inline void InitObjectPrivate(NativeObject* obj, void* ptr, size_t nbytes,
                              MemoryUse use) {
  AddCellMemory(obj, nbytes, use);
  obj->initPrivate(ptr);
}
template <typename T>
inline void InitObjectPrivate(NativeObject* obj, T* ptr, MemoryUse use) {
  InitObjectPrivate(obj, ptr, sizeof(T), use);
}

}  // namespace js

#endif /* vm_NativeObject_h */
