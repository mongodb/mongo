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

#include "jsapi.h"             // for JSContext
#include "jstypes.h"           // for JS_PUBLIC_API
#include "NamespaceImports.h"  // for Value, MutableHandleValue, HandleId

#include "gc/Rooting.h"       // for HandleDebuggerObject
#include "js/Promise.h"       // for PromiseState
#include "js/Proxy.h"         // for PropertyDescriptor
#include "vm/JSObject.h"      // for JSObject (ptr only)
#include "vm/NativeObject.h"  // for NativeObject

class JS_PUBLIC_API JSAtom;

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
                                HandleNativeObject debugger);

  void trace(JSTracer* trc);

  // Properties
  [[nodiscard]] static bool getClassName(JSContext* cx,
                                         HandleDebuggerObject object,
                                         MutableHandleString result);
  [[nodiscard]] static bool getBoundTargetFunction(
      JSContext* cx, HandleDebuggerObject object,
      MutableHandleDebuggerObject result);
  [[nodiscard]] static bool getBoundThis(JSContext* cx,
                                         HandleDebuggerObject object,
                                         MutableHandleValue result);
  [[nodiscard]] static bool getBoundArguments(
      JSContext* cx, HandleDebuggerObject object,
      MutableHandle<ValueVector> result);
  [[nodiscard]] static bool getAllocationSite(JSContext* cx,
                                              HandleDebuggerObject object,
                                              MutableHandleObject result);
  [[nodiscard]] static bool getErrorMessageName(JSContext* cx,
                                                HandleDebuggerObject object,
                                                MutableHandleString result);
  [[nodiscard]] static bool getErrorNotes(JSContext* cx,
                                          HandleDebuggerObject object,
                                          MutableHandleValue result);
  [[nodiscard]] static bool getErrorLineNumber(JSContext* cx,
                                               HandleDebuggerObject object,
                                               MutableHandleValue result);
  [[nodiscard]] static bool getErrorColumnNumber(JSContext* cx,
                                                 HandleDebuggerObject object,
                                                 MutableHandleValue result);
  [[nodiscard]] static bool getScriptedProxyTarget(
      JSContext* cx, HandleDebuggerObject object,
      MutableHandleDebuggerObject result);
  [[nodiscard]] static bool getScriptedProxyHandler(
      JSContext* cx, HandleDebuggerObject object,
      MutableHandleDebuggerObject result);
  [[nodiscard]] static bool getPromiseValue(JSContext* cx,
                                            HandleDebuggerObject object,
                                            MutableHandleValue result);
  [[nodiscard]] static bool getPromiseReason(JSContext* cx,
                                             HandleDebuggerObject object,
                                             MutableHandleValue result);

  // Methods
  [[nodiscard]] static bool isExtensible(JSContext* cx,
                                         HandleDebuggerObject object,
                                         bool& result);
  [[nodiscard]] static bool isSealed(JSContext* cx, HandleDebuggerObject object,
                                     bool& result);
  [[nodiscard]] static bool isFrozen(JSContext* cx, HandleDebuggerObject object,
                                     bool& result);
  [[nodiscard]] static JS::Result<Completion> getProperty(
      JSContext* cx, HandleDebuggerObject object, HandleId id,
      HandleValue receiver);
  [[nodiscard]] static JS::Result<Completion> setProperty(
      JSContext* cx, HandleDebuggerObject object, HandleId id,
      HandleValue value, HandleValue receiver);
  [[nodiscard]] static bool getPrototypeOf(JSContext* cx,
                                           HandleDebuggerObject object,
                                           MutableHandleDebuggerObject result);
  [[nodiscard]] static bool getOwnPropertyNames(JSContext* cx,
                                                HandleDebuggerObject object,
                                                MutableHandle<IdVector> result);
  [[nodiscard]] static bool getOwnPropertySymbols(
      JSContext* cx, HandleDebuggerObject object,
      MutableHandle<IdVector> result);
  [[nodiscard]] static bool getOwnPrivateProperties(
      JSContext* cx, HandleDebuggerObject object,
      MutableHandle<IdVector> result);
  [[nodiscard]] static bool getOwnPropertyDescriptor(
      JSContext* cx, HandleDebuggerObject object, HandleId id,
      MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc);
  [[nodiscard]] static bool preventExtensions(JSContext* cx,
                                              HandleDebuggerObject object);
  [[nodiscard]] static bool seal(JSContext* cx, HandleDebuggerObject object);
  [[nodiscard]] static bool freeze(JSContext* cx, HandleDebuggerObject object);
  [[nodiscard]] static bool defineProperty(JSContext* cx,
                                           HandleDebuggerObject object,
                                           HandleId id,
                                           Handle<PropertyDescriptor> desc);
  [[nodiscard]] static bool defineProperties(
      JSContext* cx, HandleDebuggerObject object, Handle<IdVector> ids,
      Handle<PropertyDescriptorVector> descs);
  [[nodiscard]] static bool deleteProperty(JSContext* cx,
                                           HandleDebuggerObject object,
                                           HandleId id, ObjectOpResult& result);
  [[nodiscard]] static mozilla::Maybe<Completion> call(
      JSContext* cx, HandleDebuggerObject object, HandleValue thisv,
      Handle<ValueVector> args);
  [[nodiscard]] static bool forceLexicalInitializationByName(
      JSContext* cx, HandleDebuggerObject object, HandleId id, bool& result);
  [[nodiscard]] static JS::Result<Completion> executeInGlobal(
      JSContext* cx, HandleDebuggerObject object,
      mozilla::Range<const char16_t> chars, HandleObject bindings,
      const EvalOptions& options);
  [[nodiscard]] static bool makeDebuggeeValue(JSContext* cx,
                                              HandleDebuggerObject object,
                                              HandleValue value,
                                              MutableHandleValue result);
  [[nodiscard]] static bool makeDebuggeeNativeFunction(
      JSContext* cx, HandleDebuggerObject object, HandleValue value,
      MutableHandleValue result);
  [[nodiscard]] static bool isSameNative(JSContext* cx,
                                         HandleDebuggerObject object,
                                         HandleValue value,
                                         MutableHandleValue result);
  [[nodiscard]] static bool unsafeDereference(JSContext* cx,
                                              HandleDebuggerObject object,
                                              MutableHandleObject result);
  [[nodiscard]] static bool unwrap(JSContext* cx, HandleDebuggerObject object,
                                   MutableHandleDebuggerObject result);

  // Infallible properties
  bool isCallable() const;
  bool isFunction() const;
  bool isDebuggeeFunction() const;
  bool isBoundFunction() const;
  bool isArrowFunction() const;
  bool isAsyncFunction() const;
  bool isGeneratorFunction() const;
  bool isClassConstructor() const;
  bool isGlobal() const;
  bool isScriptedProxy() const;
  bool isPromise() const;
  bool isError() const;
  JSAtom* name(JSContext* cx) const;
  JSAtom* displayName(JSContext* cx) const;
  JS::PromiseState promiseState() const;
  double promiseLifetime() const;
  double promiseTimeToResolution() const;

  bool isInstance() const;
  Debugger* owner() const;

  JSObject* referent() const {
    JSObject* obj = (JSObject*)getPrivate();
    MOZ_ASSERT(obj);
    return obj;
  }

 private:
  enum { OWNER_SLOT };

  static const unsigned RESERVED_SLOTS = 1;

  static const JSClassOps classOps_;

  static const JSPropertySpec properties_[];
  static const JSPropertySpec promiseProperties_[];
  static const JSFunctionSpec methods_[];

  PromiseObject* promise() const;

  [[nodiscard]] static bool requireGlobal(JSContext* cx,
                                          HandleDebuggerObject object);
  [[nodiscard]] static bool requirePromise(JSContext* cx,
                                           HandleDebuggerObject object);
  [[nodiscard]] static bool construct(JSContext* cx, unsigned argc, Value* vp);

  struct CallData;
  struct PromiseReactionRecordBuilder;

  [[nodiscard]] static bool getErrorReport(JSContext* cx,
                                           HandleObject maybeError,
                                           JSErrorReport*& report);
};

} /* namespace js */

#endif /* debugger_Object_h */
