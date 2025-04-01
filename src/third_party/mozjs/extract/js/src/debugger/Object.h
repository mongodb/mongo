/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_Object_h
#define debugger_Object_h

#include "mozilla/Assertions.h"  // for AssertionConditionType, MOZ_ASSERT
#include "mozilla/Maybe.h"       // for Maybe
#include "mozilla/Range.h"       // for Range
#include "mozilla/Result.h"      // for Result

#include "jstypes.h"           // for JS_PUBLIC_API
#include "NamespaceImports.h"  // for Value, MutableHandleValue, HandleId

#include "js/Promise.h"       // for PromiseState
#include "js/Proxy.h"         // for PropertyDescriptor
#include "vm/JSObject.h"      // for JSObject (ptr only)
#include "vm/NativeObject.h"  // for NativeObject

class JS_PUBLIC_API JSAtom;
struct JS_PUBLIC_API JSContext;

namespace js {

class Completion;
class Debugger;
class EvalOptions;
class GlobalObject;
class PromiseObject;

class DebuggerObject : public NativeObject {
 public:
  static const JSClass class_;

  static NativeObject* initClass(JSContext* cx, Handle<GlobalObject*> global,
                                 HandleObject debugCtor);
  static DebuggerObject* create(JSContext* cx, HandleObject proto,
                                HandleObject referent,
                                Handle<NativeObject*> debugger);

  void trace(JSTracer* trc);

  // Properties
  [[nodiscard]] static bool getClassName(JSContext* cx,
                                         Handle<DebuggerObject*> object,
                                         MutableHandleString result);
  [[nodiscard]] static bool getBoundTargetFunction(
      JSContext* cx, Handle<DebuggerObject*> object,
      MutableHandle<DebuggerObject*> result);
  [[nodiscard]] static bool getBoundThis(JSContext* cx,
                                         Handle<DebuggerObject*> object,
                                         MutableHandleValue result);
  [[nodiscard]] static bool getBoundArguments(
      JSContext* cx, Handle<DebuggerObject*> object,
      MutableHandle<ValueVector> result);
  [[nodiscard]] static bool getAllocationSite(JSContext* cx,
                                              Handle<DebuggerObject*> object,
                                              MutableHandleObject result);
  [[nodiscard]] static bool getErrorMessageName(JSContext* cx,
                                                Handle<DebuggerObject*> object,
                                                MutableHandleString result);
  [[nodiscard]] static bool getErrorNotes(JSContext* cx,
                                          Handle<DebuggerObject*> object,
                                          MutableHandleValue result);
  [[nodiscard]] static bool getErrorLineNumber(JSContext* cx,
                                               Handle<DebuggerObject*> object,
                                               MutableHandleValue result);
  [[nodiscard]] static bool getErrorColumnNumber(JSContext* cx,
                                                 Handle<DebuggerObject*> object,
                                                 MutableHandleValue result);
  [[nodiscard]] static bool getScriptedProxyTarget(
      JSContext* cx, Handle<DebuggerObject*> object,
      MutableHandle<DebuggerObject*> result);
  [[nodiscard]] static bool getScriptedProxyHandler(
      JSContext* cx, Handle<DebuggerObject*> object,
      MutableHandle<DebuggerObject*> result);
  [[nodiscard]] static bool getPromiseValue(JSContext* cx,
                                            Handle<DebuggerObject*> object,
                                            MutableHandleValue result);
  [[nodiscard]] static bool getPromiseReason(JSContext* cx,
                                             Handle<DebuggerObject*> object,
                                             MutableHandleValue result);

  // Methods
  [[nodiscard]] static bool isExtensible(JSContext* cx,
                                         Handle<DebuggerObject*> object,
                                         bool& result);
  [[nodiscard]] static bool isSealed(JSContext* cx,
                                     Handle<DebuggerObject*> object,
                                     bool& result);
  [[nodiscard]] static bool isFrozen(JSContext* cx,
                                     Handle<DebuggerObject*> object,
                                     bool& result);
  [[nodiscard]] static JS::Result<Completion> getProperty(
      JSContext* cx, Handle<DebuggerObject*> object, HandleId id,
      HandleValue receiver);
  [[nodiscard]] static JS::Result<Completion> setProperty(
      JSContext* cx, Handle<DebuggerObject*> object, HandleId id,
      HandleValue value, HandleValue receiver);
  [[nodiscard]] static bool getPrototypeOf(
      JSContext* cx, Handle<DebuggerObject*> object,
      MutableHandle<DebuggerObject*> result);
  [[nodiscard]] static bool getOwnPropertyNames(JSContext* cx,
                                                Handle<DebuggerObject*> object,
                                                MutableHandleIdVector result);
  [[nodiscard]] static bool getOwnPropertyNamesLength(
      JSContext* cx, Handle<DebuggerObject*> object, size_t* result);
  [[nodiscard]] static bool getOwnPropertySymbols(
      JSContext* cx, Handle<DebuggerObject*> object,
      MutableHandleIdVector result);
  [[nodiscard]] static bool getOwnPrivateProperties(
      JSContext* cx, Handle<DebuggerObject*> object,
      MutableHandleIdVector result);
  [[nodiscard]] static bool getOwnPropertyDescriptor(
      JSContext* cx, Handle<DebuggerObject*> object, HandleId id,
      MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc);
  [[nodiscard]] static bool preventExtensions(JSContext* cx,
                                              Handle<DebuggerObject*> object);
  [[nodiscard]] static bool seal(JSContext* cx, Handle<DebuggerObject*> object);
  [[nodiscard]] static bool freeze(JSContext* cx,
                                   Handle<DebuggerObject*> object);
  [[nodiscard]] static bool defineProperty(JSContext* cx,
                                           Handle<DebuggerObject*> object,
                                           HandleId id,
                                           Handle<PropertyDescriptor> desc);
  [[nodiscard]] static bool defineProperties(
      JSContext* cx, Handle<DebuggerObject*> object, Handle<IdVector> ids,
      Handle<PropertyDescriptorVector> descs);
  [[nodiscard]] static bool deleteProperty(JSContext* cx,
                                           Handle<DebuggerObject*> object,
                                           HandleId id, ObjectOpResult& result);
  [[nodiscard]] static mozilla::Maybe<Completion> call(
      JSContext* cx, Handle<DebuggerObject*> object, HandleValue thisv,
      Handle<ValueVector> args);
  [[nodiscard]] static bool forceLexicalInitializationByName(
      JSContext* cx, Handle<DebuggerObject*> object, HandleId id, bool& result);
  [[nodiscard]] static JS::Result<Completion> executeInGlobal(
      JSContext* cx, Handle<DebuggerObject*> object,
      mozilla::Range<const char16_t> chars, HandleObject bindings,
      const EvalOptions& options);
  [[nodiscard]] static bool makeDebuggeeValue(JSContext* cx,
                                              Handle<DebuggerObject*> object,
                                              HandleValue value,
                                              MutableHandleValue result);
  enum class CheckJitInfo { No, Yes };
  [[nodiscard]] static bool isSameNative(JSContext* cx,
                                         Handle<DebuggerObject*> object,
                                         HandleValue value,
                                         CheckJitInfo checkJitInfo,
                                         MutableHandleValue result);
  [[nodiscard]] static bool isNativeGetterWithJitInfo(
      JSContext* cx, Handle<DebuggerObject*> object, MutableHandleValue result);
  [[nodiscard]] static bool unsafeDereference(JSContext* cx,
                                              Handle<DebuggerObject*> object,
                                              MutableHandleObject result);
  [[nodiscard]] static bool unwrap(JSContext* cx,
                                   Handle<DebuggerObject*> object,
                                   MutableHandle<DebuggerObject*> result);

  // Infallible properties
  bool isCallable() const;
  bool isFunction() const;
  bool isDebuggeeFunction() const;
  bool isBoundFunction() const;
  bool isDebuggeeBoundFunction() const;
  bool isArrowFunction() const;
  bool isAsyncFunction() const;
  bool isGeneratorFunction() const;
  bool isClassConstructor() const;
  bool isGlobal() const;
  bool isScriptedProxy() const;
  bool isPromise() const;
  bool isError() const;

  bool name(JSContext* cx, JS::MutableHandle<JSAtom*> result) const;
  bool displayName(JSContext* cx, JS::MutableHandle<JSAtom*> result) const;

  JS::PromiseState promiseState() const;
  double promiseLifetime() const;
  double promiseTimeToResolution() const;

  Debugger* owner() const;

  JSObject* maybeReferent() const {
    return maybePtrFromReservedSlot<JSObject>(OBJECT_SLOT);
  }
  JSObject* referent() const {
    JSObject* obj = maybeReferent();
    MOZ_ASSERT(obj);
    return obj;
  }

  void clearReferent() { clearReservedSlotGCThingAsPrivate(OBJECT_SLOT); }

 private:
  enum { OBJECT_SLOT, OWNER_SLOT, RESERVED_SLOTS };

  static const JSClassOps classOps_;

  static const JSPropertySpec properties_[];
  static const JSPropertySpec promiseProperties_[];
  static const JSFunctionSpec methods_[];

  PromiseObject* promise() const;

  [[nodiscard]] static bool requireGlobal(JSContext* cx,
                                          Handle<DebuggerObject*> object);
  [[nodiscard]] static bool requirePromise(JSContext* cx,
                                           Handle<DebuggerObject*> object);
  [[nodiscard]] static bool construct(JSContext* cx, unsigned argc, Value* vp);

  struct CallData;
  struct PromiseReactionRecordBuilder;

  [[nodiscard]] static bool getErrorReport(JSContext* cx,
                                           HandleObject maybeError,
                                           JSErrorReport*& report);
};

} /* namespace js */

#endif /* debugger_Object_h */
