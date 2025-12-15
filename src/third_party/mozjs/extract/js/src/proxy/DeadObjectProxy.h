/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef proxy_DeadObjectProxy_h
#define proxy_DeadObjectProxy_h

#include "js/Proxy.h"

namespace js {

class ProxyObject;

enum DeadObjectProxyFlags {
  DeadObjectProxyIsCallable = 1 << 0,
  DeadObjectProxyIsConstructor = 1 << 1,
  DeadObjectProxyIsBackgroundFinalized = 1 << 2
};

class DeadObjectProxy : public NurseryAllocableProxyHandler {
 public:
  explicit constexpr DeadObjectProxy()
      : NurseryAllocableProxyHandler(&family) {}

  /* Standard internal methods. */
  virtual bool getOwnPropertyDescriptor(
      JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
      JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc)
      const override;
  virtual bool defineProperty(JSContext* cx, JS::HandleObject wrapper,
                              JS::HandleId id,
                              JS::Handle<JS::PropertyDescriptor> desc,
                              JS::ObjectOpResult& result) const override;
  virtual bool ownPropertyKeys(JSContext* cx, JS::HandleObject wrapper,
                               JS::MutableHandleIdVector props) const override;
  virtual bool delete_(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                       JS::ObjectOpResult& result) const override;
  virtual bool getPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::MutableHandleObject protop) const override;
  virtual bool getPrototypeIfOrdinary(
      JSContext* cx, JS::HandleObject proxy, bool* isOrdinary,
      JS::MutableHandleObject protop) const override;
  virtual bool preventExtensions(JSContext* cx, JS::HandleObject proxy,
                                 JS::ObjectOpResult& result) const override;
  virtual bool isExtensible(JSContext* cx, JS::HandleObject proxy,
                            bool* extensible) const override;
  virtual bool call(JSContext* cx, JS::HandleObject proxy,
                    const JS::CallArgs& args) const override;
  virtual bool construct(JSContext* cx, JS::HandleObject proxy,
                         const JS::CallArgs& args) const override;

  /* SpiderMonkey extensions. */
  // BaseProxyHandler::enumerate will throw by calling ownKeys.
  virtual bool nativeCall(JSContext* cx, JS::IsAcceptableThis test,
                          JS::NativeImpl impl,
                          const JS::CallArgs& args) const override;
  virtual bool getBuiltinClass(JSContext* cx, JS::HandleObject proxy,
                               ESClass* cls) const override;
  virtual bool isArray(JSContext* cx, JS::HandleObject proxy,
                       JS::IsArrayAnswer* answer) const override;
  virtual const char* className(JSContext* cx,
                                JS::HandleObject proxy) const override;
  virtual JSString* fun_toString(JSContext* cx, JS::HandleObject proxy,
                                 bool isToSource) const override;
  virtual RegExpShared* regexp_toShared(JSContext* cx,
                                        JS::HandleObject proxy) const override;

  // Use the regular paths for private values
  virtual bool useProxyExpandoObjectForPrivateFields() const override {
    return false;
  }

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
  static int32_t flags(JSObject* obj) { return GetProxyPrivate(obj).toInt32(); }
};

bool IsDeadProxyObject(const JSObject* obj);

JS::Value DeadProxyTargetValue(JSObject* obj);

JSObject* NewDeadProxyObject(JSContext* cx, JSObject* origObj = nullptr);

} /* namespace js */

#endif /* proxy_DeadObjectProxy_h */
