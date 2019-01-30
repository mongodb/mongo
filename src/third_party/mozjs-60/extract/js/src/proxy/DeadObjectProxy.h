/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef proxy_DeadObjectProxy_h
#define proxy_DeadObjectProxy_h

#include "js/Proxy.h"

namespace js {

class ProxyObject;

enum DeadObjectProxyFlags
{
    DeadObjectProxyIsCallable            = 1 << 0,
    DeadObjectProxyIsConstructor         = 1 << 1,
    DeadObjectProxyIsBackgroundFinalized = 1 << 2
};

class DeadObjectProxy : public BaseProxyHandler
{
  public:
    explicit constexpr DeadObjectProxy()
      : BaseProxyHandler(&family)
    { }

    /* Standard internal methods. */
    virtual bool getOwnPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc) const override;
    virtual bool defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                Handle<PropertyDescriptor> desc,
                                ObjectOpResult& result) const override;
    virtual bool ownPropertyKeys(JSContext* cx, HandleObject wrapper,
                                 AutoIdVector& props) const override;
    virtual bool delete_(JSContext* cx, HandleObject wrapper, HandleId id,
                         ObjectOpResult& result) const override;
    virtual bool getPrototype(JSContext* cx, HandleObject proxy,
                              MutableHandleObject protop) const override;
    virtual bool getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy, bool* isOrdinary,
                                        MutableHandleObject protop) const override;
    virtual bool preventExtensions(JSContext* cx, HandleObject proxy,
                                   ObjectOpResult& result) const override;
    virtual bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const override;
    virtual bool call(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;
    virtual bool construct(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;

    /* SpiderMonkey extensions. */
    // BaseProxyHandler::getPropertyDescriptor will throw by calling getOwnPropertyDescriptor.
    // BaseProxyHandler::enumerate will throw by calling ownKeys.
    virtual bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            const CallArgs& args) const override;
    virtual bool hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v,
                             bool* bp) const override;
    virtual bool getBuiltinClass(JSContext* cx, HandleObject proxy, ESClass* cls) const override;
    virtual bool isArray(JSContext* cx, HandleObject proxy, JS::IsArrayAnswer* answer) const override;
    virtual const char* className(JSContext* cx, HandleObject proxy) const override;
    virtual JSString* fun_toString(JSContext* cx, HandleObject proxy,
                                   bool isToSource) const override;
    virtual RegExpShared* regexp_toShared(JSContext* cx, HandleObject proxy) const override;

    virtual bool isCallable(JSObject* obj) const override {
        return flags(obj) & DeadObjectProxyIsCallable;
    }
    virtual bool isConstructor(JSObject* obj) const override {
        return flags(obj) & DeadObjectProxyIsConstructor;
    }

    virtual bool finalizeInBackground(const JS::Value& priv) const override {
        return priv.toInt32() & DeadObjectProxyIsBackgroundFinalized;
    }

    static const DeadObjectProxy singleton;
    static const char family;

  private:
    static int32_t flags(JSObject* obj) {
        return GetProxyPrivate(obj).toInt32();
    }
};

bool
IsDeadProxyObject(JSObject* obj);

Value
DeadProxyTargetValue(ProxyObject* obj);

JSObject*
NewDeadProxyObject(JSContext* cx, JSObject* origObj = nullptr);

} /* namespace js */

#endif /* proxy_DeadObjectProxy_h */
