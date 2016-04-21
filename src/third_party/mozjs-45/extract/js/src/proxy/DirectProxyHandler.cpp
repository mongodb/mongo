/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jswrapper.h" // UncheckedUnwrap

#include "js/Proxy.h"
#include "vm/ProxyObject.h"

#include "jsobjinlines.h"

using namespace js;

bool
DirectProxyHandler::getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc) const
{
    assertEnteredPolicy(cx, proxy, id, GET | SET | GET_PROPERTY_DESCRIPTOR);
    MOZ_ASSERT(!hasPrototype()); // Should never be called if there's a prototype.
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return GetPropertyDescriptor(cx, target, id, desc);
}

bool
DirectProxyHandler::getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                             MutableHandle<PropertyDescriptor> desc) const
{
    assertEnteredPolicy(cx, proxy, id, GET | SET | GET_PROPERTY_DESCRIPTOR);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return GetOwnPropertyDescriptor(cx, target, id, desc);
}

bool
DirectProxyHandler::defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                                   Handle<PropertyDescriptor> desc,
                                   ObjectOpResult& result) const
{
    assertEnteredPolicy(cx, proxy, id, SET);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return DefineProperty(cx, target, id, desc, result);
}

bool
DirectProxyHandler::ownPropertyKeys(JSContext* cx, HandleObject proxy,
                                    AutoIdVector& props) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, ENUMERATE);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return GetPropertyKeys(cx, target, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &props);
}

bool
DirectProxyHandler::delete_(JSContext* cx, HandleObject proxy, HandleId id,
                            ObjectOpResult& result) const
{
    assertEnteredPolicy(cx, proxy, id, SET);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return DeleteProperty(cx, target, id, result);
}

bool
DirectProxyHandler::enumerate(JSContext* cx, HandleObject proxy, MutableHandleObject objp) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, ENUMERATE);
    MOZ_ASSERT(!hasPrototype()); // Should never be called if there's a prototype.
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return GetIterator(cx, target, 0, objp);
}

bool
DirectProxyHandler::call(JSContext* cx, HandleObject proxy, const CallArgs& args) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, CALL);
    RootedValue target(cx, proxy->as<ProxyObject>().private_());
    return Invoke(cx, args.thisv(), target, args.length(), args.array(), args.rval());
}

bool
DirectProxyHandler::construct(JSContext* cx, HandleObject proxy, const CallArgs& args) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, CALL);

    RootedValue target(cx, proxy->as<ProxyObject>().private_());
    if (!IsConstructor(target)) {
        ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, target, nullptr);
        return false;
    }

    ConstructArgs cargs(cx);
    if (!FillArgumentsFromArraylike(cx, cargs, args))
        return false;

    return Construct(cx, target, cargs, args.newTarget(), args.rval());
}

bool
DirectProxyHandler::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                               const CallArgs& args) const
{
    args.setThis(ObjectValue(*args.thisv().toObject().as<ProxyObject>().target()));
    if (!test(args.thisv())) {
        ReportIncompatible(cx, args);
        return false;
    }

    return CallNativeImpl(cx, impl, args);
}

bool
DirectProxyHandler::hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v,
                                bool* bp) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, GET);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return HasInstance(cx, target, v, bp);
}

bool
DirectProxyHandler::getPrototype(JSContext* cx, HandleObject proxy, MutableHandleObject protop) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return GetPrototype(cx, target, protop);
}

bool
DirectProxyHandler::setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                                 ObjectOpResult& result) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return SetPrototype(cx, target, proto, result);
}

bool
DirectProxyHandler::setImmutablePrototype(JSContext* cx, HandleObject proxy, bool* succeeded) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return SetImmutablePrototype(cx, target, succeeded);
}

bool
DirectProxyHandler::preventExtensions(JSContext* cx, HandleObject proxy, ObjectOpResult& result) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return PreventExtensions(cx, target, result);
}

bool
DirectProxyHandler::isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return IsExtensible(cx, target, extensible);
}

bool
DirectProxyHandler::getBuiltinClass(JSContext* cx, HandleObject proxy,
                                    ESClassValue* classValue) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return GetBuiltinClass(cx, target, classValue);
}

bool
DirectProxyHandler::isArray(JSContext* cx, HandleObject proxy, JS::IsArrayAnswer* answer) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return IsArray(cx, target, answer);
}

const char*
DirectProxyHandler::className(JSContext* cx, HandleObject proxy) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, GET);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return GetObjectClassName(cx, target);
}

JSString*
DirectProxyHandler::fun_toString(JSContext* cx, HandleObject proxy,
                                 unsigned indent) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, GET);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return fun_toStringHelper(cx, target, indent);
}

bool
DirectProxyHandler::regexp_toShared(JSContext* cx, HandleObject proxy,
                                    RegExpGuard* g) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return RegExpToShared(cx, target, g);
}

bool
DirectProxyHandler::boxedValue_unbox(JSContext* cx, HandleObject proxy, MutableHandleValue vp) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return Unbox(cx, target, vp);
}

JSObject*
DirectProxyHandler::weakmapKeyDelegate(JSObject* proxy) const
{
    return UncheckedUnwrap(proxy);
}

bool
DirectProxyHandler::has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const
{
    assertEnteredPolicy(cx, proxy, id, GET);
    MOZ_ASSERT(!hasPrototype()); // Should never be called if there's a prototype.
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return HasProperty(cx, target, id, bp);
}

bool
DirectProxyHandler::hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const
{
    assertEnteredPolicy(cx, proxy, id, GET);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return HasOwnProperty(cx, target, id, bp);
}

bool
DirectProxyHandler::get(JSContext* cx, HandleObject proxy, HandleValue receiver,
                        HandleId id, MutableHandleValue vp) const
{
    assertEnteredPolicy(cx, proxy, id, GET);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return GetProperty(cx, target, receiver, id, vp);
}

bool
DirectProxyHandler::set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                        HandleValue receiver, ObjectOpResult& result) const
{
    assertEnteredPolicy(cx, proxy, id, SET);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return SetProperty(cx, target, id, v, receiver, result);
}

bool
DirectProxyHandler::getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy,
                                                 AutoIdVector& props) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, ENUMERATE);
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    return GetPropertyKeys(cx, target, JSITER_OWNONLY, &props);
}

bool
DirectProxyHandler::isCallable(JSObject* obj) const
{
    JSObject * target = obj->as<ProxyObject>().target();
    return target->isCallable();
}
