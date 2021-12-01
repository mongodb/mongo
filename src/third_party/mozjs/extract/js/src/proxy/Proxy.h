/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef proxy_Proxy_h
#define proxy_Proxy_h

#include "NamespaceImports.h"

#include "js/Class.h"

namespace js {

/*
 * Dispatch point for handlers that executes the appropriate C++ or scripted traps.
 *
 * Important: All proxy methods need either (a) an AutoEnterPolicy in their
 * Proxy::foo entry point below or (b) an override in SecurityWrapper. See bug
 * 945826 comment 0.
 */
class Proxy
{
  public:
    /* Standard internal methods. */
    static bool getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                         MutableHandle<JS::PropertyDescriptor> desc);
    static bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                               Handle<JS::PropertyDescriptor> desc, ObjectOpResult& result);
    static bool ownPropertyKeys(JSContext* cx, HandleObject proxy, AutoIdVector& props);
    static bool delete_(JSContext* cx, HandleObject proxy, HandleId id, ObjectOpResult& result);
    static JSObject* enumerate(JSContext* cx, HandleObject proxy);
    static bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible);
    static bool preventExtensions(JSContext* cx, HandleObject proxy, ObjectOpResult& result);
    static bool getPrototype(JSContext* cx, HandleObject proxy, MutableHandleObject protop);
    static bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                             ObjectOpResult& result);
    static bool getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy, bool* isOrdinary,
                                       MutableHandleObject protop);
    static bool setImmutablePrototype(JSContext* cx, HandleObject proxy, bool* succeeded);
    static bool has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp);
    static bool get(JSContext* cx, HandleObject proxy, HandleValue receiver, HandleId id,
                    MutableHandleValue vp);
    static bool getInternal(JSContext* cx, HandleObject proxy, HandleValue receiver,
                            HandleId id, MutableHandleValue vp);
    static bool set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                    HandleValue receiver, ObjectOpResult& result);
    static bool setInternal(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                            HandleValue receiver, ObjectOpResult& result);
    static bool call(JSContext* cx, HandleObject proxy, const CallArgs& args);
    static bool construct(JSContext* cx, HandleObject proxy, const CallArgs& args);

    /* SpiderMonkey extensions. */
    static bool getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                      MutableHandle<JS::PropertyDescriptor> desc);
    static bool hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp);
    static bool getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy,
                                             AutoIdVector& props);
    static bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                           const CallArgs& args);
    static bool hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v, bool* bp);
    static bool getBuiltinClass(JSContext* cx, HandleObject proxy, ESClass* cls);
    static bool isArray(JSContext* cx, HandleObject proxy, JS::IsArrayAnswer* answer);
    static const char* className(JSContext* cx, HandleObject proxy);
    static JSString* fun_toString(JSContext* cx, HandleObject proxy, bool isToSource);
    static RegExpShared* regexp_toShared(JSContext* cx, HandleObject proxy);
    static bool boxedValue_unbox(JSContext* cx, HandleObject proxy, MutableHandleValue vp);

    static bool getElements(JSContext* cx, HandleObject obj, uint32_t begin, uint32_t end,
                            ElementAdder* adder);

    static void trace(JSTracer* trc, JSObject* obj);
};

bool
proxy_Call(JSContext* cx, unsigned argc, Value* vp);
bool
proxy_Construct(JSContext* cx, unsigned argc, Value* vp);
size_t
proxy_ObjectMoved(JSObject* obj, JSObject* old);

// These functions are used by JIT code

bool
ProxyHas(JSContext* cx, HandleObject proxy, HandleValue idVal, MutableHandleValue result);

bool
ProxyHasOwn(JSContext* cx, HandleObject proxy, HandleValue idVal, MutableHandleValue result);

bool
ProxyGetProperty(JSContext* cx, HandleObject proxy, HandleId id, MutableHandleValue vp);

bool
ProxyGetPropertyByValue(JSContext* cx, HandleObject proxy, HandleValue idVal,
                        MutableHandleValue vp);

bool
ProxySetProperty(JSContext* cx, HandleObject proxy, HandleId id, HandleValue val, bool strict);

bool
ProxySetPropertyByValue(JSContext* cx, HandleObject proxy, HandleValue idVal, HandleValue val,
                        bool strict);

} /* namespace js */

#endif /* proxy_Proxy_h */
