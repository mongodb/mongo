/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef proxy_ScriptedProxyHandler_h
#define proxy_ScriptedProxyHandler_h

#include "js/Proxy.h"

namespace js {

/* Derived class for all scripted proxy handlers. */
class ScriptedProxyHandler : public BaseProxyHandler
{
  public:
    constexpr ScriptedProxyHandler()
      : BaseProxyHandler(&family)
    { }

    /* Standard internal methods. */
    virtual bool getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc) const override;
    virtual bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                                Handle<PropertyDescriptor> desc,
                                ObjectOpResult& result) const override;
    virtual bool ownPropertyKeys(JSContext* cx, HandleObject proxy,
                                 AutoIdVector& props) const override;
    virtual bool delete_(JSContext* cx, HandleObject proxy, HandleId id,
                         ObjectOpResult& result) const override;

    virtual bool getPrototype(JSContext* cx, HandleObject proxy,
                              MutableHandleObject protop) const override;
    virtual bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                              ObjectOpResult& result) const override;
    /* Non-standard, but needed to correctly implement OrdinaryGetPrototypeOf. */
    virtual bool getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy, bool* isOrdinary,
                                       MutableHandleObject protop) const override;
    /* Non-standard, but needed to handle revoked proxies. */
    virtual bool setImmutablePrototype(JSContext* cx, HandleObject proxy,
                                       bool* succeeded) const override;

    virtual bool preventExtensions(JSContext* cx, HandleObject proxy,
                                   ObjectOpResult& result) const override;
    virtual bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const override;

    virtual bool has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const override;
    virtual bool get(JSContext* cx, HandleObject proxy, HandleValue receiver, HandleId id,
                     MutableHandleValue vp) const override;
    virtual bool set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                     HandleValue receiver, ObjectOpResult& result) const override;
    virtual bool call(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;
    virtual bool construct(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;

    /* SpiderMonkey extensions. */
    virtual bool hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const override {
        return BaseProxyHandler::hasOwn(cx, proxy, id, bp);
    }

    // A scripted proxy should not be treated as generic in most contexts.
    virtual bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            const CallArgs& args) const override;
    virtual bool hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v,
                             bool* bp) const override;
    virtual bool getBuiltinClass(JSContext* cx, HandleObject proxy, ESClass* cls) const override;
    virtual bool isArray(JSContext* cx, HandleObject proxy,
                         JS::IsArrayAnswer* answer) const override;
    virtual const char* className(JSContext* cx, HandleObject proxy) const override;
    virtual JSString* fun_toString(JSContext* cx, HandleObject proxy,
                                   bool isToSource) const override;
    virtual RegExpShared* regexp_toShared(JSContext* cx, HandleObject proxy) const override;
    virtual bool boxedValue_unbox(JSContext* cx, HandleObject proxy,
                                  MutableHandleValue vp) const override;

    virtual bool isCallable(JSObject* obj) const override;
    virtual bool isConstructor(JSObject* obj) const override;

    virtual bool isScripted() const override { return true; }

    static const char family;
    static const ScriptedProxyHandler singleton;

    // The "proxy extra" slot index in which the handler is stored. Revocable proxies need to set
    // this at revocation time.
    static const int HANDLER_EXTRA = 0;
    static const int IS_CALLCONSTRUCT_EXTRA = 1;
    // Bitmasks for the "call/construct" slot
    static const int IS_CALLABLE    = 1 << 0;
    static const int IS_CONSTRUCTOR = 1 << 1;
    // The "function extended" slot index in which the revocation object is stored. Per spec, this
    // is to be cleared during the first revocation.
    static const int REVOKE_SLOT = 0;

    static JSObject* handlerObject(const JSObject* proxy);
};

bool
proxy(JSContext* cx, unsigned argc, Value* vp);

bool
proxy_revocable(JSContext* cx, unsigned argc, Value* vp);

} /* namespace js */

#endif /* proxy_ScriptedProxyHandler_h */
