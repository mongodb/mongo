/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/PublicIterators.h"
#include "js/Wrapper.h"
#include "proxy/DeadObjectProxy.h"
#include "vm/Iteration.h"
#include "vm/WrapperObject.h"

#include "gc/Nursery-inl.h"
#include "vm/JSCompartment-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;

#define PIERCE(cx, wrapper, pre, op, post)                      \
    JS_BEGIN_MACRO                                              \
        bool ok;                                                \
        {                                                       \
            AutoCompartment call(cx, wrappedObject(wrapper));   \
            ok = (pre) && (op);                                 \
        }                                                       \
        return ok && (post);                                    \
    JS_END_MACRO

#define NOTHING (true)

static bool
MarkAtoms(JSContext* cx, jsid id)
{
    cx->markId(id);
    return true;
}

static bool
MarkAtoms(JSContext* cx, const AutoIdVector& ids)
{
    for (size_t i = 0; i < ids.length(); i++)
        cx->markId(ids[i]);
    return true;
}

bool
CrossCompartmentWrapper::getPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                               MutableHandle<PropertyDescriptor> desc) const
{
    PIERCE(cx, wrapper,
           MarkAtoms(cx, id),
           Wrapper::getPropertyDescriptor(cx, wrapper, id, desc),
           cx->compartment()->wrap(cx, desc));
}

bool
CrossCompartmentWrapper::getOwnPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                                  MutableHandle<PropertyDescriptor> desc) const
{
    PIERCE(cx, wrapper,
           MarkAtoms(cx, id),
           Wrapper::getOwnPropertyDescriptor(cx, wrapper, id, desc),
           cx->compartment()->wrap(cx, desc));
}

bool
CrossCompartmentWrapper::defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                        Handle<PropertyDescriptor> desc,
                                        ObjectOpResult& result) const
{
    Rooted<PropertyDescriptor> desc2(cx, desc);
    PIERCE(cx, wrapper,
           MarkAtoms(cx, id) && cx->compartment()->wrap(cx, &desc2),
           Wrapper::defineProperty(cx, wrapper, id, desc2, result),
           NOTHING);
}

bool
CrossCompartmentWrapper::ownPropertyKeys(JSContext* cx, HandleObject wrapper,
                                         AutoIdVector& props) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::ownPropertyKeys(cx, wrapper, props),
           MarkAtoms(cx, props));
}

bool
CrossCompartmentWrapper::delete_(JSContext* cx, HandleObject wrapper, HandleId id,
                                 ObjectOpResult& result) const
{
    PIERCE(cx, wrapper,
           MarkAtoms(cx, id),
           Wrapper::delete_(cx, wrapper, id, result),
           NOTHING);
}

bool
CrossCompartmentWrapper::getPrototype(JSContext* cx, HandleObject wrapper,
                                      MutableHandleObject protop) const
{
    {
        RootedObject wrapped(cx, wrappedObject(wrapper));
        AutoCompartment call(cx, wrapped);
        if (!GetPrototype(cx, wrapped, protop))
            return false;
        if (protop) {
            if (!JSObject::setDelegate(cx, protop))
                return false;
        }
    }

    return cx->compartment()->wrap(cx, protop);
}

bool
CrossCompartmentWrapper::setPrototype(JSContext* cx, HandleObject wrapper,
                                      HandleObject proto, ObjectOpResult& result) const
{
    RootedObject protoCopy(cx, proto);
    PIERCE(cx, wrapper,
           cx->compartment()->wrap(cx, &protoCopy),
           Wrapper::setPrototype(cx, wrapper, protoCopy, result),
           NOTHING);
}

bool
CrossCompartmentWrapper::getPrototypeIfOrdinary(JSContext* cx, HandleObject wrapper,
                                                bool* isOrdinary, MutableHandleObject protop) const
{
    {
        RootedObject wrapped(cx, wrappedObject(wrapper));
        AutoCompartment call(cx, wrapped);
        if (!GetPrototypeIfOrdinary(cx, wrapped, isOrdinary, protop))
            return false;

        if (!*isOrdinary)
            return true;

        if (protop) {
            if (!JSObject::setDelegate(cx, protop))
                return false;
        }
    }

    return cx->compartment()->wrap(cx, protop);
}

bool
CrossCompartmentWrapper::setImmutablePrototype(JSContext* cx, HandleObject wrapper, bool* succeeded) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::setImmutablePrototype(cx, wrapper, succeeded),
           NOTHING);
}

bool
CrossCompartmentWrapper::preventExtensions(JSContext* cx, HandleObject wrapper,
                                           ObjectOpResult& result) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::preventExtensions(cx, wrapper, result),
           NOTHING);
}

bool
CrossCompartmentWrapper::isExtensible(JSContext* cx, HandleObject wrapper, bool* extensible) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::isExtensible(cx, wrapper, extensible),
           NOTHING);
}

bool
CrossCompartmentWrapper::has(JSContext* cx, HandleObject wrapper, HandleId id, bool* bp) const
{
    PIERCE(cx, wrapper,
           MarkAtoms(cx, id),
           Wrapper::has(cx, wrapper, id, bp),
           NOTHING);
}

bool
CrossCompartmentWrapper::hasOwn(JSContext* cx, HandleObject wrapper, HandleId id, bool* bp) const
{
    PIERCE(cx, wrapper,
           MarkAtoms(cx, id),
           Wrapper::hasOwn(cx, wrapper, id, bp),
           NOTHING);
}

static bool
WrapReceiver(JSContext* cx, HandleObject wrapper, MutableHandleValue receiver)
{
    // Usually the receiver is the wrapper and we can just unwrap it. If the
    // wrapped object is also a wrapper, things are more complicated and we
    // fall back to the slow path (it calls UncheckedUnwrap to unwrap all
    // wrappers).
    if (ObjectValue(*wrapper) == receiver) {
        JSObject* wrapped = Wrapper::wrappedObject(wrapper);
        if (!IsWrapper(wrapped)) {
            MOZ_ASSERT(wrapped->compartment() == cx->compartment());
            MOZ_ASSERT(!IsWindow(wrapped));
            receiver.setObject(*wrapped);
            return true;
        }
    }

    return cx->compartment()->wrap(cx, receiver);
}

bool
CrossCompartmentWrapper::get(JSContext* cx, HandleObject wrapper, HandleValue receiver,
                             HandleId id, MutableHandleValue vp) const
{
    RootedValue receiverCopy(cx, receiver);
    {
        AutoCompartment call(cx, wrappedObject(wrapper));
        if (!MarkAtoms(cx, id) || !WrapReceiver(cx, wrapper, &receiverCopy))
            return false;

        if (!Wrapper::get(cx, wrapper, receiverCopy, id, vp))
            return false;
    }
    return cx->compartment()->wrap(cx, vp);
}

bool
CrossCompartmentWrapper::set(JSContext* cx, HandleObject wrapper, HandleId id, HandleValue v,
                             HandleValue receiver, ObjectOpResult& result) const
{
    RootedValue valCopy(cx, v);
    RootedValue receiverCopy(cx, receiver);
    PIERCE(cx, wrapper,
           MarkAtoms(cx, id) &&
           cx->compartment()->wrap(cx, &valCopy) &&
           WrapReceiver(cx, wrapper, &receiverCopy),
           Wrapper::set(cx, wrapper, id, valCopy, receiverCopy, result),
           NOTHING);
}

bool
CrossCompartmentWrapper::getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject wrapper,
                                                      AutoIdVector& props) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::getOwnEnumerablePropertyKeys(cx, wrapper, props),
           MarkAtoms(cx, props));
}

/*
 * We can reify non-escaping iterator objects instead of having to wrap them. This
 * allows fast iteration over objects across a compartment boundary.
 */
static bool
CanReify(HandleObject obj)
{
    return obj->is<PropertyIteratorObject>();
}

struct AutoCloseIterator
{
    AutoCloseIterator(JSContext* cx, PropertyIteratorObject* obj) : obj(cx, obj) {}

    ~AutoCloseIterator() {
        if (obj)
            CloseIterator(obj);
    }

    void clear() { obj = nullptr; }

  private:
    Rooted<PropertyIteratorObject*> obj;
};

static JSObject*
Reify(JSContext* cx, JSCompartment* origin, HandleObject objp)
{
    Rooted<PropertyIteratorObject*> iterObj(cx, &objp->as<PropertyIteratorObject>());
    NativeIterator* ni = iterObj->getNativeIterator();

    RootedObject obj(cx, ni->obj);
    {
        AutoCloseIterator close(cx, iterObj);

        /* Wrap the iteratee. */
        if (!origin->wrap(cx, &obj))
            return nullptr;

        /*
         * Wrap the elements in the iterator's snapshot.
         * N.B. the order of closing/creating iterators is important due to the
         * implicit cx->enumerators state.
         */
        size_t length = ni->numKeys();
        AutoIdVector keys(cx);
        if (length > 0) {
            if (!keys.reserve(length))
                return nullptr;
            RootedId id(cx);
            RootedValue v(cx);
            for (size_t i = 0; i < length; ++i) {
                v.setString(ni->begin()[i]);
                if (!ValueToId<CanGC>(cx, v, &id))
                    return nullptr;
                cx->markId(id);
                keys.infallibleAppend(id);
            }
        }

        close.clear();
        CloseIterator(iterObj);

        obj = EnumeratedIdVectorToIterator(cx, obj, keys);
    }
    return obj;
}

JSObject*
CrossCompartmentWrapper::enumerate(JSContext* cx, HandleObject wrapper) const
{
    RootedObject res(cx);
    {
        AutoCompartment call(cx, wrappedObject(wrapper));
        res = Wrapper::enumerate(cx, wrapper);
        if (!res)
            return nullptr;
    }

    if (CanReify(res))
        return Reify(cx, cx->compartment(), res);
    if (!cx->compartment()->wrap(cx, &res))
        return nullptr;
    return res;
}

bool
CrossCompartmentWrapper::call(JSContext* cx, HandleObject wrapper, const CallArgs& args) const
{
    RootedObject wrapped(cx, wrappedObject(wrapper));

    {
        AutoCompartment call(cx, wrapped);

        args.setCallee(ObjectValue(*wrapped));
        if (!cx->compartment()->wrap(cx, args.mutableThisv()))
            return false;

        for (size_t n = 0; n < args.length(); ++n) {
            if (!cx->compartment()->wrap(cx, args[n]))
                return false;
        }

        if (!Wrapper::call(cx, wrapper, args))
            return false;
    }

    return cx->compartment()->wrap(cx, args.rval());
}

bool
CrossCompartmentWrapper::construct(JSContext* cx, HandleObject wrapper, const CallArgs& args) const
{
    RootedObject wrapped(cx, wrappedObject(wrapper));
    {
        AutoCompartment call(cx, wrapped);

        for (size_t n = 0; n < args.length(); ++n) {
            if (!cx->compartment()->wrap(cx, args[n]))
                return false;
        }
        if (!cx->compartment()->wrap(cx, args.newTarget()))
            return false;
        if (!Wrapper::construct(cx, wrapper, args))
            return false;
    }
    return cx->compartment()->wrap(cx, args.rval());
}

bool
CrossCompartmentWrapper::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                                    const CallArgs& srcArgs) const
{
    RootedObject wrapper(cx, &srcArgs.thisv().toObject());
    MOZ_ASSERT(srcArgs.thisv().isMagic(JS_IS_CONSTRUCTING) ||
               !UncheckedUnwrap(wrapper)->is<CrossCompartmentWrapperObject>());

    RootedObject wrapped(cx, wrappedObject(wrapper));
    {
        AutoCompartment call(cx, wrapped);
        InvokeArgs dstArgs(cx);
        if (!dstArgs.init(cx, srcArgs.length()))
            return false;

        Value* src = srcArgs.base();
        Value* srcend = srcArgs.array() + srcArgs.length();
        Value* dst = dstArgs.base();

        RootedValue source(cx);
        for (; src < srcend; ++src, ++dst) {
            source = *src;
            if (!cx->compartment()->wrap(cx, &source))
                return false;
            *dst = source.get();

            // Handle |this| specially. When we rewrap on the other side of the
            // membrane, we might apply a same-compartment security wrapper that
            // will stymie this whole process. If that happens, unwrap the wrapper.
            // This logic can go away when same-compartment security wrappers go away.
            if ((src == srcArgs.base() + 1) && dst->isObject()) {
                RootedObject thisObj(cx, &dst->toObject());
                if (thisObj->is<WrapperObject>() &&
                    Wrapper::wrapperHandler(thisObj)->hasSecurityPolicy())
                {
                    MOZ_ASSERT(!thisObj->is<CrossCompartmentWrapperObject>());
                    *dst = ObjectValue(*Wrapper::wrappedObject(thisObj));
                }
            }
        }

        if (!CallNonGenericMethod(cx, test, impl, dstArgs))
            return false;

        srcArgs.rval().set(dstArgs.rval());
    }
    return cx->compartment()->wrap(cx, srcArgs.rval());
}

bool
CrossCompartmentWrapper::hasInstance(JSContext* cx, HandleObject wrapper, MutableHandleValue v,
                                     bool* bp) const
{
    AutoCompartment call(cx, wrappedObject(wrapper));
    if (!cx->compartment()->wrap(cx, v))
        return false;
    return Wrapper::hasInstance(cx, wrapper, v, bp);
}

const char*
CrossCompartmentWrapper::className(JSContext* cx, HandleObject wrapper) const
{
    AutoCompartment call(cx, wrappedObject(wrapper));
    return Wrapper::className(cx, wrapper);
}

JSString*
CrossCompartmentWrapper::fun_toString(JSContext* cx, HandleObject wrapper, bool isToSource) const
{
    RootedString str(cx);
    {
        AutoCompartment call(cx, wrappedObject(wrapper));
        str = Wrapper::fun_toString(cx, wrapper, isToSource);
        if (!str)
            return nullptr;
    }
    if (!cx->compartment()->wrap(cx, &str))
        return nullptr;
    return str;
}

RegExpShared*
CrossCompartmentWrapper::regexp_toShared(JSContext* cx, HandleObject wrapper) const
{
    RootedRegExpShared re(cx);
    {
        AutoCompartment call(cx, wrappedObject(wrapper));
        re = Wrapper::regexp_toShared(cx, wrapper);
        if (!re)
            return nullptr;
    }

    // Get an equivalent RegExpShared associated with the current compartment.
    RootedAtom source(cx, re->getSource());
    cx->markAtom(source);
    return cx->zone()->regExps.get(cx, source, re->getFlags());
}

bool
CrossCompartmentWrapper::boxedValue_unbox(JSContext* cx, HandleObject wrapper, MutableHandleValue vp) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::boxedValue_unbox(cx, wrapper, vp),
           cx->compartment()->wrap(cx, vp));
}

const CrossCompartmentWrapper CrossCompartmentWrapper::singleton(0u);

bool
js::IsCrossCompartmentWrapper(JSObject* obj)
{
    return IsWrapper(obj) &&
           !!(Wrapper::wrapperHandler(obj)->flags() & Wrapper::CROSS_COMPARTMENT);
}

static void
NukeRemovedCrossCompartmentWrapper(JSContext* cx, JSObject* wrapper)
{
    MOZ_ASSERT(wrapper->is<CrossCompartmentWrapperObject>());

    NotifyGCNukeWrapper(wrapper);

    wrapper->as<ProxyObject>().nuke();

    MOZ_ASSERT(IsDeadProxyObject(wrapper));
}

JS_FRIEND_API(void)
js::NukeCrossCompartmentWrapper(JSContext* cx, JSObject* wrapper)
{
    JSCompartment* comp = wrapper->compartment();
    auto ptr = comp->lookupWrapper(Wrapper::wrappedObject(wrapper));
    if (ptr)
        comp->removeWrapper(ptr);
    NukeRemovedCrossCompartmentWrapper(cx, wrapper);
}

/*
 * NukeChromeCrossCompartmentWrappersForGlobal reaches into chrome and cuts
 * all of the cross-compartment wrappers that point to objects parented to
 * obj's global.  The snag here is that we need to avoid cutting wrappers that
 * point to the window object on page navigation (inner window destruction)
 * and only do that on tab close (outer window destruction).  Thus the
 * option of how to handle the global object.
 */
JS_FRIEND_API(bool)
js::NukeCrossCompartmentWrappers(JSContext* cx,
                                 const CompartmentFilter& sourceFilter,
                                 JSCompartment* target,
                                 js::NukeReferencesToWindow nukeReferencesToWindow,
                                 js::NukeReferencesFromTarget nukeReferencesFromTarget)
{
    CHECK_REQUEST(cx);
    JSRuntime* rt = cx->runtime();

    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        if (!sourceFilter.match(c))
            continue;

        // If the compartment matches both the source and target filter, we may
        // want to cut both incoming and outgoing wrappers.
        bool nukeAll = (nukeReferencesFromTarget == NukeAllReferences &&
                        target == c.get());

        // Iterate only the wrappers that have target compartment matched unless
        // |nukeAll| is true. The string wrappers that we're not interested in
        // won't be iterated, we can exclude them easily because they have
        // compartment nullptr. Use Maybe to avoid copying from conditionally
        // initializing NonStringWrapperEnum.
        mozilla::Maybe<JSCompartment::NonStringWrapperEnum> e;
        if (MOZ_LIKELY(!nukeAll))
            e.emplace(c, target);
        else
            e.emplace(c);
        for (; !e->empty(); e->popFront()) {
            // Skip debugger references because NukeCrossCompartmentWrapper()
            // doesn't know how to nuke them yet, see bug 1084626 for more
            // information.
            const CrossCompartmentKey& k = e->front().key();
            if (!k.is<JSObject*>())
                continue;

            AutoWrapperRooter wobj(cx, WrapperValue(*e));

            // Unwrap from the wrapped object in CrossCompartmentKey instead of
            // the wrapper, this could save us a bit of time.
            JSObject* wrapped = UncheckedUnwrap(k.as<JSObject*>());

            // We never nuke script source objects, since only ever used internally by the JS
            // engine, and are expected to remain valid throughout a scripts lifetime.
            if (MOZ_UNLIKELY(wrapped->is<ScriptSourceObject>())) {
                continue;
            }

            // We only skip nuking window references that point to a target
            // compartment, not the ones that belong to it.
            if (nukeReferencesToWindow == DontNukeWindowReferences &&
                MOZ_LIKELY(!nukeAll) && IsWindowProxy(wrapped))
            {
                continue;
            }

            // Now this is the wrapper we want to nuke.
            e->removeFront();
            NukeRemovedCrossCompartmentWrapper(cx, wobj);
        }
    }

    return true;
}

// Given a cross-compartment wrapper |wobj|, update it to point to
// |newTarget|. This recomputes the wrapper with JS_WrapValue, and thus can be
// useful even if wrapper already points to newTarget.
// This operation crashes on failure rather than leaving the heap in an
// inconsistent state.
void
js::RemapWrapper(JSContext* cx, JSObject* wobjArg, JSObject* newTargetArg)
{
    MOZ_ASSERT(!IsInsideNursery(wobjArg));
    MOZ_ASSERT(!IsInsideNursery(newTargetArg));

    RootedObject wobj(cx, wobjArg);
    RootedObject newTarget(cx, newTargetArg);
    MOZ_ASSERT(wobj->is<CrossCompartmentWrapperObject>());
    MOZ_ASSERT(!newTarget->is<CrossCompartmentWrapperObject>());
    JSObject* origTarget = Wrapper::wrappedObject(wobj);
    MOZ_ASSERT(origTarget);
    MOZ_ASSERT(!JS_IsDeadWrapper(origTarget),
               "We don't want a dead proxy in the wrapper map");
    Value origv = ObjectValue(*origTarget);
    JSCompartment* wcompartment = wobj->compartment();

    AutoDisableProxyCheck adpc;

    // If we're mapping to a different target (as opposed to just recomputing
    // for the same target), we must not have an existing wrapper for the new
    // target, otherwise this will break.
    MOZ_ASSERT_IF(origTarget != newTarget,
                  !wcompartment->lookupWrapper(ObjectValue(*newTarget)));

    // The old value should still be in the cross-compartment wrapper map, and
    // the lookup should return wobj.
    WrapperMap::Ptr p = wcompartment->lookupWrapper(origv);
    MOZ_ASSERT(&p->value().unsafeGet()->toObject() == wobj);
    wcompartment->removeWrapper(p);

    // When we remove origv from the wrapper map, its wrapper, wobj, must
    // immediately cease to be a cross-compartment wrapper. Nuke it.
    NukeCrossCompartmentWrapper(cx, wobj);

    // First, we wrap it in the new compartment. We try to use the existing
    // wrapper, |wobj|, since it's been nuked anyway. The wrap() function has
    // the choice to reuse |wobj| or not.
    RootedObject tobj(cx, newTarget);
    AutoCompartmentUnchecked ac(cx, wcompartment);
    if (!wcompartment->rewrap(cx, &tobj, wobj))
        MOZ_CRASH();

    // If wrap() reused |wobj|, it will have overwritten it and returned with
    // |tobj == wobj|. Otherwise, |tobj| will point to a new wrapper and |wobj|
    // will still be nuked. In the latter case, we replace |wobj| with the
    // contents of the new wrapper in |tobj|.
    if (tobj != wobj) {
        // Now, because we need to maintain object identity, we do a brain
        // transplant on the old object so that it contains the contents of the
        // new one.
        if (!JSObject::swap(cx, wobj, tobj))
            MOZ_CRASH();
    }

    // Before swapping, this wrapper came out of wrap(), which enforces the
    // invariant that the wrapper in the map points directly to the key.
    MOZ_ASSERT(Wrapper::wrappedObject(wobj) == newTarget);

    // Update the entry in the compartment's wrapper map to point to the old
    // wrapper, which has now been updated (via reuse or swap).
    MOZ_ASSERT(wobj->is<WrapperObject>());
    if (!wcompartment->putWrapper(cx, CrossCompartmentKey(newTarget), ObjectValue(*wobj)))
        MOZ_CRASH();
}

// Remap all cross-compartment wrappers pointing to |oldTarget| to point to
// |newTarget|. All wrappers are recomputed.
JS_FRIEND_API(bool)
js::RemapAllWrappersForObject(JSContext* cx, JSObject* oldTargetArg,
                              JSObject* newTargetArg)
{
    MOZ_ASSERT(!IsInsideNursery(oldTargetArg));
    MOZ_ASSERT(!IsInsideNursery(newTargetArg));

    RootedValue origv(cx, ObjectValue(*oldTargetArg));
    RootedObject newTarget(cx, newTargetArg);

    AutoWrapperVector toTransplant(cx);
    if (!toTransplant.reserve(cx->runtime()->numCompartments))
        return false;

    for (CompartmentsIter c(cx->runtime(), SkipAtoms); !c.done(); c.next()) {
        if (WrapperMap::Ptr wp = c->lookupWrapper(origv)) {
            // We found a wrapper. Remember and root it.
            toTransplant.infallibleAppend(WrapperValue(wp));
        }
    }

    for (const WrapperValue& v : toTransplant)
        RemapWrapper(cx, &v.toObject(), newTarget);

    return true;
}

JS_FRIEND_API(bool)
js::RecomputeWrappers(JSContext* cx, const CompartmentFilter& sourceFilter,
                      const CompartmentFilter& targetFilter)
{
    bool evictedNursery = false;

    AutoWrapperVector toRecompute(cx);
    for (CompartmentsIter c(cx->runtime(), SkipAtoms); !c.done(); c.next()) {
        // Filter by source compartment.
        if (!sourceFilter.match(c))
            continue;

        if (!evictedNursery && c->hasNurseryAllocatedWrapperEntries(targetFilter)) {
            EvictAllNurseries(cx->runtime());
            evictedNursery = true;
        }

        // Iterate over the wrappers, filtering appropriately.
        for (JSCompartment::NonStringWrapperEnum e(c, targetFilter); !e.empty(); e.popFront()) {
            // Filter out non-objects.
            CrossCompartmentKey& k = e.front().mutableKey();
            if (!k.is<JSObject*>())
                continue;

            // Add it to the list.
            if (!toRecompute.append(WrapperValue(e)))
                return false;
        }
    }

    // Recompute all the wrappers in the list.
    for (const WrapperValue& v : toRecompute) {
        JSObject* wrapper = &v.toObject();
        JSObject* wrapped = Wrapper::wrappedObject(wrapper);
        RemapWrapper(cx, wrapper, wrapped);
    }

    return true;
}
