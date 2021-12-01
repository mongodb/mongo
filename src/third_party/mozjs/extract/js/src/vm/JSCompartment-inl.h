/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSCompartment_inl_h
#define vm_JSCompartment_inl_h

#include "vm/JSCompartment.h"

#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "vm/Iteration.h"

#include "vm/JSContext-inl.h"

inline void
JSCompartment::initGlobal(js::GlobalObject& global)
{
    MOZ_ASSERT(global.compartment() == this);
    MOZ_ASSERT(!global_);
    global_.set(&global);
}

js::GlobalObject*
JSCompartment::maybeGlobal() const
{
    MOZ_ASSERT_IF(global_, global_->compartment() == this);
    return global_;
}

js::GlobalObject*
JSCompartment::unsafeUnbarrieredMaybeGlobal() const
{
    return *global_.unsafeGet();
}

inline bool
JSCompartment::globalIsAboutToBeFinalized()
{
    MOZ_ASSERT(zone_->isGCSweeping());
    return global_ && js::gc::IsAboutToBeFinalizedUnbarriered(global_.unsafeGet());
}

template <typename T>
js::AutoCompartment::AutoCompartment(JSContext* cx, const T& target)
  : cx_(cx),
    origin_(cx->compartment()),
    maybeLock_(nullptr)
{
    cx_->enterCompartmentOf(target);
}

// Protected constructor that bypasses assertions in enterCompartmentOf. Used
// only for entering the atoms compartment.
js::AutoCompartment::AutoCompartment(JSContext* cx, JSCompartment* target,
                                     js::AutoLockForExclusiveAccess& lock)
  : cx_(cx),
    origin_(cx->compartment()),
    maybeLock_(&lock)
{
    MOZ_ASSERT(target->isAtomsCompartment());
    cx_->enterAtomsCompartment(target, lock);
}

// Protected constructor that bypasses assertions in enterCompartmentOf. Should
// not be used to enter the atoms compartment.
js::AutoCompartment::AutoCompartment(JSContext* cx, JSCompartment* target)
  : cx_(cx),
    origin_(cx->compartment()),
    maybeLock_(nullptr)
{
    MOZ_ASSERT(!target->isAtomsCompartment());
    cx_->enterNonAtomsCompartment(target);
}

js::AutoCompartment::~AutoCompartment()
{
    cx_->leaveCompartment(origin_, maybeLock_);
}

js::AutoAtomsCompartment::AutoAtomsCompartment(JSContext* cx,
                                               js::AutoLockForExclusiveAccess& lock)
  : AutoCompartment(cx, cx->atomsCompartment(lock), lock)
{}

js::AutoCompartmentUnchecked::AutoCompartmentUnchecked(JSContext* cx, JSCompartment* target)
  : AutoCompartment(cx, target)
{}

inline bool
JSCompartment::wrap(JSContext* cx, JS::MutableHandleValue vp)
{
    /* Only GC things have to be wrapped or copied. */
    if (!vp.isGCThing())
        return true;

    /*
     * Symbols are GC things, but never need to be wrapped or copied because
     * they are always allocated in the atoms compartment. They still need to
     * be marked in the new compartment's zone, however.
     */
    if (vp.isSymbol()) {
        cx->markAtomValue(vp);
        return true;
    }

    /* Handle strings. */
    if (vp.isString()) {
        JS::RootedString str(cx, vp.toString());
        if (!wrap(cx, &str))
            return false;
        vp.setString(str);
        return true;
    }

    MOZ_ASSERT(vp.isObject());

    /*
     * All that's left are objects.
     *
     * Object wrapping isn't the fastest thing in the world, in part because
     * we have to unwrap and invoke the prewrap hook to find the identity
     * object before we even start checking the cache. Neither of these
     * operations are needed in the common case, where we're just wrapping
     * a plain JS object from the wrappee's side of the membrane to the
     * wrapper's side.
     *
     * To optimize this, we note that the cache should only ever contain
     * identity objects - that is to say, objects that serve as the
     * canonical representation for a unique object identity observable by
     * script. Unwrap and prewrap are both steps that we take to get to the
     * identity of an incoming objects, and as such, they shuld never map
     * one identity object to another object. This means that we can safely
     * check the cache immediately, and only risk false negatives. Do this
     * in opt builds, and do both in debug builds so that we can assert
     * that we get the same answer.
     */
#ifdef DEBUG
    MOZ_ASSERT(JS::ValueIsNotGray(vp));
    JS::RootedObject cacheResult(cx);
#endif
    JS::RootedValue v(cx, vp);
    if (js::WrapperMap::Ptr p = crossCompartmentWrappers.lookup(js::CrossCompartmentKey(v))) {
#ifdef DEBUG
        cacheResult = &p->value().get().toObject();
#else
        vp.set(p->value().get());
        return true;
#endif
    }

    JS::RootedObject obj(cx, &vp.toObject());
    if (!wrap(cx, &obj))
        return false;
    vp.setObject(*obj);
    MOZ_ASSERT_IF(cacheResult, obj == cacheResult);
    return true;
}

MOZ_ALWAYS_INLINE bool
JSCompartment::objectMaybeInIteration(JSObject* obj)
{
    MOZ_ASSERT(obj->compartment() == this);

    // If the list is empty we're not iterating any objects.
    js::NativeIterator* next = enumerators->next();
    if (enumerators == next)
        return false;

    // If the list contains a single object, check if it's |obj|.
    if (next->next() == enumerators)
        return next->obj == obj;

    return true;
}

#endif /* vm_JSCompartment_inl_h */
