/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef proxy_ScriptedProxyHandler_h
#define proxy_ScriptedProxyHandler_h

#include "js/Proxy.h"

namespace js {

/* Derived class for all scripted proxy handlers. */
class ScriptedProxyHandler : public NurseryAllocableProxyHandler {
 public:
  enum class GetTrapValidationResult {
    OK,
    MustReportSameValue,
    MustReportUndefined,
    Exception,
  };

  constexpr ScriptedProxyHandler() : NurseryAllocableProxyHandler(&family) {}

  /* Standard internal methods. */
  virtual bool getOwnPropertyDescriptor(
      JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
      JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc)
      const override;
  virtual bool defineProperty(JSContext* cx, JS::HandleObject proxy,
                              JS::HandleId id,
                              JS::Handle<JS::PropertyDescriptor> desc,
                              JS::ObjectOpResult& result) const override;
  virtual bool ownPropertyKeys(JSContext* cx, JS::HandleObject proxy,
                               JS::MutableHandleIdVector props) const override;
  virtual bool delete_(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                       JS::ObjectOpResult& result) const override;

  virtual bool getPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::MutableHandleObject protop) const override;
  virtual bool setPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::HandleObject proto,
                            JS::ObjectOpResult& result) const override;
  /* Non-standard, but needed to correctly implement OrdinaryGetPrototypeOf. */
  virtual bool getPrototypeIfOrdinary(
      JSContext* cx, JS::HandleObject proxy, bool* isOrdinary,
      JS::MutableHandleObject protop) const override;
  /* Non-standard, but needed to handle revoked proxies. */
  virtual bool setImmutablePrototype(JSContext* cx, JS::HandleObject proxy,
                                     bool* succeeded) const override;

  virtual bool preventExtensions(JSContext* cx, JS::HandleObject proxy,
                                 JS::ObjectOpResult& result) const override;
  virtual bool isExtensible(JSContext* cx, JS::HandleObject proxy,
                            bool* extensible) const override;

  virtual bool has(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                   bool* bp) const override;
  virtual bool get(JSContext* cx, JS::HandleObject proxy,
                   JS::HandleValue receiver, JS::HandleId id,
                   JS::MutableHandleValue vp) const override;
  virtual bool set(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                   JS::HandleValue v, JS::HandleValue receiver,
                   JS::ObjectOpResult& result) const override;
  virtual bool call(JSContext* cx, JS::HandleObject proxy,
                    const JS::CallArgs& args) const override;
  virtual bool construct(JSContext* cx, JS::HandleObject proxy,
                         const JS::CallArgs& args) const override;

  /* SpiderMonkey extensions. */
  virtual bool hasOwn(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                      bool* bp) const override {
    return BaseProxyHandler::hasOwn(cx, proxy, id, bp);
  }

  // A scripted proxy should not be treated as generic in most contexts.
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
  virtual bool boxedValue_unbox(JSContext* cx, JS::HandleObject proxy,
                                JS::MutableHandleValue vp) const override;

  virtual bool isCallable(JSObject* obj) const override;
  virtual bool isConstructor(JSObject* obj) const override;

  virtual bool isScripted() const override { return true; }

  static GetTrapValidationResult checkGetTrapResult(JSContext* cx,
                                                    JS::HandleObject target,
                                                    JS::HandleId id,
                                                    JS::HandleValue trapResult);

  static void reportGetTrapValidationError(JSContext* cx, JS::HandleId id,
                                           GetTrapValidationResult validation);

  static const char family;
  static const ScriptedProxyHandler singleton;

  // The "proxy extra" slot index in which the handler is stored. Revocable
  // proxies need to set this at revocation time.
  static const int HANDLER_EXTRA = 0;
  static const int IS_CALLCONSTRUCT_EXTRA = 1;
  // Bitmasks for the "call/construct" slot
  static const int IS_CALLABLE = 1 << 0;
  static const int IS_CONSTRUCTOR = 1 << 1;
  // The "function extended" slot index in which the revocation object is
  // stored. Per spec, this is to be cleared during the first revocation.
  static const int REVOKE_SLOT = 0;

  static JSObject* handlerObject(const JSObject* proxy);
};

bool proxy(JSContext* cx, unsigned argc, JS::Value* vp);

bool proxy_revocable(JSContext* cx, unsigned argc, JS::Value* vp);

} /* namespace js */

#endif /* proxy_ScriptedProxyHandler_h */
