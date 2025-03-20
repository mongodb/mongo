/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/WeakRefObject.h"

#include "jsapi.h"

#include "gc/FinalizationObservers.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"

#include "gc/PrivateIterators-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

namespace js {

/* static */
bool WeakRefObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // https://tc39.es/proposal-weakrefs/#sec-weak-ref-constructor
  // The WeakRef constructor is not intended to be called as a function and will
  // throw an exception when called in that manner.
  if (!ThrowIfNotConstructing(cx, args, "WeakRef")) {
    return false;
  }

  // https://tc39.es/proposal-weakrefs/#sec-weak-ref-target
  // 1. If NewTarget is undefined, throw a TypeError exception.
  // 2. If Type(target) is not Object, throw a TypeError exception.
  if (!args.get(0).isObject()) {
    ReportNotObject(cx, args.get(0));
    return false;
  }

  // 3. Let weakRef be ? OrdinaryCreateFromConstructor(NewTarget,
  //    "%WeakRefPrototype%", « [[Target]] »).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_WeakRef, &proto)) {
    return false;
  }

  Rooted<WeakRefObject*> weakRef(
      cx, NewObjectWithClassProto<WeakRefObject>(cx, proto));
  if (!weakRef) {
    return false;
  }

  RootedObject target(cx);
  target = CheckedUnwrapDynamic(&args[0].toObject(), cx);
  if (!target) {
    ReportAccessDenied(cx);
    return false;
  }

  // If the target is a DOM wrapper, preserve it.
  if (!preserveDOMWrapper(cx, target)) {
    return false;
  }

  // Wrap the weakRef into the target's Zone. This is a cross-compartment
  // wrapper if the Zone is different, or same-compartment (the original
  // object) if the Zone is the same *even if* the compartments are different.
  RootedObject wrappedWeakRef(cx, weakRef);
  bool sameZone = target->zone() == weakRef->zone();
  AutoRealm ar(cx, sameZone ? weakRef : target);
  if (!JS_WrapObject(cx, &wrappedWeakRef)) {
    return false;
  }

  if (JS_IsDeadWrapper(wrappedWeakRef)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
  }

  // 4. Perform AddToKeptObjects(target).
  if (!target->zone()->addToKeptObjects(target)) {
    ReportOutOfMemory(cx);
    return false;
  };

  // Add an entry to the per-zone maps from target JS object to a list of weak
  // ref objects.
  gc::GCRuntime* gc = &cx->runtime()->gc;
  if (!gc->registerWeakRef(target, wrappedWeakRef)) {
    ReportOutOfMemory(cx);
    return false;
  };

  // 5. Set weakRef.[[Target]] to target.
  weakRef->setReservedSlotGCThingAsPrivate(TargetSlot, target);

  // 6. Return weakRef.
  args.rval().setObject(*weakRef);
  return true;
}

/* static */
bool WeakRefObject::preserveDOMWrapper(JSContext* cx, HandleObject obj) {
  if (!MaybePreserveDOMWrapper(cx, obj)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_WEAKREF_TARGET);
    return false;
  }

  return true;
}

/* static */
void WeakRefObject::trace(JSTracer* trc, JSObject* obj) {
  WeakRefObject* weakRef = &obj->as<WeakRefObject>();

  if (trc->traceWeakEdges()) {
    JSObject* target = weakRef->target();
    if (target) {
      TraceManuallyBarrieredEdge(trc, &target, "WeakRefObject::target");
      weakRef->setTargetUnbarriered(target);
    }
  }
}

/* static */
void WeakRefObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  // The target is cleared when the target's zone is swept and that always
  // happens before this object is finalized because of the CCW from the target
  // zone to this object. If the CCW is nuked, the target is cleared in
  // NotifyGCNukeWrapper().
  MOZ_ASSERT(!obj->as<WeakRefObject>().target());
}

const JSClassOps WeakRefObject::classOps_ = {
    nullptr,   // addProperty
    nullptr,   // delProperty
    nullptr,   // enumerate
    nullptr,   // newEnumerate
    nullptr,   // resolve
    nullptr,   // mayResolve
    finalize,  // finalize
    nullptr,   // call
    nullptr,   // construct
    trace,     // trace
};

const ClassSpec WeakRefObject::classSpec_ = {
    GenericCreateConstructor<WeakRefObject::construct, 1,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<WeakRefObject>,
    nullptr,
    nullptr,
    WeakRefObject::methods,
    WeakRefObject::properties,
};

const JSClass WeakRefObject::class_ = {
    "WeakRef",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_WeakRef) | JSCLASS_FOREGROUND_FINALIZE,
    &classOps_, &classSpec_};

const JSClass WeakRefObject::protoClass_ = {
    // https://tc39.es/proposal-weakrefs/#sec-weak-ref.prototype
    // https://tc39.es/proposal-weakrefs/#sec-properties-of-the-weak-ref-prototype-object
    "WeakRef.prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_WeakRef),
    JS_NULL_CLASS_OPS, &classSpec_};

const JSPropertySpec WeakRefObject::properties[] = {
    JS_STRING_SYM_PS(toStringTag, "WeakRef", JSPROP_READONLY), JS_PS_END};

const JSFunctionSpec WeakRefObject::methods[] = {JS_FN("deref", deref, 0, 0),
                                                 JS_FS_END};

/* static */
bool WeakRefObject::deref(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // https://tc39.es/proposal-weakrefs/#sec-weak-ref.prototype.deref
  // 1. Let weakRef be the this value.
  // 2. If Type(weakRef) is not Object, throw a TypeError exception.
  // 3. If weakRef does not have a [[Target]] internal slot, throw a TypeError
  //    exception.
  if (!args.thisv().isObject() ||
      !args.thisv().toObject().is<WeakRefObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_A_WEAK_REF,
                              "Receiver of WeakRef.deref call");
    return false;
  }

  Rooted<WeakRefObject*> weakRef(cx,
                                 &args.thisv().toObject().as<WeakRefObject>());

  // We need to perform a read barrier, which may clear the target.
  readBarrier(cx, weakRef);

  // 4. Let target be the value of weakRef.[[Target]].
  // 5. If target is not empty,
  //    a. Perform AddToKeptObjects(target).
  //    b. Return target.
  // 6. Return undefined.
  if (!weakRef->target()) {
    args.rval().setUndefined();
    return true;
  }

  RootedObject target(cx, weakRef->target());
  if (!target->zone()->addToKeptObjects(target)) {
    return false;
  }

  // Target should be wrapped into the current realm before returning it.
  RootedObject wrappedTarget(cx, target);
  if (!JS_WrapObject(cx, &wrappedTarget)) {
    return false;
  }

  args.rval().setObject(*wrappedTarget);
  return true;
}

void WeakRefObject::setTargetUnbarriered(JSObject* target) {
  setReservedSlotGCThingAsPrivateUnbarriered(TargetSlot, target);
}

void WeakRefObject::clearTarget() {
  clearReservedSlotGCThingAsPrivate(TargetSlot);
}

/* static */
void WeakRefObject::readBarrier(JSContext* cx, Handle<WeakRefObject*> self) {
  RootedObject obj(cx, self->target());
  if (!obj) {
    return;
  }

  if (obj->getClass()->isDOMClass()) {
    // We preserved the target when the WeakRef was created. If it has since
    // been released then the DOM object it wraps has been collected, so clear
    // the target.
    MOZ_ASSERT(cx->runtime()->hasReleasedWrapperCallback);
    bool wasReleased = cx->runtime()->hasReleasedWrapperCallback(obj);
    if (wasReleased) {
      obj->zone()->finalizationObservers()->removeWeakRefTarget(obj, self);
      return;
    }
  }

  gc::ReadBarrier(obj.get());
}

namespace gc {

void GCRuntime::traceKeptObjects(JSTracer* trc) {
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    zone->traceKeptObjects(trc);
  }
}

}  // namespace gc

}  // namespace js
