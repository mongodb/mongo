/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/NativeObject-inl.h"

#include "mozilla/CheckedInt.h"

#include "jswatchpoint.h"

#include "gc/Marking.h"
#include "js/Value.h"
#include "vm/Debugger.h"
#include "vm/TypedArrayCommon.h"

#include "jsobjinlines.h"

#include "vm/ArrayObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

using JS::GenericNaN;
using mozilla::DebugOnly;
using mozilla::RoundUpPow2;

PropDesc::PropDesc()
{
    setUndefined();
}

void
PropDesc::setUndefined()
{
    value_ = UndefinedValue();
    get_ = UndefinedValue();
    set_ = UndefinedValue();
    attrs = 0;
    hasGet_ = false;
    hasSet_ = false;
    hasValue_ = false;
    hasWritable_ = false;
    hasEnumerable_ = false;
    hasConfigurable_ = false;

    isUndefined_ = true;
}

bool
PropDesc::checkGetter(JSContext* cx)
{
    if (hasGet_) {
        if (!IsCallable(get_) && !get_.isUndefined()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_BAD_GET_SET_FIELD,
                                 js_getter_str);
            return false;
        }
    }
    return true;
}

bool
PropDesc::checkSetter(JSContext* cx)
{
    if (hasSet_) {
        if (!IsCallable(set_) && !set_.isUndefined()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_BAD_GET_SET_FIELD,
                                 js_setter_str);
            return false;
        }
    }
    return true;
}

static const ObjectElements emptyElementsHeader(0, 0);

/* Objects with no elements share one empty set of elements. */
HeapSlot* const js::emptyObjectElements =
    reinterpret_cast<HeapSlot*>(uintptr_t(&emptyElementsHeader) + sizeof(ObjectElements));

#ifdef DEBUG

bool
NativeObject::canHaveNonEmptyElements()
{
    return !IsAnyTypedArray(this);
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
    MOZ_ASSERT(elementsHeapPtr != emptyObjectElements);

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
ObjectElements::MakeElementsCopyOnWrite(ExclusiveContext* cx, NativeObject* obj)
{
    static_assert(sizeof(HeapSlot) >= sizeof(HeapPtrObject),
                  "there must be enough room for the owner object pointer at "
                  "the end of the elements");
    if (!obj->ensureElements(cx, obj->getDenseInitializedLength() + 1))
        return false;

    ObjectElements* header = obj->getElementsHeader();

    // Note: this method doesn't update type information to indicate that the
    // elements might be copy on write. Handling this is left to the caller.
    MOZ_ASSERT(!header->isCopyOnWrite());
    header->flags |= COPY_ON_WRITE;

    header->ownerObject().init(obj);
    return true;
}

#ifdef DEBUG
void
js::NativeObject::checkShapeConsistency()
{
    static int throttle = -1;
    if (throttle < 0) {
        if (const char* var = getenv("JS_CHECK_SHAPE_THROTTLE"))
            throttle = atoi(var);
        if (throttle < 0)
            throttle = 0;
    }
    if (throttle == 0)
        return;

    MOZ_ASSERT(isNative());

    Shape* shape = lastProperty();
    Shape* prev = nullptr;

    if (inDictionaryMode()) {
        MOZ_ASSERT(shape->hasTable());

        ShapeTable& table = shape->table();
        for (uint32_t fslot = table.freeList();
             fslot != SHAPE_INVALID_SLOT;
             fslot = getSlot(fslot).toPrivateUint32())
        {
            MOZ_ASSERT(fslot < slotSpan());
        }

        for (int n = throttle; --n >= 0 && shape->parent; shape = shape->parent) {
            MOZ_ASSERT_IF(lastProperty() != shape, !shape->hasTable());

            ShapeTable::Entry& entry = table.search(shape->propid(), false);
            MOZ_ASSERT(entry.shape() == shape);
        }

        shape = lastProperty();
        for (int n = throttle; --n >= 0 && shape; shape = shape->parent) {
            MOZ_ASSERT_IF(shape->slot() != SHAPE_INVALID_SLOT, shape->slot() < slotSpan());
            if (!prev) {
                MOZ_ASSERT(lastProperty() == shape);
                MOZ_ASSERT(shape->listp == &shape_);
            } else {
                MOZ_ASSERT(shape->listp == &prev->parent);
            }
            prev = shape;
        }
    } else {
        for (int n = throttle; --n >= 0 && shape->parent; shape = shape->parent) {
            if (shape->hasTable()) {
                ShapeTable& table = shape->table();
                MOZ_ASSERT(shape->parent);
                for (Shape::Range<NoGC> r(shape); !r.empty(); r.popFront()) {
                    ShapeTable::Entry& entry = table.search(r.front().propid(), false);
                    MOZ_ASSERT(entry.shape() == &r.front());
                }
            }
            if (prev) {
                MOZ_ASSERT(prev->maybeSlot() >= shape->maybeSlot());
                shape->kids.checkConsistency(prev);
            }
            prev = shape;
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
    HeapSlot* fixedStart, *fixedEnd, *slotsStart, *slotsEnd;
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
    HeapSlot* fixedStart, *fixedEnd, *slotsStart, *slotsEnd;
    getSlotRange(start, length, &fixedStart, &fixedEnd, &slotsStart, &slotsEnd);
    for (HeapSlot* sp = fixedStart; sp < fixedEnd; sp++)
        sp->init(this, HeapSlot::Slot, start++, *vector++);
    for (HeapSlot* sp = slotsStart; sp < slotsEnd; sp++)
        sp->init(this, HeapSlot::Slot, start++, *vector++);
}

void
js::NativeObject::copySlotRange(uint32_t start, const Value* vector, uint32_t length)
{
    JS::Zone* zone = this->zone();
    HeapSlot* fixedStart, *fixedEnd, *slotsStart, *slotsEnd;
    getSlotRange(start, length, &fixedStart, &fixedEnd, &slotsStart, &slotsEnd);
    for (HeapSlot* sp = fixedStart; sp < fixedEnd; sp++)
        sp->set(zone, this, HeapSlot::Slot, start++, *vector++);
    for (HeapSlot* sp = slotsStart; sp < slotsEnd; sp++)
        sp->set(zone, this, HeapSlot::Slot, start++, *vector++);
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
js::NativeObject::lookup(ExclusiveContext* cx, jsid id)
{
    MOZ_ASSERT(isNative());
    ShapeTable::Entry* entry;
    return Shape::search(cx, lastProperty(), id, &entry);
}

Shape*
js::NativeObject::lookupPure(jsid id)
{
    MOZ_ASSERT(isNative());
    return Shape::searchNoHashify(lastProperty(), id);
}

uint32_t
js::NativeObject::dynamicSlotsCount(uint32_t nfixed, uint32_t span, const Class* clasp)
{
    if (span <= nfixed)
        return 0;
    span -= nfixed;

    // Increase the slots to SLOT_CAPACITY_MIN to decrease the likelihood
    // the dynamic slots need to get increased again. ArrayObjects ignore
    // this because slots are uncommon in that case.
    if (clasp != &ArrayObject::class_ && span <= SLOT_CAPACITY_MIN)
        return SLOT_CAPACITY_MIN;

    uint32_t slots = mozilla::RoundUpPow2(span);
    MOZ_ASSERT(slots >= span);
    return slots;
}

void
PropDesc::trace(JSTracer* trc)
{
    gc::MarkValueRoot(trc, &value_, "PropDesc value");
    gc::MarkValueRoot(trc, &get_, "PropDesc get");
    gc::MarkValueRoot(trc, &set_, "PropDesc set");
}

inline bool
NativeObject::updateSlotsForSpan(ExclusiveContext* cx, size_t oldSpan, size_t newSpan)
{
    MOZ_ASSERT(oldSpan != newSpan);

    size_t oldCount = dynamicSlotsCount(numFixedSlots(), oldSpan, getClass());
    size_t newCount = dynamicSlotsCount(numFixedSlots(), newSpan, getClass());

    if (oldSpan < newSpan) {
        if (oldCount < newCount && !growSlots(cx, oldCount, newCount))
            return false;

        if (newSpan == oldSpan + 1)
            initSlotUnchecked(oldSpan, UndefinedValue());
        else
            initializeSlotRange(oldSpan, newSpan - oldSpan);
    } else {
        /* Trigger write barriers on the old slots before reallocating. */
        prepareSlotRangeForOverwrite(newSpan, oldSpan);
        invalidateSlotRange(newSpan, oldSpan - newSpan);

        if (oldCount > newCount)
            shrinkSlots(cx, oldCount, newCount);
    }

    return true;
}

bool
NativeObject::setLastProperty(ExclusiveContext* cx, Shape* shape)
{
    MOZ_ASSERT(!inDictionaryMode());
    MOZ_ASSERT(!shape->inDictionary());
    MOZ_ASSERT(shape->compartment() == compartment());
    MOZ_ASSERT(shape->numFixedSlots() == numFixedSlots());
    MOZ_ASSERT(shape->getObjectClass() == getClass());

    size_t oldSpan = lastProperty()->slotSpan();
    size_t newSpan = shape->slotSpan();

    if (oldSpan == newSpan) {
        shape_ = shape;
        return true;
    }

    if (!updateSlotsForSpan(cx, oldSpan, newSpan))
        return false;

    shape_ = shape;
    return true;
}

void
NativeObject::setLastPropertyShrinkFixedSlots(Shape* shape)
{
    MOZ_ASSERT(!inDictionaryMode());
    MOZ_ASSERT(!shape->inDictionary());
    MOZ_ASSERT(shape->compartment() == compartment());
    MOZ_ASSERT(lastProperty()->slotSpan() == shape->slotSpan());
    MOZ_ASSERT(shape->getObjectClass() == getClass());

    DebugOnly<size_t> oldFixed = numFixedSlots();
    DebugOnly<size_t> newFixed = shape->numFixedSlots();
    MOZ_ASSERT(newFixed < oldFixed);
    MOZ_ASSERT(shape->slotSpan() <= oldFixed);
    MOZ_ASSERT(shape->slotSpan() <= newFixed);
    MOZ_ASSERT(dynamicSlotsCount(oldFixed, shape->slotSpan(), getClass()) == 0);
    MOZ_ASSERT(dynamicSlotsCount(newFixed, shape->slotSpan(), getClass()) == 0);

    shape_ = shape;
}

void
NativeObject::setLastPropertyMakeNonNative(Shape* shape)
{
    MOZ_ASSERT(!inDictionaryMode());
    MOZ_ASSERT(!shape->getObjectClass()->isNative());
    MOZ_ASSERT(shape->compartment() == compartment());
    MOZ_ASSERT(shape->slotSpan() == 0);
    MOZ_ASSERT(shape->numFixedSlots() == 0);
    MOZ_ASSERT(!hasDynamicElements());
    MOZ_ASSERT(!hasDynamicSlots());

    shape_ = shape;
}

void
NativeObject::setLastPropertyMakeNative(ExclusiveContext* cx, Shape* shape)
{
    MOZ_ASSERT(getClass()->isNative());
    MOZ_ASSERT(!lastProperty()->isNative());
    MOZ_ASSERT(shape->isNative());
    MOZ_ASSERT(!inDictionaryMode());
    MOZ_ASSERT(!shape->inDictionary());
    MOZ_ASSERT(shape->compartment() == compartment());

    shape_ = shape;
    slots_ = nullptr;
    elements_ = emptyObjectElements;

    size_t oldSpan = shape->numFixedSlots();
    size_t newSpan = shape->slotSpan();

    // A failure at this point will leave the object as a mutant, and we
    // can't recover.
    if (oldSpan != newSpan && !updateSlotsForSpan(cx, oldSpan, newSpan))
        CrashAtUnhandlableOOM("NativeObject::setLastPropertyMakeNative");
}

bool
NativeObject::setSlotSpan(ExclusiveContext* cx, uint32_t span)
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

// This will not run the garbage collector.  If a nursery cannot accomodate the slot array
// an attempt will be made to place the array in the tenured area.
static HeapSlot*
AllocateSlots(ExclusiveContext* cx, JSObject* obj, uint32_t nslots)
{
    if (cx->isJSContext())
        return cx->asJSContext()->runtime()->gc.nursery.allocateSlots(obj, nslots);
    return obj->zone()->pod_malloc<HeapSlot>(nslots);
}

// This will not run the garbage collector.  If a nursery cannot accomodate the slot array
// an attempt will be made to place the array in the tenured area.
//
// If this returns null then the old slots will be left alone.
static HeapSlot*
ReallocateSlots(ExclusiveContext* cx, JSObject* obj, HeapSlot* oldSlots,
                uint32_t oldCount, uint32_t newCount)
{
    if (cx->isJSContext()) {
        return cx->asJSContext()->runtime()->gc.nursery.reallocateSlots(obj, oldSlots,
                                                                        oldCount, newCount);
    }
    return obj->zone()->pod_realloc<HeapSlot>(oldSlots, oldCount, newCount);
}

bool
NativeObject::growSlots(ExclusiveContext* cx, uint32_t oldCount, uint32_t newCount)
{
    MOZ_ASSERT(newCount > oldCount);
    MOZ_ASSERT_IF(!is<ArrayObject>(), newCount >= SLOT_CAPACITY_MIN);

    /*
     * Slot capacities are determined by the span of allocated objects. Due to
     * the limited number of bits to store shape slots, object growth is
     * throttled well before the slot capacity can overflow.
     */
    NativeObject::slotsSizeMustNotOverflow();
    MOZ_ASSERT(newCount < NELEMENTS_LIMIT);

    if (!oldCount) {
        slots_ = AllocateSlots(cx, this, newCount);
        if (!slots_)
            return false;
        Debug_SetSlotRangeToCrashOnTouch(slots_, newCount);
        return true;
    }

    HeapSlot* newslots = ReallocateSlots(cx, this, slots_, oldCount, newCount);
    if (!newslots)
        return false;  /* Leave slots at its old size. */

    slots_ = newslots;

    Debug_SetSlotRangeToCrashOnTouch(slots_ + oldCount, newCount - oldCount);

    return true;
}

static void
FreeSlots(ExclusiveContext* cx, HeapSlot* slots)
{
    // Note: threads without a JSContext do not have access to GGC nursery allocated things.
    if (cx->isJSContext())
        return cx->asJSContext()->runtime()->gc.nursery.freeSlots(slots);
    js_free(slots);
}

void
NativeObject::shrinkSlots(ExclusiveContext* cx, uint32_t oldCount, uint32_t newCount)
{
    MOZ_ASSERT(newCount < oldCount);

    if (newCount == 0) {
        FreeSlots(cx, slots_);
        slots_ = nullptr;
        return;
    }

    MOZ_ASSERT_IF(!is<ArrayObject>(), newCount >= SLOT_CAPACITY_MIN);

    HeapSlot* newslots = ReallocateSlots(cx, this, slots_, oldCount, newCount);
    if (!newslots)
        return;  /* Leave slots at its old size. */

    slots_ = newslots;
}

/* static */ bool
NativeObject::sparsifyDenseElement(ExclusiveContext* cx, HandleNativeObject obj, uint32_t index)
{
    if (!obj->maybeCopyElementsForWrite(cx))
        return false;

    RootedValue value(cx, obj->getDenseElement(index));
    MOZ_ASSERT(!value.isMagic(JS_ELEMENTS_HOLE));

    removeDenseElementForSparseIndex(cx, obj, index);

    uint32_t slot = obj->slotSpan();
    if (!obj->addDataProperty(cx, INT_TO_JSID(index), slot, JSPROP_ENUMERATE)) {
        obj->setDenseElement(index, value);
        return false;
    }

    MOZ_ASSERT(slot == obj->slotSpan() - 1);
    obj->initSlot(slot, value);

    return true;
}

/* static */ bool
NativeObject::sparsifyDenseElements(js::ExclusiveContext* cx, HandleNativeObject obj)
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
        obj->setDenseInitializedLength(0);

    /*
     * Reduce storage for dense elements which are now holes. Explicitly mark
     * the elements capacity as zero, so that any attempts to add dense
     * elements will be caught in ensureDenseElements.
     */
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

    if (requiredCapacity >= NELEMENTS_LIMIT)
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

/* static */ NativeObject::EnsureDenseResult
NativeObject::maybeDensifySparseElements(js::ExclusiveContext* cx, HandleNativeObject obj)
{
    /*
     * Wait until after the object goes into dictionary mode, which must happen
     * when sparsely packing any array with more than MIN_SPARSE_INDEX elements
     * (see PropertyTree::MAX_HEIGHT).
     */
    if (!obj->inDictionaryMode())
        return ED_SPARSE;

    /*
     * Only measure the number of indexed properties every log(n) times when
     * populating the object.
     */
    uint32_t slotSpan = obj->slotSpan();
    if (slotSpan != RoundUpPow2(slotSpan))
        return ED_SPARSE;

    /* Watch for conditions under which an object's elements cannot be dense. */
    if (!obj->nonProxyIsExtensible() || obj->watched())
        return ED_SPARSE;

    /*
     * The indexes in the object need to be sufficiently dense before they can
     * be converted to dense mode.
     */
    uint32_t numDenseElements = 0;
    uint32_t newInitializedLength = 0;

    RootedShape shape(cx, obj->lastProperty());
    while (!shape->isEmptyShape()) {
        uint32_t index;
        if (js_IdIsIndex(shape->propid(), &index)) {
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
                return ED_SPARSE;
            }
        }
        shape = shape->previous();
    }

    if (numDenseElements * SPARSE_DENSITY_RATIO < newInitializedLength)
        return ED_SPARSE;

    if (newInitializedLength >= NELEMENTS_LIMIT)
        return ED_SPARSE;

    /*
     * This object meets all necessary restrictions, convert all indexed
     * properties into dense elements.
     */

    if (!obj->maybeCopyElementsForWrite(cx))
        return ED_FAILED;

    if (newInitializedLength > obj->getDenseCapacity()) {
        if (!obj->growElements(cx, newInitializedLength))
            return ED_FAILED;
    }

    obj->ensureDenseInitializedLength(cx, newInitializedLength, 0);

    RootedValue value(cx);

    shape = obj->lastProperty();
    while (!shape->isEmptyShape()) {
        jsid id = shape->propid();
        uint32_t index;
        if (js_IdIsIndex(id, &index)) {
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
                if (!obj->removeProperty(cx, id))
                    return ED_FAILED;
            } else {
                if (!obj->removeProperty(cx, id))
                    return ED_FAILED;
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
    if (!obj->clearFlag(cx, BaseShape::INDEXED))
        return ED_FAILED;

    return ED_OK;
}

// This will not run the garbage collector.  If a nursery cannot accomodate the element array
// an attempt will be made to place the array in the tenured area.
static ObjectElements*
AllocateElements(ExclusiveContext* cx, JSObject* obj, uint32_t nelems)
{
    if (cx->isJSContext())
        return cx->asJSContext()->runtime()->gc.nursery.allocateElements(obj, nelems);
    return reinterpret_cast<js::ObjectElements*>(obj->zone()->pod_malloc<HeapSlot>(nelems));
}

// This will not run the garbage collector.  If a nursery cannot accomodate the element array
// an attempt will be made to place the array in the tenured area.
static ObjectElements*
ReallocateElements(ExclusiveContext* cx, JSObject* obj, ObjectElements* oldHeader,
                   uint32_t oldCount, uint32_t newCount)
{
    if (cx->isJSContext()) {
        return cx->asJSContext()->runtime()->gc.nursery.reallocateElements(obj, oldHeader,
                                                                           oldCount, newCount);
    }
    return reinterpret_cast<js::ObjectElements*>(
            obj->zone()->pod_realloc<HeapSlot>(reinterpret_cast<HeapSlot*>(oldHeader),
                                               oldCount, newCount));
}

// Round up |reqAllocated| to a good size. Up to 1 Mebi (i.e. 1,048,576) the
// slot count is usually a power-of-two:
//
//   8, 16, 32, 64, ..., 256 Ki, 512 Ki, 1 Mi
//
// Beyond that, we use this formula:
//
//   count(n+1) = Math.ceil(count(n) * 1.125)
//
// where |count(n)| is the size of the nth bucket measured in MiSlots.
//
// These counts lets us add N elements to an array in amortized O(N) time.
// Having the second class means that for bigger arrays the constant factor is
// higher, but we waste less space.
//
// There is one exception to the above rule: for the power-of-two cases, if the
// chosen capacity would be 2/3 or more of the array's length, the chosen
// capacity is adjusted (up or down) to be equal to the array's length
// (assuming length is at least as large as the required capacity). This avoids
// the allocation of excess elements which are unlikely to be needed, either in
// this resizing or a subsequent one. The 2/3 factor is chosen so that
// exceptional resizings will at most triple the capacity, as opposed to the
// usual doubling.
//
/* static */ uint32_t
NativeObject::goodAllocated(uint32_t reqAllocated, uint32_t length = 0)
{
    static const uint32_t Mebi = 1024 * 1024;

    // This table was generated with this JavaScript code and a small amount
    // subsequent reformatting:
    //
    //   for (let n = 1, i = 0; i < 57; i++) {
    //     print((n * 1024 * 1024) + ', ');
    //     n = Math.ceil(n * 1.125);
    //   }
    //   print('0');
    //
    // The final element is a sentinel value.
    static const uint32_t BigBuckets[] = {
        1048576, 2097152, 3145728, 4194304, 5242880, 6291456, 7340032, 8388608,
        9437184, 11534336, 13631488, 15728640, 17825792, 20971520, 24117248,
        27262976, 31457280, 35651584, 40894464, 46137344, 52428800, 59768832,
        68157440, 77594624, 88080384, 99614720, 112197632, 126877696,
        143654912, 162529280, 183500800, 206569472, 232783872, 262144000,
        295698432, 333447168, 375390208, 422576128, 476053504, 535822336,
        602931200, 678428672, 763363328, 858783744, 966787072, 1088421888,
        1224736768, 1377828864, 1550843904, 1744830464, 1962934272, 2208301056,
        2485125120, 2796552192, 3146776576, 3541041152, 3984588800, 0
    };

    // This code relies very much on |goodAllocated| being a uint32_t.
    uint32_t goodAllocated = reqAllocated;
    if (goodAllocated < Mebi) {
        goodAllocated = RoundUpPow2(goodAllocated);

        // Look for the abovementioned exception.
        uint32_t goodCapacity = goodAllocated - ObjectElements::VALUES_PER_HEADER;
        uint32_t reqCapacity = reqAllocated - ObjectElements::VALUES_PER_HEADER;
        if (length >= reqCapacity && goodCapacity > (length / 3) * 2)
            goodAllocated = length + ObjectElements::VALUES_PER_HEADER;

        if (goodAllocated < SLOT_CAPACITY_MIN)
            goodAllocated = SLOT_CAPACITY_MIN;

    } else {
        uint32_t i = 0;
        while (true) {
            uint32_t b = BigBuckets[i++];
            if (b >= goodAllocated) {
                // Found the first bucket greater than or equal to
                // |goodAllocated|.
                goodAllocated = b;
                break;
            } else if (b == 0) {
                // Hit the end; return the maximum possible goodAllocated.
                goodAllocated = 0xffffffff;
                break;
            }
        }
    }

    return goodAllocated;
}

bool
NativeObject::growElements(ExclusiveContext* cx, uint32_t reqCapacity)
{
    MOZ_ASSERT(nonProxyIsExtensible());
    MOZ_ASSERT(canHaveNonEmptyElements());
    if (denseElementsAreCopyOnWrite())
        MOZ_CRASH();

    uint32_t oldCapacity = getDenseCapacity();
    MOZ_ASSERT(oldCapacity < reqCapacity);

    using mozilla::CheckedInt;

    CheckedInt<uint32_t> checkedOldAllocated =
        CheckedInt<uint32_t>(oldCapacity) + ObjectElements::VALUES_PER_HEADER;
    CheckedInt<uint32_t> checkedReqAllocated =
        CheckedInt<uint32_t>(reqCapacity) + ObjectElements::VALUES_PER_HEADER;
    if (!checkedOldAllocated.isValid() || !checkedReqAllocated.isValid())
        return false;

    uint32_t reqAllocated = checkedReqAllocated.value();
    uint32_t oldAllocated = checkedOldAllocated.value();

    uint32_t newAllocated;
    if (is<ArrayObject>() && !as<ArrayObject>().lengthIsWritable()) {
        MOZ_ASSERT(reqCapacity <= as<ArrayObject>().length());
        // Preserve the |capacity <= length| invariant for arrays with
        // non-writable length.  See also js::ArraySetLength which initially
        // enforces this requirement.
        newAllocated = reqAllocated;
    } else {
        newAllocated = goodAllocated(reqAllocated, getElementsHeader()->length);
    }

    uint32_t newCapacity = newAllocated - ObjectElements::VALUES_PER_HEADER;
    MOZ_ASSERT(newCapacity > oldCapacity && newCapacity >= reqCapacity);

    // Don't let nelements get close to wrapping around uint32_t.
    if (newCapacity >= NELEMENTS_LIMIT)
        return false;

    uint32_t initlen = getDenseInitializedLength();

    ObjectElements* newheader;
    if (hasDynamicElements()) {
        newheader = ReallocateElements(cx, this, getElementsHeader(), oldAllocated, newAllocated);
        if (!newheader)
            return false;   // Leave elements at its old size.
    } else {
        newheader = AllocateElements(cx, this, newAllocated);
        if (!newheader)
            return false;   // Leave elements at its old size.
        js_memcpy(newheader, getElementsHeader(),
                  (ObjectElements::VALUES_PER_HEADER + initlen) * sizeof(Value));
    }

    newheader->capacity = newCapacity;
    elements_ = newheader->elements();

    Debug_SetSlotRangeToCrashOnTouch(elements_ + initlen, newCapacity - initlen);

    return true;
}

void
NativeObject::shrinkElements(ExclusiveContext* cx, uint32_t reqCapacity)
{
    MOZ_ASSERT(canHaveNonEmptyElements());
    if (denseElementsAreCopyOnWrite())
        MOZ_CRASH();

    if (!hasDynamicElements())
        return;

    uint32_t oldCapacity = getDenseCapacity();
    MOZ_ASSERT(reqCapacity < oldCapacity);

    uint32_t oldAllocated = oldCapacity + ObjectElements::VALUES_PER_HEADER;
    uint32_t reqAllocated = reqCapacity + ObjectElements::VALUES_PER_HEADER;
    uint32_t newAllocated = goodAllocated(reqAllocated);
    if (newAllocated == oldAllocated)
        return;  // Leave elements at its old size.

    MOZ_ASSERT(newAllocated > ObjectElements::VALUES_PER_HEADER);
    uint32_t newCapacity = newAllocated - ObjectElements::VALUES_PER_HEADER;

    ObjectElements* newheader = ReallocateElements(cx, this, getElementsHeader(),
                                                   oldAllocated, newAllocated);
    if (!newheader) {
        cx->recoverFromOutOfMemory();
        return;  // Leave elements at its old size.
    }

    newheader->capacity = newCapacity;
    elements_ = newheader->elements();
}

/* static */ bool
NativeObject::CopyElementsForWrite(ExclusiveContext* cx, NativeObject* obj)
{
    MOZ_ASSERT(obj->denseElementsAreCopyOnWrite());

    // The original owner of a COW elements array should never be modified.
    MOZ_ASSERT(obj->getElementsHeader()->ownerObject() != obj);

    uint32_t initlen = obj->getDenseInitializedLength();
    uint32_t allocated = initlen + ObjectElements::VALUES_PER_HEADER;
    uint32_t newAllocated = goodAllocated(allocated);

    uint32_t newCapacity = newAllocated - ObjectElements::VALUES_PER_HEADER;

    if (newCapacity >= NELEMENTS_LIMIT)
        return false;

    JSObject::writeBarrierPre(obj->getElementsHeader()->ownerObject());

    ObjectElements* newheader = AllocateElements(cx, obj, newAllocated);
    if (!newheader)
        return false;
    js_memcpy(newheader, obj->getElementsHeader(),
              (ObjectElements::VALUES_PER_HEADER + initlen) * sizeof(Value));

    newheader->capacity = newCapacity;
    newheader->clearCopyOnWrite();
    obj->elements_ = newheader->elements();

    Debug_SetSlotRangeToCrashOnTouch(obj->elements_ + initlen, newCapacity - initlen);

    return true;
}

/* static */ bool
NativeObject::allocSlot(ExclusiveContext* cx, HandleNativeObject obj, uint32_t* slotp)
{
    uint32_t slot = obj->slotSpan();
    MOZ_ASSERT(slot >= JSSLOT_FREE(obj->getClass()));

    /*
     * If this object is in dictionary mode, try to pull a free slot from the
     * shape table's slot-number freelist.
     */
    if (obj->inDictionaryMode()) {
        ShapeTable& table = obj->lastProperty()->table();
        uint32_t last = table.freeList();
        if (last != SHAPE_INVALID_SLOT) {
#ifdef DEBUG
            MOZ_ASSERT(last < slot);
            uint32_t next = obj->getSlot(last).toPrivateUint32();
            MOZ_ASSERT_IF(next != SHAPE_INVALID_SLOT, next < slot);
#endif

            *slotp = last;

            const Value& vref = obj->getSlot(last);
            table.setFreeList(vref.toPrivateUint32());
            obj->setSlot(last, UndefinedValue());
            return true;
        }
    }

    if (slot >= SHAPE_MAXIMUM_SLOT) {
        js_ReportOutOfMemory(cx);
        return false;
    }

    *slotp = slot;

    if (obj->inDictionaryMode() && !obj->setSlotSpan(cx, slot + 1))
        return false;

    return true;
}

void
NativeObject::freeSlot(uint32_t slot)
{
    MOZ_ASSERT(slot < slotSpan());

    if (inDictionaryMode()) {
        ShapeTable& table = lastProperty()->table();
        uint32_t last = table.freeList();

        /* Can't afford to check the whole freelist, but let's check the head. */
        MOZ_ASSERT_IF(last != SHAPE_INVALID_SLOT, last < slotSpan() && last != slot);

        /*
         * Place all freed slots other than reserved slots (bug 595230) on the
         * dictionary's free list.
         */
        if (JSSLOT_FREE(getClass()) <= slot) {
            MOZ_ASSERT_IF(last != SHAPE_INVALID_SLOT, last < slotSpan());
            setSlot(slot, PrivateUint32Value(last));
            table.setFreeList(slot);
            return;
        }
    }
    setSlot(slot, UndefinedValue());
}

Shape*
NativeObject::addDataProperty(ExclusiveContext* cx, jsid idArg, uint32_t slot, unsigned attrs)
{
    MOZ_ASSERT(!(attrs & (JSPROP_GETTER | JSPROP_SETTER)));
    RootedNativeObject self(cx, this);
    RootedId id(cx, idArg);
    return addProperty(cx, self, id, nullptr, nullptr, slot, attrs, 0);
}

Shape*
NativeObject::addDataProperty(ExclusiveContext* cx, HandlePropertyName name,
                          uint32_t slot, unsigned attrs)
{
    MOZ_ASSERT(!(attrs & (JSPROP_GETTER | JSPROP_SETTER)));
    RootedNativeObject self(cx, this);
    RootedId id(cx, NameToId(name));
    return addProperty(cx, self, id, nullptr, nullptr, slot, attrs, 0);
}

/*
 * Backward compatibility requires allowing addProperty hooks to mutate the
 * nominal initial value of a slotful property, while GC safety wants that
 * value to be stored before the call-out through the hook.  Optimize to do
 * both while saving cycles for classes that stub their addProperty hook.
 */
static inline bool
CallAddPropertyHook(ExclusiveContext* cx, HandleNativeObject obj, HandleShape shape,
                    HandleValue nominal)
{
    if (JSPropertyOp addProperty = obj->getClass()->addProperty) {
        MOZ_ASSERT(addProperty != JS_PropertyStub);

        if (!cx->shouldBeJSContext())
            return false;

        /* Make a local copy of value so addProperty can mutate its inout parameter. */
        RootedValue value(cx, nominal);

        Rooted<jsid> id(cx, shape->propid());
        if (!CallJSPropertyOp(cx->asJSContext(), addProperty, obj, id, &value)) {
            obj->removeProperty(cx, shape->propid());
            return false;
        }
        if (value.get() != nominal) {
            if (shape->hasSlot())
                obj->setSlotWithType(cx, shape, value);
        }
    }
    return true;
}

static inline bool
CallAddPropertyHookDense(ExclusiveContext* cx, HandleNativeObject obj, uint32_t index,
                         HandleValue nominal)
{
    /* Inline addProperty for array objects. */
    if (obj->is<ArrayObject>()) {
        ArrayObject* arr = &obj->as<ArrayObject>();
        uint32_t length = arr->length();
        if (index >= length)
            arr->setLength(cx, index + 1);
        return true;
    }

    if (JSPropertyOp addProperty = obj->getClass()->addProperty) {
        MOZ_ASSERT(addProperty != JS_PropertyStub);

        if (!cx->shouldBeJSContext())
            return false;

        if (!obj->maybeCopyElementsForWrite(cx))
            return false;

        /* Make a local copy of value so addProperty can mutate its inout parameter. */
        RootedValue value(cx, nominal);

        Rooted<jsid> id(cx, INT_TO_JSID(index));
        if (!CallJSPropertyOp(cx->asJSContext(), addProperty, obj, id, &value)) {
            obj->setDenseElementHole(cx, index);
            return false;
        }
        if (value.get() != nominal)
            obj->setDenseElementWithType(cx, index, value);
    }

    return true;
}

static bool
UpdateShapeTypeAndValue(ExclusiveContext* cx, NativeObject* obj, Shape* shape, const Value& value)
{
    jsid id = shape->propid();
    if (shape->hasSlot()) {
        obj->setSlotWithType(cx, shape, value, /* overwriting = */ false);

        // Per the acquired properties analysis, when the shape of a partially
        // initialized object is changed to its fully initialized shape, its
        // group can be updated as well.
        if (TypeNewScript* newScript = obj->groupRaw()->newScript()) {
            if (newScript->initializedShape() == shape)
                obj->setGroup(newScript->initializedGroup());
        }
    }
    if (!shape->hasSlot() || !shape->hasDefaultGetter() || !shape->hasDefaultSetter())
        MarkTypePropertyNonData(cx, obj, id);
    if (!shape->writable())
        MarkTypePropertyNonWritable(cx, obj, id);
    return true;
}

static bool
NativeSet(JSContext* cx, HandleNativeObject obj, HandleObject receiver,
          HandleShape shape, bool strict, MutableHandleValue vp);

static inline bool
DefinePropertyOrElement(ExclusiveContext* cx, HandleNativeObject obj, HandleId id,
                        PropertyOp getter, StrictPropertyOp setter,
                        unsigned attrs, HandleValue value,
                        bool callSetterAfterwards, bool setterIsStrict)
{
    MOZ_ASSERT(getter != JS_PropertyStub);
    MOZ_ASSERT(setter != JS_StrictPropertyStub);

    /* Use dense storage for new indexed properties where possible. */
    if (JSID_IS_INT(id) &&
        !getter &&
        !setter &&
        attrs == JSPROP_ENUMERATE &&
        (!obj->isIndexed() || !obj->containsPure(id)) &&
        !IsAnyTypedArray(obj))
    {
        uint32_t index = JSID_TO_INT(id);
        bool definesPast;
        if (!WouldDefinePastNonwritableLength(cx, obj, index, setterIsStrict, &definesPast))
            return false;
        if (definesPast)
            return true;

        NativeObject::EnsureDenseResult result;
        result = obj->ensureDenseElements(cx, index, 1);

        if (result == NativeObject::ED_FAILED)
            return false;
        if (result == NativeObject::ED_OK) {
            obj->setDenseElementWithType(cx, index, value);
            return CallAddPropertyHookDense(cx, obj, index, value);
        }
    }

    if (obj->is<ArrayObject>()) {
        Rooted<ArrayObject*> arr(cx, &obj->as<ArrayObject>());
        if (id == NameToId(cx->names().length)) {
            if (!cx->shouldBeJSContext())
                return false;
            return ArraySetLength(cx->asJSContext(), arr, id, attrs, value, setterIsStrict);
        }

        uint32_t index;
        if (js_IdIsIndex(id, &index)) {
            bool definesPast;
            if (!WouldDefinePastNonwritableLength(cx, arr, index, setterIsStrict, &definesPast))
                return false;
            if (definesPast)
                return true;
        }
    }

    // Don't define new indexed properties on typed arrays.
    if (IsAnyTypedArray(obj)) {
        uint64_t index;
        if (IsTypedArrayIndex(id, &index))
            return true;
    }

    AutoRooterGetterSetter gsRoot(cx, attrs, &getter, &setter);
    RootedShape shape(cx, NativeObject::putProperty(cx, obj, id, getter, setter,
                                                    SHAPE_INVALID_SLOT, attrs, 0));
    if (!shape)
        return false;

    if (!UpdateShapeTypeAndValue(cx, obj, shape, value))
        return false;

    /*
     * Clear any existing dense index after adding a sparse indexed property,
     * and investigate converting the object to dense indexes.
     */
    if (JSID_IS_INT(id)) {
        if (!obj->maybeCopyElementsForWrite(cx))
            return false;

        uint32_t index = JSID_TO_INT(id);
        NativeObject::removeDenseElementForSparseIndex(cx, obj, index);
        NativeObject::EnsureDenseResult result = NativeObject::maybeDensifySparseElements(cx, obj);
        if (result == NativeObject::ED_FAILED)
            return false;
        if (result == NativeObject::ED_OK) {
            MOZ_ASSERT(!setter);
            return CallAddPropertyHookDense(cx, obj, index, value);
        }
    }

    if (!CallAddPropertyHook(cx, obj, shape, value))
        return false;

    if (callSetterAfterwards && setter) {
        if (!cx->shouldBeJSContext())
            return false;
        RootedValue nvalue(cx, value);
        return NativeSet(cx->asJSContext(), obj, obj, shape, setterIsStrict, &nvalue);
    }
    return true;
}

static unsigned
ApplyOrDefaultAttributes(unsigned attrs, const Shape* shape = nullptr)
{
    bool enumerable = shape ? shape->enumerable() : false;
    bool writable = shape ? shape->writable() : false;
    bool configurable = shape ? shape->configurable() : false;
    return ApplyAttributes(attrs, enumerable, writable, configurable);
}

static bool
PurgeProtoChain(ExclusiveContext* cx, JSObject* objArg, HandleId id)
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
            return obj->as<NativeObject>().shadowingShapeChange(cx, *shape);

        obj = obj->getProto();
    }

    return true;
}

static bool
PurgeScopeChainHelper(ExclusiveContext* cx, HandleObject objArg, HandleId id)
{
    /* Re-root locally so we can re-assign. */
    RootedObject obj(cx, objArg);

    MOZ_ASSERT(obj->isNative());
    MOZ_ASSERT(obj->isDelegate());

    /* Lookups on integer ids cannot be cached through prototypes. */
    if (JSID_IS_INT(id))
        return true;

    if (!PurgeProtoChain(cx, obj->getProto(), id))
        return false;

    /*
     * We must purge the scope chain only for Call objects as they are the only
     * kind of cacheable non-global object that can gain properties after outer
     * properties with the same names have been cached or traced. Call objects
     * may gain such properties via eval introducing new vars; see bug 490364.
     */
    if (obj->is<CallObject>()) {
        while ((obj = obj->enclosingScope()) != nullptr) {
            if (!PurgeProtoChain(cx, obj, id))
                return false;
        }
    }

    return true;
}

/*
 * PurgeScopeChain does nothing if obj is not itself a prototype or parent
 * scope, else it reshapes the scope and prototype chains it links. It calls
 * PurgeScopeChainHelper, which asserts that obj is flagged as a delegate
 * (i.e., obj has ever been on a prototype or parent chain).
 */
static inline bool
PurgeScopeChain(ExclusiveContext* cx, HandleObject obj, HandleId id)
{
    if (obj->isDelegate() && obj->isNative())
        return PurgeScopeChainHelper(cx, obj, id);
    return true;
}

/*
 * Check whether we're redefining away a non-configurable getter, and
 * throw if so.
 */
static inline bool
CheckAccessorRedefinition(ExclusiveContext* cx, HandleObject obj, HandleShape shape,
                          PropertyOp getter, StrictPropertyOp setter, HandleId id, unsigned attrs)
{
    MOZ_ASSERT(shape->isAccessorDescriptor());
    if (shape->configurable() || (getter == shape->getter() && setter == shape->setter()))
        return true;

    /*
     *  Only allow redefining if JSPROP_REDEFINE_NONCONFIGURABLE is set _and_
     *  the object is a non-DOM global.  The idea is that a DOM object can
     *  never have such a thing on its proto chain directly on the web, so we
     *  should be OK optimizing access to accessors found on such an object.
     */
    if ((attrs & JSPROP_REDEFINE_NONCONFIGURABLE) &&
        obj->is<GlobalObject>() &&
        !obj->getClass()->isDOMClass())
    {
        return true;
    }

    if (!cx->isJSContext())
        return false;

    return Throw(cx->asJSContext(), id, JSMSG_CANT_REDEFINE_PROP);
}

bool
js::NativeDefineProperty(ExclusiveContext* cx, HandleNativeObject obj, HandleId id, HandleValue value,
                         PropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    MOZ_ASSERT(getter != JS_PropertyStub);
    MOZ_ASSERT(setter != JS_StrictPropertyStub);
    MOZ_ASSERT(!(attrs & JSPROP_PROPOP_ACCESSORS));

    AutoRooterGetterSetter gsRoot(cx, attrs, &getter, &setter);

    RootedShape shape(cx);
    RootedValue updateValue(cx, value);
    bool shouldDefine = true;

    /*
     * If defining a getter or setter, we must check for its counterpart and
     * update the attributes and property ops.  A getter or setter is really
     * only half of a property.
     */
    if (attrs & (JSPROP_GETTER | JSPROP_SETTER)) {
        if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &shape))
            return false;
        if (shape) {
            /*
             * If we are defining a getter whose setter was already defined, or
             * vice versa, finish the job via obj->changeProperty.
             */
            if (IsImplicitDenseOrTypedArrayElement(shape)) {
                if (IsAnyTypedArray(obj)) {
                    /* Ignore getter/setter properties added to typed arrays. */
                    return true;
                }
                if (!NativeObject::sparsifyDenseElement(cx, obj, JSID_TO_INT(id)))
                    return false;
                shape = obj->lookup(cx, id);
            }
            if (shape->isAccessorDescriptor()) {
                if (!CheckAccessorRedefinition(cx, obj, shape, getter, setter, id, attrs))
                    return false;
                attrs = ApplyOrDefaultAttributes(attrs, shape);
                shape = NativeObject::changeProperty(cx, obj, shape, attrs,
                                                     JSPROP_GETTER | JSPROP_SETTER,
                                                     (attrs & JSPROP_GETTER)
                                                     ? getter
                                                     : shape->getter(),
                                                     (attrs & JSPROP_SETTER)
                                                     ? setter
                                                     : shape->setter());
                if (!shape)
                    return false;
                shouldDefine = false;
            }
        }
    } else if (!(attrs & JSPROP_IGNORE_VALUE)) {
        // If we did a normal lookup here, it would cause resolve hook recursion in
        // the following case. Suppose the first script we run in a lazy global is
        // |parseInt()|.
        //   - js_InitNumber is called to resolve parseInt.
        //   - js_InitNumber tries to define the Number constructor on the global.
        //   - We end up here.
        //   - This lookup for 'Number' triggers the global resolve hook.
        //   - js_InitNumber is called again, this time to resolve Number.
        //   - It creates a second Number constructor, which trips an assertion.
        //
        // Therefore we do a special lookup that does not call the resolve hook.
        NativeLookupOwnPropertyNoResolve(cx, obj, id, &shape);

        if (shape) {
            // If any other JSPROP_IGNORE_* attributes are present, copy the
            // corresponding JSPROP_* attributes from the existing property.
            if (IsImplicitDenseOrTypedArrayElement(shape)) {
                attrs = ApplyAttributes(attrs, true, true, !IsAnyTypedArray(obj));
            } else {
                attrs = ApplyOrDefaultAttributes(attrs, shape);

                // Do not redefine a nonconfigurable accessor property.
                if (shape->isAccessorDescriptor()) {
                    if (!CheckAccessorRedefinition(cx, obj, shape, getter, setter, id, attrs))
                        return false;
                }
            }
        }
    } else {
        // We have been asked merely to update some attributes. If the
        // property already exists and it's a data property, we can just
        // call JSObject::changeProperty.
        if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &shape))
            return false;

        if (shape) {
            // Don't forget about arrays.
            if (IsImplicitDenseOrTypedArrayElement(shape)) {
                if (IsAnyTypedArray(obj)) {
                    /*
                     * Silently ignore attempts to change individial index attributes.
                     * FIXME: Uses the same broken behavior as for accessors. This should
                     *        probably throw.
                     */
                    return true;
                }
                if (!NativeObject::sparsifyDenseElement(cx, obj, JSID_TO_INT(id)))
                    return false;
                shape = obj->lookup(cx, id);
            }

            if (shape->isAccessorDescriptor() &&
                !CheckAccessorRedefinition(cx, obj, shape, getter, setter, id, attrs))
            {
                return false;
            }

            attrs = ApplyOrDefaultAttributes(attrs, shape);

            if (shape->isAccessorDescriptor() && !(attrs & JSPROP_IGNORE_READONLY)) {
                // ES6 draft 2014-10-14 9.1.6.3 step 7.c: Since [[Writable]] 
                // is present, change the existing accessor property to a data 
                // property.
                updateValue = UndefinedValue();
            } else {
                // We are at most changing some attributes, and cannot convert
                // from data descriptor to accessor, or vice versa. Take
                // everything from the shape that we aren't changing.
                uint32_t propMask = JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT;
                attrs = (shape->attributes() & ~propMask) | (attrs & propMask);
                getter = shape->getter();
                setter = shape->setter();
                if (shape->hasSlot())
                    updateValue = obj->getSlot(shape->slot());
            }
        }
    }

    /*
     * Purge the property cache of any properties named by id that are about
     * to be shadowed in obj's scope chain.
     */
    if (!PurgeScopeChain(cx, obj, id))
        return false;

    if (shouldDefine) {
        // Handle the default cases here. Anyone that wanted to set non-default attributes has
        // cleared the IGNORE flags by now. Since we can never get here with JSPROP_IGNORE_VALUE
        // relevant, just clear it.
        attrs = ApplyOrDefaultAttributes(attrs) & ~JSPROP_IGNORE_VALUE;
        return DefinePropertyOrElement(cx, obj, id, getter, setter,
                                       attrs, updateValue, false, false);
    }

    MOZ_ASSERT(shape);

    JS_ALWAYS_TRUE(UpdateShapeTypeAndValue(cx, obj, shape, updateValue));

    return CallAddPropertyHook(cx, obj, shape, updateValue);
}

template <AllowGC allowGC>
bool
js::NativeLookupOwnProperty(ExclusiveContext* cx,
                            typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                            typename MaybeRooted<jsid, allowGC>::HandleType id,
                            typename MaybeRooted<Shape*, allowGC>::MutableHandleType propp)
{
    bool done;
    return LookupOwnPropertyInline<allowGC>(cx, obj, id, propp, &done);
}

template bool
js::NativeLookupOwnProperty<CanGC>(ExclusiveContext* cx, HandleNativeObject obj, HandleId id,
                                   MutableHandleShape propp);

template bool
js::NativeLookupOwnProperty<NoGC>(ExclusiveContext* cx, NativeObject* obj, jsid id,
                                  FakeMutableHandle<Shape*> propp);

template <AllowGC allowGC>
bool
js::NativeLookupProperty(ExclusiveContext* cx,
                         typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                         typename MaybeRooted<jsid, allowGC>::HandleType id,
                         typename MaybeRooted<JSObject*, allowGC>::MutableHandleType objp,
                         typename MaybeRooted<Shape*, allowGC>::MutableHandleType propp)
{
    return LookupPropertyInline<allowGC>(cx, obj, id, objp, propp);
}

template bool
js::NativeLookupProperty<CanGC>(ExclusiveContext* cx, HandleNativeObject obj, HandleId id,
                                MutableHandleObject objp, MutableHandleShape propp);

template bool
js::NativeLookupProperty<NoGC>(ExclusiveContext* cx, NativeObject* obj, jsid id,
                               FakeMutableHandle<JSObject*> objp,
                               FakeMutableHandle<Shape*> propp);

bool
js::NativeLookupElement(JSContext* cx, HandleNativeObject obj, uint32_t index,
                        MutableHandleObject objp, MutableHandleShape propp)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;

    return LookupPropertyInline<CanGC>(cx, obj, id, objp, propp);
}

bool
js::NativeDefineElement(ExclusiveContext* cx, HandleNativeObject obj, uint32_t index, HandleValue value,
                        PropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    RootedId id(cx);
    if (index <= JSID_INT_MAX) {
        id = INT_TO_JSID(index);
        return NativeDefineProperty(cx, obj, id, value, getter, setter, attrs);
    }

    AutoRooterGetterSetter gsRoot(cx, attrs, &getter, &setter);

    if (!IndexToId(cx, index, &id))
        return false;

    return NativeDefineProperty(cx, obj, id, value, getter, setter, attrs);
}

/*** [[HasProperty]] *****************************************************************************/

// ES6 draft rev31 9.1.7.1 OrdinaryHasProperty
bool
js::NativeHasProperty(JSContext* cx, HandleNativeObject obj, HandleId id, bool* foundp)
{
    RootedNativeObject pobj(cx, obj);
    RootedShape shape(cx);

    // This loop isn't explicit in the spec algorithm. See the comment on step
    // 7.a. below.
    for (;;) {
        // Steps 2-3. ('done' is a SpiderMonkey-specific thing, used below.)
        bool done;
        if (!LookupOwnPropertyInline<CanGC>(cx, pobj, id, &shape, &done))
            return false;

        // Step 4.
        if (shape) {
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
        RootedObject proto(cx, done ? nullptr : pobj->getProto());

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
        if (!proto->isNative())
            return HasProperty(cx, proto, id, foundp);

        pobj = &proto->as<NativeObject>();
    }
}

/*** [[Get]] *************************************************************************************/

static inline bool
CallGetter(JSContext* cx, HandleObject receiver, HandleShape shape, MutableHandleValue vp)
{
    MOZ_ASSERT(!shape->hasDefaultGetter());

    if (shape->hasGetterValue()) {
        Value fval = shape->getterValue();
        return InvokeGetterOrSetter(cx, receiver, fval, 0, 0, vp);
    }

    RootedId id(cx, shape->propid());
    return CallJSPropertyOp(cx, shape->getterOp(), receiver, id, vp);
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE bool
GetExistingProperty(JSContext* cx,
                    typename MaybeRooted<JSObject*, allowGC>::HandleType receiver,
                    typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                    typename MaybeRooted<Shape*, allowGC>::HandleType shape,
                    typename MaybeRooted<Value, allowGC>::MutableHandleType vp)
{
    if (shape->hasSlot()) {
        vp.set(obj->getSlot(shape->slot()));
        MOZ_ASSERT_IF(!vp.isMagic(JS_UNINITIALIZED_LEXICAL) &&
                      !obj->isSingleton() &&
                      !obj->template is<ScopeObject>() &&
                      shape->hasDefaultGetter(),
                      ObjectGroupHasProperty(cx, obj->group(), shape->propid(), vp));
    } else {
        vp.setUndefined();
    }
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

    if (!CallGetter(cx,
                    MaybeRooted<JSObject*, allowGC>::toHandle(receiver),
                    MaybeRooted<Shape*, allowGC>::toHandle(shape),
                    MaybeRooted<Value, allowGC>::toMutableHandle(vp)))
    {
        return false;
    }

    // Ancient nonstandard extension: via the JSAPI it's possible to create a
    // data property that has both a slot and a getter. In that case, copy the
    // value returned by the getter back into the slot.
    if (shape->hasSlot() && obj->contains(cx, shape))
        obj->setSlot(shape->slot(), vp);

    return true;
}

bool
js::NativeGetExistingProperty(JSContext* cx, HandleObject receiver, HandleNativeObject obj,
                              HandleShape shape, MutableHandleValue vp)
{
    return GetExistingProperty<CanGC>(cx, receiver, obj, shape, vp);
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

    // General case: a branch or equality op follows the access.
    JSOp op = JSOp(*pc);
    if (js_CodeSpec[op].format & JOF_DETECTING)
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
            (pc += js_CodeSpec[op].length) < endpc) {
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
 * 1.  We add support for the nonstandard JSClass::getProperty hook.
 *
 * 2.  This function also runs when we're evaluating an expression that's an
 *     Identifier (that is, an unqualified name lookup), so we need to figure
 *     out if that's what's happening and throw a ReferenceError if so.
 *
 * 3.  We also emit an optional warning for this. (It's not super useful on the
 *     web, as there are too many false positives, but anecdotally useful in
 *     Gecko code.)
 */
static bool
GetNonexistentProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                       HandleObject receiver, IsNameLookup nameLookup, MutableHandleValue vp)
{
    vp.setUndefined();

    // Non-standard extension: Call the getProperty hook. If it sets vp to a
    // value other than undefined, we're done. If not, fall through to the
    // warning/error checks below.
    if (JSPropertyOp getProperty = obj->getClass()->getProperty) {
        if (!CallJSPropertyOp(cx, getProperty, obj, id, vp))
            return false;

        if (!vp.isUndefined())
            return true;
    }

    // If we are doing a name lookup, this is a ReferenceError.
    if (nameLookup) {
        JSAutoByteString printable;
        if (js_ValueToPrintable(cx, IdToValue(id), &printable))
            js_ReportIsNotDefined(cx, printable.ptr());
        return false;
    }

    // Give a strict warning if foo.bar is evaluated by a script for an object
    // foo with no property named 'bar'.
    //
    // Don't warn if extra warnings not enabled or for random getprop
    // operations.
    if (!cx->compartment()->options().extraWarnings(cx))
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

    // We may just be checking if that object has an iterator.
    if (JSID_IS_ATOM(id, cx->names().iteratorIntrinsic))
        return true;

    // Do not warn about tests like (obj[prop] == undefined).
    pc += js_CodeSpec[*pc].length;
    if (Detecting(cx, script, pc))
        return true;

    unsigned flags = JSREPORT_WARNING | JSREPORT_STRICT;
    script->setWarnedAboutUndefinedProp();

    // Ok, bad undefined property reference: whine about it.
    RootedValue val(cx, IdToValue(id));
    return js_ReportValueErrorFlags(cx, flags, JSMSG_UNDEFINED_PROP, JSDVG_IGNORE_STACK, val,
                                    js::NullPtr(), nullptr, nullptr);
}

/* The NoGC version of GetNonexistentProperty, present only to make types line up. */
bool
GetNonexistentProperty(JSContext* cx, NativeObject* obj, jsid id, JSObject* receiver,
                       IsNameLookup nameLookup, FakeMutableHandle<Value> vp)
{
    return false;
}

static inline bool
GeneralizedGetProperty(JSContext* cx, HandleObject obj, HandleId id, HandleObject receiver,
                       MutableHandleValue vp)
{
    JS_CHECK_RECURSION(cx, return false);
    return GetProperty(cx, obj, receiver, id, vp);
}

static inline bool
GeneralizedGetProperty(JSContext* cx, JSObject* obj, jsid id, JSObject* receiver,
                       FakeMutableHandle<Value> vp)
{
    JS_CHECK_RECURSION_DONT_REPORT(cx, return false);
    return GetPropertyNoGC(cx, obj, receiver, id, vp.address());
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE bool
NativeGetPropertyInline(JSContext* cx,
                        typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                        typename MaybeRooted<JSObject*, allowGC>::HandleType receiver,
                        typename MaybeRooted<jsid, allowGC>::HandleType id,
                        IsNameLookup nameLookup,
                        typename MaybeRooted<Value, allowGC>::MutableHandleType vp)
{
    typename MaybeRooted<NativeObject*, allowGC>::RootType pobj(cx, obj);
    typename MaybeRooted<Shape*, allowGC>::RootType shape(cx);

    // This loop isn't explicit in the spec algorithm. See the comment on step
    // 4.d below.
    for (;;) {
        // Steps 2-3. ('done' is a SpiderMonkey-specific thing, used below.)
        bool done;
        if (!LookupOwnPropertyInline<allowGC>(cx, pobj, id, &shape, &done))
            return false;

        if (shape) {
            // Steps 5-8. Special case for dense elements because
            // GetExistingProperty doesn't support those.
            if (IsImplicitDenseOrTypedArrayElement(shape)) {
                vp.set(pobj->getDenseOrTypedArrayElement(JSID_TO_INT(id)));
                return true;
            }
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
        RootedObject proto(cx, done ? nullptr : pobj->getProto());

        // Step 4.c. The spec algorithm simply returns undefined if proto is
        // null, but see the comment on GetNonexistentProperty.
        if (!proto)
            return GetNonexistentProperty(cx, obj, id, receiver, nameLookup, vp);

        // Step 4.d. If the prototype is also native, this step is a
        // recursive tail call, and we don't need to go through all the
        // plumbing of JSObject::getGeneric; the top of the loop is where
        // we're going to end up anyway. But if pobj is non-native,
        // that optimization would be incorrect.
        if (!proto->isNative())
            return GeneralizedGetProperty(cx, proto, id, receiver, vp);

        pobj = &proto->as<NativeObject>();
    }
}

bool
js::NativeGetProperty(JSContext* cx, HandleNativeObject obj, HandleObject receiver, HandleId id,
                      MutableHandleValue vp)
{
    return NativeGetPropertyInline<CanGC>(cx, obj, receiver, id, NotNameLookup, vp);
}

bool
js::NativeGetPropertyNoGC(JSContext* cx, NativeObject* obj, JSObject* receiver, jsid id, Value* vp)
{
    AutoAssertNoException noexc(cx);
    return NativeGetPropertyInline<NoGC>(cx, obj, receiver, id, NotNameLookup, vp);
}

bool
js::GetPropertyForNameLookup(JSContext* cx, HandleObject obj, HandleId id, MutableHandleValue vp)
{
    if (GetPropertyOp op = obj->getOps()->getProperty)
        return op(cx, obj, obj, id, vp);
    return NativeGetPropertyInline<CanGC>(cx, obj.as<NativeObject>(), obj, id, NameLookup, vp);
}

/*** [[Set]] *************************************************************************************/

static bool
MaybeReportUndeclaredVarAssignment(JSContext* cx, JSString* propname)
{
    {
        jsbytecode* pc;
        JSScript* script = cx->currentScript(&pc, JSContext::ALLOW_CROSS_COMPARTMENT);
        if (!script)
            return true;

        // If the code is not strict and extra warnings aren't enabled, then no
        // check is needed.
        if (!IsStrictSetPC(pc) && !cx->compartment()->options().extraWarnings(cx))
            return true;
    }

    JSAutoByteString bytes(cx, propname);
    return !!bytes &&
           JS_ReportErrorFlagsAndNumber(cx,
                                        (JSREPORT_WARNING | JSREPORT_STRICT
                                         | JSREPORT_STRICT_MODE_ERROR),
                                        js_GetErrorMessage, nullptr,
                                        JSMSG_UNDECLARED_VAR, bytes.ptr());
}

/*
 * When a [[Set]] operation finds no existing property with the given id
 * or finds a writable data property on the prototype chain, we end up here.
 * Finish the [[Set]] by defining a new property on receiver.
 *
 * This implements ES6 draft rev 28, 9.1.9 [[Set]] steps 5.c-f, but it
 * is really old code and there are a few barnacles.
 */
bool
js::SetPropertyByDefining(JSContext* cx, HandleObject obj, HandleObject receiver,
                          HandleId id, HandleValue v, bool strict, bool objHasOwn)
{
    // Step 5.c-d: Test whether receiver has an existing own property
    // receiver[id]. The spec calls [[GetOwnProperty]]; js::HasOwnProperty is
    // the same thing except faster in the non-proxy case. Sometimes we can
    // even optimize away the HasOwnProperty call.
    bool existing;
    if (receiver == obj) {
        // The common case. The caller has necessarily done a property lookup
        // on obj and passed us the answer as objHasOwn.
#ifdef DEBUG
        // Check that objHasOwn is correct. This could fail if receiver or a
        // native object on its prototype chain has a nondeterministic resolve
        // hook. We shouldn't have any that are quite that badly behaved.
        if (!HasOwnProperty(cx, receiver, id, &existing))
            return false;
        MOZ_ASSERT(existing == objHasOwn);
#endif
        existing = objHasOwn;
    } else {
        if (!HasOwnProperty(cx, receiver, id, &existing))
            return false;
    }

    // If the property doesn't already exist, check for an inextensible
    // receiver. (According to the specification, this is supposed to be
    // enforced by [[DefineOwnProperty]], but we haven't implemented that yet.)
    if (!existing) {
        bool extensible;
        if (!IsExtensible(cx, receiver, &extensible))
            return false;
        if (!extensible) {
            // Error in strict mode code, warn with extra warnings option,
            // otherwise do nothing.
            if (strict)
                return receiver->reportNotExtensible(cx);
            if (cx->compartment()->options().extraWarnings(cx))
                return receiver->reportNotExtensible(cx, JSREPORT_STRICT | JSREPORT_WARNING);
            return true;
        }
    }

    // Invalidate SpiderMonkey-specific caches or bail.
    const Class* clasp = receiver->getClass();

    // Purge the property cache of now-shadowed id in receiver's scope chain.
    if (!PurgeScopeChain(cx, receiver, id))
        return false;

    // Step 5.e-f. Define the new data property.
    unsigned attrs =
        existing
        ? JSPROP_IGNORE_ENUMERATE | JSPROP_IGNORE_READONLY | JSPROP_IGNORE_PERMANENT
        : JSPROP_ENUMERATE;
    JSPropertyOp getter = clasp->getProperty;
    JSStrictPropertyOp setter = clasp->setProperty;
    MOZ_ASSERT(getter != JS_PropertyStub);
    MOZ_ASSERT(setter != JS_StrictPropertyStub);
    if (!receiver->is<NativeObject>())
        return DefineProperty(cx, receiver, id, v, getter, setter, attrs);

    Rooted<NativeObject*> nativeReceiver(cx, &receiver->as<NativeObject>());
    return DefinePropertyOrElement(cx, nativeReceiver, id, getter, setter, attrs, v, true, strict);
}

// When setting |id| for |receiver| and |obj| has no property for id, continue
// the search up the prototype chain.
bool
js::SetPropertyOnProto(JSContext* cx, HandleObject obj, HandleObject receiver,
                       HandleId id, MutableHandleValue vp, bool strict)
{
    MOZ_ASSERT(!obj->is<ProxyObject>());

    RootedObject proto(cx, obj->getProto());
    if (proto)
        return SetProperty(cx, proto, receiver, id, vp, strict);
    return SetPropertyByDefining(cx, obj, receiver, id, vp, strict, false);
}

bool
js::SetNonWritableProperty(JSContext* cx, HandleId id, bool strict)
{
    // Setting a non-writable property is an error in strict mode code, a
    // warning with the extra warnings option, and otherwise does nothing.

    if (strict)
        return JSObject::reportReadOnly(cx, id, JSREPORT_ERROR);
    if (cx->compartment()->options().extraWarnings(cx))
        return JSObject::reportReadOnly(cx, id, JSREPORT_STRICT | JSREPORT_WARNING);
    return true;
}

/*
 * Implement "the rest of" assignment to a property when no property receiver[id]
 * was found anywhere on the prototype chain.
 *
 * FIXME: This should be updated to follow ES6 draft rev 28, section 9.1.9,
 * steps 4.d.i and 5.
 *
 * Note that receiver is not necessarily native.
 */
static bool
SetNonexistentProperty(JSContext* cx, HandleNativeObject obj, HandleObject receiver, HandleId id,
                       QualifiedBool qualified, HandleValue v, bool strict)
{
    // We should never add properties to lexical blocks.
    MOZ_ASSERT(!receiver->is<BlockObject>());

    if (receiver->isUnqualifiedVarObj() && !qualified) {
        if (!MaybeReportUndeclaredVarAssignment(cx, JSID_TO_STRING(id)))
            return false;
    }

    return SetPropertyByDefining(cx, obj, receiver, id, v, strict, false);
}

/*
 * Set an existing own property obj[index] that's a dense element or typed
 * array element.
 */
static bool
SetDenseOrTypedArrayElement(JSContext* cx, HandleNativeObject obj, uint32_t index,
                            MutableHandleValue vp, bool strict)
{
    if (IsAnyTypedArray(obj)) {
        double d;
        if (!ToNumber(cx, vp, &d))
            return false;

        // Silently do nothing for out-of-bounds sets, for consistency with
        // current behavior.  (ES6 currently says to throw for this in
        // strict mode code, so we may eventually need to change.)
        uint32_t len = AnyTypedArrayLength(obj);
        if (index < len) {
            if (obj->is<TypedArrayObject>())
                TypedArrayObject::setElement(obj->as<TypedArrayObject>(), index, d);
            else
                SharedTypedArrayObject::setElement(obj->as<SharedTypedArrayObject>(), index, d);
        }
        return true;
    }

    bool definesPast;
    if (!WouldDefinePastNonwritableLength(cx, obj, index, strict, &definesPast))
        return false;
    if (definesPast)
        return true;

    if (!obj->maybeCopyElementsForWrite(cx))
        return false;

    obj->setDenseElementWithType(cx, index, vp);
    return true;
}

/*
 * Finish assignment to a shapeful property of a native object obj. This
 * conforms to no standard and there is a lot of legacy baggage here.
 */
static bool
NativeSet(JSContext* cx, HandleNativeObject obj, HandleObject receiver,
          HandleShape shape, bool strict, MutableHandleValue vp)
{
    MOZ_ASSERT(obj->isNative());

    if (shape->hasSlot()) {
        /* If shape has a stub setter, just store vp. */
        if (shape->hasDefaultSetter()) {
            // Global properties declared with 'var' will be initially
            // defined with an undefined value, so don't treat the initial
            // assignments to such properties as overwrites.
            bool overwriting = !obj->is<GlobalObject>() || !obj->getSlot(shape->slot()).isUndefined();
            obj->setSlotWithType(cx, shape, vp, overwriting);
            return true;
        }
    }

    if (!shape->hasSlot()) {
        /*
         * Allow API consumers to create shared properties with stub setters.
         * Such properties effectively function as data descriptors which are
         * not writable, so attempting to set such a property should do nothing
         * or throw if we're in strict mode.
         */
        if (!shape->hasGetterValue() && shape->hasDefaultSetter())
            return js_ReportGetterOnlyAssignment(cx, strict);
    }

    RootedValue ovp(cx, vp);

    uint32_t sample = cx->runtime()->propertyRemovals;
    if (!shape->set(cx, obj, receiver, strict, vp))
        return false;

    /*
     * Update any slot for the shape with the value produced by the setter,
     * unless the setter deleted the shape.
     */
    if (shape->hasSlot() &&
        (MOZ_LIKELY(cx->runtime()->propertyRemovals == sample) ||
         obj->contains(cx, shape)))
    {
        obj->setSlot(shape->slot(), vp);
    }

    return true;
}

/*
 * Implement "the rest of" assignment to receiver[id] when an existing property
 * (shape) has been found on a native object (pobj).
 *
 * It is necessary to pass both id and shape because shape could be an implicit
 * dense or typed array element (i.e. not actually a pointer to a Shape).
 */
static bool
SetExistingProperty(JSContext* cx, HandleNativeObject obj, HandleObject receiver, HandleId id,
                    HandleNativeObject pobj, HandleShape shape, MutableHandleValue vp, bool strict)
{
    if (IsImplicitDenseOrTypedArrayElement(shape)) {
        /* ES5 8.12.4 [[Put]] step 2, for a dense data property on pobj. */
        if (pobj == receiver)
            return SetDenseOrTypedArrayElement(cx, pobj, JSID_TO_INT(id), vp, strict);
    } else {
        /* ES5 8.12.4 [[Put]] step 2. */
        if (shape->isAccessorDescriptor()) {
            if (shape->hasDefaultSetter())
                return js_ReportGetterOnlyAssignment(cx, strict);
        } else {
            MOZ_ASSERT(shape->isDataDescriptor());

            if (!shape->writable())
                return SetNonWritableProperty(cx, id, strict);
        }

        if (pobj == receiver) {
            if (pobj->is<ArrayObject>() && id == NameToId(cx->names().length)) {
                Rooted<ArrayObject*> arr(cx, &pobj->as<ArrayObject>());
                return ArraySetLength(cx, arr, id, shape->attributes(), vp, strict);
            }
            return NativeSet(cx, obj, receiver, shape, strict, vp);
        }

        // pobj[id] is not an own property of receiver. Call the setter or shadow it.
        if (!shape->shadowable() &&
            !(pobj->is<ArrayObject>() && id == NameToId(cx->names().length)))
        {
            // Weird special case: slotless property with default setter.
            if (shape->hasDefaultSetter() && !shape->hasGetterValue())
                return true;

            return shape->set(cx, obj, receiver, strict, vp);
        }
    }

    // Shadow pobj[id] by defining a new data property receiver[id].
    return SetPropertyByDefining(cx, obj, receiver, id, vp, strict, obj == pobj);
}

bool
js::NativeSetProperty(JSContext* cx, HandleNativeObject obj, HandleObject receiver, HandleId id,
                      QualifiedBool qualified, MutableHandleValue vp, bool strict)
{
    // Fire watchpoints, if any.
    if (MOZ_UNLIKELY(obj->watched())) {
        WatchpointMap* wpmap = cx->compartment()->watchpointMap;
        if (wpmap && !wpmap->triggerWatchpoint(cx, obj, id, vp))
            return false;
    }

    // Step numbers below reference ES6 rev 27 9.1.9, the [[Set]] internal
    // method for ordinary objects. We substitute our own names for these names
    // used in the spec: O -> pobj, P -> id, V -> *vp, ownDesc -> shape.
    RootedShape shape(cx);
    RootedNativeObject pobj(cx, obj);

    // This loop isn't explicit in the spec algorithm. See the comment on step
    // 4.c.i below. (There's a very similar loop in the NativeGetProperty
    // implementation, but unfortunately not similar enough to common up.)
    for (;;) {
        // Steps 2-3. ('done' is a SpiderMonkey-specific thing, used below.)
        bool done;
        if (!LookupOwnPropertyInline<CanGC>(cx, pobj, id, &shape, &done))
            return false;

        if (shape) {
            // Steps 5-6.
            return SetExistingProperty(cx, obj, receiver, id, pobj, shape, vp, strict);
        }

        // Steps 4.a-b. The check for 'done' on this next line is tricky.
        // done can be true in exactly these unlikely-sounding cases:
        // - We're looking up an element, and pobj is a TypedArray that
        //   doesn't have that many elements.
        // - We're being called from a resolve hook to assign to the property
        //   being resolved.
        // What they all have in common is we do not want to keep walking
        // the prototype chain.
        RootedObject proto(cx, done ? nullptr : pobj->getProto());
        if (!proto) {
            // Step 4.d.i (and step 5).
            return SetNonexistentProperty(cx, obj, receiver, id, qualified, vp, strict);
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
            if (!qualified) {
                bool found;
                if (!HasProperty(cx, proto, id, &found))
                    return false;
                if (!found)
                    return SetNonexistentProperty(cx, obj, receiver, id, qualified, vp, strict);
            }

            return SetProperty(cx, proto, receiver, id, vp, strict);
        }
        pobj = &proto->as<NativeObject>();
    }
}

bool
js::NativeSetElement(JSContext* cx, HandleNativeObject obj, HandleObject receiver, uint32_t index,
                     MutableHandleValue vp, bool strict)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    return NativeSetProperty(cx, obj, receiver, id, Qualified, vp, strict);
}

/*** [[Delete]] **********************************************************************************/

// ES6 draft rev31 9.1.10 [[Delete]]
bool
js::NativeDeleteProperty(JSContext* cx, HandleNativeObject obj, HandleId id, bool* succeeded)
{
    // Steps 2-3.
    RootedShape shape(cx);
    if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &shape))
        return false;

    // Step 4.
    if (!shape) {
        // If no property call the class's delProperty hook, passing succeeded
        // as the result parameter. This always succeeds when there is no hook.
        return CallJSDeletePropertyOp(cx, obj->getClass()->delProperty, obj, id, succeeded);
    }

    cx->runtime()->gc.poke();

    // Step 6. Non-configurable property.
    if (GetShapeAttributes(obj, shape) & JSPROP_PERMANENT) {
        *succeeded = false;
        return true;
    }

    if (!CallJSDeletePropertyOp(cx, obj->getClass()->delProperty, obj, id, succeeded))
        return false;
    if (!*succeeded)
        return true;

    // Step 5.
    if (IsImplicitDenseOrTypedArrayElement(shape)) {
        // Typed array elements are non-configurable.
        MOZ_ASSERT(!IsAnyTypedArray(obj));

        if (!obj->maybeCopyElementsForWrite(cx))
            return false;

        obj->setDenseElementHole(cx, JSID_TO_INT(id));
    } else {
        if (!obj->removeProperty(cx, id))
            return false;
    }

    return SuppressDeletedProperty(cx, obj, id);
}
