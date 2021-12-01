/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/NativeObject-inl.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"

#include "gc/Marking.h"
#include "js/Value.h"
#include "vm/Debugger.h"
#include "vm/TypedArrayObject.h"

#include "gc/Nursery-inl.h"
#include "vm/ArrayObject-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

using JS::AutoCheckCannotGC;
using mozilla::ArrayLength;
using mozilla::CheckedInt;
using mozilla::DebugOnly;
using mozilla::PodCopy;
using mozilla::RoundUpPow2;

struct EmptyObjectElements
{
    const ObjectElements emptyElementsHeader;

    // Add an extra (unused) Value to make sure an out-of-bounds index when
    // masked (resulting in index 0) accesses valid memory.
    const Value val;

  public:
    constexpr EmptyObjectElements()
      : emptyElementsHeader(0, 0),
        val(UndefinedValue())
    {}
    explicit constexpr EmptyObjectElements(ObjectElements::SharedMemory shmem)
      : emptyElementsHeader(0, 0, shmem),
        val(UndefinedValue())
    {}
};

static constexpr EmptyObjectElements emptyElementsHeader;

/* Objects with no elements share one empty set of elements. */
HeapSlot* const js::emptyObjectElements =
    reinterpret_cast<HeapSlot*>(uintptr_t(&emptyElementsHeader) + sizeof(ObjectElements));

static constexpr
EmptyObjectElements emptyElementsHeaderShared(ObjectElements::SharedMemory::IsShared);

/* Objects with no elements share one empty set of elements. */
HeapSlot* const js::emptyObjectElementsShared =
    reinterpret_cast<HeapSlot*>(uintptr_t(&emptyElementsHeaderShared) + sizeof(ObjectElements));


#ifdef DEBUG

bool
NativeObject::canHaveNonEmptyElements()
{
    return !this->is<TypedArrayObject>();
}

#endif // DEBUG

/* static */ bool
ObjectElements::ConvertElementsToDoubles(JSContext* cx, uintptr_t elementsPtr)
{
    /*
     * This function is infallible, but has a fallible interface so that it can
     * be called directly from Ion code. Only arrays can have their dense
     * elements converted to doubles, and arrays never have empty elements.
     */
    HeapSlot* elementsHeapPtr = (HeapSlot*) elementsPtr;
    MOZ_ASSERT(elementsHeapPtr != emptyObjectElements &&
               elementsHeapPtr != emptyObjectElementsShared);

    ObjectElements* header = ObjectElements::fromElements(elementsHeapPtr);
    MOZ_ASSERT(!header->shouldConvertDoubleElements());

    // Note: the elements can be mutated in place even for copy on write
    // arrays. See comment on ObjectElements.
    Value* vp = (Value*) elementsPtr;
    for (size_t i = 0; i < header->initializedLength; i++) {
        if (vp[i].isInt32())
            vp[i].setDouble(vp[i].toInt32());
    }

    header->setShouldConvertDoubleElements();
    return true;
}

/* static */ bool
ObjectElements::MakeElementsCopyOnWrite(JSContext* cx, NativeObject* obj)
{
    static_assert(sizeof(HeapSlot) >= sizeof(GCPtrObject),
                  "there must be enough room for the owner object pointer at "
                  "the end of the elements");
    if (!obj->ensureElements(cx, obj->getDenseInitializedLength() + 1))
        return false;

    ObjectElements* header = obj->getElementsHeader();

    // Note: this method doesn't update type information to indicate that the
    // elements might be copy on write. Handling this is left to the caller.
    MOZ_ASSERT(!header->isCopyOnWrite());
    MOZ_ASSERT(!header->isFrozen());
    header->flags |= COPY_ON_WRITE;

    header->ownerObject().init(obj);
    return true;
}

/* static */ bool
ObjectElements::FreezeElements(JSContext* cx, HandleNativeObject obj)
{
    MOZ_ASSERT_IF(obj->is<ArrayObject>(),
                  !obj->as<ArrayObject>().lengthIsWritable());

    if (!obj->maybeCopyElementsForWrite(cx))
        return false;

    if (obj->hasEmptyElements() || obj->denseElementsAreFrozen())
        return true;

    if (obj->getElementsHeader()->numShiftedElements() > 0)
        obj->moveShiftedElements();

    MarkObjectGroupFlags(cx, obj, OBJECT_FLAG_FROZEN_ELEMENTS);
    obj->getElementsHeader()->freeze();

    return true;
}

#ifdef DEBUG
static mozilla::Atomic<bool, mozilla::Relaxed> gShapeConsistencyChecksEnabled(false);

/* static */ void
js::NativeObject::enableShapeConsistencyChecks()
{
    gShapeConsistencyChecksEnabled = true;
}

void
js::NativeObject::checkShapeConsistency()
{
    if (!gShapeConsistencyChecksEnabled)
        return;

    MOZ_ASSERT(isNative());

    Shape* shape = lastProperty();
    Shape* prev = nullptr;

    AutoCheckCannotGC nogc;
    if (inDictionaryMode()) {
        if (ShapeTable* table = shape->maybeTable(nogc)) {
            for (uint32_t fslot = table->freeList();
                 fslot != SHAPE_INVALID_SLOT;
                 fslot = getSlot(fslot).toPrivateUint32())
            {
                MOZ_ASSERT(fslot < slotSpan());
            }

            while (shape->parent) {
                MOZ_ASSERT_IF(lastProperty() != shape, !shape->hasTable());

                ShapeTable::Entry& entry = table->search<MaybeAdding::NotAdding>(shape->propid(),
                                                                                 nogc);
                MOZ_ASSERT(entry.shape() == shape);
                shape = shape->parent;
            }
        }

        shape = lastProperty();
        while (shape) {
            MOZ_ASSERT_IF(!shape->isEmptyShape() && shape->isDataProperty(),
                          shape->slot() < slotSpan());
            if (!prev) {
                MOZ_ASSERT(lastProperty() == shape);
                MOZ_ASSERT(shape->listp == &shapeRef());
            } else {
                MOZ_ASSERT(shape->listp == &prev->parent);
            }
            prev = shape;
            shape = shape->parent;
        }
    } else {
        while (shape->parent) {
            if (ShapeTable* table = shape->maybeTable(nogc)) {
                MOZ_ASSERT(shape->parent);
                for (Shape::Range<NoGC> r(shape); !r.empty(); r.popFront()) {
                    ShapeTable::Entry& entry =
                        table->search<MaybeAdding::NotAdding>(r.front().propid(), nogc);
                    MOZ_ASSERT(entry.shape() == &r.front());
                }
            }
            if (prev) {
                MOZ_ASSERT_IF(shape->isDataProperty(), prev->maybeSlot() >= shape->maybeSlot());
                shape->kids.checkConsistency(prev);
            }
            prev = shape;
            shape = shape->parent;
        }
    }
}
#endif

void
js::NativeObject::initializeSlotRange(uint32_t start, uint32_t length)
{
    /*
     * No bounds check, as this is used when the object's shape does not
     * reflect its allocated slots (updateSlotsForSpan).
     */
    HeapSlot* fixedStart;
    HeapSlot* fixedEnd;
    HeapSlot* slotsStart;
    HeapSlot* slotsEnd;
    getSlotRangeUnchecked(start, length, &fixedStart, &fixedEnd, &slotsStart, &slotsEnd);

    uint32_t offset = start;
    for (HeapSlot* sp = fixedStart; sp < fixedEnd; sp++)
        sp->init(this, HeapSlot::Slot, offset++, UndefinedValue());
    for (HeapSlot* sp = slotsStart; sp < slotsEnd; sp++)
        sp->init(this, HeapSlot::Slot, offset++, UndefinedValue());
}

void
js::NativeObject::initSlotRange(uint32_t start, const Value* vector, uint32_t length)
{
    HeapSlot* fixedStart;
    HeapSlot* fixedEnd;
    HeapSlot* slotsStart;
    HeapSlot* slotsEnd;
    getSlotRange(start, length, &fixedStart, &fixedEnd, &slotsStart, &slotsEnd);
    for (HeapSlot* sp = fixedStart; sp < fixedEnd; sp++)
        sp->init(this, HeapSlot::Slot, start++, *vector++);
    for (HeapSlot* sp = slotsStart; sp < slotsEnd; sp++)
        sp->init(this, HeapSlot::Slot, start++, *vector++);
}

void
js::NativeObject::copySlotRange(uint32_t start, const Value* vector, uint32_t length)
{
    HeapSlot* fixedStart;
    HeapSlot* fixedEnd;
    HeapSlot* slotsStart;
    HeapSlot* slotsEnd;
    getSlotRange(start, length, &fixedStart, &fixedEnd, &slotsStart, &slotsEnd);
    for (HeapSlot* sp = fixedStart; sp < fixedEnd; sp++)
        sp->set(this, HeapSlot::Slot, start++, *vector++);
    for (HeapSlot* sp = slotsStart; sp < slotsEnd; sp++)
        sp->set(this, HeapSlot::Slot, start++, *vector++);
}

#ifdef DEBUG
bool
js::NativeObject::slotInRange(uint32_t slot, SentinelAllowed sentinel) const
{
    uint32_t capacity = numFixedSlots() + numDynamicSlots();
    if (sentinel == SENTINEL_ALLOWED)
        return slot <= capacity;
    return slot < capacity;
}
#endif /* DEBUG */

Shape*
js::NativeObject::lookup(JSContext* cx, jsid id)
{
    MOZ_ASSERT(isNative());
    return Shape::search(cx, lastProperty(), id);
}

Shape*
js::NativeObject::lookupPure(jsid id)
{
    MOZ_ASSERT(isNative());
    return Shape::searchNoHashify(lastProperty(), id);
}

uint32_t
js::NativeObject::numFixedSlotsForCompilation() const
{
    // This is an alternative method for getting the number of fixed slots in an
    // object. It requires more logic and memory accesses than numFixedSlots()
    // but is safe to be called from the compilation thread, even if the active
    // thread is mutating the VM.

    // The compiler does not have access to nursery things.
    MOZ_ASSERT(!IsInsideNursery(this));

    if (this->is<ArrayObject>())
        return 0;

    gc::AllocKind kind = asTenured().getAllocKind();
    return gc::GetGCKindSlots(kind, getClass());
}

void
NativeObject::setLastPropertyShrinkFixedSlots(Shape* shape)
{
    MOZ_ASSERT(!inDictionaryMode());
    MOZ_ASSERT(!shape->inDictionary());
    MOZ_ASSERT(shape->zone() == zone());
    MOZ_ASSERT(lastProperty()->slotSpan() == shape->slotSpan());
    MOZ_ASSERT(shape->getObjectClass() == getClass());

    DebugOnly<size_t> oldFixed = numFixedSlots();
    DebugOnly<size_t> newFixed = shape->numFixedSlots();
    MOZ_ASSERT(newFixed < oldFixed);
    MOZ_ASSERT(shape->slotSpan() <= oldFixed);
    MOZ_ASSERT(shape->slotSpan() <= newFixed);
    MOZ_ASSERT(dynamicSlotsCount(oldFixed, shape->slotSpan(), getClass()) == 0);
    MOZ_ASSERT(dynamicSlotsCount(newFixed, shape->slotSpan(), getClass()) == 0);

    setShape(shape);
}

void
NativeObject::setLastPropertyMakeNonNative(Shape* shape)
{
    MOZ_ASSERT(!inDictionaryMode());
    MOZ_ASSERT(!shape->getObjectClass()->isNative());
    MOZ_ASSERT(shape->zone() == zone());
    MOZ_ASSERT(shape->slotSpan() == 0);
    MOZ_ASSERT(shape->numFixedSlots() == 0);

    if (hasDynamicElements())
        js_free(getUnshiftedElementsHeader());
    if (hasDynamicSlots()) {
        js_free(slots_);
        slots_ = nullptr;
    }

    setShape(shape);
}

void
NativeObject::setLastPropertyMakeNative(JSContext* cx, Shape* shape)
{
    MOZ_ASSERT(getClass()->isNative());
    MOZ_ASSERT(shape->getObjectClass()->isNative());
    MOZ_ASSERT(!shape->inDictionary());

    // This method is used to convert unboxed objects into native objects. In
    // this case, the shape_ field was previously used to store other data and
    // this should be treated as an initialization.
    initShape(shape);

    slots_ = nullptr;
    elements_ = emptyObjectElements;

    size_t oldSpan = shape->numFixedSlots();
    size_t newSpan = shape->slotSpan();

    initializeSlotRange(0, oldSpan);

    // A failure at this point will leave the object as a mutant, and we
    // can't recover.
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (oldSpan != newSpan && !updateSlotsForSpan(cx, oldSpan, newSpan))
        oomUnsafe.crash("NativeObject::setLastPropertyMakeNative");
}

bool
NativeObject::setSlotSpan(JSContext* cx, uint32_t span)
{
    MOZ_ASSERT(inDictionaryMode());

    size_t oldSpan = lastProperty()->base()->slotSpan();
    if (oldSpan == span)
        return true;

    if (!updateSlotsForSpan(cx, oldSpan, span))
        return false;

    lastProperty()->base()->setSlotSpan(span);
    return true;
}

bool
NativeObject::growSlots(JSContext* cx, uint32_t oldCount, uint32_t newCount)
{
    MOZ_ASSERT(newCount > oldCount);
    MOZ_ASSERT_IF(!is<ArrayObject>(), newCount >= SLOT_CAPACITY_MIN);

    /*
     * Slot capacities are determined by the span of allocated objects. Due to
     * the limited number of bits to store shape slots, object growth is
     * throttled well before the slot capacity can overflow.
     */
    NativeObject::slotsSizeMustNotOverflow();
    MOZ_ASSERT(newCount <= MAX_SLOTS_COUNT);

    if (!oldCount) {
        MOZ_ASSERT(!slots_);
        slots_ = AllocateObjectBuffer<HeapSlot>(cx, this, newCount);
        if (!slots_)
            return false;
        Debug_SetSlotRangeToCrashOnTouch(slots_, newCount);
        return true;
    }

    HeapSlot* newslots = ReallocateObjectBuffer<HeapSlot>(cx, this, slots_, oldCount, newCount);
    if (!newslots)
        return false;  /* Leave slots at its old size. */

    slots_ = newslots;

    Debug_SetSlotRangeToCrashOnTouch(slots_ + oldCount, newCount - oldCount);

    return true;
}

/* static */ bool
NativeObject::growSlotsDontReportOOM(JSContext* cx, NativeObject* obj, uint32_t newCount)
{
    // IC code calls this directly.
    AutoUnsafeCallWithABI unsafe;

    if (!obj->growSlots(cx, obj->numDynamicSlots(), newCount)) {
        cx->recoverFromOutOfMemory();
        return false;
    }
    return true;
}

/* static */ bool
NativeObject::addDenseElementDontReportOOM(JSContext* cx, NativeObject* obj)
{
    // IC code calls this directly.
    AutoUnsafeCallWithABI unsafe;

    MOZ_ASSERT(obj->getDenseInitializedLength() == obj->getDenseCapacity());
    MOZ_ASSERT(!obj->denseElementsAreCopyOnWrite());
    MOZ_ASSERT(!obj->denseElementsAreFrozen());
    MOZ_ASSERT(!obj->isIndexed());
    MOZ_ASSERT(!obj->is<TypedArrayObject>());
    MOZ_ASSERT_IF(obj->is<ArrayObject>(), obj->as<ArrayObject>().lengthIsWritable());

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

static void
FreeSlots(JSContext* cx, HeapSlot* slots)
{
    if (cx->helperThread())
        js_free(slots);
    else
        cx->nursery().freeBuffer(slots);
}

void
NativeObject::shrinkSlots(JSContext* cx, uint32_t oldCount, uint32_t newCount)
{
    MOZ_ASSERT(newCount < oldCount);

    if (newCount == 0) {
        FreeSlots(cx, slots_);
        slots_ = nullptr;
        return;
    }

    MOZ_ASSERT_IF(!is<ArrayObject>(), newCount >= SLOT_CAPACITY_MIN);

    HeapSlot* newslots = ReallocateObjectBuffer<HeapSlot>(cx, this, slots_, oldCount, newCount);
    if (!newslots) {
        cx->recoverFromOutOfMemory();
        return;  /* Leave slots at its old size. */
    }

    slots_ = newslots;
}

/* static */ bool
NativeObject::sparsifyDenseElement(JSContext* cx, HandleNativeObject obj, uint32_t index)
{
    if (!obj->maybeCopyElementsForWrite(cx))
        return false;

    RootedValue value(cx, obj->getDenseElement(index));
    MOZ_ASSERT(!value.isMagic(JS_ELEMENTS_HOLE));

    removeDenseElementForSparseIndex(cx, obj, index);

    RootedId id(cx, INT_TO_JSID(index));

    AutoKeepShapeTables keep(cx);
    ShapeTable* table = nullptr;
    ShapeTable::Entry* entry = nullptr;
    if (obj->inDictionaryMode()) {
        table = obj->lastProperty()->ensureTableForDictionary(cx, keep);
        if (!table)
            return false;
        entry = &table->search<MaybeAdding::Adding>(id, keep);
    }

    // NOTE: We don't use addDataProperty because we don't want the
    // extensibility check if we're, for example, sparsifying frozen objects..
    Shape* shape = addDataPropertyInternal(cx, obj, id, SHAPE_INVALID_SLOT,
                                           obj->getElementsHeader()->elementAttributes(),
                                           table, entry, keep);
    if (!shape) {
        obj->setDenseElementUnchecked(index, value);
        return false;
    }

    obj->initSlot(shape->slot(), value);

    return true;
}

/* static */ bool
NativeObject::sparsifyDenseElements(JSContext* cx, HandleNativeObject obj)
{
    if (!obj->maybeCopyElementsForWrite(cx))
        return false;

    uint32_t initialized = obj->getDenseInitializedLength();

    /* Create new properties with the value of non-hole dense elements. */
    for (uint32_t i = 0; i < initialized; i++) {
        if (obj->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE))
            continue;

        if (!sparsifyDenseElement(cx, obj, i))
            return false;
    }

    if (initialized)
        obj->setDenseInitializedLengthUnchecked(0);

    // Reduce storage for dense elements which are now holes. Explicitly mark
    // the elements capacity as zero, so that any attempts to add dense
    // elements will be caught in ensureDenseElements.

    if (obj->getElementsHeader()->numShiftedElements() > 0)
        obj->moveShiftedElements();

    if (obj->getDenseCapacity()) {
        obj->shrinkElements(cx, 0);
        obj->getElementsHeader()->capacity = 0;
    }

    return true;
}

bool
NativeObject::willBeSparseElements(uint32_t requiredCapacity, uint32_t newElementsHint)
{
    MOZ_ASSERT(isNative());
    MOZ_ASSERT(requiredCapacity > MIN_SPARSE_INDEX);

    uint32_t cap = getDenseCapacity();
    MOZ_ASSERT(requiredCapacity >= cap);

    if (requiredCapacity > MAX_DENSE_ELEMENTS_COUNT)
        return true;

    uint32_t minimalDenseCount = requiredCapacity / SPARSE_DENSITY_RATIO;
    if (newElementsHint >= minimalDenseCount)
        return false;
    minimalDenseCount -= newElementsHint;

    if (minimalDenseCount > cap)
        return true;

    uint32_t len = getDenseInitializedLength();
    const Value* elems = getDenseElements();
    for (uint32_t i = 0; i < len; i++) {
        if (!elems[i].isMagic(JS_ELEMENTS_HOLE) && !--minimalDenseCount)
            return false;
    }
    return true;
}

/* static */ DenseElementResult
NativeObject::maybeDensifySparseElements(JSContext* cx, HandleNativeObject obj)
{
    /*
     * Wait until after the object goes into dictionary mode, which must happen
     * when sparsely packing any array with more than MIN_SPARSE_INDEX elements
     * (see PropertyTree::MAX_HEIGHT).
     */
    if (!obj->inDictionaryMode())
        return DenseElementResult::Incomplete;

    /*
     * Only measure the number of indexed properties every log(n) times when
     * populating the object.
     */
    uint32_t slotSpan = obj->slotSpan();
    if (slotSpan != RoundUpPow2(slotSpan))
        return DenseElementResult::Incomplete;

    /* Watch for conditions under which an object's elements cannot be dense. */
    if (!obj->nonProxyIsExtensible())
        return DenseElementResult::Incomplete;

    /*
     * The indexes in the object need to be sufficiently dense before they can
     * be converted to dense mode.
     */
    uint32_t numDenseElements = 0;
    uint32_t newInitializedLength = 0;

    RootedShape shape(cx, obj->lastProperty());
    while (!shape->isEmptyShape()) {
        uint32_t index;
        if (IdIsIndex(shape->propid(), &index)) {
            if (shape->attributes() == JSPROP_ENUMERATE &&
                shape->hasDefaultGetter() &&
                shape->hasDefaultSetter())
            {
                numDenseElements++;
                newInitializedLength = Max(newInitializedLength, index + 1);
            } else {
                /*
                 * For simplicity, only densify the object if all indexed
                 * properties can be converted to dense elements.
                 */
                return DenseElementResult::Incomplete;
            }
        }
        shape = shape->previous();
    }

    if (numDenseElements * SPARSE_DENSITY_RATIO < newInitializedLength)
        return DenseElementResult::Incomplete;

    if (newInitializedLength > MAX_DENSE_ELEMENTS_COUNT)
        return DenseElementResult::Incomplete;

    /*
     * This object meets all necessary restrictions, convert all indexed
     * properties into dense elements.
     */

    if (!obj->maybeCopyElementsForWrite(cx))
        return DenseElementResult::Failure;

    if (newInitializedLength > obj->getDenseCapacity()) {
        if (!obj->growElements(cx, newInitializedLength))
            return DenseElementResult::Failure;
    }

    obj->ensureDenseInitializedLength(cx, newInitializedLength, 0);

    RootedValue value(cx);

    shape = obj->lastProperty();
    while (!shape->isEmptyShape()) {
        jsid id = shape->propid();
        uint32_t index;
        if (IdIsIndex(id, &index)) {
            value = obj->getSlot(shape->slot());

            /*
             * When removing a property from a dictionary, the specified
             * property will be removed from the dictionary list and the
             * last property will then be changed due to reshaping the object.
             * Compute the next shape in the traverse, watching for such
             * removals from the list.
             */
            if (shape != obj->lastProperty()) {
                shape = shape->previous();
                if (!NativeObject::removeProperty(cx, obj, id))
                    return DenseElementResult::Failure;
            } else {
                if (!NativeObject::removeProperty(cx, obj, id))
                    return DenseElementResult::Failure;
                shape = obj->lastProperty();
            }

            obj->setDenseElement(index, value);
        } else {
            shape = shape->previous();
        }
    }

    /*
     * All indexed properties on the object are now dense, clear the indexed
     * flag so that we will not start using sparse indexes again if we need
     * to grow the object.
     */
    if (!NativeObject::clearFlag(cx, obj, BaseShape::INDEXED))
        return DenseElementResult::Failure;

    return DenseElementResult::Success;
}

void
NativeObject::moveShiftedElements()
{
    ObjectElements* header = getElementsHeader();
    uint32_t numShifted = header->numShiftedElements();
    MOZ_ASSERT(numShifted > 0);

    uint32_t initLength = header->initializedLength;

    ObjectElements* newHeader = static_cast<ObjectElements*>(getUnshiftedElementsHeader());
    memmove(newHeader, header, sizeof(ObjectElements));

    newHeader->clearShiftedElements();
    newHeader->capacity += numShifted;
    elements_ = newHeader->elements();

    // To move the elements, temporarily update initializedLength to include
    // the shifted elements.
    newHeader->initializedLength += numShifted;

    // Move the elements. Initialize to |undefined| to ensure pre-barriers
    // don't see garbage.
    for (size_t i = 0; i < numShifted; i++)
        initDenseElement(i, UndefinedValue());
    moveDenseElements(0, numShifted, initLength);

    // Restore the initialized length. We use setDenseInitializedLength to
    // make sure prepareElementRangeForOverwrite is called on the shifted
    // elements.
    setDenseInitializedLength(initLength);
}

void
NativeObject::maybeMoveShiftedElements()
{
    ObjectElements* header = getElementsHeader();
    MOZ_ASSERT(header->numShiftedElements() > 0);

    // Move the elements if less than a third of the allocated space is in use.
    if (header->capacity < header->numAllocatedElements() / 3)
        moveShiftedElements();
}

bool
NativeObject::tryUnshiftDenseElements(uint32_t count)
{
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
            header->isCopyOnWrite() ||
            header->isFrozen() ||
            header->hasNonwritableArrayLength() ||
            MOZ_UNLIKELY(count > ObjectElements::MaxShiftedElements))
        {
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
        if (toShift > unusedCapacity)
            return false;

        // Move more elements than we need, so that other unshift calls will be
        // fast. We just have to make sure we don't exceed unusedCapacity.
        toShift = Min(toShift + unusedCapacity / 2, unusedCapacity);

        // Ensure |numShifted + toShift| does not exceed MaxShiftedElements.
        if (numShifted + toShift > ObjectElements::MaxShiftedElements)
            toShift = ObjectElements::MaxShiftedElements - numShifted;

        MOZ_ASSERT(count <= numShifted + toShift);
        MOZ_ASSERT(numShifted + toShift <= ObjectElements::MaxShiftedElements);
        MOZ_ASSERT(toShift <= unusedCapacity);

        // Now move/unshift the elements.
        uint32_t initLen = header->initializedLength;
        setDenseInitializedLength(initLen + toShift);
        for (uint32_t i = 0; i < toShift; i++)
            initDenseElement(initLen + i, UndefinedValue());
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
    for (uint32_t i = 0; i < count; i++)
        initDenseElement(i, UndefinedValue());

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
//   * provide at least SLOT_CAPACITY_MIN elements no matter what (so adding
//     the first several elements to small arrays only needs one allocation).
//
// Note: the structure and behavior of this method follow along with
// UnboxedArrayObject::chooseCapacityIndex. Changes to the allocation strategy
// in one should generally be matched by the other.
/* static */ bool
NativeObject::goodElementsAllocationAmount(JSContext* cx, uint32_t reqCapacity,
                                           uint32_t length, uint32_t* goodAmount)
{
    if (reqCapacity > MAX_DENSE_ELEMENTS_COUNT) {
        ReportOutOfMemory(cx);
        return false;
    }

    uint32_t reqAllocated = reqCapacity + ObjectElements::VALUES_PER_HEADER;

    // Handle "small" requests primarily by doubling.
    const uint32_t Mebi = 1 << 20;
    if (reqAllocated < Mebi) {
        uint32_t amount = mozilla::AssertedCast<uint32_t>(RoundUpPow2(reqAllocated));

        // If |amount| would be 2/3 or more of the array's length, adjust
        // it (up or down) to be equal to the array's length.  This avoids
        // allocating excess elements that aren't likely to be needed, either
        // in this resizing or a subsequent one.  The 2/3 factor is chosen so
        // that exceptional resizings will at most triple the capacity, as
        // opposed to the usual doubling.
        uint32_t goodCapacity = amount - ObjectElements::VALUES_PER_HEADER;
        if (length >= reqCapacity && goodCapacity > (length / 3) * 2)
            amount = length + ObjectElements::VALUES_PER_HEADER;

        if (amount < SLOT_CAPACITY_MIN)
            amount = SLOT_CAPACITY_MIN;

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
    static const uint32_t BigBuckets[] = {
        0x100000, 0x200000, 0x300000, 0x400000, 0x500000, 0x600000, 0x700000,
        0x800000, 0x900000, 0xb00000, 0xd00000, 0xf00000, 0x1100000, 0x1400000,
        0x1700000, 0x1a00000, 0x1e00000, 0x2200000, 0x2700000, 0x2c00000,
        0x3200000, 0x3900000, 0x4100000, 0x4a00000, 0x5400000, 0x5f00000,
        0x6b00000, 0x7900000, 0x8900000, 0x9b00000, 0xaf00000, 0xc500000,
        0xde00000, 0xfa00000
    };
    MOZ_ASSERT(BigBuckets[ArrayLength(BigBuckets) - 1] <= MAX_DENSE_ELEMENTS_ALLOCATION);

    // Pick the first bucket that'll fit |reqAllocated|.
    for (uint32_t b : BigBuckets) {
        if (b >= reqAllocated) {
            *goodAmount = b;
            return true;
        }
    }

    // Otherwise, return the maximum bucket size.
    *goodAmount = MAX_DENSE_ELEMENTS_ALLOCATION;
    return true;
}

bool
NativeObject::growElements(JSContext* cx, uint32_t reqCapacity)
{
    MOZ_ASSERT(nonProxyIsExtensible());
    MOZ_ASSERT(canHaveNonEmptyElements());
    MOZ_ASSERT(!denseElementsAreFrozen());
    if (denseElementsAreCopyOnWrite())
        MOZ_CRASH();

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

        if (getElementsHeader()->initializedLength <= MaxElementsToMoveEagerly)
            moveShiftedElements();
        else
            maybeMoveShiftedElements();
        if (getDenseCapacity() >= reqCapacity)
            return true;
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
        MOZ_ASSERT(reqCapacity <= as<ArrayObject>().length());
        MOZ_ASSERT(reqCapacity <= MAX_DENSE_ELEMENTS_COUNT);
        // Preserve the |capacity <= length| invariant for arrays with
        // non-writable length.  See also js::ArraySetLength which initially
        // enforces this requirement.
        newAllocated = reqCapacity + numShifted + ObjectElements::VALUES_PER_HEADER;
    } else {
        if (!goodElementsAllocationAmount(cx, reqCapacity + numShifted,
                                          getElementsHeader()->length,
                                          &newAllocated))
        {
            return false;
        }
    }

    uint32_t newCapacity = newAllocated - ObjectElements::VALUES_PER_HEADER - numShifted;
    MOZ_ASSERT(newCapacity > oldCapacity && newCapacity >= reqCapacity);

    // If newCapacity exceeds MAX_DENSE_ELEMENTS_COUNT, the array should become
    // sparse.
    MOZ_ASSERT(newCapacity <= MAX_DENSE_ELEMENTS_COUNT);

    uint32_t initlen = getDenseInitializedLength();

    HeapSlot* oldHeaderSlots = reinterpret_cast<HeapSlot*>(getUnshiftedElementsHeader());
    HeapSlot* newHeaderSlots;
    if (hasDynamicElements()) {
        MOZ_ASSERT(oldCapacity <= MAX_DENSE_ELEMENTS_COUNT);
        uint32_t oldAllocated = oldCapacity + ObjectElements::VALUES_PER_HEADER + numShifted;

        newHeaderSlots = ReallocateObjectBuffer<HeapSlot>(cx, this, oldHeaderSlots, oldAllocated, newAllocated);
        if (!newHeaderSlots)
            return false;   // Leave elements at its old size.
    } else {
        newHeaderSlots = AllocateObjectBuffer<HeapSlot>(cx, this, newAllocated);
        if (!newHeaderSlots)
            return false;   // Leave elements at its old size.
        PodCopy(newHeaderSlots, oldHeaderSlots,
                ObjectElements::VALUES_PER_HEADER + initlen + numShifted);
    }

    ObjectElements* newheader = reinterpret_cast<ObjectElements*>(newHeaderSlots);
    elements_ = newheader->elements() + numShifted;
    getElementsHeader()->capacity = newCapacity;

    Debug_SetSlotRangeToCrashOnTouch(elements_ + initlen, newCapacity - initlen);

    return true;
}

void
NativeObject::shrinkElements(JSContext* cx, uint32_t reqCapacity)
{
    MOZ_ASSERT(canHaveNonEmptyElements());
    MOZ_ASSERT(reqCapacity >= getDenseInitializedLength());

    if (denseElementsAreCopyOnWrite())
        MOZ_CRASH();

    if (!hasDynamicElements())
        return;

    // If we have shifted elements, consider moving them.
    uint32_t numShifted = getElementsHeader()->numShiftedElements();
    if (numShifted > 0) {
        maybeMoveShiftedElements();
        numShifted = getElementsHeader()->numShiftedElements();
    }

    uint32_t oldCapacity = getDenseCapacity();
    MOZ_ASSERT(reqCapacity < oldCapacity);

    uint32_t newAllocated = 0;
    MOZ_ALWAYS_TRUE(goodElementsAllocationAmount(cx, reqCapacity + numShifted, 0, &newAllocated));
    MOZ_ASSERT(oldCapacity <= MAX_DENSE_ELEMENTS_COUNT);

    uint32_t oldAllocated = oldCapacity + ObjectElements::VALUES_PER_HEADER + numShifted;
    if (newAllocated == oldAllocated)
        return;  // Leave elements at its old size.

    MOZ_ASSERT(newAllocated > ObjectElements::VALUES_PER_HEADER);
    uint32_t newCapacity = newAllocated - ObjectElements::VALUES_PER_HEADER - numShifted;
    MOZ_ASSERT(newCapacity <= MAX_DENSE_ELEMENTS_COUNT);

    HeapSlot* oldHeaderSlots = reinterpret_cast<HeapSlot*>(getUnshiftedElementsHeader());
    HeapSlot* newHeaderSlots = ReallocateObjectBuffer<HeapSlot>(cx, this, oldHeaderSlots,
                                                                oldAllocated, newAllocated);
    if (!newHeaderSlots) {
        cx->recoverFromOutOfMemory();
        return;  // Leave elements at its old size.
    }

    ObjectElements* newheader = reinterpret_cast<ObjectElements*>(newHeaderSlots);
    elements_ = newheader->elements() + numShifted;
    getElementsHeader()->capacity = newCapacity;
}

/* static */ bool
NativeObject::CopyElementsForWrite(JSContext* cx, NativeObject* obj)
{
    MOZ_ASSERT(obj->denseElementsAreCopyOnWrite());
    MOZ_ASSERT(!obj->denseElementsAreFrozen());

    // The original owner of a COW elements array should never be modified.
    MOZ_ASSERT(obj->getElementsHeader()->ownerObject() != obj);

    uint32_t initlen = obj->getDenseInitializedLength();
    uint32_t newAllocated = 0;
    if (!goodElementsAllocationAmount(cx, initlen, 0, &newAllocated))
        return false;

    uint32_t newCapacity = newAllocated - ObjectElements::VALUES_PER_HEADER;

    // COPY_ON_WRITE flags is set only if obj is a dense array.
    MOZ_ASSERT(newCapacity <= MAX_DENSE_ELEMENTS_COUNT);

    JSObject::writeBarrierPre(obj->getElementsHeader()->ownerObject());

    HeapSlot* newHeaderSlots = AllocateObjectBuffer<HeapSlot>(cx, obj, newAllocated);
    if (!newHeaderSlots)
        return false;
    ObjectElements* newheader = reinterpret_cast<ObjectElements*>(newHeaderSlots);
    js_memcpy(newheader, obj->getElementsHeader(),
              (ObjectElements::VALUES_PER_HEADER + initlen) * sizeof(Value));

    newheader->capacity = newCapacity;
    newheader->clearCopyOnWrite();
    obj->elements_ = newheader->elements();

    Debug_SetSlotRangeToCrashOnTouch(obj->elements_ + initlen, newCapacity - initlen);

    return true;
}

/* static */ bool
NativeObject::allocDictionarySlot(JSContext* cx, HandleNativeObject obj, uint32_t* slotp)
{
    MOZ_ASSERT(obj->inDictionaryMode());

    uint32_t slot = obj->slotSpan();
    MOZ_ASSERT(slot >= JSSLOT_FREE(obj->getClass()));

    // Try to pull a free slot from the shape table's slot-number free list.
    // Shapes without a ShapeTable have an empty free list, because we only
    // purge ShapeTables with an empty free list.
    {
        AutoCheckCannotGC nogc;
        if (ShapeTable* table = obj->lastProperty()->maybeTable(nogc)) {
            uint32_t last = table->freeList();
            if (last != SHAPE_INVALID_SLOT) {
#ifdef DEBUG
                MOZ_ASSERT(last < slot);
                uint32_t next = obj->getSlot(last).toPrivateUint32();
                MOZ_ASSERT_IF(next != SHAPE_INVALID_SLOT, next < slot);
#endif

                *slotp = last;

                const Value& vref = obj->getSlot(last);
                table->setFreeList(vref.toPrivateUint32());
                obj->setSlot(last, UndefinedValue());
                return true;
            }
        }
    }

    if (slot >= SHAPE_MAXIMUM_SLOT) {
        ReportOutOfMemory(cx);
        return false;
    }

    *slotp = slot;

    return obj->setSlotSpan(cx, slot + 1);
}

void
NativeObject::freeSlot(JSContext* cx, uint32_t slot)
{
    MOZ_ASSERT(slot < slotSpan());

    if (inDictionaryMode()) {
        // Ensure we have a ShapeTable as it stores the object's free list (the
        // list of available slots in dictionary objects).
        AutoCheckCannotGC nogc;
        if (ShapeTable* table = lastProperty()->ensureTableForDictionary(cx, nogc)) {
            uint32_t last = table->freeList();

            // Can't afford to check the whole free list, but let's check the head.
            MOZ_ASSERT_IF(last != SHAPE_INVALID_SLOT, last < slotSpan() && last != slot);

            // Place all freed slots other than reserved slots (bug 595230) on the
            // dictionary's free list.
            if (JSSLOT_FREE(getClass()) <= slot) {
                MOZ_ASSERT_IF(last != SHAPE_INVALID_SLOT, last < slotSpan());
                setSlot(slot, PrivateUint32Value(last));
                table->setFreeList(slot);
                return;
            }
        } else {
            // OOM while creating the ShapeTable holding the free list. We can
            // recover from it - it just means we won't be able to reuse this
            // slot later.
            cx->recoverFromOutOfMemory();
        }
    }
    setSlot(slot, UndefinedValue());
}

/* static */ Shape*
NativeObject::addDataProperty(JSContext* cx, HandleNativeObject obj,
                              HandlePropertyName name, uint32_t slot, unsigned attrs)
{
    MOZ_ASSERT(!(attrs & (JSPROP_GETTER | JSPROP_SETTER)));
    RootedId id(cx, NameToId(name));
    return addDataProperty(cx, obj, id, slot, attrs);
}

template <AllowGC allowGC>
bool
js::NativeLookupOwnProperty(JSContext* cx,
                            typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                            typename MaybeRooted<jsid, allowGC>::HandleType id,
                            typename MaybeRooted<PropertyResult, allowGC>::MutableHandleType propp)
{
    bool done;
    return LookupOwnPropertyInline<allowGC>(cx, obj, id, propp, &done);
}

template bool
js::NativeLookupOwnProperty<CanGC>(JSContext* cx, HandleNativeObject obj, HandleId id,
                                   MutableHandle<PropertyResult> propp);

template bool
js::NativeLookupOwnProperty<NoGC>(JSContext* cx, NativeObject* const& obj, const jsid& id,
                                  FakeMutableHandle<PropertyResult> propp);

/*** [[DefineOwnProperty]] ***********************************************************************/

static MOZ_ALWAYS_INLINE bool
CallAddPropertyHook(JSContext* cx, HandleNativeObject obj, HandleId id, HandleValue value)
{
    JSAddPropertyOp addProperty = obj->getClass()->getAddProperty();
    if (MOZ_UNLIKELY(addProperty)) {
        MOZ_ASSERT(!cx->helperThread());

        if (!CallJSAddPropertyOp(cx, addProperty, obj, id, value)) {
            NativeObject::removeProperty(cx, obj, id);
            return false;
        }
    }
    return true;
}

static MOZ_ALWAYS_INLINE bool
CallAddPropertyHookDense(JSContext* cx, HandleNativeObject obj, uint32_t index,
                         HandleValue value)
{
    // Inline addProperty for array objects.
    if (obj->is<ArrayObject>()) {
        ArrayObject* arr = &obj->as<ArrayObject>();
        uint32_t length = arr->length();
        if (index >= length)
            arr->setLength(cx, index + 1);
        return true;
    }

    JSAddPropertyOp addProperty = obj->getClass()->getAddProperty();
    if (MOZ_UNLIKELY(addProperty)) {
        MOZ_ASSERT(!cx->helperThread());

        if (!obj->maybeCopyElementsForWrite(cx))
            return false;

        RootedId id(cx, INT_TO_JSID(index));
        if (!CallJSAddPropertyOp(cx, addProperty, obj, id, value)) {
            obj->setDenseElementHole(cx, index);
            return false;
        }
    }
    return true;
}

static MOZ_ALWAYS_INLINE void
UpdateShapeTypeAndValue(JSContext* cx, NativeObject* obj, Shape* shape, jsid id,
                        const Value& value)
{
    MOZ_ASSERT(id == shape->propid());

    if (shape->isDataProperty()) {
        obj->setSlotWithType(cx, shape, value, /* overwriting = */ false);

        // Per the acquired properties analysis, when the shape of a partially
        // initialized object is changed to its fully initialized shape, its
        // group can be updated as well.
        if (TypeNewScript* newScript = obj->groupRaw()->newScript()) {
            if (newScript->initializedShape() == shape)
                obj->setGroup(newScript->initializedGroup());
        }
    } else {
        MarkTypePropertyNonData(cx, obj, id);
    }
    if (!shape->writable())
        MarkTypePropertyNonWritable(cx, obj, id);
}

// Version of UpdateShapeTypeAndValue optimized for plain data properties.
static MOZ_ALWAYS_INLINE void
UpdateShapeTypeAndValueForWritableDataProp(JSContext* cx, NativeObject* obj, Shape* shape,
                                           jsid id, const Value& value)
{
    MOZ_ASSERT(id == shape->propid());

    MOZ_ASSERT(shape->isDataProperty());
    MOZ_ASSERT(shape->hasDefaultGetter());
    MOZ_ASSERT(shape->hasDefaultSetter());
    MOZ_ASSERT(shape->writable());

    obj->setSlotWithType(cx, shape, value, /* overwriting = */ false);

    // Per the acquired properties analysis, when the shape of a partially
    // initialized object is changed to its fully initialized shape, its
    // group can be updated as well.
    if (TypeNewScript* newScript = obj->groupRaw()->newScript()) {
        if (newScript->initializedShape() == shape)
            obj->setGroup(newScript->initializedGroup());
    }
}

void
js::AddPropertyTypesAfterProtoChange(JSContext* cx, NativeObject* obj, ObjectGroup* oldGroup)
{
    MOZ_ASSERT(obj->group() != oldGroup);
    MOZ_ASSERT(!obj->group()->unknownProperties());

    // First copy the dynamic flags.
    MarkObjectGroupFlags(cx, obj, oldGroup->flags() &
                         (OBJECT_FLAG_DYNAMIC_MASK & ~OBJECT_FLAG_UNKNOWN_PROPERTIES));

    // Now update all property types. If the object has many properties, this
    // function may be slow so we mark all properties as unknown.
    static const size_t MaxPropertyCount = 40;

    size_t nprops = obj->getDenseInitializedLength();
    if (nprops > MaxPropertyCount) {
        MarkObjectGroupUnknownProperties(cx, obj->group());
        return;
    }

    // Add dense element types.
    for (size_t i = 0; i < obj->getDenseInitializedLength(); i++) {
        Value val = obj->getDenseElement(i);
        if (!val.isMagic(JS_ELEMENTS_HOLE))
            AddTypePropertyId(cx, obj, JSID_VOID, val);
    }

    // Add property types.
    for (Shape::Range<NoGC> r(obj->lastProperty()); !r.empty(); r.popFront()) {
        Shape* shape = &r.front();
        jsid id = shape->propid();
        if (JSID_IS_EMPTY(id))
            continue;

        if (nprops++ > MaxPropertyCount) {
            MarkObjectGroupUnknownProperties(cx, obj->group());
            return;
        }

        Value val = shape->isDataProperty() ? obj->getSlot(shape->slot()) : UndefinedValue();
        UpdateShapeTypeAndValue(cx, obj, shape, id, val);
    }
}

static bool
PurgeProtoChain(JSContext* cx, JSObject* objArg, HandleId id)
{
    /* Root locally so we can re-assign. */
    RootedObject obj(cx, objArg);

    RootedShape shape(cx);
    while (obj) {
        /* Lookups will not be cached through non-native protos. */
        if (!obj->isNative())
            break;

        shape = obj->as<NativeObject>().lookup(cx, id);
        if (shape)
            return NativeObject::reshapeForShadowedProp(cx, obj.as<NativeObject>());

        obj = obj->staticPrototype();
    }

    return true;
}

static bool
PurgeEnvironmentChainHelper(JSContext* cx, HandleObject objArg, HandleId id)
{
    /* Re-root locally so we can re-assign. */
    RootedObject obj(cx, objArg);

    MOZ_ASSERT(obj->isNative());
    MOZ_ASSERT(obj->isDelegate());

    /* Lookups on integer ids cannot be cached through prototypes. */
    if (JSID_IS_INT(id))
        return true;

    if (!PurgeProtoChain(cx, obj->staticPrototype(), id))
        return false;

    /*
     * We must purge the environment chain only for Call objects as they are
     * the only kind of cacheable non-global object that can gain properties
     * after outer properties with the same names have been cached or
     * traced. Call objects may gain such properties via eval introducing new
     * vars; see bug 490364.
     */
    if (obj->is<CallObject>()) {
        while ((obj = obj->enclosingEnvironment()) != nullptr) {
            if (!PurgeProtoChain(cx, obj, id))
                return false;
        }
    }

    return true;
}

/*
 * ReshapeForShadowedProp does nothing if obj is not itself a prototype or
 * parent environment, else it reshapes the scope and prototype chains it
 * links. It calls PurgeEnvironmentChainHelper, which asserts that obj is
 * flagged as a delegate (i.e., obj has ever been on a prototype or parent
 * chain).
 */
static MOZ_ALWAYS_INLINE bool
ReshapeForShadowedProp(JSContext* cx, HandleObject obj, HandleId id)
{
    if (obj->isDelegate() && obj->isNative())
        return PurgeEnvironmentChainHelper(cx, obj, id);
    return true;
}

/* static */ bool
NativeObject::reshapeForShadowedProp(JSContext* cx, HandleNativeObject obj)
{
    return generateOwnShape(cx, obj);
}

/* static */ bool
NativeObject::reshapeForProtoMutation(JSContext* cx, HandleNativeObject obj)
{
    return generateOwnShape(cx, obj);
}

enum class IsAddOrChange { Add, AddOrChange };

template <IsAddOrChange AddOrChange>
static MOZ_ALWAYS_INLINE bool
AddOrChangeProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                    Handle<PropertyDescriptor> desc)
{
    desc.assertComplete();

    if (!ReshapeForShadowedProp(cx, obj, id))
        return false;

    // Use dense storage for new indexed properties where possible.
    if (JSID_IS_INT(id) &&
        !desc.getter() &&
        !desc.setter() &&
        desc.attributes() == JSPROP_ENUMERATE &&
        (!obj->isIndexed() || !obj->containsPure(id)) &&
        !obj->is<TypedArrayObject>())
    {
        uint32_t index = JSID_TO_INT(id);
        DenseElementResult edResult = obj->ensureDenseElements(cx, index, 1);
        if (edResult == DenseElementResult::Failure)
            return false;
        if (edResult == DenseElementResult::Success) {
            obj->setDenseElementWithType(cx, index, desc.value());
            if (!CallAddPropertyHookDense(cx, obj, index, desc.value()))
                return false;
            return true;
        }
    }

    // If we know this is a new property we can call addProperty instead of
    // the slower putProperty.
    Shape* shape;
    if (AddOrChange == IsAddOrChange::Add) {
        if (Shape::isDataProperty(desc.attributes(), desc.getter(), desc.setter())) {
            shape = NativeObject::addDataProperty(cx, obj, id, SHAPE_INVALID_SLOT,
                                                  desc.attributes());
        } else {
            shape = NativeObject::addAccessorProperty(cx, obj, id, desc.getter(), desc.setter(),
                                                      desc.attributes());
        }
    } else {
        if (Shape::isDataProperty(desc.attributes(), desc.getter(), desc.setter())) {
            shape = NativeObject::putDataProperty(cx, obj, id, desc.attributes());
        } else {
            shape = NativeObject::putAccessorProperty(cx, obj, id, desc.getter(), desc.setter(),
                                                      desc.attributes());
        }
    }
    if (!shape)
        return false;

    UpdateShapeTypeAndValue(cx, obj, shape, id, desc.value());

    // Clear any existing dense index after adding a sparse indexed property,
    // and investigate converting the object to dense indexes.
    if (JSID_IS_INT(id)) {
        if (!obj->maybeCopyElementsForWrite(cx))
            return false;

        uint32_t index = JSID_TO_INT(id);
        NativeObject::removeDenseElementForSparseIndex(cx, obj, index);
        DenseElementResult edResult =
            NativeObject::maybeDensifySparseElements(cx, obj);
        if (edResult == DenseElementResult::Failure)
            return false;
        if (edResult == DenseElementResult::Success) {
            MOZ_ASSERT(!desc.setter());
            return CallAddPropertyHookDense(cx, obj, index, desc.value());
        }
    }

    return CallAddPropertyHook(cx, obj, id, desc.value());
}

// Version of AddOrChangeProperty optimized for adding a plain data property.
// This function doesn't handle integer ids as we may have to store them in
// dense elements.
static MOZ_ALWAYS_INLINE bool
AddDataProperty(JSContext* cx, HandleNativeObject obj, HandleId id, HandleValue v)
{
    MOZ_ASSERT(!JSID_IS_INT(id));

    if (!ReshapeForShadowedProp(cx, obj, id))
        return false;

    Shape* shape = NativeObject::addEnumerableDataProperty(cx, obj, id);
    if (!shape)
        return false;

    UpdateShapeTypeAndValueForWritableDataProp(cx, obj, shape, id, v);

    return CallAddPropertyHook(cx, obj, id, v);
}

static bool IsConfigurable(unsigned attrs) { return (attrs & JSPROP_PERMANENT) == 0; }
static bool IsEnumerable(unsigned attrs) { return (attrs & JSPROP_ENUMERATE) != 0; }
static bool IsWritable(unsigned attrs) { return (attrs & JSPROP_READONLY) == 0; }

static bool IsAccessorDescriptor(unsigned attrs) {
    return (attrs & (JSPROP_GETTER | JSPROP_SETTER)) != 0;
}

static bool IsDataDescriptor(unsigned attrs) {
    MOZ_ASSERT((attrs & (JSPROP_IGNORE_VALUE | JSPROP_IGNORE_READONLY)) == 0);
    return !IsAccessorDescriptor(attrs);
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE bool
GetExistingProperty(JSContext* cx,
                    typename MaybeRooted<Value, allowGC>::HandleType receiver,
                    typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                    typename MaybeRooted<Shape*, allowGC>::HandleType shape,
                    typename MaybeRooted<Value, allowGC>::MutableHandleType vp);

static bool
GetExistingPropertyValue(JSContext* cx, HandleNativeObject obj, HandleId id,
                         Handle<PropertyResult> prop, MutableHandleValue vp)
{
    if (prop.isDenseOrTypedArrayElement()) {
        vp.set(obj->getDenseOrTypedArrayElement(JSID_TO_INT(id)));
        return true;
    }
    MOZ_ASSERT(!cx->helperThread());

    MOZ_ASSERT(prop.shape()->propid() == id);
    MOZ_ASSERT(obj->contains(cx, prop.shape()));

    RootedValue receiver(cx, ObjectValue(*obj));
    RootedShape shape(cx, prop.shape());
    return GetExistingProperty<CanGC>(cx, receiver, obj, shape, vp);
}

/*
 * If desc is redundant with an existing own property obj[id], then set
 * |*redundant = true| and return true.
 */
static bool
DefinePropertyIsRedundant(JSContext* cx, HandleNativeObject obj, HandleId id,
                          Handle<PropertyResult> prop, unsigned shapeAttrs,
                          Handle<PropertyDescriptor> desc, bool *redundant)
{
    *redundant = false;

    if (desc.hasConfigurable() && desc.configurable() != IsConfigurable(shapeAttrs))
        return true;
    if (desc.hasEnumerable() && desc.enumerable() != IsEnumerable(shapeAttrs))
        return true;
    if (desc.isDataDescriptor()) {
        if (IsAccessorDescriptor(shapeAttrs))
            return true;
        if (desc.hasWritable() && desc.writable() != IsWritable(shapeAttrs))
            return true;
        if (desc.hasValue()) {
            // Get the current value of the existing property.
            RootedValue currentValue(cx);
            if (!prop.isDenseOrTypedArrayElement() &&
                prop.shape()->isDataProperty())
            {
                // Inline GetExistingPropertyValue in order to omit a type
                // correctness assertion that's too strict for this particular
                // call site. For details, see bug 1125624 comments 13-16.
                currentValue.set(obj->getSlot(prop.shape()->slot()));
            } else {
                if (!GetExistingPropertyValue(cx, obj, id, prop, &currentValue))
                    return false;
            }

            // Don't call SameValue here to ensure we properly update distinct
            // NaN values.
            if (desc.value() != currentValue)
                return true;
        }

        GetterOp existingGetterOp =
            prop.isDenseOrTypedArrayElement() ? nullptr : prop.shape()->getter();
        if (desc.getter() != existingGetterOp)
            return true;

        SetterOp existingSetterOp =
            prop.isDenseOrTypedArrayElement() ? nullptr : prop.shape()->setter();
        if (desc.setter() != existingSetterOp)
            return true;
    } else {
        if (desc.hasGetterObject() &&
            (!(shapeAttrs & JSPROP_GETTER) || desc.getterObject() != prop.shape()->getterObject()))
        {
            return true;
        }
        if (desc.hasSetterObject() &&
            (!(shapeAttrs & JSPROP_SETTER) || desc.setterObject() != prop.shape()->setterObject()))
        {
            return true;
        }
    }

    *redundant = true;
    return true;
}

bool
js::NativeDefineProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                         Handle<PropertyDescriptor> desc_,
                         ObjectOpResult& result)
{
    desc_.assertValid();

    // Section numbers and step numbers below refer to ES2018, draft rev
    // 540b827fccf6122a984be99ab9af7be20e3b5562.
    //
    // This function aims to implement 9.1.6 [[DefineOwnProperty]] as well as
    // the [[DefineOwnProperty]] methods described in 9.4.2.1 (arrays), 9.4.4.2
    // (arguments), and 9.4.5.3 (typed array views).

    // Dispense with custom behavior of exotic native objects first.
    if (obj->is<ArrayObject>()) {
        // 9.4.2.1 step 2. Redefining an array's length is very special.
        Rooted<ArrayObject*> arr(cx, &obj->as<ArrayObject>());
        if (id == NameToId(cx->names().length)) {
            // 9.1.6.3 ValidateAndApplyPropertyDescriptor, step 7.a.
            if (desc_.isAccessorDescriptor())
                return result.fail(JSMSG_CANT_REDEFINE_PROP);

            MOZ_ASSERT(!cx->helperThread());
            return ArraySetLength(cx, arr, id, desc_.attributes(), desc_.value(), result);
        }

        // 9.4.2.1 step 3. Don't extend a fixed-length array.
        uint32_t index;
        if (IdIsIndex(id, &index)) {
            if (WouldDefinePastNonwritableLength(obj, index))
                return result.fail(JSMSG_CANT_DEFINE_PAST_ARRAY_LENGTH);
        }
    } else if (obj->is<TypedArrayObject>()) {
        // 9.4.5.3 step 3. Indexed properties of typed arrays are special.
        uint64_t index;
        if (IsTypedArrayIndex(id, &index)) {
            MOZ_ASSERT(!cx->helperThread());
            return DefineTypedArrayElement(cx, obj, index, desc_, result);
        }
    } else if (obj->is<ArgumentsObject>()) {
        Rooted<ArgumentsObject*> argsobj(cx, &obj->as<ArgumentsObject>());
        if (id == NameToId(cx->names().length)) {
            // Either we are resolving the .length property on this object,
            // or redefining it. In the latter case only, we must reify the
            // property. To distinguish the two cases, we note that when
            // resolving, the JSPROP_RESOLVING mask is set; whereas the first
            // time it is redefined, it isn't set.
            if ((desc_.attributes() & JSPROP_RESOLVING) == 0) {
                if (!ArgumentsObject::reifyLength(cx, argsobj))
                    return false;
            }
        } else if (JSID_IS_SYMBOL(id) && JSID_TO_SYMBOL(id) == cx->wellKnownSymbols().iterator) {
            // Do same thing as .length for [@@iterator].
            if ((desc_.attributes() & JSPROP_RESOLVING) == 0) {
                if (!ArgumentsObject::reifyIterator(cx, argsobj))
                    return false;
            }
        } else if (JSID_IS_INT(id)) {
            if ((desc_.attributes() & JSPROP_RESOLVING) == 0)
                argsobj->markElementOverridden();
        }
    }

    // 9.1.6.1 OrdinaryDefineOwnProperty step 1.
    Rooted<PropertyResult> prop(cx);
    if (desc_.attributes() & JSPROP_RESOLVING) {
        // We are being called from a resolve or enumerate hook to reify a
        // lazily-resolved property. To avoid reentering the resolve hook and
        // recursing forever, skip the resolve hook when doing this lookup.
        NativeLookupOwnPropertyNoResolve(cx, obj, id, &prop);
    } else {
        if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &prop))
            return false;
    }

    // From this point, the step numbers refer to
    // 9.1.6.3, ValidateAndApplyPropertyDescriptor.
    // Step 1 is a redundant assertion.

    // Filling in desc: Here we make a copy of the desc_ argument. We will turn
    // it into a complete descriptor before updating obj. The spec algorithm
    // does not explicitly do this, but the end result is the same. Search for
    // "fill in" below for places where the filling-in actually occurs.
    Rooted<PropertyDescriptor> desc(cx, desc_);

    // Step 2.
    if (!prop) {
        if (!obj->nonProxyIsExtensible())
            return result.fail(JSMSG_CANT_DEFINE_PROP_OBJECT_NOT_EXTENSIBLE);

        // Fill in missing desc fields with defaults.
        CompletePropertyDescriptor(&desc);

        if (!AddOrChangeProperty<IsAddOrChange::Add>(cx, obj, id, desc))
            return false;
        return result.succeed();
    }

    // Step 3 and 7.a.i.3, 8.a.iii, 10 (partially). We use shapeAttrs as a
    // stand-in for shape in many places below, since shape might not be a
    // pointer to a real Shape (see IsImplicitDenseOrTypedArrayElement).
    unsigned shapeAttrs = GetPropertyAttributes(obj, prop);
    bool redundant;
    if (!DefinePropertyIsRedundant(cx, obj, id, prop, shapeAttrs, desc, &redundant))
        return false;
    if (redundant) {
        // In cases involving JSOP_NEWOBJECT and JSOP_INITPROP, obj can have a
        // type for this property that doesn't match the value in the slot.
        // Update the type here, even though this DefineProperty call is
        // otherwise a no-op. (See bug 1125624 comment 13.)
        if (!prop.isDenseOrTypedArrayElement() && desc.hasValue())
            UpdateShapeTypeAndValue(cx, obj, prop.shape(), id, desc.value());
        return result.succeed();
    }

    // Non-standard hack: Allow redefining non-configurable properties if
    // JSPROP_REDEFINE_NONCONFIGURABLE is set _and_ the object is a non-DOM
    // global. The idea is that a DOM object can never have such a thing on
    // its proto chain directly on the web, so we should be OK optimizing
    // access to accessors found on such an object. Bug 1105518 contemplates
    // removing this hack.
    bool skipRedefineChecks = (desc.attributes() & JSPROP_REDEFINE_NONCONFIGURABLE) &&
                              obj->is<GlobalObject>() &&
                              !obj->getClass()->isDOMClass();

    // Step 4.
    if (!IsConfigurable(shapeAttrs) && !skipRedefineChecks) {
        if (desc.hasConfigurable() && desc.configurable())
            return result.fail(JSMSG_CANT_REDEFINE_PROP);
        if (desc.hasEnumerable() && desc.enumerable() != IsEnumerable(shapeAttrs))
            return result.fail(JSMSG_CANT_REDEFINE_PROP);
    }

    // Fill in desc.[[Configurable]] and desc.[[Enumerable]] if missing.
    if (!desc.hasConfigurable())
        desc.setConfigurable(IsConfigurable(shapeAttrs));
    if (!desc.hasEnumerable())
        desc.setEnumerable(IsEnumerable(shapeAttrs));

    // Steps 5-8.
    if (desc.isGenericDescriptor()) {
        // Step 5. No further validation is required.

        // Fill in desc. A generic descriptor has none of these fields, so copy
        // everything from shape.
        MOZ_ASSERT(!desc.hasValue());
        MOZ_ASSERT(!desc.hasWritable());
        MOZ_ASSERT(!desc.hasGetterObject());
        MOZ_ASSERT(!desc.hasSetterObject());
        if (IsDataDescriptor(shapeAttrs)) {
            RootedValue currentValue(cx);
            if (!GetExistingPropertyValue(cx, obj, id, prop, &currentValue))
                return false;
            desc.setValue(currentValue);
            desc.setWritable(IsWritable(shapeAttrs));
        } else {
            desc.setGetterObject(prop.shape()->getterObject());
            desc.setSetterObject(prop.shape()->setterObject());
        }
    } else if (desc.isDataDescriptor() != IsDataDescriptor(shapeAttrs)) {
        // Step 6.
        if (!IsConfigurable(shapeAttrs) && !skipRedefineChecks)
            return result.fail(JSMSG_CANT_REDEFINE_PROP);

        if (prop.isDenseOrTypedArrayElement()) {
            MOZ_ASSERT(!obj->is<TypedArrayObject>());
            if (!NativeObject::sparsifyDenseElement(cx, obj, JSID_TO_INT(id)))
                return false;
            prop.setNativeProperty(obj->lookup(cx, id));
        }

        // Fill in desc fields with default values (steps 6.b.i and 6.c.i).
        CompletePropertyDescriptor(&desc);
    } else if (desc.isDataDescriptor()) {
        // Step 7.
        bool frozen = !IsConfigurable(shapeAttrs) && !IsWritable(shapeAttrs);

        // Step 7.a.i.1.
        if (frozen && desc.hasWritable() && desc.writable() && !skipRedefineChecks)
            return result.fail(JSMSG_CANT_REDEFINE_PROP);

        if (frozen || !desc.hasValue()) {
            if (prop.isDenseOrTypedArrayElement()) {
                MOZ_ASSERT(!obj->is<TypedArrayObject>());
                if (!NativeObject::sparsifyDenseElement(cx, obj, JSID_TO_INT(id)))
                    return false;
                prop.setNativeProperty(obj->lookup(cx, id));
            }

            RootedValue currentValue(cx);
            if (!GetExistingPropertyValue(cx, obj, id, prop, &currentValue))
                return false;

            if (!desc.hasValue()) {
                // Fill in desc.[[Value]].
                desc.setValue(currentValue);
            } else {
                // Step 7.a.i.2.
                bool same;
                MOZ_ASSERT(!cx->helperThread());
                if (!SameValue(cx, desc.value(), currentValue, &same))
                    return false;
                if (!same && !skipRedefineChecks)
                    return result.fail(JSMSG_CANT_REDEFINE_PROP);
            }
        }

        // Step 7.a.i.3.
        if (frozen && !skipRedefineChecks)
            return result.succeed();

        if (!desc.hasWritable())
            desc.setWritable(IsWritable(shapeAttrs));
    } else {
        // Step 8.
        MOZ_ASSERT(prop.shape()->isAccessorDescriptor());
        MOZ_ASSERT(desc.isAccessorDescriptor());

        // The spec says to use SameValue, but since the values in
        // question are objects, we can just compare pointers.
        if (desc.hasSetterObject()) {
            // Step 8.a.i.
            if (!IsConfigurable(shapeAttrs) &&
                desc.setterObject() != prop.shape()->setterObject() &&
                !skipRedefineChecks)
            {
                return result.fail(JSMSG_CANT_REDEFINE_PROP);
            }
        } else {
            // Fill in desc.[[Set]] from shape.
            desc.setSetterObject(prop.shape()->setterObject());
        }
        if (desc.hasGetterObject()) {
            // Step 8.a.ii.
            if (!IsConfigurable(shapeAttrs) &&
                desc.getterObject() != prop.shape()->getterObject() &&
                !skipRedefineChecks)
            {
                return result.fail(JSMSG_CANT_REDEFINE_PROP);
            }
        } else {
            // Fill in desc.[[Get]] from shape.
            desc.setGetterObject(prop.shape()->getterObject());
        }

        // Step 8.a.iii (Omitted).
    }

    // Step 9.
    if (!AddOrChangeProperty<IsAddOrChange::AddOrChange>(cx, obj, id, desc))
        return false;

    // Step 10.
    return result.succeed();
}

bool
js::NativeDefineAccessorProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                                 GetterOp getter, SetterOp setter, unsigned attrs,
                                 ObjectOpResult& result)
{
    Rooted<PropertyDescriptor> desc(cx);
    desc.initFields(nullptr, UndefinedHandleValue, attrs, getter, setter);
    return NativeDefineProperty(cx, obj, id, desc, result);
}

bool
js::NativeDefineDataProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                             HandleValue value, unsigned attrs, ObjectOpResult& result)
{
    Rooted<PropertyDescriptor> desc(cx);
    desc.initFields(nullptr, value, attrs, nullptr, nullptr);
    return NativeDefineProperty(cx, obj, id, desc, result);
}

bool
js::NativeDefineAccessorProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                                 JSGetterOp getter, JSSetterOp setter, unsigned attrs)
{
    ObjectOpResult result;
    if (!NativeDefineAccessorProperty(cx, obj, id, getter, setter, attrs, result))
        return false;
    if (!result) {
        // Off-thread callers should not get here: they must call this
        // function only with known-valid arguments. Populating a new
        // PlainObject with configurable properties is fine.
        MOZ_ASSERT(!cx->helperThread());
        result.reportError(cx, obj, id);
        return false;
    }
    return true;
}

bool
js::NativeDefineDataProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                             HandleValue value, unsigned attrs)
{
    ObjectOpResult result;
    if (!NativeDefineDataProperty(cx, obj, id, value, attrs, result))
        return false;
    if (!result) {
        // Off-thread callers should not get here: they must call this
        // function only with known-valid arguments. Populating a new
        // PlainObject with configurable properties is fine.
        MOZ_ASSERT(!cx->helperThread());
        result.reportError(cx, obj, id);
        return false;
    }
    return true;
}

bool
js::NativeDefineAccessorProperty(JSContext* cx, HandleNativeObject obj, PropertyName* name,
                                 JSGetterOp getter, JSSetterOp setter, unsigned attrs)
{
    RootedId id(cx, NameToId(name));
    return NativeDefineAccessorProperty(cx, obj, id, getter, setter, attrs);
}

bool
js::NativeDefineDataProperty(JSContext* cx, HandleNativeObject obj, PropertyName* name,
                             HandleValue value, unsigned attrs)
{
    RootedId id(cx, NameToId(name));
    return NativeDefineDataProperty(cx, obj, id, value, attrs);
}

static bool
DefineNonexistentProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                          HandleValue v, ObjectOpResult& result)
{
    // Optimized NativeDefineProperty() version for known absent properties.

    // Dispense with custom behavior of exotic native objects first.
    if (obj->is<ArrayObject>()) {
        // Array's length property is non-configurable, so we shouldn't
        // encounter it in this function.
        MOZ_ASSERT(id != NameToId(cx->names().length));

        // 9.4.2.1 step 3. Don't extend a fixed-length array.
        uint32_t index;
        if (IdIsIndex(id, &index)) {
            if (WouldDefinePastNonwritableLength(obj, index))
                return result.fail(JSMSG_CANT_DEFINE_PAST_ARRAY_LENGTH);
        }
    } else if (obj->is<TypedArrayObject>()) {
        // 9.4.5.3 step 3. Indexed properties of typed arrays are special.
        uint64_t index;
        if (IsTypedArrayIndex(id, &index)) {
            // This method is only called for non-existent properties, which
            // means any absent indexed property must be out of range.
            MOZ_ASSERT(index >= obj->as<TypedArrayObject>().length());

            // We (wrongly) ignore out of range defines.
            return result.succeed();
        }
    } else if (obj->is<ArgumentsObject>()) {
        // If this method is called with either |length| or |@@iterator|, the
        // property was previously deleted and hence should already be marked
        // as overridden.
        MOZ_ASSERT_IF(id == NameToId(cx->names().length),
                      obj->as<ArgumentsObject>().hasOverriddenLength());
        MOZ_ASSERT_IF(JSID_IS_SYMBOL(id) && JSID_TO_SYMBOL(id) == cx->wellKnownSymbols().iterator,
                      obj->as<ArgumentsObject>().hasOverriddenIterator());

        // We still need to mark any element properties as overridden.
        if (JSID_IS_INT(id))
            obj->as<ArgumentsObject>().markElementOverridden();
    }

#ifdef DEBUG
    Rooted<PropertyResult> prop(cx);
    NativeLookupOwnPropertyNoResolve(cx, obj, id, &prop);
    MOZ_ASSERT(!prop, "didn't expect to find an existing property");
#endif

    // 9.1.6.3, ValidateAndApplyPropertyDescriptor.
    // Step 1 is a redundant assertion, step 3 and later don't apply here.

    // Step 2.
    if (!obj->nonProxyIsExtensible())
        return result.fail(JSMSG_CANT_DEFINE_PROP_OBJECT_NOT_EXTENSIBLE);

    if (JSID_IS_INT(id)) {
        // This might be a dense element. Use AddOrChangeProperty as it knows
        // how to deal with that.

        Rooted<PropertyDescriptor> desc(cx);
        desc.setDataDescriptor(v, JSPROP_ENUMERATE);

        if (!AddOrChangeProperty<IsAddOrChange::Add>(cx, obj, id, desc))
            return false;
    } else {
        if (!AddDataProperty(cx, obj, id, v))
            return false;
    }

    return result.succeed();
}


/*** [[HasProperty]] *****************************************************************************/

// ES6 draft rev31 9.1.7.1 OrdinaryHasProperty
bool
js::NativeHasProperty(JSContext* cx, HandleNativeObject obj, HandleId id, bool* foundp)
{
    RootedNativeObject pobj(cx, obj);
    Rooted<PropertyResult> prop(cx);

    // This loop isn't explicit in the spec algorithm. See the comment on step
    // 7.a. below.
    for (;;) {
        // Steps 2-3. ('done' is a SpiderMonkey-specific thing, used below.)
        bool done;
        if (!LookupOwnPropertyInline<CanGC>(cx, pobj, id, &prop, &done))
            return false;

        // Step 4.
        if (prop) {
            *foundp = true;
            return true;
        }

        // Step 5-6. The check for 'done' on this next line is tricky.
        // done can be true in exactly these unlikely-sounding cases:
        // - We're looking up an element, and pobj is a TypedArray that
        //   doesn't have that many elements.
        // - We're being called from a resolve hook to assign to the property
        //   being resolved.
        // What they all have in common is we do not want to keep walking
        // the prototype chain, and always claim that the property
        // doesn't exist.
        JSObject* proto = done ? nullptr : pobj->staticPrototype();

        // Step 8.
        if (!proto) {
            *foundp = false;
            return true;
        }

        // Step 7.a. If the prototype is also native, this step is a
        // recursive tail call, and we don't need to go through all the
        // plumbing of HasProperty; the top of the loop is where
        // we're going to end up anyway. But if pobj is non-native,
        // that optimization would be incorrect.
        if (!proto->isNative()) {
            RootedObject protoRoot(cx, proto);
            return HasProperty(cx, protoRoot, id, foundp);
        }

        pobj = &proto->as<NativeObject>();
    }
}


/*** [[GetOwnPropertyDescriptor]] ****************************************************************/

bool
js::NativeGetOwnPropertyDescriptor(JSContext* cx, HandleNativeObject obj, HandleId id,
                                   MutableHandle<PropertyDescriptor> desc)
{
    Rooted<PropertyResult> prop(cx);
    if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &prop))
        return false;
    if (!prop) {
        desc.object().set(nullptr);
        return true;
    }

    desc.setAttributes(GetPropertyAttributes(obj, prop));
    if (desc.isAccessorDescriptor()) {
        // The result of GetOwnPropertyDescriptor() must be either undefined or
        // a complete property descriptor (per ES6 draft rev 32 (2015 Feb 2)
        // 6.1.7.3, Invariants of the Essential Internal Methods).
        //
        // It is an unfortunate fact that in SM, properties can exist that have
        // JSPROP_GETTER or JSPROP_SETTER but not both. In these cases, rather
        // than return true with desc incomplete, we fill out the missing
        // getter or setter with a null, following CompletePropertyDescriptor.
        if (desc.hasGetterObject()) {
            desc.setGetterObject(prop.shape()->getterObject());
        } else {
            desc.setGetterObject(nullptr);
            desc.attributesRef() |= JSPROP_GETTER;
        }
        if (desc.hasSetterObject()) {
            desc.setSetterObject(prop.shape()->setterObject());
        } else {
            desc.setSetterObject(nullptr);
            desc.attributesRef() |= JSPROP_SETTER;
        }

        desc.value().setUndefined();
    } else {
        // This is either a straight-up data property or (rarely) a
        // property with a JSGetterOp/JSSetterOp. The latter must be
        // reported to the caller as a plain data property, so clear
        // desc.getter/setter, and mask away the SHARED bit.
        desc.setGetter(nullptr);
        desc.setSetter(nullptr);

        if (prop.isDenseOrTypedArrayElement()) {
            desc.value().set(obj->getDenseOrTypedArrayElement(JSID_TO_INT(id)));
        } else {
            RootedShape shape(cx, prop.shape());
            if (!NativeGetExistingProperty(cx, obj, obj, shape, desc.value()))
                return false;
        }
    }

    desc.object().set(obj);
    desc.assertComplete();
    return true;
}


/*** [[Get]] *************************************************************************************/

static inline bool
CallGetter(JSContext* cx, HandleObject obj, HandleValue receiver, HandleShape shape,
           MutableHandleValue vp)
{
    MOZ_ASSERT(!shape->hasDefaultGetter());

    if (shape->hasGetterValue()) {
        RootedValue getter(cx, shape->getterValue());
        return js::CallGetter(cx, receiver, getter, vp);
    }

    // In contrast to normal getters JSGetterOps always want the holder.
    RootedId id(cx, shape->propid());
    return CallJSGetterOp(cx, shape->getterOp(), obj, id, vp);
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE bool
GetExistingProperty(JSContext* cx,
                    typename MaybeRooted<Value, allowGC>::HandleType receiver,
                    typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                    typename MaybeRooted<Shape*, allowGC>::HandleType shape,
                    typename MaybeRooted<Value, allowGC>::MutableHandleType vp)
{
    if (shape->isDataProperty()) {
        MOZ_ASSERT(shape->hasDefaultGetter());

        vp.set(obj->getSlot(shape->slot()));

        MOZ_ASSERT_IF(!vp.isMagic(JS_UNINITIALIZED_LEXICAL) &&
                      !obj->isSingleton() &&
                      !obj->template is<EnvironmentObject>() &&
                      shape->hasDefaultGetter(),
                      ObjectGroupHasProperty(cx, obj->group(), shape->propid(), vp));
        return true;
    }

    vp.setUndefined();

    if (shape->hasDefaultGetter())
        return true;

    {
        jsbytecode* pc;
        JSScript* script = cx->currentScript(&pc);
        if (script && script->hasBaselineScript()) {
            switch (JSOp(*pc)) {
              case JSOP_GETPROP:
              case JSOP_CALLPROP:
              case JSOP_LENGTH:
                script->baselineScript()->noteAccessedGetter(script->pcToOffset(pc));
                break;
              default:
                break;
            }
        }
    }

    if (!allowGC)
        return false;

    return CallGetter(cx,
                      MaybeRooted<JSObject*, allowGC>::toHandle(obj),
                      MaybeRooted<Value, allowGC>::toHandle(receiver),
                      MaybeRooted<Shape*, allowGC>::toHandle(shape),
                      MaybeRooted<Value, allowGC>::toMutableHandle(vp));
}

bool
js::NativeGetExistingProperty(JSContext* cx, HandleObject receiver, HandleNativeObject obj,
                              HandleShape shape, MutableHandleValue vp)
{
    RootedValue receiverValue(cx, ObjectValue(*receiver));
    return GetExistingProperty<CanGC>(cx, receiverValue, obj, shape, vp);
}

/*
 * Given pc pointing after a property accessing bytecode, return true if the
 * access is "property-detecting" -- that is, if we shouldn't warn about it
 * even if no such property is found and strict warnings are enabled.
 */
static bool
Detecting(JSContext* cx, JSScript* script, jsbytecode* pc)
{
    MOZ_ASSERT(script->containsPC(pc));

    // Skip jump target opcodes.
    while (pc < script->codeEnd() && BytecodeIsJumpTarget(JSOp(*pc)))
        pc = GetNextPc(pc);

    MOZ_ASSERT(script->containsPC(pc));
    if (pc >= script->codeEnd())
        return false;

    // General case: a branch or equality op follows the access.
    JSOp op = JSOp(*pc);
    if (CodeSpec[op].format & JOF_DETECTING)
        return true;

    jsbytecode* endpc = script->codeEnd();

    if (op == JSOP_NULL) {
        // Special case #1: don't warn about (obj.prop == null).
        if (++pc < endpc) {
            op = JSOp(*pc);
            return op == JSOP_EQ || op == JSOP_NE;
        }
        return false;
    }

    if (op == JSOP_GETGNAME || op == JSOP_GETNAME) {
        // Special case #2: don't warn about (obj.prop == undefined).
        JSAtom* atom = script->getAtom(GET_UINT32_INDEX(pc));
        if (atom == cx->names().undefined &&
            (pc += CodeSpec[op].length) < endpc) {
            op = JSOp(*pc);
            return op == JSOP_EQ || op == JSOP_NE || op == JSOP_STRICTEQ || op == JSOP_STRICTNE;
        }
    }

    return false;
}

enum IsNameLookup { NotNameLookup = false, NameLookup = true };

/*
 * Finish getting the property `receiver[id]` after looking at every object on
 * the prototype chain and not finding any such property.
 *
 * Per the spec, this should just set the result to `undefined` and call it a
 * day. However:
 *
 * 1.  This function also runs when we're evaluating an expression that's an
 *     Identifier (that is, an unqualified name lookup), so we need to figure
 *     out if that's what's happening and throw a ReferenceError if so.
 *
 * 2.  We also emit an optional warning for this. (It's not super useful on the
 *     web, as there are too many false positives, but anecdotally useful in
 *     Gecko code.)
 */
static bool
GetNonexistentProperty(JSContext* cx, HandleId id, IsNameLookup nameLookup, MutableHandleValue vp)
{
    vp.setUndefined();

    // If we are doing a name lookup, this is a ReferenceError.
    if (nameLookup)
        return ReportIsNotDefined(cx, id);

    // Give a strict warning if foo.bar is evaluated by a script for an object
    // foo with no property named 'bar'.
    //
    // Don't warn if extra warnings not enabled or for random getprop
    // operations.
    if (MOZ_LIKELY(!cx->compartment()->behaviors().extraWarnings(cx)))
        return true;

    jsbytecode* pc;
    RootedScript script(cx, cx->currentScript(&pc));
    if (!script)
        return true;

    if (*pc != JSOP_GETPROP && *pc != JSOP_GETELEM)
        return true;

    // Don't warn repeatedly for the same script.
    if (script->warnedAboutUndefinedProp())
        return true;

    // Don't warn in self-hosted code (where the further presence of
    // JS::RuntimeOptions::werror() would result in impossible-to-avoid
    // errors to entirely-innocent client code).
    if (script->selfHosted())
        return true;

    // Do not warn about tests like (obj[prop] == undefined).
    pc += CodeSpec[*pc].length;
    if (Detecting(cx, script, pc))
        return true;

    unsigned flags = JSREPORT_WARNING | JSREPORT_STRICT;
    script->setWarnedAboutUndefinedProp();

    // Ok, bad undefined property reference: whine about it.
    RootedValue val(cx, IdToValue(id));
    return ReportValueErrorFlags(cx, flags, JSMSG_UNDEFINED_PROP, JSDVG_IGNORE_STACK, val,
                                    nullptr, nullptr, nullptr);
}

/* The NoGC version of GetNonexistentProperty, present only to make types line up. */
bool
GetNonexistentProperty(JSContext* cx, const jsid& id, IsNameLookup nameLookup,
                       FakeMutableHandle<Value> vp)
{
    return false;
}

static inline bool
GeneralizedGetProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue receiver,
                       IsNameLookup nameLookup, MutableHandleValue vp)
{
    if (!CheckRecursionLimit(cx))
        return false;
    if (nameLookup) {
        // When nameLookup is true, GetProperty implements ES6 rev 34 (2015 Feb
        // 20) 8.1.1.2.6 GetBindingValue, with step 3 (the call to HasProperty)
        // and step 6 (the call to Get) fused so that only a single lookup is
        // needed.
        //
        // If we get here, we've reached a non-native object. Fall back on the
        // algorithm as specified, with two separate lookups. (Note that we
        // throw ReferenceErrors regardless of strictness, technically a bug.)

        bool found;
        if (!HasProperty(cx, obj, id, &found))
            return false;
        if (!found)
            return ReportIsNotDefined(cx, id);
    }

    return GetProperty(cx, obj, receiver, id, vp);
}

static inline bool
GeneralizedGetProperty(JSContext* cx, JSObject* obj, jsid id, const Value& receiver,
                       IsNameLookup nameLookup, FakeMutableHandle<Value> vp)
{
    if (!CheckRecursionLimitDontReport(cx))
        return false;
    if (nameLookup)
        return false;
    return GetPropertyNoGC(cx, obj, receiver, id, vp.address());
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE bool
NativeGetPropertyInline(JSContext* cx,
                        typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                        typename MaybeRooted<Value, allowGC>::HandleType receiver,
                        typename MaybeRooted<jsid, allowGC>::HandleType id,
                        IsNameLookup nameLookup,
                        typename MaybeRooted<Value, allowGC>::MutableHandleType vp)
{
    typename MaybeRooted<NativeObject*, allowGC>::RootType pobj(cx, obj);
    typename MaybeRooted<PropertyResult, allowGC>::RootType prop(cx);

    // This loop isn't explicit in the spec algorithm. See the comment on step
    // 4.d below.
    for (;;) {
        // Steps 2-3. ('done' is a SpiderMonkey-specific thing, used below.)
        bool done;
        if (!LookupOwnPropertyInline<allowGC>(cx, pobj, id, &prop, &done))
            return false;

        if (prop) {
            // Steps 5-8. Special case for dense elements because
            // GetExistingProperty doesn't support those.
            if (prop.isDenseOrTypedArrayElement()) {
                vp.set(pobj->getDenseOrTypedArrayElement(JSID_TO_INT(id)));
                return true;
            }

            typename MaybeRooted<Shape*, allowGC>::RootType shape(cx, prop.shape());
            return GetExistingProperty<allowGC>(cx, receiver, pobj, shape, vp);
        }

        // Steps 4.a-b. The check for 'done' on this next line is tricky.
        // done can be true in exactly these unlikely-sounding cases:
        // - We're looking up an element, and pobj is a TypedArray that
        //   doesn't have that many elements.
        // - We're being called from a resolve hook to assign to the property
        //   being resolved.
        // What they all have in common is we do not want to keep walking
        // the prototype chain.
        JSObject* proto = done ? nullptr : pobj->staticPrototype();

        // Step 4.c. The spec algorithm simply returns undefined if proto is
        // null, but see the comment on GetNonexistentProperty.
        if (!proto)
            return GetNonexistentProperty(cx, id, nameLookup, vp);

        // Step 4.d. If the prototype is also native, this step is a
        // recursive tail call, and we don't need to go through all the
        // plumbing of JSObject::getGeneric; the top of the loop is where
        // we're going to end up anyway. But if pobj is non-native,
        // that optimization would be incorrect.
        if (proto->getOpsGetProperty()) {
            RootedObject protoRoot(cx, proto);
            return GeneralizedGetProperty(cx, protoRoot, id, receiver, nameLookup, vp);
        }

        pobj = &proto->as<NativeObject>();
    }
}

bool
js::NativeGetProperty(JSContext* cx, HandleNativeObject obj, HandleValue receiver, HandleId id,
                      MutableHandleValue vp)
{
    return NativeGetPropertyInline<CanGC>(cx, obj, receiver, id, NotNameLookup, vp);
}

bool
js::NativeGetPropertyNoGC(JSContext* cx, NativeObject* obj, const Value& receiver, jsid id, Value* vp)
{
    AutoAssertNoException noexc(cx);
    return NativeGetPropertyInline<NoGC>(cx, obj, receiver, id, NotNameLookup, vp);
}

bool
js::GetNameBoundInEnvironment(JSContext* cx, HandleObject envArg, HandleId id, MutableHandleValue vp)
{
    // Manually unwrap 'with' environments to prevent looking up @@unscopables
    // twice.
    //
    // This is unfortunate because internally, the engine does not distinguish
    // HasProperty from HasBinding: both are implemented as a HasPropertyOp
    // hook on a WithEnvironmentObject.
    //
    // In the case of attempting to get the value of a binding already looked
    // up via BINDNAME, calling HasProperty on the WithEnvironmentObject is
    // equivalent to calling HasBinding a second time. This results in the
    // incorrect behavior of performing the @@unscopables check again.
    RootedObject env(cx, MaybeUnwrapWithEnvironment(envArg));
    RootedValue receiver(cx, ObjectValue(*env));
    if (env->getOpsGetProperty())
        return GeneralizedGetProperty(cx, env, id, receiver, NameLookup, vp);
    return NativeGetPropertyInline<CanGC>(cx, env.as<NativeObject>(), receiver, id, NameLookup, vp);
}


/*** [[Set]] *************************************************************************************/

static bool
MaybeReportUndeclaredVarAssignment(JSContext* cx, HandleString propname)
{
    unsigned flags;
    {
        jsbytecode* pc;
        JSScript* script = cx->currentScript(&pc, JSContext::ALLOW_CROSS_COMPARTMENT);
        if (!script)
            return true;

        // If the code is not strict and extra warnings aren't enabled, then no
        // check is needed.
        if (IsStrictSetPC(pc))
            flags = JSREPORT_ERROR;
        else if (cx->compartment()->behaviors().extraWarnings(cx))
            flags = JSREPORT_WARNING | JSREPORT_STRICT;
        else
            return true;
    }

    JSAutoByteString bytes;
    if (!bytes.encodeUtf8(cx, propname))
        return false;
    return JS_ReportErrorFlagsAndNumberUTF8(cx, flags, GetErrorMessage, nullptr,
                                            JSMSG_UNDECLARED_VAR, bytes.ptr());
}

/*
 * Finish assignment to a shapeful data property of a native object obj. This
 * conforms to no standard and there is a lot of legacy baggage here.
 */
static bool
NativeSetExistingDataProperty(JSContext* cx, HandleNativeObject obj, HandleShape shape,
                              HandleValue v, ObjectOpResult& result)
{
    MOZ_ASSERT(obj->isNative());
    MOZ_ASSERT(shape->isDataDescriptor());

    if (shape->hasDefaultSetter()) {
        if (shape->isDataProperty()) {
            // The common path. Standard data property.

            // Global properties declared with 'var' will be initially
            // defined with an undefined value, so don't treat the initial
            // assignments to such properties as overwrites.
            bool overwriting = !obj->is<GlobalObject>() || !obj->getSlot(shape->slot()).isUndefined();
            obj->setSlotWithType(cx, shape, v, overwriting);
            return result.succeed();
        }

        // Bizarre: shared (slotless) property that's writable but has no
        // JSSetterOp. JS code can't define such a property, but it can be done
        // through the JSAPI. Treat it as non-writable.
        return result.fail(JSMSG_GETTER_ONLY);
    }

    MOZ_ASSERT(!obj->is<WithEnvironmentObject>());  // See bug 1128681.

    RootedId id(cx, shape->propid());
    return CallJSSetterOp(cx, shape->setterOp(), obj, id, v, result);
}

/*
 * When a [[Set]] operation finds no existing property with the given id
 * or finds a writable data property on the prototype chain, we end up here.
 * Finish the [[Set]] by defining a new property on receiver.
 *
 * This implements ES6 draft rev 28, 9.1.9 [[Set]] steps 5.b-f, but it
 * is really old code and there are a few barnacles.
 */
bool
js::SetPropertyByDefining(JSContext* cx, HandleId id, HandleValue v, HandleValue receiverValue,
                          ObjectOpResult& result)
{
    // Step 5.b.
    if (!receiverValue.isObject())
        return result.fail(JSMSG_SET_NON_OBJECT_RECEIVER);
    RootedObject receiver(cx, &receiverValue.toObject());

    bool existing;
    {
        // Steps 5.c-d.
        Rooted<PropertyDescriptor> desc(cx);
        if (!GetOwnPropertyDescriptor(cx, receiver, id, &desc))
            return false;

        existing = !!desc.object();

        // Step 5.e.
        if (existing) {
            // Step 5.e.i.
            if (desc.isAccessorDescriptor())
                return result.fail(JSMSG_OVERWRITING_ACCESSOR);

            // Step 5.e.ii.
            if (!desc.writable())
                return result.fail(JSMSG_READ_ONLY);
        }
    }

    // Purge the property cache of now-shadowed id in receiver's environment chain.
    if (!ReshapeForShadowedProp(cx, receiver, id))
        return false;

    // Steps 5.e.iii-iv. and 5.f.i. Define the new data property.
    unsigned attrs =
        existing
        ? JSPROP_IGNORE_ENUMERATE | JSPROP_IGNORE_READONLY | JSPROP_IGNORE_PERMANENT
        : JSPROP_ENUMERATE;
    return DefineDataProperty(cx, receiver, id, v, attrs, result);
}

// When setting |id| for |receiver| and |obj| has no property for id, continue
// the search up the prototype chain.
bool
js::SetPropertyOnProto(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                       HandleValue receiver, ObjectOpResult& result)
{
    MOZ_ASSERT(!obj->is<ProxyObject>());

    RootedObject proto(cx, obj->staticPrototype());
    if (proto)
        return SetProperty(cx, proto, id, v, receiver, result);

    return SetPropertyByDefining(cx, id, v, receiver, result);
}

/*
 * Implement "the rest of" assignment to a property when no property receiver[id]
 * was found anywhere on the prototype chain.
 *
 * FIXME: This should be updated to follow ES6 draft rev 28, section 9.1.9,
 * steps 4.d.i and 5.
 */
template <QualifiedBool IsQualified>
static bool
SetNonexistentProperty(JSContext* cx, HandleNativeObject obj, HandleId id, HandleValue v,
                       HandleValue receiver, ObjectOpResult& result)
{
    if (!IsQualified && receiver.isObject() && receiver.toObject().isUnqualifiedVarObj()) {
        RootedString idStr(cx, JSID_TO_STRING(id));
        if (!MaybeReportUndeclaredVarAssignment(cx, idStr))
            return false;
    }

    // Pure optimization for the common case. There's no point performing the
    // lookup in step 5.c again, as our caller just did it for us.
    if (IsQualified && receiver.isObject() && obj == &receiver.toObject()) {
        // Ensure that a custom GetOwnPropertyOp, if present, doesn't
        // introduce additional properties which weren't previously found by
        // LookupOwnProperty.
#ifdef DEBUG
        if (GetOwnPropertyOp op = obj->getOpsGetOwnPropertyDescriptor()) {
            Rooted<PropertyDescriptor> desc(cx);
            if (!op(cx, obj, id, &desc))
                return false;

            MOZ_ASSERT(!desc.object());
        }
#endif

        // Step 5.e. Define the new data property.

        if (DefinePropertyOp op = obj->getOpsDefineProperty()) {
            // Purge the property cache of now-shadowed id in receiver's environment chain.
            if (!ReshapeForShadowedProp(cx, obj, id))
                return false;

            Rooted<PropertyDescriptor> desc(cx);
            desc.initFields(nullptr, v, JSPROP_ENUMERATE, nullptr, nullptr);

            MOZ_ASSERT(!cx->helperThread());
            return op(cx, obj, id, desc, result);
        }

        return DefineNonexistentProperty(cx, obj, id, v, result);
    }

    return SetPropertyByDefining(cx, id, v, receiver, result);
}

/*
 * Set an existing own property obj[index] that's a dense element or typed
 * array element.
 */
static bool
SetDenseOrTypedArrayElement(JSContext* cx, HandleNativeObject obj, uint32_t index, HandleValue v,
                            ObjectOpResult& result)
{
    if (obj->is<TypedArrayObject>()) {
        double d;
        if (!ToNumber(cx, v, &d))
            return false;

        // Silently do nothing for out-of-bounds sets, for consistency with
        // current behavior.  (ES6 currently says to throw for this in
        // strict mode code, so we may eventually need to change.)
        uint32_t len = obj->as<TypedArrayObject>().length();
        if (index < len)
            TypedArrayObject::setElement(obj->as<TypedArrayObject>(), index, d);
        return result.succeed();
    }

    if (WouldDefinePastNonwritableLength(obj, index))
        return result.fail(JSMSG_CANT_DEFINE_PAST_ARRAY_LENGTH);

    if (!obj->maybeCopyElementsForWrite(cx))
        return false;

    obj->setDenseElementWithType(cx, index, v);
    return result.succeed();
}

/*
 * Finish the assignment `receiver[id] = v` when an existing property (shape)
 * has been found on a native object (pobj). This implements ES6 draft rev 32
 * (2015 Feb 2) 9.1.9 steps 5 and 6.
 *
 * It is necessary to pass both id and shape because shape could be an implicit
 * dense or typed array element (i.e. not actually a pointer to a Shape).
 */
static bool
SetExistingProperty(JSContext* cx, HandleNativeObject obj, HandleId id, HandleValue v,
                    HandleValue receiver, HandleNativeObject pobj, Handle<PropertyResult> prop,
                    ObjectOpResult& result)
{
    // Step 5 for dense elements.
    if (prop.isDenseOrTypedArrayElement()) {
        // Step 5.a.
        if (pobj->getElementsHeader()->isFrozen())
            return result.fail(JSMSG_READ_ONLY);

        // Pure optimization for the common case:
        if (receiver.isObject() && pobj == &receiver.toObject())
            return SetDenseOrTypedArrayElement(cx, pobj, JSID_TO_INT(id), v, result);

        // Steps 5.b-f.
        return SetPropertyByDefining(cx, id, v, receiver, result);
    }

    // Step 5 for all other properties.
    RootedShape shape(cx, prop.shape());
    if (shape->isDataDescriptor()) {
        // Step 5.a.
        if (!shape->writable())
            return result.fail(JSMSG_READ_ONLY);

        // steps 5.c-f.
        if (receiver.isObject() && pobj == &receiver.toObject()) {
            // Pure optimization for the common case. There's no point performing
            // the lookup in step 5.c again, as our caller just did it for us. The
            // result is |shape|.

            // Steps 5.e.i-ii.
            if (pobj->is<ArrayObject>() && id == NameToId(cx->names().length)) {
                Rooted<ArrayObject*> arr(cx, &pobj->as<ArrayObject>());
                return ArraySetLength(cx, arr, id, shape->attributes(), v, result);
            }
            return NativeSetExistingDataProperty(cx, pobj, shape, v, result);
        }

        // SpiderMonkey special case: assigning to an inherited slotless
        // property causes the setter to be called, instead of shadowing,
        // unless the existing property is JSPROP_SHADOWABLE (see bug 552432).
        if (!shape->isDataProperty() && !shape->hasShadowable()) {
            // Even weirder sub-special-case: inherited slotless data property
            // with default setter. Wut.
            if (shape->hasDefaultSetter())
                return result.succeed();

            return CallJSSetterOp(cx, shape->setterOp(), obj, id, v, result);
        }

        // Shadow pobj[id] by defining a new data property receiver[id].
        // Delegate everything to SetPropertyByDefining.
        return SetPropertyByDefining(cx, id, v, receiver, result);
    }

    // Steps 6-11.
    MOZ_ASSERT(shape->isAccessorDescriptor());
    MOZ_ASSERT_IF(!shape->hasSetterObject(), shape->hasDefaultSetter());
    if (shape->hasDefaultSetter())
        return result.fail(JSMSG_GETTER_ONLY);

    RootedValue setter(cx, ObjectValue(*shape->setterObject()));
    if (!js::CallSetter(cx, receiver, setter, v))
        return false;

    return result.succeed();
}

template <QualifiedBool IsQualified>
bool
js::NativeSetProperty(JSContext* cx, HandleNativeObject obj, HandleId id, HandleValue v,
                      HandleValue receiver, ObjectOpResult& result)
{
    // Step numbers below reference ES6 rev 27 9.1.9, the [[Set]] internal
    // method for ordinary objects. We substitute our own names for these names
    // used in the spec: O -> pobj, P -> id, ownDesc -> shape.
    Rooted<PropertyResult> prop(cx);
    RootedNativeObject pobj(cx, obj);

    // This loop isn't explicit in the spec algorithm. See the comment on step
    // 4.c.i below. (There's a very similar loop in the NativeGetProperty
    // implementation, but unfortunately not similar enough to common up.)
    for (;;) {
        // Steps 2-3. ('done' is a SpiderMonkey-specific thing, used below.)
        bool done;
        if (!LookupOwnPropertyInline<CanGC>(cx, pobj, id, &prop, &done))
            return false;

        if (prop) {
            // Steps 5-6.
            return SetExistingProperty(cx, obj, id, v, receiver, pobj, prop, result);
        }

        // Steps 4.a-b. The check for 'done' on this next line is tricky.
        // done can be true in exactly these unlikely-sounding cases:
        // - We're looking up an element, and pobj is a TypedArray that
        //   doesn't have that many elements.
        // - We're being called from a resolve hook to assign to the property
        //   being resolved.
        // What they all have in common is we do not want to keep walking
        // the prototype chain.
        JSObject* proto = done ? nullptr : pobj->staticPrototype();
        if (!proto) {
            // Step 4.d.i (and step 5).
            return SetNonexistentProperty<IsQualified>(cx, obj, id, v, receiver, result);
        }

        // Step 4.c.i. If the prototype is also native, this step is a
        // recursive tail call, and we don't need to go through all the
        // plumbing of SetProperty; the top of the loop is where we're going to
        // end up anyway. But if pobj is non-native, that optimization would be
        // incorrect.
        if (!proto->isNative()) {
            // Unqualified assignments are not specified to go through [[Set]]
            // at all, but they do go through this function. So check for
            // unqualified assignment to a nonexistent global (a strict error).
            RootedObject protoRoot(cx, proto);
            if (!IsQualified) {
                bool found;
                if (!HasProperty(cx, protoRoot, id, &found))
                    return false;
                if (!found)
                    return SetNonexistentProperty<IsQualified>(cx, obj, id, v, receiver, result);
            }

            return SetProperty(cx, protoRoot, id, v, receiver, result);
        }
        pobj = &proto->as<NativeObject>();
    }
}

template bool
js::NativeSetProperty<Qualified>(JSContext* cx, HandleNativeObject obj, HandleId id,
                                 HandleValue value, HandleValue receiver,
                                 ObjectOpResult& result);

template bool
js::NativeSetProperty<Unqualified>(JSContext* cx, HandleNativeObject obj, HandleId id,
                                   HandleValue value, HandleValue receiver,
                                   ObjectOpResult& result);

bool
js::NativeSetElement(JSContext* cx, HandleNativeObject obj, uint32_t index, HandleValue v,
                     HandleValue receiver, ObjectOpResult& result)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    return NativeSetProperty<Qualified>(cx, obj, id, v, receiver, result);
}

/*** [[Delete]] **********************************************************************************/

// ES6 draft rev31 9.1.10 [[Delete]]
bool
js::NativeDeleteProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                         ObjectOpResult& result)
{
    // Steps 2-3.
    Rooted<PropertyResult> prop(cx);
    if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &prop))
        return false;

    // Step 4.
    if (!prop) {
        // If no property call the class's delProperty hook, passing succeeded
        // as the result parameter. This always succeeds when there is no hook.
        return CallJSDeletePropertyOp(cx, obj->getClass()->getDelProperty(), obj, id, result);
    }

    // Step 6. Non-configurable property.
    if (GetPropertyAttributes(obj, prop) & JSPROP_PERMANENT)
        return result.failCantDelete();

    if (!CallJSDeletePropertyOp(cx, obj->getClass()->getDelProperty(), obj, id, result))
        return false;
    if (!result)
        return true;

    // Step 5.
    if (prop.isDenseOrTypedArrayElement()) {
        // Typed array elements are non-configurable.
        MOZ_ASSERT(!obj->is<TypedArrayObject>());

        if (!obj->maybeCopyElementsForWrite(cx))
            return false;

        obj->setDenseElementHole(cx, JSID_TO_INT(id));
    } else {
        if (!NativeObject::removeProperty(cx, obj, id))
            return false;
    }

    return SuppressDeletedProperty(cx, obj, id);
}
