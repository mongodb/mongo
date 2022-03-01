/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Iteration_h
#define vm_Iteration_h

/*
 * JavaScript iterators.
 */

#include "mozilla/ArrayUtils.h"
#include "mozilla/MemoryReporting.h"

#include "builtin/SelfHostingDefines.h"
#include "gc/Barrier.h"
#include "vm/Stack.h"

namespace js {

class PlainObject;
class PropertyIteratorObject;

struct NativeIterator {
 private:
  // Object being iterated.  Non-null except in NativeIterator sentinels and
  // empty property iterators created when |null| or |undefined| is iterated.
  GCPtrObject objectBeingIterated_ = {};

  // Internal iterator object.
  const GCPtrObject iterObj_ = {};

  // The end of GCPtrShapes that appear directly after |this|, as part of an
  // overall allocation that stores |*this|, shapes, and iterated strings.
  // Once this has been fully initialized, it also equals the start of iterated
  // strings.
  GCPtrShape* shapesEnd_;  // initialized by constructor

  // The next property, pointing into an array of strings directly after any
  // GCPtrShapes that appear directly after |*this|, as part of an overall
  // allocation that stores |*this|, shapes, and iterated strings.
  GCPtrLinearString* propertyCursor_;  // initialized by constructor

  // The limit/end of properties to iterate (and, assuming no error occurred
  // while constructing this NativeIterator, the end of the full allocation
  // storing |*this|, shapes, and strings).  Beware!  This value may change as
  // properties are deleted from the observed object.
  GCPtrLinearString* propertiesEnd_;  // initialized by constructor

  HashNumber shapesHash_;  // initialized by constructor

 public:
  // For cacheable native iterators, whether the iterator is currently
  // active.  Not serialized by XDR.
  struct Flags {
    // This flag is set when all shapes and properties associated with this
    // NativeIterator have been initialized, such that |shapesEnd_|, in
    // addition to being the end of shapes, is also the beginning of
    // properties.
    //
    // This flag is only *not* set when a NativeIterator is in the process
    // of being constructed.  At such time |shapesEnd_| accounts only for
    // shapes that have been initialized -- potentially none of them.
    // Instead, |propertyCursor_| is initialized to the ultimate/actual
    // start of properties and must be used instead of |propertiesBegin()|,
    // which asserts that this flag is present to guard against misuse.
    static constexpr uint32_t Initialized = 0x1;

    // This flag indicates that this NativeIterator is currently being used
    // to enumerate an object's properties and has not yet been closed.
    static constexpr uint32_t Active = 0x2;

    // This flag indicates that the object being enumerated by this
    // |NativeIterator| had a property deleted from it before it was
    // visited, forcing the properties array in this to be mutated to
    // remove it.
    static constexpr uint32_t HasUnvisitedPropertyDeletion = 0x4;

    // If any of these bits are set on a |NativeIterator|, it isn't
    // currently reusable.  (An active |NativeIterator| can't be stolen
    // *right now*; a |NativeIterator| that's had its properties mutated
    // can never be reused, because it would give incorrect results.)
    static constexpr uint32_t NotReusable =
        Active | HasUnvisitedPropertyDeletion;
  };

 private:
  static constexpr uint32_t FlagsBits = 3;
  static constexpr uint32_t FlagsMask = (1 << FlagsBits) - 1;

 public:
  static constexpr uint32_t PropCountLimit = 1 << (32 - FlagsBits);

 private:
  // While in compartment->enumerators, these form a doubly linked list.
  NativeIterator* next_ = nullptr;
  NativeIterator* prev_ = nullptr;

  // Stores Flags bits in the lower bits and the initial property count above
  // them.
  uint32_t flagsAndCount_ = 0;

#ifdef DEBUG
  // If true, this iterator may contain indexed properties that came from
  // objects on the prototype chain. This is used by certain debug assertions.
  bool maybeHasIndexedPropertiesFromProto_ = false;
#endif

  // END OF PROPERTIES

  // No further fields appear after here *in NativeIterator*, but this class
  // is always allocated with space tacked on immediately after |this| to
  // store iterated property names up to |props_end| and |numShapes| shapes
  // after that.

 public:
  /**
   * Initialize a NativeIterator properly allocated for |props.length()|
   * properties and |numShapes| shapes.
   *
   * Despite being a constructor, THIS FUNCTION CAN REPORT ERRORS.  Users
   * MUST set |*hadError = false| on entry and consider |*hadError| on return
   * to mean this function failed.
   */
  NativeIterator(JSContext* cx, Handle<PropertyIteratorObject*> propIter,
                 Handle<JSObject*> objBeingIterated, HandleIdVector props,
                 uint32_t numShapes, HashNumber shapesHash, bool* hadError);

  /** Initialize an |ObjectRealm::enumerators| sentinel. */
  NativeIterator();

  JSObject* objectBeingIterated() const { return objectBeingIterated_; }

  void changeObjectBeingIterated(JSObject& obj) { objectBeingIterated_ = &obj; }

  GCPtrShape* shapesBegin() const {
    static_assert(alignof(GCPtrShape) <= alignof(NativeIterator),
                  "NativeIterator must be aligned to begin storing "
                  "GCPtrShapes immediately after it with no required padding");
    const NativeIterator* immediatelyAfter = this + 1;
    auto* afterNonConst = const_cast<NativeIterator*>(immediatelyAfter);
    return reinterpret_cast<GCPtrShape*>(afterNonConst);
  }

  GCPtrShape* shapesEnd() const { return shapesEnd_; }

  uint32_t shapeCount() const {
    return mozilla::PointerRangeSize(shapesBegin(), shapesEnd());
  }

  GCPtrLinearString* propertiesBegin() const {
    static_assert(alignof(GCPtrShape) >= alignof(GCPtrLinearString),
                  "GCPtrLinearStrings for properties must be able to appear "
                  "directly after any GCPtrShapes after this NativeIterator, "
                  "with no padding space required for correct alignment");
    static_assert(alignof(NativeIterator) >= alignof(GCPtrLinearString),
                  "GCPtrLinearStrings for properties must be able to appear "
                  "directly after this NativeIterator when no GCPtrShapes are "
                  "present, with no padding space required for correct "
                  "alignment");

    // We *could* just check the assertion below if we wanted, but the
    // incompletely-initialized NativeIterator case matters for so little
    // code that we prefer not imposing the condition-check on every single
    // user.
    MOZ_ASSERT(isInitialized(),
               "NativeIterator must be initialized, or else |shapesEnd_| "
               "isn't necessarily the start of properties and instead "
               "|propertyCursor_| is");

    return reinterpret_cast<GCPtrLinearString*>(shapesEnd_);
  }

  GCPtrLinearString* propertiesEnd() const { return propertiesEnd_; }

  GCPtrLinearString* nextProperty() const { return propertyCursor_; }

  MOZ_ALWAYS_INLINE JS::Value nextIteratedValueAndAdvance() {
    if (propertyCursor_ >= propertiesEnd_) {
      MOZ_ASSERT(propertyCursor_ == propertiesEnd_);
      return JS::MagicValue(JS_NO_ITER_VALUE);
    }

    JSLinearString* str = *propertyCursor_;
    incCursor();
    return JS::StringValue(str);
  }

  void resetPropertyCursorForReuse() {
    MOZ_ASSERT(isInitialized());

    // This function is called unconditionally on IteratorClose, so
    // unvisited properties might have been deleted, so we can't assert
    // this NativeIterator is reusable.  (Should we not bother resetting
    // the cursor in that case?)

    // Note: JIT code inlines |propertyCursor_| resetting when an iterator
    //       ends: see |CodeGenerator::visitIteratorEnd|.
    propertyCursor_ = propertiesBegin();
  }

  bool previousPropertyWas(JS::Handle<JSLinearString*> str) {
    MOZ_ASSERT(isInitialized());
    return propertyCursor_ > propertiesBegin() && propertyCursor_[-1] == str;
  }

  size_t numKeys() const {
    return mozilla::PointerRangeSize(propertiesBegin(), propertiesEnd());
  }

  void trimLastProperty() {
    MOZ_ASSERT(isInitialized());

    propertiesEnd_--;

    // This invokes the pre barrier on this property, since it's no longer
    // going to be marked, and it ensures that any existing remembered set
    // entry will be dropped.
    *propertiesEnd_ = nullptr;
  }

  JSObject* iterObj() const { return iterObj_; }
  GCPtrLinearString* currentProperty() const {
    MOZ_ASSERT(propertyCursor_ < propertiesEnd());
    return propertyCursor_;
  }

  NativeIterator* next() { return next_; }

  void incCursor() {
    MOZ_ASSERT(isInitialized());
    propertyCursor_++;
  }

  HashNumber shapesHash() const { return shapesHash_; }

  bool isInitialized() const { return flags() & Flags::Initialized; }

  size_t allocationSize() const;

#ifdef DEBUG
  void setMaybeHasIndexedPropertiesFromProto() {
    maybeHasIndexedPropertiesFromProto_ = true;
  }
  bool maybeHasIndexedPropertiesFromProto() const {
    return maybeHasIndexedPropertiesFromProto_;
  }
#endif

 private:
  uint32_t flags() const { return flagsAndCount_ & FlagsMask; }

  uint32_t initialPropertyCount() const { return flagsAndCount_ >> FlagsBits; }

  static uint32_t initialFlagsAndCount(uint32_t count) {
    // No flags are initially set.
    MOZ_ASSERT(count < PropCountLimit);
    return count << FlagsBits;
  }

  void setFlags(uint32_t flags) {
    MOZ_ASSERT((flags & ~FlagsMask) == 0);
    flagsAndCount_ = (initialPropertyCount() << FlagsBits) | flags;
  }

  void markInitialized() {
    MOZ_ASSERT(flags() == 0);
    setFlags(Flags::Initialized);
  }

 public:
  bool isActive() const {
    MOZ_ASSERT(isInitialized());

    return flags() & Flags::Active;
  }

  void markActive() {
    MOZ_ASSERT(isInitialized());

    flagsAndCount_ |= Flags::Active;
  }

  void markInactive() {
    MOZ_ASSERT(isInitialized());

    flagsAndCount_ &= ~Flags::Active;
  }

  bool isReusable() const {
    MOZ_ASSERT(isInitialized());

    // Cached NativeIterators are reusable if they're not currently active
    // and their properties array hasn't been mutated, i.e. if only
    // |Flags::Initialized| is set.  Using |Flags::NotReusable| to test
    // would also work, but this formulation is safer against memory
    // corruption.
    return flags() == Flags::Initialized;
  }

  void markHasUnvisitedPropertyDeletion() {
    MOZ_ASSERT(isInitialized());

    flagsAndCount_ |= Flags::HasUnvisitedPropertyDeletion;
  }

  void link(NativeIterator* other) {
    // The NativeIterator sentinel doesn't have to be linked, because it's
    // the start of the list.  Anything else added should have been
    // initialized.
    MOZ_ASSERT(isInitialized());

    /* A NativeIterator cannot appear in the enumerator list twice. */
    MOZ_ASSERT(!next_ && !prev_);

    this->next_ = other;
    this->prev_ = other->prev_;
    other->prev_->next_ = this;
    other->prev_ = this;
  }
  void unlink() {
    MOZ_ASSERT(isInitialized());

    next_->prev_ = prev_;
    prev_->next_ = next_;
    next_ = nullptr;
    prev_ = nullptr;
  }

  static NativeIterator* allocateSentinel(JSContext* cx);

  void trace(JSTracer* trc);

  static constexpr size_t offsetOfObjectBeingIterated() {
    return offsetof(NativeIterator, objectBeingIterated_);
  }

  static constexpr size_t offsetOfShapesEnd() {
    return offsetof(NativeIterator, shapesEnd_);
  }

  static constexpr size_t offsetOfPropertyCursor() {
    return offsetof(NativeIterator, propertyCursor_);
  }

  static constexpr size_t offsetOfPropertiesEnd() {
    return offsetof(NativeIterator, propertiesEnd_);
  }

  static constexpr size_t offsetOfFlagsAndCount() {
    return offsetof(NativeIterator, flagsAndCount_);
  }

  static constexpr size_t offsetOfNext() {
    return offsetof(NativeIterator, next_);
  }

  static constexpr size_t offsetOfPrev() {
    return offsetof(NativeIterator, prev_);
  }
};

class PropertyIteratorObject : public NativeObject {
  static const JSClassOps classOps_;

 public:
  static const JSClass class_;

  // We don't use the fixed slot but the JITs use this constant to load the
  // private value (the NativeIterator*).
  static const uint32_t NUM_FIXED_SLOTS = 1;

  NativeIterator* getNativeIterator() const {
    return static_cast<js::NativeIterator*>(getPrivate());
  }
  void setNativeIterator(js::NativeIterator* ni) { setPrivate(ni); }

  size_t sizeOfMisc(mozilla::MallocSizeOf mallocSizeOf) const;

 private:
  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JSFreeOp* fop, JSObject* obj);
};

class ArrayIteratorObject : public NativeObject {
 public:
  static const JSClass class_;
};

ArrayIteratorObject* NewArrayIteratorTemplate(JSContext* cx);
ArrayIteratorObject* NewArrayIterator(JSContext* cx);

class StringIteratorObject : public NativeObject {
 public:
  static const JSClass class_;
};

StringIteratorObject* NewStringIteratorTemplate(JSContext* cx);
StringIteratorObject* NewStringIterator(JSContext* cx);

class RegExpStringIteratorObject : public NativeObject {
 public:
  static const JSClass class_;
};

RegExpStringIteratorObject* NewRegExpStringIteratorTemplate(JSContext* cx);
RegExpStringIteratorObject* NewRegExpStringIterator(JSContext* cx);

[[nodiscard]] bool EnumerateProperties(JSContext* cx, HandleObject obj,
                                       MutableHandleIdVector props);

PropertyIteratorObject* LookupInIteratorCache(JSContext* cx, HandleObject obj);

JSObject* ValueToIterator(JSContext* cx, HandleValue vp);

void CloseIterator(JSObject* obj);

bool IteratorCloseForException(JSContext* cx, HandleObject obj);

void UnwindIteratorForUncatchableException(JSObject* obj);

extern bool SuppressDeletedProperty(JSContext* cx, HandleObject obj, jsid id);

extern bool SuppressDeletedElement(JSContext* cx, HandleObject obj,
                                   uint32_t index);

#ifdef DEBUG
extern void AssertDenseElementsNotIterated(NativeObject* obj);
#else
inline void AssertDenseElementsNotIterated(NativeObject* obj) {}
#endif

/*
 * IteratorMore() returns the next iteration value. If no value is available,
 * MagicValue(JS_NO_ITER_VALUE) is returned.
 */
inline Value IteratorMore(JSObject* iterobj) {
  NativeIterator* ni =
      iterobj->as<PropertyIteratorObject>().getNativeIterator();
  return ni->nextIteratedValueAndAdvance();
}

/*
 * Create an object of the form { value: VALUE, done: DONE }.
 * ES 2017 draft 7.4.7.
 */
extern PlainObject* CreateIterResultObject(JSContext* cx, HandleValue value,
                                           bool done);

/*
 * Global Iterator constructor.
 * Iterator Helpers proposal 2.1.3.
 */
class IteratorObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass protoClass_;
};

/*
 * Wrapper for iterators created via Iterator.from.
 * Iterator Helpers proposal 2.1.3.3.1.1.
 */
class WrapForValidIteratorObject : public NativeObject {
 public:
  static const JSClass class_;

  enum { IteratedSlot, SlotCount };

  static_assert(
      IteratedSlot == ITERATED_SLOT,
      "IteratedSlot must match self-hosting define for iterated object slot.");
};

WrapForValidIteratorObject* NewWrapForValidIterator(JSContext* cx);

/*
 * Generator-esque object returned by Iterator Helper methods.
 */
class IteratorHelperObject : public NativeObject {
 public:
  static const JSClass class_;

  enum {
    // The implementation (an instance of one of the generators in
    // builtin/Iterator.js).
    // Never null.
    GeneratorSlot,

    SlotCount,
  };

  static_assert(GeneratorSlot == ITERATOR_HELPER_GENERATOR_SLOT,
                "GeneratorSlot must match self-hosting define for generator "
                "object slot.");
};

IteratorHelperObject* NewIteratorHelper(JSContext* cx);

} /* namespace js */

#endif /* vm_Iteration_h */
