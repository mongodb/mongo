/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ProxyObject_h
#define vm_ProxyObject_h

#include "js/Proxy.h"
#include "js/shadow/Object.h"  // JS::shadow::Object
#include "vm/JSObject.h"

namespace js {

/**
 * This is the base class for the various kinds of proxy objects.  It's never
 * instantiated.
 *
 * Proxy objects use their shape primarily to record flags. Property
 * information, &c. is all dynamically computed.
 *
 * There is no class_ member to force specialization of JSObject::is<T>().
 * The implementation in JSObject is incorrect for proxies since it doesn't
 * take account of the handler type.
 */
class ProxyObject : public JSObject {
  // GetProxyDataLayout computes the address of this field.
  detail::ProxyDataLayout data;

  void static_asserts() {
    static_assert(sizeof(ProxyObject) == sizeof(JSObject_Slots0),
                  "proxy object size must match GC thing size");
    static_assert(offsetof(ProxyObject, data) == detail::ProxyDataOffset,
                  "proxy object layout must match shadow interface");
    static_assert(offsetof(ProxyObject, data.reservedSlots) ==
                      offsetof(JS::shadow::Object, slots),
                  "Proxy reservedSlots must overlay native object slots field");
  }

 public:
  static ProxyObject* New(JSContext* cx, const BaseProxyHandler* handler,
                          HandleValue priv, TaggedProto proto_,
                          const JSClass* clasp);

  void init(const BaseProxyHandler* handler, HandleValue priv, JSContext* cx);

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
    data.reservedSlots =
        &reinterpret_cast<detail::ProxyValueArray*>(inlineDataStart())
             ->reservedSlots;
  }

  // For use from JSObject::swap.
  [[nodiscard]] bool prepareForSwap(JSContext* cx,
                                    MutableHandleValueVector valuesOut);
  [[nodiscard]] bool fixupAfterSwap(JSContext* cx, HandleValueVector values);

  const Value& private_() const { return GetProxyPrivate(this); }
  const Value& expando() const { return GetProxyExpando(this); }

  void setExpando(JSObject* expando);

  void setCrossCompartmentPrivate(const Value& priv);
  void setSameCompartmentPrivate(const Value& priv);

  JSObject* target() const { return private_().toObjectOrNull(); }

  const BaseProxyHandler* handler() const { return GetProxyHandler(this); }

  void setHandler(const BaseProxyHandler* handler) {
    SetProxyHandler(this, handler);
  }

  static size_t offsetOfReservedSlots() {
    return offsetof(ProxyObject, data.reservedSlots);
  }
  static size_t offsetOfHandler() {
    return offsetof(ProxyObject, data.handler);
  }

  size_t numReservedSlots() const { return JSCLASS_RESERVED_SLOTS(getClass()); }
  const Value& reservedSlot(size_t n) const {
    return GetProxyReservedSlot(this, n);
  }

  void setReservedSlot(size_t n, const Value& extra) {
    SetProxyReservedSlot(this, n, extra);
  }

  gc::AllocKind allocKindForTenure() const;

 private:
  GCPtr<Value>* reservedSlotPtr(size_t n) {
    return reinterpret_cast<GCPtr<Value>*>(
        &detail::GetProxyDataLayout(this)->reservedSlots->slots[n]);
  }

  GCPtr<Value>* slotOfPrivate() {
    return reinterpret_cast<GCPtr<Value>*>(
        &detail::GetProxyDataLayout(this)->values()->privateSlot);
  }

  GCPtr<Value>* slotOfExpando() {
    return reinterpret_cast<GCPtr<Value>*>(
        &detail::GetProxyDataLayout(this)->values()->expandoSlot);
  }

  void setPrivate(const Value& priv);

  static bool isValidProxyClass(const JSClass* clasp) {
    // Since we can take classes from the outside, make sure that they
    // are "sane". They have to quack enough like proxies for us to belive
    // they should be treated as such.

    // Proxy classes are not allowed to have call or construct hooks directly.
    // Their callability is instead decided by handler()->isCallable().
    return clasp->isProxyObject() && clasp->isTrace(ProxyObject::trace) &&
           !clasp->getCall() && !clasp->getConstruct();
  }

 public:
  static unsigned grayLinkReservedSlot(JSObject* obj);

  void renew(const BaseProxyHandler* handler, const Value& priv);

  static void trace(JSTracer* trc, JSObject* obj);

  static void traceEdgeToTarget(JSTracer* trc, ProxyObject* obj);

  void nurseryProxyTenured(ProxyObject* old);

  void nuke();
};

bool IsDerivedProxyObject(const JSObject* obj,
                          const js::BaseProxyHandler* handler);

}  // namespace js

template <>
inline bool JSObject::is<js::ProxyObject>() const {
  // Note: this method is implemented in terms of the IsProxy() friend API
  // functions to ensure the implementations are tied together.
  // Note 2: this specialization isn't used for subclasses of ProxyObject
  // which must supply their own implementation.
  return js::IsProxy(this);
}

inline bool js::IsDerivedProxyObject(const JSObject* obj,
                                     const js::BaseProxyHandler* handler) {
  return obj->is<js::ProxyObject>() &&
         obj->as<js::ProxyObject>().handler() == handler;
}

#endif /* vm_ProxyObject_h */
