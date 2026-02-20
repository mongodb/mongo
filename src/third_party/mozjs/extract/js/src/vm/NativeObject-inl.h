/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_NativeObject_inl_h
#define vm_NativeObject_inl_h

#include "vm/NativeObject.h"

#include "mozilla/Maybe.h"

#include <type_traits>

#include "gc/GCEnum.h"
#include "gc/GCProbes.h"
#include "gc/MaybeRooted.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "vm/Compartment.h"
#include "vm/Iteration.h"
#include "vm/PlainObject.h"
#include "vm/PropertyResult.h"
#include "vm/StringType.h"
#include "vm/TypedArrayObject.h"

#include "gc/Marking-inl.h"
#include "gc/ObjectKind-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Realm-inl.h"
#include "vm/Shape-inl.h"

namespace js {

constexpr ObjectSlots::ObjectSlots(uint32_t capacity,
                                   uint32_t dictionarySlotSpan,
                                   uint64_t maybeUniqueId)
    : capacity_(capacity),
      dictionarySlotSpan_(dictionarySlotSpan),
      maybeUniqueId_(maybeUniqueId) {
  MOZ_ASSERT(this->capacity() == capacity);
  MOZ_ASSERT(this->dictionarySlotSpan() == dictionarySlotSpan);
}

inline uint32_t NativeObject::numFixedSlotsMaybeForwarded() const {
  return gc::MaybeForwarded(JSObject::shape())->asNative().numFixedSlots();
}

inline uint8_t* NativeObject::fixedData(size_t nslots) const {
  MOZ_ASSERT(ClassCanHaveFixedData(gc::MaybeForwardedObjectClass(this)));
  MOZ_ASSERT(nslots == numFixedSlotsMaybeForwarded());
  return reinterpret_cast<uint8_t*>(&fixedSlots()[nslots]);
}

inline void NativeObject::initDenseElementHole(uint32_t index) {
  markDenseElementsNotPacked();
  initDenseElementUnchecked(index, MagicValue(JS_ELEMENTS_HOLE));
}

inline void NativeObject::setDenseElementHole(uint32_t index) {
  markDenseElementsNotPacked();
  setDenseElementUnchecked(index, MagicValue(JS_ELEMENTS_HOLE));
}

inline void NativeObject::removeDenseElementForSparseIndex(uint32_t index) {
  MOZ_ASSERT(containsPure(PropertyKey::Int(index)));
  if (containsDenseElement(index)) {
    setDenseElementHole(index);
  }
}

inline void NativeObject::markDenseElementsNotPacked() {
  MOZ_ASSERT(is<NativeObject>());
  getElementsHeader()->markNonPacked();
}

inline void NativeObject::elementsRangePostWriteBarrier(uint32_t start,
                                                        uint32_t count) {
  if (!isTenured()) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    const Value& v = elements_[start + i];
    if (v.isGCThing()) {
      if (gc::StoreBuffer* sb = v.toGCThing()->storeBuffer()) {
        sb->putSlot(this, HeapSlot::Element, unshiftedIndex(start + i),
                    count - i);
        return;
      }
    }
  }
}

inline void NativeObject::copyDenseElements(uint32_t dstStart, const Value* src,
                                            uint32_t count) {
  MOZ_ASSERT(dstStart + count <= getDenseCapacity());
  MOZ_ASSERT(isExtensible());
  MOZ_ASSERT_IF(count > 0, src != nullptr);
#ifdef DEBUG
  for (uint32_t i = 0; i < count; ++i) {
    checkStoredValue(src[i]);
  }
#endif
  if (count == 0) {
    return;
  }
  if (zone()->needsIncrementalBarrier()) {
    uint32_t numShifted = getElementsHeader()->numShiftedElements();
    for (uint32_t i = 0; i < count; ++i) {
      elements_[dstStart + i].set(this, HeapSlot::Element,
                                  dstStart + i + numShifted, src[i]);
    }
  } else {
    memcpy(reinterpret_cast<Value*>(&elements_[dstStart]), src,
           count * sizeof(Value));
    elementsRangePostWriteBarrier(dstStart, count);
  }
}

inline void NativeObject::initDenseElements(NativeObject* src,
                                            uint32_t srcStart, uint32_t count) {
  MOZ_ASSERT(src->getDenseInitializedLength() >= srcStart + count);

  const Value* vp = src->getDenseElements() + srcStart;

  if (!src->denseElementsArePacked()) {
    // Mark non-packed if we're copying holes or if there are too many elements
    // to check this efficiently.
    static constexpr uint32_t MaxCountForPackedCheck = 30;
    if (count > MaxCountForPackedCheck) {
      markDenseElementsNotPacked();
    } else {
      for (uint32_t i = 0; i < count; i++) {
        if (vp[i].isMagic(JS_ELEMENTS_HOLE)) {
          markDenseElementsNotPacked();
          break;
        }
      }
    }
  }

  initDenseElements(vp, count);
}

inline void NativeObject::initDenseElements(const Value* src, uint32_t count) {
  MOZ_ASSERT(getDenseInitializedLength() == 0);
  MOZ_ASSERT(count <= getDenseCapacity());
  MOZ_ASSERT(src);
  MOZ_ASSERT(isExtensible());

  setDenseInitializedLength(count);

#ifdef DEBUG
  for (uint32_t i = 0; i < count; ++i) {
    checkStoredValue(src[i]);
  }
#endif

  memcpy(reinterpret_cast<Value*>(elements_), src, count * sizeof(Value));
  elementsRangePostWriteBarrier(0, count);
}

inline void NativeObject::initDenseElements(JSLinearString** src,
                                            uint32_t count) {
  MOZ_ASSERT(getDenseInitializedLength() == 0);
  MOZ_ASSERT(count <= getDenseCapacity());
  MOZ_ASSERT(src);
  MOZ_ASSERT(isExtensible());

  setDenseInitializedLength(count);
  Value* elementsBase = reinterpret_cast<Value*>(elements_);
  for (size_t i = 0; i < count; i++) {
    elementsBase[i].setString(src[i]);
  }

  elementsRangePostWriteBarrier(0, count);
}

inline void NativeObject::initDenseElementRange(uint32_t destStart,
                                                NativeObject* src,
                                                uint32_t count) {
  MOZ_ASSERT(count <= src->getDenseInitializedLength());

  // The initialized length must already be set to the correct value.
  MOZ_ASSERT(destStart + count == getDenseInitializedLength());

  if (!src->denseElementsArePacked()) {
    markDenseElementsNotPacked();
  }

  const Value* vp = src->getDenseElements();
#ifdef DEBUG
  for (uint32_t i = 0; i < count; ++i) {
    checkStoredValue(vp[i]);
  }
#endif
  memcpy(reinterpret_cast<Value*>(elements_) + destStart, vp,
         count * sizeof(Value));
  elementsRangePostWriteBarrier(destStart, count);
}

template <typename Iter>
inline bool NativeObject::initDenseElementsFromRange(JSContext* cx, Iter begin,
                                                     Iter end) {
  // This method populates the elements of a particular Array that's an
  // internal implementation detail of GeneratorObject. Failing any of the
  // following means the Array has escaped and/or been mistreated.
  MOZ_ASSERT(isExtensible());
  MOZ_ASSERT(!isIndexed());
  MOZ_ASSERT(is<ArrayObject>());
  MOZ_ASSERT(as<ArrayObject>().lengthIsWritable());
  MOZ_ASSERT(!denseElementsAreFrozen());
  MOZ_ASSERT(getElementsHeader()->numShiftedElements() == 0);

  MOZ_ASSERT(getDenseInitializedLength() == 0);

  auto size = end - begin;
  uint32_t count = uint32_t(size);
  MOZ_ASSERT(count <= uint32_t(INT32_MAX));
  if (count > getDenseCapacity()) {
    if (!growElements(cx, count)) {
      return false;
    }
  }

  HeapSlot* sp = elements_;
  size_t slot = 0;
  for (; begin != end; sp++, begin++) {
    Value v = *begin;
#ifdef DEBUG
    checkStoredValue(v);
#endif
    sp->init(this, HeapSlot::Element, slot++, v);
  }
  MOZ_ASSERT(slot == count);

  getElementsHeader()->initializedLength = count;
  as<ArrayObject>().setLengthToInitializedLength();
  return true;
}

inline bool NativeObject::tryShiftDenseElements(uint32_t count) {
  MOZ_ASSERT(isExtensible());

  ObjectElements* header = getElementsHeader();
  if (header->initializedLength == count ||
      count > ObjectElements::MaxShiftedElements ||
      header->hasNonwritableArrayLength()) {
    return false;
  }

  shiftDenseElementsUnchecked(count);
  return true;
}

inline void NativeObject::shiftDenseElementsUnchecked(uint32_t count) {
  MOZ_ASSERT(isExtensible());

  ObjectElements* header = getElementsHeader();
  MOZ_ASSERT(count > 0);
  MOZ_ASSERT(count < header->initializedLength);

  if (MOZ_UNLIKELY(header->numShiftedElements() + count >
                   ObjectElements::MaxShiftedElements)) {
    moveShiftedElements();
    header = getElementsHeader();
  }

  prepareElementRangeForOverwrite(0, count);
  header->addShiftedElements(count);

  elements_ += count;
  ObjectElements* newHeader = getElementsHeader();
  memmove(newHeader, header, sizeof(ObjectElements));
}

inline void NativeObject::moveDenseElements(uint32_t dstStart,
                                            uint32_t srcStart, uint32_t count) {
  MOZ_ASSERT(dstStart + count <= getDenseCapacity());
  MOZ_ASSERT(srcStart + count <= getDenseInitializedLength());
  MOZ_ASSERT(isExtensible());

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
  if (zone()->needsIncrementalBarrier()) {
    uint32_t numShifted = getElementsHeader()->numShiftedElements();
    if (dstStart < srcStart) {
      HeapSlot* dst = elements_ + dstStart;
      HeapSlot* src = elements_ + srcStart;
      for (uint32_t i = 0; i < count; i++, dst++, src++) {
        dst->set(this, HeapSlot::Element, dst - elements_ + numShifted, *src);
      }
    } else {
      HeapSlot* dst = elements_ + dstStart + count - 1;
      HeapSlot* src = elements_ + srcStart + count - 1;
      for (uint32_t i = 0; i < count; i++, dst--, src--) {
        dst->set(this, HeapSlot::Element, dst - elements_ + numShifted, *src);
      }
    }
  } else {
    memmove(elements_ + dstStart, elements_ + srcStart,
            count * sizeof(HeapSlot));
    elementsRangePostWriteBarrier(dstStart, count);
  }
}

inline void NativeObject::reverseDenseElementsNoPreBarrier(uint32_t length) {
  MOZ_ASSERT(!zone()->needsIncrementalBarrier());

  MOZ_ASSERT(isExtensible());

  MOZ_ASSERT(length > 1);
  MOZ_ASSERT(length <= getDenseInitializedLength());

  Value* valLo = reinterpret_cast<Value*>(elements_);
  Value* valHi = valLo + (length - 1);
  MOZ_ASSERT(valLo < valHi);

  do {
    Value origLo = *valLo;
    *valLo = *valHi;
    *valHi = origLo;
    ++valLo;
    --valHi;
  } while (valLo < valHi);

  elementsRangePostWriteBarrier(0, length);
}

inline void NativeObject::ensureDenseInitializedLength(uint32_t index,
                                                       uint32_t extra) {
  // Ensure that the array's contents have been initialized up to index, and
  // mark the elements through 'index + extra' as initialized in preparation
  // for a write.

  MOZ_ASSERT(!denseElementsAreFrozen());
  MOZ_ASSERT(isExtensible() || (containsDenseElement(index) && extra == 1));
  MOZ_ASSERT(index + extra <= getDenseCapacity());

  uint32_t initlen = getDenseInitializedLength();
  if (index + extra <= initlen) {
    return;
  }

  MOZ_ASSERT(isExtensible());

  if (index > initlen) {
    markDenseElementsNotPacked();
  }

  uint32_t numShifted = getElementsHeader()->numShiftedElements();
  size_t offset = initlen;
  for (HeapSlot* sp = elements_ + initlen; sp != elements_ + (index + extra);
       sp++, offset++) {
    sp->init(this, HeapSlot::Element, offset + numShifted,
             MagicValue(JS_ELEMENTS_HOLE));
  }

  getElementsHeader()->initializedLength = index + extra;
}

DenseElementResult NativeObject::extendDenseElements(JSContext* cx,
                                                     uint32_t requiredCapacity,
                                                     uint32_t extra) {
  MOZ_ASSERT(isExtensible());

  /*
   * Don't grow elements for objects which already have sparse indexes.
   * This avoids needing to count non-hole elements in willBeSparseElements
   * every time a new index is added.
   */
  if (isIndexed()) {
    return DenseElementResult::Incomplete;
  }

  /*
   * We use the extra argument also as a hint about number of non-hole
   * elements to be inserted.
   */
  if (requiredCapacity > MIN_SPARSE_INDEX &&
      willBeSparseElements(requiredCapacity, extra)) {
    return DenseElementResult::Incomplete;
  }

  if (!growElements(cx, requiredCapacity)) {
    return DenseElementResult::Failure;
  }

  return DenseElementResult::Success;
}

inline DenseElementResult NativeObject::ensureDenseElements(JSContext* cx,
                                                            uint32_t index,
                                                            uint32_t extra) {
  MOZ_ASSERT(is<NativeObject>());
  MOZ_ASSERT(isExtensible() || (containsDenseElement(index) && extra == 1));

  uint32_t requiredCapacity;
  if (extra == 1) {
    /* Optimize for the common case. */
    if (index < getDenseCapacity()) {
      ensureDenseInitializedLength(index, 1);
      return DenseElementResult::Success;
    }
    requiredCapacity = index + 1;
    if (requiredCapacity == 0) {
      /* Overflow. */
      return DenseElementResult::Incomplete;
    }
  } else {
    requiredCapacity = index + extra;
    if (requiredCapacity < index) {
      /* Overflow. */
      return DenseElementResult::Incomplete;
    }
    if (requiredCapacity <= getDenseCapacity()) {
      ensureDenseInitializedLength(index, extra);
      return DenseElementResult::Success;
    }
  }

  DenseElementResult result = extendDenseElements(cx, requiredCapacity, extra);
  if (result != DenseElementResult::Success) {
    return result;
  }

  ensureDenseInitializedLength(index, extra);
  return DenseElementResult::Success;
}

inline DenseElementResult NativeObject::setOrExtendDenseElements(
    JSContext* cx, uint32_t start, const Value* vp, uint32_t count) {
  if (!isExtensible()) {
    return DenseElementResult::Incomplete;
  }

  if (is<ArrayObject>() && !as<ArrayObject>().lengthIsWritable() &&
      start + count >= as<ArrayObject>().length()) {
    return DenseElementResult::Incomplete;
  }

  DenseElementResult result = ensureDenseElements(cx, start, count);
  if (result != DenseElementResult::Success) {
    return result;
  }

  if (is<ArrayObject>() && start + count >= as<ArrayObject>().length()) {
    as<ArrayObject>().setLengthToInitializedLength();
  }

  copyDenseElements(start, vp, count);
  return DenseElementResult::Success;
}

/* static */
inline NativeObject* NativeObject::create(
    JSContext* cx, js::gc::AllocKind kind, js::gc::Heap heap,
    js::Handle<SharedShape*> shape, js::gc::AllocSite* site /* = nullptr */) {
  debugCheckNewObject(shape, kind, heap);

  const JSClass* clasp = shape->getObjectClass();
  MOZ_ASSERT(clasp->isNativeObject());
  MOZ_ASSERT(!clasp->isJSFunction(), "should use JSFunction::create");
  MOZ_ASSERT(clasp != &ArrayObject::class_, "should use ArrayObject::create");

  const uint32_t nfixed = shape->numFixedSlots();
  const uint32_t slotSpan = shape->slotSpan();
  const size_t nDynamicSlots = calculateDynamicSlots(nfixed, slotSpan, clasp);

  NativeObject* nobj = cx->newCell<NativeObject>(kind, heap, clasp, site);
  if (!nobj) {
    return nullptr;
  }

  nobj->initShape(shape);
  nobj->setEmptyElements();

  if (!nDynamicSlots) {
    nobj->initEmptyDynamicSlots();
  } else if (!nobj->allocateInitialSlots(cx, nDynamicSlots)) {
    return nullptr;
  }

  if (slotSpan > 0) {
    nobj->initSlots(nfixed, slotSpan);
  }

  if (MOZ_UNLIKELY(cx->realm()->hasAllocationMetadataBuilder())) {
    if (clasp->shouldDelayMetadataBuilder()) {
      cx->realm()->setObjectPendingMetadata(nobj);
    } else {
      nobj = SetNewObjectMetadata(cx, nobj);
    }
  }

  js::gc::gcprobes::CreateObject(nobj);

  return nobj;
}

MOZ_ALWAYS_INLINE void NativeObject::initEmptyDynamicSlots() {
  setEmptyDynamicSlots(0);
}

MOZ_ALWAYS_INLINE void NativeObject::setDictionaryModeSlotSpan(uint32_t span) {
  MOZ_ASSERT(inDictionaryMode());

  if (!hasDynamicSlots()) {
    setEmptyDynamicSlots(span);
    return;
  }

  getSlotsHeader()->setDictionarySlotSpan(span);
}

MOZ_ALWAYS_INLINE void NativeObject::setEmptyDynamicSlots(
    uint32_t dictionarySlotSpan) {
  MOZ_ASSERT_IF(!inDictionaryMode(), dictionarySlotSpan == 0);
  MOZ_ASSERT(dictionarySlotSpan <= MAX_FIXED_SLOTS);

  slots_ = emptyObjectSlotsForDictionaryObject[dictionarySlotSpan];

  MOZ_ASSERT(getSlotsHeader()->capacity() == 0);
  MOZ_ASSERT(getSlotsHeader()->dictionarySlotSpan() == dictionarySlotSpan);
  MOZ_ASSERT(!hasDynamicSlots());
  MOZ_ASSERT(!hasUniqueId());
}

MOZ_ALWAYS_INLINE bool NativeObject::setShapeAndAddNewSlots(
    JSContext* cx, SharedShape* newShape, uint32_t oldSpan, uint32_t newSpan) {
  MOZ_ASSERT(!inDictionaryMode());
  MOZ_ASSERT(newShape->isShared());
  MOZ_ASSERT(newShape->zone() == zone());
  MOZ_ASSERT(newShape->numFixedSlots() == numFixedSlots());
  MOZ_ASSERT(newShape->getObjectClass() == getClass());

  MOZ_ASSERT(oldSpan < newSpan);
  MOZ_ASSERT(sharedShape()->slotSpan() == oldSpan);
  MOZ_ASSERT(newShape->slotSpan() == newSpan);

  uint32_t numFixed = newShape->numFixedSlots();
  if (newSpan > numFixed) {
    uint32_t oldCapacity = numDynamicSlots();
    uint32_t newCapacity =
        calculateDynamicSlots(numFixed, newSpan, newShape->getObjectClass());
    MOZ_ASSERT(oldCapacity <= newCapacity);

    if (oldCapacity < newCapacity) {
      if (MOZ_UNLIKELY(!growSlots(cx, oldCapacity, newCapacity))) {
        return false;
      }
    }
  }

  // Initialize slots [oldSpan, newSpan). Use the *Unchecked version because
  // the shape's slot span does not reflect the allocated slots at this
  // point.
  auto initRange = [](HeapSlot* start, HeapSlot* end) {
    for (HeapSlot* slot = start; slot < end; slot++) {
      slot->initAsUndefined();
    }
  };
  forEachSlotRangeUnchecked(oldSpan, newSpan, initRange);

  setShape(newShape);
  return true;
}

MOZ_ALWAYS_INLINE bool NativeObject::setShapeAndAddNewSlot(
    JSContext* cx, SharedShape* newShape, uint32_t slot) {
  MOZ_ASSERT(!inDictionaryMode());
  MOZ_ASSERT(newShape->isShared());
  MOZ_ASSERT(newShape->zone() == zone());
  MOZ_ASSERT(newShape->numFixedSlots() == numFixedSlots());

  MOZ_ASSERT(newShape->base() == shape()->base());
  MOZ_ASSERT(newShape->slotSpan() == sharedShape()->slotSpan() + 1);
  MOZ_ASSERT(newShape->slotSpan() == slot + 1);

  uint32_t numFixed = newShape->numFixedSlots();
  if (slot < numFixed) {
    initFixedSlot(slot, UndefinedValue());
  } else {
    uint32_t dynamicSlotIndex = slot - numFixed;
    if (dynamicSlotIndex >= numDynamicSlots()) {
      if (MOZ_UNLIKELY(!growSlotsForNewSlot(cx, numFixed, slot))) {
        return false;
      }
    }
    initDynamicSlot(numFixed, slot, UndefinedValue());
  }

  setShape(newShape);
  return true;
}

inline js::gc::AllocKind NativeObject::allocKindForTenure() const {
  using namespace js::gc;
  AllocKind kind = GetGCObjectFixedSlotsKind(numFixedSlots());
  MOZ_ASSERT(!IsFinalizedKind(kind));
  return GetFinalizedAllocKindForClass(kind, getClass());
}

inline js::GlobalObject& NativeObject::global() const { return nonCCWGlobal(); }

inline bool NativeObject::denseElementsHaveMaybeInIterationFlag() {
  if (!getElementsHeader()->maybeInIteration()) {
    AssertDenseElementsNotIterated(this);
    return false;
  }
  return true;
}

inline bool NativeObject::denseElementsMaybeInIteration() {
  if (!denseElementsHaveMaybeInIterationFlag()) {
    return false;
  }
  return compartment()->objectMaybeInIteration(this);
}

/*
 * Call obj's resolve hook.
 *
 * cx and id are the parameters initially passed to the ongoing lookup;
 * propp and recursedp are its out parameters.
 *
 * There are four possible outcomes:
 *
 *  - On failure, report an error or exception and return false.
 *
 *  - If we are already resolving a property of obj, call setRecursiveResolve on
 *    propp and return true.
 *
 *  - If the resolve hook finds or defines the sought property, set propp
 *    appropriately, and return true.
 *
 *  - Otherwise no property was resolved. Set propp to NotFound and return true.
 */
static MOZ_ALWAYS_INLINE bool CallResolveOp(JSContext* cx,
                                            Handle<NativeObject*> obj,
                                            HandleId id,
                                            PropertyResult* propp) {
  // Avoid recursion on (obj, id) already being resolved on cx.
  AutoResolving resolving(cx, obj, id);
  if (resolving.alreadyStarted()) {
    // Already resolving id in obj, suppress recursion.
    propp->setRecursiveResolve();
    return true;
  }

  bool resolved = false;
  AutoRealm ar(cx, obj);
  if (!obj->getClass()->getResolve()(cx, obj, id, &resolved)) {
    return false;
  }

  if (!resolved) {
    propp->setNotFound();
    return true;
  }

  // Assert the mayResolve hook, if there is one, returns true for this
  // property.
  MOZ_ASSERT_IF(obj->getClass()->getMayResolve(),
                obj->getClass()->getMayResolve()(cx->names(), id, obj));

  if (id.isInt()) {
    uint32_t index = id.toInt();
    if (obj->containsDenseElement(index)) {
      propp->setDenseElement(index);
      return true;
    }
  }

  MOZ_ASSERT(!obj->is<TypedArrayObject>());

  mozilla::Maybe<PropertyInfo> prop = obj->lookup(cx, id);
  if (prop.isSome()) {
    propp->setNativeProperty(*prop);
  } else {
    propp->setNotFound();
  }

  return true;
}

enum class LookupResolveMode {
  IgnoreResolve,
  CheckResolve,
  CheckMayResolve,
};

template <AllowGC allowGC,
          LookupResolveMode resolveMode = LookupResolveMode::CheckResolve>
static MOZ_ALWAYS_INLINE bool NativeLookupOwnPropertyInline(
    JSContext* cx, typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
    typename MaybeRooted<jsid, allowGC>::HandleType id, PropertyResult* propp) {
  // Native objects should should avoid `lookupProperty` hooks, and those that
  // use them should avoid recursively triggering lookup, and those that still
  // violate this guidance are the ModuleEnvironmentObject.
  MOZ_ASSERT_IF(obj->getOpsLookupProperty(),
                obj->template is<ModuleEnvironmentObject>());

  // Check for a native dense element.
  if (id.isInt()) {
    uint32_t index = id.toInt();
    if (obj->containsDenseElement(index)) {
      propp->setDenseElement(index);
      return true;
    }
  }

  // Check for a typed array element. Integer lookups always finish here
  // so that integer properties on the prototype are ignored even for out
  // of bounds accesses.
  if (obj->template is<TypedArrayObject>()) {
    if (mozilla::Maybe<uint64_t> index = ToTypedArrayIndex(id)) {
      uint64_t idx = index.value();
      if (idx < obj->template as<TypedArrayObject>().length().valueOr(0)) {
        propp->setTypedArrayElement(idx);
      } else {
        propp->setTypedArrayOutOfRange();
      }
      return true;
    }
  }

  MOZ_ASSERT(cx->compartment() == obj->compartment());

  // Check for a native property. Call Shape::lookup directly (instead of
  // NativeObject::lookup) because it's inlined.
  uint32_t index;
  if (PropMap* map = obj->shape()->lookup(cx, id, &index)) {
    propp->setNativeProperty(map->getPropertyInfo(index));
    return true;
  }

  // Some callers explicitily want us to ignore the resolve hook entirely. In
  // that case, we report the property as NotFound.
  if constexpr (resolveMode == LookupResolveMode::IgnoreResolve) {
    propp->setNotFound();
    return true;
  }

  // JITs in particular use the `mayResolve` hook to determine a JSClass can
  // never resolve this property name (for all instances of the class).
  if constexpr (resolveMode == LookupResolveMode::CheckMayResolve) {
    static_assert(allowGC == false,
                  "CheckMayResolve can only be used with NoGC");

    MOZ_ASSERT(propp->isNotFound());
    return !ClassMayResolveId(cx->names(), obj->getClass(), id, obj);
  }

  MOZ_ASSERT(resolveMode == LookupResolveMode::CheckResolve);

  // If there is no resolve hook, the property definitely does not exist.
  if (obj->getClass()->getResolve()) {
    if constexpr (!allowGC) {
      return false;
    } else {
      return CallResolveOp(cx, obj, id, propp);
    }
  }

  propp->setNotFound();
  return true;
}

/*
 * Simplified version of NativeLookupOwnPropertyInline that doesn't call
 * resolve hooks.
 */
[[nodiscard]] static inline bool NativeLookupOwnPropertyNoResolve(
    JSContext* cx, NativeObject* obj, jsid id, PropertyResult* result) {
  return NativeLookupOwnPropertyInline<NoGC, LookupResolveMode::IgnoreResolve>(
      cx, obj, id, result);
}

template <AllowGC allowGC,
          LookupResolveMode resolveMode = LookupResolveMode::CheckResolve>
static MOZ_ALWAYS_INLINE bool NativeLookupPropertyInline(
    JSContext* cx, typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
    typename MaybeRooted<jsid, allowGC>::HandleType id,
    typename MaybeRooted<
        std::conditional_t<allowGC == AllowGC::CanGC, JSObject*, NativeObject*>,
        allowGC>::MutableHandleType objp,
    PropertyResult* propp) {
  /* Search scopes starting with obj and following the prototype link. */
  typename MaybeRooted<NativeObject*, allowGC>::RootType current(cx, obj);

  while (true) {
    if (!NativeLookupOwnPropertyInline<allowGC, resolveMode>(cx, current, id,
                                                             propp)) {
      return false;
    }

    if (propp->isFound()) {
      objp.set(current);
      return true;
    }

    if (propp->shouldIgnoreProtoChain()) {
      break;
    }

    JSObject* proto = current->staticPrototype();
    if (!proto) {
      break;
    }

    // If a `lookupProperty` hook exists, recurse into LookupProperty, otherwise
    // we can simply loop within this call frame.
    if (proto->getOpsLookupProperty()) {
      if constexpr (allowGC) {
        RootedObject protoRoot(cx, proto);
        return LookupProperty(cx, protoRoot, id, objp, propp);
      } else {
        return false;
      }
    }

    current = &proto->as<NativeObject>();
  }

  MOZ_ASSERT(propp->isNotFound());
  objp.set(nullptr);
  return true;
}

inline bool ThrowIfNotConstructing(JSContext* cx, const CallArgs& args,
                                   const char* builtinName) {
  if (args.isConstructing()) {
    return true;
  }
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_BUILTIN_CTOR_NO_NEW, builtinName);
  return false;
}

inline bool IsPackedArray(JSObject* obj) {
  if (!obj->is<ArrayObject>()) {
    return false;
  }

  ArrayObject* arr = &obj->as<ArrayObject>();
  if (arr->getDenseInitializedLength() != arr->length()) {
    return false;
  }

  if (!arr->denseElementsArePacked()) {
    return false;
  }

#ifdef DEBUG
  // Assert correctness of the NON_PACKED flag by checking the first few
  // elements don't contain holes.
  uint32_t numToCheck = std::min<uint32_t>(5, arr->getDenseInitializedLength());
  for (uint32_t i = 0; i < numToCheck; i++) {
    MOZ_ASSERT(!arr->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE));
  }
#endif

  return true;
}

// Like AddDataProperty but optimized for plain objects. Plain objects don't
// have an addProperty hook.
MOZ_ALWAYS_INLINE bool AddDataPropertyToPlainObject(
    JSContext* cx, Handle<PlainObject*> obj, HandleId id, HandleValue v,
    uint32_t* resultSlot = nullptr) {
  MOZ_ASSERT(!id.isInt());

  uint32_t slot;
  if (!resultSlot) {
    resultSlot = &slot;
  }
  if (!NativeObject::addProperty(
          cx, obj, id, PropertyFlags::defaultDataPropFlags, resultSlot)) {
    return false;
  }

  obj->initSlot(*resultSlot, v);

  MOZ_ASSERT(!obj->getClass()->getAddProperty());
  return true;
}

}  // namespace js

#endif /* vm_NativeObject_inl_h */
