/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "debugger/Environment-inl.h"

#include "mozilla/Assertions.h"  // for AssertionConditionType
#include "mozilla/Maybe.h"       // for Maybe, Some, Nothing
#include "mozilla/Vector.h"      // for Vector

#include <string.h>  // for strlen, size_t
#include <utility>   // for move

#include "debugger/Debugger.h"  // for Env, Debugger, ValueToIdentifier
#include "debugger/Object.h"    // for DebuggerObject
#include "debugger/Script.h"    // for DebuggerScript
#include "gc/Tracer.h"    // for TraceManuallyBarrieredCrossCompartmentEdge
#include "js/CallArgs.h"  // for CallArgs
#include "js/friend/ErrorMessages.h"  // for GetErrorMessage, JSMSG_*
#include "js/HeapAPI.h"               // for IsInsideNursery
#include "js/RootingAPI.h"            // for Rooted, MutableHandle
#include "util/Identifier.h"          // for IsIdentifier
#include "vm/Compartment.h"           // for Compartment
#include "vm/JSAtomUtils.h"           // for Atomize
#include "vm/JSContext.h"             // for JSContext
#include "vm/JSFunction.h"            // for JSFunction
#include "vm/JSObject.h"              // for JSObject, RequireObject,
#include "vm/NativeObject.h"          // for NativeObject, JSObject::is
#include "vm/Realm.h"                 // for AutoRealm, ErrorCopier
#include "vm/Scope.h"                 // for ScopeKind, ScopeKindString
#include "vm/StringType.h"            // for JSAtom

#include "gc/StableCellHasher-inl.h"
#include "vm/Compartment-inl.h"        // for Compartment::wrap
#include "vm/EnvironmentObject-inl.h"  // for JSObject::enclosingEnvironment
#include "vm/JSObject-inl.h"  // for IsInternalFunctionObject, NewObjectWithGivenProtoAndKind
#include "vm/ObjectOperations-inl.h"  // for HasProperty, GetProperty
#include "vm/Realm-inl.h"             // for AutoRealm::AutoRealm

namespace js {
class GlobalObject;
}

using namespace js;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

const JSClassOps DebuggerEnvironment::classOps_ = {
    nullptr,                               // addProperty
    nullptr,                               // delProperty
    nullptr,                               // enumerate
    nullptr,                               // newEnumerate
    nullptr,                               // resolve
    nullptr,                               // mayResolve
    nullptr,                               // finalize
    nullptr,                               // call
    nullptr,                               // construct
    CallTraceMethod<DebuggerEnvironment>,  // trace
};

const JSClass DebuggerEnvironment::class_ = {
    "Environment",
    JSCLASS_HAS_RESERVED_SLOTS(DebuggerEnvironment::RESERVED_SLOTS),
    &classOps_,
};

void DebuggerEnvironment::trace(JSTracer* trc) {
  // There is a barrier on private pointers, so the Unbarriered marking
  // is okay.
  if (Env* referent = maybeReferent()) {
    TraceManuallyBarrieredCrossCompartmentEdge(trc, this, &referent,
                                               "Debugger.Environment referent");
    if (referent != maybeReferent()) {
      setReservedSlotGCThingAsPrivateUnbarriered(ENV_SLOT, referent);
    }
  }
}

static DebuggerEnvironment* DebuggerEnvironment_checkThis(
    JSContext* cx, const CallArgs& args) {
  JSObject* thisobj = RequireObject(cx, args.thisv());
  if (!thisobj) {
    return nullptr;
  }
  if (!thisobj->is<DebuggerEnvironment>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "Debugger.Environment",
                              "method", thisobj->getClass()->name);
    return nullptr;
  }

  return &thisobj->as<DebuggerEnvironment>();
}

struct MOZ_STACK_CLASS DebuggerEnvironment::CallData {
  JSContext* cx;
  const CallArgs& args;

  Handle<DebuggerEnvironment*> environment;

  CallData(JSContext* cx, const CallArgs& args,
           Handle<DebuggerEnvironment*> env)
      : cx(cx), args(args), environment(env) {}

  bool typeGetter();
  bool scopeKindGetter();
  bool parentGetter();
  bool objectGetter();
  bool calleeScriptGetter();
  bool inspectableGetter();
  bool optimizedOutGetter();

  bool namesMethod();
  bool findMethod();
  bool getVariableMethod();
  bool setVariableMethod();

  using Method = bool (CallData::*)();

  template <Method MyMethod>
  static bool ToNative(JSContext* cx, unsigned argc, Value* vp);
};

template <DebuggerEnvironment::CallData::Method MyMethod>
/* static */
bool DebuggerEnvironment::CallData::ToNative(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DebuggerEnvironment*> environment(
      cx, DebuggerEnvironment_checkThis(cx, args));
  if (!environment) {
    return false;
  }

  CallData data(cx, args, environment);
  return (data.*MyMethod)();
}

/* static */
bool DebuggerEnvironment::construct(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                            "Debugger.Environment");
  return false;
}

static bool IsDeclarative(Env* env) {
  return env->is<DebugEnvironmentProxy>() &&
         env->as<DebugEnvironmentProxy>().isForDeclarative();
}

template <typename T>
static bool IsDebugEnvironmentWrapper(Env* env) {
  return env->is<DebugEnvironmentProxy>() &&
         env->as<DebugEnvironmentProxy>().environment().is<T>();
}

bool DebuggerEnvironment::CallData::typeGetter() {
  if (!environment->requireDebuggee(cx)) {
    return false;
  }

  DebuggerEnvironmentType type = environment->type();

  const char* s;
  switch (type) {
    case DebuggerEnvironmentType::Declarative:
      s = "declarative";
      break;
    case DebuggerEnvironmentType::With:
      s = "with";
      break;
    case DebuggerEnvironmentType::Object:
      s = "object";
      break;
  }

  JSAtom* str = Atomize(cx, s, strlen(s));
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

bool DebuggerEnvironment::CallData::scopeKindGetter() {
  if (!environment->requireDebuggee(cx)) {
    return false;
  }

  Maybe<ScopeKind> kind = environment->scopeKind();
  if (kind.isSome()) {
    const char* s = ScopeKindString(*kind);
    JSAtom* str = Atomize(cx, s, strlen(s));
    if (!str) {
      return false;
    }
    args.rval().setString(str);
  } else {
    args.rval().setNull();
  }

  return true;
}

bool DebuggerEnvironment::CallData::parentGetter() {
  if (!environment->requireDebuggee(cx)) {
    return false;
  }

  Rooted<DebuggerEnvironment*> result(cx);
  if (!environment->getParent(cx, &result)) {
    return false;
  }

  args.rval().setObjectOrNull(result);
  return true;
}

bool DebuggerEnvironment::CallData::objectGetter() {
  if (!environment->requireDebuggee(cx)) {
    return false;
  }

  if (environment->type() == DebuggerEnvironmentType::Declarative) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_NO_ENV_OBJECT);
    return false;
  }

  Rooted<DebuggerObject*> result(cx);
  if (!environment->getObject(cx, &result)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

bool DebuggerEnvironment::CallData::calleeScriptGetter() {
  if (!environment->requireDebuggee(cx)) {
    return false;
  }

  Rooted<DebuggerScript*> result(cx);
  if (!environment->getCalleeScript(cx, &result)) {
    return false;
  }

  args.rval().setObjectOrNull(result);
  return true;
}

bool DebuggerEnvironment::CallData::inspectableGetter() {
  args.rval().setBoolean(environment->isDebuggee());
  return true;
}

bool DebuggerEnvironment::CallData::optimizedOutGetter() {
  args.rval().setBoolean(environment->isOptimized());
  return true;
}

bool DebuggerEnvironment::CallData::namesMethod() {
  if (!environment->requireDebuggee(cx)) {
    return false;
  }

  RootedIdVector ids(cx);
  if (!DebuggerEnvironment::getNames(cx, environment, &ids)) {
    return false;
  }

  JSObject* obj = IdVectorToArray(cx, ids);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

bool DebuggerEnvironment::CallData::findMethod() {
  if (!args.requireAtLeast(cx, "Debugger.Environment.find", 1)) {
    return false;
  }

  RootedId id(cx);
  if (!ValueToIdentifier(cx, args[0], &id)) {
    return false;
  }

  if (!environment->requireDebuggee(cx)) {
    return false;
  }

  Rooted<DebuggerEnvironment*> result(cx);
  if (!DebuggerEnvironment::find(cx, environment, id, &result)) {
    return false;
  }

  args.rval().setObjectOrNull(result);
  return true;
}

bool DebuggerEnvironment::CallData::getVariableMethod() {
  if (!args.requireAtLeast(cx, "Debugger.Environment.getVariable", 1)) {
    return false;
  }

  RootedId id(cx);
  if (!ValueToIdentifier(cx, args[0], &id)) {
    return false;
  }

  if (!environment->requireDebuggee(cx)) {
    return false;
  }

  return DebuggerEnvironment::getVariable(cx, environment, id, args.rval());
}

bool DebuggerEnvironment::CallData::setVariableMethod() {
  if (!args.requireAtLeast(cx, "Debugger.Environment.setVariable", 2)) {
    return false;
  }

  RootedId id(cx);
  if (!ValueToIdentifier(cx, args[0], &id)) {
    return false;
  }

  if (!environment->requireDebuggee(cx)) {
    return false;
  }

  if (!DebuggerEnvironment::setVariable(cx, environment, id, args[1])) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool DebuggerEnvironment::requireDebuggee(JSContext* cx) const {
  if (!isDebuggee()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_NOT_DEBUGGEE, "Debugger.Environment",
                              "environment");

    return false;
  }

  return true;
}

const JSPropertySpec DebuggerEnvironment::properties_[] = {
    JS_DEBUG_PSG("type", typeGetter),
    JS_DEBUG_PSG("scopeKind", scopeKindGetter),
    JS_DEBUG_PSG("parent", parentGetter),
    JS_DEBUG_PSG("object", objectGetter),
    JS_DEBUG_PSG("calleeScript", calleeScriptGetter),
    JS_DEBUG_PSG("inspectable", inspectableGetter),
    JS_DEBUG_PSG("optimizedOut", optimizedOutGetter),
    JS_PS_END,
};

const JSFunctionSpec DebuggerEnvironment::methods_[] = {
    JS_DEBUG_FN("names", namesMethod, 0),
    JS_DEBUG_FN("find", findMethod, 1),
    JS_DEBUG_FN("getVariable", getVariableMethod, 1),
    JS_DEBUG_FN("setVariable", setVariableMethod, 2),
    JS_FS_END,
};

/* static */
NativeObject* DebuggerEnvironment::initClass(JSContext* cx,
                                             Handle<GlobalObject*> global,
                                             HandleObject dbgCtor) {
  return InitClass(cx, dbgCtor, nullptr, nullptr, "Environment", construct, 0,
                   properties_, methods_, nullptr, nullptr);
}

/* static */
DebuggerEnvironment* DebuggerEnvironment::create(
    JSContext* cx, HandleObject proto, HandleObject referent,
    Handle<NativeObject*> debugger) {
  DebuggerEnvironment* obj =
      IsInsideNursery(referent)
          ? NewObjectWithGivenProto<DebuggerEnvironment>(cx, proto)
          : NewTenuredObjectWithGivenProto<DebuggerEnvironment>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  obj->setReservedSlotGCThingAsPrivate(ENV_SLOT, referent);
  obj->setReservedSlot(OWNER_SLOT, ObjectValue(*debugger));

  return obj;
}

/* static */
DebuggerEnvironmentType DebuggerEnvironment::type() const {
  // Don't bother switching compartments just to check env's type.
  if (IsDeclarative(referent())) {
    return DebuggerEnvironmentType::Declarative;
  }
  if (IsDebugEnvironmentWrapper<WithEnvironmentObject>(referent())) {
    return DebuggerEnvironmentType::With;
  }
  return DebuggerEnvironmentType::Object;
}

mozilla::Maybe<ScopeKind> DebuggerEnvironment::scopeKind() const {
  if (!referent()->is<DebugEnvironmentProxy>()) {
    return Nothing();
  }
  EnvironmentObject& env =
      referent()->as<DebugEnvironmentProxy>().environment();
  Scope* scope = GetEnvironmentScope(env);
  return scope ? Some(scope->kind()) : Nothing();
}

bool DebuggerEnvironment::getParent(
    JSContext* cx, MutableHandle<DebuggerEnvironment*> result) const {
  // Don't bother switching compartments just to get env's parent.
  Rooted<Env*> parent(cx, referent()->enclosingEnvironment());
  if (!parent) {
    result.set(nullptr);
    return true;
  }

  return owner()->wrapEnvironment(cx, parent, result);
}

bool DebuggerEnvironment::getObject(
    JSContext* cx, MutableHandle<DebuggerObject*> result) const {
  MOZ_ASSERT(type() != DebuggerEnvironmentType::Declarative);

  // Don't bother switching compartments just to get env's object.
  RootedObject object(cx);
  if (IsDebugEnvironmentWrapper<WithEnvironmentObject>(referent())) {
    object.set(&referent()
                    ->as<DebugEnvironmentProxy>()
                    .environment()
                    .as<WithEnvironmentObject>()
                    .object());
  } else if (IsDebugEnvironmentWrapper<NonSyntacticVariablesObject>(
                 referent())) {
    object.set(&referent()
                    ->as<DebugEnvironmentProxy>()
                    .environment()
                    .as<NonSyntacticVariablesObject>());
  } else {
    object.set(referent());
    MOZ_ASSERT(!object->is<DebugEnvironmentProxy>());
  }

  return owner()->wrapDebuggeeObject(cx, object, result);
}

bool DebuggerEnvironment::getCalleeScript(
    JSContext* cx, MutableHandle<DebuggerScript*> result) const {
  if (!referent()->is<DebugEnvironmentProxy>()) {
    result.set(nullptr);
    return true;
  }

  JSObject& scope = referent()->as<DebugEnvironmentProxy>().environment();
  if (!scope.is<CallObject>()) {
    result.set(nullptr);
    return true;
  }

  Rooted<BaseScript*> script(cx, scope.as<CallObject>().callee().baseScript());

  DebuggerScript* scriptObject = owner()->wrapScript(cx, script);
  if (!scriptObject) {
    return false;
  }

  result.set(scriptObject);
  return true;
}

bool DebuggerEnvironment::isDebuggee() const {
  MOZ_ASSERT(referent());
  MOZ_ASSERT(!referent()->is<EnvironmentObject>());

  return owner()->observesGlobal(&referent()->nonCCWGlobal());
}

bool DebuggerEnvironment::isOptimized() const {
  return referent()->is<DebugEnvironmentProxy>() &&
         referent()->as<DebugEnvironmentProxy>().isOptimizedOut();
}

/* static */
bool DebuggerEnvironment::getNames(JSContext* cx,
                                   Handle<DebuggerEnvironment*> environment,
                                   MutableHandleIdVector result) {
  MOZ_ASSERT(environment->isDebuggee());
  MOZ_ASSERT(result.empty());

  Rooted<Env*> referent(cx, environment->referent());
  {
    Maybe<AutoRealm> ar;
    ar.emplace(cx, referent);

    ErrorCopier ec(ar);
    if (!GetPropertyKeys(cx, referent, JSITER_HIDDEN, result)) {
      return false;
    }
  }

  result.eraseIf([](PropertyKey key) {
    return !key.isAtom() || !IsIdentifier(key.toAtom());
  });

  for (size_t i = 0; i < result.length(); ++i) {
    cx->markAtom(result[i].toAtom());
  }

  return true;
}

/* static */
bool DebuggerEnvironment::find(JSContext* cx,
                               Handle<DebuggerEnvironment*> environment,
                               HandleId id,
                               MutableHandle<DebuggerEnvironment*> result) {
  MOZ_ASSERT(environment->isDebuggee());

  Rooted<Env*> env(cx, environment->referent());
  Debugger* dbg = environment->owner();

  {
    Maybe<AutoRealm> ar;
    ar.emplace(cx, env);

    cx->markId(id);

    // This can trigger resolve hooks.
    ErrorCopier ec(ar);
    for (; env; env = env->enclosingEnvironment()) {
      bool found;
      if (!HasProperty(cx, env, id, &found)) {
        return false;
      }
      if (found) {
        break;
      }
    }
  }

  if (!env) {
    result.set(nullptr);
    return true;
  }

  return dbg->wrapEnvironment(cx, env, result);
}

/* static */
bool DebuggerEnvironment::getVariable(JSContext* cx,
                                      Handle<DebuggerEnvironment*> environment,
                                      HandleId id, MutableHandleValue result) {
  MOZ_ASSERT(environment->isDebuggee());

  Rooted<Env*> referent(cx, environment->referent());
  Debugger* dbg = environment->owner();

  {
    Maybe<AutoRealm> ar;
    ar.emplace(cx, referent);

    cx->markId(id);

    // This can trigger getters.
    ErrorCopier ec(ar);

    bool found;
    if (!HasProperty(cx, referent, id, &found)) {
      return false;
    }
    if (!found) {
      result.setUndefined();
      return true;
    }

    // For DebugEnvironmentProxys, we get sentinel values for optimized out
    // slots and arguments instead of throwing (the default behavior).
    //
    // See wrapDebuggeeValue for how the sentinel values are wrapped.
    if (referent->is<DebugEnvironmentProxy>()) {
      Rooted<DebugEnvironmentProxy*> env(
          cx, &referent->as<DebugEnvironmentProxy>());
      if (!DebugEnvironmentProxy::getMaybeSentinelValue(cx, env, id, result)) {
        return false;
      }
    } else {
      if (!GetProperty(cx, referent, referent, id, result)) {
        return false;
      }
    }
  }

  // When we've faked up scope chain objects for optimized-out scopes,
  // declarative environments may contain internal JSFunction objects, which
  // we shouldn't expose to the user.
  if (result.isObject()) {
    RootedObject obj(cx, &result.toObject());
    if (obj->is<JSFunction>() &&
        IsInternalFunctionObject(obj->as<JSFunction>()))
      result.setMagic(JS_OPTIMIZED_OUT);
  }

  return dbg->wrapDebuggeeValue(cx, result);
}

/* static */
bool DebuggerEnvironment::setVariable(JSContext* cx,
                                      Handle<DebuggerEnvironment*> environment,
                                      HandleId id, HandleValue value_) {
  MOZ_ASSERT(environment->isDebuggee());

  Rooted<Env*> referent(cx, environment->referent());
  Debugger* dbg = environment->owner();

  RootedValue value(cx, value_);
  if (!dbg->unwrapDebuggeeValue(cx, &value)) {
    return false;
  }

  {
    Maybe<AutoRealm> ar;
    ar.emplace(cx, referent);
    if (!cx->compartment()->wrap(cx, &value)) {
      return false;
    }
    cx->markId(id);

    // This can trigger setters.
    ErrorCopier ec(ar);

    // Make sure the environment actually has the specified binding.
    bool found;
    if (!HasProperty(cx, referent, id, &found)) {
      return false;
    }
    if (!found) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_VARIABLE_NOT_FOUND);
      return false;
    }

    // Just set the property.
    if (!SetProperty(cx, referent, id, value)) {
      return false;
    }
  }

  return true;
}
