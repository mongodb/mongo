/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ProxyObject_h
#define vm_ProxyObject_h

#include "js/Proxy.h"
#include "vm/NativeObject.h"

namespace js {

// This is the base class for the various kinds of proxy objects.  It's never
// instantiated.
class ProxyObject : public JSObject
{
    // GetProxyDataLayout computes the address of this field.
    ProxyDataLayout data;

    void static_asserts() {
        static_assert(sizeof(ProxyObject) == sizeof(JSObject_Slots0),
                      "proxy object size must match GC thing size");
        static_assert(offsetof(ProxyObject, data) == ProxyDataOffset,
                      "proxy object layout must match shadow interface");
    }

  public:
    static ProxyObject* New(JSContext* cx, const BaseProxyHandler* handler, HandleValue priv,
                            TaggedProto proto_, JSObject* parent_,
                            const ProxyOptions& options);

    const Value& private_() {
        return GetProxyPrivate(this);
    }

    void setCrossCompartmentPrivate(const Value& priv);
    void setSameCompartmentPrivate(const Value& priv);

    HeapValue* slotOfPrivate() {
        return reinterpret_cast<HeapValue*>(&GetProxyDataLayout(this)->values->privateSlot);
    }

    JSObject* target() const {
        return const_cast<ProxyObject*>(this)->private_().toObjectOrNull();
    }

    const BaseProxyHandler* handler() const {
        return GetProxyHandler(const_cast<ProxyObject*>(this));
    }

    void setHandler(const BaseProxyHandler* handler) {
        SetProxyHandler(this, handler);
    }

    static size_t offsetOfValues() {
        return offsetof(ProxyObject, data.values);
    }
    static size_t offsetOfHandler() {
        return offsetof(ProxyObject, data.handler);
    }
    static size_t offsetOfExtraSlotInValues(size_t slot) {
        MOZ_ASSERT(slot < PROXY_EXTRA_SLOTS);
        return offsetof(ProxyValueArray, extraSlots) + slot * sizeof(Value);
    }

    const Value& extra(size_t n) const {
        return GetProxyExtra(const_cast<ProxyObject*>(this), n);
    }

    void setExtra(size_t n, const Value& extra) {
        SetProxyExtra(this, n, extra);
    }

  private:
    HeapValue* slotOfExtra(size_t n) {
        MOZ_ASSERT(n < PROXY_EXTRA_SLOTS);
        return reinterpret_cast<HeapValue*>(&GetProxyDataLayout(this)->values->extraSlots[n]);
    }

    static bool isValidProxyClass(const Class* clasp) {
        // Since we can take classes from the outside, make sure that they
        // are "sane". They have to quack enough like proxies for us to belive
        // they should be treated as such.

        // proxy_Trace is just a trivial wrapper around ProxyObject::trace for
        // friend api exposure.

        // Proxy classes are not allowed to have call or construct hooks directly. Their
        // callability is instead decided by handler()->isCallable().
        return clasp->isProxy() &&
               (clasp->flags & JSCLASS_IMPLEMENTS_BARRIERS) &&
               clasp->trace == proxy_Trace &&
               !clasp->call && !clasp->construct;
    }

  public:
    static unsigned grayLinkExtraSlot(JSObject* obj);

    void renew(JSContext* cx, const BaseProxyHandler* handler, Value priv);

    static void trace(JSTracer* trc, JSObject* obj);

    void nuke(const BaseProxyHandler* handler);

    static const Class class_;
};

} // namespace js

// Note: the following |JSObject::is<T>| methods are implemented in terms of
// the Is*Proxy() friend API functions to ensure the implementations are tied
// together.  The exception is |JSObject::is<js::OuterWindowProxyObject>()
// const|, which uses the standard template definition, because there is no
// IsOuterWindowProxy() function in the friend API.

template<>
inline bool
JSObject::is<js::ProxyObject>() const
{
    return js::IsProxy(const_cast<JSObject*>(this));
}

#endif /* vm_ProxyObject_h */
