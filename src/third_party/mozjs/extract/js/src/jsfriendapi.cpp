/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsfriendapi.h"

#include "mozilla/Atomics.h"
#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TimeStamp.h"

#include <stdint.h>

#include "builtin/BigInt.h"
#include "builtin/MapObject.h"
#include "builtin/TestingFunctions.h"
#include "gc/GC.h"
#include "gc/PublicIterators.h"
#include "gc/WeakMap.h"
#include "js/CharacterEncoding.h"
#include "js/experimental/CodeCoverage.h"
#include "js/experimental/CTypes.h"  // JS::AutoCTypesActivityCallback, JS::SetCTypesActivityCallback
#include "js/experimental/Intl.h"  // JS::AddMoz{DateTimeFormat,DisplayNames}Constructor
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // JS_STACK_GROWTH_DIRECTION
#include "js/friend/WindowProxy.h"    // js::ToWindowIfWindowProxy
#include "js/Object.h"                // JS::GetClass
#include "js/Printf.h"
#include "js/Proxy.h"
#include "js/shadow/Object.h"  // JS::shadow::Object
#include "js/String.h"         // JS::detail::StringToLinearStringSlow
#include "js/Wrapper.h"
#include "proxy/DeadObjectProxy.h"
#include "util/Poison.h"
#include "vm/ArgumentsObject.h"
#include "vm/BooleanObject.h"
#include "vm/DateObject.h"
#include "vm/ErrorObject.h"
#include "vm/FrameIter.h"  // js::FrameIter
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/NumberObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Printer.h"
#include "vm/PromiseObject.h"  // js::PromiseObject
#include "vm/Realm.h"
#include "vm/StringObject.h"
#include "vm/Time.h"
#include "vm/WrapperObject.h"

#include "gc/Nursery-inl.h"
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::PodArrayZero;

JS::RootingContext::RootingContext() : realm_(nullptr), zone_(nullptr) {
  for (auto& listHead : stackRoots_) {
    listHead = nullptr;
  }
  for (auto& listHead : autoGCRooters_) {
    listHead = nullptr;
  }

  PodArrayZero(nativeStackLimit);
#if JS_STACK_GROWTH_DIRECTION > 0
  for (int i = 0; i < StackKindCount; i++) {
    nativeStackLimit[i] = UINTPTR_MAX;
  }
#endif
}

JS_PUBLIC_API void JS_SetGrayGCRootsTracer(JSContext* cx, JSTraceDataOp traceOp,
                                           void* data) {
  cx->runtime()->gc.setGrayRootsTracer(traceOp, data);
}

JS_PUBLIC_API JSObject* JS_FindCompilationScope(JSContext* cx,
                                                HandleObject objArg) {
  cx->check(objArg);

  RootedObject obj(cx, objArg);

  /*
   * We unwrap wrappers here. This is a little weird, but it's what's being
   * asked of us.
   */
  if (obj->is<WrapperObject>()) {
    obj = UncheckedUnwrap(obj);
  }

  /*
   * Get the Window if `obj` is a WindowProxy so that we compile in the
   * correct (global) scope.
   */
  return ToWindowIfWindowProxy(obj);
}

JS_PUBLIC_API JSFunction* JS_GetObjectFunction(JSObject* obj) {
  if (obj->is<JSFunction>()) {
    return &obj->as<JSFunction>();
  }
  return nullptr;
}

JS_PUBLIC_API JSObject* JS_NewObjectWithoutMetadata(
    JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> proto) {
  cx->check(proto);
  AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);
  return JS_NewObjectWithGivenProto(cx, clasp, proto);
}

JS_PUBLIC_API bool JS::GetIsSecureContext(JS::Realm* realm) {
  return realm->creationOptions().secureContext();
}

JS_PUBLIC_API JSPrincipals* JS::GetRealmPrincipals(JS::Realm* realm) {
  return realm->principals();
}

JS_PUBLIC_API void JS::SetRealmPrincipals(JS::Realm* realm,
                                          JSPrincipals* principals) {
  // Short circuit if there's no change.
  if (principals == realm->principals()) {
    return;
  }

  // We'd like to assert that our new principals is always same-origin
  // with the old one, but JSPrincipals doesn't give us a way to do that.
  // But we can at least assert that we're not switching between system
  // and non-system.
  const JSPrincipals* trusted =
      realm->runtimeFromMainThread()->trustedPrincipals();
  bool isSystem = principals && principals == trusted;
  MOZ_RELEASE_ASSERT(realm->isSystem() == isSystem);

  // Clear out the old principals, if any.
  if (realm->principals()) {
    JS_DropPrincipals(TlsContext.get(), realm->principals());
    realm->setPrincipals(nullptr);
  }

  // Set up the new principals.
  if (principals) {
    JS_HoldPrincipals(principals);
    realm->setPrincipals(principals);
  }
}

JS_PUBLIC_API JSPrincipals* JS_GetScriptPrincipals(JSScript* script) {
  return script->principals();
}

JS_PUBLIC_API bool JS_ScriptHasMutedErrors(JSScript* script) {
  return script->mutedErrors();
}

JS_PUBLIC_API bool JS_WrapPropertyDescriptor(
    JSContext* cx, JS::MutableHandle<JS::PropertyDescriptor> desc) {
  return cx->compartment()->wrap(cx, desc);
}

JS_PUBLIC_API bool JS_WrapPropertyDescriptor(
    JSContext* cx,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) {
  return cx->compartment()->wrap(cx, desc);
}

JS_PUBLIC_API void JS_TraceShapeCycleCollectorChildren(JS::CallbackTracer* trc,
                                                       JS::GCCellPtr shape) {
  MOZ_ASSERT(shape.is<Shape>());
  TraceCycleCollectorChildren(trc, &shape.as<Shape>());
}

static bool DefineHelpProperty(JSContext* cx, HandleObject obj,
                               const char* prop, const char* value) {
  RootedAtom atom(cx, Atomize(cx, value, strlen(value)));
  if (!atom) {
    return false;
  }
  return JS_DefineProperty(cx, obj, prop, atom,
                           JSPROP_READONLY | JSPROP_PERMANENT);
}

JS_PUBLIC_API bool JS_DefineFunctionsWithHelp(
    JSContext* cx, HandleObject obj, const JSFunctionSpecWithHelp* fs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  CHECK_THREAD(cx);
  cx->check(obj);
  for (; fs->name; fs++) {
    JSAtom* atom = Atomize(cx, fs->name, strlen(fs->name));
    if (!atom) {
      return false;
    }

    Rooted<jsid> id(cx, AtomToId(atom));
    RootedFunction fun(cx, DefineFunction(cx, obj, id, fs->call, fs->nargs,
                                          fs->flags | JSPROP_RESOLVING));
    if (!fun) {
      return false;
    }

    if (fs->jitInfo) {
      fun->setJitInfo(fs->jitInfo);
    }

    if (fs->usage) {
      if (!DefineHelpProperty(cx, fun, "usage", fs->usage)) {
        return false;
      }
    }

    if (fs->help) {
      if (!DefineHelpProperty(cx, fun, "help", fs->help)) {
        return false;
      }
    }
  }

  return true;
}

JS_PUBLIC_API bool JS::GetBuiltinClass(JSContext* cx, HandleObject obj,
                                       js::ESClass* cls) {
  if (MOZ_UNLIKELY(obj->is<ProxyObject>())) {
    return Proxy::getBuiltinClass(cx, obj, cls);
  }

  if (obj->is<PlainObject>()) {
    *cls = ESClass::Object;
  } else if (obj->is<ArrayObject>()) {
    *cls = ESClass::Array;
  } else if (obj->is<NumberObject>()) {
    *cls = ESClass::Number;
  } else if (obj->is<StringObject>()) {
    *cls = ESClass::String;
  } else if (obj->is<BooleanObject>()) {
    *cls = ESClass::Boolean;
  } else if (obj->is<RegExpObject>()) {
    *cls = ESClass::RegExp;
  } else if (obj->is<ArrayBufferObject>()) {
    *cls = ESClass::ArrayBuffer;
  } else if (obj->is<SharedArrayBufferObject>()) {
    *cls = ESClass::SharedArrayBuffer;
  } else if (obj->is<DateObject>()) {
    *cls = ESClass::Date;
  } else if (obj->is<SetObject>()) {
    *cls = ESClass::Set;
  } else if (obj->is<MapObject>()) {
    *cls = ESClass::Map;
  } else if (obj->is<PromiseObject>()) {
    *cls = ESClass::Promise;
  } else if (obj->is<MapIteratorObject>()) {
    *cls = ESClass::MapIterator;
  } else if (obj->is<SetIteratorObject>()) {
    *cls = ESClass::SetIterator;
  } else if (obj->is<ArgumentsObject>()) {
    *cls = ESClass::Arguments;
  } else if (obj->is<ErrorObject>()) {
    *cls = ESClass::Error;
  } else if (obj->is<BigIntObject>()) {
    *cls = ESClass::BigInt;
  } else if (obj->is<JSFunction>()) {
    *cls = ESClass::Function;
  } else {
    *cls = ESClass::Other;
  }

  return true;
}

JS_PUBLIC_API bool js::IsArgumentsObject(HandleObject obj) {
  return obj->is<ArgumentsObject>();
}

JS_PUBLIC_API JS::Zone* js::GetRealmZone(JS::Realm* realm) {
  return realm->zone();
}

JS_PUBLIC_API bool js::IsSystemCompartment(JS::Compartment* comp) {
  // Realms in the same compartment must either all be system realms or
  // non-system realms. We assert this in NewRealm and SetRealmPrincipals,
  // but do an extra sanity check here.
  MOZ_ASSERT(comp->realms()[0]->isSystem() ==
             comp->realms().back()->isSystem());
  return comp->realms()[0]->isSystem();
}

JS_PUBLIC_API bool js::IsSystemRealm(JS::Realm* realm) {
  return realm->isSystem();
}

JS_PUBLIC_API bool js::IsSystemZone(Zone* zone) { return zone->isSystemZone(); }

JS_PUBLIC_API bool js::IsFunctionObject(JSObject* obj) {
  return obj->is<JSFunction>();
}

JS_PUBLIC_API bool js::IsSavedFrame(JSObject* obj) {
  return obj->is<SavedFrame>();
}

JS_PUBLIC_API bool js::UninlinedIsCrossCompartmentWrapper(const JSObject* obj) {
  return js::IsCrossCompartmentWrapper(obj);
}

JS_PUBLIC_API void js::AssertSameCompartment(JSContext* cx, JSObject* obj) {
  cx->check(obj);
}

JS_PUBLIC_API void js::AssertSameCompartment(JSContext* cx, JS::HandleValue v) {
  cx->check(v);
}

#ifdef DEBUG
JS_PUBLIC_API void js::AssertSameCompartment(JSObject* objA, JSObject* objB) {
  MOZ_ASSERT(objA->compartment() == objB->compartment());
}
#endif

JS_PUBLIC_API void js::NotifyAnimationActivity(JSObject* obj) {
  MOZ_ASSERT(obj->is<GlobalObject>());

  auto timeNow = mozilla::TimeStamp::Now();
  obj->as<GlobalObject>().realm()->lastAnimationTime = timeNow;
  obj->runtimeFromMainThread()->lastAnimationTime = timeNow;
}

JS_PUBLIC_API bool js::IsObjectInContextCompartment(JSObject* obj,
                                                    const JSContext* cx) {
  return obj->compartment() == cx->compartment();
}

JS_PUBLIC_API bool js::AutoCheckRecursionLimit::runningWithTrustedPrincipals(
    JSContext* cx) const {
  return cx->runningWithTrustedPrincipals();
}

JS_PUBLIC_API JSFunction* js::DefineFunctionWithReserved(
    JSContext* cx, JSObject* objArg, const char* name, JSNative call,
    unsigned nargs, unsigned attrs) {
  RootedObject obj(cx, objArg);
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  CHECK_THREAD(cx);
  cx->check(obj);
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return nullptr;
  }
  Rooted<jsid> id(cx, AtomToId(atom));
  return DefineFunction(cx, obj, id, call, nargs, attrs,
                        gc::AllocKind::FUNCTION_EXTENDED);
}

JS_PUBLIC_API JSFunction* js::NewFunctionWithReserved(JSContext* cx,
                                                      JSNative native,
                                                      unsigned nargs,
                                                      unsigned flags,
                                                      const char* name) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  CHECK_THREAD(cx);

  RootedAtom atom(cx);
  if (name) {
    atom = Atomize(cx, name, strlen(name));
    if (!atom) {
      return nullptr;
    }
  }

  return (flags & JSFUN_CONSTRUCTOR)
             ? NewNativeConstructor(cx, native, nargs, atom,
                                    gc::AllocKind::FUNCTION_EXTENDED)
             : NewNativeFunction(cx, native, nargs, atom,
                                 gc::AllocKind::FUNCTION_EXTENDED);
}

JS_PUBLIC_API JSFunction* js::NewFunctionByIdWithReserved(
    JSContext* cx, JSNative native, unsigned nargs, unsigned flags, jsid id) {
  MOZ_ASSERT(id.isAtom());
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  CHECK_THREAD(cx);
  cx->check(id);

  RootedAtom atom(cx, id.toAtom());
  return (flags & JSFUN_CONSTRUCTOR)
             ? NewNativeConstructor(cx, native, nargs, atom,
                                    gc::AllocKind::FUNCTION_EXTENDED)
             : NewNativeFunction(cx, native, nargs, atom,
                                 gc::AllocKind::FUNCTION_EXTENDED);
}

JS_PUBLIC_API const Value& js::GetFunctionNativeReserved(JSObject* fun,
                                                         size_t which) {
  MOZ_ASSERT(fun->as<JSFunction>().isNativeFun());
  return fun->as<JSFunction>().getExtendedSlot(which);
}

JS_PUBLIC_API void js::SetFunctionNativeReserved(JSObject* fun, size_t which,
                                                 const Value& val) {
  MOZ_ASSERT(fun->as<JSFunction>().isNativeFun());
  MOZ_ASSERT_IF(val.isObject(),
                val.toObject().compartment() == fun->compartment());
  fun->as<JSFunction>().setExtendedSlot(which, val);
}

JS_PUBLIC_API bool js::FunctionHasNativeReserved(JSObject* fun) {
  MOZ_ASSERT(fun->as<JSFunction>().isNativeFun());
  return fun->as<JSFunction>().isExtended();
}

bool js::GetObjectProto(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::MutableHandle<JSObject*> proto) {
  cx->check(obj);

  if (obj->is<ProxyObject>()) {
    return JS_GetPrototype(cx, obj, proto);
  }

  proto.set(obj->staticPrototype());
  return true;
}

JS_PUBLIC_API JSObject* js::GetStaticPrototype(JSObject* obj) {
  MOZ_ASSERT(obj->hasStaticPrototype());
  return obj->staticPrototype();
}

JS_PUBLIC_API bool js::GetRealmOriginalEval(JSContext* cx,
                                            MutableHandleObject eval) {
  return GlobalObject::getOrCreateEval(cx, cx->global(), eval);
}

void JS::detail::SetReservedSlotWithBarrier(JSObject* obj, size_t slot,
                                            const Value& value) {
  if (obj->is<ProxyObject>()) {
    obj->as<ProxyObject>().setReservedSlot(slot, value);
  } else {
    obj->as<NativeObject>().setSlot(slot, value);
  }
}

void js::SetPreserveWrapperCallbacks(
    JSContext* cx, PreserveWrapperCallback preserveWrapper,
    HasReleasedWrapperCallback hasReleasedWrapper) {
  cx->runtime()->preserveWrapperCallback = preserveWrapper;
  cx->runtime()->hasReleasedWrapperCallback = hasReleasedWrapper;
}

JS_PUBLIC_API unsigned JS_PCToLineNumber(JSScript* script, jsbytecode* pc,
                                         unsigned* columnp) {
  return PCToLineNumber(script, pc, columnp);
}

JS_PUBLIC_API bool JS_IsDeadWrapper(JSObject* obj) {
  return IsDeadProxyObject(obj);
}

JS_PUBLIC_API JSObject* JS_NewDeadWrapper(JSContext* cx, JSObject* origObj) {
  return NewDeadProxyObject(cx, origObj);
}

void js::TraceWeakMaps(WeakMapTracer* trc) {
  WeakMapBase::traceAllMappings(trc);
}

extern JS_PUBLIC_API bool js::AreGCGrayBitsValid(JSRuntime* rt) {
  return rt->gc.areGrayBitsValid();
}

JS_PUBLIC_API bool js::ZoneGlobalsAreAllGray(JS::Zone* zone) {
  for (RealmsInZoneIter realm(zone); !realm.done(); realm.next()) {
    JSObject* obj = realm->unsafeUnbarrieredMaybeGlobal();
    if (!obj || !JS::ObjectIsMarkedGray(obj)) {
      return false;
    }
  }
  return true;
}

JS_PUBLIC_API bool js::IsCompartmentZoneSweepingOrCompacting(
    JS::Compartment* comp) {
  MOZ_ASSERT(comp);
  return comp->zone()->isGCSweepingOrCompacting();
}

JS_PUBLIC_API void js::TraceGrayWrapperTargets(JSTracer* trc, Zone* zone) {
  JS::AutoSuppressGCAnalysis nogc;

  for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
    for (Compartment::ObjectWrapperEnum e(comp); !e.empty(); e.popFront()) {
      JSObject* target = e.front().key();
      if (target->isMarkedGray()) {
        TraceManuallyBarrieredEdge(trc, &target, "gray CCW target");
        MOZ_ASSERT(target == e.front().key());
      }
    }
  }
}

JSLinearString* JS::detail::StringToLinearStringSlow(JSContext* cx,
                                                     JSString* str) {
  return str->ensureLinear(cx);
}

static bool CopyProxyObject(JSContext* cx, Handle<ProxyObject*> from,
                            Handle<ProxyObject*> to) {
  MOZ_ASSERT(from->getClass() == to->getClass());

  if (from->is<WrapperObject>() &&
      (Wrapper::wrapperHandler(from)->flags() & Wrapper::CROSS_COMPARTMENT)) {
    to->setCrossCompartmentPrivate(GetProxyPrivate(from));
  } else {
    RootedValue v(cx, GetProxyPrivate(from));
    if (!cx->compartment()->wrap(cx, &v)) {
      return false;
    }
    to->setSameCompartmentPrivate(v);
  }

  MOZ_ASSERT(from->numReservedSlots() == to->numReservedSlots());

  RootedValue v(cx);
  for (size_t n = 0; n < from->numReservedSlots(); n++) {
    v = GetProxyReservedSlot(from, n);
    if (!cx->compartment()->wrap(cx, &v)) {
      return false;
    }
    SetProxyReservedSlot(to, n, v);
  }

  return true;
}

JS_PUBLIC_API JSObject* JS_CloneObject(JSContext* cx, HandleObject obj,
                                       HandleObject proto) {
  // |obj| might be in a different compartment.
  cx->check(proto);

  if (!obj->is<NativeObject>() && !obj->is<ProxyObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_CLONE_OBJECT);
    return nullptr;
  }

  RootedObject clone(cx);
  if (obj->is<NativeObject>()) {
    // JS_CloneObject is used to create the target object for JSObject::swap().
    // swap() requires its arguments are tenured, so ensure tenure allocation.
    clone = NewTenuredObjectWithGivenProto(cx, obj->getClass(), proto);
    if (!clone) {
      return nullptr;
    }

    if (clone->is<JSFunction>() &&
        (obj->compartment() != clone->compartment())) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_CANT_CLONE_OBJECT);
      return nullptr;
    }

    if (obj->as<NativeObject>().hasPrivate()) {
      clone->as<NativeObject>().setPrivate(
          obj->as<NativeObject>().getPrivate());
    }
  } else {
    auto* handler = GetProxyHandler(obj);

    // Same as above, require tenure allocation of the clone. This means for
    // proxy objects we need to reject nursery allocatable proxies.
    if (handler->canNurseryAllocate()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_CANT_CLONE_OBJECT);
      return nullptr;
    }

    clone = ProxyObject::New(cx, handler, JS::NullHandleValue,
                             AsTaggedProto(proto), obj->getClass());
    if (!clone) {
      return nullptr;
    }

    if (!CopyProxyObject(cx, obj.as<ProxyObject>(), clone.as<ProxyObject>())) {
      return nullptr;
    }
  }

  return clone;
}

extern JS_PUBLIC_API bool JS::ForceLexicalInitialization(JSContext* cx,
                                                         HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  bool initializedAny = false;
  NativeObject* nobj = &obj->as<NativeObject>();

  for (ShapePropertyIter<NoGC> iter(nobj->shape()); !iter.done(); iter++) {
    Value v = nobj->getSlot(iter->slot());
    if (iter->isDataProperty() && v.isMagic() &&
        v.whyMagic() == JS_UNINITIALIZED_LEXICAL) {
      nobj->setSlot(iter->slot(), UndefinedValue());
      initializedAny = true;
    }
  }
  return initializedAny;
}

extern JS_PUBLIC_API int JS::IsGCPoisoning() {
#ifdef JS_GC_ALLOW_EXTRA_POISONING
  return js::gExtraPoisoningEnabled;
#else
  return false;
#endif
}

JS_PUBLIC_API void JS::NotifyGCRootsRemoved(JSContext* cx) {
  cx->runtime()->gc.notifyRootsRemoved();
}

JS_PUBLIC_API JS::Realm* js::GetAnyRealmInZone(JS::Zone* zone) {
  if (zone->isAtomsZone()) {
    return nullptr;
  }

  RealmsInZoneIter realm(zone);
  MOZ_ASSERT(!realm.done());
  return realm.get();
}

JS_PUBLIC_API bool js::IsSharableCompartment(JS::Compartment* comp) {
  // If this compartment has nuked outgoing wrappers (because all its globals
  // got nuked), we won't be able to create any useful CCWs out of it in the
  // future, and so we shouldn't use it for any new globals.
  if (comp->nukedOutgoingWrappers) {
    return false;
  }

  // If this compartment has no live globals, it might be in the middle of being
  // GCed.  Don't create any new Realms inside.  There's no point to doing that
  // anyway, since the idea would be to avoid CCWs from existing Realms in the
  // compartment to the new Realm, and there are no existing Realms.
  if (!CompartmentHasLiveGlobal(comp)) {
    return false;
  }

  // Good to go.
  return true;
}

JS_PUBLIC_API JSObject* js::GetTestingFunctions(JSContext* cx) {
  RootedObject obj(cx, JS_NewPlainObject(cx));
  if (!obj) {
    return nullptr;
  }

  if (!DefineTestingFunctions(cx, obj, false, false)) {
    return nullptr;
  }

  return obj;
}

JS_PUBLIC_API void js::SetDOMCallbacks(JSContext* cx,
                                       const DOMCallbacks* callbacks) {
  cx->runtime()->DOMcallbacks = callbacks;
}

JS_PUBLIC_API const DOMCallbacks* js::GetDOMCallbacks(JSContext* cx) {
  return cx->runtime()->DOMcallbacks;
}

JS_PUBLIC_API void js::PrepareScriptEnvironmentAndInvoke(
    JSContext* cx, HandleObject global,
    ScriptEnvironmentPreparer::Closure& closure) {
  MOZ_ASSERT(!cx->isExceptionPending());
  MOZ_ASSERT(global->is<GlobalObject>());

  MOZ_RELEASE_ASSERT(
      cx->runtime()->scriptEnvironmentPreparer,
      "Embedding needs to set a scriptEnvironmentPreparer callback");

  cx->runtime()->scriptEnvironmentPreparer->invoke(global, closure);
}

JS_PUBLIC_API void js::SetScriptEnvironmentPreparer(
    JSContext* cx, ScriptEnvironmentPreparer* preparer) {
  cx->runtime()->scriptEnvironmentPreparer = preparer;
}

JS_PUBLIC_API void JS::SetCTypesActivityCallback(JSContext* cx,
                                                 CTypesActivityCallback cb) {
  cx->runtime()->ctypesActivityCallback = cb;
}

JS::AutoCTypesActivityCallback::AutoCTypesActivityCallback(
    JSContext* cx, CTypesActivityType beginType, CTypesActivityType endType)
    : cx(cx),
      callback(cx->runtime()->ctypesActivityCallback),
      endType(endType) {
  if (callback) {
    callback(cx, beginType);
  }
}

JS_PUBLIC_API void js::SetAllocationMetadataBuilder(
    JSContext* cx, const AllocationMetadataBuilder* callback) {
  cx->realm()->setAllocationMetadataBuilder(callback);
}

JS_PUBLIC_API JSObject* js::GetAllocationMetadata(JSObject* obj) {
  ObjectWeakMap* map = ObjectRealm::get(obj).objectMetadataTable.get();
  if (map) {
    return map->lookup(obj);
  }
  return nullptr;
}

JS_PUBLIC_API bool js::ReportIsNotFunction(JSContext* cx, HandleValue v) {
  cx->check(v);
  return ReportIsNotFunction(cx, v, -1);
}

#ifdef DEBUG
bool js::HasObjectMovedOp(JSObject* obj) {
  return !!JS::GetClass(obj)->extObjectMovedOp();
}
#endif

JS_PUBLIC_API bool js::ForwardToNative(JSContext* cx, JSNative native,
                                       const CallArgs& args) {
  return native(cx, args.length(), args.base());
}

AutoAssertNoContentJS::AutoAssertNoContentJS(JSContext* cx)
    : context_(cx), prevAllowContentJS_(cx->runtime()->allowContentJS_) {
  cx->runtime()->allowContentJS_ = false;
}

AutoAssertNoContentJS::~AutoAssertNoContentJS() {
  context_->runtime()->allowContentJS_ = prevAllowContentJS_;
}

JS_PUBLIC_API void js::EnableCodeCoverage() { js::coverage::EnableLCov(); }

JS_PUBLIC_API JS::Value js::MaybeGetScriptPrivate(JSObject* object) {
  if (!object->is<ScriptSourceObject>()) {
    return UndefinedValue();
  }

  return object->as<ScriptSourceObject>().canonicalPrivate();
}

JS_PUBLIC_API uint64_t js::GetGCHeapUsageForObjectZone(JSObject* obj) {
  return obj->zone()->gcHeapSize.bytes();
}

#ifdef DEBUG
JS_PUBLIC_API bool js::RuntimeIsBeingDestroyed() {
  JSRuntime* runtime = TlsContext.get()->runtime();
  MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtime));
  return runtime->isBeingDestroyed();
}
#endif

// No-op implementations of public API that would depend on --with-intl-api

#ifndef JS_HAS_INTL_API

static bool IntlNotEnabled(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                            JSMSG_SUPPORT_NOT_ENABLED, "Intl");
  return false;
}

bool JS::AddMozDateTimeFormatConstructor(JSContext* cx, JS::HandleObject intl) {
  return IntlNotEnabled(cx);
}

bool JS::AddMozDisplayNamesConstructor(JSContext* cx, JS::HandleObject intl) {
  return IntlNotEnabled(cx);
}

#endif  // !JS_HAS_INTL_API

JS_PUBLIC_API JS::Zone* js::GetObjectZoneFromAnyThread(const JSObject* obj) {
  return MaybeForwarded(obj)->zoneFromAnyThread();
}
