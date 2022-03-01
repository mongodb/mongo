/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_Environment_h
#define debugger_Environment_h

#include "mozilla/Assertions.h"  // for AssertionConditionType, MOZ_ASSERT
#include "mozilla/Maybe.h"       // for Maybe

#include "jstypes.h"            // for JS_PUBLIC_API
#include "NamespaceImports.h"   // for Value, HandleId, HandleObject
#include "debugger/Debugger.h"  // for Env
#include "gc/Rooting.h"         // for HandleDebuggerEnvironment
#include "js/PropertySpec.h"    // for JSFunctionSpec, JSPropertySpec
#include "js/RootingAPI.h"      // for Handle, MutableHandle
#include "vm/NativeObject.h"    // for NativeObject
#include "vm/Scope.h"           // for ScopeKind

class JS_PUBLIC_API JSObject;
struct JS_PUBLIC_API JSContext;
class JSTracer;

namespace js {

class GlobalObject;

enum class DebuggerEnvironmentType { Declarative, With, Object };

class DebuggerEnvironment : public NativeObject {
 public:
  enum { OWNER_SLOT };

  static const unsigned RESERVED_SLOTS = 1;

  static const JSClass class_;

  static NativeObject* initClass(JSContext* cx, Handle<GlobalObject*> global,
                                 HandleObject dbgCtor);
  static DebuggerEnvironment* create(JSContext* cx, HandleObject proto,
                                     HandleObject referent,
                                     HandleNativeObject debugger);

  void trace(JSTracer* trc);

  DebuggerEnvironmentType type() const;
  mozilla::Maybe<ScopeKind> scopeKind() const;
  [[nodiscard]] bool getParent(JSContext* cx,
                               MutableHandleDebuggerEnvironment result) const;
  [[nodiscard]] bool getObject(JSContext* cx,
                               MutableHandleDebuggerObject result) const;
  [[nodiscard]] bool getCalleeScript(JSContext* cx,
                                     MutableHandleDebuggerScript result) const;
  bool isDebuggee() const;
  bool isOptimized() const;

  [[nodiscard]] static bool getNames(JSContext* cx,
                                     HandleDebuggerEnvironment environment,
                                     MutableHandle<IdVector> result);
  [[nodiscard]] static bool find(JSContext* cx,
                                 HandleDebuggerEnvironment environment,
                                 HandleId id,
                                 MutableHandleDebuggerEnvironment result);
  [[nodiscard]] static bool getVariable(JSContext* cx,
                                        HandleDebuggerEnvironment environment,
                                        HandleId id, MutableHandleValue result);
  [[nodiscard]] static bool setVariable(JSContext* cx,
                                        HandleDebuggerEnvironment environment,
                                        HandleId id, HandleValue value);

  bool isInstance() const;
  Debugger* owner() const;

  Env* referent() const {
    Env* env = static_cast<Env*>(getPrivate());
    MOZ_ASSERT(env);
    return env;
  }

 private:
  static const JSClassOps classOps_;

  static const JSPropertySpec properties_[];
  static const JSFunctionSpec methods_[];

  bool requireDebuggee(JSContext* cx) const;

  [[nodiscard]] static bool construct(JSContext* cx, unsigned argc, Value* vp);

  struct CallData;
};

} /* namespace js */

#endif /* debugger_Environment_h */
