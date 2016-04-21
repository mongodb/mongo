/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jswrapper.h"

#include "jsobjinlines.h"

using namespace js;

bool
OpaqueCrossCompartmentWrapper::getOwnPropertyDescriptor(JSContext* cx,
                                                        HandleObject wrapper,
                                                        HandleId id,
                                                        MutableHandle<PropertyDescriptor> desc) const
{
    desc.object().set(nullptr);
    return true;
}

bool
OpaqueCrossCompartmentWrapper::defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                              Handle<JSPropertyDescriptor> desc,
                                              ObjectOpResult& result) const
{
    return result.succeed();
}

bool
OpaqueCrossCompartmentWrapper::ownPropertyKeys(JSContext* cx, HandleObject wrapper,
                                               AutoIdVector& props) const
{
    return true;
}

bool
OpaqueCrossCompartmentWrapper::delete_(JSContext* cx, HandleObject wrapper, HandleId id,
                                       ObjectOpResult& result) const
{
    return result.succeed();
}

bool
OpaqueCrossCompartmentWrapper::enumerate(JSContext* cx, HandleObject wrapper,
                                         MutableHandleObject objp) const
{
    return BaseProxyHandler::enumerate(cx, wrapper, objp);
}

bool
OpaqueCrossCompartmentWrapper::getPrototype(JSContext* cx, HandleObject proxy,
                                            MutableHandleObject protop) const
{
    protop.set(nullptr);
    return true;
}

bool
OpaqueCrossCompartmentWrapper::setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                                            ObjectOpResult& result) const
{
    return result.succeed();
}

bool
OpaqueCrossCompartmentWrapper::setImmutablePrototype(JSContext* cx, HandleObject proxy,
                                                     bool* succeeded) const
{
    *succeeded = false;
    return true;
}

bool
OpaqueCrossCompartmentWrapper::preventExtensions(JSContext* cx, HandleObject wrapper,
                                                 ObjectOpResult& result) const
{
    return result.failCantPreventExtensions();
}

bool
OpaqueCrossCompartmentWrapper::isExtensible(JSContext* cx, HandleObject wrapper,
                                            bool* extensible) const
{
    *extensible = true;
    return true;
}

bool
OpaqueCrossCompartmentWrapper::has(JSContext* cx, HandleObject wrapper, HandleId id,
                                   bool* bp) const
{
    return BaseProxyHandler::has(cx, wrapper, id, bp);
}

bool
OpaqueCrossCompartmentWrapper::get(JSContext* cx, HandleObject wrapper, HandleValue receiver,
                                   HandleId id, MutableHandleValue vp) const
{
    return BaseProxyHandler::get(cx, wrapper, receiver, id, vp);
}

bool
OpaqueCrossCompartmentWrapper::set(JSContext* cx, HandleObject wrapper, HandleId id,
                                   HandleValue v, HandleValue receiver,
                                   ObjectOpResult& result) const
{
    return BaseProxyHandler::set(cx, wrapper, id, v, receiver, result);
}

bool
OpaqueCrossCompartmentWrapper::call(JSContext* cx, HandleObject wrapper,
                                    const CallArgs& args) const
{
    RootedValue v(cx, ObjectValue(*wrapper));
    ReportIsNotFunction(cx, v);
    return false;
}

bool
OpaqueCrossCompartmentWrapper::construct(JSContext* cx, HandleObject wrapper,
                                         const CallArgs& args) const
{
    RootedValue v(cx, ObjectValue(*wrapper));
    ReportIsNotFunction(cx, v);
    return false;
}

bool
OpaqueCrossCompartmentWrapper::getPropertyDescriptor(JSContext* cx,
                                                     HandleObject wrapper,
                                                     HandleId id,
                                                     MutableHandle<JSPropertyDescriptor> desc) const
{
    return BaseProxyHandler::getPropertyDescriptor(cx, wrapper, id, desc);
}

bool
OpaqueCrossCompartmentWrapper::hasOwn(JSContext* cx, HandleObject wrapper, HandleId id,
                                      bool* bp) const
{
    return BaseProxyHandler::hasOwn(cx, wrapper, id, bp);
}

bool
OpaqueCrossCompartmentWrapper::getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject wrapper,
                                                            AutoIdVector& props) const
{
    return BaseProxyHandler::getOwnEnumerablePropertyKeys(cx, wrapper, props);
}

bool
OpaqueCrossCompartmentWrapper::getBuiltinClass(JSContext* cx, HandleObject wrapper,
                                               ESClassValue* classValue) const
{
    *classValue = ESClass_Other;
    return true;
}

bool
OpaqueCrossCompartmentWrapper::isArray(JSContext* cx, HandleObject obj,
                                       JS::IsArrayAnswer* answer) const
{
    *answer = JS::IsArrayAnswer::NotArray;
    return true;
}

const char*
OpaqueCrossCompartmentWrapper::className(JSContext* cx,
                                         HandleObject proxy) const
{
    return "Opaque";
}

JSString*
OpaqueCrossCompartmentWrapper::fun_toString(JSContext* cx, HandleObject proxy,
                                            unsigned indent) const
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO, js_Function_str,
                         js_toString_str, "object");
    return nullptr;
}

const OpaqueCrossCompartmentWrapper OpaqueCrossCompartmentWrapper::singleton;
