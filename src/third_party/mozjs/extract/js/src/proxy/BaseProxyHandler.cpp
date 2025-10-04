/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsapi.h"
#include "NamespaceImports.h"

#include "gc/GC.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Proxy.h"
#include "proxy/DeadObjectProxy.h"
#include "vm/Interpreter.h"
#include "vm/ProxyObject.h"
#include "vm/WrapperObject.h"

#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;

using JS::IsArrayAnswer;

bool BaseProxyHandler::enter(JSContext* cx, HandleObject wrapper, HandleId id,
                             Action act, bool mayThrow, bool* bp) const {
  *bp = true;
  return true;
}

bool BaseProxyHandler::has(JSContext* cx, HandleObject proxy, HandleId id,
                           bool* bp) const {
  assertEnteredPolicy(cx, proxy, id, GET);

  // This method is not covered by any spec, but we follow ES 2016
  // (February 11, 2016) 9.1.7.1 fairly closely.

  // Step 2. (Step 1 is a superfluous assertion.)
  // Non-standard: Use our faster hasOwn trap.
  if (!hasOwn(cx, proxy, id, bp)) {
    return false;
  }

  // Step 3.
  if (*bp) {
    return true;
  }

  // The spec calls this variable "parent", but that word has weird
  // connotations in SpiderMonkey, so let's go with "proto".
  // Step 4.
  RootedObject proto(cx);
  if (!GetPrototype(cx, proxy, &proto)) {
    return false;
  }

  // Step 5.,5.a.
  if (proto) {
    return HasProperty(cx, proto, id, bp);
  }

  // Step 6.
  *bp = false;
  return true;
}

bool BaseProxyHandler::hasOwn(JSContext* cx, HandleObject proxy, HandleId id,
                              bool* bp) const {
  assertEnteredPolicy(cx, proxy, id, GET);
  Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  if (!getOwnPropertyDescriptor(cx, proxy, id, &desc)) {
    return false;
  }
  *bp = desc.isSome();
  return true;
}

bool BaseProxyHandler::get(JSContext* cx, HandleObject proxy,
                           HandleValue receiver, HandleId id,
                           MutableHandleValue vp) const {
  assertEnteredPolicy(cx, proxy, id, GET);

  // This method is not covered by any spec, but we follow ES 2016
  // (January 21, 2016) 9.1.8 fairly closely.

  // Step 2. (Step 1 is a superfluous assertion.)
  Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  if (!getOwnPropertyDescriptor(cx, proxy, id, &desc)) {
    return false;
  }
  if (desc.isSome()) {
    desc->assertComplete();
  }

  // Step 3.
  if (desc.isNothing()) {
    // The spec calls this variable "parent", but that word has weird
    // connotations in SpiderMonkey, so let's go with "proto".
    // Step 3.a.
    RootedObject proto(cx);
    if (!GetPrototype(cx, proxy, &proto)) {
      return false;
    }

    // Step 3.b.
    if (!proto) {
      vp.setUndefined();
      return true;
    }

    // Step 3.c.
    return GetProperty(cx, proto, receiver, id, vp);
  }

  // Step 4.
  if (desc->isDataDescriptor()) {
    vp.set(desc->value());
    return true;
  }

  // Step 5.
  MOZ_ASSERT(desc->isAccessorDescriptor());
  RootedObject getter(cx, desc->getter());

  // Step 6.
  if (!getter) {
    vp.setUndefined();
    return true;
  }

  // Step 7.
  RootedValue getterFunc(cx, ObjectValue(*getter));
  return CallGetter(cx, receiver, getterFunc, vp);
}

bool BaseProxyHandler::set(JSContext* cx, HandleObject proxy, HandleId id,
                           HandleValue v, HandleValue receiver,
                           ObjectOpResult& result) const {
  assertEnteredPolicy(cx, proxy, id, SET);

  // This method is not covered by any spec, but we follow ES6 draft rev 28
  // (2014 Oct 14) 9.1.9 fairly closely, adapting it slightly for
  // SpiderMonkey's particular foibles.

  // Steps 2-3.  (Step 1 is a superfluous assertion.)
  Rooted<mozilla::Maybe<PropertyDescriptor>> ownDesc(cx);
  if (!getOwnPropertyDescriptor(cx, proxy, id, &ownDesc)) {
    return false;
  }
  if (ownDesc.isSome()) {
    ownDesc->assertComplete();
  }

  // The rest is factored out into a separate function with a weird name.
  // This algorithm continues just below.
  return SetPropertyIgnoringNamedGetter(cx, proxy, id, v, receiver, ownDesc,
                                        result);
}

bool js::SetPropertyIgnoringNamedGetter(
    JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
    HandleValue receiver, Handle<mozilla::Maybe<PropertyDescriptor>> ownDesc_,
    ObjectOpResult& result) {
  Rooted<PropertyDescriptor> ownDesc(cx);

  // Step 4.
  if (ownDesc_.isNothing()) {
    // The spec calls this variable "parent", but that word has weird
    // connotations in SpiderMonkey, so let's go with "proto".
    RootedObject proto(cx);
    if (!GetPrototype(cx, obj, &proto)) {
      return false;
    }
    if (proto) {
      return SetProperty(cx, proto, id, v, receiver, result);
    }

    // Step 4.d.
    ownDesc.set(PropertyDescriptor::Data(
        UndefinedValue(),
        {JS::PropertyAttribute::Configurable, JS::PropertyAttribute::Enumerable,
         JS::PropertyAttribute::Writable}));
  } else {
    ownDesc.set(*ownDesc_);
  }

  // Step 5.
  if (ownDesc.isDataDescriptor()) {
    // Steps 5.a-b.
    if (!ownDesc.writable()) {
      return result.fail(JSMSG_READ_ONLY);
    }
    if (!receiver.isObject()) {
      return result.fail(JSMSG_SET_NON_OBJECT_RECEIVER);
    }
    RootedObject receiverObj(cx, &receiver.toObject());

    // Steps 5.c-d.
    Rooted<mozilla::Maybe<PropertyDescriptor>> existingDescriptor(cx);
    if (!GetOwnPropertyDescriptor(cx, receiverObj, id, &existingDescriptor)) {
      return false;
    }

    // Step 5.e.
    if (existingDescriptor.isSome()) {
      // Step 5.e.i.
      if (existingDescriptor->isAccessorDescriptor()) {
        return result.fail(JSMSG_OVERWRITING_ACCESSOR);
      }

      // Step 5.e.ii.
      if (!existingDescriptor->writable()) {
        return result.fail(JSMSG_READ_ONLY);
      }
    }

    // Steps 5.e.iii-iv. and 5.f.i.
    Rooted<PropertyDescriptor> desc(cx);
    if (existingDescriptor.isSome()) {
      desc = PropertyDescriptor::Empty();
      desc.setValue(v);
    } else {
      desc = PropertyDescriptor::Data(v, {JS::PropertyAttribute::Configurable,
                                          JS::PropertyAttribute::Enumerable,
                                          JS::PropertyAttribute::Writable});
    }
    return DefineProperty(cx, receiverObj, id, desc, result);
  }

  // Step 6.
  MOZ_ASSERT(ownDesc.isAccessorDescriptor());
  RootedObject setter(cx);
  if (ownDesc.hasSetter()) {
    setter = ownDesc.setter();
  }
  if (!setter) {
    return result.fail(JSMSG_GETTER_ONLY);
  }
  RootedValue setterValue(cx, ObjectValue(*setter));
  if (!CallSetter(cx, receiver, setterValue, v)) {
    return false;
  }
  return result.succeed();
}

bool BaseProxyHandler::getOwnEnumerablePropertyKeys(
    JSContext* cx, HandleObject proxy, MutableHandleIdVector props) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), ENUMERATE);
  MOZ_ASSERT(props.length() == 0);

  if (!ownPropertyKeys(cx, proxy, props)) {
    return false;
  }

  /* Select only the enumerable properties through in-place iteration. */
  RootedId id(cx);
  size_t i = 0;
  for (size_t j = 0, len = props.length(); j < len; j++) {
    MOZ_ASSERT(i <= j);
    id = props[j];
    if (id.isSymbol()) {
      continue;
    }

    AutoWaivePolicy policy(cx, proxy, id, BaseProxyHandler::GET);
    Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
    if (!getOwnPropertyDescriptor(cx, proxy, id, &desc)) {
      return false;
    }
    if (desc.isSome()) {
      desc->assertComplete();
    }

    if (desc.isSome() && desc->enumerable()) {
      props[i++].set(id);
    }
  }

  MOZ_ASSERT(i <= props.length());
  if (!props.resize(i)) {
    return false;
  }

  return true;
}

bool BaseProxyHandler::enumerate(JSContext* cx, HandleObject proxy,
                                 MutableHandleIdVector props) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), ENUMERATE);

  // GetPropertyKeys will invoke getOwnEnumerablePropertyKeys along the proto
  // chain for us.
  MOZ_ASSERT(props.empty());
  return GetPropertyKeys(cx, proxy, 0, props);
}

bool BaseProxyHandler::call(JSContext* cx, HandleObject proxy,
                            const CallArgs& args) const {
  MOZ_CRASH("callable proxies should implement call trap");
}

bool BaseProxyHandler::construct(JSContext* cx, HandleObject proxy,
                                 const CallArgs& args) const {
  MOZ_CRASH("callable proxies should implement construct trap");
}

const char* BaseProxyHandler::className(JSContext* cx,
                                        HandleObject proxy) const {
  return proxy->isCallable() ? "Function" : "Object";
}

JSString* BaseProxyHandler::fun_toString(JSContext* cx, HandleObject proxy,
                                         bool isToSource) const {
  if (proxy->isCallable()) {
    return JS_NewStringCopyZ(cx, "function () {\n    [native code]\n}");
  }

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_INCOMPATIBLE_PROTO, "Function", "toString",
                            "object");
  return nullptr;
}

RegExpShared* BaseProxyHandler::regexp_toShared(JSContext* cx,
                                                HandleObject proxy) const {
  MOZ_CRASH("This should have been a wrapped regexp");
}

bool BaseProxyHandler::boxedValue_unbox(JSContext* cx, HandleObject proxy,
                                        MutableHandleValue vp) const {
  vp.setUndefined();
  return true;
}

bool BaseProxyHandler::nativeCall(JSContext* cx, IsAcceptableThis test,
                                  NativeImpl impl, const CallArgs& args) const {
  ReportIncompatible(cx, args);
  return false;
}

bool BaseProxyHandler::getBuiltinClass(JSContext* cx, HandleObject proxy,
                                       ESClass* cls) const {
  *cls = ESClass::Other;
  return true;
}

bool BaseProxyHandler::isArray(JSContext* cx, HandleObject proxy,
                               IsArrayAnswer* answer) const {
  *answer = IsArrayAnswer::NotArray;
  return true;
}

void BaseProxyHandler::trace(JSTracer* trc, JSObject* proxy) const {}

void BaseProxyHandler::finalize(JS::GCContext* gcx, JSObject* proxy) const {}

size_t BaseProxyHandler::objectMoved(JSObject* proxy, JSObject* old) const {
  return 0;
}

bool BaseProxyHandler::getPrototype(JSContext* cx, HandleObject proxy,
                                    MutableHandleObject protop) const {
  MOZ_CRASH("must override getPrototype with dynamic prototype");
}

bool BaseProxyHandler::setPrototype(JSContext* cx, HandleObject proxy,
                                    HandleObject proto,
                                    ObjectOpResult& result) const {
  // Disallow sets of protos on proxies with dynamic prototypes but no hook.
  // This keeps us away from the footgun of having the first proto set opt
  // you out of having dynamic protos altogether.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_CANT_SET_PROTO_OF, "incompatible Proxy");
  return false;
}

bool BaseProxyHandler::setImmutablePrototype(JSContext* cx, HandleObject proxy,
                                             bool* succeeded) const {
  *succeeded = false;
  return true;
}

bool BaseProxyHandler::getElements(JSContext* cx, HandleObject proxy,
                                   uint32_t begin, uint32_t end,
                                   ElementAdder* adder) const {
  assertEnteredPolicy(cx, proxy, JS::PropertyKey::Void(), GET);

  return js::GetElementsWithAdder(cx, proxy, proxy, begin, end, adder);
}

bool BaseProxyHandler::isCallable(JSObject* obj) const { return false; }

bool BaseProxyHandler::isConstructor(JSObject* obj) const { return false; }

JS_PUBLIC_API void js::NukeNonCCWProxy(JSContext* cx, HandleObject proxy) {
  MOZ_ASSERT(proxy->is<ProxyObject>());
  MOZ_ASSERT(!proxy->is<CrossCompartmentWrapperObject>());

  // (NotifyGCNukeWrapper() only needs to be called on CCWs.)

  // The proxy is about to be replaced, so we need to do any necessary
  // cleanup first.
  proxy->as<ProxyObject>().handler()->finalize(cx->gcContext(), proxy);

  proxy->as<ProxyObject>().nuke();

  MOZ_ASSERT(IsDeadProxyObject(proxy));
}

JS_PUBLIC_API void js::NukeRemovedCrossCompartmentWrapper(JSContext* cx,
                                                          JSObject* wrapper) {
  MOZ_ASSERT(wrapper->is<CrossCompartmentWrapperObject>());

  NotifyGCNukeWrapper(cx, wrapper);

  // We don't need to call finalize here because the CCW finalizer doesn't do
  // anything. Skipping finalize means that |wrapper| doesn't need to be rooted
  // to pass the hazard analysis, which is needed because this method is called
  // from some tricky places inside transplanting where rooting can be
  // difficult.

  wrapper->as<ProxyObject>().nuke();

  MOZ_ASSERT(IsDeadProxyObject(wrapper));
}
