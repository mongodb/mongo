/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Proxy.h"

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <string.h>

#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/friend/StackLimits.h"  // js::AutoCheckRecursionLimit, js::GetNativeStackLimit
#include "js/friend/WindowProxy.h"  // js::IsWindow, js::IsWindowProxy, js::ToWindowProxyIfWindow
#include "js/PropertySpec.h"
#include "js/Value.h"  // JS::ObjectValue
#include "js/Wrapper.h"
#include "proxy/DeadObjectProxy.h"
#include "proxy/ScriptedProxyHandler.h"
#include "vm/Compartment.h"
#include "vm/Interpreter.h"  // js::CallGetter
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/WrapperObject.h"

#include "gc/Marking-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

// Used by private fields to manipulate the ProxyExpando:
// All the following methods are called iff the handler for the proxy
// returns true for useProxyExpandoObjectForPrivateFields.
static bool ProxySetOnExpando(JSContext* cx, HandleObject proxy, HandleId id,
                              HandleValue v, HandleValue receiver,
                              ObjectOpResult& result) {
  MOZ_ASSERT(id.isPrivateName());

  // For BaseProxyHandler, private names are stored in the expando object.
  RootedObject expando(cx, proxy->as<ProxyObject>().expando().toObjectOrNull());

  // SetPrivateElementOperation checks for hasOwn first, which ensures the
  // expando exsists.
  //
  // If we don't have an expando, then we're probably misusing debugger apis and
  // should just throw.
  if (!expando) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SET_MISSING_PRIVATE);
    return false;
  }

  Rooted<mozilla::Maybe<PropertyDescriptor>> ownDesc(cx);
  if (!GetOwnPropertyDescriptor(cx, expando, id, &ownDesc)) {
    return false;
  }
  if (ownDesc.isNothing()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SET_MISSING_PRIVATE);
    return false;
  }

  RootedValue expandoValue(cx, proxy->as<ProxyObject>().expando());
  return SetPropertyIgnoringNamedGetter(cx, expando, id, v, expandoValue,
                                        ownDesc, result);
}

static bool ProxyGetOwnPropertyDescriptorFromExpando(
    JSContext* cx, HandleObject proxy, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) {
  RootedObject expando(cx, proxy->as<ProxyObject>().expando().toObjectOrNull());

  if (!expando) {
    return true;
  }

  return GetOwnPropertyDescriptor(cx, expando, id, desc);
}

static bool ProxyGetOnExpando(JSContext* cx, HandleObject proxy,
                              HandleValue receiver, HandleId id,
                              MutableHandleValue vp) {
  // For BaseProxyHandler, private names are stored in the expando object.
  RootedObject expando(cx, proxy->as<ProxyObject>().expando().toObjectOrNull());

  // We must have the expando, or GetPrivateElemOperation didn't call
  // hasPrivate first.
  if (!expando) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_GET_MISSING_PRIVATE);
    return false;
  }

  // Because we controlled the creation of the expando, we know it's not a
  // proxy, and so can safely call internal methods on it without worrying about
  // exposing information about private names.
  Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, expando, id, &desc)) {
    return false;
  }
  // We must have the object, same reasoning as the expando.
  if (desc.isNothing()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SET_MISSING_PRIVATE);
    return false;
  }

  // If the private name has a getter, delegate to that.
  if (desc->hasGetter()) {
    RootedValue getter(cx, JS::ObjectValue(*desc->getter()));
    return js::CallGetter(cx, receiver, getter, vp);
  }

  MOZ_ASSERT(desc->hasValue());
  MOZ_ASSERT(desc->isDataDescriptor());

  vp.set(desc->value());
  return true;
}

static bool ProxyHasOnExpando(JSContext* cx, HandleObject proxy, HandleId id,
                              bool* bp) {
  // For BaseProxyHandler, private names are stored in the expando object.
  RootedObject expando(cx, proxy->as<ProxyObject>().expando().toObjectOrNull());

  // If there is no expando object, then there is no private field.
  if (!expando) {
    *bp = false;
    return true;
  }

  return HasOwnProperty(cx, expando, id, bp);
}

static bool ProxyDefineOnExpando(JSContext* cx, HandleObject proxy, HandleId id,
                                 Handle<PropertyDescriptor> desc,
                                 ObjectOpResult& result) {
  MOZ_ASSERT(id.isPrivateName());

  // For BaseProxyHandler, private names are stored in the expando object.
  RootedObject expando(cx, proxy->as<ProxyObject>().expando().toObjectOrNull());

  if (!expando) {
    expando = NewPlainObjectWithProto(cx, nullptr);
    if (!expando) {
      return false;
    }

    proxy->as<ProxyObject>().setExpando(expando);
  }

  return DefineProperty(cx, expando, id, desc, result);
}

void js::AutoEnterPolicy::reportErrorIfExceptionIsNotPending(JSContext* cx,
                                                             HandleId id) {
  if (JS_IsExceptionPending(cx)) {
    return;
  }

  if (id.isVoid()) {
    ReportAccessDenied(cx);
  } else {
    Throw(cx, id, JSMSG_PROPERTY_ACCESS_DENIED);
  }
}

#ifdef DEBUG
void js::AutoEnterPolicy::recordEnter(JSContext* cx, HandleObject proxy,
                                      HandleId id, Action act) {
  if (allowed()) {
    context = cx;
    enteredProxy.emplace(proxy);
    enteredId.emplace(id);
    enteredAction = act;
    prev = cx->enteredPolicy;
    cx->enteredPolicy = this;
  }
}

void js::AutoEnterPolicy::recordLeave() {
  if (enteredProxy) {
    MOZ_ASSERT(context->enteredPolicy == this);
    context->enteredPolicy = prev;
  }
}

JS_PUBLIC_API void js::assertEnteredPolicy(JSContext* cx, JSObject* proxy,
                                           jsid id,
                                           BaseProxyHandler::Action act) {
  MOZ_ASSERT(proxy->is<ProxyObject>());
  MOZ_ASSERT(cx->enteredPolicy);
  MOZ_ASSERT(cx->enteredPolicy->enteredProxy->get() == proxy);
  MOZ_ASSERT(cx->enteredPolicy->enteredId->get() == id);
  MOZ_ASSERT(cx->enteredPolicy->enteredAction & act);
}
#endif

bool Proxy::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject proxy, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  desc.reset();  // default result if we refuse to perform this action
  AutoEnterPolicy policy(cx, handler, proxy, id,
                         BaseProxyHandler::GET_PROPERTY_DESCRIPTOR, true);
  if (!policy.allowed()) {
    return policy.returnValue();
  }

  if (handler->useProxyExpandoObjectForPrivateFields() && id.isPrivateName()) {
    return ProxyGetOwnPropertyDescriptorFromExpando(cx, proxy, id, desc);
  }
  return handler->getOwnPropertyDescriptor(cx, proxy, id, desc);
}

bool Proxy::defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                           Handle<PropertyDescriptor> desc,
                           ObjectOpResult& result) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();

  // We shouldn't be definining a private field if we are supposed to throw;
  // this ought to have been caught by CheckPrivateField.
  MOZ_ASSERT_IF(id.isPrivateName(), !handler->throwOnPrivateField());

  AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
  if (!policy.allowed()) {
    if (!policy.returnValue()) {
      return false;
    }
    return result.succeed();
  }

  // Private field accesses have different semantics depending on the kind
  // of proxy involved, and so take a different path compared to regular
  // [[Get]] operations. For example, scripted handlers don't fire traps
  // when accessing private fields (because of the WeakMap semantics)
  if (id.isPrivateName() && handler->useProxyExpandoObjectForPrivateFields()) {
    return ProxyDefineOnExpando(cx, proxy, id, desc, result);
  }

  return proxy->as<ProxyObject>().handler()->defineProperty(cx, proxy, id, desc,
                                                            result);
}

bool Proxy::ownPropertyKeys(JSContext* cx, HandleObject proxy,
                            MutableHandleIdVector props) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  AutoEnterPolicy policy(cx, handler, proxy, JS::VoidHandlePropertyKey,
                         BaseProxyHandler::ENUMERATE, true);
  if (!policy.allowed()) {
    return policy.returnValue();
  }
  return proxy->as<ProxyObject>().handler()->ownPropertyKeys(cx, proxy, props);
}

bool Proxy::delete_(JSContext* cx, HandleObject proxy, HandleId id,
                    ObjectOpResult& result) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
  if (!policy.allowed()) {
    bool ok = policy.returnValue();
    if (ok) {
      result.succeed();
    }
    return ok;
  }

  // Private names shouldn't take this path, as deleting a private name
  // should be a syntax error.
  MOZ_ASSERT(!id.isPrivateName());

  return proxy->as<ProxyObject>().handler()->delete_(cx, proxy, id, result);
}

JS_PUBLIC_API bool js::AppendUnique(JSContext* cx, MutableHandleIdVector base,
                                    HandleIdVector others) {
  RootedIdVector uniqueOthers(cx);
  if (!uniqueOthers.reserve(others.length())) {
    return false;
  }
  for (size_t i = 0; i < others.length(); ++i) {
    bool unique = true;
    for (size_t j = 0; j < base.length(); ++j) {
      if (others[i].get() == base[j]) {
        unique = false;
        break;
      }
    }
    if (unique) {
      if (!uniqueOthers.append(others[i])) {
        return false;
      }
    }
  }
  return base.appendAll(std::move(uniqueOthers));
}

/* static */
bool Proxy::getPrototype(JSContext* cx, HandleObject proxy,
                         MutableHandleObject proto) {
  MOZ_ASSERT(proxy->hasDynamicPrototype());
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  return proxy->as<ProxyObject>().handler()->getPrototype(cx, proxy, proto);
}

/* static */
bool Proxy::setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                         ObjectOpResult& result) {
  MOZ_ASSERT(proxy->hasDynamicPrototype());
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  return proxy->as<ProxyObject>().handler()->setPrototype(cx, proxy, proto,
                                                          result);
}

/* static */
bool Proxy::getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy,
                                   bool* isOrdinary,
                                   MutableHandleObject proto) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  return proxy->as<ProxyObject>().handler()->getPrototypeIfOrdinary(
      cx, proxy, isOrdinary, proto);
}

/* static */
bool Proxy::setImmutablePrototype(JSContext* cx, HandleObject proxy,
                                  bool* succeeded) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  return handler->setImmutablePrototype(cx, proxy, succeeded);
}

/* static */
bool Proxy::preventExtensions(JSContext* cx, HandleObject proxy,
                              ObjectOpResult& result) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  return handler->preventExtensions(cx, proxy, result);
}

/* static */
bool Proxy::isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  return proxy->as<ProxyObject>().handler()->isExtensible(cx, proxy,
                                                          extensible);
}

bool Proxy::has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  *bp = false;  // default result if we refuse to perform this action
  AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
  if (!policy.allowed()) {
    return policy.returnValue();
  }

  // Private names shouldn't take this path, but only hasOwn;
  MOZ_ASSERT(!id.isPrivateName());

  if (handler->hasPrototype()) {
    if (!handler->hasOwn(cx, proxy, id, bp)) {
      return false;
    }
    if (*bp) {
      return true;
    }

    RootedObject proto(cx);
    if (!GetPrototype(cx, proxy, &proto)) {
      return false;
    }
    if (!proto) {
      return true;
    }

    return HasProperty(cx, proto, id, bp);
  }

  return handler->has(cx, proxy, id, bp);
}

bool js::ProxyHas(JSContext* cx, HandleObject proxy, HandleValue idVal,
                  bool* result) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, idVal, &id)) {
    return false;
  }

  return Proxy::has(cx, proxy, id, result);
}

bool Proxy::hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  *bp = false;  // default result if we refuse to perform this action

  // If the handler is supposed to throw, we'll never have a private field so
  // simply return, as we shouldn't throw an invalid security error when
  // checking for the presence of a private field (WeakMap model).
  if (id.isPrivateName() && handler->throwOnPrivateField()) {
    return true;
  }

  AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
  if (!policy.allowed()) {
    return policy.returnValue();
  }

  // Private field accesses have different semantics depending on the kind
  // of proxy involved, and so take a different path compared to regular
  // [[Get]] operations. For example, scripted handlers don't fire traps
  // when accessing private fields (because of the WeakMap semantics)
  if (id.isPrivateName() && handler->useProxyExpandoObjectForPrivateFields()) {
    return ProxyHasOnExpando(cx, proxy, id, bp);
  }

  return handler->hasOwn(cx, proxy, id, bp);
}

bool js::ProxyHasOwn(JSContext* cx, HandleObject proxy, HandleValue idVal,
                     bool* result) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, idVal, &id)) {
    return false;
  }

  return Proxy::hasOwn(cx, proxy, id, result);
}

static MOZ_ALWAYS_INLINE Value ValueToWindowProxyIfWindow(const Value& v,
                                                          JSObject* proxy) {
  if (v.isObject() && v != ObjectValue(*proxy)) {
    return ObjectValue(*ToWindowProxyIfWindow(&v.toObject()));
  }
  return v;
}

MOZ_ALWAYS_INLINE bool Proxy::getInternal(JSContext* cx, HandleObject proxy,
                                          HandleValue receiver, HandleId id,
                                          MutableHandleValue vp) {
  MOZ_ASSERT_IF(receiver.isObject(), !IsWindow(&receiver.toObject()));

  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();

  // Shouldn't have gotten here, as this should have been caught earlier.
  MOZ_ASSERT_IF(id.isPrivateName(), !handler->throwOnPrivateField());

  vp.setUndefined();  // default result if we refuse to perform this action
  AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
  if (!policy.allowed()) {
    return policy.returnValue();
  }

  // Private field accesses have different semantics depending on the kind
  // of proxy involved, and so take a different path compared to regular
  // [[Get]] operations. For example, scripted handlers don't fire traps
  // when accessing private fields (because of the WeakMap semantics)
  if (id.isPrivateName() && handler->useProxyExpandoObjectForPrivateFields()) {
    return ProxyGetOnExpando(cx, proxy, receiver, id, vp);
  }

  if (handler->hasPrototype()) {
    bool own;
    if (!handler->hasOwn(cx, proxy, id, &own)) {
      return false;
    }
    if (!own) {
      RootedObject proto(cx);
      if (!GetPrototype(cx, proxy, &proto)) {
        return false;
      }
      if (!proto) {
        return true;
      }
      return GetProperty(cx, proto, receiver, id, vp);
    }
  }

  return handler->get(cx, proxy, receiver, id, vp);
}

bool Proxy::get(JSContext* cx, HandleObject proxy, HandleValue receiver_,
                HandleId id, MutableHandleValue vp) {
  // Use the WindowProxy as receiver if receiver_ is a Window. Proxy handlers
  // shouldn't have to know about the Window/WindowProxy distinction.
  RootedValue receiver(cx, ValueToWindowProxyIfWindow(receiver_, proxy));
  return getInternal(cx, proxy, receiver, id, vp);
}

bool js::ProxyGetProperty(JSContext* cx, HandleObject proxy, HandleId id,
                          MutableHandleValue vp) {
  RootedValue receiver(cx, ObjectValue(*proxy));
  return Proxy::getInternal(cx, proxy, receiver, id, vp);
}

bool js::ProxyGetPropertyByValue(JSContext* cx, HandleObject proxy,
                                 HandleValue idVal, MutableHandleValue vp) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, idVal, &id)) {
    return false;
  }

  RootedValue receiver(cx, ObjectValue(*proxy));
  return Proxy::getInternal(cx, proxy, receiver, id, vp);
}

MOZ_ALWAYS_INLINE bool Proxy::setInternal(JSContext* cx, HandleObject proxy,
                                          HandleId id, HandleValue v,
                                          HandleValue receiver,
                                          ObjectOpResult& result) {
  MOZ_ASSERT_IF(receiver.isObject(), !IsWindow(&receiver.toObject()));

  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();

  // Should have been handled already.
  MOZ_ASSERT_IF(id.isPrivateName(), !handler->throwOnPrivateField());

  AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
  if (!policy.allowed()) {
    if (!policy.returnValue()) {
      return false;
    }
    return result.succeed();
  }

  // Private field accesses have different semantics depending on the kind
  // of proxy involved, and so take a different path compared to regular
  // [[Set]] operations.
  //
  // This doesn't interact with hasPrototype, as PrivateFields are always
  // own propertiers, and so we never deal with prototype traversals.
  if (id.isPrivateName() && handler->useProxyExpandoObjectForPrivateFields()) {
    return ProxySetOnExpando(cx, proxy, id, v, receiver, result);
  }

  // Special case. See the comment on BaseProxyHandler::mHasPrototype.
  if (handler->hasPrototype()) {
    return handler->BaseProxyHandler::set(cx, proxy, id, v, receiver, result);
  }

  return handler->set(cx, proxy, id, v, receiver, result);
}

bool Proxy::set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                HandleValue receiver_, ObjectOpResult& result) {
  // Use the WindowProxy as receiver if receiver_ is a Window. Proxy handlers
  // shouldn't have to know about the Window/WindowProxy distinction.
  RootedValue receiver(cx, ValueToWindowProxyIfWindow(receiver_, proxy));
  return setInternal(cx, proxy, id, v, receiver, result);
}

bool js::ProxySetProperty(JSContext* cx, HandleObject proxy, HandleId id,
                          HandleValue val, bool strict) {
  ObjectOpResult result;
  RootedValue receiver(cx, ObjectValue(*proxy));
  if (!Proxy::setInternal(cx, proxy, id, val, receiver, result)) {
    return false;
  }
  return result.checkStrictModeError(cx, proxy, id, strict);
}

bool js::ProxySetPropertyByValue(JSContext* cx, HandleObject proxy,
                                 HandleValue idVal, HandleValue val,
                                 bool strict) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, idVal, &id)) {
    return false;
  }

  ObjectOpResult result;
  RootedValue receiver(cx, ObjectValue(*proxy));
  if (!Proxy::setInternal(cx, proxy, id, val, receiver, result)) {
    return false;
  }
  return result.checkStrictModeError(cx, proxy, id, strict);
}

bool Proxy::getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy,
                                         MutableHandleIdVector props) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  AutoEnterPolicy policy(cx, handler, proxy, JS::VoidHandlePropertyKey,
                         BaseProxyHandler::ENUMERATE, true);
  if (!policy.allowed()) {
    return policy.returnValue();
  }
  return handler->getOwnEnumerablePropertyKeys(cx, proxy, props);
}

bool Proxy::enumerate(JSContext* cx, HandleObject proxy,
                      MutableHandleIdVector props) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  if (handler->hasPrototype()) {
    if (!Proxy::getOwnEnumerablePropertyKeys(cx, proxy, props)) {
      return false;
    }

    RootedObject proto(cx);
    if (!GetPrototype(cx, proxy, &proto)) {
      return false;
    }
    if (!proto) {
      return true;
    }

    cx->check(proxy, proto);

    RootedIdVector protoProps(cx);
    if (!GetPropertyKeys(cx, proto, 0, &protoProps)) {
      return false;
    }
    return AppendUnique(cx, props, protoProps);
  }

  AutoEnterPolicy policy(cx, handler, proxy, JS::VoidHandlePropertyKey,
                         BaseProxyHandler::ENUMERATE, true);

  // If the policy denies access but wants us to return true, we need
  // to return an empty |props| list.
  if (!policy.allowed()) {
    MOZ_ASSERT(props.empty());
    return policy.returnValue();
  }

  return handler->enumerate(cx, proxy, props);
}

bool Proxy::call(JSContext* cx, HandleObject proxy, const CallArgs& args) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();

  // Because vp[0] is JS_CALLEE on the way in and JS_RVAL on the way out, we
  // can only set our default value once we're sure that we're not calling the
  // trap.
  AutoEnterPolicy policy(cx, handler, proxy, JS::VoidHandlePropertyKey,
                         BaseProxyHandler::CALL, true);
  if (!policy.allowed()) {
    args.rval().setUndefined();
    return policy.returnValue();
  }

  return handler->call(cx, proxy, args);
}

bool Proxy::construct(JSContext* cx, HandleObject proxy, const CallArgs& args) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();

  // Because vp[0] is JS_CALLEE on the way in and JS_RVAL on the way out, we
  // can only set our default value once we're sure that we're not calling the
  // trap.
  AutoEnterPolicy policy(cx, handler, proxy, JS::VoidHandlePropertyKey,
                         BaseProxyHandler::CALL, true);
  if (!policy.allowed()) {
    args.rval().setUndefined();
    return policy.returnValue();
  }

  return handler->construct(cx, proxy, args);
}

bool Proxy::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                       const CallArgs& args) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  RootedObject proxy(cx, &args.thisv().toObject());
  // Note - we don't enter a policy here because our security architecture
  // guards against nativeCall by overriding the trap itself in the right
  // circumstances.
  return proxy->as<ProxyObject>().handler()->nativeCall(cx, test, impl, args);
}

bool Proxy::getBuiltinClass(JSContext* cx, HandleObject proxy, ESClass* cls) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  return proxy->as<ProxyObject>().handler()->getBuiltinClass(cx, proxy, cls);
}

bool Proxy::isArray(JSContext* cx, HandleObject proxy,
                    JS::IsArrayAnswer* answer) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  return proxy->as<ProxyObject>().handler()->isArray(cx, proxy, answer);
}

const char* Proxy::className(JSContext* cx, HandleObject proxy) {
  // Check for unbounded recursion, but don't signal an error; className
  // needs to be infallible.
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.checkDontReport(cx)) {
    return "too much recursion";
  }

  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  AutoEnterPolicy policy(cx, handler, proxy, JS::VoidHandlePropertyKey,
                         BaseProxyHandler::GET, /* mayThrow = */ false);
  // Do the safe thing if the policy rejects.
  if (!policy.allowed()) {
    return handler->BaseProxyHandler::className(cx, proxy);
  }
  return handler->className(cx, proxy);
}

JSString* Proxy::fun_toString(JSContext* cx, HandleObject proxy,
                              bool isToSource) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return nullptr;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  AutoEnterPolicy policy(cx, handler, proxy, JS::VoidHandlePropertyKey,
                         BaseProxyHandler::GET, /* mayThrow = */ false);
  // Do the safe thing if the policy rejects.
  if (!policy.allowed()) {
    return handler->BaseProxyHandler::fun_toString(cx, proxy, isToSource);
  }
  return handler->fun_toString(cx, proxy, isToSource);
}

RegExpShared* Proxy::regexp_toShared(JSContext* cx, HandleObject proxy) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return nullptr;
  }
  return proxy->as<ProxyObject>().handler()->regexp_toShared(cx, proxy);
}

bool Proxy::boxedValue_unbox(JSContext* cx, HandleObject proxy,
                             MutableHandleValue vp) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  return proxy->as<ProxyObject>().handler()->boxedValue_unbox(cx, proxy, vp);
}

JSObject* const TaggedProto::LazyProto = reinterpret_cast<JSObject*>(0x1);

/* static */
bool Proxy::getElements(JSContext* cx, HandleObject proxy, uint32_t begin,
                        uint32_t end, ElementAdder* adder) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  AutoEnterPolicy policy(cx, handler, proxy, JS::VoidHandlePropertyKey,
                         BaseProxyHandler::GET,
                         /* mayThrow = */ true);
  if (!policy.allowed()) {
    if (policy.returnValue()) {
      MOZ_ASSERT(!cx->isExceptionPending());
      return js::GetElementsWithAdder(cx, proxy, proxy, begin, end, adder);
    }
    return false;
  }
  return handler->getElements(cx, proxy, begin, end, adder);
}

/* static */
void Proxy::trace(JSTracer* trc, JSObject* proxy) {
  const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
  handler->trace(trc, proxy);
}

static bool proxy_LookupProperty(JSContext* cx, HandleObject obj, HandleId id,
                                 MutableHandleObject objp,
                                 PropertyResult* propp) {
  bool found;
  if (!Proxy::has(cx, obj, id, &found)) {
    return false;
  }

  if (found) {
    propp->setProxyProperty();
    objp.set(obj);
  } else {
    propp->setNotFound();
    objp.set(nullptr);
  }
  return true;
}

static bool proxy_DeleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                                 ObjectOpResult& result) {
  if (!Proxy::delete_(cx, obj, id, result)) {
    return false;
  }
  return SuppressDeletedProperty(cx, obj, id);  // XXX is this necessary?
}

/* static */
void ProxyObject::traceEdgeToTarget(JSTracer* trc, ProxyObject* obj) {
  TraceCrossCompartmentEdge(trc, obj, obj->slotOfPrivate(), "proxy target");
}

#ifdef DEBUG
static inline void CheckProxyIsInCCWMap(ProxyObject* proxy) {
  if (proxy->zone()->isGCCompacting()) {
    // Skip this check during compacting GC since objects' object groups may be
    // forwarded. It's not impossible to make this work, but requires adding a
    // parallel lookupWrapper() path for this one case.
    return;
  }

  JSObject* referent = MaybeForwarded(proxy->target());
  if (referent->compartment() != proxy->compartment()) {
    // Assert that this proxy is tracked in the wrapper map. We maintain the
    // invariant that the wrapped object is the key in the wrapper map.
    ObjectWrapperMap::Ptr p = proxy->compartment()->lookupWrapper(referent);
    MOZ_ASSERT(p);
    MOZ_ASSERT(*p->value().unsafeGet() == proxy);
  }
}
#endif

/* static */
void ProxyObject::trace(JSTracer* trc, JSObject* obj) {
  ProxyObject* proxy = &obj->as<ProxyObject>();

  TraceNullableEdge(trc, proxy->slotOfExpando(), "expando");

#ifdef DEBUG
  JSContext* cx = TlsContext.get();
  if (cx && cx->isStrictProxyCheckingEnabled() && proxy->is<WrapperObject>()) {
    CheckProxyIsInCCWMap(proxy);
  }
#endif

  // Note: If you add new slots here, make sure to change
  // nuke() to cope.

  traceEdgeToTarget(trc, proxy);

  size_t nreserved = proxy->numReservedSlots();
  for (size_t i = 0; i < nreserved; i++) {
    /*
     * The GC can use the second reserved slot to link the cross compartment
     * wrappers into a linked list, in which case we don't want to trace it.
     */
    if (proxy->is<CrossCompartmentWrapperObject>() &&
        i == CrossCompartmentWrapperObject::GrayLinkReservedSlot) {
      continue;
    }
    TraceEdge(trc, proxy->reservedSlotPtr(i), "proxy_reserved");
  }

  Proxy::trace(trc, obj);
}

static void proxy_Finalize(JS::GCContext* gcx, JSObject* obj) {
  // Suppress a bogus warning about finalize().
  JS::AutoSuppressGCAnalysis nogc;

  MOZ_ASSERT(obj->is<ProxyObject>());
  ProxyObject* proxy = &obj->as<ProxyObject>();
  proxy->handler()->finalize(gcx, obj);

  if (!proxy->usingInlineValueArray() && proxy->isTenured()) {
    auto* valArray = js::detail::GetProxyDataLayout(obj)->values();
    size_t size =
        js::detail::ProxyValueArray::sizeOf(proxy->numReservedSlots());
    gcx->free_(obj, valArray, size, MemoryUse::ProxyExternalValueArray);
  }
}

size_t js::proxy_ObjectMoved(JSObject* obj, JSObject* old) {
  ProxyObject& proxy = obj->as<ProxyObject>();

  if (IsInsideNursery(old)) {
    proxy.nurseryProxyTenured(&old->as<ProxyObject>());
  }

  return proxy.handler()->objectMoved(obj, old);
}

void ProxyObject::nurseryProxyTenured(ProxyObject* old) {
  if (old->usingInlineValueArray()) {
    setInlineValueArray();
    return;
  }

  Nursery& nursery = runtimeFromMainThread()->gc.nursery();
  nursery.removeMallocedBufferDuringMinorGC(data.values());

  size_t size = detail::ProxyValueArray::sizeOf(numReservedSlots());
  AddCellMemory(this, size, MemoryUse::ProxyExternalValueArray);
}

const JSClassOps js::ProxyClassOps = {
    nullptr,             // addProperty
    nullptr,             // delProperty
    nullptr,             // enumerate
    nullptr,             // newEnumerate
    nullptr,             // resolve
    nullptr,             // mayResolve
    proxy_Finalize,      // finalize
    nullptr,             // call
    nullptr,             // construct
    ProxyObject::trace,  // trace
};

const ClassExtension js::ProxyClassExtension = {
    proxy_ObjectMoved,  // objectMovedOp
};

const ObjectOps js::ProxyObjectOps = {
    proxy_LookupProperty,             // lookupProperty
    Proxy::defineProperty,            // defineProperty
    Proxy::has,                       // hasProperty
    Proxy::get,                       // getProperty
    Proxy::set,                       // setProperty
    Proxy::getOwnPropertyDescriptor,  // getOwnPropertyDescriptor
    proxy_DeleteProperty,             // deleteProperty
    Proxy::getElements,               // getElements
    Proxy::fun_toString,              // funToString
};

static const JSFunctionSpec proxy_static_methods[] = {
    JS_FN("revocable", proxy_revocable, 2, 0), JS_FS_END};

static const ClassSpec ProxyClassSpec = {
    GenericCreateConstructor<js::proxy, 2, gc::AllocKind::FUNCTION>, nullptr,
    proxy_static_methods, nullptr};

const JSClass js::ProxyClass = PROXY_CLASS_DEF_WITH_CLASS_SPEC(
    "Proxy",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Proxy) | JSCLASS_HAS_RESERVED_SLOTS(2),
    &ProxyClassSpec);

JS_PUBLIC_API JSObject* js::NewProxyObject(JSContext* cx,
                                           const BaseProxyHandler* handler,
                                           HandleValue priv, JSObject* proto_,
                                           const ProxyOptions& options) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  // This can be called from the compartment wrap hooks while in a realm with a
  // gray global. Trigger the read barrier on the global to ensure this is
  // unmarked.
  cx->realm()->maybeGlobal();

  if (proto_ != TaggedProto::LazyProto) {
    cx->check(proto_);  // |priv| might be cross-compartment.
  }

  if (options.lazyProto()) {
    MOZ_ASSERT(!proto_);
    proto_ = TaggedProto::LazyProto;
  }

  return ProxyObject::New(cx, handler, priv, TaggedProto(proto_),
                          options.clasp());
}

void ProxyObject::renew(const BaseProxyHandler* handler, const Value& priv) {
  MOZ_ASSERT_IF(IsCrossCompartmentWrapper(this), IsDeadProxyObject(this));
  MOZ_ASSERT(getClass() == &ProxyClass);
  MOZ_ASSERT(!IsWindowProxy(this));
  MOZ_ASSERT(hasDynamicPrototype());

  setHandler(handler);
  setCrossCompartmentPrivate(priv);
  for (size_t i = 0; i < numReservedSlots(); i++) {
    setReservedSlot(i, UndefinedValue());
  }
}

// This implementation of HostEnsureCanAddPrivateElement is designed to work in
// collaboration with Gecko to support the HTML implementation, which applies
// only to Proxy type objects, and as a result we can simply provide proxy
// handlers to correctly match the required semantics.
bool DefaultHostEnsureCanAddPrivateElementCallback(JSContext* cx,
                                                   HandleValue val) {
  if (!val.isObject()) {
    return true;
  }

  Rooted<JSObject*> valObj(cx, &val.toObject());
  if (!IsProxy(valObj)) {
    return true;
  }

  if (GetProxyHandler(valObj)->throwOnPrivateField()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ILLEGAL_PRIVATE_EXOTIC);
    return false;
  }
  return true;
}
