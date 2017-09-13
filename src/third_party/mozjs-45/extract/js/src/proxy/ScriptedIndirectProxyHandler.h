/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef proxy_ScriptedIndirectProxyHandler_h
#define proxy_ScriptedIndirectProxyHandler_h

#include "js/Proxy.h"

namespace js {

/* Derived class for all scripted indirect proxy handlers. */
class ScriptedIndirectProxyHandler : public BaseProxyHandler
{
  public:
    MOZ_CONSTEXPR ScriptedIndirectProxyHandler()
      : BaseProxyHandler(&family)
    { }

    /* Standard internal methods. */
    virtual bool getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                          MutableHandle<JSPropertyDescriptor> desc) const override;
    virtual bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                                Handle<JSPropertyDescriptor> desc,
                                ObjectOpResult& result) const override;
    virtual bool ownPropertyKeys(JSContext* cx, HandleObject proxy,
                                 AutoIdVector& props) const override;
    virtual bool delete_(JSContext* cx, HandleObject proxy, HandleId id,
                         ObjectOpResult& result) const override;
    virtual bool enumerate(JSContext* cx, HandleObject proxy,
                           MutableHandleObject objp) const override;
    virtual bool preventExtensions(JSContext* cx, HandleObject proxy,
                                   ObjectOpResult& result) const override;
    virtual bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const override;
    virtual bool has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const override;
    virtual bool get(JSContext* cx, HandleObject proxy, HandleValue receiver, HandleId id,
                     MutableHandleValue vp) const override;
    virtual bool set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                     HandleValue receiver, ObjectOpResult& result) const override;

    /* SpiderMonkey extensions. */
    virtual bool getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                       MutableHandle<JSPropertyDescriptor> desc) const override;
    virtual bool hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const override;
    virtual bool getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy,
                                              AutoIdVector& props) const override;
    virtual bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            const CallArgs& args) const override;
    virtual JSString* fun_toString(JSContext* cx, HandleObject proxy, unsigned indent) const override;
    virtual bool isScripted() const override { return true; }

    static const char family;
    static const ScriptedIndirectProxyHandler singleton;

private:
    bool derivedSet(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                    HandleValue receiver, ObjectOpResult& result) const;
};

/* Derived class to handle Proxy.createFunction() */
class CallableScriptedIndirectProxyHandler : public ScriptedIndirectProxyHandler
{
  public:
    CallableScriptedIndirectProxyHandler() : ScriptedIndirectProxyHandler() { }
    virtual bool call(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;
    virtual bool construct(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;

    virtual bool isCallable(JSObject* obj) const override {
        return true;
    }
    virtual bool isConstructor(JSObject* obj) const override {
        return true;
    }

    static const CallableScriptedIndirectProxyHandler singleton;
};

bool
proxy_create(JSContext* cx, unsigned argc, Value* vp);

bool
proxy_createFunction(JSContext* cx, unsigned argc, Value* vp);

} /* namespace js */

#endif /* proxy_ScriptedIndirectProxyHandler_h */
