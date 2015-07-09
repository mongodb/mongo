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

class RegExpGuard;

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
                                         MutableHandle<JSPropertyDescriptor> desc);
    static bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                               MutableHandle<JSPropertyDescriptor> desc);
    static bool ownPropertyKeys(JSContext* cx, HandleObject proxy, AutoIdVector& props);
    static bool delete_(JSContext* cx, HandleObject proxy, HandleId id, bool* bp);
    static bool enumerate(JSContext* cx, HandleObject proxy, MutableHandleObject objp);
    static bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible);
    static bool preventExtensions(JSContext* cx, HandleObject proxy, bool* succeeded);
    static bool getPrototypeOf(JSContext* cx, HandleObject proxy, MutableHandleObject protop);
    static bool setPrototypeOf(JSContext* cx, HandleObject proxy, HandleObject proto, bool* bp);
    static bool setImmutablePrototype(JSContext* cx, HandleObject proxy, bool* succeeded);
    static bool has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp);
    static bool get(JSContext* cx, HandleObject proxy, HandleObject receiver, HandleId id,
                    MutableHandleValue vp);
    static bool set(JSContext* cx, HandleObject proxy, HandleObject receiver, HandleId id,
                    bool strict, MutableHandleValue vp);
    static bool call(JSContext* cx, HandleObject proxy, const CallArgs& args);
    static bool construct(JSContext* cx, HandleObject proxy, const CallArgs& args);

    /* SpiderMonkey extensions. */
    static bool getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                      MutableHandle<JSPropertyDescriptor> desc);
    static bool hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp);
    static bool getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy,
                                             AutoIdVector& props);
    static bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl, CallArgs args);
    static bool hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v, bool* bp);
    static bool objectClassIs(HandleObject obj, ESClassValue classValue, JSContext* cx);
    static const char* className(JSContext* cx, HandleObject proxy);
    static JSString* fun_toString(JSContext* cx, HandleObject proxy, unsigned indent);
    static bool regexp_toShared(JSContext* cx, HandleObject proxy, RegExpGuard* g);
    static bool boxedValue_unbox(JSContext* cx, HandleObject proxy, MutableHandleValue vp);
    static bool defaultValue(JSContext* cx, HandleObject obj, JSType hint, MutableHandleValue vp);

    static bool watch(JSContext* cx, HandleObject proxy, HandleId id, HandleObject callable);
    static bool unwatch(JSContext* cx, HandleObject proxy, HandleId id);

    static bool getElements(JSContext* cx, HandleObject obj, uint32_t begin, uint32_t end,
                            ElementAdder* adder);

    static void trace(JSTracer* trc, JSObject* obj);

    /* IC entry path for handling __noSuchMethod__ on access. */
    static bool callProp(JSContext* cx, HandleObject proxy, HandleObject reveiver, HandleId id,
                         MutableHandleValue vp);
};

} /* namespace js */

#endif /* proxy_Proxy_h */
