/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/NativeObject-inl.h"

#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"

#include <algorithm>
#include <iterator>

#include "gc/MaybeRooted.h"
#include "gc/StableCellHasher.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/Printer.h"               // js::GenericPrinter
#include "js/Value.h"
#include "vm/EqualityOperations.h"  // js::SameValue
#include "vm/GetterSetter.h"        // js::GetterSetter
#include "vm/Interpreter.h"         // js::CallGetter, js::CallSetter
#include "vm/JSONPrinter.h"         // js::JSONPrinter
#include "vm/PlainObject.h"         // js::PlainObject
#include "vm/TypedArrayObject.h"
#include "vm/Watchtower.h"
#include "gc/Nursery-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

using mozilla::CheckedInt;
using mozilla::PodCopy;
using mozilla::RoundUpPow2;

struct EmptyObjectElements {
  const ObjectElements emptyElementsHeader;

  // Add an extra (unused) Value to make sure an out-of-bounds index when
  // masked (resulting in index 0) accesses valid memory.
  const Value val;

 public:
  constexpr EmptyObjectElements()
      : emptyElementsHeader(0, 0), val(UndefinedValue()) {}
  explicit constexpr EmptyObjectElements(ObjectElements::SharedMemory shmem)
      : emptyElementsHeader(0, 0, shmem), val(UndefinedValue()) {}
};

static constexpr EmptyObjectElements emptyElementsHeader;

/* Objects with no elements share one empty set of elements. */
HeapSlot* const js::emptyObjectElements = reinterpret_cast<HeapSlot*>(
    uintptr_t(&emptyElementsHeader) + sizeof(ObjectElements));

static constexpr EmptyObjectElements emptyElementsHeaderShared(
    ObjectElements::SharedMemory::IsShared);

/* Objects with no elements share one empty set of elements. */
HeapSlot* const js::emptyObjectElementsShared = reinterpret_cast<HeapSlot*>(
    uintptr_t(&emptyElementsHeaderShared) + sizeof(ObjectElements));

struct EmptyObjectSlots : public ObjectSlots {
  explicit constexpr EmptyObjectSlots(size_t dictionarySlotSpan)
      : ObjectSlots(0, dictionarySlotSpan, NoUniqueIdInSharedEmptySlots) {}
};

static constexpr EmptyObjectSlots emptyObjectSlotsHeaders[17] = {
    EmptyObjectSlots(0),  EmptyObjectSlots(1),  EmptyObjectSlots(2),
    EmptyObjectSlots(3),  EmptyObjectSlots(4),  EmptyObjectSlots(5),
    EmptyObjectSlots(6),  EmptyObjectSlots(7),  EmptyObjectSlots(8),
    EmptyObjectSlots(9),  EmptyObjectSlots(10), EmptyObjectSlots(11),
    EmptyObjectSlots(12), EmptyObjectSlots(13), EmptyObjectSlots(14),
    EmptyObjectSlots(15), EmptyObjectSlots(16)};

static_assert(std::size(emptyObjectSlotsHeaders) ==
              NativeObject::MAX_FIXED_SLOTS + 1);

MOZ_RUNINIT HeapSlot* const js::emptyObjectSlotsForDictionaryObject[17] = {
    emptyObjectSlotsHeaders[0].slots(),  emptyObjectSlotsHeaders[1].slots(),
    emptyObjectSlotsHeaders[2].slots(),  emptyObjectSlotsHeaders[3].slots(),
    emptyObjectSlotsHeaders[4].slots(),  emptyObjectSlotsHeaders[5].slots(),
    emptyObjectSlotsHeaders[6].slots(),  emptyObjectSlotsHeaders[7].slots(),
    emptyObjectSlotsHeaders[8].slots(),  emptyObjectSlotsHeaders[9].slots(),
    emptyObjectSlotsHeaders[10].slots(), emptyObjectSlotsHeaders[11].slots(),
    emptyObjectSlotsHeaders[12].slots(), emptyObjectSlotsHeaders[13].slots(),
    emptyObjectSlotsHeaders[14].slots(), emptyObjectSlotsHeaders[15].slots(),
    emptyObjectSlotsHeaders[16].slots()};

static_assert(std::size(emptyObjectSlotsForDictionaryObject) ==
              NativeObject::MAX_FIXED_SLOTS + 1);

MOZ_RUNINIT HeapSlot* const js::emptyObjectSlots =
    emptyObjectSlotsForDictionaryObject[0];

#ifdef DEBUG

bool NativeObject::canHaveNonEmptyElements() {
  return !this->is<TypedArrayObject>();
}

#endif  // DEBUG

/* static */
void ObjectElements::PrepareForPreventExtensions(JSContext* cx,
                                                 NativeObject* obj) {
  if (!obj->hasEmptyElements()) {
    obj->shrinkCapacityToInitializedLength(cx);
  }

  // shrinkCapacityToInitializedLength ensures there are no shifted elements.
  MOZ_ASSERT(obj->getElementsHeader()->numShiftedElements() == 0);
}

/* static */
void ObjectElements::PreventExtensions(NativeObject* obj) {
  MOZ_ASSERT(!obj->isExtensible());
  MOZ_ASSERT(obj->getElementsHeader()->numShiftedElements() == 0);
  MOZ_ASSERT(obj->getDenseInitializedLength() == obj->getDenseCapacity());

  if (!obj->hasEmptyElements()) {
    obj->getElementsHeader()->setNotExtensible();
  }
}

/* static */
bool ObjectElements::FreezeOrSeal(JSContext* cx, Handle<NativeObject*> obj,
                                  IntegrityLevel level) {
  MOZ_ASSERT_IF(level == IntegrityLevel::Frozen && obj->is<ArrayObject>(),
                !obj->as<ArrayObject>().lengthIsWritable());
  MOZ_ASSERT(!obj->isExtensible());
  MOZ_ASSERT(obj->getElementsHeader()->numShiftedElements() == 0);

  if (obj->hasEmptyElements() || obj->denseElementsAreFrozen()) {
    return true;
  }

  if (level == IntegrityLevel::Frozen) {
    if (!JSObject::setFlag(cx, obj, ObjectFlag::FrozenElements)) {
      return false;
    }
  }

  if (!obj->denseElementsAreSealed()) {
    obj->getElementsHeader()->seal();
  }

  if (level == IntegrityLevel::Frozen) {
    obj->getElementsHeader()->freeze();
  }

  return true;
}

#if defined(DEBUG) || defined(JS_JITSPEW)

template <typename KnownF, typename UnknownF>
void ForEachObjectElementsFlag(uint16_t flags, KnownF known, UnknownF unknown) {
  for (uint16_t i = 1; i; i = i << 1) {
    if (!(flags & i)) {
      continue;
    }
    switch (ObjectElements::Flags(flags & i)) {
      case ObjectElements::Flags::FIXED:
        known("FIXED");
        break;
      case ObjectElements::Flags::NONWRITABLE_ARRAY_LENGTH:
        known("NONWRITABLE_ARRAY_LENGTH");
        break;
      case ObjectElements::Flags::SHARED_MEMORY:
        known("SHARED_MEMORY");
        break;
      case ObjectElements::Flags::NOT_EXTENSIBLE:
        known("NOT_EXTENSIBLE");
        break;
      case ObjectElements::Flags::SEALED:
        known("SEALED");
        break;
      case ObjectElements::Flags::FROZEN:
        known("FROZEN");
        break;
      case ObjectElements::Flags::NON_PACKED:
        known("NON_PACKED");
        break;
      case ObjectElements::Flags::MAYBE_IN_ITERATION:
        known("MAYBE_IN_ITERATION");
        break;
      default:
        unknown(i);
        break;
    }
  }
}

void ObjectElements::dumpStringContent(js::GenericPrinter& out) const {
  out.printf("<(js::ObjectElements*)0x%p, flags=[", this);

  bool first = true;
  ForEachObjectElementsFlag(
      flags,
      [&](const char* name) {
        if (!first) {
          out.put(", ");
        }
        first = false;

        out.put(name);
      },
      [&](uint16_t value) {
        if (!first) {
          out.put(", ");
        }
        first = false;

        out.printf("Unknown(%04x)", value);
      });
  out.put("]");

  out.printf(", init=%u, capacity=%u, length=%u>", initializedLength, capacity,
             length);
}
#endif

#ifdef DEBUG
static mozilla::Atomic<bool, mozilla::Relaxed> gShapeConsistencyChecksEnabled(
    false);

/* static */
void js::NativeObject::enableShapeConsistencyChecks() {
  gShapeConsistencyChecksEnabled = true;
}

void js::NativeObject::checkShapeConsistency() {
  if (!gShapeConsistencyChecksEnabled) {
    return;
  }

  MOZ_ASSERT(is<NativeObject>());

  if (PropMap* map = shape()->propMap()) {
    map->checkConsistency(this);
  } else {
    MOZ_ASSERT(shape()->propMapLength() == 0);
  }
}
#endif

#ifdef DEBUG

bool js::NativeObject::slotInRange(uint32_t slot,
                                   SentinelAllowed sentinel) const {
  MOZ_ASSERT(!gc::IsForwarded(shape()));
  uint32_t capacity = numFixedSlots() + numDynamicSlots();
  if (sentinel == SENTINEL_ALLOWED) {
    return slot <= capacity;
  }
  return slot < capacity;
}

bool js::NativeObject::slotIsFixed(uint32_t slot) const {
  // We call numFixedSlotsMaybeForwarded() to allow reading slots of
  // associated objects in trace hooks that may be called during a moving GC.
  return slot < numFixedSlotsMaybeForwarded();
}

bool js::NativeObject::isNumFixedSlots(uint32_t nfixed) const {
  // We call numFixedSlotsMaybeForwarded() to allow reading slots of
  // associated objects in trace hooks that may be called during a moving GC.
  return nfixed == numFixedSlotsMaybeForwarded();
}

uint32_t js::NativeObject::outOfLineNumDynamicSlots() const {
  return numDynamicSlots();
}
#endif /* DEBUG */

mozilla::Maybe<PropertyInfo> js::NativeObject::lookup(JSContext* cx, jsid id) {
  MOZ_ASSERT(is<NativeObject>());
  uint32_t index;
  if (PropMap* map = shape()->lookup(cx, id, &index)) {
    return mozilla::Some(map->getPropertyInfo(index));
  }
  return mozilla::Nothing();
}

mozilla::Maybe<PropertyInfo> js::NativeObject::lookupPure(jsid id) {
  MOZ_ASSERT(is<NativeObject>());
  uint32_t index;
  if (PropMap* map = shape()->lookupPure(id, &index)) {
    return mozilla::Some(map->getPropertyInfo(index));
  }
  return mozilla::Nothing();
}

bool NativeObject::setUniqueId(JSRuntime* runtime, uint64_t uid) {
  MOZ_ASSERT(!hasUniqueId());
  MOZ_ASSERT(!gc::HasUniqueId(this));

  Nursery& nursery = runtime->gc.nursery();
  if (!hasDynamicSlots() && !allocateSlots(nursery, 0)) {
    return false;
  }

  getSlotsHeader()->setUniqueId(uid);
  return true;
}

bool NativeObject::setOrUpdateUniqueId(JSContext* cx, uint64_t uid) {
  if (!hasDynamicSlots() && !allocateSlots(cx->nursery(), 0)) {
    ReportOutOfMemory(cx);
    return false;
  }

  getSlotsHeader()->setUniqueId(uid);
  return true;
}

bool NativeObject::growSlots(JSContext* cx, uint32_t oldCapacity,
                             uint32_t newCapacity) {
  MOZ_ASSERT(newCapacity > oldCapacity);

  /*
   * Slot capacities are determined by the span of allocated objects. Due to
   * the limited number of bits to store shape slots, object growth is
   * throttled well before the slot capacity can overflow.
   */
  NativeObject::slotsSizeMustNotOverflow();
  MOZ_ASSERT(newCapacity <= MAX_SLOTS_COUNT);

  if (!hasDynamicSlots()) {
    if (!allocateSlots(cx->nursery(), newCapacity)) {
      ReportOutOfMemory(cx);
      return false;
    }

    return true;
  }

  uint64_t uid = maybeUniqueId();

  uint32_t newAllocated = ObjectSlots::allocCount(newCapacity);

  uint32_t dictionarySpan = getSlotsHeader()->dictionarySlotSpan();

  uint32_t oldAllocated = ObjectSlots::allocCount(oldCapacity);

  ObjectSlots* oldHeaderSlots = ObjectSlots::fromSlots(slots_);
  MOZ_ASSERT(oldHeaderSlots->capacity() == oldCapacity);

  HeapSlot* allocation = ReallocateCellBuffer<HeapSlot>(
      cx, this, reinterpret_cast<HeapSlot*>(oldHeaderSlots), oldAllocated,
      newAllocated);
  if (!allocation) {
    return false; /* Leave slots at its old size. */
  }

  auto* newHeaderSlots =
      new (allocation) ObjectSlots(newCapacity, dictionarySpan, uid);
  slots_ = newHeaderSlots->slots();

  Debug_SetSlotRangeToCrashOnTouch(slots_ + oldCapacity,
                                   newCapacity - oldCapacity);

  MOZ_ASSERT(hasDynamicSlots());
  return true;
}

bool NativeObject::growSlotsForNewSlot(JSContext* cx, uint32_t numFixed,
                                       uint32_t slot) {
  MOZ_ASSERT(slotSpan() == slot);
  MOZ_ASSERT(shape()->numFixedSlots() == numFixed);
  MOZ_ASSERT(slot >= numFixed);

  uint32_t newCapacity = calculateDynamicSlots(numFixed, slot + 1, getClass());

  uint32_t oldCapacity = numDynamicSlots();
  MOZ_ASSERT(oldCapacity < newCapacity);

  return growSlots(cx, oldCapacity, newCapacity);
}

bool NativeObject::allocateInitialSlots(JSContext* cx, uint32_t capacity) {
  uint32_t count = ObjectSlots::allocCount(capacity);
  HeapSlot* allocation = AllocateCellBuffer<HeapSlot>(cx, this, count);
  if (MOZ_UNLIKELY(!allocation)) {
    // The new object will be unreachable, but we have to make it safe for
    // finalization. It can also be observed with dumpHeap().
    // Give it a dummy shape that has no dynamic slots.
    setShape(GlobalObject::getEmptyPlainObjectShape(cx));
    initEmptyDynamicSlots();
    return false;
  }

  auto* headerSlots = new (allocation)
      ObjectSlots(capacity, 0, ObjectSlots::NoUniqueIdInDynamicSlots);
  slots_ = headerSlots->slots();

  Debug_SetSlotRangeToCrashOnTouch(slots_, capacity);

  MOZ_ASSERT(hasDynamicSlots());
  return true;
}

bool NativeObject::allocateSlots(Nursery& nursery, uint32_t newCapacity) {
  MOZ_ASSERT(!hasUniqueId());
  MOZ_ASSERT(!hasDynamicSlots());

  uint32_t newAllocated = ObjectSlots::allocCount(newCapacity);

  uint32_t dictionarySpan = getSlotsHeader()->dictionarySlotSpan();

  HeapSlot* allocation =
      AllocateCellBuffer<HeapSlot>(nursery, zone(), this, newAllocated);
  if (!allocation) {
    return false;
  }

  auto* newHeaderSlots = new (allocation) ObjectSlots(
      newCapacity, dictionarySpan, ObjectSlots::NoUniqueIdInDynamicSlots);
  slots_ = newHeaderSlots->slots();

  Debug_SetSlotRangeToCrashOnTouch(slots_, newCapacity);

  MOZ_ASSERT(hasDynamicSlots());
  return true;
}

/* static */
bool NativeObject::growSlotsPure(JSContext* cx, NativeObject* obj,
                                 uint32_t newCapacity) {
  // IC code calls this directly.
  AutoUnsafeCallWithABI unsafe;

  if (!obj->growSlots(cx, obj->numDynamicSlots(), newCapacity)) {
    cx->recoverFromOutOfMemory();
    return false;
  }

  return true;
}

/* static */
bool NativeObject::addDenseElementPure(JSContext* cx, NativeObject* obj) {
  // IC code calls this directly.
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(obj->isExtensible());
  MOZ_ASSERT(!obj->isIndexed());
  MOZ_ASSERT(!obj->is<TypedArrayObject>());
  MOZ_ASSERT_IF(obj->is<ArrayObject>(),
                obj->as<ArrayObject>().lengthIsWritable());

  // growElements will report OOM also if the number of dense elements will
  // exceed MAX_DENSE_ELEMENTS_COUNT. See goodElementsAllocationAmount.
  uint32_t oldCapacity = obj->getDenseCapacity();
  if (MOZ_UNLIKELY(!obj->growElements(cx, oldCapacity + 1))) {
    cx->recoverFromOutOfMemory();
    return false;
  }

  MOZ_ASSERT(obj->getDenseCapacity() > oldCapacity);
  MOZ_ASSERT(obj->getDenseCapacity() <= MAX_DENSE_ELEMENTS_COUNT);
  return true;
}

void NativeObject::shrinkSlots(JSContext* cx, uint32_t oldCapacity,
                               uint32_t newCapacity) {
  MOZ_ASSERT(hasDynamicSlots());
  MOZ_ASSERT(newCapacity < oldCapacity);
  MOZ_ASSERT(oldCapacity == getSlotsHeader()->capacity());

  ObjectSlots* oldHeaderSlots = ObjectSlots::fromSlots(slots_);
  MOZ_ASSERT(oldHeaderSlots->capacity() == oldCapacity);

  uint64_t uid = maybeUniqueId();

  uint32_t oldAllocated = ObjectSlots::allocCount(oldCapacity);

  if (newCapacity == 0 && uid == 0) {
    if (gc::IsBufferAlloc(oldHeaderSlots)) {
      gc::FreeBuffer(zone(), oldHeaderSlots);
    }
    // dictionarySlotSpan is initialized to the correct value by the callers.
    setEmptyDynamicSlots(0);
    return;
  }

  MOZ_ASSERT_IF(!is<ArrayObject>() && !hasUniqueId(),
                newCapacity >= SLOT_CAPACITY_MIN);

  uint32_t dictionarySpan = getSlotsHeader()->dictionarySlotSpan();

  uint32_t newAllocated = ObjectSlots::allocCount(newCapacity);

  HeapSlot* allocation = ReallocateCellBuffer<HeapSlot>(
      cx, this, reinterpret_cast<HeapSlot*>(oldHeaderSlots), oldAllocated,
      newAllocated);
  if (!allocation) {
    // It's possible for realloc to fail when shrinking an allocation. In this
    // case we continue using the original allocation but still update the
    // capacity to the new requested capacity, which is smaller than the actual
    // capacity.
    cx->recoverFromOutOfMemory();
    allocation = reinterpret_cast<HeapSlot*>(getSlotsHeader());
  }

  auto* newHeaderSlots =
      new (allocation) ObjectSlots(newCapacity, dictionarySpan, uid);
  slots_ = newHeaderSlots->slots();
}

void NativeObject::initFixedElements(gc::AllocKind kind, uint32_t length) {
  uint32_t capacity =
      gc::GetGCKindSlots(kind) - ObjectElements::VALUES_PER_HEADER;

  setFixedElements();
  new (getElementsHeader()) ObjectElements(capacity, length);
  getElementsHeader()->flags |= ObjectElements::FIXED;

  MOZ_ASSERT(hasFixedElements());
}

bool NativeObject::willBeSparseElements(uint32_t requiredCapacity,
                                        uint32_t newElementsHint) {
  MOZ_ASSERT(is<NativeObject>());
  MOZ_ASSERT(requiredCapacity > MIN_SPARSE_INDEX);

  uint32_t cap = getDenseCapacity();
  MOZ_ASSERT(requiredCapacity >= cap);

  if (requiredCapacity > MAX_DENSE_ELEMENTS_COUNT) {
    return true;
  }

  uint32_t minimalDenseCount = requiredCapacity / SPARSE_DENSITY_RATIO;
  if (newElementsHint >= minimalDenseCount) {
    return false;
  }
  minimalDenseCount -= newElementsHint;

  if (minimalDenseCount > cap) {
    return true;
  }

  uint32_t initLen = getDenseInitializedLength();
  if (denseElementsArePacked()) {
    return minimalDenseCount > initLen;
  }

  const Value* elems = getDenseElements();
  for (uint32_t i = 0; i < initLen; i++) {
    if (!elems[i].isMagic(JS_ELEMENTS_HOLE) && !--minimalDenseCount) {
      return false;
    }
  }
  return true;
}

/* static */
DenseElementResult NativeObject::maybeDensifySparseElements(
    JSContext* cx, Handle<NativeObject*> obj) {
  /*
   * Wait until after the object goes into dictionary mode, which must happen
   * when sparsely packing any array with more than MIN_SPARSE_INDEX elements
   * (see PropertyTree::MAX_HEIGHT).
   */
  if (!obj->inDictionaryMode()) {
    return DenseElementResult::Incomplete;
  }

  /*
   * Only measure the number of indexed properties every log(n) times when
   * populating the object.
   */
  uint32_t slotSpan = obj->slotSpan();
  if (slotSpan != RoundUpPow2(slotSpan)) {
    return DenseElementResult::Incomplete;
  }

  /* Watch for conditions under which an object's elements cannot be dense. */
  if (!obj->isExtensible()) {
    return DenseElementResult::Incomplete;
  }

  /*
   * The indexes in the object need to be sufficiently dense before they can
   * be converted to dense mode.
   */
  uint32_t numDenseElements = 0;
  uint32_t newInitializedLength = 0;

  for (ShapePropertyIter<NoGC> iter(obj->shape()); !iter.done(); iter++) {
    uint32_t index;
    if (!IdIsIndex(iter->key(), &index)) {
      continue;
    }
    if (iter->flags() != PropertyFlags::defaultDataPropFlags) {
      // For simplicity, only densify the object if all indexed properties can
      // be converted to dense elements.
      return DenseElementResult::Incomplete;
    }
    MOZ_ASSERT(iter->isDataProperty());
    numDenseElements++;
    newInitializedLength = std::max(newInitializedLength, index + 1);
  }

  if (numDenseElements * SPARSE_DENSITY_RATIO < newInitializedLength) {
    return DenseElementResult::Incomplete;
  }

  if (newInitializedLength > MAX_DENSE_ELEMENTS_COUNT) {
    return DenseElementResult::Incomplete;
  }

  /*
   * This object meets all necessary restrictions, convert all indexed
   * properties into dense elements.
   */

  if (newInitializedLength > obj->getDenseCapacity()) {
    if (!obj->growElements(cx, newInitializedLength)) {
      return DenseElementResult::Failure;
    }
  }

  obj->ensureDenseInitializedLength(newInitializedLength, 0);

  if (obj->compartment()->objectMaybeInIteration(obj)) {
    // Mark the densified elements as maybe-in-iteration. See also the comment
    // in GetIterator.
    obj->markDenseElementsMaybeInIteration();
  }

  if (!NativeObject::densifySparseElements(cx, obj)) {
    return DenseElementResult::Failure;
  }

  return DenseElementResult::Success;
}

void NativeObject::moveShiftedElements() {
  MOZ_ASSERT(isExtensible());

  ObjectElements* header = getElementsHeader();
  uint32_t numShifted = header->numShiftedElements();
  MOZ_ASSERT(numShifted > 0);

  uint32_t initLength = header->initializedLength;

  ObjectElements* newHeader =
      static_cast<ObjectElements*>(getUnshiftedElementsHeader());
  memmove(newHeader, header, sizeof(ObjectElements));

  newHeader->clearShiftedElements();
  newHeader->capacity += numShifted;
  elements_ = newHeader->elements();

  // To move the elements, temporarily update initializedLength to include
  // the shifted elements.
  newHeader->initializedLength += numShifted;

  // Move the elements. Initialize to |undefined| to ensure pre-barriers
  // don't see garbage.
  for (size_t i = 0; i < numShifted; i++) {
    initDenseElement(i, UndefinedValue());
  }
  moveDenseElements(0, numShifted, initLength);

  // Restore the initialized length. We use setDenseInitializedLength to
  // make sure prepareElementRangeForOverwrite is called on the shifted
  // elements.
  setDenseInitializedLength(initLength);
}

void NativeObject::maybeMoveShiftedElements() {
  MOZ_ASSERT(isExtensible());

  ObjectElements* header = getElementsHeader();
  MOZ_ASSERT(header->numShiftedElements() > 0);

  // Move the elements if less than a third of the allocated space is in use.
  if (header->capacity < header->numAllocatedElements() / 3) {
    moveShiftedElements();
  }
}

bool NativeObject::tryUnshiftDenseElements(uint32_t count) {
  MOZ_ASSERT(isExtensible());
  MOZ_ASSERT(count > 0);

  ObjectElements* header = getElementsHeader();
  uint32_t numShifted = header->numShiftedElements();

  if (count > numShifted) {
    // We need more elements than are easily available. Try to make space
    // for more elements than we need (and shift the remaining ones) so
    // that unshifting more elements later will be fast.

    // Don't bother reserving elements if the number of elements is small.
    // Note that there's no technical reason for using this particular
    // limit.
    if (header->initializedLength <= 10 ||
        header->hasNonwritableArrayLength() ||
        MOZ_UNLIKELY(count > ObjectElements::MaxShiftedElements)) {
      return false;
    }

    MOZ_ASSERT(header->capacity >= header->initializedLength);
    uint32_t unusedCapacity = header->capacity - header->initializedLength;

    // Determine toShift, the number of extra elements we want to make
    // available.
    uint32_t toShift = count - numShifted;
    MOZ_ASSERT(toShift <= ObjectElements::MaxShiftedElements,
               "count <= MaxShiftedElements so toShift <= MaxShiftedElements");

    // Give up if we need to allocate more elements.
    if (toShift > unusedCapacity) {
      return false;
    }

    // Move more elements than we need, so that other unshift calls will be
    // fast. We just have to make sure we don't exceed unusedCapacity.
    toShift = std::min(toShift + unusedCapacity / 2, unusedCapacity);

    // Ensure |numShifted + toShift| does not exceed MaxShiftedElements.
    if (numShifted + toShift > ObjectElements::MaxShiftedElements) {
      toShift = ObjectElements::MaxShiftedElements - numShifted;
    }

    MOZ_ASSERT(count <= numShifted + toShift);
    MOZ_ASSERT(numShifted + toShift <= ObjectElements::MaxShiftedElements);
    MOZ_ASSERT(toShift <= unusedCapacity);

    // Now move/unshift the elements.
    uint32_t initLen = header->initializedLength;
    setDenseInitializedLength(initLen + toShift);
    for (uint32_t i = 0; i < toShift; i++) {
      initDenseElement(initLen + i, UndefinedValue());
    }
    moveDenseElements(toShift, 0, initLen);

    // Shift the elements we just prepended.
    shiftDenseElementsUnchecked(toShift);

    // We can now fall-through to the fast path below.
    header = getElementsHeader();
    MOZ_ASSERT(header->numShiftedElements() == numShifted + toShift);

    numShifted = header->numShiftedElements();
    MOZ_ASSERT(count <= numShifted);
  }

  elements_ -= count;
  ObjectElements* newHeader = getElementsHeader();
  memmove(newHeader, header, sizeof(ObjectElements));

  newHeader->unshiftShiftedElements(count);

  // Initialize to |undefined| to ensure pre-barriers don't see garbage.
  for (uint32_t i = 0; i < count; i++) {
    initDenseElement(i, UndefinedValue());
  }

  return true;
}

// Given a requested capacity (in elements) and (potentially) the length of an
// array for which elements are being allocated, compute an actual allocation
// amount (in elements).  (Allocation amounts include space for an
// ObjectElements instance, so a return value of |N| implies
// |N - ObjectElements::VALUES_PER_HEADER| usable elements.)
//
// The requested/actual allocation distinction is meant to:
//
//   * preserve amortized O(N) time to add N elements;
//   * minimize the number of unused elements beyond an array's length, and
//   * provide at least ELEMENT_CAPACITY_MIN elements no matter what (so adding
//     the first several elements to small arrays only needs one allocation).
/* static */
bool NativeObject::goodElementsAllocationAmount(JSContext* cx,
                                                uint32_t reqCapacity,
                                                uint32_t length,
                                                uint32_t* goodAmount) {
  if (reqCapacity > MAX_DENSE_ELEMENTS_COUNT) {
    ReportOutOfMemory(cx);
    return false;
  }

  uint32_t reqAllocated = reqCapacity + ObjectElements::VALUES_PER_HEADER;

  // Handle "small" requests primarily by doubling.
  const uint32_t Mebi = 1 << 20;
  if (reqAllocated < Mebi) {
    uint32_t amount =
        gc::GetGoodPower2ElementCount(reqAllocated, sizeof(Value));

    // If |amount| would be 2/3 or more of the array's length, adjust
    // it (up or down) to be equal to the array's length.  This avoids
    // allocating excess elements that aren't likely to be needed, either
    // in this resizing or a subsequent one.  The 2/3 factor is chosen so
    // that exceptional resizings will at most triple the capacity, as
    // opposed to the usual doubling.
    uint32_t goodCapacity = amount - ObjectElements::VALUES_PER_HEADER;
    if (length >= reqCapacity && goodCapacity > (length / 3) * 2) {
      amount = gc::GetGoodElementCount(
          length + ObjectElements::VALUES_PER_HEADER, sizeof(Value));
    }

    const size_t AmountMin =
        ELEMENT_CAPACITY_MIN + ObjectElements::VALUES_PER_HEADER;

    // Check this size doesn't waste any space in the allocation.
    MOZ_ASSERT(AmountMin == gc::GetGoodElementCount(AmountMin, sizeof(Value)));

    if (amount < AmountMin) {
      amount = AmountMin;
    }

    *goodAmount = amount;

    return true;
  }

  // The almost-doubling above wastes a lot of space for larger bucket sizes.
  // For large amounts, switch to bucket sizes that obey this formula:
  //
  //   count(n+1) = Math.ceil(count(n) * 1.125)
  //
  // where |count(n)| is the size of the nth bucket, measured in 2**20 slots.
  // These bucket sizes still preserve amortized O(N) time to add N elements,
  // just with a larger constant factor.
  //
  // The bucket size table below was generated with this JavaScript (and
  // manual reformatting):
  //
  //   for (let n = 1, i = 0; i < 34; i++) {
  //     print('0x' + (n * (1 << 20)).toString(16) + ', ');
  //     n = Math.ceil(n * 1.125);
  //   }
  static constexpr uint32_t BigBuckets[] = {
      0x100000,  0x200000,  0x300000,  0x400000,  0x500000,  0x600000,
      0x700000,  0x800000,  0x900000,  0xb00000,  0xd00000,  0xf00000,
      0x1100000, 0x1400000, 0x1700000, 0x1a00000, 0x1e00000, 0x2200000,
      0x2700000, 0x2c00000, 0x3200000, 0x3900000, 0x4100000, 0x4a00000,
      0x5400000, 0x5f00000, 0x6b00000, 0x7900000, 0x8900000, 0x9b00000,
      0xaf00000, 0xc500000, 0xde00000, 0xfa00000};
  static_assert(BigBuckets[std::size(BigBuckets) - 1] <=
                MAX_DENSE_ELEMENTS_ALLOCATION);

  // We will allocate these in large buffers so account for the header size
  // required there.
  static_assert(sizeof(Value) * Mebi >= gc::ChunkSize);

  // Pick the first bucket that'll fit |reqAllocated|.
  for (uint32_t b : BigBuckets) {
    if (b >= reqAllocated) {
      MOZ_ASSERT(b == gc::GetGoodElementCount(b, sizeof(Value)));
      *goodAmount = b;
      return true;
    }
  }

  // Otherwise, return the maximum bucket size.
  *goodAmount = MAX_DENSE_ELEMENTS_ALLOCATION;
  return true;
}

bool NativeObject::growElements(JSContext* cx, uint32_t reqCapacity) {
  MOZ_ASSERT(isExtensible());
  MOZ_ASSERT(canHaveNonEmptyElements());

  // If there are shifted elements, consider moving them first. If we don't
  // move them here, the code below will include the shifted elements in the
  // resize.
  uint32_t numShifted = getElementsHeader()->numShiftedElements();
  if (numShifted > 0) {
    // If the number of elements is small, it's cheaper to just move them as
    // it may avoid a malloc/realloc. Note that there's no technical reason
    // for using this particular value, but it works well in real-world use
    // cases.
    static const size_t MaxElementsToMoveEagerly = 20;

    if (getElementsHeader()->initializedLength <= MaxElementsToMoveEagerly) {
      moveShiftedElements();
    } else {
      maybeMoveShiftedElements();
    }
    if (getDenseCapacity() >= reqCapacity) {
      return true;
    }
    // moveShiftedElements() may have changed the number of shifted elements;
    // update `numShifted` accordingly.
    numShifted = getElementsHeader()->numShiftedElements();

    // If |reqCapacity + numShifted| overflows, we just move all shifted
    // elements to avoid the problem.
    CheckedInt<uint32_t> checkedReqCapacity(reqCapacity);
    checkedReqCapacity += numShifted;
    if (MOZ_UNLIKELY(!checkedReqCapacity.isValid())) {
      moveShiftedElements();
      numShifted = 0;
    }
  }

  uint32_t oldCapacity = getDenseCapacity();
  MOZ_ASSERT(oldCapacity < reqCapacity);

  uint32_t newAllocated = 0;
  if (is<ArrayObject>() && !as<ArrayObject>().lengthIsWritable()) {
    // Preserve the |capacity <= length| invariant for arrays with
    // non-writable length.  See also js::ArraySetLength which initially
    // enforces this requirement.
    MOZ_ASSERT(reqCapacity <= as<ArrayObject>().length());
    // Adding to reqCapacity must not overflow uint32_t.
    MOZ_ASSERT(reqCapacity <= MAX_DENSE_ELEMENTS_COUNT);

    // Then, add the header and shifted elements sizes to the new capacity
    // to get the overall amount to allocate.
    newAllocated = reqCapacity + numShifted + ObjectElements::VALUES_PER_HEADER;
  } else {
    // For arrays with writable length, and all non-Array objects, call
    // `NativeObject::goodElementsAllocationAmount()` to determine the
    // amount to allocate from the the requested capacity and existing length.
    uint32_t length = is<ArrayObject>() ? as<ArrayObject>().length() : 0;
    if (!goodElementsAllocationAmount(cx, reqCapacity + numShifted, length,
                                      &newAllocated)) {
      return false;
    }
  }

  // newAllocated now contains the size of the buffer we need to allocate;
  // subtract off the header and shifted elements size to get the new capacity
  uint32_t newCapacity =
      newAllocated - ObjectElements::VALUES_PER_HEADER - numShifted;
  // If the new capacity isn't strictly greater than the old capacity, then this
  // method shouldn't have been called; if the new capacity doesn't satisfy
  // what was requested, then one of the calculations above must have been
  // wrong.
  MOZ_ASSERT(newCapacity > oldCapacity && newCapacity >= reqCapacity);

  // If newCapacity exceeds MAX_DENSE_ELEMENTS_COUNT, the array should become
  // sparse.
  MOZ_ASSERT(newCapacity <= MAX_DENSE_ELEMENTS_COUNT);

  uint32_t initlen = getDenseInitializedLength();

  HeapSlot* oldHeaderSlots =
      reinterpret_cast<HeapSlot*>(getUnshiftedElementsHeader());
  HeapSlot* newHeaderSlots;
  uint32_t oldAllocated = 0;
  if (hasDynamicElements()) {
    // If the object has dynamic elements, then we might be able to resize the
    // buffer in-place.

    // First, check that adding to oldCapacity won't overflow uint32_t
    MOZ_ASSERT(oldCapacity <= MAX_DENSE_ELEMENTS_COUNT);
    // Then, add the header and shifted elements sizes to get the overall size
    // of the existing buffer
    oldAllocated = oldCapacity + ObjectElements::VALUES_PER_HEADER + numShifted;

    // Finally, try to resize the buffer.
    newHeaderSlots = ReallocateCellBuffer<HeapSlot>(cx, this, oldHeaderSlots,
                                                    oldAllocated, newAllocated);
    if (!newHeaderSlots) {
      return false;  // If the resizing failed, then we leave elements at its
                     // old size.
    }
  } else {
    // If the object has fixed elements, then we always need to allocate a new
    // buffer, because if we've reached this code, then the requested capacity
    // is greater than the existing inline space available within the object
    newHeaderSlots = AllocateCellBuffer<HeapSlot>(cx, this, newAllocated);
    if (!newHeaderSlots) {
      return false;  // Leave elements at its old size.
    }

    // Copy the initialized elements into the new buffer,
    PodCopy(newHeaderSlots, oldHeaderSlots,
            ObjectElements::VALUES_PER_HEADER + initlen + numShifted);
  }

  ObjectElements* newheader = reinterpret_cast<ObjectElements*>(newHeaderSlots);
  // Update the elements pointer to point to the new elements buffer.
  elements_ = newheader->elements() + numShifted;

  // Clear the "fixed elements" flag, because if this code has been reached,
  // this object now has dynamic elements.
  getElementsHeader()->flags &= ~ObjectElements::FIXED;
  getElementsHeader()->capacity = newCapacity;

  // Poison the uninitialized portion of the new elements buffer.
  Debug_SetSlotRangeToCrashOnTouch(elements_ + initlen, newCapacity - initlen);

  return true;
}

void NativeObject::shrinkElements(JSContext* cx, uint32_t reqCapacity) {
  MOZ_ASSERT(canHaveNonEmptyElements());
  MOZ_ASSERT(reqCapacity >= getDenseInitializedLength());

  if (!hasDynamicElements()) {
    return;
  }

  // If we have shifted elements, consider moving them.
  uint32_t numShifted = getElementsHeader()->numShiftedElements();
  if (numShifted > 0) {
    maybeMoveShiftedElements();
    numShifted = getElementsHeader()->numShiftedElements();
  }

  uint32_t oldCapacity = getDenseCapacity();
  MOZ_ASSERT(reqCapacity < oldCapacity);

  uint32_t newAllocated = 0;
  MOZ_ALWAYS_TRUE(goodElementsAllocationAmount(cx, reqCapacity + numShifted, 0,
                                               &newAllocated));
  MOZ_ASSERT(oldCapacity <= MAX_DENSE_ELEMENTS_COUNT);

  uint32_t oldAllocated =
      oldCapacity + ObjectElements::VALUES_PER_HEADER + numShifted;
  if (newAllocated == oldAllocated) {
    return;  // Leave elements at its old size.
  }

  MOZ_ASSERT(newAllocated > ObjectElements::VALUES_PER_HEADER);
  uint32_t newCapacity =
      newAllocated - ObjectElements::VALUES_PER_HEADER - numShifted;
  MOZ_ASSERT(newCapacity <= MAX_DENSE_ELEMENTS_COUNT);

  HeapSlot* oldHeaderSlots =
      reinterpret_cast<HeapSlot*>(getUnshiftedElementsHeader());
  HeapSlot* newHeaderSlots = ReallocateCellBuffer<HeapSlot>(
      cx, this, oldHeaderSlots, oldAllocated, newAllocated);
  if (!newHeaderSlots) {
    cx->recoverFromOutOfMemory();
    return;  // Leave elements at its old size.
  }

  ObjectElements* newheader = reinterpret_cast<ObjectElements*>(newHeaderSlots);
  elements_ = newheader->elements() + numShifted;
  getElementsHeader()->capacity = newCapacity;
}

void NativeObject::shrinkCapacityToInitializedLength(JSContext* cx) {
  // When an array's length becomes non-writable, writes to indexes greater
  // greater than or equal to the length don't change the array.  We handle this
  // with a check for non-writable length in most places. But in JIT code every
  // check counts -- so we piggyback the check on the already-required range
  // check for |index < capacity| by making capacity of arrays with non-writable
  // length never exceed the length. This mechanism is also used when an object
  // becomes non-extensible.

  if (getElementsHeader()->numShiftedElements() > 0) {
    moveShiftedElements();
  }

  ObjectElements* header = getElementsHeader();
  uint32_t len = header->initializedLength;
  MOZ_ASSERT(header->capacity >= len);
  if (header->capacity == len) {
    return;
  }

  shrinkElements(cx, len);

  getElementsHeader()->capacity = len;
}

/* static */
bool NativeObject::allocDictionarySlot(JSContext* cx, Handle<NativeObject*> obj,
                                       uint32_t* slotp) {
  MOZ_ASSERT(obj->inDictionaryMode());

  uint32_t slotSpan = obj->slotSpan();
  MOZ_ASSERT(slotSpan >= JSSLOT_FREE(obj->getClass()));

  // Try to pull a free slot from the slot-number free list.
  DictionaryPropMap* map = obj->dictionaryShape()->propMap();
  uint32_t last = map->freeList();
  if (last != SHAPE_INVALID_SLOT) {
#ifdef DEBUG
    MOZ_ASSERT(last < slotSpan);
    uint32_t next = obj->getSlot(last).toPrivateUint32();
    MOZ_ASSERT_IF(next != SHAPE_INVALID_SLOT, next < slotSpan);
#endif
    *slotp = last;
    const Value& vref = obj->getSlot(last);
    map->setFreeList(vref.toPrivateUint32());
    obj->setSlot(last, UndefinedValue());
    return true;
  }

  if (MOZ_UNLIKELY(slotSpan >= SHAPE_MAXIMUM_SLOT)) {
    ReportOutOfMemory(cx);
    return false;
  }

  *slotp = slotSpan;

  uint32_t numFixed = obj->numFixedSlots();
  if (slotSpan < numFixed) {
    obj->initFixedSlot(slotSpan, UndefinedValue());
    obj->setDictionaryModeSlotSpan(slotSpan + 1);
    return true;
  }

  uint32_t dynamicSlotIndex = slotSpan - numFixed;
  if (dynamicSlotIndex >= obj->numDynamicSlots()) {
    if (MOZ_UNLIKELY(!obj->growSlotsForNewSlot(cx, numFixed, slotSpan))) {
      return false;
    }
  }
  obj->initDynamicSlot(numFixed, slotSpan, UndefinedValue());
  obj->setDictionaryModeSlotSpan(slotSpan + 1);
  return true;
}

void NativeObject::freeDictionarySlot(uint32_t slot) {
  MOZ_ASSERT(inDictionaryMode());
  MOZ_ASSERT(slot < slotSpan());

  DictionaryPropMap* map = dictionaryShape()->propMap();
  uint32_t last = map->freeList();

  // Can't afford to check the whole free list, but let's check the head.
  MOZ_ASSERT_IF(last != SHAPE_INVALID_SLOT, last < slotSpan() && last != slot);

  // Place all freed slots other than reserved slots (bug 595230) on the
  // dictionary's free list.
  if (JSSLOT_FREE(getClass()) <= slot) {
    MOZ_ASSERT_IF(last != SHAPE_INVALID_SLOT, last < slotSpan());
    setSlot(slot, PrivateUint32Value(last));
    map->setFreeList(slot);
  } else {
    setSlot(slot, UndefinedValue());
  }
}

template <AllowGC allowGC>
bool js::NativeLookupOwnProperty(
    JSContext* cx, typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
    typename MaybeRooted<jsid, allowGC>::HandleType id, PropertyResult* propp) {
  return NativeLookupOwnPropertyInline<allowGC>(cx, obj, id, propp);
}

template bool js::NativeLookupOwnProperty<CanGC>(JSContext* cx,
                                                 Handle<NativeObject*> obj,
                                                 HandleId id,
                                                 PropertyResult* propp);

template bool js::NativeLookupOwnProperty<NoGC>(JSContext* cx,
                                                NativeObject* const& obj,
                                                const jsid& id,
                                                PropertyResult* propp);

/*** [[DefineOwnProperty]] **************************************************/

static bool CallJSAddPropertyOp(JSContext* cx, JSAddPropertyOp op,
                                HandleObject obj, HandleId id, HandleValue v) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  cx->check(obj, id, v);
  return op(cx, obj, id, v);
}

static MOZ_ALWAYS_INLINE bool CallAddPropertyHook(JSContext* cx,
                                                  Handle<NativeObject*> obj,
                                                  HandleId id,
                                                  HandleValue value) {
  JSAddPropertyOp addProperty = obj->getClass()->getAddProperty();
  if (MOZ_UNLIKELY(addProperty)) {
    if (!CallJSAddPropertyOp(cx, addProperty, obj, id, value)) {
      NativeObject::removeProperty(cx, obj, id);
      return false;
    }
  }
  return true;
}

static MOZ_ALWAYS_INLINE bool CallAddPropertyHookDense(
    JSContext* cx, Handle<NativeObject*> obj, uint32_t index,
    HandleValue value) {
  // Inline addProperty for array objects.
  if (obj->is<ArrayObject>()) {
    ArrayObject* arr = &obj->as<ArrayObject>();
    uint32_t length = arr->length();
    if (index >= length) {
      arr->setLength(cx, index + 1);
    }
    return true;
  }

  JSAddPropertyOp addProperty = obj->getClass()->getAddProperty();
  if (MOZ_UNLIKELY(addProperty)) {
    RootedId id(cx, PropertyKey::Int(index));
    if (!CallJSAddPropertyOp(cx, addProperty, obj, id, value)) {
      obj->setDenseElementHole(index);
      return false;
    }
  }
  return true;
}

/**
 * Determines whether a write to the given element on |arr| should fail
 * because |arr| has a non-writable length, and writing that element would
 * increase the length of the array.
 */
static bool WouldDefinePastNonwritableLength(ArrayObject* arr, uint32_t index) {
  return !arr->lengthIsWritable() && index >= arr->length();
}

static bool ChangeProperty(JSContext* cx, Handle<NativeObject*> obj,
                           HandleId id, HandleObject getter,
                           HandleObject setter, PropertyFlags flags,
                           PropertyResult* existing, uint32_t* slotOut) {
  MOZ_ASSERT(existing);

  Rooted<GetterSetter*> gs(cx);

  // If we're redefining a getter/setter property but the getter and setter
  // objects are still the same, use the existing GetterSetter.
  if (existing->isNativeProperty()) {
    PropertyInfo prop = existing->propertyInfo();
    if (prop.isAccessorProperty()) {
      GetterSetter* current = obj->getGetterSetter(prop);
      if (current->getter() == getter && current->setter() == setter) {
        gs = current;
      }
    }
  }

  if (!gs) {
    gs = GetterSetter::create(cx, getter, setter);
    if (!gs) {
      return false;
    }
  }

  if (existing->isNativeProperty()) {
    Rooted<Value> value(cx, PrivateGCThingValue(gs));
    if (!NativeObject::changeProperty(cx, obj, id, flags, slotOut)) {
      return false;
    }
    Watchtower::watchPropertyValueChange<AllowGC::CanGC>(
        cx, obj, id, value, existing->propertyInfo());
    obj->setSlot(*slotOut, value);
    return true;
  }

  if (!NativeObject::addProperty(cx, obj, id, flags, slotOut)) {
    return false;
  }
  obj->initSlot(*slotOut, PrivateGCThingValue(gs));
  return true;
}

static PropertyFlags ComputePropertyFlags(const PropertyDescriptor& desc) {
  desc.assertComplete();

  PropertyFlags flags;
  flags.setFlag(PropertyFlag::Configurable, desc.configurable());
  flags.setFlag(PropertyFlag::Enumerable, desc.enumerable());

  if (desc.isDataDescriptor()) {
    flags.setFlag(PropertyFlag::Writable, desc.writable());
  } else {
    MOZ_ASSERT(desc.isAccessorDescriptor());
    flags.setFlag(PropertyFlag::AccessorProperty);
  }

  return flags;
}

// Whether we're adding a new property or changing an existing property (this
// can be either a property stored in the shape tree or a dense element).
enum class IsAddOrChange { Add, Change };

template <IsAddOrChange AddOrChange>
static MOZ_ALWAYS_INLINE bool AddOrChangeProperty(
    JSContext* cx, Handle<NativeObject*> obj, HandleId id,
    Handle<PropertyDescriptor> desc, PropertyResult* existing = nullptr) {
  desc.assertComplete();

#ifdef DEBUG
  if constexpr (AddOrChange == IsAddOrChange::Add) {
    MOZ_ASSERT(existing == nullptr);
    MOZ_ASSERT(!obj->containsPure(id));
  } else {
    static_assert(AddOrChange == IsAddOrChange::Change);
    MOZ_ASSERT(existing);
    MOZ_ASSERT(existing->isNativeProperty() || existing->isDenseElement());
  }
#endif

  // Use dense storage for indexed properties where possible: when we have an
  // integer key with default property attributes and are either adding a new
  // property or changing a dense element.
  PropertyFlags flags = ComputePropertyFlags(desc);
  if (id.isInt() && flags == PropertyFlags::defaultDataPropFlags &&
      (AddOrChange == IsAddOrChange::Add || existing->isDenseElement())) {
    MOZ_ASSERT(!desc.isAccessorDescriptor());
    MOZ_ASSERT(!obj->is<TypedArrayObject>());
    uint32_t index = id.toInt();
    DenseElementResult edResult = obj->ensureDenseElements(cx, index, 1);
    if (edResult == DenseElementResult::Failure) {
      return false;
    }
    if (edResult == DenseElementResult::Success) {
      obj->setDenseElement(index, desc.value());
      if (!CallAddPropertyHookDense(cx, obj, index, desc.value())) {
        return false;
      }
      return true;
    }
  }

  uint32_t slot;
  if constexpr (AddOrChange == IsAddOrChange::Add) {
    if (desc.isAccessorDescriptor()) {
      Rooted<GetterSetter*> gs(
          cx, GetterSetter::create(cx, desc.getter(), desc.setter()));
      if (!gs) {
        return false;
      }
      if (!NativeObject::addProperty(cx, obj, id, flags, &slot)) {
        return false;
      }
      obj->initSlot(slot, PrivateGCThingValue(gs));
    } else {
      if (!NativeObject::addProperty(cx, obj, id, flags, &slot)) {
        return false;
      }
      obj->initSlot(slot, desc.value());
    }
  } else {
    if (desc.isAccessorDescriptor()) {
      if (!ChangeProperty(cx, obj, id, desc.getter(), desc.setter(), flags,
                          existing, &slot)) {
        return false;
      }
    } else {
      if (existing->isNativeProperty()) {
        if (!NativeObject::changeProperty(cx, obj, id, flags, &slot)) {
          return false;
        }
        Watchtower::watchPropertyValueChange<AllowGC::CanGC>(
            cx, obj, id, desc.value(), existing->propertyInfo());
        obj->setSlot(slot, desc.value());
      } else {
        if (!NativeObject::addProperty(cx, obj, id, flags, &slot)) {
          return false;
        }
        obj->initSlot(slot, desc.value());
      }
    }
  }

  MOZ_ASSERT(slot < obj->slotSpan());

  // Clear any existing dense index after adding a sparse indexed property,
  // and investigate converting the object to dense indexes.
  if (id.isInt()) {
    uint32_t index = id.toInt();
    if constexpr (AddOrChange == IsAddOrChange::Add) {
      MOZ_ASSERT(!obj->containsDenseElement(index));
    } else {
      obj->removeDenseElementForSparseIndex(index);
    }
    // Only try to densify sparse elements if the property we just added/changed
    // is in the last slot. This avoids a perf cliff in pathological cases: in
    // maybeDensifySparseElements we densify if the slot span is a power-of-two,
    // but if we get slots from the free list, the slot span will stay the same
    // until the free list is empty. This means we'd get quadratic behavior by
    // trying to densify for each sparse element we add. See bug 1782487.
    if (slot == obj->slotSpan() - 1) {
      DenseElementResult edResult =
          NativeObject::maybeDensifySparseElements(cx, obj);
      if (edResult == DenseElementResult::Failure) {
        return false;
      }
      if (edResult == DenseElementResult::Success) {
        MOZ_ASSERT(!desc.isAccessorDescriptor());
        return CallAddPropertyHookDense(cx, obj, index, desc.value());
      }
    }
  }

  if (desc.isDataDescriptor()) {
    return CallAddPropertyHook(cx, obj, id, desc.value());
  }

  return CallAddPropertyHook(cx, obj, id, UndefinedHandleValue);
}

// Versions of AddOrChangeProperty optimized for adding a plain data property.
// This function doesn't handle integer ids as we may have to store them in
// dense elements.
static MOZ_ALWAYS_INLINE bool AddDataProperty(JSContext* cx,
                                              Handle<NativeObject*> obj,
                                              HandleId id, HandleValue v) {
  MOZ_ASSERT(!id.isInt());

  uint32_t slot;
  if (!NativeObject::addProperty(cx, obj, id,
                                 PropertyFlags::defaultDataPropFlags, &slot)) {
    return false;
  }

  obj->initSlot(slot, v);

  return CallAddPropertyHook(cx, obj, id, v);
}

bool js::AddSlotAndCallAddPropHook(JSContext* cx, Handle<NativeObject*> obj,
                                   HandleValue v, Handle<Shape*> newShape) {
  MOZ_ASSERT(obj->getClass()->getAddProperty());
  MOZ_ASSERT(newShape->asShared().lastProperty().isDataProperty());

  RootedId id(cx, newShape->asShared().lastProperty().key());
  MOZ_ASSERT(!id.isInt());

  uint32_t slot = newShape->asShared().lastProperty().slot();
  if (!obj->setShapeAndAddNewSlot(cx, &newShape->asShared(), slot)) {
    return false;
  }
  obj->initSlot(slot, v);

  return CallAddPropertyHook(cx, obj, id, v);
}

static bool IsAccessorDescriptor(const PropertyResult& prop) {
  if (prop.isNativeProperty()) {
    return prop.propertyInfo().isAccessorProperty();
  }

  MOZ_ASSERT(prop.isDenseElement() || prop.isTypedArrayElement());
  return false;
}

static bool IsDataDescriptor(const PropertyResult& prop) {
  return !IsAccessorDescriptor(prop);
}

static bool GetCustomDataProperty(JSContext* cx, HandleObject obj, HandleId id,
                                  MutableHandleValue vp);

static bool GetExistingDataProperty(JSContext* cx, Handle<NativeObject*> obj,
                                    HandleId id, const PropertyResult& prop,
                                    MutableHandleValue vp) {
  if (prop.isDenseElement()) {
    vp.set(obj->getDenseElement(prop.denseElementIndex()));
    return true;
  }
  if (prop.isTypedArrayElement()) {
    size_t idx = prop.typedArrayElementIndex();
    return obj->as<TypedArrayObject>().getElement<CanGC>(cx, idx, vp);
  }

  PropertyInfo propInfo = prop.propertyInfo();
  if (propInfo.isDataProperty()) {
    vp.set(obj->getSlot(propInfo.slot()));
    return true;
  }

  MOZ_RELEASE_ASSERT(propInfo.isCustomDataProperty());
  return GetCustomDataProperty(cx, obj, id, vp);
}

/*
 * If desc is redundant with an existing own property obj[id], then set
 * |*redundant = true| and return true.
 */
static bool DefinePropertyIsRedundant(JSContext* cx, Handle<NativeObject*> obj,
                                      HandleId id, const PropertyResult& prop,
                                      JS::PropertyAttributes attrs,
                                      Handle<PropertyDescriptor> desc,
                                      bool* redundant) {
  *redundant = false;

  if (desc.hasConfigurable() && desc.configurable() != attrs.configurable()) {
    return true;
  }
  if (desc.hasEnumerable() && desc.enumerable() != attrs.enumerable()) {
    return true;
  }
  if (desc.isDataDescriptor()) {
    if (IsAccessorDescriptor(prop)) {
      return true;
    }
    if (desc.hasWritable() && desc.writable() != attrs.writable()) {
      return true;
    }
    if (desc.hasValue()) {
      // Get the current value of the existing property.
      RootedValue currentValue(cx);
      if (!GetExistingDataProperty(cx, obj, id, prop, &currentValue)) {
        return false;
      }

      // Don't call SameValue here to ensure we properly update distinct
      // NaN values.
      if (desc.value() != currentValue) {
        return true;
      }
    }

    // Check for custom data properties for ArrayObject/ArgumentsObject.
    // PropertyDescriptor can't represent these properties so they're never
    // redundant.
    if (prop.isNativeProperty() && prop.propertyInfo().isCustomDataProperty()) {
      return true;
    }
  } else if (desc.isAccessorDescriptor()) {
    if (!prop.isNativeProperty()) {
      return true;
    }
    PropertyInfo propInfo = prop.propertyInfo();
    if (desc.hasGetter() && (!propInfo.isAccessorProperty() ||
                             desc.getter() != obj->getGetter(propInfo))) {
      return true;
    }
    if (desc.hasSetter() && (!propInfo.isAccessorProperty() ||
                             desc.setter() != obj->getSetter(propInfo))) {
      return true;
    }
  }

  *redundant = true;
  return true;
}

bool js::NativeDefineProperty(JSContext* cx, Handle<NativeObject*> obj,
                              HandleId id, Handle<PropertyDescriptor> desc_,
                              ObjectOpResult& result) {
  desc_.assertValid();

  // Section numbers and step numbers below refer to ES2025, draft rev
  // ac21460fedf4b926520b06c9820bdbebad596a8b.
  //
  // This function aims to implement 10.1.6 [[DefineOwnProperty]] as well as
  // the [[DefineOwnProperty]] methods described in 10.4.2.1 (arrays), 10.4.4.2
  // (arguments), and 10.4.5.3 (typed arrays).

  // Dispense with custom behavior of exotic native objects first.
  if (obj->is<ArrayObject>()) {
    // 10.4.2.1 step 1. Redefining an array's length is very special.
    auto arr = HandleObject(obj).as<ArrayObject>();
    if (id == NameToId(cx->names().length)) {
      // 10.1.6.3 ValidateAndApplyPropertyDescriptor, step 5.c.
      if (desc_.isAccessorDescriptor()) {
        return result.fail(JSMSG_CANT_REDEFINE_PROP);
      }

      return ArraySetLength(cx, arr, id, desc_, result);
    }

    // 10.4.2.1 step 2. Don't extend a fixed-length array.
    uint32_t index;
    if (IdIsIndex(id, &index)) {
      if (WouldDefinePastNonwritableLength(arr, index)) {
        return result.fail(JSMSG_CANT_DEFINE_PAST_ARRAY_LENGTH);
      }
    }
  } else if (obj->is<TypedArrayObject>()) {
    // 10.4.5.3 step 1. Indexed properties of typed arrays are special.
    if (mozilla::Maybe<uint64_t> index = ToTypedArrayIndex(id)) {
      auto tobj = HandleObject(obj).as<TypedArrayObject>();
      return DefineTypedArrayElement(cx, tobj, index.value(), desc_, result);
    }
  } else if (obj->is<ArgumentsObject>() && !desc_.resolving()) {
    auto argsobj = HandleObject(obj).as<ArgumentsObject>();
    if (id.isAtom(cx->names().length)) {
      // Either we are resolving the .length property on this object,
      // or redefining it. In the latter case only, we must reify the
      // property.
      if (!ArgumentsObject::reifyLength(cx, argsobj)) {
        return false;
      }
    } else if (id.isAtom(cx->names().callee) &&
               obj->is<MappedArgumentsObject>()) {
      // Do same thing as .length for .callee on MappedArgumentsObject.
      auto mapped = HandleObject(argsobj).as<MappedArgumentsObject>();
      if (!MappedArgumentsObject::reifyCallee(cx, mapped)) {
        return false;
      }
    } else if (id.isWellKnownSymbol(JS::SymbolCode::iterator)) {
      // Do same thing as .length for [@@iterator].
      if (!ArgumentsObject::reifyIterator(cx, argsobj)) {
        return false;
      }
    } else if (id.isInt()) {
      argsobj->markElementOverridden();
    }
  }

  // 10.1.6.1 OrdinaryDefineOwnProperty step 1.
  PropertyResult prop;
  if (desc_.resolving()) {
    // We are being called from a resolve or enumerate hook to reify a
    // lazily-resolved property. To avoid reentering the resolve hook and
    // recursing forever, skip the resolve hook when doing this lookup.
    if (!NativeLookupOwnPropertyNoResolve(cx, obj, id, &prop)) {
      return false;
    }
  } else {
    if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &prop)) {
      return false;
    }
  }
  MOZ_ASSERT(!prop.isTypedArrayElement());

  // From this point, the step numbers refer to
  // 10.1.6.3, ValidateAndApplyPropertyDescriptor.
  // Step 1 is a redundant assertion.

  // Filling in desc: Here we make a copy of the desc_ argument. We will turn
  // it into a complete descriptor before updating obj. The spec algorithm
  // does not explicitly do this, but the end result is the same. Search for
  // "fill in" below for places where the filling-in actually occurs.
  Rooted<PropertyDescriptor> desc(cx, desc_);

  // Step 2.
  if (prop.isNotFound()) {
    // Note: We are sharing the property definition machinery with private
    //       fields. Private fields may be added to non-extensible objects.
    if (!obj->isExtensible() && !id.isPrivateName()) {
      return result.fail(JSMSG_CANT_DEFINE_PROP_OBJECT_NOT_EXTENSIBLE);
    }

    // Fill in missing desc fields with defaults.
    CompletePropertyDescriptor(&desc);

    if (!AddOrChangeProperty<IsAddOrChange::Add>(cx, obj, id, desc)) {
      return false;
    }
    return result.succeed();
  }

  // Step 3 is an assertion.

  // Step 4 and 5 (partially). Prop might not actually have a real shape, e.g.
  // in the case of dense array elements, GetPropertyAttributes is used to
  // paper-over that difference.
  JS::PropertyAttributes attrs = GetPropertyAttributes(obj, prop);
  bool redundant;
  if (!DefinePropertyIsRedundant(cx, obj, id, prop, attrs, desc, &redundant)) {
    return false;
  }
  if (redundant) {
    return result.succeed();
  }

  // Step 5.
  if (!attrs.configurable()) {
    // Step 5.a.
    if (desc.hasConfigurable() && desc.configurable()) {
      return result.fail(JSMSG_CANT_REDEFINE_PROP);
    }

    // Step 5.b.
    if (desc.hasEnumerable() && desc.enumerable() != attrs.enumerable()) {
      return result.fail(JSMSG_CANT_REDEFINE_PROP);
    }

    MOZ_ASSERT(
        !desc.isGenericDescriptor(),
        "redundant or conflicting generic property descriptor already handled");

    // Steps 5.c-d.
    //
    // If this is an existing accessor property and the property definition is
    // non-redundant, this must be an attempt to change an accessor function or
    // a redefinition to a data property. Both operations are invalid.
    //
    // If this is an existing data property and the incoming property descriptor
    // is an accessor property descriptor, this is an invalid redefinition to an
    // accessor property.
    if (IsAccessorDescriptor(prop) || desc.isAccessorDescriptor()) {
      return result.fail(JSMSG_CANT_REDEFINE_PROP);
    }

    // Step 5.e.
    if (!attrs.writable()) {
      // Step 5.e.i.
      if (desc.hasWritable() && desc.writable()) {
        return result.fail(JSMSG_CANT_REDEFINE_PROP);
      }

      // Step 5.e.ii.
      if (desc.hasValue()) {
        RootedValue currentValue(cx);
        if (!GetExistingDataProperty(cx, obj, id, prop, &currentValue)) {
          return false;
        }

        bool same;
        if (!SameValue(cx, desc.value(), currentValue, &same)) {
          return false;
        }
        if (!same) {
          return result.fail(JSMSG_CANT_REDEFINE_PROP);
        }
      }

      return result.succeed();
    }
  }

  // Fill in desc.[[Configurable]] and desc.[[Enumerable]] if missing.
  if (!desc.hasConfigurable()) {
    desc.setConfigurable(attrs.configurable());
  }
  if (!desc.hasEnumerable()) {
    desc.setEnumerable(attrs.enumerable());
  }

  // Step 6.
  if (desc.isDataDescriptor()) {
    // Fill in desc.[[Value]] and desc.[[Writable]].
    if (IsDataDescriptor(prop)) {
      if (!desc.hasValue()) {
        RootedValue currentValue(cx);
        if (!GetExistingDataProperty(cx, obj, id, prop, &currentValue)) {
          return false;
        }

        desc.setValue(currentValue);
      }
      if (!desc.hasWritable()) {
        desc.setWritable(attrs.writable());
      }
    } else {
      if (!desc.hasValue()) {
        desc.setValue(UndefinedHandleValue);
      }
      if (!desc.hasWritable()) {
        desc.setWritable(false);
      }
    }
  } else if (desc.isAccessorDescriptor()) {
    // Fill in desc.[[Get]] and desc.[[Set]] from shape.
    if (IsAccessorDescriptor(prop)) {
      PropertyInfo propInfo = prop.propertyInfo();
      MOZ_ASSERT(propInfo.isAccessorProperty());
      MOZ_ASSERT(desc.isAccessorDescriptor());

      if (!desc.hasGetter()) {
        desc.setGetter(obj->getGetter(propInfo));
      }
      if (!desc.hasSetter()) {
        desc.setSetter(obj->getSetter(propInfo));
      }
    } else {
      if (!desc.hasGetter()) {
        desc.setGetter(nullptr);
      }
      if (!desc.hasSetter()) {
        desc.setSetter(nullptr);
      }
    }
  } else {
    MOZ_ASSERT(desc.isGenericDescriptor());

    // Fill in desc. A generic descriptor has none of these fields, so copy
    // everything from shape.
    MOZ_ASSERT(!desc.hasValue());
    MOZ_ASSERT(!desc.hasWritable());
    MOZ_ASSERT(!desc.hasGetter());
    MOZ_ASSERT(!desc.hasSetter());
    if (IsDataDescriptor(prop)) {
      RootedValue currentValue(cx);
      if (!GetExistingDataProperty(cx, obj, id, prop, &currentValue)) {
        return false;
      }
      desc.setValue(currentValue);
      desc.setWritable(attrs.writable());
    } else {
      PropertyInfo propInfo = prop.propertyInfo();
      desc.setGetter(obj->getGetter(propInfo));
      desc.setSetter(obj->getSetter(propInfo));
    }
  }
  desc.assertComplete();

  if (!AddOrChangeProperty<IsAddOrChange::Change>(cx, obj, id, desc, &prop)) {
    return false;
  }

  // Step 7.
  return result.succeed();
}

bool js::NativeDefineDataProperty(JSContext* cx, Handle<NativeObject*> obj,
                                  HandleId id, HandleValue value,
                                  unsigned attrs, ObjectOpResult& result) {
  Rooted<PropertyDescriptor> desc(cx, PropertyDescriptor::Data(value, attrs));
  return NativeDefineProperty(cx, obj, id, desc, result);
}

bool js::NativeDefineAccessorProperty(JSContext* cx, Handle<NativeObject*> obj,
                                      HandleId id, HandleObject getter,
                                      HandleObject setter, unsigned attrs) {
  Rooted<PropertyDescriptor> desc(
      cx, PropertyDescriptor::Accessor(
              getter ? mozilla::Some(getter) : mozilla::Nothing(),
              setter ? mozilla::Some(setter) : mozilla::Nothing(), attrs));

  ObjectOpResult result;
  if (!NativeDefineProperty(cx, obj, id, desc, result)) {
    return false;
  }

  if (!result) {
    // Off-thread callers should not get here: they must call this
    // function only with known-valid arguments. Populating a new
    // PlainObject with configurable properties is fine.
    result.reportError(cx, obj, id);
    return false;
  }

  return true;
}

bool js::NativeDefineDataProperty(JSContext* cx, Handle<NativeObject*> obj,
                                  HandleId id, HandleValue value,
                                  unsigned attrs) {
  ObjectOpResult result;
  if (!NativeDefineDataProperty(cx, obj, id, value, attrs, result)) {
    return false;
  }
  if (!result) {
    // Off-thread callers should not get here: they must call this
    // function only with known-valid arguments. Populating a new
    // PlainObject with configurable properties is fine.
    result.reportError(cx, obj, id);
    return false;
  }
  return true;
}

bool js::NativeDefineDataProperty(JSContext* cx, Handle<NativeObject*> obj,
                                  PropertyName* name, HandleValue value,
                                  unsigned attrs) {
  RootedId id(cx, NameToId(name));
  return NativeDefineDataProperty(cx, obj, id, value, attrs);
}

static bool DefineNonexistentProperty(JSContext* cx, Handle<NativeObject*> obj,
                                      HandleId id, HandleValue v,
                                      ObjectOpResult& result) {
  // Optimized NativeDefineProperty() version for known absent properties.

  // Dispense with custom behavior of exotic native objects first.
  if (obj->is<ArrayObject>()) {
    // Array's length property is non-configurable, so we shouldn't
    // encounter it in this function.
    MOZ_ASSERT(id != NameToId(cx->names().length));

    // 10.4.2.1 step 2. Don't extend a fixed-length array.
    uint32_t index;
    if (IdIsIndex(id, &index)) {
      if (WouldDefinePastNonwritableLength(&obj->as<ArrayObject>(), index)) {
        return result.fail(JSMSG_CANT_DEFINE_PAST_ARRAY_LENGTH);
      }
    }
  } else if (obj->is<ArgumentsObject>()) {
    // If this method is called with either |length| or |@@iterator|, the
    // property was previously deleted and hence should already be marked
    // as overridden.
    MOZ_ASSERT_IF(id.isAtom(cx->names().length),
                  obj->as<ArgumentsObject>().hasOverriddenLength());
    MOZ_ASSERT_IF(id.isWellKnownSymbol(JS::SymbolCode::iterator),
                  obj->as<ArgumentsObject>().hasOverriddenIterator());

    // We still need to mark any element properties as overridden.
    if (id.isInt()) {
      obj->as<ArgumentsObject>().markElementOverridden();
    }
  }

  // Indexed properties of typed arrays are handled by the caller.
  MOZ_ASSERT_IF(obj->is<TypedArrayObject>(), ToTypedArrayIndex(id).isNothing());

#ifdef DEBUG
  PropertyResult prop;
  if (!NativeLookupOwnPropertyNoResolve(cx, obj, id, &prop)) {
    return false;
  }
  MOZ_ASSERT(prop.isNotFound(), "didn't expect to find an existing property");
#endif

  // 10.1.6.3, ValidateAndApplyPropertyDescriptor.
  // Step 1 is a redundant assertion, step 3 and later don't apply here.

  // Step 2.
  if (!obj->isExtensible()) {
    return result.fail(JSMSG_CANT_DEFINE_PROP_OBJECT_NOT_EXTENSIBLE);
  }

  if (id.isInt()) {
    // This might be a dense element. Use AddOrChangeProperty as it knows
    // how to deal with that.
    Rooted<PropertyDescriptor> desc(
        cx, PropertyDescriptor::Data(v, {JS::PropertyAttribute::Configurable,
                                         JS::PropertyAttribute::Enumerable,
                                         JS::PropertyAttribute::Writable}));
    if (!AddOrChangeProperty<IsAddOrChange::Add>(cx, obj, id, desc)) {
      return false;
    }
  } else {
    if (!AddDataProperty(cx, obj, id, v)) {
      return false;
    }
  }

  return result.succeed();
}

bool js::AddOrUpdateSparseElementHelper(JSContext* cx,
                                        Handle<NativeObject*> obj,
                                        int32_t int_id, HandleValue v,
                                        bool strict) {
  MOZ_ASSERT(obj->is<ArrayObject>() || obj->is<PlainObject>());

  // This helper doesn't handle the case where the index is a dense element.
  MOZ_ASSERT(int_id >= 0);
  MOZ_ASSERT(!obj->containsDenseElement(int_id));

  MOZ_ASSERT(PropertyKey::fitsInInt(int_id));
  RootedId id(cx, PropertyKey::Int(int_id));

  // First decide if this is an add or an update. Because the IC guards have
  // already ensured this exists exterior to the dense array range, and the
  // prototype checks have ensured there are no indexes on the prototype, we
  // can use the shape lineage to find the element if it exists:
  uint32_t index;
  PropMap* map = obj->shape()->lookup(cx, id, &index);

  // If we didn't find the property, we're on the add path: delegate to
  // AddOrChangeProperty. This will add either a sparse element or a dense
  // element.
  if (map == nullptr) {
    Rooted<PropertyDescriptor> desc(
        cx, PropertyDescriptor::Data(v, {JS::PropertyAttribute::Configurable,
                                         JS::PropertyAttribute::Enumerable,
                                         JS::PropertyAttribute::Writable}));
    return AddOrChangeProperty<IsAddOrChange::Add>(cx, obj, id, desc);
  }

  // At this point we're updating a property: See SetExistingProperty.
  PropertyInfo prop = map->getPropertyInfo(index);
  if (prop.isDataProperty() && prop.writable()) {
    obj->setSlot(prop.slot(), v);
    return true;
  }

  // We don't know exactly what this object looks like, hit the slowpath.
  RootedValue receiver(cx, ObjectValue(*obj));
  JS::ObjectOpResult result;
  return SetProperty(cx, obj, id, v, receiver, result) &&
         result.checkStrictModeError(cx, obj, id, strict);
}

/*** [[HasProperty]] ********************************************************/

/**
 * 10.1.7.1 OrdinaryHasProperty ( O, P )
 *
 * ES2025 draft rev ac21460fedf4b926520b06c9820bdbebad596a8b
 */
bool js::NativeHasProperty(JSContext* cx, Handle<NativeObject*> obj,
                           HandleId id, bool* foundp) {
  Rooted<NativeObject*> pobj(cx, obj);
  PropertyResult prop;

  // This loop isn't explicit in the spec algorithm. See the comment on step
  // 4.a. below.
  for (;;) {
    // Step 1.
    if (!NativeLookupOwnPropertyInline<CanGC>(cx, pobj, id, &prop)) {
      return false;
    }

    // Step 2.
    if (prop.isFound()) {
      *foundp = true;
      return true;
    }

    // Step 3.
    JSObject* proto = pobj->staticPrototype();

    // Step 5. (Reordered)
    // As a side-effect of NativeLookupOwnPropertyInline, we may determine that
    // a property is not found and the proto chain should not be searched. This
    // can occur for:
    //  - Out-of-range numeric properties of a TypedArrayObject
    //  - Recursive resolve hooks (which is expected when they try to set the
    //    property being resolved).
    if (!proto || prop.shouldIgnoreProtoChain()) {
      *foundp = false;
      return true;
    }

    // Step 4.a. If the prototype is also native, this step is a recursive tail
    // call, and we don't need to go through all the plumbing of HasProperty;
    // the top of the loop is where we're going to end up anyway. But if |proto|
    // is non-native, that optimization would be incorrect.
    if (!proto->is<NativeObject>()) {
      RootedObject protoRoot(cx, proto);
      return HasProperty(cx, protoRoot, id, foundp);
    }

    pobj = &proto->as<NativeObject>();
  }
}

/*** [[GetOwnPropertyDescriptor]] *******************************************/

bool js::NativeGetOwnPropertyDescriptor(
    JSContext* cx, Handle<NativeObject*> obj, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) {
  PropertyResult prop;
  if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &prop)) {
    return false;
  }
  if (prop.isNotFound()) {
    desc.reset();
    return true;
  }

  if (prop.isNativeProperty() && prop.propertyInfo().isAccessorProperty()) {
    PropertyInfo propInfo = prop.propertyInfo();
    desc.set(mozilla::Some(PropertyDescriptor::Accessor(
        obj->getGetter(propInfo), obj->getSetter(propInfo),
        propInfo.propAttributes())));
    return true;
  }

  RootedValue value(cx);
  if (!GetExistingDataProperty(cx, obj, id, prop, &value)) {
    return false;
  }

  JS::PropertyAttributes attrs = GetPropertyAttributes(obj, prop);
  desc.set(mozilla::Some(PropertyDescriptor::Data(value, attrs)));
  return true;
}

/*** [[Get]] ****************************************************************/

static bool GetCustomDataProperty(JSContext* cx, HandleObject obj, HandleId id,
                                  MutableHandleValue vp) {
  cx->check(obj, id, vp);

  const JSClass* clasp = obj->getClass();
  if (clasp == &ArrayObject::class_) {
    if (!ArrayLengthGetter(cx, obj, id, vp)) {
      return false;
    }
  } else if (clasp == &MappedArgumentsObject::class_) {
    if (!MappedArgGetter(cx, obj, id, vp)) {
      return false;
    }
  } else {
    MOZ_RELEASE_ASSERT(clasp == &UnmappedArgumentsObject::class_);
    if (!UnmappedArgGetter(cx, obj, id, vp)) {
      return false;
    }
  }

  cx->check(vp);
  return true;
}

static inline bool CallGetter(JSContext* cx, Handle<NativeObject*> obj,
                              HandleValue receiver, HandleId id,
                              PropertyInfo prop, MutableHandleValue vp) {
  MOZ_ASSERT(!prop.isDataProperty());

  if (prop.isAccessorProperty()) {
    RootedValue getter(cx, obj->getGetterValue(prop));
    return js::CallGetter(cx, receiver, getter, vp);
  }

  MOZ_ASSERT(prop.isCustomDataProperty());

  return GetCustomDataProperty(cx, obj, id, vp);
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE bool GetExistingProperty(
    JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType receiver,
    typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
    typename MaybeRooted<jsid, allowGC>::HandleType id, PropertyInfo prop,
    typename MaybeRooted<Value, allowGC>::MutableHandleType vp) {
  if (prop.isDataProperty()) {
    vp.set(obj->getSlot(prop.slot()));
    return true;
  }

  vp.setUndefined();

  if (!prop.isCustomDataProperty() && !obj->hasGetter(prop)) {
    return true;
  }

  if constexpr (!allowGC) {
    return false;
  } else {
    return CallGetter(cx, obj, receiver, id, prop, vp);
  }
}

bool js::NativeGetExistingProperty(JSContext* cx, HandleObject receiver,
                                   Handle<NativeObject*> obj, HandleId id,
                                   PropertyInfo prop, MutableHandleValue vp) {
  RootedValue receiverValue(cx, ObjectValue(*receiver));
  return GetExistingProperty<CanGC>(cx, receiverValue, obj, id, prop, vp);
}

enum IsNameLookup { NotNameLookup = false, NameLookup = true };

/*
 * Finish getting the property `receiver[id]` after looking at every object on
 * the prototype chain and not finding any such property.
 *
 * Per the spec, this should just set the result to `undefined` and call it a
 * day. However this function also runs when we're evaluating an
 * expression that's an Identifier (that is, an unqualified name lookup),
 * so we need to figure out if that's what's happening and throw
 * a ReferenceError if so.
 */
static bool GetNonexistentProperty(JSContext* cx, HandleId id,
                                   IsNameLookup nameLookup,
                                   MutableHandleValue vp) {
  vp.setUndefined();

  // If we are doing a name lookup, this is a ReferenceError.
  if (nameLookup) {
    ReportIsNotDefined(cx, id);
    return false;
  }

  // Otherwise, just return |undefined|.
  return true;
}

// The NoGC version of GetNonexistentProperty, present only to make types line
// up.
bool GetNonexistentProperty(JSContext* cx, const jsid& id,
                            IsNameLookup nameLookup,
                            FakeMutableHandle<Value> vp) {
  return false;
}

static inline bool GeneralizedGetProperty(JSContext* cx, HandleObject obj,
                                          HandleId id, HandleValue receiver,
                                          IsNameLookup nameLookup,
                                          MutableHandleValue vp) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  if (nameLookup) {
    // When nameLookup is true, GeneralizedGetProperty implements 9.1.1.2.6
    // GetBindingValue, ES2025 rev ac21460fedf4b926520b06c9820bdbebad596a8b,
    // with step 2 (the call to HasProperty) and step 4 (the call to Get) fused
    // so that only a single lookup is needed.
    //
    // If we get here, we've reached a non-native object. Fall back on the
    // algorithm as specified, with two separate lookups. (Note that we
    // throw ReferenceErrors regardless of strictness, technically a bug.)

    bool found;
    if (!HasProperty(cx, obj, id, &found)) {
      return false;
    }
    if (!found) {
      ReportIsNotDefined(cx, id);
      return false;
    }
  }

  return GetProperty(cx, obj, receiver, id, vp);
}

static inline bool GeneralizedGetProperty(JSContext* cx, JSObject* obj, jsid id,
                                          const Value& receiver,
                                          IsNameLookup nameLookup,
                                          FakeMutableHandle<Value> vp) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.checkDontReport(cx)) {
    return false;
  }
  if (nameLookup) {
    return false;
  }
  return GetPropertyNoGC(cx, obj, receiver, id, vp.address());
}

bool js::GetSparseElementHelper(JSContext* cx, Handle<NativeObject*> obj,
                                int32_t int_id, MutableHandleValue result) {
  MOZ_ASSERT(obj->is<ArrayObject>() || obj->is<PlainObject>());

  // This helper doesn't handle the case where the index is a dense element.
  MOZ_ASSERT(int_id >= 0);
  MOZ_ASSERT(!obj->containsDenseElement(int_id));

  // Indexed properties can not exist on the prototype chain.
  MOZ_ASSERT(!PrototypeMayHaveIndexedProperties(obj));

  MOZ_ASSERT(PropertyKey::fitsInInt(int_id));
  RootedId id(cx, PropertyKey::Int(int_id));

  uint32_t index;
  PropMap* map = obj->shape()->lookup(cx, id, &index);
  if (!map) {
    // Property not found, return directly.
    result.setUndefined();
    return true;
  }

  PropertyInfo prop = map->getPropertyInfo(index);
  RootedValue receiver(cx, ObjectValue(*obj));
  return GetExistingProperty<CanGC>(cx, receiver, obj, id, prop, result);
}

/**
 * 10.1.8.1 OrdinaryGet ( O, P, Receiver )
 *
 * ES2025 draft rev ac21460fedf4b926520b06c9820bdbebad596a8b
 */
template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE bool NativeGetPropertyInline(
    JSContext* cx, typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
    typename MaybeRooted<Value, allowGC>::HandleType receiver,
    typename MaybeRooted<jsid, allowGC>::HandleType id, IsNameLookup nameLookup,
    typename MaybeRooted<Value, allowGC>::MutableHandleType vp) {
  typename MaybeRooted<NativeObject*, allowGC>::RootType pobj(cx, obj);
  PropertyResult prop;

  // This loop isn't explicit in the spec algorithm. See the comment on step
  // 2.c below.
  for (;;) {
    // Step 1.
    if (!NativeLookupOwnPropertyInline<allowGC>(cx, pobj, id, &prop)) {
      return false;
    }

    if (prop.isFound()) {
      // Steps 3-7. Special case for dense elements because
      // GetExistingProperty doesn't support those.
      if (prop.isDenseElement()) {
        vp.set(pobj->getDenseElement(prop.denseElementIndex()));
        return true;
      }
      if (prop.isTypedArrayElement()) {
        size_t idx = prop.typedArrayElementIndex();
        auto* tarr = &pobj->template as<TypedArrayObject>();
        return tarr->template getElement<allowGC>(cx, idx, vp);
      }

      return GetExistingProperty<allowGC>(cx, receiver, pobj, id,
                                          prop.propertyInfo(), vp);
    }

    // Step 2.a.
    JSObject* proto = pobj->staticPrototype();

    // Step 2.b. The spec algorithm simply returns undefined if proto is
    // null, but see the comment on GetNonexistentProperty.
    if (!proto || prop.shouldIgnoreProtoChain()) {
      return GetNonexistentProperty(cx, id, nameLookup, vp);
    }

    // Step 2. If the prototype is also native, this step is a recursive tail
    // call, and we don't need to go through all the plumbing of GetProperty;
    // the top of the loop is where we're going to end up anyway. But if |proto|
    // is non-native, that optimization would be incorrect.
    if (proto->getOpsGetProperty()) {
      RootedObject protoRoot(cx, proto);
      return GeneralizedGetProperty(cx, protoRoot, id, receiver, nameLookup,
                                    vp);
    }

    pobj = &proto->as<NativeObject>();
  }
}

bool js::NativeGetProperty(JSContext* cx, Handle<NativeObject*> obj,
                           HandleValue receiver, HandleId id,
                           MutableHandleValue vp) {
  return NativeGetPropertyInline<CanGC>(cx, obj, receiver, id, NotNameLookup,
                                        vp);
}

bool js::NativeGetPropertyNoGC(JSContext* cx, NativeObject* obj,
                               const Value& receiver, jsid id, Value* vp) {
  AutoAssertNoPendingException noexc(cx);
  return NativeGetPropertyInline<NoGC>(cx, obj, receiver, id, NotNameLookup,
                                       vp);
}

bool js::NativeGetElement(JSContext* cx, Handle<NativeObject*> obj,
                          HandleValue receiver, int32_t index,
                          MutableHandleValue vp) {
  RootedId id(cx);

  if (MOZ_LIKELY(index >= 0)) {
    if (!IndexToId(cx, index, &id)) {
      return false;
    }
  } else {
    RootedValue indexVal(cx, Int32Value(index));
    if (!PrimitiveValueToId<CanGC>(cx, indexVal, &id)) {
      return false;
    }
  }
  return NativeGetProperty(cx, obj, receiver, id, vp);
}

bool js::GetNameBoundInEnvironment(JSContext* cx, HandleObject envArg,
                                   HandleId id, MutableHandleValue vp) {
  // Manually unwrap 'with' environments to prevent looking up @@unscopables
  // twice.
  //
  // This is unfortunate because internally, the engine does not distinguish
  // HasProperty from HasBinding: both are implemented as a HasPropertyOp
  // hook on a WithEnvironmentObject.
  //
  // In the case of attempting to get the value of a binding already looked up
  // via JSOp::BindName or JSOp::BindUnqualifiedName, calling HasProperty on the
  // WithEnvironmentObject is equivalent to calling HasBinding a second time.
  // This results in the incorrect behavior of performing the @@unscopables
  // check again.
  RootedObject env(cx, MaybeUnwrapWithEnvironment(envArg));
  RootedValue receiver(cx, ObjectValue(*env));
  if (env->getOpsGetProperty()) {
    return GeneralizedGetProperty(cx, env, id, receiver, NameLookup, vp);
  }
  return NativeGetPropertyInline<CanGC>(cx, env.as<NativeObject>(), receiver,
                                        id, NameLookup, vp);
}

/*** [[Set]] ****************************************************************/

static bool SetCustomDataProperty(JSContext* cx, HandleObject obj, HandleId id,
                                  HandleValue v, ObjectOpResult& result) {
  cx->check(obj, id, v);

  const JSClass* clasp = obj->getClass();
  if (clasp == &ArrayObject::class_) {
    return ArrayLengthSetter(cx, obj, id, v, result);
  }
  if (clasp == &MappedArgumentsObject::class_) {
    return MappedArgSetter(cx, obj, id, v, result);
  }
  MOZ_RELEASE_ASSERT(clasp == &UnmappedArgumentsObject::class_);
  return UnmappedArgSetter(cx, obj, id, v, result);
}

static bool MaybeReportUndeclaredVarAssignment(JSContext* cx, HandleId id) {
  {
    jsbytecode* pc;
    JSScript* script =
        cx->currentScript(&pc, JSContext::AllowCrossRealm::Allow);
    if (!script) {
      return true;
    }

    if (!IsStrictSetPC(pc)) {
      return true;
    }
  }

  UniqueChars bytes =
      IdToPrintableUTF8(cx, id, IdToPrintableBehavior::IdIsIdentifier);
  if (!bytes) {
    return false;
  }
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_UNDECLARED_VAR,
                           bytes.get());
  return false;
}

/*
 * Finish assignment to a shapeful data property of a native object obj. This
 * conforms to no standard and there is a lot of legacy baggage here.
 */
static bool NativeSetExistingDataProperty(JSContext* cx,
                                          Handle<NativeObject*> obj,
                                          HandleId id, PropertyInfo prop,
                                          HandleValue v,
                                          ObjectOpResult& result) {
  MOZ_ASSERT(obj->is<NativeObject>());
  MOZ_ASSERT(prop.isDataDescriptor());

  Watchtower::watchPropertyValueChange<AllowGC::CanGC>(cx, obj, id, v, prop);

  if (prop.isDataProperty()) {
    // The common path. Standard data property.
    obj->setSlot(prop.slot(), v);
    return result.succeed();
  }

  MOZ_ASSERT(prop.isCustomDataProperty());
  MOZ_ASSERT(!obj->is<WithEnvironmentObject>());  // See bug 1128681.

  return SetCustomDataProperty(cx, obj, id, v, result);
}

/**
 * 10.1.9.2 OrdinarySetWithOwnDescriptor ( O, P, V, Receiver, ownDesc )
 *
 * ES2025 draft rev ac21460fedf4b926520b06c9820bdbebad596a8b
 *
 * When a [[Set]] operation finds no existing property with the given id
 * or finds a writable data property on the prototype chain, we end up here.
 * Finish the [[Set]] by defining a new property on receiver.
 *
 * This implements steps 2.b-e, but it is really old code and there are a few
 * barnacles.
 */
static bool SetPropertyByDefining(JSContext* cx, HandleId id, HandleValue v,
                                  HandleValue receiverValue,
                                  ObjectOpResult& result) {
  // Step 2.b.
  if (!receiverValue.isObject()) {
    return result.fail(JSMSG_SET_NON_OBJECT_RECEIVER);
  }
  RootedObject receiver(cx, &receiverValue.toObject());

  bool existing;
  {
    // Step 2.c.
    Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
    if (!GetOwnPropertyDescriptor(cx, receiver, id, &desc)) {
      return false;
    }

    existing = desc.isSome();

    // Step 2.d.
    if (existing) {
      // Step 2.d.i.
      if (desc->isAccessorDescriptor()) {
        return result.fail(JSMSG_OVERWRITING_ACCESSOR);
      }

      // Step 2.d.ii.
      if (!desc->writable()) {
        return result.fail(JSMSG_READ_ONLY);
      }
    }
  }

  // Steps 2.d.iii-iv. and 2.e.i. Define the new data property.
  Rooted<PropertyDescriptor> desc(cx);
  if (existing) {
    desc = PropertyDescriptor::Empty();
    desc.setValue(v);
  } else {
    desc = PropertyDescriptor::Data(v, {JS::PropertyAttribute::Configurable,
                                        JS::PropertyAttribute::Enumerable,
                                        JS::PropertyAttribute::Writable});
  }
  return DefineProperty(cx, receiver, id, desc, result);
}

enum class TypedArrayOutOfRange : bool { No, Yes };

/**
 * 10.1.9.2 OrdinarySetWithOwnDescriptor ( O, P, V, Receiver, ownDesc )
 * 10.4.5.5 [[Set]] ( P, V, Receiver )
 *
 * ES2025 draft rev ac21460fedf4b926520b06c9820bdbebad596a8b
 *
 * Implement "the rest of" assignment to a property when no property
 * receiver[id] was found anywhere on the prototype chain.
 */
template <QualifiedBool IsQualified>
static bool SetNonexistentProperty(JSContext* cx, Handle<NativeObject*> obj,
                                   Handle<NativeObject*> pobj, HandleId id,
                                   HandleValue v, HandleValue receiver,
                                   TypedArrayOutOfRange typedArrayOutOfRange,
                                   ObjectOpResult& result) {
  if (!IsQualified && receiver.isObject() &&
      receiver.toObject().isUnqualifiedVarObj()) {
    if (!MaybeReportUndeclaredVarAssignment(cx, id)) {
      return false;
    }
  }

  // Unqualified access may also need to take this code path, but it's currently
  // hard to tell, because with-environment objects call SetProperty<Qualified>,
  // which seems a bit dubious. And other environment objects probably don't
  // have typed array objects in the middle of their prototype chain, so we
  // can't really test this code path without first fixing with-environments.
  if constexpr (IsQualified) {
    // Indexed properties of typed arrays are special.
    if (typedArrayOutOfRange == TypedArrayOutOfRange::Yes) {
      MOZ_ASSERT(pobj->is<TypedArrayObject>(),
                 "typed array out-of-range reported by non-typed array?");
      MOZ_ASSERT(pobj == obj || !obj->is<TypedArrayObject>(),
                 "prototype chain not traversed for typed array indices");

      // 10.4.5.5, step 1.b.i.
      if (receiver.isObject() && pobj == &receiver.toObject()) {
        mozilla::Maybe<uint64_t> index = ToTypedArrayIndex(id);
        MOZ_ASSERT(index, "typed array out-of-range reported by non-index?");

        auto tobj = HandleObject(pobj).as<TypedArrayObject>();
        return SetTypedArrayElement(cx, tobj, *index, v, result);
      }

      // 10.4.5.5, step 1.b.ii.
      return result.succeed();
    }
  }

  // Pure optimization for the common case. There's no point performing the
  // lookup in step 2.c again, as our caller just did it for us.
  if (IsQualified && receiver.isObject() && obj == &receiver.toObject()) {
    // Ensure that a custom GetOwnPropertyOp, if present, doesn't
    // introduce additional properties which weren't previously found by
    // LookupOwnProperty.
#ifdef DEBUG
    if (GetOwnPropertyOp op = obj->getOpsGetOwnPropertyDescriptor()) {
      Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
      if (!op(cx, obj, id, &desc)) {
        return false;
      }
      MOZ_ASSERT(desc.isNothing());
    }
#endif

    // Step 2.e. Define the new data property.
    if (DefinePropertyOp op = obj->getOpsDefineProperty()) {
      Rooted<PropertyDescriptor> desc(
          cx, PropertyDescriptor::Data(v, {JS::PropertyAttribute::Configurable,
                                           JS::PropertyAttribute::Enumerable,
                                           JS::PropertyAttribute::Writable}));
      return op(cx, obj, id, desc, result);
    }

    return DefineNonexistentProperty(cx, obj, id, v, result);
  }

  return SetPropertyByDefining(cx, id, v, receiver, result);
}

// Set an existing own property obj[index] that's a dense element.
static bool SetDenseElement(JSContext* cx, Handle<NativeObject*> obj,
                            uint32_t index, HandleValue v,
                            ObjectOpResult& result) {
  MOZ_ASSERT(!obj->is<TypedArrayObject>());
  MOZ_ASSERT(obj->containsDenseElement(index));

  obj->setDenseElement(index, v);
  return result.succeed();
}

/**
 * 10.1.9.2 OrdinarySetWithOwnDescriptor ( O, P, V, Receiver, ownDesc )
 *
 * ES2025 draft rev ac21460fedf4b926520b06c9820bdbebad596a8b
 *
 * Finish the assignment `receiver[id] = v` when an existing property |prop|
 * has been found on a native object |pobj|.
 *
 * It is necessary to pass both id and prop because prop could be an implicit
 * dense or typed array element (i.e. not actually a pointer to a Shape).
 */
static bool SetExistingProperty(JSContext* cx, HandleId id, HandleValue v,
                                HandleValue receiver,
                                Handle<NativeObject*> pobj,
                                const PropertyResult& prop,
                                ObjectOpResult& result) {
  // Step 1. (Performed in caller)

  // Step 2 for dense and typed array elements.
  if (prop.isDenseElement() || prop.isTypedArrayElement()) {
    // Step 2.a.
    if (pobj->denseElementsAreFrozen()) {
      return result.fail(JSMSG_READ_ONLY);
    }

    // Pure optimization for the common case:
    if (receiver.isObject() && pobj == &receiver.toObject()) {
      if (prop.isTypedArrayElement()) {
        Rooted<TypedArrayObject*> tobj(cx, &pobj->as<TypedArrayObject>());
        size_t idx = prop.typedArrayElementIndex();
        return SetTypedArrayElement(cx, tobj, idx, v, result);
      }

      return SetDenseElement(cx, pobj, prop.denseElementIndex(), v, result);
    }

    // Steps 2.b-e.
    return SetPropertyByDefining(cx, id, v, receiver, result);
  }

  // Step 2 for all other properties.
  PropertyInfo propInfo = prop.propertyInfo();
  if (propInfo.isDataDescriptor()) {
    // Step 2.a.
    if (!propInfo.writable()) {
      return result.fail(JSMSG_READ_ONLY);
    }

    // steps 2.c-d.
    if (receiver.isObject() && pobj == &receiver.toObject()) {
      // Pure optimization for the common case. There's no point performing
      // the lookup in step 2.c again, as our caller just did it for us. The
      // result is |shapeProp|.

      // Steps 2.d.iii-iv.
      return NativeSetExistingDataProperty(cx, pobj, id, propInfo, v, result);
    }

    // Shadow pobj[id] by defining a new data property receiver[id].
    // Delegate everything to SetPropertyByDefining.
    return SetPropertyByDefining(cx, id, v, receiver, result);
  }

  // Step 3.
  MOZ_ASSERT(propInfo.isAccessorProperty());

  // Step 4.
  JSObject* setterObject = pobj->getSetter(propInfo);

  // Step 5.
  if (!setterObject) {
    return result.fail(JSMSG_GETTER_ONLY);
  }

  // Step 6.
  RootedValue setter(cx, ObjectValue(*setterObject));
  if (!js::CallSetter(cx, receiver, setter, v)) {
    return false;
  }

  // Step 7.
  return result.succeed();
}

/**
 * 10.1.9 [[Set]] ( P, V, Receiver )
 * 10.1.9.1 OrdinarySet ( O, P, V, Receiver )
 * 10.1.9.2 OrdinarySetWithOwnDescriptor ( O, P, V, Receiver, ownDesc )
 *
 * ES2025 draft rev ac21460fedf4b926520b06c9820bdbebad596a8b
 */
template <QualifiedBool IsQualified>
bool js::NativeSetProperty(JSContext* cx, Handle<NativeObject*> obj,
                           HandleId id, HandleValue v, HandleValue receiver,
                           ObjectOpResult& result) {
  // We substitute our own names for these names used in the spec:
  // O -> pobj, P -> id, ownDesc -> prop.
  PropertyResult prop;
  Rooted<NativeObject*> pobj(cx, obj);

  // This loop isn't explicit in the spec algorithm. See the comment on step
  // 1.b.i below. (There's a very similar loop in the NativeGetProperty
  // implementation, but unfortunately not similar enough to common up.)
  for (;;) {
    // OrdinarySet, step 1.
    if (!NativeLookupOwnPropertyInline<CanGC>(cx, pobj, id, &prop)) {
      return false;
    }

    // OrdinarySetWithOwnDescriptor, steps 2-3.
    if (prop.isFound()) {
      return SetExistingProperty(cx, id, v, receiver, pobj, prop, result);
    }

    // OrdinarySetWithOwnDescriptor, steps 1.a-b.
    // As a side-effect of NativeLookupOwnPropertyInline, we may determine that
    // a property is not found and the proto chain should not be searched. This
    // can occur for:
    //  - Out-of-range numeric properties of a TypedArrayObject
    //  - Recursive resolve hooks (which is expected when they try to set the
    //    property being resolved).
    JSObject* proto = pobj->staticPrototype();
    if (!proto || prop.shouldIgnoreProtoChain()) {
      // OrdinarySetWithOwnDescriptor, step 1.c.i (and step 2).
      return SetNonexistentProperty<IsQualified>(
          cx, obj, pobj, id, v, receiver,
          TypedArrayOutOfRange{prop.isTypedArrayOutOfRange()}, result);
    }

    // Step 1.b.i. If the prototype is also native, this step is a recursive
    // tail call, and we don't need to go through all the plumbing of
    // SetProperty; the top of the loop is where we're going to end up anyway.
    // But if |proto| is non-native, that optimization would be incorrect.
    if (!proto->is<NativeObject>()) {
      // Unqualified assignments are not specified to go through [[Set]]
      // at all, but they do go through this function. So check for
      // unqualified assignment to a nonexistent global (a strict error).
      RootedObject protoRoot(cx, proto);
      if (!IsQualified) {
        bool found;
        if (!HasProperty(cx, protoRoot, id, &found)) {
          return false;
        }
        if (!found) {
          return SetNonexistentProperty<IsQualified>(
              cx, obj, pobj, id, v, receiver, TypedArrayOutOfRange::No, result);
        }
      }

      return SetProperty(cx, protoRoot, id, v, receiver, result);
    }
    pobj = &proto->as<NativeObject>();
  }
}

template bool js::NativeSetProperty<Qualified>(JSContext* cx,
                                               Handle<NativeObject*> obj,
                                               HandleId id, HandleValue value,
                                               HandleValue receiver,
                                               ObjectOpResult& result);

template bool js::NativeSetProperty<Unqualified>(JSContext* cx,
                                                 Handle<NativeObject*> obj,
                                                 HandleId id, HandleValue value,
                                                 HandleValue receiver,
                                                 ObjectOpResult& result);

bool js::NativeSetElement(JSContext* cx, Handle<NativeObject*> obj,
                          uint32_t index, HandleValue v, HandleValue receiver,
                          ObjectOpResult& result) {
  RootedId id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return NativeSetProperty<Qualified>(cx, obj, id, v, receiver, result);
}

/*** [[Delete]] *************************************************************/

static bool CallJSDeletePropertyOp(JSContext* cx, JSDeletePropertyOp op,
                                   HandleObject receiver, HandleId id,
                                   ObjectOpResult& result) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  cx->check(receiver, id);
  if (op) {
    return op(cx, receiver, id, result);
  }
  return result.succeed();
}

/**
 * 10.1.10.1 OrdinaryDelete ( O, P )
 *
 * ES2025 draft rev ac21460fedf4b926520b06c9820bdbebad596a8b
 */
bool js::NativeDeleteProperty(JSContext* cx, Handle<NativeObject*> obj,
                              HandleId id, ObjectOpResult& result) {
  // Step 1.
  PropertyResult prop;
  if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &prop)) {
    return false;
  }

  // Step 2.
  if (prop.isNotFound()) {
    // If no property call the class's delProperty hook, passing succeeded
    // as the result parameter. This always succeeds when there is no hook.
    return CallJSDeletePropertyOp(cx, obj->getClass()->getDelProperty(), obj,
                                  id, result);
  }

  // Step 4. (Reordered) Non-configurable property.
  if (!GetPropertyAttributes(obj, prop).configurable()) {
    return result.failCantDelete();
  }

  // Typed array elements are configurable, but can't be deleted.
  if (prop.isTypedArrayElement()) {
    return result.failCantDelete();
  }

  if (!CallJSDeletePropertyOp(cx, obj->getClass()->getDelProperty(), obj, id,
                              result)) {
    return false;
  }
  if (!result) {
    return true;
  }

  // Step 3.
  if (prop.isDenseElement()) {
    obj->setDenseElementHole(prop.denseElementIndex());
  } else {
    if (!NativeObject::removeProperty(cx, obj, id)) {
      return false;
    }
  }

  return SuppressDeletedProperty(cx, obj, id);
}

bool js::CopyDataPropertiesNative(JSContext* cx, Handle<PlainObject*> target,
                                  Handle<NativeObject*> from,
                                  Handle<PlainObject*> excludedItems,
                                  bool* optimized) {
  *optimized = false;

  // Don't use the fast path if |from| may have extra indexed or lazy
  // properties.
  if (from->getDenseInitializedLength() > 0 || from->isIndexed() ||
      from->is<TypedArrayObject>() || from->getClass()->getNewEnumerate() ||
      from->getClass()->getEnumerate()) {
    return true;
  }

  // Collect all enumerable data properties.
  Rooted<PropertyInfoWithKeyVector> props(cx, PropertyInfoWithKeyVector(cx));

  Rooted<NativeShape*> fromShape(cx, from->shape());
  for (ShapePropertyIter<NoGC> iter(fromShape); !iter.done(); iter++) {
    jsid id = iter->key();
    MOZ_ASSERT(!id.isInt());

    if (!iter->enumerable()) {
      continue;
    }
    if (excludedItems && excludedItems->contains(cx, id)) {
      continue;
    }

    // Don't use the fast path if |from| contains non-data properties.
    //
    // This enables two optimizations:
    // 1. We don't need to handle the case when accessors modify |from|.
    // 2. String and symbol properties can be added in one go.
    if (!iter->isDataProperty()) {
      return true;
    }

    if (!props.append(*iter)) {
      return false;
    }
  }

  *optimized = true;

  // If |target| contains no own properties, we can directly call
  // AddDataPropertyNonPrototype.
  const bool targetHadNoOwnProperties = target->empty();

  RootedId key(cx);
  RootedValue value(cx);
  for (size_t i = props.length(); i > 0; i--) {
    PropertyInfoWithKey prop = props[i - 1];
    MOZ_ASSERT(prop.isDataProperty());
    MOZ_ASSERT(prop.enumerable());

    key = prop.key();
    MOZ_ASSERT(!key.isInt());

    MOZ_ASSERT(from->is<NativeObject>());
    MOZ_ASSERT(from->shape() == fromShape);

    value = from->getSlot(prop.slot());
    if (targetHadNoOwnProperties) {
      MOZ_ASSERT(!target->containsPure(key),
                 "didn't expect to find an existing property");

      if (!AddDataPropertyToPlainObject(cx, target, key, value)) {
        return false;
      }
    } else {
      if (!NativeDefineDataProperty(cx, target, key, value, JSPROP_ENUMERATE)) {
        return false;
      }
    }
  }

  return true;
}
