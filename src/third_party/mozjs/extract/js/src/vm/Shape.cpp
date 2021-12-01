/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS symbol tables. */

#include "vm/Shape-inl.h"

#include "mozilla/MathAlgorithms.h"
#include "mozilla/PodOperations.h"

#include "gc/FreeOp.h"
#include "gc/HashUtil.h"
#include "gc/Policy.h"
#include "gc/PublicIterators.h"
#include "js/HashTable.h"
#include "util/Text.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"

#include "vm/Caches-inl.h"
#include "vm/JSCompartment-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::CeilingLog2Size;
using mozilla::PodZero;

using JS::AutoCheckCannotGC;

Shape* const ShapeTable::Entry::SHAPE_REMOVED = (Shape*)ShapeTable::Entry::SHAPE_COLLISION;

bool
ShapeTable::init(JSContext* cx, Shape* lastProp)
{
    uint32_t sizeLog2 = CeilingLog2Size(entryCount_);
    uint32_t size = JS_BIT(sizeLog2);
    if (entryCount_ >= size - (size >> 2))
        sizeLog2++;
    if (sizeLog2 < MIN_SIZE_LOG2)
        sizeLog2 = MIN_SIZE_LOG2;

    size = JS_BIT(sizeLog2);
    entries_ = cx->pod_calloc<Entry>(size);
    if (!entries_)
        return false;

    MOZ_ASSERT(sizeLog2 <= HASH_BITS);
    hashShift_ = HASH_BITS - sizeLog2;

    for (Shape::Range<NoGC> r(lastProp); !r.empty(); r.popFront()) {
        Shape& shape = r.front();
        Entry& entry = searchUnchecked<MaybeAdding::Adding>(shape.propid());

        /*
         * Beware duplicate args and arg vs. var conflicts: the youngest shape
         * (nearest to lastProp) must win. See bug 600067.
         */
        if (!entry.shape())
            entry.setPreservingCollision(&shape);
    }

    MOZ_ASSERT(capacity() == size);
    MOZ_ASSERT(size >= MIN_SIZE);
    MOZ_ASSERT(!needsToGrow());
    return true;
}

void
Shape::removeFromDictionary(NativeObject* obj)
{
    MOZ_ASSERT(inDictionary());
    MOZ_ASSERT(obj->inDictionaryMode());
    MOZ_ASSERT(listp);

    MOZ_ASSERT(obj->shape()->inDictionary());
    MOZ_ASSERT(obj->shape()->listp == obj->shapePtr());

    if (parent)
        parent->listp = listp;
    *listp = parent;
    listp = nullptr;

    obj->shape()->clearCachedBigEnoughForShapeTable();
}

void
Shape::insertIntoDictionary(GCPtrShape* dictp)
{
    // Don't assert inDictionaryMode() here because we may be called from
    // NativeObject::toDictionaryMode via Shape::initDictionaryShape.
    MOZ_ASSERT(inDictionary());
    MOZ_ASSERT(!listp);

    MOZ_ASSERT_IF(*dictp, (*dictp)->inDictionary());
    MOZ_ASSERT_IF(*dictp, (*dictp)->listp == dictp);
    MOZ_ASSERT_IF(*dictp, zone() == (*dictp)->zone());

    setParent(dictp->get());
    if (parent)
        parent->listp = &parent;
    listp = (GCPtrShape*) dictp;
    *dictp = this;
}

bool
Shape::makeOwnBaseShape(JSContext* cx)
{
    MOZ_ASSERT(!base()->isOwned());
    MOZ_ASSERT(cx->zone() == zone());

    BaseShape* nbase = Allocate<BaseShape, NoGC>(cx);
    if (!nbase)
        return false;

    new (nbase) BaseShape(StackBaseShape(this));
    nbase->setOwned(base()->toUnowned());

    this->base_ = nbase;

    return true;
}

void
Shape::handoffTableTo(Shape* shape)
{
    MOZ_ASSERT(inDictionary() && shape->inDictionary());

    if (this == shape)
        return;

    MOZ_ASSERT(base()->isOwned() && !shape->base()->isOwned());

    BaseShape* nbase = base();

    MOZ_ASSERT_IF(!shape->isEmptyShape() && shape->isDataProperty(),
                  nbase->slotSpan() > shape->slot());

    this->base_ = nbase->baseUnowned();
    nbase->adoptUnowned(shape->base()->toUnowned());

    shape->base_ = nbase;
}

/* static */ bool
Shape::hashify(JSContext* cx, Shape* shape)
{
    MOZ_ASSERT(!shape->hasTable());

    if (!shape->ensureOwnBaseShape(cx))
        return false;

    ShapeTable* table = cx->new_<ShapeTable>(shape->entryCount());
    if (!table)
        return false;

    if (!table->init(cx, shape)) {
        js_free(table);
        return false;
    }

    shape->base()->setTable(table);
    return true;
}

bool
ShapeTable::change(JSContext* cx, int log2Delta)
{
    MOZ_ASSERT(entries_);
    MOZ_ASSERT(-1 <= log2Delta && log2Delta <= 1);

    /*
     * Grow, shrink, or compress by changing this->entries_.
     */
    uint32_t oldLog2 = HASH_BITS - hashShift_;
    uint32_t newLog2 = oldLog2 + log2Delta;
    uint32_t oldSize = JS_BIT(oldLog2);
    uint32_t newSize = JS_BIT(newLog2);
    Entry* newTable = cx->maybe_pod_calloc<Entry>(newSize);
    if (!newTable)
        return false;

    /* Now that we have newTable allocated, update members. */
    MOZ_ASSERT(newLog2 <= HASH_BITS);
    hashShift_ = HASH_BITS - newLog2;
    removedCount_ = 0;
    Entry* oldTable = entries_;
    entries_ = newTable;

    /* Copy only live entries, leaving removed and free ones behind. */
    AutoCheckCannotGC nogc;
    for (Entry* oldEntry = oldTable; oldSize != 0; oldEntry++) {
        if (Shape* shape = oldEntry->shape()) {
            Entry& entry = search<MaybeAdding::Adding>(shape->propid(), nogc);
            MOZ_ASSERT(entry.isFree());
            entry.setShape(shape);
        }
        oldSize--;
    }

    MOZ_ASSERT(capacity() == newSize);

    /* Finally, free the old entries storage. */
    js_free(oldTable);
    return true;
}

bool
ShapeTable::grow(JSContext* cx)
{
    MOZ_ASSERT(needsToGrow());

    uint32_t size = capacity();
    int delta = removedCount_ < (size >> 2);

    MOZ_ASSERT(entryCount_ + removedCount_ <= size - 1);

    if (!change(cx, delta)) {
        if (entryCount_ + removedCount_ == size - 1) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    return true;
}

void
ShapeTable::trace(JSTracer* trc)
{
    for (size_t i = 0; i < capacity(); i++) {
        Entry& entry = getEntry(i);
        Shape* shape = entry.shape();
        if (shape) {
            TraceManuallyBarrieredEdge(trc, &shape, "ShapeTable shape");
            if (shape != entry.shape())
                entry.setPreservingCollision(shape);
        }
    }
}

#ifdef JSGC_HASH_TABLE_CHECKS

void
ShapeTable::checkAfterMovingGC()
{
    for (size_t i = 0; i < capacity(); i++) {
        Entry& entry = getEntry(i);
        Shape* shape = entry.shape();
        if (shape)
            CheckGCThingAfterMovingGC(shape);
    }
}

#endif

/* static */ Shape*
Shape::replaceLastProperty(JSContext* cx, StackBaseShape& base,
                           TaggedProto proto, HandleShape shape)
{
    MOZ_ASSERT(!shape->inDictionary());

    if (!shape->parent) {
        /* Treat as resetting the initial property of the shape hierarchy. */
        AllocKind kind = gc::GetGCObjectKind(shape->numFixedSlots());
        return EmptyShape::getInitialShape(cx, base.clasp, proto, kind,
                                           base.flags & BaseShape::OBJECT_FLAG_MASK);
    }

    UnownedBaseShape* nbase = BaseShape::getUnowned(cx, base);
    if (!nbase)
        return nullptr;

    Rooted<StackShape> child(cx, StackShape(shape));
    child.setBase(nbase);

    return cx->zone()->propertyTree().getChild(cx, shape->parent, child);
}

/*
 * Get or create a property-tree or dictionary child property of |parent|,
 * which must be lastProperty() if inDictionaryMode(), else parent must be
 * one of lastProperty() or lastProperty()->parent.
 */
/* static */ MOZ_ALWAYS_INLINE Shape*
NativeObject::getChildDataProperty(JSContext* cx,
                                   HandleNativeObject obj, HandleShape parent,
                                   MutableHandle<StackShape> child)
{
    MOZ_ASSERT(child.isDataProperty());

    if (child.hasMissingSlot()) {
        uint32_t slot;
        if (obj->inDictionaryMode()) {
            if (!allocDictionarySlot(cx, obj, &slot))
                return nullptr;
        } else {
            slot = obj->slotSpan();
            MOZ_ASSERT(slot >= JSSLOT_FREE(obj->getClass()));
            // Objects with many properties are converted to dictionary
            // mode, so we can't overflow SHAPE_MAXIMUM_SLOT here.
            MOZ_ASSERT(slot < JSSLOT_FREE(obj->getClass()) + PropertyTree::MAX_HEIGHT);
            MOZ_ASSERT(slot < SHAPE_MAXIMUM_SLOT);
        }
        child.setSlot(slot);
    } else {
        /*
         * Slots can only be allocated out of order on objects in
         * dictionary mode.  Otherwise the child's slot must be after the
         * parent's slot (if it has one), because slot number determines
         * slot span for objects with that shape.  Usually child slot
         * *immediately* follows parent slot, but there may be a slot gap
         * when the object uses some -- but not all -- of its reserved
         * slots to store properties.
         */
        MOZ_ASSERT(obj->inDictionaryMode() ||
                   parent->hasMissingSlot() ||
                   child.slot() == parent->maybeSlot() + 1 ||
                   (parent->maybeSlot() + 1 < JSSLOT_FREE(obj->getClass()) &&
                    child.slot() == JSSLOT_FREE(obj->getClass())));
    }

    if (obj->inDictionaryMode()) {
        MOZ_ASSERT(parent == obj->lastProperty());
        Shape* shape = Allocate<Shape>(cx);
        if (!shape)
            return nullptr;
        if (child.slot() >= obj->lastProperty()->base()->slotSpan()) {
            if (!obj->setSlotSpan(cx, child.slot() + 1)) {
                new (shape) Shape(obj->lastProperty()->base()->unowned(), 0);
                return nullptr;
            }
        }
        shape->initDictionaryShape(child, obj->numFixedSlots(), obj->shapePtr());
        return shape;
    }

    Shape* shape = cx->zone()->propertyTree().inlinedGetChild(cx, parent, child);
    if (!shape)
        return nullptr;

    MOZ_ASSERT(shape->parent == parent);
    MOZ_ASSERT_IF(parent != obj->lastProperty(), parent == obj->lastProperty()->parent);

    if (!obj->setLastProperty(cx, shape))
        return nullptr;
    return shape;
}

/* static */ MOZ_ALWAYS_INLINE Shape*
NativeObject::getChildAccessorProperty(JSContext* cx,
                                       HandleNativeObject obj, HandleShape parent,
                                       MutableHandle<StackShape> child)
{
    MOZ_ASSERT(!child.isDataProperty());

    // Accessor properties have no slot, but slot_ will reflect that of parent.
    child.setSlot(parent->maybeSlot());

    if (obj->inDictionaryMode()) {
        MOZ_ASSERT(parent == obj->lastProperty());
        Shape* shape = Allocate<AccessorShape>(cx);
        if (!shape)
            return nullptr;
        shape->initDictionaryShape(child, obj->numFixedSlots(), obj->shapePtr());
        return shape;
    }

    Shape* shape = cx->zone()->propertyTree().inlinedGetChild(cx, parent, child);
    if (!shape)
        return nullptr;

    MOZ_ASSERT(shape->parent == parent);
    MOZ_ASSERT_IF(parent != obj->lastProperty(), parent == obj->lastProperty()->parent);

    if (!obj->setLastProperty(cx, shape))
        return nullptr;
    return shape;
}

/* static */ bool
js::NativeObject::toDictionaryMode(JSContext* cx, HandleNativeObject obj)
{
    MOZ_ASSERT(!obj->inDictionaryMode());
    MOZ_ASSERT(cx->isInsideCurrentCompartment(obj));

    uint32_t span = obj->slotSpan();

    // Clone the shapes into a new dictionary list. Don't update the last
    // property of this object until done, otherwise a GC triggered while
    // creating the dictionary will get the wrong slot span for this object.
    RootedShape root(cx);
    RootedShape dictionaryShape(cx);

    RootedShape shape(cx, obj->lastProperty());
    while (shape) {
        MOZ_ASSERT(!shape->inDictionary());

        Shape* dprop = shape->isAccessorShape() ? Allocate<AccessorShape>(cx) : Allocate<Shape>(cx);
        if (!dprop) {
            ReportOutOfMemory(cx);
            return false;
        }

        GCPtrShape* listp = dictionaryShape ? &dictionaryShape->parent : nullptr;
        StackShape child(shape);
        dprop->initDictionaryShape(child, obj->numFixedSlots(), listp);

        if (!dictionaryShape)
            root = dprop;

        MOZ_ASSERT(!dprop->hasTable());
        dictionaryShape = dprop;
        shape = shape->previous();
    }

    if (!Shape::hashify(cx, root)) {
        ReportOutOfMemory(cx);
        return false;
    }

    if (IsInsideNursery(obj) &&
        !cx->nursery().queueDictionaryModeObjectToSweep(obj))
    {
        ReportOutOfMemory(cx);
        return false;
    }

    MOZ_ASSERT(root->listp == nullptr);
    root->listp = obj->shapePtr();
    obj->setShape(root);

    MOZ_ASSERT(obj->inDictionaryMode());
    root->base()->setSlotSpan(span);

    return true;
}

static bool
ShouldConvertToDictionary(NativeObject* obj)
{
    /*
     * Use a lower limit if this object is likely a hashmap (SETELEM was used
     * to set properties).
     */
    if (obj->hadElementsAccess())
        return obj->lastProperty()->entryCount() >= PropertyTree::MAX_HEIGHT_WITH_ELEMENTS_ACCESS;
    return obj->lastProperty()->entryCount() >= PropertyTree::MAX_HEIGHT;
}

static MOZ_ALWAYS_INLINE UnownedBaseShape*
GetBaseShapeForNewShape(JSContext* cx, HandleShape last, HandleId id)
{
    uint32_t index;
    bool indexed = IdIsIndex(id, &index);
    bool interestingSymbol = JSID_IS_SYMBOL(id) && JSID_TO_SYMBOL(id)->isInterestingSymbol();

    if (MOZ_LIKELY(!indexed && !interestingSymbol))
        return last->base()->unowned();

    StackBaseShape base(last->base());
    if (indexed)
        base.flags |= BaseShape::INDEXED;
    else if (interestingSymbol)
        base.flags |= BaseShape::HAS_INTERESTING_SYMBOL;
    return BaseShape::getUnowned(cx, base);
}

namespace js {

class MOZ_RAII AutoCheckShapeConsistency
{
#ifdef DEBUG
    HandleNativeObject obj_;
#endif

  public:
    explicit AutoCheckShapeConsistency(HandleNativeObject obj)
#ifdef DEBUG
      : obj_(obj)
#endif
    {}

#ifdef DEBUG
    ~AutoCheckShapeConsistency() {
        obj_->checkShapeConsistency();
    }
#endif
};

} // namespace js

/* static */ MOZ_ALWAYS_INLINE bool
NativeObject::maybeConvertToOrGrowDictionaryForAdd(JSContext* cx, HandleNativeObject obj, HandleId id,
                                                   ShapeTable** table, ShapeTable::Entry** entry,
                                                   const AutoKeepShapeTables& keep)
{
    MOZ_ASSERT(!!*table == !!*entry);

    // The code below deals with either converting obj to dictionary mode or
    // growing an object that's already in dictionary mode.
    if (!obj->inDictionaryMode()) {
        if (!ShouldConvertToDictionary(obj))
            return true;
        if (!toDictionaryMode(cx, obj))
            return false;
        *table = obj->lastProperty()->maybeTable(keep);
    } else {
        if (!(*table)->needsToGrow())
            return true;
        if (!(*table)->grow(cx))
            return false;
    }

    *entry = &(*table)->search<MaybeAdding::Adding>(id, keep);
    MOZ_ASSERT(!(*entry)->shape());
    return true;
}

MOZ_ALWAYS_INLINE void
Shape::updateDictionaryTable(ShapeTable* table, ShapeTable::Entry* entry,
                             const AutoKeepShapeTables& keep)
{
    MOZ_ASSERT(table);
    MOZ_ASSERT(entry);
    MOZ_ASSERT(inDictionary());

    // Store this Shape in the table entry.
    entry->setPreservingCollision(this);
    table->incEntryCount();

    // Pass the table along to the new last property, namely *this.
    MOZ_ASSERT(parent->maybeTable(keep) == table);
    parent->handoffTableTo(this);
}

/* static */ Shape*
NativeObject::addAccessorPropertyInternal(JSContext* cx,
                                          HandleNativeObject obj, HandleId id,
                                          GetterOp getter, SetterOp setter, unsigned attrs,
                                          ShapeTable* table, ShapeTable::Entry* entry,
                                          const AutoKeepShapeTables& keep)
{
    AutoCheckShapeConsistency check(obj);
    AutoRooterGetterSetter gsRoot(cx, attrs, &getter, &setter);

    if (!maybeConvertToOrGrowDictionaryForAdd(cx, obj, id, &table, &entry, keep))
        return nullptr;

    // Find or create a property tree node labeled by our arguments.
    RootedShape shape(cx);
    {
        RootedShape last(cx, obj->lastProperty());
        Rooted<UnownedBaseShape*> nbase(cx, GetBaseShapeForNewShape(cx, last, id));
        if (!nbase)
            return nullptr;

        Rooted<StackShape> child(cx, StackShape(nbase, id, SHAPE_INVALID_SLOT, attrs));
        child.updateGetterSetter(getter, setter);
        shape = getChildAccessorProperty(cx, obj, last, &child);
        if (!shape)
            return nullptr;
    }

    MOZ_ASSERT(shape == obj->lastProperty());

    if (table)
        shape->updateDictionaryTable(table, entry, keep);

    return shape;
}

/* static */ Shape*
NativeObject::addDataPropertyInternal(JSContext* cx,
                                      HandleNativeObject obj, HandleId id,
                                      uint32_t slot, unsigned attrs,
                                      ShapeTable* table,
                                      ShapeTable::Entry* entry,
                                      const AutoKeepShapeTables& keep)
{
    AutoCheckShapeConsistency check(obj);

    // The slot, if any, must be a reserved slot.
    MOZ_ASSERT(slot == SHAPE_INVALID_SLOT ||
               slot < JSCLASS_RESERVED_SLOTS(obj->getClass()));

    if (!maybeConvertToOrGrowDictionaryForAdd(cx, obj, id, &table, &entry, keep))
        return nullptr;

    // Find or create a property tree node labeled by our arguments.
    RootedShape shape(cx);
    {
        RootedShape last(cx, obj->lastProperty());
        Rooted<UnownedBaseShape*> nbase(cx, GetBaseShapeForNewShape(cx, last, id));
        if (!nbase)
            return nullptr;

        Rooted<StackShape> child(cx, StackShape(nbase, id, slot, attrs));
        shape = getChildDataProperty(cx, obj, last, &child);
        if (!shape)
            return nullptr;
    }

    MOZ_ASSERT(shape == obj->lastProperty());

    if (table)
        shape->updateDictionaryTable(table, entry, keep);

    return shape;
}

static MOZ_ALWAYS_INLINE Shape*
PropertyTreeReadBarrier(Shape* parent, Shape* shape)
{
    JS::Zone* zone = shape->zone();
    if (zone->needsIncrementalBarrier()) {
        // We need a read barrier for the shape tree, since these are weak
        // pointers.
        Shape* tmp = shape;
        TraceManuallyBarrieredEdge(zone->barrierTracer(), &tmp, "read barrier");
        MOZ_ASSERT(tmp == shape);
        return shape;
    }

    if (MOZ_LIKELY(!zone->isGCSweepingOrCompacting() ||
                   !IsAboutToBeFinalizedUnbarriered(&shape)))
    {
        if (shape->isMarkedGray())
            UnmarkGrayShapeRecursively(shape);
        return shape;
    }

    // The shape we've found is unreachable and due to be finalized, so
    // remove our weak reference to it and don't use it.
    MOZ_ASSERT(parent->isMarkedAny());
    parent->removeChild(shape);

    return nullptr;
}

/* static */ Shape*
NativeObject::addEnumerableDataProperty(JSContext* cx, HandleNativeObject obj, HandleId id)
{
    // Like addProperty(Internal), but optimized for the common case of adding a
    // new enumerable data property.

    AutoCheckShapeConsistency check(obj);

    // Fast path for non-dictionary shapes with a single kid.
    do {
        AutoCheckCannotGC nogc;

        Shape* lastProperty = obj->lastProperty();
        if (lastProperty->inDictionary())
            break;

        KidsPointer* kidp = &lastProperty->kids;
        if (!kidp->isShape())
            break;

        Shape* kid = kidp->toShape();
        MOZ_ASSERT(!kid->inDictionary());

        if (kid->propidRaw() != id ||
            kid->isAccessorShape() ||
            kid->attributes() != JSPROP_ENUMERATE ||
            kid->base()->unowned() != lastProperty->base()->unowned())
        {
            break;
        }

        MOZ_ASSERT(kid->isDataProperty());

        kid = PropertyTreeReadBarrier(lastProperty, kid);
        if (!kid)
            break;

        if (!obj->setLastProperty(cx, kid))
            return nullptr;
        return kid;
    } while (0);

    AutoKeepShapeTables keep(cx);
    ShapeTable* table = nullptr;
    ShapeTable::Entry* entry = nullptr;

    if (!obj->inDictionaryMode()) {
        if (MOZ_UNLIKELY(ShouldConvertToDictionary(obj))) {
            if (!toDictionaryMode(cx, obj))
                return nullptr;
            table = obj->lastProperty()->maybeTable(keep);
            entry = &table->search<MaybeAdding::Adding>(id, keep);
        }
    } else {
        table = obj->lastProperty()->ensureTableForDictionary(cx, keep);
        if (!table)
            return nullptr;
        if (table->needsToGrow()) {
            if (!table->grow(cx))
                return nullptr;
        }
        entry = &table->search<MaybeAdding::Adding>(id, keep);
        MOZ_ASSERT(!entry->shape());
    }

    MOZ_ASSERT(!!table == !!entry);

    /* Find or create a property tree node labeled by our arguments. */
    RootedShape last(cx, obj->lastProperty());
    UnownedBaseShape* nbase = GetBaseShapeForNewShape(cx, last, id);
    if (!nbase)
        return nullptr;

    Shape* shape;
    if (obj->inDictionaryMode()) {
        uint32_t slot;
        if (!allocDictionarySlot(cx, obj, &slot))
            return nullptr;

        Rooted<StackShape> child(cx, StackShape(nbase, id, slot, JSPROP_ENUMERATE));

        MOZ_ASSERT(last == obj->lastProperty());
        shape = Allocate<Shape>(cx);
        if (!shape)
            return nullptr;
        if (slot >= obj->lastProperty()->base()->slotSpan()) {
            if (MOZ_UNLIKELY(!obj->setSlotSpan(cx, slot + 1))) {
                new (shape) Shape(obj->lastProperty()->base()->unowned(), 0);
                return nullptr;
            }
        }
        shape->initDictionaryShape(child, obj->numFixedSlots(), obj->shapePtr());
    } else {
        uint32_t slot = obj->slotSpan();
        MOZ_ASSERT(slot >= JSSLOT_FREE(obj->getClass()));
        // Objects with many properties are converted to dictionary
        // mode, so we can't overflow SHAPE_MAXIMUM_SLOT here.
        MOZ_ASSERT(slot < JSSLOT_FREE(obj->getClass()) + PropertyTree::MAX_HEIGHT);
        MOZ_ASSERT(slot < SHAPE_MAXIMUM_SLOT);

        Rooted<StackShape> child(cx, StackShape(nbase, id, slot, JSPROP_ENUMERATE));
        shape = cx->zone()->propertyTree().inlinedGetChild(cx, last, child);
        if (!shape)
            return nullptr;
        if (!obj->setLastProperty(cx, shape))
            return nullptr;
    }

    MOZ_ASSERT(shape == obj->lastProperty());

    if (table)
        shape->updateDictionaryTable(table, entry, keep);

    return shape;
}

Shape*
js::ReshapeForAllocKind(JSContext* cx, Shape* shape, TaggedProto proto,
                                 gc::AllocKind allocKind)
{
    // Compute the number of fixed slots with the new allocation kind.
    size_t nfixed = gc::GetGCKindSlots(allocKind, shape->getObjectClass());

    // Get all the ids in the shape, in order.
    js::AutoIdVector ids(cx);
    {
        for (unsigned i = 0; i < shape->slotSpan(); i++) {
            if (!ids.append(JSID_VOID))
                return nullptr;
        }
        Shape* nshape = shape;
        while (!nshape->isEmptyShape()) {
            ids[nshape->slot()].set(nshape->propid());
            nshape = nshape->previous();
        }
    }

    // Construct the new shape, without updating type information.
    RootedId id(cx);
    RootedShape newShape(cx, EmptyShape::getInitialShape(cx, shape->getObjectClass(),
                                                         proto, nfixed, shape->getObjectFlags()));
    if (!newShape)
        return nullptr;

    for (unsigned i = 0; i < ids.length(); i++) {
        id = ids[i];

        UnownedBaseShape* nbase = GetBaseShapeForNewShape(cx, newShape, id);
        if (!nbase)
            return nullptr;

        Rooted<StackShape> child(cx, StackShape(nbase, id, i, JSPROP_ENUMERATE));
        newShape = cx->zone()->propertyTree().getChild(cx, newShape, child);
        if (!newShape)
            return nullptr;
    }

    return newShape;
}

/*
 * Assert some invariants that should hold when changing properties. It's the
 * responsibility of the callers to ensure these hold.
 */
static void
AssertCanChangeAttrs(Shape* shape, unsigned attrs)
{
#ifdef DEBUG
    if (shape->configurable())
        return;

    /* A permanent property must stay permanent. */
    MOZ_ASSERT(attrs & JSPROP_PERMANENT);

    /* Reject attempts to remove a slot from the permanent data property. */
    MOZ_ASSERT_IF(shape->isDataProperty(),
                  !(attrs & (JSPROP_GETTER | JSPROP_SETTER)));
#endif
}

static void
AssertValidArrayIndex(NativeObject* obj, jsid id)
{
#ifdef DEBUG
    if (obj->is<ArrayObject>()) {
        ArrayObject* arr = &obj->as<ArrayObject>();
        uint32_t index;
        if (IdIsIndex(id, &index))
            MOZ_ASSERT(index < arr->length() || arr->lengthIsWritable());
    }
#endif
}

/* static */ bool
NativeObject::maybeToDictionaryModeForPut(JSContext* cx, HandleNativeObject obj,
                                          MutableHandleShape shape)
{
    // Overwriting a non-last property requires switching to dictionary mode.
    // The shape tree is shared immutable, and we can't removeProperty and then
    // addAccessorPropertyInternal because a failure under add would lose data.

    if (shape == obj->lastProperty() || obj->inDictionaryMode())
        return true;

    if (!toDictionaryMode(cx, obj))
        return false;

    AutoCheckCannotGC nogc;
    ShapeTable* table = obj->lastProperty()->maybeTable(nogc);
    MOZ_ASSERT(table);
    shape.set(table->search<MaybeAdding::NotAdding>(shape->propid(), nogc).shape());
    return true;
}

/* static */ Shape*
NativeObject::putDataProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                              unsigned attrs)
{
    MOZ_ASSERT(!JSID_IS_VOID(id));

    AutoCheckShapeConsistency check(obj);
    AssertValidArrayIndex(obj, id);

    // Search for id in order to claim its entry if table has been allocated.
    AutoKeepShapeTables keep(cx);
    RootedShape shape(cx);
    {
        ShapeTable* table;
        ShapeTable::Entry* entry;
        if (!Shape::search<MaybeAdding::Adding>(cx, obj->lastProperty(), id, keep,
                                                shape.address(), &table, &entry))
        {
            return nullptr;
        }

        if (!shape) {
            MOZ_ASSERT(obj->nonProxyIsExtensible(),
                       "Can't add new property to non-extensible object");
            return addDataPropertyInternal(cx, obj, id, SHAPE_INVALID_SLOT, attrs, table, entry,
                                           keep);
        }

        // Property exists: search must have returned a valid entry.
        MOZ_ASSERT_IF(entry, !entry->isRemoved());
    }

    AssertCanChangeAttrs(shape, attrs);

    // If the caller wants to allocate a slot, but doesn't care which slot,
    // copy the existing shape's slot into slot so we can match shape, if all
    // other members match.
    bool hadSlot = shape->isDataProperty();
    uint32_t oldSlot = shape->maybeSlot();
    uint32_t slot = hadSlot ? oldSlot : SHAPE_INVALID_SLOT;

    Rooted<UnownedBaseShape*> nbase(cx);
    {
        RootedShape shape(cx, obj->lastProperty());
        nbase = GetBaseShapeForNewShape(cx, shape, id);
        if (!nbase)
            return nullptr;
    }

    // Now that we've possibly preserved slot, check whether all members match.
    // If so, this is a redundant "put" and we can return without more work.
    if (shape->matchesParamsAfterId(nbase, slot, attrs, nullptr, nullptr))
        return shape;

    if (!maybeToDictionaryModeForPut(cx, obj, &shape))
        return nullptr;

    MOZ_ASSERT_IF(shape->isDataProperty(), shape->slot() == slot);

    if (obj->inDictionaryMode()) {
        // Updating some property in a dictionary-mode object. Create a new
        // shape for the existing property, and also generate a new shape for
        // the last property of the dictionary (unless the modified property
        // is also the last property).
        bool updateLast = (shape == obj->lastProperty());
        shape = NativeObject::replaceWithNewEquivalentShape(cx, obj, shape, nullptr,
                                                            /* accessorShape = */ false);
        if (!shape)
            return nullptr;
        if (!updateLast && !NativeObject::generateOwnShape(cx, obj))
            return nullptr;

        if (slot == SHAPE_INVALID_SLOT) {
            if (!allocDictionarySlot(cx, obj, &slot))
                return nullptr;
        }

        if (updateLast)
            shape->base()->adoptUnowned(nbase);
        else
            shape->base_ = nbase;

        shape->setSlot(slot);
        shape->attrs = uint8_t(attrs);
        shape->flags = Shape::IN_DICTIONARY;
    } else {
        // Updating the last property in a non-dictionary-mode object. Find an
        // alternate shared child of the last property's previous shape.

        MOZ_ASSERT(shape == obj->lastProperty());

        // Find or create a property tree node labeled by our arguments.
        Rooted<StackShape> child(cx, StackShape(nbase, id, slot, attrs));
        RootedShape parent(cx, shape->parent);
        shape = getChildDataProperty(cx, obj, parent, &child);
        if (!shape)
            return nullptr;
    }

    MOZ_ASSERT(shape->isDataProperty());
    return shape;
}

/* static */ Shape*
NativeObject::putAccessorProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                                  GetterOp getter, SetterOp setter, unsigned attrs)
{
    MOZ_ASSERT(!JSID_IS_VOID(id));

    AutoCheckShapeConsistency check(obj);
    AssertValidArrayIndex(obj, id);

    AutoRooterGetterSetter gsRoot(cx, attrs, &getter, &setter);

    // Search for id in order to claim its entry if table has been allocated.
    AutoKeepShapeTables keep(cx);
    RootedShape shape(cx);
    {
        ShapeTable* table;
        ShapeTable::Entry* entry;
        if (!Shape::search<MaybeAdding::Adding>(cx, obj->lastProperty(), id, keep,
                                                shape.address(), &table, &entry))
        {
            return nullptr;
        }

        if (!shape) {
            MOZ_ASSERT(obj->nonProxyIsExtensible(),
                       "Can't add new property to non-extensible object");
            return addAccessorPropertyInternal(cx, obj, id, getter, setter, attrs, table, entry,
                                               keep);
        }

        // Property exists: search must have returned a valid entry.
        MOZ_ASSERT_IF(entry, !entry->isRemoved());
    }

    AssertCanChangeAttrs(shape, attrs);

    bool hadSlot = shape->isDataProperty();
    uint32_t oldSlot = shape->maybeSlot();

    Rooted<UnownedBaseShape*> nbase(cx);
    {
        RootedShape shape(cx, obj->lastProperty());
        nbase = GetBaseShapeForNewShape(cx, shape, id);
        if (!nbase)
            return nullptr;
    }

    // Check whether all members match. If so, this is a redundant "put" and we can
    // return without more work.
    if (shape->matchesParamsAfterId(nbase, SHAPE_INVALID_SLOT, attrs, getter, setter))
        return shape;

    if (!maybeToDictionaryModeForPut(cx, obj, &shape))
        return nullptr;

    if (obj->inDictionaryMode()) {
        // Updating some property in a dictionary-mode object. Create a new
        // shape for the existing property, and also generate a new shape for
        // the last property of the dictionary (unless the modified property
        // is also the last property).
        bool updateLast = (shape == obj->lastProperty());
        shape = NativeObject::replaceWithNewEquivalentShape(cx, obj, shape, nullptr,
                                                            /* accessorShape = */ true);
        if (!shape)
            return nullptr;
        if (!updateLast && !NativeObject::generateOwnShape(cx, obj))
            return nullptr;

        if (updateLast)
            shape->base()->adoptUnowned(nbase);
        else
            shape->base_ = nbase;

        shape->setSlot(SHAPE_INVALID_SLOT);
        shape->attrs = uint8_t(attrs);
        shape->flags = Shape::IN_DICTIONARY | Shape::ACCESSOR_SHAPE;

        AccessorShape& accShape = shape->asAccessorShape();
        accShape.rawGetter = getter;
        accShape.rawSetter = setter;
        GetterSetterWriteBarrierPost(&accShape);
    } else {
        // Updating the last property in a non-dictionary-mode object. Find an
        // alternate shared child of the last property's previous shape.

        MOZ_ASSERT(shape == obj->lastProperty());

        // Find or create a property tree node labeled by our arguments.
        Rooted<StackShape> child(cx, StackShape(nbase, id, SHAPE_INVALID_SLOT, attrs));
        child.updateGetterSetter(getter, setter);
        RootedShape parent(cx, shape->parent);
        shape = getChildAccessorProperty(cx, obj, parent, &child);
        if (!shape)
            return nullptr;
    }

    // Can't fail now, so free the previous incarnation's slot. But we do not
    // need to free oldSlot (and must not, as trying to will botch an assertion
    // in NativeObject::freeSlot) if the new last property (shape here) has a
    // slotSpan that does not cover it.
    if (hadSlot && oldSlot < obj->slotSpan())
        obj->freeSlot(cx, oldSlot);

    MOZ_ASSERT(!shape->isDataProperty());
    return shape;
}

/* static */ Shape*
NativeObject::changeProperty(JSContext* cx, HandleNativeObject obj, HandleShape shape,
                             unsigned attrs, GetterOp getter, SetterOp setter)
{
    MOZ_ASSERT(obj->containsPure(shape));

    AutoCheckShapeConsistency check(obj);

    /* Allow only shared (slotless) => unshared (slotful) transition. */
#ifdef DEBUG
    bool needSlot = Shape::isDataProperty(attrs, getter, setter);
    MOZ_ASSERT_IF(shape->isDataProperty() != needSlot, needSlot);
#endif

    MarkTypePropertyNonData(cx, obj, shape->propid());

    AssertCanChangeAttrs(shape, attrs);

    if (shape->attrs == attrs && shape->getter() == getter && shape->setter() == setter)
        return shape;

    RootedId propid(cx, shape->propid());
    return putAccessorProperty(cx, obj, propid, getter, setter, attrs);
}

/* static */ bool
NativeObject::removeProperty(JSContext* cx, HandleNativeObject obj, jsid id_)
{
    RootedId id(cx, id_);

    AutoKeepShapeTables keep(cx);
    ShapeTable* table;
    ShapeTable::Entry* entry;
    RootedShape shape(cx);
    if (!Shape::search(cx, obj->lastProperty(), id, keep, shape.address(), &table, &entry))
        return false;

    if (!shape)
        return true;

    /*
     * If shape is not the last property added, or the last property cannot
     * be removed, switch to dictionary mode.
     */
    if (!obj->inDictionaryMode() && (shape != obj->lastProperty() || !obj->canRemoveLastProperty())) {
        if (!toDictionaryMode(cx, obj))
            return false;
        table = obj->lastProperty()->maybeTable(keep);
        MOZ_ASSERT(table);
        entry = &table->search<MaybeAdding::NotAdding>(shape->propid(), keep);
        shape = entry->shape();
    }

    /*
     * If in dictionary mode, get a new shape for the last property after the
     * removal. We need a fresh shape for all dictionary deletions, even of
     * the last property. Otherwise, a shape could replay and caches might
     * return deleted DictionaryShapes! See bug 595365. Do this before changing
     * the object or table, so the remaining removal is infallible.
     */
    RootedShape spare(cx);
    if (obj->inDictionaryMode()) {
        /* For simplicity, always allocate an accessor shape for now. */
        spare = Allocate<AccessorShape>(cx);
        if (!spare)
            return false;
        new (spare) Shape(shape->base()->unowned(), 0);
        if (shape == obj->lastProperty()) {
            /*
             * Get an up to date unowned base shape for the new last property
             * when removing the dictionary's last property. Information in
             * base shapes for non-last properties may be out of sync with the
             * object's state.
             */
            RootedShape previous(cx, obj->lastProperty()->parent);
            StackBaseShape base(obj->lastProperty()->base());
            BaseShape* nbase = BaseShape::getUnowned(cx, base);
            if (!nbase)
                return false;
            previous->base_ = nbase;
        }
    }

    /* If shape has a slot, free its slot number. */
    if (shape->isDataProperty())
        obj->freeSlot(cx, shape->slot());

    /*
     * A dictionary-mode object owns mutable, unique shapes on a non-circular
     * doubly linked list, hashed by lastProperty()->table. So we can edit the
     * list and hash in place.
     */
    if (obj->inDictionaryMode()) {
        MOZ_ASSERT(obj->lastProperty()->maybeTable(keep) == table);

        if (entry->hadCollision()) {
            entry->setRemoved();
            table->decEntryCount();
            table->incRemovedCount();
        } else {
            entry->setFree();
            table->decEntryCount();

#ifdef DEBUG
            /*
             * Check the consistency of the table but limit the number of
             * checks not to alter significantly the complexity of the
             * delete in debug builds, see bug 534493.
             */
            Shape* aprop = obj->lastProperty();
            for (int n = 50; --n >= 0 && aprop->parent; aprop = aprop->parent)
                MOZ_ASSERT_IF(aprop != shape, obj->contains(cx, aprop));
#endif
        }

        {
            /* Remove shape from its non-circular doubly linked list. */
            Shape* oldLastProp = obj->lastProperty();
            shape->removeFromDictionary(obj);

            /* Hand off table from the old to new last property. */
            oldLastProp->handoffTableTo(obj->lastProperty());
        }

        /* Generate a new shape for the object, infallibly. */
        JS_ALWAYS_TRUE(NativeObject::generateOwnShape(cx, obj, spare));

        /* Consider shrinking table if its load factor is <= .25. */
        uint32_t size = table->capacity();
        if (size > ShapeTable::MIN_SIZE && table->entryCount() <= size >> 2)
            (void) table->change(cx, -1);
    } else {
        /*
         * Non-dictionary-mode shape tables are shared immutables, so all we
         * need do is retract the last property and we'll either get or else
         * lazily make via a later hashify the exact table for the new property
         * lineage.
         */
        MOZ_ASSERT(shape == obj->lastProperty());
        obj->removeLastProperty(cx);
    }

    obj->checkShapeConsistency();
    return true;
}

/* static */ void
NativeObject::clear(JSContext* cx, HandleNativeObject obj)
{
    Shape* shape = obj->lastProperty();
    MOZ_ASSERT(obj->inDictionaryMode() == shape->inDictionary());

    while (shape->parent) {
        shape = shape->parent;
        MOZ_ASSERT(obj->inDictionaryMode() == shape->inDictionary());
    }
    MOZ_ASSERT(shape->isEmptyShape());

    if (obj->inDictionaryMode())
        shape->listp = obj->shapePtr();

    JS_ALWAYS_TRUE(obj->setLastProperty(cx, shape));

    obj->checkShapeConsistency();
}

/* static */ bool
NativeObject::rollbackProperties(JSContext* cx, HandleNativeObject obj, uint32_t slotSpan)
{
    /*
     * Remove properties from this object until it has a matching slot span.
     * The object cannot have escaped in a way which would prevent safe
     * removal of the last properties.
     */
    MOZ_ASSERT(!obj->inDictionaryMode() && slotSpan <= obj->slotSpan());
    while (true) {
        if (obj->lastProperty()->isEmptyShape()) {
            MOZ_ASSERT(slotSpan == 0);
            break;
        } else {
            uint32_t slot = obj->lastProperty()->slot();
            if (slot < slotSpan)
                break;
        }
        if (!NativeObject::removeProperty(cx, obj, obj->lastProperty()->propid()))
            return false;
    }

    return true;
}

/* static */ Shape*
NativeObject::replaceWithNewEquivalentShape(JSContext* cx, HandleNativeObject obj,
                                            Shape* oldShape, Shape* newShape, bool accessorShape)
{
    MOZ_ASSERT(cx->isInsideCurrentZone(oldShape));
    MOZ_ASSERT_IF(oldShape != obj->lastProperty(),
                  obj->inDictionaryMode() && obj->lookup(cx, oldShape->propidRef()) == oldShape);

    if (!obj->inDictionaryMode()) {
        RootedShape newRoot(cx, newShape);
        if (!toDictionaryMode(cx, obj))
            return nullptr;
        oldShape = obj->lastProperty();
        newShape = newRoot;
    }

    if (!newShape) {
        RootedShape oldRoot(cx, oldShape);
        newShape = (oldShape->isAccessorShape() || accessorShape)
                   ? Allocate<AccessorShape>(cx)
                   : Allocate<Shape>(cx);
        if (!newShape)
            return nullptr;
        new (newShape) Shape(oldRoot->base()->unowned(), 0);
        oldShape = oldRoot;
    }

    AutoCheckCannotGC nogc;
    ShapeTable* table = obj->lastProperty()->ensureTableForDictionary(cx, nogc);
    if (!table)
        return nullptr;

    ShapeTable::Entry* entry = oldShape->isEmptyShape()
        ? nullptr
        : &table->search<MaybeAdding::NotAdding>(oldShape->propidRef(), nogc);

    /*
     * Splice the new shape into the same position as the old shape, preserving
     * enumeration order (see bug 601399).
     */
    StackShape nshape(oldShape);
    newShape->initDictionaryShape(nshape, obj->numFixedSlots(), oldShape->listp);

    MOZ_ASSERT(newShape->parent == oldShape);
    oldShape->removeFromDictionary(obj);

    if (newShape == obj->lastProperty())
        oldShape->handoffTableTo(newShape);

    if (entry)
        entry->setPreservingCollision(newShape);
    return newShape;
}

/* static */ bool
JSObject::setFlags(JSContext* cx, HandleObject obj, BaseShape::Flag flags,
                   GenerateShape generateShape)
{
    if (obj->hasAllFlags(flags))
        return true;

    Shape* existingShape = obj->ensureShape(cx);
    if (!existingShape)
        return false;

    if (obj->isNative() && obj->as<NativeObject>().inDictionaryMode()) {
        if (generateShape == GENERATE_SHAPE) {
            if (!NativeObject::generateOwnShape(cx, obj.as<NativeObject>()))
                return false;
        }
        StackBaseShape base(obj->as<NativeObject>().lastProperty());
        base.flags |= flags;
        UnownedBaseShape* nbase = BaseShape::getUnowned(cx, base);
        if (!nbase)
            return false;

        obj->as<NativeObject>().lastProperty()->base()->adoptUnowned(nbase);
        return true;
    }

    Shape* newShape = Shape::setObjectFlags(cx, flags, obj->taggedProto(), existingShape);
    if (!newShape)
        return false;

    // The success of the |JSObject::ensureShape| call above means that |obj|
    // can be assumed to have a shape.
    obj->as<ShapedObject>().setShape(newShape);

    return true;
}

/* static */ bool
NativeObject::clearFlag(JSContext* cx, HandleNativeObject obj, BaseShape::Flag flag)
{
    MOZ_ASSERT(obj->lastProperty()->getObjectFlags() & flag);

    if (!obj->inDictionaryMode()) {
        if (!toDictionaryMode(cx, obj))
            return false;
    }

    StackBaseShape base(obj->lastProperty());
    base.flags &= ~flag;
    UnownedBaseShape* nbase = BaseShape::getUnowned(cx, base);
    if (!nbase)
        return false;

    obj->lastProperty()->base()->adoptUnowned(nbase);
    return true;
}

/* static */ Shape*
Shape::setObjectFlags(JSContext* cx, BaseShape::Flag flags, TaggedProto proto, Shape* last)
{
    if ((last->getObjectFlags() & flags) == flags)
        return last;

    StackBaseShape base(last);
    base.flags |= flags;

    RootedShape lastRoot(cx, last);
    return replaceLastProperty(cx, base, proto, lastRoot);
}

inline
BaseShape::BaseShape(const StackBaseShape& base)
  : clasp_(base.clasp),
    flags(base.flags),
    slotSpan_(0),
    unowned_(nullptr),
    table_(nullptr)
{
}

/* static */ void
BaseShape::copyFromUnowned(BaseShape& dest, UnownedBaseShape& src)
{
    dest.clasp_ = src.clasp_;
    dest.slotSpan_ = src.slotSpan_;
    dest.unowned_ = &src;
    dest.flags = src.flags | OWNED_SHAPE;
}

inline void
BaseShape::adoptUnowned(UnownedBaseShape* other)
{
    // This is a base shape owned by a dictionary object, update it to reflect the
    // unowned base shape of a new last property.
    MOZ_ASSERT(isOwned());

    uint32_t span = slotSpan();

    BaseShape::copyFromUnowned(*this, *other);
    setSlotSpan(span);

    assertConsistency();
}

/* static */ UnownedBaseShape*
BaseShape::getUnowned(JSContext* cx, StackBaseShape& base)
{
    auto& table = cx->zone()->baseShapes();

    if (!table.initialized() && !table.init()) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    auto p = MakeDependentAddPtr(cx, table, base);
    if (p)
        return *p;

    BaseShape* nbase_ = Allocate<BaseShape>(cx);
    if (!nbase_)
        return nullptr;

    new (nbase_) BaseShape(base);

    UnownedBaseShape* nbase = static_cast<UnownedBaseShape*>(nbase_);

    if (!p.add(cx, table, base, nbase))
        return nullptr;

    return nbase;
}

void
BaseShape::assertConsistency()
{
#ifdef DEBUG
    if (isOwned()) {
        UnownedBaseShape* unowned = baseUnowned();
        MOZ_ASSERT(getObjectFlags() == unowned->getObjectFlags());
    }
#endif
}

void
BaseShape::traceChildren(JSTracer* trc)
{
    traceChildrenSkipShapeTable(trc);
    traceShapeTable(trc);
}

void
BaseShape::traceChildrenSkipShapeTable(JSTracer* trc)
{
    if (isOwned())
        TraceEdge(trc, &unowned_, "base");

    assertConsistency();
}

void
BaseShape::traceShapeTable(JSTracer* trc)
{
    AutoCheckCannotGC nogc;
    if (ShapeTable* table = maybeTable(nogc))
        table->trace(trc);
}

#ifdef DEBUG
bool
BaseShape::canSkipMarkingShapeTable(Shape* lastShape)
{
    // Check that every shape in the shape table will be marked by marking
    // |lastShape|.

    AutoCheckCannotGC nogc;
    ShapeTable* table = maybeTable(nogc);
    if (!table)
        return true;

    uint32_t count = 0;
    for (Shape::Range<NoGC> r(lastShape); !r.empty(); r.popFront()) {
        Shape* shape = &r.front();
        ShapeTable::Entry& entry = table->search<MaybeAdding::NotAdding>(shape->propid(), nogc);
        if (entry.isLive())
            count++;
    }

    return count == table->entryCount();
}
#endif

#ifdef JSGC_HASH_TABLE_CHECKS

void
Zone::checkBaseShapeTableAfterMovingGC()
{
    if (!baseShapes().initialized())
        return;

    for (auto r = baseShapes().all(); !r.empty(); r.popFront()) {
        UnownedBaseShape* base = r.front().unbarrieredGet();
        CheckGCThingAfterMovingGC(base);

        BaseShapeSet::Ptr ptr = baseShapes().lookup(base);
        MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
    }
}

#endif // JSGC_HASH_TABLE_CHECKS

void
BaseShape::finalize(FreeOp* fop)
{
    if (table_) {
        fop->delete_(table_);
        table_ = nullptr;
    }
}

inline
InitialShapeEntry::InitialShapeEntry() : shape(nullptr), proto()
{
}

inline
InitialShapeEntry::InitialShapeEntry(Shape* shape, const Lookup::ShapeProto& proto)
  : shape(shape), proto(proto)
{
}

#ifdef JSGC_HASH_TABLE_CHECKS

void
Zone::checkInitialShapesTableAfterMovingGC()
{
    if (!initialShapes().initialized())
        return;

    /*
     * Assert that the postbarriers have worked and that nothing is left in
     * initialShapes that points into the nursery, and that the hash table
     * entries are discoverable.
     */
    for (auto r = initialShapes().all(); !r.empty(); r.popFront()) {
        InitialShapeEntry entry = r.front();
        JSProtoKey protoKey = entry.proto.key();
        TaggedProto proto = entry.proto.proto().unbarrieredGet();
        Shape* shape = entry.shape.unbarrieredGet();

        CheckGCThingAfterMovingGC(shape);
        if (proto.isObject())
            CheckGCThingAfterMovingGC(proto.toObject());

        using Lookup = InitialShapeEntry::Lookup;
        Lookup lookup(shape->getObjectClass(),
                      Lookup::ShapeProto(protoKey, proto),
                      shape->numFixedSlots(),
                      shape->getObjectFlags());
        InitialShapeSet::Ptr ptr = initialShapes().lookup(lookup);
        MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
    }
}

#endif // JSGC_HASH_TABLE_CHECKS

Shape*
EmptyShape::new_(JSContext* cx, Handle<UnownedBaseShape*> base, uint32_t nfixed)
{
    Shape* shape = Allocate<Shape>(cx);
    if (!shape) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    new (shape) EmptyShape(base, nfixed);
    return shape;
}

MOZ_ALWAYS_INLINE HashNumber
ShapeHasher::hash(const Lookup& l)
{
    return l.hash();
}

MOZ_ALWAYS_INLINE bool
ShapeHasher::match(const Key k, const Lookup& l)
{
    return k->matches(l);
}

static KidsHash*
HashChildren(Shape* kid1, Shape* kid2)
{
    KidsHash* hash = js_new<KidsHash>();
    if (!hash || !hash->init(2)) {
        js_delete(hash);
        return nullptr;
    }

    hash->putNewInfallible(StackShape(kid1), kid1);
    hash->putNewInfallible(StackShape(kid2), kid2);
    return hash;
}

bool
PropertyTree::insertChild(JSContext* cx, Shape* parent, Shape* child)
{
    MOZ_ASSERT(!parent->inDictionary());
    MOZ_ASSERT(!child->parent);
    MOZ_ASSERT(!child->inDictionary());
    MOZ_ASSERT(child->zone() == parent->zone());
    MOZ_ASSERT(cx->zone() == zone_);

    KidsPointer* kidp = &parent->kids;

    if (kidp->isNull()) {
        child->setParent(parent);
        kidp->setShape(child);
        return true;
    }

    if (kidp->isShape()) {
        Shape* shape = kidp->toShape();
        MOZ_ASSERT(shape != child);
        MOZ_ASSERT(!shape->matches(child));

        KidsHash* hash = HashChildren(shape, child);
        if (!hash) {
            ReportOutOfMemory(cx);
            return false;
        }
        kidp->setHash(hash);
        child->setParent(parent);
        return true;
    }

    if (!kidp->toHash()->putNew(StackShape(child), child)) {
        ReportOutOfMemory(cx);
        return false;
    }

    child->setParent(parent);
    return true;
}

void
Shape::removeChild(Shape* child)
{
    MOZ_ASSERT(!child->inDictionary());
    MOZ_ASSERT(child->parent == this);

    KidsPointer* kidp = &kids;

    if (kidp->isShape()) {
        MOZ_ASSERT(kidp->toShape() == child);
        kidp->setNull();
        child->parent = nullptr;
        return;
    }

    KidsHash* hash = kidp->toHash();
    MOZ_ASSERT(hash->count() >= 2);      /* otherwise kidp->isShape() should be true */

#ifdef DEBUG
    size_t oldCount = hash->count();
#endif

    hash->remove(StackShape(child));
    child->parent = nullptr;

    MOZ_ASSERT(hash->count() == oldCount - 1);

    if (hash->count() == 1) {
        /* Convert from HASH form back to SHAPE form. */
        KidsHash::Range r = hash->all();
        Shape* otherChild = r.front();
        MOZ_ASSERT((r.popFront(), r.empty()));    /* No more elements! */
        kidp->setShape(otherChild);
        js_delete(hash);
    }
}

MOZ_ALWAYS_INLINE Shape*
PropertyTree::inlinedGetChild(JSContext* cx, Shape* parent, Handle<StackShape> child)
{
    MOZ_ASSERT(parent);

    Shape* existingShape = nullptr;

    /*
     * The property tree has extremely low fan-out below its root in
     * popular embeddings with real-world workloads. Patterns such as
     * defining closures that capture a constructor's environment as
     * getters or setters on the new object that is passed in as
     * |this| can significantly increase fan-out below the property
     * tree root -- see bug 335700 for details.
     */
    KidsPointer* kidp = &parent->kids;
    if (kidp->isShape()) {
        Shape* kid = kidp->toShape();
        if (kid->matches(child))
            existingShape = kid;
    } else if (kidp->isHash()) {
        if (KidsHash::Ptr p = kidp->toHash()->lookup(child))
            existingShape = *p;
    } else {
        /* If kidp->isNull(), we always insert. */
    }

    if (existingShape) {
        existingShape = PropertyTreeReadBarrier(parent, existingShape);
        if (existingShape)
            return existingShape;
    }

    RootedShape parentRoot(cx, parent);
    Shape* shape = Shape::new_(cx, child, parentRoot->numFixedSlots());
    if (!shape)
        return nullptr;

    if (!insertChild(cx, parentRoot, shape))
        return nullptr;

    return shape;
}

Shape*
PropertyTree::getChild(JSContext* cx, Shape* parent, Handle<StackShape> child)
{
    return inlinedGetChild(cx, parent, child);
}

void
Shape::sweep()
{
    /*
     * We detach the child from the parent if the parent is reachable.
     *
     * This test depends on shape arenas not being freed until after we finish
     * incrementally sweeping them. If that were not the case the parent pointer
     * could point to a marked cell that had been deallocated and then
     * reallocated, since allocating a cell in a zone that is being marked will
     * set the mark bit for that cell.
     */
    if (parent && parent->isMarkedAny()) {
        if (inDictionary()) {
            if (parent->listp == &parent)
                parent->listp = nullptr;
        } else {
            parent->removeChild(this);
        }
    }
}

void
Shape::finalize(FreeOp* fop)
{
    if (!inDictionary() && kids.isHash())
        fop->delete_(kids.toHash());
}

void
Shape::fixupDictionaryShapeAfterMovingGC()
{
    if (!listp)
        return;

    // The listp field either points to the parent field of the next shape in
    // the list if there is one.  Otherwise if this shape is the last in the
    // list then it points to the shape_ field of the object the list is for.
    // We can tell which it is because the base shape is owned if this is the
    // last property and not otherwise.
    bool listpPointsIntoShape = !MaybeForwarded(base())->isOwned();

#ifdef DEBUG
    // Check that we got this right by interrogating the arena.
    // We use a fake cell pointer for this: it might not point to the beginning
    // of a cell, but will point into the right arena and will have the right
    // alignment.
    Cell* cell = reinterpret_cast<Cell*>(uintptr_t(listp) & ~CellAlignMask);
    AllocKind kind = TenuredCell::fromPointer(cell)->getAllocKind();
    MOZ_ASSERT_IF(listpPointsIntoShape, IsShapeAllocKind(kind));
    MOZ_ASSERT_IF(!listpPointsIntoShape, IsObjectAllocKind(kind));
#endif

    if (listpPointsIntoShape) {
        // listp points to the parent field of the next shape.
        Shape* next = Shape::fromParentFieldPointer(uintptr_t(listp));
        if (gc::IsForwarded(next))
            listp = &gc::Forwarded(next)->parent;
    } else {
        // listp points to the shape_ field of an object.
        JSObject* last = ShapedObject::fromShapeFieldPointer(uintptr_t(listp));
        if (gc::IsForwarded(last))
            listp = gc::Forwarded(last)->as<NativeObject>().shapePtr();
    }
}

void
Shape::fixupShapeTreeAfterMovingGC()
{
    if (kids.isNull())
        return;

    if (kids.isShape()) {
        if (gc::IsForwarded(kids.toShape()))
            kids.setShape(gc::Forwarded(kids.toShape()));
        return;
    }

    MOZ_ASSERT(kids.isHash());
    KidsHash* kh = kids.toHash();
    for (KidsHash::Enum e(*kh); !e.empty(); e.popFront()) {
        Shape* key = e.front();
        if (IsForwarded(key))
            key = Forwarded(key);

        BaseShape* base = key->base();
        if (IsForwarded(base))
            base = Forwarded(base);
        UnownedBaseShape* unowned = base->unowned();
        if (IsForwarded(unowned))
            unowned = Forwarded(unowned);

        GetterOp getter = key->getter();
        if (key->hasGetterObject())
            getter = GetterOp(MaybeForwarded(key->getterObject()));

        SetterOp setter = key->setter();
        if (key->hasSetterObject())
            setter = SetterOp(MaybeForwarded(key->setterObject()));

        StackShape lookup(unowned,
                          const_cast<Shape*>(key)->propidRef(),
                          key->slotInfo & Shape::SLOT_MASK,
                          key->attrs);
        lookup.updateGetterSetter(getter, setter);
        e.rekeyFront(lookup, key);
    }
}

void
Shape::fixupAfterMovingGC()
{
    if (inDictionary())
        fixupDictionaryShapeAfterMovingGC();
    else
        fixupShapeTreeAfterMovingGC();
}

void
NurseryShapesRef::trace(JSTracer* trc)
{
    auto& shapes = zone_->nurseryShapes();
    for (auto shape : shapes)
        shape->fixupGetterSetterForBarrier(trc);
    shapes.clearAndFree();
}

void
Shape::fixupGetterSetterForBarrier(JSTracer* trc)
{
    if (!hasGetterValue() && !hasSetterValue())
        return;

    JSObject* priorGetter = asAccessorShape().getterObj;
    JSObject* priorSetter = asAccessorShape().setterObj;
    if (!priorGetter && !priorSetter)
        return;

    JSObject* postGetter = priorGetter;
    JSObject* postSetter = priorSetter;
    if (priorGetter)
        TraceManuallyBarrieredEdge(trc, &postGetter, "getterObj");
    if (priorSetter)
        TraceManuallyBarrieredEdge(trc, &postSetter, "setterObj");
    if (priorGetter == postGetter && priorSetter == postSetter)
        return;

    if (parent && !parent->inDictionary() && parent->kids.isHash()) {
        // Relocating the getterObj or setterObj will have changed our location
        // in our parent's KidsHash, so take care to update it.  We must do this
        // before we update the shape itself, since the shape is used to match
        // the original entry in the hash set.

        StackShape original(this);
        StackShape updated(this);
        updated.rawGetter = reinterpret_cast<GetterOp>(postGetter);
        updated.rawSetter = reinterpret_cast<SetterOp>(postSetter);

        KidsHash* kh = parent->kids.toHash();
        MOZ_ALWAYS_TRUE(kh->rekeyAs(original, updated, this));
    }

    asAccessorShape().getterObj = postGetter;
    asAccessorShape().setterObj = postSetter;

    MOZ_ASSERT_IF(parent && !parent->inDictionary() && parent->kids.isHash(),
                  parent->kids.toHash()->has(StackShape(this)));
}

#ifdef DEBUG

void
KidsPointer::checkConsistency(Shape* aKid) const
{
    if (isShape()) {
        MOZ_ASSERT(toShape() == aKid);
    } else {
        MOZ_ASSERT(isHash());
        KidsHash* hash = toHash();
        KidsHash::Ptr ptr = hash->lookup(StackShape(aKid));
        MOZ_ASSERT(*ptr == aKid);
    }
}

void
Shape::dump(js::GenericPrinter& out) const
{
    jsid propid = this->propid();

    MOZ_ASSERT(!JSID_IS_VOID(propid));

    if (JSID_IS_INT(propid)) {
        out.printf("[%ld]", (long) JSID_TO_INT(propid));
    } else if (JSID_IS_ATOM(propid)) {
        if (JSLinearString* str = JSID_TO_ATOM(propid))
            EscapedStringPrinter(out, str, '"');
        else
            out.put("<error>");
    } else {
        MOZ_ASSERT(JSID_IS_SYMBOL(propid));
        JSID_TO_SYMBOL(propid)->dump(out);
    }

    out.printf(" g/s %p/%p slot %d attrs %x ",
               JS_FUNC_TO_DATA_PTR(void*, getter()),
               JS_FUNC_TO_DATA_PTR(void*, setter()),
               isDataProperty() ? slot() : -1, attrs);

    if (attrs) {
        int first = 1;
        out.putChar('(');
#define DUMP_ATTR(name, display) if (attrs & JSPROP_##name) out.put(&(" " #display)[first]), first = 0
        DUMP_ATTR(ENUMERATE, enumerate);
        DUMP_ATTR(READONLY, readonly);
        DUMP_ATTR(PERMANENT, permanent);
        DUMP_ATTR(GETTER, getter);
        DUMP_ATTR(SETTER, setter);
#undef  DUMP_ATTR
        out.putChar(')');
    }

    out.printf("flags %x ", flags);
    if (flags) {
        int first = 1;
        out.putChar('(');
#define DUMP_FLAG(name, display) if (flags & name) out.put(&(" " #display)[first]), first = 0
        DUMP_FLAG(IN_DICTIONARY, in_dictionary);
#undef  DUMP_FLAG
        out.putChar(')');
    }
}

void
Shape::dump() const
{
    Fprinter out(stderr);
    dump(out);
}

void
Shape::dumpSubtree(int level, js::GenericPrinter& out) const
{
    if (!parent) {
        MOZ_ASSERT(level == 0);
        MOZ_ASSERT(JSID_IS_EMPTY(propid_));
        out.printf("class %s emptyShape\n", getObjectClass()->name);
    } else {
        out.printf("%*sid ", level, "");
        dump(out);
    }

    if (!kids.isNull()) {
        ++level;
        if (kids.isShape()) {
            Shape* kid = kids.toShape();
            MOZ_ASSERT(kid->parent == this);
            kid->dumpSubtree(level, out);
        } else {
            const KidsHash& hash = *kids.toHash();
            for (KidsHash::Range range = hash.all(); !range.empty(); range.popFront()) {
                Shape* kid = range.front();

                MOZ_ASSERT(kid->parent == this);
                kid->dumpSubtree(level, out);
            }
        }
    }
}

#endif

static bool
IsOriginalProto(GlobalObject* global, JSProtoKey key, JSObject& proto)
{
    if (global->getPrototype(key) != ObjectValue(proto))
        return false;

    if (key == JSProto_Object) {
        MOZ_ASSERT(proto.staticPrototypeIsImmutable(),
                   "proto should be Object.prototype, whose prototype is "
                   "immutable");
        MOZ_ASSERT(proto.staticPrototype() == nullptr,
                   "Object.prototype must have null prototype");
        return true;
    }

    // Check that other prototypes still have Object.prototype as proto.
    JSObject* protoProto = proto.staticPrototype();
    if (!protoProto || global->getPrototype(JSProto_Object) != ObjectValue(*protoProto))
        return false;

    MOZ_ASSERT(protoProto->staticPrototypeIsImmutable(),
               "protoProto should be Object.prototype, whose prototype is "
               "immutable");
    MOZ_ASSERT(protoProto->staticPrototype() == nullptr,
               "Object.prototype must have null prototype");
    return true;
}

static JSProtoKey
GetInitialShapeProtoKey(TaggedProto proto, JSContext* cx)
{
    if (proto.isObject() && proto.toObject()->hasStaticPrototype()) {
        GlobalObject* global = cx->global();
        JSObject& obj = *proto.toObject();
        MOZ_ASSERT(global == &obj.global());

        if (IsOriginalProto(global, JSProto_Object, obj))
            return JSProto_Object;
        if (IsOriginalProto(global, JSProto_Function, obj))
            return JSProto_Function;
        if (IsOriginalProto(global, JSProto_Array, obj))
            return JSProto_Array;
        if (IsOriginalProto(global, JSProto_RegExp, obj))
            return JSProto_RegExp;
    }
    return JSProto_LIMIT;
}

/* static */ Shape*
EmptyShape::getInitialShape(JSContext* cx, const Class* clasp, TaggedProto proto,
                            size_t nfixed, uint32_t objectFlags)
{
    MOZ_ASSERT_IF(proto.isObject(), cx->isInsideCurrentCompartment(proto.toObject()));

    auto& table = cx->zone()->initialShapes();

    if (!table.initialized() && !table.init()) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    using Lookup = InitialShapeEntry::Lookup;
    auto protoPointer = MakeDependentAddPtr(cx, table,
                                            Lookup(clasp, Lookup::ShapeProto(proto),
                                                   nfixed, objectFlags));
    if (protoPointer)
        return protoPointer->shape;

    // No entry for this proto. If the proto is one of a few common builtin
    // prototypes, try to do a lookup based on the JSProtoKey, so we can share
    // shapes across globals.
    Rooted<TaggedProto> protoRoot(cx, proto);
    Shape* shape = nullptr;
    bool insertKey = false;
    mozilla::Maybe<DependentAddPtr<InitialShapeSet>> keyPointer;

    JSProtoKey key = GetInitialShapeProtoKey(protoRoot, cx);
    if (key != JSProto_LIMIT) {
        keyPointer.emplace(MakeDependentAddPtr(cx, table,
                                               Lookup(clasp, Lookup::ShapeProto(key),
                                                      nfixed, objectFlags)));
        if (keyPointer.ref()) {
            shape = keyPointer.ref()->shape;
            MOZ_ASSERT(shape);
        } else {
            insertKey = true;
        }
    }

    if (!shape) {
        StackBaseShape base(clasp, objectFlags);
        Rooted<UnownedBaseShape*> nbase(cx, BaseShape::getUnowned(cx, base));
        if (!nbase)
            return nullptr;

        shape = EmptyShape::new_(cx, nbase, nfixed);
        if (!shape)
            return nullptr;
    }

    Lookup::ShapeProto shapeProto(protoRoot);
    Lookup lookup(clasp, shapeProto, nfixed, objectFlags);
    if (!protoPointer.add(cx, table, lookup, InitialShapeEntry(shape, shapeProto)))
        return nullptr;

    // Also add an entry based on the JSProtoKey, if needed.
    if (insertKey) {
        Lookup::ShapeProto shapeProto(key);
        Lookup lookup(clasp, shapeProto, nfixed, objectFlags);
        if (!keyPointer->add(cx, table, lookup, InitialShapeEntry(shape, shapeProto)))
            return nullptr;
    }

    return shape;
}

/* static */ Shape*
EmptyShape::getInitialShape(JSContext* cx, const Class* clasp, TaggedProto proto,
                            AllocKind kind, uint32_t objectFlags)
{
    return getInitialShape(cx, clasp, proto, GetGCKindSlots(kind, clasp), objectFlags);
}

void
NewObjectCache::invalidateEntriesForShape(JSContext* cx, HandleShape shape, HandleObject proto)
{
    const Class* clasp = shape->getObjectClass();

    gc::AllocKind kind = gc::GetGCObjectKind(shape->numFixedSlots());
    if (CanBeFinalizedInBackground(kind, clasp))
        kind = GetBackgroundAllocKind(kind);

    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, clasp, TaggedProto(proto)));
    if (!group) {
        purge();
        cx->recoverFromOutOfMemory();
        return;
    }

    EntryIndex entry;
    for (CompartmentsInZoneIter comp(shape->zone()); !comp.done(); comp.next()) {
        if (GlobalObject* global = comp->unsafeUnbarrieredMaybeGlobal()) {
            if (lookupGlobal(clasp, global, kind, &entry))
                PodZero(&entries[entry]);
        }
    }
    if (!proto->is<GlobalObject>() && lookupProto(clasp, proto, kind, &entry))
        PodZero(&entries[entry]);
    if (lookupGroup(group, kind, &entry))
        PodZero(&entries[entry]);
}

/* static */ void
EmptyShape::insertInitialShape(JSContext* cx, HandleShape shape, HandleObject proto)
{
    using Lookup = InitialShapeEntry::Lookup;
    Lookup lookup(shape->getObjectClass(), Lookup::ShapeProto(TaggedProto(proto)),
                  shape->numFixedSlots(), shape->getObjectFlags());

    InitialShapeSet::Ptr p = cx->zone()->initialShapes().lookup(lookup);
    MOZ_ASSERT(p);

    InitialShapeEntry& entry = const_cast<InitialShapeEntry&>(*p);

    // The metadata callback can end up causing redundant changes of the initial shape.
    if (entry.shape == shape)
        return;

    // The new shape had better be rooted at the old one.
#ifdef DEBUG
    Shape* nshape = shape;
    while (!nshape->isEmptyShape())
        nshape = nshape->previous();
    MOZ_ASSERT(nshape == entry.shape);
#endif

    entry.shape = ReadBarrieredShape(shape);

    // For certain prototypes -- namely, those of various builtin classes,
    // keyed by JSProtoKey |key| -- there are two entries: one for a lookup
    // via |proto|, and one for a lookup via |key|.  If this is such a
    // prototype, also update the alternate |key|-keyed shape.
    JSProtoKey key = GetInitialShapeProtoKey(TaggedProto(proto), cx);
    if (key != JSProto_LIMIT) {
        Lookup lookup(shape->getObjectClass(), Lookup::ShapeProto(key),
                      shape->numFixedSlots(), shape->getObjectFlags());
        if (InitialShapeSet::Ptr p = cx->zone()->initialShapes().lookup(lookup)) {
            InitialShapeEntry& entry = const_cast<InitialShapeEntry&>(*p);
            if (entry.shape != shape)
                entry.shape = ReadBarrieredShape(shape);
        }
    }

    /*
     * This affects the shape that will be produced by the various NewObject
     * methods, so clear any cache entry referring to the old shape. This is
     * not required for correctness: the NewObject must always check for a
     * nativeEmpty() result and generate the appropriate properties if found.
     * Clearing the cache entry avoids this duplicate regeneration.
     *
     * Clearing is not necessary when this context is running off
     * thread, as it will not use the new object cache for allocations.
     */
    if (!cx->helperThread())
        cx->caches().newObjectCache.invalidateEntriesForShape(cx, shape, proto);
}

void
Zone::fixupInitialShapeTable()
{
    if (!initialShapes().initialized())
        return;

    for (InitialShapeSet::Enum e(initialShapes()); !e.empty(); e.popFront()) {
        // The shape may have been moved, but we can update that in place.
        Shape* shape = e.front().shape.unbarrieredGet();
        if (IsForwarded(shape)) {
            shape = Forwarded(shape);
            e.mutableFront().shape.set(shape);
        }
        shape->updateBaseShapeAfterMovingGC();

        // If the prototype has moved we have to rekey the entry.
        InitialShapeEntry entry = e.front();
        if (entry.proto.proto().isObject() && IsForwarded(entry.proto.proto().toObject())) {
            entry.proto.setProto(TaggedProto(Forwarded(entry.proto.proto().toObject())));
            using Lookup = InitialShapeEntry::Lookup;
            Lookup relookup(shape->getObjectClass(),
                            Lookup::ShapeProto(entry.proto),
                            shape->numFixedSlots(),
                            shape->getObjectFlags());
            e.rekeyFront(relookup, entry);
        }
    }
}

void
AutoRooterGetterSetter::Inner::trace(JSTracer* trc)
{
    if ((attrs & JSPROP_GETTER) && *pgetter)
        TraceRoot(trc, (JSObject**) pgetter, "AutoRooterGetterSetter getter");
    if ((attrs & JSPROP_SETTER) && *psetter)
        TraceRoot(trc, (JSObject**) psetter, "AutoRooterGetterSetter setter");
}

JS::ubi::Node::Size
JS::ubi::Concrete<js::Shape>::size(mozilla::MallocSizeOf mallocSizeOf) const
{
    Size size = js::gc::Arena::thingSize(get().asTenured().getAllocKind());

    AutoCheckCannotGC nogc;
    if (ShapeTable* table = get().maybeTable(nogc))
        size += table->sizeOfIncludingThis(mallocSizeOf);

    if (!get().inDictionary() && get().kids.isHash())
        size += get().kids.toHash()->sizeOfIncludingThis(mallocSizeOf);

    return size;
}

JS::ubi::Node::Size
JS::ubi::Concrete<js::BaseShape>::size(mozilla::MallocSizeOf mallocSizeOf) const
{
    return js::gc::Arena::thingSize(get().asTenured().getAllocKind());
}

void
PropertyResult::trace(JSTracer* trc)
{
    if (isNativeProperty())
        TraceRoot(trc, &shape_, "PropertyResult::shape_");
}
