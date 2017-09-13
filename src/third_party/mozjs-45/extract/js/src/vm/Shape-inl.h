/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Shape_inl_h
#define vm_Shape_inl_h

#include "vm/Shape.h"

#include "mozilla/TypeTraits.h"

#include "jsobj.h"

#include "gc/Allocator.h"
#include "vm/Interpreter.h"
#include "vm/ScopeObject.h"
#include "vm/TypedArrayCommon.h"

#include "jsatominlines.h"
#include "jscntxtinlines.h"

namespace js {

inline
StackBaseShape::StackBaseShape(ExclusiveContext* cx, const Class* clasp, uint32_t objectFlags)
  : flags(objectFlags),
    clasp(clasp),
    compartment(cx->compartment_)
{}

inline Shape*
Shape::search(ExclusiveContext* cx, jsid id)
{
    ShapeTable::Entry* _;
    return search(cx, this, id, &_);
}

/* static */ inline Shape*
Shape::search(ExclusiveContext* cx, Shape* start, jsid id, ShapeTable::Entry** pentry, bool adding)
{
    if (start->inDictionary()) {
        *pentry = &start->table().search(id, adding);
        return (*pentry)->shape();
    }

    *pentry = nullptr;

    if (start->hasTable()) {
        ShapeTable::Entry& entry = start->table().search(id, adding);
        return entry.shape();
    }

    if (start->numLinearSearches() == LINEAR_SEARCHES_MAX) {
        if (start->isBigEnoughForAShapeTable()) {
            if (Shape::hashify(cx, start)) {
                ShapeTable::Entry& entry = start->table().search(id, adding);
                return entry.shape();
            } else {
                cx->recoverFromOutOfMemory();
            }
        }
        /*
         * No table built -- there weren't enough entries, or OOM occurred.
         * Don't increment numLinearSearches, to keep hasTable() false.
         */
        MOZ_ASSERT(!start->hasTable());
    } else {
        start->incrementNumLinearSearches();
    }

    for (Shape* shape = start; shape; shape = shape->parent) {
        if (shape->propidRef() == id)
            return shape;
    }

    return nullptr;
}

inline Shape*
Shape::new_(ExclusiveContext* cx, Handle<StackShape> other, uint32_t nfixed)
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

template<class ObjectSubclass>
/* static */ inline bool
EmptyShape::ensureInitialCustomShape(ExclusiveContext* cx, Handle<ObjectSubclass*> obj)
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
    RootedObject proto(cx, obj->getProto());
    EmptyShape::insertInitialShape(cx, shape, proto);
    return true;
}

inline
AutoRooterGetterSetter::Inner::Inner(ExclusiveContext* cx, uint8_t attrs,
                                     GetterOp* pgetter_, SetterOp* psetter_)
  : CustomAutoRooter(cx), attrs(attrs),
    pgetter(pgetter_), psetter(psetter_)
{}

inline
AutoRooterGetterSetter::AutoRooterGetterSetter(ExclusiveContext* cx, uint8_t attrs,
                                               GetterOp* pgetter, SetterOp* psetter
                                               MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
{
    if (attrs & (JSPROP_GETTER | JSPROP_SETTER))
        inner.emplace(cx, attrs, pgetter, psetter);
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
}

inline
AutoRooterGetterSetter::AutoRooterGetterSetter(ExclusiveContext* cx, uint8_t attrs,
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
GetShapeAttributes(JSObject* obj, Shape* shape)
{
    MOZ_ASSERT(obj->isNative());

    if (IsImplicitDenseOrTypedArrayElement(shape)) {
        if (IsAnyTypedArray(obj))
            return JSPROP_ENUMERATE | JSPROP_PERMANENT;
        return JSPROP_ENUMERATE;
    }

    return shape->attributes();
}

} /* namespace js */

#endif /* vm_Shape_inl_h */
