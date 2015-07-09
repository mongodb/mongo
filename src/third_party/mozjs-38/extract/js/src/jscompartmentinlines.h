/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jscompartmentinlines_h
#define jscompartmentinlines_h

#include "jscompartment.h"

#include "gc/Barrier.h"

#include "jscntxtinlines.h"

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

js::AutoCompartment::AutoCompartment(ExclusiveContext* cx, JSObject* target)
  : cx_(cx),
    origin_(cx->compartment_)
{
    cx_->enterCompartment(target->compartment());
}

js::AutoCompartment::AutoCompartment(ExclusiveContext* cx, JSCompartment* target)
  : cx_(cx),
    origin_(cx_->compartment_)
{
    cx_->enterCompartment(target);
}

js::AutoCompartment::~AutoCompartment()
{
    cx_->leaveCompartment(origin_);
}

inline bool
JSCompartment::wrap(JSContext* cx, JS::MutableHandleValue vp, JS::HandleObject existing)
{
    MOZ_ASSERT_IF(existing, vp.isObject());

    /* Only GC things have to be wrapped or copied. */
    if (!vp.isMarkable())
        return true;

    /*
     * Symbols are GC things, but never need to be wrapped or copied because
     * they are always allocated in the atoms compartment.
     */
    if (vp.isSymbol())
        return true;

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
    JS::RootedObject cacheResult(cx);
#endif
    JS::RootedValue v(cx, vp);
    if (js::WrapperMap::Ptr p = crossCompartmentWrappers.lookup(js::CrossCompartmentKey(v))) {
#ifdef DEBUG
        cacheResult = &p->value().get().toObject();
#else
        vp.set(p->value());
        return true;
#endif
    }

    JS::RootedObject obj(cx, &vp.toObject());
    if (!wrap(cx, &obj, existing))
        return false;
    vp.setObject(*obj);
    MOZ_ASSERT_IF(cacheResult, obj == cacheResult);
    return true;
}

#endif /* jscompartmentinlines_h */
