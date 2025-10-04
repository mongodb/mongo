/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/FinalizationRegistryObject.h"
#include "gc/GC.h"
#include "gc/PublicIterators.h"
#include "js/friend/WindowProxy.h"  // js::IsWindow, js::IsWindowProxy
#include "js/Wrapper.h"
#include "proxy/DeadObjectProxy.h"
#include "proxy/DOMProxy.h"
#include "vm/Iteration.h"
#include "vm/Runtime.h"
#include "vm/WrapperObject.h"

#include "gc/Nursery-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Realm-inl.h"

using namespace js;

#define PIERCE(cx, wrapper, pre, op, post)        \
  JS_BEGIN_MACRO                                  \
    bool ok;                                      \
    {                                             \
      AutoRealm call(cx, wrappedObject(wrapper)); \
      ok = (pre) && (op);                         \
    }                                             \
    return ok && (post);                          \
  JS_END_MACRO

#define NOTHING (true)

static bool MarkAtoms(JSContext* cx, jsid id) {
  cx->markId(id);
  return true;
}

static bool MarkAtoms(JSContext* cx, HandleIdVector ids) {
  for (size_t i = 0; i < ids.length(); i++) {
    cx->markId(ids[i]);
  }
  return true;
}

bool CrossCompartmentWrapper::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject wrapper, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
  PIERCE(cx, wrapper, MarkAtoms(cx, id),
         Wrapper::getOwnPropertyDescriptor(cx, wrapper, id, desc),
         cx->compartment()->wrap(cx, desc));
}

bool CrossCompartmentWrapper::defineProperty(JSContext* cx,
                                             HandleObject wrapper, HandleId id,
                                             Handle<PropertyDescriptor> desc,
                                             ObjectOpResult& result) const {
  Rooted<PropertyDescriptor> desc2(cx, desc);
  PIERCE(cx, wrapper, MarkAtoms(cx, id) && cx->compartment()->wrap(cx, &desc2),
         Wrapper::defineProperty(cx, wrapper, id, desc2, result), NOTHING);
}

bool CrossCompartmentWrapper::ownPropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  PIERCE(cx, wrapper, NOTHING, Wrapper::ownPropertyKeys(cx, wrapper, props),
         MarkAtoms(cx, props));
}

bool CrossCompartmentWrapper::delete_(JSContext* cx, HandleObject wrapper,
                                      HandleId id,
                                      ObjectOpResult& result) const {
  PIERCE(cx, wrapper, MarkAtoms(cx, id),
         Wrapper::delete_(cx, wrapper, id, result), NOTHING);
}

bool CrossCompartmentWrapper::getPrototype(JSContext* cx, HandleObject wrapper,
                                           MutableHandleObject protop) const {
  {
    RootedObject wrapped(cx, wrappedObject(wrapper));
    AutoRealm call(cx, wrapped);
    if (!GetPrototype(cx, wrapped, protop)) {
      return false;
    }
  }

  return cx->compartment()->wrap(cx, protop);
}

bool CrossCompartmentWrapper::setPrototype(JSContext* cx, HandleObject wrapper,
                                           HandleObject proto,
                                           ObjectOpResult& result) const {
  RootedObject protoCopy(cx, proto);
  PIERCE(cx, wrapper, cx->compartment()->wrap(cx, &protoCopy),
         Wrapper::setPrototype(cx, wrapper, protoCopy, result), NOTHING);
}

bool CrossCompartmentWrapper::getPrototypeIfOrdinary(
    JSContext* cx, HandleObject wrapper, bool* isOrdinary,
    MutableHandleObject protop) const {
  {
    RootedObject wrapped(cx, wrappedObject(wrapper));
    AutoRealm call(cx, wrapped);
    if (!GetPrototypeIfOrdinary(cx, wrapped, isOrdinary, protop)) {
      return false;
    }

    if (!*isOrdinary) {
      return true;
    }
  }

  return cx->compartment()->wrap(cx, protop);
}

bool CrossCompartmentWrapper::setImmutablePrototype(JSContext* cx,
                                                    HandleObject wrapper,
                                                    bool* succeeded) const {
  PIERCE(cx, wrapper, NOTHING,
         Wrapper::setImmutablePrototype(cx, wrapper, succeeded), NOTHING);
}

bool CrossCompartmentWrapper::preventExtensions(JSContext* cx,
                                                HandleObject wrapper,
                                                ObjectOpResult& result) const {
  PIERCE(cx, wrapper, NOTHING, Wrapper::preventExtensions(cx, wrapper, result),
         NOTHING);
}

bool CrossCompartmentWrapper::isExtensible(JSContext* cx, HandleObject wrapper,
                                           bool* extensible) const {
  PIERCE(cx, wrapper, NOTHING, Wrapper::isExtensible(cx, wrapper, extensible),
         NOTHING);
}

bool CrossCompartmentWrapper::has(JSContext* cx, HandleObject wrapper,
                                  HandleId id, bool* bp) const {
  PIERCE(cx, wrapper, MarkAtoms(cx, id), Wrapper::has(cx, wrapper, id, bp),
         NOTHING);
}

bool CrossCompartmentWrapper::hasOwn(JSContext* cx, HandleObject wrapper,
                                     HandleId id, bool* bp) const {
  PIERCE(cx, wrapper, MarkAtoms(cx, id), Wrapper::hasOwn(cx, wrapper, id, bp),
         NOTHING);
}

static bool WrapReceiver(JSContext* cx, HandleObject wrapper,
                         MutableHandleValue receiver) {
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

bool CrossCompartmentWrapper::get(JSContext* cx, HandleObject wrapper,
                                  HandleValue receiver, HandleId id,
                                  MutableHandleValue vp) const {
  RootedValue receiverCopy(cx, receiver);
  {
    AutoRealm call(cx, wrappedObject(wrapper));
    if (!MarkAtoms(cx, id) || !WrapReceiver(cx, wrapper, &receiverCopy)) {
      return false;
    }

    if (!Wrapper::get(cx, wrapper, receiverCopy, id, vp)) {
      return false;
    }
  }
  return cx->compartment()->wrap(cx, vp);
}

bool CrossCompartmentWrapper::set(JSContext* cx, HandleObject wrapper,
                                  HandleId id, HandleValue v,
                                  HandleValue receiver,
                                  ObjectOpResult& result) const {
  RootedValue valCopy(cx, v);
  RootedValue receiverCopy(cx, receiver);
  PIERCE(cx, wrapper,
         MarkAtoms(cx, id) && cx->compartment()->wrap(cx, &valCopy) &&
             WrapReceiver(cx, wrapper, &receiverCopy),
         Wrapper::set(cx, wrapper, id, valCopy, receiverCopy, result), NOTHING);
}

bool CrossCompartmentWrapper::getOwnEnumerablePropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  PIERCE(cx, wrapper, NOTHING,
         Wrapper::getOwnEnumerablePropertyKeys(cx, wrapper, props),
         MarkAtoms(cx, props));
}

bool CrossCompartmentWrapper::enumerate(JSContext* cx, HandleObject wrapper,
                                        MutableHandleIdVector props) const {
  PIERCE(cx, wrapper, NOTHING, Wrapper::enumerate(cx, wrapper, props),
         MarkAtoms(cx, props));
}

bool CrossCompartmentWrapper::call(JSContext* cx, HandleObject wrapper,
                                   const CallArgs& args) const {
  RootedObject wrapped(cx, wrappedObject(wrapper));

  {
    AutoRealm call(cx, wrapped);

    args.setCallee(ObjectValue(*wrapped));
    if (!cx->compartment()->wrap(cx, args.mutableThisv())) {
      return false;
    }

    for (size_t n = 0; n < args.length(); ++n) {
      if (!cx->compartment()->wrap(cx, args[n])) {
        return false;
      }
    }

    if (!Wrapper::call(cx, wrapper, args)) {
      return false;
    }
  }

  return cx->compartment()->wrap(cx, args.rval());
}

bool CrossCompartmentWrapper::construct(JSContext* cx, HandleObject wrapper,
                                        const CallArgs& args) const {
  RootedObject wrapped(cx, wrappedObject(wrapper));
  {
    AutoRealm call(cx, wrapped);

    for (size_t n = 0; n < args.length(); ++n) {
      if (!cx->compartment()->wrap(cx, args[n])) {
        return false;
      }
    }
    if (!cx->compartment()->wrap(cx, args.newTarget())) {
      return false;
    }
    if (!Wrapper::construct(cx, wrapper, args)) {
      return false;
    }
  }
  return cx->compartment()->wrap(cx, args.rval());
}

bool CrossCompartmentWrapper::nativeCall(JSContext* cx, IsAcceptableThis test,
                                         NativeImpl impl,
                                         const CallArgs& srcArgs) const {
  RootedObject wrapper(cx, &srcArgs.thisv().toObject());
  MOZ_ASSERT(srcArgs.thisv().isMagic(JS_IS_CONSTRUCTING) ||
             !UncheckedUnwrap(wrapper)->is<CrossCompartmentWrapperObject>());

  RootedObject wrapped(cx, wrappedObject(wrapper));
  {
    AutoRealm call(cx, wrapped);
    InvokeArgs dstArgs(cx);
    if (!dstArgs.init(cx, srcArgs.length())) {
      return false;
    }

    Value* src = srcArgs.base();
    Value* srcend = srcArgs.array() + srcArgs.length();
    Value* dst = dstArgs.base();

    RootedValue source(cx);
    for (; src < srcend; ++src, ++dst) {
      source = *src;
      if (!cx->compartment()->wrap(cx, &source)) {
        return false;
      }
      *dst = source.get();

      // Handle |this| specially. When we rewrap on the other side of the
      // membrane, we might apply a same-compartment security wrapper that
      // will stymie this whole process. If that happens, unwrap the wrapper.
      // This logic can go away when same-compartment security wrappers go away.
      if ((src == srcArgs.base() + 1) && dst->isObject()) {
        RootedObject thisObj(cx, &dst->toObject());
        if (thisObj->is<WrapperObject>() &&
            Wrapper::wrapperHandler(thisObj)->hasSecurityPolicy()) {
          MOZ_ASSERT(!thisObj->is<CrossCompartmentWrapperObject>());
          *dst = ObjectValue(*Wrapper::wrappedObject(thisObj));
        }
      }
    }

    if (!CallNonGenericMethod(cx, test, impl, dstArgs)) {
      return false;
    }

    srcArgs.rval().set(dstArgs.rval());
  }
  return cx->compartment()->wrap(cx, srcArgs.rval());
}

const char* CrossCompartmentWrapper::className(JSContext* cx,
                                               HandleObject wrapper) const {
  AutoRealm call(cx, wrappedObject(wrapper));
  return Wrapper::className(cx, wrapper);
}

JSString* CrossCompartmentWrapper::fun_toString(JSContext* cx,
                                                HandleObject wrapper,
                                                bool isToSource) const {
  RootedString str(cx);
  {
    AutoRealm call(cx, wrappedObject(wrapper));
    str = Wrapper::fun_toString(cx, wrapper, isToSource);
    if (!str) {
      return nullptr;
    }
  }
  if (!cx->compartment()->wrap(cx, &str)) {
    return nullptr;
  }
  return str;
}

RegExpShared* CrossCompartmentWrapper::regexp_toShared(
    JSContext* cx, HandleObject wrapper) const {
  RootedRegExpShared re(cx);
  {
    AutoRealm call(cx, wrappedObject(wrapper));
    re = Wrapper::regexp_toShared(cx, wrapper);
    if (!re) {
      return nullptr;
    }
  }

  // Get an equivalent RegExpShared associated with the current compartment.
  Rooted<JSAtom*> source(cx, re->getSource());
  cx->markAtom(source);
  return cx->zone()->regExps().get(cx, source, re->getFlags());
}

bool CrossCompartmentWrapper::boxedValue_unbox(JSContext* cx,
                                               HandleObject wrapper,
                                               MutableHandleValue vp) const {
  PIERCE(cx, wrapper, NOTHING, Wrapper::boxedValue_unbox(cx, wrapper, vp),
         cx->compartment()->wrap(cx, vp));
}

const CrossCompartmentWrapper CrossCompartmentWrapper::singleton(0u);

JS_PUBLIC_API void js::NukeCrossCompartmentWrapper(JSContext* cx,
                                                   JSObject* wrapper) {
  JS::Compartment* comp = wrapper->compartment();
  auto ptr = comp->lookupWrapper(Wrapper::wrappedObject(wrapper));
  if (ptr) {
    comp->removeWrapper(ptr);
  }
  NukeRemovedCrossCompartmentWrapper(cx, wrapper);
}

JS_PUBLIC_API void js::NukeCrossCompartmentWrapperIfExists(
    JSContext* cx, JS::Compartment* source, JSObject* target) {
  MOZ_ASSERT(source != target->compartment());
  MOZ_ASSERT(!target->is<CrossCompartmentWrapperObject>());
  auto ptr = source->lookupWrapper(target);
  if (ptr) {
    JSObject* wrapper = ptr->value().get();
    NukeCrossCompartmentWrapper(cx, wrapper);
  }
}

// Returns true iff all realms in the compartment have been nuked.
static bool NukedAllRealms(JS::Compartment* comp) {
  for (RealmsInCompartmentIter realm(comp); !realm.done(); realm.next()) {
    if (!realm->nukedIncomingWrappers) {
      return false;
    }
  }
  return true;
}

/*
 * NukeCrossCompartmentWrappers invalidates all cross-compartment wrappers
 * that point to objects in the |target| realm.
 *
 * There is some complexity in targeting to preserve semantics which requires
 * the filtering and behavioural options:
 *
 * - |sourceFilter| limits the compartments searched for source pointers
 * - |nukeReferencesToWindow| will, if set to DontNukeWindowReferences skip
 *   wrappers whose target is the window proxy of the target realm.
 * - |nukeReferencesFromTarget| will, when set to NukeAllReferences, disallow
 *   the creation of new wrappers to the target realm. This option can also
 *   allow more wrappers to be cleaned up transitively.
 */
JS_PUBLIC_API bool js::NukeCrossCompartmentWrappers(
    JSContext* cx, const CompartmentFilter& sourceFilter, JS::Realm* target,
    js::NukeReferencesToWindow nukeReferencesToWindow,
    js::NukeReferencesFromTarget nukeReferencesFromTarget) {
  CHECK_THREAD(cx);
  JSRuntime* rt = cx->runtime();

  // If we're nuking all wrappers into the target realm, prevent us from
  // creating new wrappers for it in the future.
  if (nukeReferencesFromTarget == NukeAllReferences) {
    target->nukedIncomingWrappers = true;
  }

  for (CompartmentsIter c(rt); !c.done(); c.next()) {
    if (!sourceFilter.match(c)) {
      continue;
    }

    // If the realm matches both the source and target filter, we may want to
    // cut outgoing wrappers too, if we nuked all realms in the compartment.
    bool nukeAll =
        (nukeReferencesFromTarget == NukeAllReferences &&
         target->compartment() == c.get() && NukedAllRealms(c.get()));

    // Iterate only the wrappers that have target compartment matched unless
    // |nukeAll| is true. Use Maybe to avoid copying from conditionally
    // initializing ObjectWrapperEnum.
    mozilla::Maybe<Compartment::ObjectWrapperEnum> e;
    if (MOZ_LIKELY(!nukeAll)) {
      e.emplace(c, target->compartment());
    } else {
      e.emplace(c);
      c.get()->nukedOutgoingWrappers = true;
    }
    for (; !e->empty(); e->popFront()) {
      JSObject* key = e->front().key();

      AutoWrapperRooter wobj(cx, WrapperValue(*e));

      // Unwrap from the wrapped object in key instead of the wrapper, this
      // could save us a bit of time.
      JSObject* wrapped = UncheckedUnwrap(key);

      // Don't nuke wrappers for objects in other realms in the target
      // compartment unless nukeAll is set because in that case we want to nuke
      // all outgoing wrappers for the current compartment.
      if (!nukeAll && wrapped->nonCCWRealm() != target) {
        continue;
      }

      // We only skip nuking window references that point to a target
      // compartment, not the ones that belong to it.
      if (nukeReferencesToWindow == DontNukeWindowReferences &&
          MOZ_LIKELY(!nukeAll) && IsWindowProxy(wrapped)) {
        continue;
      }

      // Now this is the wrapper we want to nuke.
      e->removeFront();
      NukeRemovedCrossCompartmentWrapper(cx, wobj);
    }
  }

  return true;
}

JS_PUBLIC_API bool js::AllowNewWrapper(JS::Compartment* target, JSObject* obj) {
  // Disallow creating new wrappers if we nuked the object realm or target
  // compartment.

  MOZ_ASSERT(obj->compartment() != target);

  if (target->nukedOutgoingWrappers ||
      obj->nonCCWRealm()->nukedIncomingWrappers) {
    return false;
  }

  return true;
}

JS_PUBLIC_API bool js::NukedObjectRealm(JSObject* obj) {
  return obj->nonCCWRealm()->nukedIncomingWrappers;
}

// Given a cross-compartment wrapper |wobj|, update it to point to
// |newTarget|. This recomputes the wrapper with JS_WrapValue, and thus can be
// useful even if wrapper already points to newTarget.
// This operation crashes on failure rather than leaving the heap in an
// inconsistent state.
void js::RemapWrapper(JSContext* cx, JSObject* wobjArg,
                      JSObject* newTargetArg) {
  RootedObject wobj(cx, wobjArg);
  RootedObject newTarget(cx, newTargetArg);
  MOZ_ASSERT(wobj->is<CrossCompartmentWrapperObject>());
  MOZ_ASSERT(!newTarget->is<CrossCompartmentWrapperObject>());
  JSObject* origTarget = Wrapper::wrappedObject(wobj);
  MOZ_ASSERT(origTarget);
  JS::Compartment* wcompartment = wobj->compartment();
  MOZ_ASSERT(wcompartment != newTarget->compartment());

  AutoDisableProxyCheck adpc;

  // If we're mapping to a different target (as opposed to just recomputing
  // for the same target), we must not have an existing wrapper for the new
  // target, otherwise this will break.
  MOZ_ASSERT_IF(origTarget != newTarget,
                !wcompartment->lookupWrapper(newTarget));

  // The old value should still be in the cross-compartment wrapper map, and
  // the lookup should return wobj.
  ObjectWrapperMap::Ptr p = wcompartment->lookupWrapper(origTarget);
  MOZ_ASSERT(*p->value().unsafeGet() == wobj);
  wcompartment->removeWrapper(p);

  // When we remove origv from the wrapper map, its wrapper, wobj, must
  // immediately cease to be a cross-compartment wrapper. Nuke it.
  NukeCrossCompartmentWrapper(cx, wobj);

  // If the target is a dead wrapper, and we're just fixing wrappers for
  // it, then we're done now that the CCW is a dead wrapper.
  if (JS_IsDeadWrapper(origTarget)) {
    MOZ_RELEASE_ASSERT(origTarget == newTarget);
    return;
  }

  js::RemapDeadWrapper(cx, wobj, newTarget);
}

// Given a dead proxy object |wobj|, turn it into a cross-compartment wrapper
// pointing at |newTarget|.
// This operation crashes on failure rather than leaving the heap in an
// inconsistent state.
void js::RemapDeadWrapper(JSContext* cx, HandleObject wobj,
                          HandleObject newTarget) {
  MOZ_ASSERT(IsDeadProxyObject(wobj));
  MOZ_ASSERT(!newTarget->is<CrossCompartmentWrapperObject>());

  // These are not exposed. Doing this would require updating the
  // FinalizationObservers data structures.
  MOZ_ASSERT(!newTarget->is<FinalizationRecordObject>());

  AutoDisableProxyCheck adpc;

  // wobj is not a cross-compartment wrapper, so we can use nonCCWRealm.
  Realm* wrealm = wobj->nonCCWRealm();

  // First, we wrap it in the new compartment. We try to use the existing
  // wrapper, |wobj|, since it's been nuked anyway. The rewrap() function has
  // the choice to reuse |wobj| or not.
  RootedObject tobj(cx, newTarget);
  AutoRealmUnchecked ar(cx, wrealm);
  AutoEnterOOMUnsafeRegion oomUnsafe;
  JS::Compartment* wcompartment = wobj->compartment();
  if (!wcompartment->rewrap(cx, &tobj, wobj)) {
    oomUnsafe.crash("js::RemapWrapper");
  }

  // If rewrap() reused |wobj|, it will have overwritten it and returned with
  // |tobj == wobj|. Otherwise, |tobj| will point to a new wrapper and |wobj|
  // will still be nuked. In the latter case, we replace |wobj| with the
  // contents of the new wrapper in |tobj|.
  if (tobj != wobj) {
    // Now, because we need to maintain object identity, we do a brain
    // transplant on the old object so that it contains the contents of the
    // new one.
    JSObject::swap(cx, wobj, tobj, oomUnsafe);
  }

  if (!wobj->is<WrapperObject>()) {
    MOZ_ASSERT(js::IsDOMRemoteProxyObject(wobj) || IsDeadProxyObject(wobj));
    return;
  }

  // Before swapping, this wrapper came out of rewrap(), which enforces the
  // invariant that the wrapper in the map points directly to the key.
  MOZ_ASSERT(Wrapper::wrappedObject(wobj) == newTarget);

  // Update the entry in the compartment's wrapper map to point to the old
  // wrapper, which has now been updated (via reuse or swap).
  if (!wcompartment->putWrapper(cx, newTarget, wobj)) {
    oomUnsafe.crash("js::RemapWrapper");
  }
}

// Remap all cross-compartment wrappers pointing to |oldTarget| to point to
// |newTarget|. All wrappers are recomputed.
JS_PUBLIC_API bool js::RemapAllWrappersForObject(JSContext* cx,
                                                 HandleObject oldTarget,
                                                 HandleObject newTarget) {
  AutoWrapperVector toTransplant(cx);

  for (CompartmentsIter c(cx->runtime()); !c.done(); c.next()) {
    if (ObjectWrapperMap::Ptr wp = c->lookupWrapper(oldTarget)) {
      // We found a wrapper. Remember and root it.
      if (!toTransplant.append(WrapperValue(wp))) {
        return false;
      }
    }
  }

  for (const WrapperValue& v : toTransplant) {
    RemapWrapper(cx, v, newTarget);
  }

  return true;
}

JS_PUBLIC_API bool js::RecomputeWrappers(
    JSContext* cx, const CompartmentFilter& sourceFilter,
    const CompartmentFilter& targetFilter) {
  bool evictedNursery = false;

  AutoWrapperVector toRecompute(cx);
  for (CompartmentsIter c(cx->runtime()); !c.done(); c.next()) {
    // Filter by source compartment.
    if (!sourceFilter.match(c)) {
      continue;
    }

    if (!evictedNursery &&
        c->hasNurseryAllocatedObjectWrapperEntries(targetFilter)) {
      cx->runtime()->gc.evictNursery();
      evictedNursery = true;
    }

    // Iterate over object wrappers, filtering appropriately.
    for (Compartment::ObjectWrapperEnum e(c, targetFilter); !e.empty();
         e.popFront()) {
      // Don't remap wrappers to finalization record objects. These are used
      // internally and are not exposed.
      JSObject* wrapper = *e.front().value().unsafeGet();
      if (Wrapper::wrappedObject(wrapper)->is<FinalizationRecordObject>()) {
        continue;
      }

      // Add the wrapper to the list.
      if (!toRecompute.append(WrapperValue(e))) {
        return false;
      }
    }
  }

  // Recompute all the wrappers in the list.
  for (const WrapperValue& wrapper : toRecompute) {
    JSObject* wrapped = Wrapper::wrappedObject(wrapper);
    RemapWrapper(cx, wrapper, wrapped);
  }

  return true;
}
