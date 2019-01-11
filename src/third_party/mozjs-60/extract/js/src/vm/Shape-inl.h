/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Shape_inl_h
#define vm_Shape_inl_h

#include "vm/Shape.h"

#include "mozilla/TypeTraits.h"

#include "gc/Allocator.h"
#include "vm/Interpreter.h"
#include "vm/JSObject.h"
#include "vm/TypedArrayObject.h"

#include "gc/Marking-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSContext-inl.h"

namespace js {

inline
AutoKeepShapeTables::AutoKeepShapeTables(JSContext* cx)
  : cx_(cx),
    prev_(cx->zone()->keepShapeTables())
{
    cx->zone()->setKeepShapeTables(true);
}

inline
AutoKeepShapeTables::~AutoKeepShapeTables()
{
    cx_->zone()->setKeepShapeTables(prev_);
}

inline
StackBaseShape::StackBaseShape(const Class* clasp, uint32_t objectFlags)
  : flags(objectFlags),
    clasp(clasp)
{}

MOZ_ALWAYS_INLINE Shape*
Shape::search(JSContext* cx, jsid id)
{
    return search(cx, this, id);
}

MOZ_ALWAYS_INLINE bool
Shape::maybeCreateTableForLookup(JSContext* cx)
{
    if (hasTable())
        return true;

    if (!inDictionary() && numLinearSearches() < LINEAR_SEARCHES_MAX) {
        incrementNumLinearSearches();
        return true;
    }

    if (!isBigEnoughForAShapeTable())
        return true;

    return Shape::hashify(cx, this);
}

template<MaybeAdding Adding>
/* static */ inline bool
Shape::search(JSContext* cx, Shape* start, jsid id, const AutoKeepShapeTables& keep,
              Shape** pshape, ShapeTable** ptable, ShapeTable::Entry** pentry)
{
    if (start->inDictionary()) {
        ShapeTable* table = start->ensureTableForDictionary(cx, keep);
        if (!table)
            return false;
        *ptable = table;
        *pentry = &table->search<Adding>(id, keep);
        *pshape = (*pentry)->shape();
        return true;
    }

    *ptable = nullptr;
    *pentry = nullptr;
    *pshape = Shape::search<Adding>(cx, start, id);
    return true;
}

template<MaybeAdding Adding>
/* static */ MOZ_ALWAYS_INLINE Shape*
Shape::search(JSContext* cx, Shape* start, jsid id)
{
    if (start->maybeCreateTableForLookup(cx)) {
        JS::AutoCheckCannotGC nogc;
        if (ShapeTable* table = start->maybeTable(nogc)) {
            ShapeTable::Entry& entry = table->search<Adding>(id, nogc);
            return entry.shape();
        }
    } else {
        // Just do a linear search.
        cx->recoverFromOutOfMemory();
    }

    return start->searchLinear(id);
}

inline Shape*
Shape::new_(JSContext* cx, Handle<StackShape> other, uint32_t nfixed)
{
    Shape* shape = other.isAccessorShape()
                   ? js::Allocate<AccessorShape>(cx)
                   : js::Allocate<Shape>(cx);
    if (!shape) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    if (other.isAccessorShape())
        new (shape) AccessorShape(other, nfixed);
    else
        new (shape) Shape(other, nfixed);

    return shape;
}

inline void
Shape::updateBaseShapeAfterMovingGC()
{
    BaseShape* base = base_;
    if (IsForwarded(base))
        base_.unsafeSet(Forwarded(base));
}

static inline void
GetterSetterWriteBarrierPost(AccessorShape* shape)
{
    // If the shape contains any nursery pointers then add it to a vector on the
    // zone that we fixup on minor GC. Prevent this vector growing too large
    // since we don't tolerate OOM here.

    static const size_t MaxShapeVectorLength = 5000;

    MOZ_ASSERT(shape);

    if (!(shape->hasGetterObject() && IsInsideNursery(shape->getterObject())) &&
        !(shape->hasSetterObject() && IsInsideNursery(shape->setterObject())))
    {
        return;
    }

    auto& nurseryShapes = shape->zone()->nurseryShapes();

    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!nurseryShapes.append(shape))
            oomUnsafe.crash("GetterSetterWriteBarrierPost");
    }

    auto& storeBuffer = shape->runtimeFromActiveCooperatingThread()->gc.storeBuffer();
    if (nurseryShapes.length() == 1) {
        storeBuffer.putGeneric(NurseryShapesRef(shape->zone()));
    } else if (nurseryShapes.length() == MaxShapeVectorLength) {
        storeBuffer.setAboutToOverflow(JS::gcreason::FULL_SHAPE_BUFFER);
    }
}

inline
AccessorShape::AccessorShape(const StackShape& other, uint32_t nfixed)
  : Shape(other, nfixed),
    rawGetter(other.rawGetter),
    rawSetter(other.rawSetter)
{
    MOZ_ASSERT(getAllocKind() == gc::AllocKind::ACCESSOR_SHAPE);
    GetterSetterWriteBarrierPost(this);
}

inline void
Shape::initDictionaryShape(const StackShape& child, uint32_t nfixed, GCPtrShape* dictp)
{
    if (child.isAccessorShape())
        new (this) AccessorShape(child, nfixed);
    else
        new (this) Shape(child, nfixed);
    this->flags |= IN_DICTIONARY;

    this->listp = nullptr;
    if (dictp)
        insertIntoDictionary(dictp);
}

template<class ObjectSubclass>
/* static */ inline bool
EmptyShape::ensureInitialCustomShape(JSContext* cx, Handle<ObjectSubclass*> obj)
{
    static_assert(mozilla::IsBaseOf<JSObject, ObjectSubclass>::value,
                  "ObjectSubclass must be a subclass of JSObject");

    // If the provided object has a non-empty shape, it was given the cached
    // initial shape when created: nothing to do.
    if (!obj->empty())
        return true;

    // If no initial shape was assigned, do so.
    RootedShape shape(cx, ObjectSubclass::assignInitialShape(cx, obj));
    if (!shape)
        return false;
    MOZ_ASSERT(!obj->empty());

    // If the object is a standard prototype -- |RegExp.prototype|,
    // |String.prototype|, |RangeError.prototype|, &c. -- GlobalObject.cpp's
    // |CreateBlankProto| marked it as a delegate.  These are the only objects
    // of this class that won't use the standard prototype, and there's no
    // reason to pollute the initial shape cache with entries for them.
    if (obj->isDelegate())
        return true;

    // Cache the initial shape for non-prototype objects, however, so that
    // future instances will begin life with that shape.
    RootedObject proto(cx, obj->staticPrototype());
    EmptyShape::insertInitialShape(cx, shape, proto);
    return true;
}

inline
AutoRooterGetterSetter::Inner::Inner(JSContext* cx, uint8_t attrs,
                                     GetterOp* pgetter_, SetterOp* psetter_)
  : CustomAutoRooter(cx), attrs(attrs),
    pgetter(pgetter_), psetter(psetter_)
{}

inline
AutoRooterGetterSetter::AutoRooterGetterSetter(JSContext* cx, uint8_t attrs,
                                               GetterOp* pgetter, SetterOp* psetter
                                               MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
{
    if (attrs & (JSPROP_GETTER | JSPROP_SETTER))
        inner.emplace(cx, attrs, pgetter, psetter);
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
}

inline
AutoRooterGetterSetter::AutoRooterGetterSetter(JSContext* cx, uint8_t attrs,
                                               JSNative* pgetter, JSNative* psetter
                                               MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
{
    if (attrs & (JSPROP_GETTER | JSPROP_SETTER)) {
        inner.emplace(cx, attrs, reinterpret_cast<GetterOp*>(pgetter),
                      reinterpret_cast<SetterOp*>(psetter));
    }
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
}

static inline uint8_t
GetPropertyAttributes(JSObject* obj, PropertyResult prop)
{
    MOZ_ASSERT(obj->isNative());

    if (prop.isDenseOrTypedArrayElement()) {
        if (obj->is<TypedArrayObject>())
            return JSPROP_ENUMERATE | JSPROP_PERMANENT;
        return obj->as<NativeObject>().getElementsHeader()->elementAttributes();
    }

    return prop.shape()->attributes();
}

/*
 * Double hashing needs the second hash code to be relatively prime to table
 * size, so we simply make hash2 odd.
 */
MOZ_ALWAYS_INLINE HashNumber
Hash1(HashNumber hash0, uint32_t shift)
{
    return hash0 >> shift;
}

MOZ_ALWAYS_INLINE HashNumber
Hash2(HashNumber hash0, uint32_t log2, uint32_t shift)
{
    return ((hash0 << log2) >> shift) | 1;
}

template<MaybeAdding Adding>
MOZ_ALWAYS_INLINE ShapeTable::Entry&
ShapeTable::searchUnchecked(jsid id)
{
    MOZ_ASSERT(entries_);
    MOZ_ASSERT(!JSID_IS_EMPTY(id));

    /* Compute the primary hash address. */
    HashNumber hash0 = HashId(id);
    HashNumber hash1 = Hash1(hash0, hashShift_);
    Entry* entry = &getEntry(hash1);

    /* Miss: return space for a new entry. */
    if (entry->isFree())
        return *entry;

    /* Hit: return entry. */
    Shape* shape = entry->shape();
    if (shape && shape->propidRaw() == id)
        return *entry;

    /* Collision: double hash. */
    uint32_t sizeLog2 = HASH_BITS - hashShift_;
    HashNumber hash2 = Hash2(hash0, sizeLog2, hashShift_);
    uint32_t sizeMask = JS_BITMASK(sizeLog2);

    /* Save the first removed entry pointer so we can recycle it if adding. */
    Entry* firstRemoved;
    if (Adding == MaybeAdding::Adding) {
        if (entry->isRemoved()) {
            firstRemoved = entry;
        } else {
            firstRemoved = nullptr;
            if (!entry->hadCollision())
                entry->flagCollision();
        }
    }

#ifdef DEBUG
    bool collisionFlag = true;
    if (!entry->isRemoved())
        collisionFlag = entry->hadCollision();
#endif

    while (true) {
        hash1 -= hash2;
        hash1 &= sizeMask;
        entry = &getEntry(hash1);

        if (entry->isFree())
            return (Adding == MaybeAdding::Adding && firstRemoved) ? *firstRemoved : *entry;

        shape = entry->shape();
        if (shape && shape->propidRaw() == id) {
            MOZ_ASSERT(collisionFlag);
            return *entry;
        }

        if (Adding == MaybeAdding::Adding) {
            if (entry->isRemoved()) {
                if (!firstRemoved)
                    firstRemoved = entry;
            } else {
                if (!entry->hadCollision())
                    entry->flagCollision();
            }
        }

#ifdef DEBUG
        if (!entry->isRemoved())
            collisionFlag &= entry->hadCollision();
#endif
    }

    MOZ_CRASH("Shape::search failed to find an expected entry.");
}

template<MaybeAdding Adding>
MOZ_ALWAYS_INLINE ShapeTable::Entry&
ShapeTable::search(jsid id, const AutoKeepShapeTables&)
{
    return searchUnchecked<Adding>(id);
}

template<MaybeAdding Adding>
MOZ_ALWAYS_INLINE ShapeTable::Entry&
ShapeTable::search(jsid id, const JS::AutoCheckCannotGC&)
{
    return searchUnchecked<Adding>(id);
}

/*
 * Keep this function in sync with search. It neither hashifies the start
 * shape nor increments linear search count.
 */
MOZ_ALWAYS_INLINE Shape*
Shape::searchNoHashify(Shape* start, jsid id)
{
    /*
     * If we have a table, search in the shape table, else do a linear
     * search. We never hashify into a table in parallel.
     */
    JS::AutoCheckCannotGC nogc;
    if (ShapeTable* table = start->maybeTable(nogc)) {
        ShapeTable::Entry& entry = table->search<MaybeAdding::NotAdding>(id, nogc);
        return entry.shape();
    }

    return start->searchLinear(id);
}

/* static */ MOZ_ALWAYS_INLINE Shape*
NativeObject::addDataProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                              uint32_t slot, unsigned attrs)
{
    MOZ_ASSERT(!JSID_IS_VOID(id));
    MOZ_ASSERT(obj->uninlinedNonProxyIsExtensible());
    MOZ_ASSERT(!obj->containsPure(id));

    AutoKeepShapeTables keep(cx);
    ShapeTable* table = nullptr;
    ShapeTable::Entry* entry = nullptr;
    if (obj->inDictionaryMode()) {
        table = obj->lastProperty()->ensureTableForDictionary(cx, keep);
        if (!table)
            return nullptr;
        entry = &table->search<MaybeAdding::Adding>(id, keep);
    }

    return addDataPropertyInternal(cx, obj, id, slot, attrs, table, entry, keep);
}

/* static */ MOZ_ALWAYS_INLINE Shape*
NativeObject::addAccessorProperty(JSContext* cx, HandleNativeObject obj, HandleId id,
                                  GetterOp getter, SetterOp setter, unsigned attrs)
{
    MOZ_ASSERT(!JSID_IS_VOID(id));
    MOZ_ASSERT(obj->uninlinedNonProxyIsExtensible());
    MOZ_ASSERT(!obj->containsPure(id));

    AutoKeepShapeTables keep(cx);
    ShapeTable* table = nullptr;
    ShapeTable::Entry* entry = nullptr;
    if (obj->inDictionaryMode()) {
        table = obj->lastProperty()->ensureTableForDictionary(cx, keep);
        if (!table)
            return nullptr;
        entry = &table->search<MaybeAdding::Adding>(id, keep);
    }

    return addAccessorPropertyInternal(cx, obj, id, getter, setter, attrs, table, entry, keep);
}

} /* namespace js */

#endif /* vm_Shape_inl_h */
