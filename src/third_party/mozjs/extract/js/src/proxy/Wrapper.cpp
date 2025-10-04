/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Wrapper.h"

#include "jsexn.h"

#include "js/CallAndConstruct.h"      // JS::Construct, JS::IsConstructor
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/WindowProxy.h"    // js::IsWindowProxy
#include "js/Object.h"                // JS::GetBuiltinClass
#include "js/Proxy.h"
#include "vm/Compartment.h"
#include "vm/ErrorObject.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/ProxyObject.h"
#include "vm/Realm.h"
#include "vm/RegExpObject.h"
#include "vm/WrapperObject.h"

#include "gc/Marking-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

bool Wrapper::finalizeInBackground(const Value& priv) const {
  if (!priv.isObject()) {
    return true;
  }

  /*
   * Make the 'background-finalized-ness' of the wrapper the same as the
   * wrapped object, to allow transplanting between them.
   */
  JSObject* wrapped = MaybeForwarded(&priv.toObject());
  gc::AllocKind wrappedKind;
  if (IsInsideNursery(wrapped)) {
    JSRuntime* rt = wrapped->runtimeFromMainThread();
    wrappedKind = wrapped->allocKindForTenure(rt->gc.nursery());
  } else {
    wrappedKind = wrapped->asTenured().getAllocKind();
  }
  return IsBackgroundFinalized(wrappedKind);
}

bool ForwardingProxyHandler::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject proxy, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
  assertEnteredPolicy(cx, proxy, id, GET | SET | GET_PROPERTY_DESCRIPTOR);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return GetOwnPropertyDescriptor(cx, target, id, desc);
}

bool ForwardingProxyHandler::defineProperty(JSContext* cx, HandleObject proxy,
                                            HandleId id,
                                            Handle<PropertyDescriptor> desc,
                                            ObjectOpResult& result) const {
  assertEnteredPolicy(cx, proxy, id, SET);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return DefineProperty(cx, target, id, desc, result);
}

bool ForwardingProxyHandler::ownPropertyKeys(
    JSContext* cx, HandleObject proxy, MutableHandleIdVector props) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), ENUMERATE);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return GetPropertyKeys(
      cx, target, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, props);
}

bool ForwardingProxyHandler::delete_(JSContext* cx, HandleObject proxy,
                                     HandleId id,
                                     ObjectOpResult& result) const {
  assertEnteredPolicy(cx, proxy, id, SET);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return DeleteProperty(cx, target, id, result);
}

bool ForwardingProxyHandler::enumerate(JSContext* cx, HandleObject proxy,
                                       MutableHandleIdVector props) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), ENUMERATE);
  MOZ_ASSERT(
      !hasPrototype());  // Should never be called if there's a prototype.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return EnumerateProperties(cx, target, props);
}

bool ForwardingProxyHandler::getPrototype(JSContext* cx, HandleObject proxy,
                                          MutableHandleObject protop) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return GetPrototype(cx, target, protop);
}

bool ForwardingProxyHandler::setPrototype(JSContext* cx, HandleObject proxy,
                                          HandleObject proto,
                                          ObjectOpResult& result) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return SetPrototype(cx, target, proto, result);
}

bool ForwardingProxyHandler::getPrototypeIfOrdinary(
    JSContext* cx, HandleObject proxy, bool* isOrdinary,
    MutableHandleObject protop) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return GetPrototypeIfOrdinary(cx, target, isOrdinary, protop);
}

bool ForwardingProxyHandler::setImmutablePrototype(JSContext* cx,
                                                   HandleObject proxy,
                                                   bool* succeeded) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return SetImmutablePrototype(cx, target, succeeded);
}

bool ForwardingProxyHandler::preventExtensions(JSContext* cx,
                                               HandleObject proxy,
                                               ObjectOpResult& result) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return PreventExtensions(cx, target, result);
}

bool ForwardingProxyHandler::isExtensible(JSContext* cx, HandleObject proxy,
                                          bool* extensible) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return IsExtensible(cx, target, extensible);
}

bool ForwardingProxyHandler::has(JSContext* cx, HandleObject proxy, HandleId id,
                                 bool* bp) const {
  assertEnteredPolicy(cx, proxy, id, GET);
  MOZ_ASSERT(
      !hasPrototype());  // Should never be called if there's a prototype.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return HasProperty(cx, target, id, bp);
}

bool ForwardingProxyHandler::get(JSContext* cx, HandleObject proxy,
                                 HandleValue receiver, HandleId id,
                                 MutableHandleValue vp) const {
  assertEnteredPolicy(cx, proxy, id, GET);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return GetProperty(cx, target, receiver, id, vp);
}

bool ForwardingProxyHandler::set(JSContext* cx, HandleObject proxy, HandleId id,
                                 HandleValue v, HandleValue receiver,
                                 ObjectOpResult& result) const {
  assertEnteredPolicy(cx, proxy, id, SET);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return SetProperty(cx, target, id, v, receiver, result);
}

bool ForwardingProxyHandler::call(JSContext* cx, HandleObject proxy,
                                  const CallArgs& args) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), CALL);
  RootedValue target(cx, proxy->as<ProxyObject>().private_());

  InvokeArgs iargs(cx);
  if (!FillArgumentsFromArraylike(cx, iargs, args)) {
    return false;
  }

  return js::Call(cx, target, args.thisv(), iargs, args.rval());
}

bool ForwardingProxyHandler::construct(JSContext* cx, HandleObject proxy,
                                       const CallArgs& args) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), CALL);

  RootedValue target(cx, proxy->as<ProxyObject>().private_());
  if (!IsConstructor(target)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, target,
                     nullptr);
    return false;
  }

  ConstructArgs cargs(cx);
  if (!FillArgumentsFromArraylike(cx, cargs, args)) {
    return false;
  }

  RootedObject obj(cx);
  if (!Construct(cx, target, cargs, args.newTarget(), &obj)) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

bool ForwardingProxyHandler::hasOwn(JSContext* cx, HandleObject proxy,
                                    HandleId id, bool* bp) const {
  assertEnteredPolicy(cx, proxy, id, GET);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return HasOwnProperty(cx, target, id, bp);
}

bool ForwardingProxyHandler::getOwnEnumerablePropertyKeys(
    JSContext* cx, HandleObject proxy, MutableHandleIdVector props) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), ENUMERATE);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return GetPropertyKeys(cx, target, JSITER_OWNONLY, props);
}

bool ForwardingProxyHandler::nativeCall(JSContext* cx, IsAcceptableThis test,
                                        NativeImpl impl,
                                        const CallArgs& args) const {
  args.setThis(
      ObjectValue(*args.thisv().toObject().as<ProxyObject>().target()));
  if (!test(args.thisv())) {
    ReportIncompatible(cx, args);
    return false;
  }

  return CallNativeImpl(cx, impl, args);
}

bool ForwardingProxyHandler::getBuiltinClass(JSContext* cx, HandleObject proxy,
                                             ESClass* cls) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return JS::GetBuiltinClass(cx, target, cls);
}

bool ForwardingProxyHandler::isArray(JSContext* cx, HandleObject proxy,
                                     JS::IsArrayAnswer* answer) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return IsArray(cx, target, answer);
}

const char* ForwardingProxyHandler::className(JSContext* cx,
                                              HandleObject proxy) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), GET);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return GetObjectClassName(cx, target);
}

JSString* ForwardingProxyHandler::fun_toString(JSContext* cx,
                                               HandleObject proxy,
                                               bool isToSource) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), GET);
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return fun_toStringHelper(cx, target, isToSource);
}

RegExpShared* ForwardingProxyHandler::regexp_toShared(
    JSContext* cx, HandleObject proxy) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return RegExpToShared(cx, target);
}

bool ForwardingProxyHandler::boxedValue_unbox(JSContext* cx, HandleObject proxy,
                                              MutableHandleValue vp) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  return Unbox(cx, target, vp);
}

bool ForwardingProxyHandler::isCallable(JSObject* obj) const {
  JSObject* target = obj->as<ProxyObject>().target();
  return target->isCallable();
}

bool ForwardingProxyHandler::isConstructor(JSObject* obj) const {
  JSObject* target = obj->as<ProxyObject>().target();
  return target->isConstructor();
}

JSObject* Wrapper::New(JSContext* cx, JSObject* obj, const Wrapper* handler,
                       const WrapperOptions& options) {
  // If this is a cross-compartment wrapper allocate it in the compartment's
  // first global. See Compartment::globalForNewCCW.
  mozilla::Maybe<AutoRealm> ar;
  if (handler->isCrossCompartmentWrapper()) {
    ar.emplace(cx, &cx->compartment()->globalForNewCCW());
  }
  RootedValue priv(cx, ObjectValue(*obj));
  return NewProxyObject(cx, handler, priv, options.proto(), options);
}

JSObject* Wrapper::Renew(JSObject* existing, JSObject* obj,
                         const Wrapper* handler) {
  existing->as<ProxyObject>().renew(handler, ObjectValue(*obj));
  return existing;
}

JSObject* Wrapper::wrappedObject(JSObject* wrapper) {
  MOZ_ASSERT(wrapper->is<WrapperObject>());
  JSObject* target = wrapper->as<ProxyObject>().target();

  if (target) {
    // A cross-compartment wrapper should never wrap a CCW. We rely on this
    // in the wrapper handlers (we use AutoRealm on our return value, and
    // AutoRealm cannot be used with CCWs).
    MOZ_ASSERT_IF(IsCrossCompartmentWrapper(wrapper),
                  !IsCrossCompartmentWrapper(target));

#ifdef DEBUG
    // An incremental GC will eventually mark the targets of black wrappers
    // black but while it is in progress we can observe gray targets.
    if (!wrapper->runtimeFromMainThread()->gc.isIncrementalGCInProgress() &&
        wrapper->isMarkedBlack()) {
      JS::AssertObjectIsNotGray(target);
    }
#endif

    // Unmark wrapper targets that should be black in case an incremental GC
    // hasn't marked them the correct color yet.
    JS::ExposeObjectToActiveJS(target);
  }

  return target;
}

JS_PUBLIC_API JSObject* js::UncheckedUnwrapWithoutExpose(JSObject* wrapped) {
  while (true) {
    if (!wrapped->is<WrapperObject>() || MOZ_UNLIKELY(IsWindowProxy(wrapped))) {
      break;
    }
    wrapped = wrapped->as<WrapperObject>().target();

    // This can be called from when getting a weakmap key delegate() on a
    // wrapper whose referent has been moved while it is still unmarked.
    if (wrapped) {
      wrapped = MaybeForwarded(wrapped);
    }
  }
  return wrapped;
}

JS_PUBLIC_API JSObject* js::UncheckedUnwrap(JSObject* wrapped,
                                            bool stopAtWindowProxy,
                                            unsigned* flagsp) {
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(wrapped->runtimeFromAnyThread()));

  unsigned flags = 0;
  while (true) {
    if (!wrapped->is<WrapperObject>() ||
        MOZ_UNLIKELY(stopAtWindowProxy && IsWindowProxy(wrapped))) {
      break;
    }
    flags |= Wrapper::wrapperHandler(wrapped)->flags();
    wrapped = Wrapper::wrappedObject(wrapped);
  }
  if (flagsp) {
    *flagsp = flags;
  }
  return wrapped;
}

JS_PUBLIC_API JSObject* js::CheckedUnwrapStatic(JSObject* obj) {
  while (true) {
    JSObject* wrapper = obj;
    obj = UnwrapOneCheckedStatic(obj);
    if (!obj || obj == wrapper) {
      return obj;
    }
  }
}

JS_PUBLIC_API JSObject* js::UnwrapOneCheckedStatic(JSObject* obj) {
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(obj->runtimeFromAnyThread()));

  // Note: callers that care about WindowProxy unwrapping should use
  // CheckedUnwrapDynamic or UnwrapOneCheckedDynamic instead of this. We don't
  // unwrap WindowProxy here to preserve legacy behavior and for consistency
  // with CheckedUnwrapDynamic's default stopAtWindowProxy = true.
  if (!obj->is<WrapperObject>() || MOZ_UNLIKELY(IsWindowProxy(obj))) {
    return obj;
  }

  const Wrapper* handler = Wrapper::wrapperHandler(obj);
  return handler->hasSecurityPolicy() ? nullptr : Wrapper::wrappedObject(obj);
}

JS_PUBLIC_API JSObject* js::CheckedUnwrapDynamic(JSObject* obj, JSContext* cx,
                                                 bool stopAtWindowProxy) {
  RootedObject wrapper(cx, obj);
  while (true) {
    JSObject* unwrapped =
        UnwrapOneCheckedDynamic(wrapper, cx, stopAtWindowProxy);
    if (!unwrapped || unwrapped == wrapper) {
      return unwrapped;
    }
    wrapper = unwrapped;
  }
}

JS_PUBLIC_API JSObject* js::UnwrapOneCheckedDynamic(HandleObject obj,
                                                    JSContext* cx,
                                                    bool stopAtWindowProxy) {
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(obj->runtimeFromAnyThread()));
  // We should know who's asking.
  MOZ_ASSERT(cx);
  MOZ_ASSERT(cx->realm());

  if (!obj->is<WrapperObject>() ||
      MOZ_UNLIKELY(stopAtWindowProxy && IsWindowProxy(obj))) {
    return obj;
  }

  const Wrapper* handler = Wrapper::wrapperHandler(obj);
  if (!handler->hasSecurityPolicy() ||
      handler->dynamicCheckedUnwrapAllowed(obj, cx)) {
    return Wrapper::wrappedObject(obj);
  }

  return nullptr;
}

void js::ReportAccessDenied(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_OBJECT_ACCESS_DENIED);
}

const char Wrapper::family = 0;
const Wrapper Wrapper::singleton((unsigned)0);
const Wrapper Wrapper::singletonWithPrototype((unsigned)0, true);
JSObject* const Wrapper::defaultProto = TaggedProto::LazyProto;

/* Compartments. */

JSObject* js::TransparentObjectWrapper(JSContext* cx, HandleObject existing,
                                       HandleObject obj) {
  // Allow wrapping outer window proxies.
  MOZ_ASSERT(!obj->is<WrapperObject>() || IsWindowProxy(obj));
  return Wrapper::New(cx, obj, &CrossCompartmentWrapper::singleton);
}

ErrorCopier::~ErrorCopier() {
  JSContext* cx = ar->context();

  // The provenance of Debugger.DebuggeeWouldRun is the topmost locking
  // debugger compartment; it should not be copied around.
  if (ar->origin()->compartment() != cx->compartment() &&
      cx->isExceptionPending() && !cx->isThrowingDebuggeeWouldRun()) {
    RootedValue exc(cx);
    if (cx->getPendingException(&exc) && exc.isObject() &&
        exc.toObject().is<ErrorObject>()) {
      Rooted<SavedFrame*> stack(cx, cx->getPendingExceptionStack());
      cx->clearPendingException();
      ar.reset();
      Rooted<ErrorObject*> errObj(cx, &exc.toObject().as<ErrorObject>());
      if (JSObject* copyobj = CopyErrorObject(cx, errObj)) {
        RootedValue rootedCopy(cx, ObjectValue(*copyobj));
        cx->setPendingException(rootedCopy, stack);
      }
    }
  }
}
