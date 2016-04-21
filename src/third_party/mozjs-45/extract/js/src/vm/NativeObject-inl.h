/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_NativeObject_inl_h
#define vm_NativeObject_inl_h

#include "vm/NativeObject.h"

#include "jscntxt.h"

#include "builtin/TypedObject.h"
#include "proxy/Proxy.h"
#include "vm/ProxyObject.h"
#include "vm/TypedArrayObject.h"

#include "jsobjinlines.h"

namespace js {

inline uint8_t*
NativeObject::fixedData(size_t nslots) const
{
    MOZ_ASSERT(ClassCanHaveFixedData(getClass()));
    MOZ_ASSERT(nslots == numFixedSlots() + (hasPrivate() ? 1 : 0));
    return reinterpret_cast<uint8_t*>(&fixedSlots()[nslots]);
}

inline void
NativeObject::removeLastProperty(ExclusiveContext* cx)
{
    MOZ_ASSERT(canRemoveLastProperty());
    JS_ALWAYS_TRUE(setLastProperty(cx, lastProperty()->previous()));
}

inline bool
NativeObject::canRemoveLastProperty()
{
    /*
     * Check that the information about the object stored in the last
     * property's base shape is consistent with that stored in the previous
     * shape. If not consistent, then the last property cannot be removed as it
     * will induce a change in the object itself, and the object must be
     * converted to dictionary mode instead. See BaseShape comment in jsscope.h
     */
    MOZ_ASSERT(!inDictionaryMode());
    Shape* previous = lastProperty()->previous().get();
    return previous->getObjectFlags() == lastProperty()->getObjectFlags();
}

inline void
NativeObject::setShouldConvertDoubleElements()
{
    MOZ_ASSERT(is<ArrayObject>() && !hasEmptyElements());
    getElementsHeader()->setShouldConvertDoubleElements();
}

inline void
NativeObject::clearShouldConvertDoubleElements()
{
    MOZ_ASSERT(is<ArrayObject>() && !hasEmptyElements());
    getElementsHeader()->clearShouldConvertDoubleElements();
}

inline void
NativeObject::setDenseElementWithType(ExclusiveContext* cx, uint32_t index, const Value& val)
{
    // Avoid a slow AddTypePropertyId call if the type is the same as the type
    // of the previous element.
    TypeSet::Type thisType = TypeSet::GetValueType(val);
    if (index == 0 || TypeSet::GetValueType(elements_[index - 1]) != thisType)
        AddTypePropertyId(cx, this, JSID_VOID, thisType);
    setDenseElementMaybeConvertDouble(index, val);
}

inline void
NativeObject::initDenseElementWithType(ExclusiveContext* cx, uint32_t index, const Value& val)
{
    MOZ_ASSERT(!shouldConvertDoubleElements());
    if (val.isMagic(JS_ELEMENTS_HOLE))
        markDenseElementsNotPacked(cx);
    else
        AddTypePropertyId(cx, this, JSID_VOID, val);
    initDenseElement(index, val);
}

inline void
NativeObject::setDenseElementHole(ExclusiveContext* cx, uint32_t index)
{
    MarkObjectGroupFlags(cx, this, OBJECT_FLAG_NON_PACKED);
    setDenseElement(index, MagicValue(JS_ELEMENTS_HOLE));
}

/* static */ inline void
NativeObject::removeDenseElementForSparseIndex(ExclusiveContext* cx,
                                               HandleNativeObject obj, uint32_t index)
{
    MarkObjectGroupFlags(cx, obj, OBJECT_FLAG_NON_PACKED | OBJECT_FLAG_SPARSE_INDEXES);
    if (obj->containsDenseElement(index))
        obj->setDenseElement(index, MagicValue(JS_ELEMENTS_HOLE));
}

inline bool
NativeObject::writeToIndexWouldMarkNotPacked(uint32_t index)
{
    return getElementsHeader()->initializedLength < index;
}

inline void
NativeObject::markDenseElementsNotPacked(ExclusiveContext* cx)
{
    MOZ_ASSERT(isNative());
    MarkObjectGroupFlags(cx, this, OBJECT_FLAG_NON_PACKED);
}

inline void
NativeObject::ensureDenseInitializedLengthNoPackedCheck(ExclusiveContext* cx, uint32_t index,
                                                        uint32_t extra)
{
    MOZ_ASSERT(!denseElementsAreCopyOnWrite());

    /*
     * Ensure that the array's contents have been initialized up to index, and
     * mark the elements through 'index + extra' as initialized in preparation
     * for a write.
     */
    MOZ_ASSERT(index + extra <= getDenseCapacity());
    uint32_t& initlen = getElementsHeader()->initializedLength;

    if (initlen < index + extra) {
        size_t offset = initlen;
        for (HeapSlot* sp = elements_ + initlen;
             sp != elements_ + (index + extra);
             sp++, offset++)
        {
            sp->init(this, HeapSlot::Element, offset, MagicValue(JS_ELEMENTS_HOLE));
        }
        initlen = index + extra;
    }
}

inline void
NativeObject::ensureDenseInitializedLength(ExclusiveContext* cx, uint32_t index, uint32_t extra)
{
    if (writeToIndexWouldMarkNotPacked(index))
        markDenseElementsNotPacked(cx);
    ensureDenseInitializedLengthNoPackedCheck(cx, index, extra);
}

DenseElementResult
NativeObject::extendDenseElements(ExclusiveContext* cx,
                                  uint32_t requiredCapacity, uint32_t extra)
{
    MOZ_ASSERT(!denseElementsAreCopyOnWrite());

    /*
     * Don't grow elements for non-extensible objects or watched objects. Dense
     * elements can be added/written with no extensible or watchpoint checks as
     * long as there is capacity for them.
     */
    if (!nonProxyIsExtensible() || watched()) {
        MOZ_ASSERT(getDenseCapacity() == 0);
        return DenseElementResult::Incomplete;
    }

    /*
     * Don't grow elements for objects which already have sparse indexes.
     * This avoids needing to count non-hole elements in willBeSparseElements
     * every time a new index is added.
     */
    if (isIndexed())
        return DenseElementResult::Incomplete;

    /*
     * We use the extra argument also as a hint about number of non-hole
     * elements to be inserted.
     */
    if (requiredCapacity > MIN_SPARSE_INDEX &&
        willBeSparseElements(requiredCapacity, extra)) {
        return DenseElementResult::Incomplete;
    }

    if (!growElements(cx, requiredCapacity))
        return DenseElementResult::Failure;

    return DenseElementResult::Success;
}

inline DenseElementResult
NativeObject::ensureDenseElements(ExclusiveContext* cx, uint32_t index, uint32_t extra)
{
    MOZ_ASSERT(isNative());

    if (writeToIndexWouldMarkNotPacked(index))
        markDenseElementsNotPacked(cx);

    if (!maybeCopyElementsForWrite(cx))
        return DenseElementResult::Failure;

    uint32_t currentCapacity = getDenseCapacity();

    uint32_t requiredCapacity;
    if (extra == 1) {
        /* Optimize for the common case. */
        if (index < currentCapacity) {
            ensureDenseInitializedLengthNoPackedCheck(cx, index, 1);
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
        if (requiredCapacity <= currentCapacity) {
            ensureDenseInitializedLengthNoPackedCheck(cx, index, extra);
            return DenseElementResult::Success;
        }
    }

    DenseElementResult result = extendDenseElements(cx, requiredCapacity, extra);
    if (result != DenseElementResult::Success)
        return result;

    ensureDenseInitializedLengthNoPackedCheck(cx, index, extra);
    return DenseElementResult::Success;
}

inline Value
NativeObject::getDenseOrTypedArrayElement(uint32_t idx)
{
    if (is<TypedArrayObject>())
        return as<TypedArrayObject>().getElement(idx);
    return getDenseElement(idx);
}

/* static */ inline NativeObject*
NativeObject::copy(ExclusiveContext* cx, gc::AllocKind kind, gc::InitialHeap heap,
                   HandleNativeObject templateObject)
{
    RootedShape shape(cx, templateObject->lastProperty());
    RootedObjectGroup group(cx, templateObject->group());
    MOZ_ASSERT(!templateObject->denseElementsAreCopyOnWrite());

    JSObject* baseObj = create(cx, kind, heap, shape, group);
    if (!baseObj)
        return nullptr;
    NativeObject* obj = &baseObj->as<NativeObject>();

    size_t span = shape->slotSpan();
    if (span) {
        uint32_t numFixed = templateObject->numFixedSlots();
        const Value* fixed = &templateObject->getSlot(0);
        // Only copy elements which are registered in the shape, even if the
        // number of fixed slots is larger.
        if (span < numFixed)
            numFixed = span;
        obj->copySlotRange(0, fixed, numFixed);

        if (numFixed < span) {
            uint32_t numSlots = span - numFixed;
            const Value* slots = &templateObject->getSlot(numFixed);
            obj->copySlotRange(numFixed, slots, numSlots);
        }
    }

    return obj;
}

inline void
NativeObject::setSlotWithType(ExclusiveContext* cx, Shape* shape,
                              const Value& value, bool overwriting)
{
    setSlot(shape->slot(), value);

    if (overwriting)
        shape->setOverwritten();

    AddTypePropertyId(cx, this, shape->propid(), value);
}

/* Make an object with pregenerated shape from a NEWOBJECT bytecode. */
static inline PlainObject*
CopyInitializerObject(JSContext* cx, HandlePlainObject baseobj, NewObjectKind newKind = GenericObject)
{
    MOZ_ASSERT(!baseobj->inDictionaryMode());

    gc::AllocKind allocKind = gc::GetGCObjectFixedSlotsKind(baseobj->numFixedSlots());
    allocKind = gc::GetBackgroundAllocKind(allocKind);
    MOZ_ASSERT_IF(baseobj->isTenured(), allocKind == baseobj->asTenured().getAllocKind());
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx, allocKind, newKind));
    if (!obj)
        return nullptr;

    if (!obj->setLastProperty(cx, baseobj->lastProperty()))
        return nullptr;

    return obj;
}

inline NativeObject*
NewNativeObjectWithGivenTaggedProto(ExclusiveContext* cx, const Class* clasp,
                                    Handle<TaggedProto> proto,
                                    gc::AllocKind allocKind, NewObjectKind newKind)
{
    return MaybeNativeObject(NewObjectWithGivenTaggedProto(cx, clasp, proto, allocKind,
                                                           newKind));
}

inline NativeObject*
NewNativeObjectWithGivenTaggedProto(ExclusiveContext* cx, const Class* clasp,
                                    Handle<TaggedProto> proto,
                                    NewObjectKind newKind = GenericObject)
{
    return MaybeNativeObject(NewObjectWithGivenTaggedProto(cx, clasp, proto, newKind));
}

inline NativeObject*
NewNativeObjectWithGivenProto(ExclusiveContext* cx, const Class* clasp,
                              HandleObject proto,
                              gc::AllocKind allocKind, NewObjectKind newKind)
{
    return MaybeNativeObject(NewObjectWithGivenProto(cx, clasp, proto, allocKind, newKind));
}

inline NativeObject*
NewNativeObjectWithGivenProto(ExclusiveContext* cx, const Class* clasp,
                              HandleObject proto,
                              NewObjectKind newKind = GenericObject)
{
    return MaybeNativeObject(NewObjectWithGivenProto(cx, clasp, proto, newKind));
}

inline NativeObject*
NewNativeObjectWithClassProto(ExclusiveContext* cx, const Class* clasp, HandleObject proto,
                              gc::AllocKind allocKind,
                              NewObjectKind newKind = GenericObject)
{
    return MaybeNativeObject(NewObjectWithClassProto(cx, clasp, proto, allocKind, newKind));
}

inline NativeObject*
NewNativeObjectWithClassProto(ExclusiveContext* cx, const Class* clasp, HandleObject proto,
                              NewObjectKind newKind = GenericObject)
{
    return MaybeNativeObject(NewObjectWithClassProto(cx, clasp, proto, newKind));
}

/*
 * Call obj's resolve hook.
 *
 * cx and id are the parameters initially passed to the ongoing lookup;
 * propp and recursedp are its out parameters.
 *
 * There are four possible outcomes:
 *
 *   - On failure, report an error or exception and return false.
 *
 *   - If we are already resolving a property of obj, set *recursedp = true,
 *     and return true.
 *
 *   - If the resolve hook finds or defines the sought property, set propp
 *      appropriately, set *recursedp = false, and return true.
 *
 *   - Otherwise no property was resolved. Set propp to nullptr and
 *     *recursedp = false and return true.
 */
static MOZ_ALWAYS_INLINE bool
CallResolveOp(JSContext* cx, HandleNativeObject obj, HandleId id, MutableHandleShape propp,
              bool* recursedp)
{
    // Avoid recursion on (obj, id) already being resolved on cx.
    AutoResolving resolving(cx, obj, id);
    if (resolving.alreadyStarted()) {
        // Already resolving id in obj, suppress recursion.
        *recursedp = true;
        return true;
    }
    *recursedp = false;

    bool resolved = false;
    if (!obj->getClass()->resolve(cx, obj, id, &resolved))
        return false;

    if (!resolved)
        return true;

    // Assert the mayResolve hook, if there is one, returns true for this
    // property.
    MOZ_ASSERT_IF(obj->getClass()->mayResolve,
                  obj->getClass()->mayResolve(cx->names(), id, obj));

    if (JSID_IS_INT(id) && obj->containsDenseElement(JSID_TO_INT(id))) {
        MarkDenseOrTypedArrayElementFound<CanGC>(propp);
        return true;
    }

    MOZ_ASSERT(!IsAnyTypedArray(obj));

    propp.set(obj->lookup(cx, id));
    return true;
}

static MOZ_ALWAYS_INLINE bool
ClassMayResolveId(const JSAtomState& names, const Class* clasp, jsid id, JSObject* maybeObj)
{
    MOZ_ASSERT_IF(maybeObj, maybeObj->getClass() == clasp);

    if (!clasp->resolve) {
        // Sanity check: we should only have a mayResolve hook if we have a
        // resolve hook.
        MOZ_ASSERT(!clasp->mayResolve, "Class with mayResolve hook but no resolve hook");
        return false;
    }

    if (clasp->mayResolve) {
        // Tell the analysis our mayResolve hooks won't trigger GC.
        JS::AutoSuppressGCAnalysis nogc;
        if (!clasp->mayResolve(names, id, maybeObj))
            return false;
    }

    return true;
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE bool
LookupOwnPropertyInline(ExclusiveContext* cx,
                        typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                        typename MaybeRooted<jsid, allowGC>::HandleType id,
                        typename MaybeRooted<Shape*, allowGC>::MutableHandleType propp,
                        bool* donep)
{
    // Check for a native dense element.
    if (JSID_IS_INT(id) && obj->containsDenseElement(JSID_TO_INT(id))) {
        MarkDenseOrTypedArrayElementFound<allowGC>(propp);
        *donep = true;
        return true;
    }

    // Check for a typed array element. Integer lookups always finish here
    // so that integer properties on the prototype are ignored even for out
    // of bounds accesses.
    if (IsAnyTypedArray(obj)) {
        uint64_t index;
        if (IsTypedArrayIndex(id, &index)) {
            if (index < AnyTypedArrayLength(obj)) {
                MarkDenseOrTypedArrayElementFound<allowGC>(propp);
            } else {
                propp.set(nullptr);
            }
            *donep = true;
            return true;
        }
    }

    // Check for a native property.
    if (Shape* shape = obj->lookup(cx, id)) {
        propp.set(shape);
        *donep = true;
        return true;
    }

    // id was not found in obj. Try obj's resolve hook, if any.
    if (obj->getClass()->resolve)
    {
        if (!cx->shouldBeJSContext() || !allowGC)
            return false;

        bool recursed;
        if (!CallResolveOp(cx->asJSContext(),
                           MaybeRooted<NativeObject*, allowGC>::toHandle(obj),
                           MaybeRooted<jsid, allowGC>::toHandle(id),
                           MaybeRooted<Shape*, allowGC>::toMutableHandle(propp),
                           &recursed))
        {
            return false;
        }

        if (recursed) {
            propp.set(nullptr);
            *donep = true;
            return true;
        }

        if (propp) {
            *donep = true;
            return true;
        }
    }

    propp.set(nullptr);
    *donep = false;
    return true;
}

/*
 * Simplified version of LookupOwnPropertyInline that doesn't call resolve
 * hooks.
 */
static inline void
NativeLookupOwnPropertyNoResolve(ExclusiveContext* cx, HandleNativeObject obj, HandleId id,
                                 MutableHandleShape result)
{
    // Check for a native dense element.
    if (JSID_IS_INT(id) && obj->containsDenseElement(JSID_TO_INT(id))) {
        MarkDenseOrTypedArrayElementFound<CanGC>(result);
        return;
    }

    // Check for a typed array element.
    if (IsAnyTypedArray(obj)) {
        uint64_t index;
        if (IsTypedArrayIndex(id, &index)) {
            if (index < AnyTypedArrayLength(obj))
                MarkDenseOrTypedArrayElementFound<CanGC>(result);
            else
                result.set(nullptr);
            return;
        }
    }

    // Check for a native property.
    result.set(obj->lookup(cx, id));
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE bool
LookupPropertyInline(ExclusiveContext* cx,
                     typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
                     typename MaybeRooted<jsid, allowGC>::HandleType id,
                     typename MaybeRooted<JSObject*, allowGC>::MutableHandleType objp,
                     typename MaybeRooted<Shape*, allowGC>::MutableHandleType propp)
{
    /* NB: The logic of this procedure is implicitly reflected in
     *     BaselineIC.cpp's |EffectlesslyLookupProperty| logic.
     *     If this changes, please remember to update the logic there as well.
     */

    /* Search scopes starting with obj and following the prototype link. */
    typename MaybeRooted<NativeObject*, allowGC>::RootType current(cx, obj);

    while (true) {
        bool done;
        if (!LookupOwnPropertyInline<allowGC>(cx, current, id, propp, &done))
            return false;
        if (done) {
            if (propp)
                objp.set(current);
            else
                objp.set(nullptr);
            return true;
        }

        typename MaybeRooted<JSObject*, allowGC>::RootType proto(cx, current->getProto());

        if (!proto)
            break;
        if (!proto->isNative()) {
            if (!cx->shouldBeJSContext() || !allowGC)
                return false;
            return LookupProperty(cx->asJSContext(),
                                  MaybeRooted<JSObject*, allowGC>::toHandle(proto),
                                  MaybeRooted<jsid, allowGC>::toHandle(id),
                                  MaybeRooted<JSObject*, allowGC>::toMutableHandle(objp),
                                  MaybeRooted<Shape*, allowGC>::toMutableHandle(propp));
        }

        current = &proto->template as<NativeObject>();
    }

    objp.set(nullptr);
    propp.set(nullptr);
    return true;
}

inline bool
ThrowIfNotConstructing(JSContext *cx, const CallArgs &args, const char *builtinName)
{
    if (args.isConstructing())
        return true;
    return JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
                                        JSMSG_BUILTIN_CTOR_NO_NEW, builtinName);
}

} // namespace js

#endif /* vm_NativeObject_inl_h */
