/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef proxy_DeadObjectProxy_h
#define proxy_DeadObjectProxy_h

#include "js/Proxy.h"

namespace js {

class DeadObjectProxy : public BaseProxyHandler
{
  public:
    explicit MOZ_CONSTEXPR DeadObjectProxy()
      : BaseProxyHandler(&family)
    { }

    /* Standard internal methods. */
    virtual bool getOwnPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                          MutableHandle<JSPropertyDescriptor> desc) const override;
    virtual bool defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                MutableHandle<JSPropertyDescriptor> desc) const override;
    virtual bool ownPropertyKeys(JSContext* cx, HandleObject wrapper,
                                 AutoIdVector& props) const override;
    virtual bool delete_(JSContext* cx, HandleObject wrapper, HandleId id, bool* bp) const override;
    virtual bool enumerate(JSContext* cx, HandleObject wrapper, MutableHandleObject objp) const override;
    virtual bool getPrototypeOf(JSContext* cx, HandleObject proxy,
                                MutableHandleObject protop) const override;
    virtual bool preventExtensions(JSContext* cx, HandleObject proxy, bool* succeeded) const override;
    virtual bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const override;
    virtual bool call(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;
    virtual bool construct(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;

    /* SpiderMonkey extensions. */
    virtual bool getPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                       MutableHandle<JSPropertyDescriptor> desc) const override;
    virtual bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            CallArgs args) const override;
    virtual bool hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v,
                             bool* bp) const override;
    virtual bool objectClassIs(HandleObject obj, ESClassValue classValue,
                               JSContext* cx) const override;
    virtual const char* className(JSContext* cx, HandleObject proxy) const override;
    virtual JSString* fun_toString(JSContext* cx, HandleObject proxy, unsigned indent) const override;
    virtual bool regexp_toShared(JSContext* cx, HandleObject proxy, RegExpGuard* g) const override;
    virtual bool defaultValue(JSContext* cx, HandleObject obj, JSType hint,
                              MutableHandleValue vp) const override;

    static const char family;
    static const DeadObjectProxy singleton;
};

bool
IsDeadProxyObject(JSObject* obj);

} /* namespace js */

#endif /* proxy_DeadObjectProxy_h */
