/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ProxyObject_h
#define vm_ProxyObject_h

#include "js/Proxy.h"
#include "vm/ShapedObject.h"

namespace js {

/**
 * This is the base class for the various kinds of proxy objects.  It's never
 * instantiated.
 *
 * Proxy objects use ShapedObject::shape_ primarily to record flags.  Property
 * information, &c. is all dynamically computed.
 */
class ProxyObject : public ShapedObject
{
    // GetProxyDataLayout computes the address of this field.
    detail::ProxyDataLayout data;

    void static_asserts() {
        static_assert(sizeof(ProxyObject) == sizeof(JSObject_Slots0),
                      "proxy object size must match GC thing size");
        static_assert(offsetof(ProxyObject, data) == detail::ProxyDataOffset,
                      "proxy object layout must match shadow interface");
        static_assert(offsetof(ProxyObject, data.reservedSlots) == offsetof(shadow::Object, slots),
                      "Proxy reservedSlots must overlay native object slots field");
    }

    static JS::Result<ProxyObject*, JS::OOM&>
    create(JSContext* cx, const js::Class* clasp, Handle<TaggedProto> proto,
           js::gc::AllocKind allocKind, js::NewObjectKind newKind);

  public:
    static ProxyObject* New(JSContext* cx, const BaseProxyHandler* handler, HandleValue priv,
                            TaggedProto proto_, const ProxyOptions& options);

    // Proxies usually store their ProxyValueArray inline in the object.
    // There's one unfortunate exception: when a proxy is swapped with another
    // object, and the sizes don't match, we malloc the ProxyValueArray.
    void* inlineDataStart() const {
        return (void*)(uintptr_t(this) + sizeof(ProxyObject));
    }
    bool usingInlineValueArray() const {
        return data.values() == inlineDataStart();
    }
    void setInlineValueArray() {
        data.reservedSlots = &reinterpret_cast<detail::ProxyValueArray*>(inlineDataStart())->reservedSlots;
    }
    MOZ_MUST_USE bool initExternalValueArrayAfterSwap(JSContext* cx, const Vector<Value>& values);

    const Value& private_() {
        return GetProxyPrivate(this);
    }

    void setCrossCompartmentPrivate(const Value& priv);
    void setSameCompartmentPrivate(const Value& priv);

    JSObject* target() const {
        return const_cast<ProxyObject*>(this)->private_().toObjectOrNull();
    }

    const BaseProxyHandler* handler() const {
        return GetProxyHandler(const_cast<ProxyObject*>(this));
    }

    void setHandler(const BaseProxyHandler* handler) {
        SetProxyHandler(this, handler);
    }

    static size_t offsetOfReservedSlots() {
        return offsetof(ProxyObject, data.reservedSlots);
    }
    static size_t offsetOfHandler() {
        return offsetof(ProxyObject, data.handler);
    }

    size_t numReservedSlots() const {
        return JSCLASS_RESERVED_SLOTS(getClass());
    }
    const Value& reservedSlot(size_t n) const {
        return GetProxyReservedSlot(const_cast<ProxyObject*>(this), n);
    }

    void setReservedSlot(size_t n, const Value& extra) {
        SetProxyReservedSlot(this, n, extra);
    }

    gc::AllocKind allocKindForTenure() const;

  private:
    GCPtrValue* reservedSlotPtr(size_t n) {
        return reinterpret_cast<GCPtrValue*>(&detail::GetProxyDataLayout(this)->reservedSlots->slots[n]);
    }

    GCPtrValue* slotOfPrivate() {
        return reinterpret_cast<GCPtrValue*>(&detail::GetProxyDataLayout(this)->values()->privateSlot);
    }

    void setPrivate(const Value& priv);

    static bool isValidProxyClass(const Class* clasp) {
        // Since we can take classes from the outside, make sure that they
        // are "sane". They have to quack enough like proxies for us to belive
        // they should be treated as such.

        // Proxy classes are not allowed to have call or construct hooks directly. Their
        // callability is instead decided by handler()->isCallable().
        return clasp->isProxy() &&
               clasp->isTrace(ProxyObject::trace) &&
               !clasp->getCall() && !clasp->getConstruct();
    }

  public:
    static unsigned grayLinkReservedSlot(JSObject* obj);

    void renew(const BaseProxyHandler* handler, const Value& priv);

    static void trace(JSTracer* trc, JSObject* obj);

    static void traceEdgeToTarget(JSTracer* trc, ProxyObject* obj);

    void nuke();

    // There is no class_ member to force specialization of JSObject::is<T>().
    // The implementation in JSObject is incorrect for proxies since it doesn't
    // take account of the handler type.
    static const Class proxyClass;
};

inline bool
IsProxyClass(const Class* clasp)
{
    return clasp->isProxy();
}

bool IsDerivedProxyObject(const JSObject* obj, const js::BaseProxyHandler* handler);

} // namespace js

template<>
inline bool
JSObject::is<js::ProxyObject>() const
{
    // Note: this method is implemented in terms of the IsProxy() friend API
    // functions to ensure the implementations are tied together.
    // Note 2: this specialization isn't used for subclasses of ProxyObject
    // which must supply their own implementation.
    return js::IsProxy(const_cast<JSObject*>(this));
}

inline bool
js::IsDerivedProxyObject(const JSObject* obj, const js::BaseProxyHandler* handler) {
    return obj->is<js::ProxyObject>() && obj->as<js::ProxyObject>().handler() == handler;
}

#endif /* vm_ProxyObject_h */
