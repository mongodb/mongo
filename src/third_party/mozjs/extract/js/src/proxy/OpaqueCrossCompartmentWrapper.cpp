/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Wrapper.h"

#include "vm/JSObject-inl.h"

using namespace js;

bool OpaqueCrossCompartmentWrapper::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject wrapper, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
  desc.reset();
  return true;
}

bool OpaqueCrossCompartmentWrapper::defineProperty(
    JSContext* cx, HandleObject wrapper, HandleId id,
    Handle<PropertyDescriptor> desc, ObjectOpResult& result) const {
  return result.succeed();
}

bool OpaqueCrossCompartmentWrapper::ownPropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  return true;
}

bool OpaqueCrossCompartmentWrapper::delete_(JSContext* cx, HandleObject wrapper,
                                            HandleId id,
                                            ObjectOpResult& result) const {
  return result.succeed();
}

bool OpaqueCrossCompartmentWrapper::enumerate(
    JSContext* cx, HandleObject proxy, MutableHandleIdVector props) const {
  return BaseProxyHandler::enumerate(cx, proxy, props);
}

bool OpaqueCrossCompartmentWrapper::getPrototype(
    JSContext* cx, HandleObject proxy, MutableHandleObject protop) const {
  protop.set(nullptr);
  return true;
}

bool OpaqueCrossCompartmentWrapper::setPrototype(JSContext* cx,
                                                 HandleObject proxy,
                                                 HandleObject proto,
                                                 ObjectOpResult& result) const {
  return result.succeed();
}

bool OpaqueCrossCompartmentWrapper::getPrototypeIfOrdinary(
    JSContext* cx, HandleObject proxy, bool* isOrdinary,
    MutableHandleObject protop) const {
  *isOrdinary = false;
  return true;
}

bool OpaqueCrossCompartmentWrapper::setImmutablePrototype(
    JSContext* cx, HandleObject proxy, bool* succeeded) const {
  *succeeded = false;
  return true;
}

bool OpaqueCrossCompartmentWrapper::preventExtensions(
    JSContext* cx, HandleObject wrapper, ObjectOpResult& result) const {
  return result.failCantPreventExtensions();
}

bool OpaqueCrossCompartmentWrapper::isExtensible(JSContext* cx,
                                                 HandleObject wrapper,
                                                 bool* extensible) const {
  *extensible = true;
  return true;
}

bool OpaqueCrossCompartmentWrapper::has(JSContext* cx, HandleObject wrapper,
                                        HandleId id, bool* bp) const {
  return BaseProxyHandler::has(cx, wrapper, id, bp);
}

bool OpaqueCrossCompartmentWrapper::get(JSContext* cx, HandleObject wrapper,
                                        HandleValue receiver, HandleId id,
                                        MutableHandleValue vp) const {
  return BaseProxyHandler::get(cx, wrapper, receiver, id, vp);
}

bool OpaqueCrossCompartmentWrapper::set(JSContext* cx, HandleObject wrapper,
                                        HandleId id, HandleValue v,
                                        HandleValue receiver,
                                        ObjectOpResult& result) const {
  return BaseProxyHandler::set(cx, wrapper, id, v, receiver, result);
}

bool OpaqueCrossCompartmentWrapper::call(JSContext* cx, HandleObject wrapper,
                                         const CallArgs& args) const {
  RootedValue v(cx, ObjectValue(*wrapper));
  ReportIsNotFunction(cx, v);
  return false;
}

bool OpaqueCrossCompartmentWrapper::construct(JSContext* cx,
                                              HandleObject wrapper,
                                              const CallArgs& args) const {
  RootedValue v(cx, ObjectValue(*wrapper));
  ReportIsNotFunction(cx, v);
  return false;
}

bool OpaqueCrossCompartmentWrapper::hasOwn(JSContext* cx, HandleObject wrapper,
                                           HandleId id, bool* bp) const {
  return BaseProxyHandler::hasOwn(cx, wrapper, id, bp);
}

bool OpaqueCrossCompartmentWrapper::getOwnEnumerablePropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  return BaseProxyHandler::getOwnEnumerablePropertyKeys(cx, wrapper, props);
}

bool OpaqueCrossCompartmentWrapper::getBuiltinClass(JSContext* cx,
                                                    HandleObject wrapper,
                                                    ESClass* cls) const {
  *cls = ESClass::Other;
  return true;
}

bool OpaqueCrossCompartmentWrapper::isArray(JSContext* cx, HandleObject obj,
                                            JS::IsArrayAnswer* answer) const {
  *answer = JS::IsArrayAnswer::NotArray;
  return true;
}

const char* OpaqueCrossCompartmentWrapper::className(JSContext* cx,
                                                     HandleObject proxy) const {
  return "Opaque";
}

JSString* OpaqueCrossCompartmentWrapper::fun_toString(JSContext* cx,
                                                      HandleObject proxy,
                                                      bool isToSource) const {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_INCOMPATIBLE_PROTO, "Function", "toString",
                            "object");
  return nullptr;
}

const OpaqueCrossCompartmentWrapper OpaqueCrossCompartmentWrapper::singleton;
