/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "debugger/Object-inl.h"

#include "mozilla/Maybe.h"   // for Maybe, Nothing, Some
#include "mozilla/Range.h"   // for Range
#include "mozilla/Result.h"  // for Result
#include "mozilla/Vector.h"  // for Vector

#include <algorithm>
#include <string.h>     // for size_t, strlen
#include <type_traits>  // for remove_reference<>::type
#include <utility>      // for move

#include "jsapi.h"  // for CallArgs, RootedObject, Rooted

#include "builtin/Array.h"       // for NewDenseCopiedArray
#include "builtin/Promise.h"     // for PromiseReactionRecordBuilder
#include "debugger/Debugger.h"   // for Completion, Debugger
#include "debugger/Environment.h"
#include "debugger/Frame.h"      // for DebuggerFrame
#include "debugger/NoExecute.h"  // for LeaveDebuggeeNoExecute
#include "debugger/Script.h"     // for DebuggerScript
#include "debugger/Source.h"     // for DebuggerSource
#include "gc/Barrier.h"          // for ImmutablePropertyNamePtr
#include "gc/Rooting.h"          // for RootedDebuggerObject
#include "gc/Tracer.h"  // for TraceManuallyBarrieredCrossCompartmentEdge
#include "js/CompilationAndEvaluation.h"  //  for Compile
#include "js/Conversions.h"               // for ToObject
#include "js/friend/ErrorMessages.h"      // for GetErrorMessage, JSMSG_*
#include "js/friend/WindowProxy.h"  // for IsWindow, IsWindowProxy, ToWindowIfWindowProxy
#include "js/HeapAPI.h"             // for IsInsideNursery
#include "js/Promise.h"             // for PromiseState
#include "js/Proxy.h"               // for PropertyDescriptor
#include "js/SourceText.h"               // for SourceText
#include "js/StableStringChars.h"        // for AutoStableStringChars
#include "js/String.h"                   // for JS::StringHasLatin1Chars
#include "proxy/ScriptedProxyHandler.h"  // for ScriptedProxyHandler
#include "vm/ArgumentsObject.h"          // for ARGS_LENGTH_MAX
#include "vm/ArrayObject.h"              // for ArrayObject
#include "vm/AsyncFunction.h"            // for AsyncGeneratorObject
#include "vm/AsyncIteration.h"           // for AsyncFunctionGeneratorObject
#include "vm/BytecodeUtil.h"             // for JSDVG_SEARCH_STACK
#include "vm/Compartment.h"              // for Compartment
#include "vm/EnvironmentObject.h"        // for GetDebugEnvironmentForFunction
#include "vm/ErrorObject.h"              // for JSObject::is, ErrorObject
#include "vm/GeneratorObject.h"          // for AbstractGeneratorObject
#include "vm/GlobalObject.h"             // for JSObject::is, GlobalObject
#include "vm/Interpreter.h"              // for Call
#include "vm/JSAtom.h"                   // for Atomize
#include "vm/JSContext.h"                // for JSContext, ReportValueError
#include "vm/JSFunction.h"               // for JSFunction
#include "vm/JSObject.h"                 // for GenericObject, NewObjectKind
#include "vm/JSScript.h"                 // for JSScript
#include "vm/NativeObject.h"             // for NativeObject, JSObject::is
#include "vm/ObjectOperations.h"         // for DefineProperty
#include "vm/PlainObject.h"              // for js::PlainObject
#include "vm/PromiseObject.h"            // for js::PromiseObject
#include "vm/Realm.h"                    // for AutoRealm, ErrorCopier, Realm
#include "vm/Runtime.h"                  // for JSAtomState
#include "vm/SavedFrame.h"               // for SavedFrame
#include "vm/Scope.h"                    // for PositionalFormalParameterIter
#include "vm/SelfHosting.h"              // for GetClonedSelfHostedFunctionName
#include "vm/Shape.h"                    // for Shape
#include "vm/Stack.h"                    // for InvokeArgs
#include "vm/StringType.h"               // for JSAtom, PropertyName
#include "vm/WellKnownAtom.h"            // for js_apply_str
#include "vm/WrapperObject.h"            // for JSObject::is, WrapperObject

#include "vm/Compartment-inl.h"  // for Compartment::wrap
#include "vm/JSObject-inl.h"  // for GetObjectClassName, InitClass, NewObjectWithGivenProtoAndKind, ToPropertyKey
#include "vm/NativeObject-inl.h"      // for NativeObject::global
#include "vm/ObjectOperations-inl.h"  // for DeleteProperty, GetProperty
#include "vm/Realm-inl.h"             // for AutoRealm::AutoRealm

using namespace js;

using JS::AutoStableStringChars;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

const JSClassOps DebuggerObject::classOps_ = {
    nullptr,                          // addProperty
    nullptr,                          // delProperty
    nullptr,                          // enumerate
    nullptr,                          // newEnumerate
    nullptr,                          // resolve
    nullptr,                          // mayResolve
    nullptr,                          // finalize
    nullptr,                          // call
    nullptr,                          // hasInstance
    nullptr,                          // construct
    CallTraceMethod<DebuggerObject>,  // trace
};

const JSClass DebuggerObject::class_ = {
    "Object", JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS),
    &classOps_};

void DebuggerObject::trace(JSTracer* trc) {
  // There is a barrier on private pointers, so the Unbarriered marking
  // is okay.
  if (JSObject* referent = (JSObject*)getPrivate()) {
    TraceManuallyBarrieredCrossCompartmentEdge(
        trc, static_cast<JSObject*>(this), &referent,
        "Debugger.Object referent");
    setPrivateUnbarriered(referent);
  }
}

static DebuggerObject* DebuggerObject_checkThis(JSContext* cx,
                                                const CallArgs& args) {
  JSObject* thisobj = RequireObject(cx, args.thisv());
  if (!thisobj) {
    return nullptr;
  }
  if (!thisobj->is<DebuggerObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "Debugger.Object",
                              "method", thisobj->getClass()->name);
    return nullptr;
  }

  // Forbid Debugger.Object.prototype, which is of class DebuggerObject::class_
  // but isn't a real working Debugger.Object.
  DebuggerObject* nthisobj = &thisobj->as<DebuggerObject>();
  if (!nthisobj->isInstance()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "Debugger.Object",
                              "method", "prototype object");
    return nullptr;
  }
  return nthisobj;
}

/* static */
bool DebuggerObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                            "Debugger.Object");
  return false;
}

struct MOZ_STACK_CLASS DebuggerObject::CallData {
  JSContext* cx;
  const CallArgs& args;

  HandleDebuggerObject object;
  RootedObject referent;

  CallData(JSContext* cx, const CallArgs& args, HandleDebuggerObject obj)
      : cx(cx), args(args), object(obj), referent(cx, obj->referent()) {}

  // JSNative properties
  bool callableGetter();
  bool isBoundFunctionGetter();
  bool isArrowFunctionGetter();
  bool isAsyncFunctionGetter();
  bool isClassConstructorGetter();
  bool isGeneratorFunctionGetter();
  bool protoGetter();
  bool classGetter();
  bool nameGetter();
  bool displayNameGetter();
  bool parameterNamesGetter();
  bool scriptGetter();
  bool environmentGetter();
  bool boundTargetFunctionGetter();
  bool boundThisGetter();
  bool boundArgumentsGetter();
  bool allocationSiteGetter();
  bool isErrorGetter();
  bool errorMessageNameGetter();
  bool errorNotesGetter();
  bool errorLineNumberGetter();
  bool errorColumnNumberGetter();
  bool isProxyGetter();
  bool proxyTargetGetter();
  bool proxyHandlerGetter();
  bool isPromiseGetter();
  bool promiseStateGetter();
  bool promiseValueGetter();
  bool promiseReasonGetter();
  bool promiseLifetimeGetter();
  bool promiseTimeToResolutionGetter();
  bool promiseAllocationSiteGetter();
  bool promiseResolutionSiteGetter();
  bool promiseIDGetter();
  bool promiseDependentPromisesGetter();

  // JSNative methods
  bool isExtensibleMethod();
  bool isSealedMethod();
  bool isFrozenMethod();
  bool getPropertyMethod();
  bool setPropertyMethod();
  bool getOwnPropertyNamesMethod();
  bool getOwnPropertySymbolsMethod();
  bool getOwnPrivatePropertiesMethod();
  bool getOwnPropertyDescriptorMethod();
  bool preventExtensionsMethod();
  bool sealMethod();
  bool freezeMethod();
  bool definePropertyMethod();
  bool definePropertiesMethod();
  bool deletePropertyMethod();
  bool callMethod();
  bool applyMethod();
  bool asEnvironmentMethod();
  bool forceLexicalInitializationByNameMethod();
  bool executeInGlobalMethod();
  bool executeInGlobalWithBindingsMethod();
  bool createSource();
  bool makeDebuggeeValueMethod();
  bool makeDebuggeeNativeFunctionMethod();
  bool isSameNativeMethod();
  bool unsafeDereferenceMethod();
  bool unwrapMethod();
  bool getPromiseReactionsMethod();

  using Method = bool (CallData::*)();

  template <Method MyMethod>
  static bool ToNative(JSContext* cx, unsigned argc, Value* vp);
};

template <DebuggerObject::CallData::Method MyMethod>
/* static */
bool DebuggerObject::CallData::ToNative(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedDebuggerObject obj(cx, DebuggerObject_checkThis(cx, args));
  if (!obj) {
    return false;
  }

  CallData data(cx, args, obj);
  return (data.*MyMethod)();
}

bool DebuggerObject::CallData::callableGetter() {
  args.rval().setBoolean(object->isCallable());
  return true;
}

bool DebuggerObject::CallData::isBoundFunctionGetter() {
  if (!object->isDebuggeeFunction()) {
    args.rval().setUndefined();
    return true;
  }

  args.rval().setBoolean(object->isBoundFunction());
  return true;
}

bool DebuggerObject::CallData::isArrowFunctionGetter() {
  if (!object->isDebuggeeFunction()) {
    args.rval().setUndefined();
    return true;
  }

  args.rval().setBoolean(object->isArrowFunction());
  return true;
}

bool DebuggerObject::CallData::isAsyncFunctionGetter() {
  if (!object->isDebuggeeFunction()) {
    args.rval().setUndefined();
    return true;
  }

  args.rval().setBoolean(object->isAsyncFunction());
  return true;
}

bool DebuggerObject::CallData::isGeneratorFunctionGetter() {
  if (!object->isDebuggeeFunction()) {
    args.rval().setUndefined();
    return true;
  }

  args.rval().setBoolean(object->isGeneratorFunction());
  return true;
}

bool DebuggerObject::CallData::isClassConstructorGetter() {
  if (!object->isDebuggeeFunction()) {
    args.rval().setUndefined();
    return true;
  }

  args.rval().setBoolean(object->isClassConstructor());
  return true;
}

bool DebuggerObject::CallData::protoGetter() {
  RootedDebuggerObject result(cx);
  if (!DebuggerObject::getPrototypeOf(cx, object, &result)) {
    return false;
  }

  args.rval().setObjectOrNull(result);
  return true;
}

bool DebuggerObject::CallData::classGetter() {
  RootedString result(cx);
  if (!DebuggerObject::getClassName(cx, object, &result)) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

bool DebuggerObject::CallData::nameGetter() {
  if (!object->isFunction()) {
    args.rval().setUndefined();
    return true;
  }

  RootedString result(cx, object->name(cx));
  if (result) {
    args.rval().setString(result);
  } else {
    args.rval().setUndefined();
  }
  return true;
}

bool DebuggerObject::CallData::displayNameGetter() {
  if (!object->isFunction()) {
    args.rval().setUndefined();
    return true;
  }

  RootedString result(cx, object->displayName(cx));
  if (result) {
    args.rval().setString(result);
  } else {
    args.rval().setUndefined();
  }
  return true;
}

bool DebuggerObject::CallData::parameterNamesGetter() {
  if (!object->isDebuggeeFunction()) {
    args.rval().setUndefined();
    return true;
  }

  RootedFunction referent(cx, &object->referent()->as<JSFunction>());

  ArrayObject* arr = GetFunctionParameterNamesArray(cx, referent);
  if (!arr) {
    return false;
  }

  args.rval().setObject(*arr);
  return true;
}

bool DebuggerObject::CallData::scriptGetter() {
  Debugger* dbg = object->owner();

  if (!referent->is<JSFunction>()) {
    args.rval().setUndefined();
    return true;
  }

  RootedFunction fun(cx, &referent->as<JSFunction>());
  if (!IsInterpretedNonSelfHostedFunction(fun)) {
    args.rval().setUndefined();
    return true;
  }

  RootedScript script(cx, GetOrCreateFunctionScript(cx, fun));
  if (!script) {
    return false;
  }

  // Only hand out debuggee scripts.
  if (!dbg->observesScript(script)) {
    args.rval().setNull();
    return true;
  }

  RootedDebuggerScript scriptObject(cx, dbg->wrapScript(cx, script));
  if (!scriptObject) {
    return false;
  }

  args.rval().setObject(*scriptObject);
  return true;
}

bool DebuggerObject::CallData::environmentGetter() {
  Debugger* dbg = object->owner();

  // Don't bother switching compartments just to check obj's type and get its
  // env.
  if (!referent->is<JSFunction>()) {
    args.rval().setUndefined();
    return true;
  }

  RootedFunction fun(cx, &referent->as<JSFunction>());
  if (!IsInterpretedNonSelfHostedFunction(fun)) {
    args.rval().setUndefined();
    return true;
  }

  // Only hand out environments of debuggee functions.
  if (!dbg->observesGlobal(&fun->global())) {
    args.rval().setNull();
    return true;
  }

  Rooted<Env*> env(cx);
  {
    AutoRealm ar(cx, fun);
    env = GetDebugEnvironmentForFunction(cx, fun);
    if (!env) {
      return false;
    }
  }

  return dbg->wrapEnvironment(cx, env, args.rval());
}

bool DebuggerObject::CallData::boundTargetFunctionGetter() {
  if (!object->isDebuggeeFunction() || !object->isBoundFunction()) {
    args.rval().setUndefined();
    return true;
  }

  RootedDebuggerObject result(cx);
  if (!DebuggerObject::getBoundTargetFunction(cx, object, &result)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

bool DebuggerObject::CallData::boundThisGetter() {
  if (!object->isDebuggeeFunction() || !object->isBoundFunction()) {
    args.rval().setUndefined();
    return true;
  }

  return DebuggerObject::getBoundThis(cx, object, args.rval());
}

bool DebuggerObject::CallData::boundArgumentsGetter() {
  if (!object->isDebuggeeFunction() || !object->isBoundFunction()) {
    args.rval().setUndefined();
    return true;
  }

  Rooted<ValueVector> result(cx, ValueVector(cx));
  if (!DebuggerObject::getBoundArguments(cx, object, &result)) {
    return false;
  }

  RootedObject obj(cx,
                   NewDenseCopiedArray(cx, result.length(), result.begin()));
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

bool DebuggerObject::CallData::allocationSiteGetter() {
  RootedObject result(cx);
  if (!DebuggerObject::getAllocationSite(cx, object, &result)) {
    return false;
  }

  args.rval().setObjectOrNull(result);
  return true;
}

// Returns the "name" field (see js/public/friend/ErrorNumbers.msg), which may
// be used as a unique identifier, for any error object with a JSErrorReport or
// undefined if the object has no JSErrorReport.
bool DebuggerObject::CallData::errorMessageNameGetter() {
  RootedString result(cx);
  if (!DebuggerObject::getErrorMessageName(cx, object, &result)) {
    return false;
  }

  if (result) {
    args.rval().setString(result);
  } else {
    args.rval().setUndefined();
  }
  return true;
}

bool DebuggerObject::CallData::isErrorGetter() {
  args.rval().setBoolean(object->isError());
  return true;
}

bool DebuggerObject::CallData::errorNotesGetter() {
  return DebuggerObject::getErrorNotes(cx, object, args.rval());
}

bool DebuggerObject::CallData::errorLineNumberGetter() {
  return DebuggerObject::getErrorLineNumber(cx, object, args.rval());
}

bool DebuggerObject::CallData::errorColumnNumberGetter() {
  return DebuggerObject::getErrorColumnNumber(cx, object, args.rval());
}

bool DebuggerObject::CallData::isProxyGetter() {
  args.rval().setBoolean(object->isScriptedProxy());
  return true;
}

bool DebuggerObject::CallData::proxyTargetGetter() {
  if (!object->isScriptedProxy()) {
    args.rval().setUndefined();
    return true;
  }

  Rooted<DebuggerObject*> result(cx);
  if (!DebuggerObject::getScriptedProxyTarget(cx, object, &result)) {
    return false;
  }

  args.rval().setObjectOrNull(result);
  return true;
}

bool DebuggerObject::CallData::proxyHandlerGetter() {
  if (!object->isScriptedProxy()) {
    args.rval().setUndefined();
    return true;
  }
  Rooted<DebuggerObject*> result(cx);
  if (!DebuggerObject::getScriptedProxyHandler(cx, object, &result)) {
    return false;
  }

  args.rval().setObjectOrNull(result);
  return true;
}

bool DebuggerObject::CallData::isPromiseGetter() {
  args.rval().setBoolean(object->isPromise());
  return true;
}

bool DebuggerObject::CallData::promiseStateGetter() {
  if (!DebuggerObject::requirePromise(cx, object)) {
    return false;
  }

  RootedValue result(cx);
  switch (object->promiseState()) {
    case JS::PromiseState::Pending:
      result.setString(cx->names().pending);
      break;
    case JS::PromiseState::Fulfilled:
      result.setString(cx->names().fulfilled);
      break;
    case JS::PromiseState::Rejected:
      result.setString(cx->names().rejected);
      break;
  }

  args.rval().set(result);
  return true;
}

bool DebuggerObject::CallData::promiseValueGetter() {
  if (!DebuggerObject::requirePromise(cx, object)) {
    return false;
  }

  if (object->promiseState() != JS::PromiseState::Fulfilled) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_PROMISE_NOT_FULFILLED);
    return false;
  }

  return DebuggerObject::getPromiseValue(cx, object, args.rval());
  ;
}

bool DebuggerObject::CallData::promiseReasonGetter() {
  if (!DebuggerObject::requirePromise(cx, object)) {
    return false;
  }

  if (object->promiseState() != JS::PromiseState::Rejected) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_PROMISE_NOT_REJECTED);
    return false;
  }

  return DebuggerObject::getPromiseReason(cx, object, args.rval());
}

bool DebuggerObject::CallData::promiseLifetimeGetter() {
  if (!DebuggerObject::requirePromise(cx, object)) {
    return false;
  }

  args.rval().setNumber(object->promiseLifetime());
  return true;
}

bool DebuggerObject::CallData::promiseTimeToResolutionGetter() {
  if (!DebuggerObject::requirePromise(cx, object)) {
    return false;
  }

  if (object->promiseState() == JS::PromiseState::Pending) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_PROMISE_NOT_RESOLVED);
    return false;
  }

  args.rval().setNumber(object->promiseTimeToResolution());
  return true;
}

static PromiseObject* EnsurePromise(JSContext* cx, HandleObject referent) {
  // We only care about promises, so CheckedUnwrapStatic is OK.
  RootedObject obj(cx, CheckedUnwrapStatic(referent));
  if (!obj) {
    ReportAccessDenied(cx);
    return nullptr;
  }
  if (!obj->is<PromiseObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, "Debugger", "Promise",
                              obj->getClass()->name);
    return nullptr;
  }
  return &obj->as<PromiseObject>();
}

bool DebuggerObject::CallData::promiseAllocationSiteGetter() {
  Rooted<PromiseObject*> promise(cx, EnsurePromise(cx, referent));
  if (!promise) {
    return false;
  }

  RootedObject allocSite(cx, promise->allocationSite());
  if (!allocSite) {
    args.rval().setNull();
    return true;
  }

  if (!cx->compartment()->wrap(cx, &allocSite)) {
    return false;
  }
  args.rval().set(ObjectValue(*allocSite));
  return true;
}

bool DebuggerObject::CallData::promiseResolutionSiteGetter() {
  Rooted<PromiseObject*> promise(cx, EnsurePromise(cx, referent));
  if (!promise) {
    return false;
  }

  if (promise->state() == JS::PromiseState::Pending) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_PROMISE_NOT_RESOLVED);
    return false;
  }

  RootedObject resolutionSite(cx, promise->resolutionSite());
  if (!resolutionSite) {
    args.rval().setNull();
    return true;
  }

  if (!cx->compartment()->wrap(cx, &resolutionSite)) {
    return false;
  }
  args.rval().set(ObjectValue(*resolutionSite));
  return true;
}

bool DebuggerObject::CallData::promiseIDGetter() {
  Rooted<PromiseObject*> promise(cx, EnsurePromise(cx, referent));
  if (!promise) {
    return false;
  }

  args.rval().setNumber(double(promise->getID()));
  return true;
}

bool DebuggerObject::CallData::promiseDependentPromisesGetter() {
  Debugger* dbg = object->owner();

  Rooted<PromiseObject*> promise(cx, EnsurePromise(cx, referent));
  if (!promise) {
    return false;
  }

  Rooted<GCVector<Value>> values(cx, GCVector<Value>(cx));
  {
    JSAutoRealm ar(cx, promise);
    if (!promise->dependentPromises(cx, &values)) {
      return false;
    }
  }
  for (size_t i = 0; i < values.length(); i++) {
    if (!dbg->wrapDebuggeeValue(cx, values[i])) {
      return false;
    }
  }
  RootedArrayObject promises(cx);
  if (values.length() == 0) {
    promises = NewDenseEmptyArray(cx);
  } else {
    promises = NewDenseCopiedArray(cx, values.length(), values[0].address());
  }
  if (!promises) {
    return false;
  }
  args.rval().setObject(*promises);
  return true;
}

bool DebuggerObject::CallData::isExtensibleMethod() {
  bool result;
  if (!DebuggerObject::isExtensible(cx, object, result)) {
    return false;
  }

  args.rval().setBoolean(result);
  return true;
}

bool DebuggerObject::CallData::isSealedMethod() {
  bool result;
  if (!DebuggerObject::isSealed(cx, object, result)) {
    return false;
  }

  args.rval().setBoolean(result);
  return true;
}

bool DebuggerObject::CallData::isFrozenMethod() {
  bool result;
  if (!DebuggerObject::isFrozen(cx, object, result)) {
    return false;
  }

  args.rval().setBoolean(result);
  return true;
}

bool DebuggerObject::CallData::getOwnPropertyNamesMethod() {
  Rooted<IdVector> ids(cx, IdVector(cx));
  if (!DebuggerObject::getOwnPropertyNames(cx, object, &ids)) {
    return false;
  }

  RootedObject obj(cx, IdVectorToArray(cx, ids));
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

bool DebuggerObject::CallData::getOwnPropertySymbolsMethod() {
  Rooted<IdVector> ids(cx, IdVector(cx));
  if (!DebuggerObject::getOwnPropertySymbols(cx, object, &ids)) {
    return false;
  }

  RootedObject obj(cx, IdVectorToArray(cx, ids));
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

bool DebuggerObject::CallData::getOwnPrivatePropertiesMethod() {
  Rooted<IdVector> ids(cx, IdVector(cx));
  if (!DebuggerObject::getOwnPrivateProperties(cx, object, &ids)) {
    return false;
  }

  RootedObject obj(cx, IdVectorToArray(cx, ids));
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

bool DebuggerObject::CallData::getOwnPropertyDescriptorMethod() {
  RootedId id(cx);
  if (!ToPropertyKey(cx, args.get(0), &id)) {
    return false;
  }

  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!DebuggerObject::getOwnPropertyDescriptor(cx, object, id, &desc)) {
    return false;
  }

  return JS::FromPropertyDescriptor(cx, desc, args.rval());
}

bool DebuggerObject::CallData::preventExtensionsMethod() {
  if (!DebuggerObject::preventExtensions(cx, object)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool DebuggerObject::CallData::sealMethod() {
  if (!DebuggerObject::seal(cx, object)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool DebuggerObject::CallData::freezeMethod() {
  if (!DebuggerObject::freeze(cx, object)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool DebuggerObject::CallData::definePropertyMethod() {
  if (!args.requireAtLeast(cx, "Debugger.Object.defineProperty", 2)) {
    return false;
  }

  RootedId id(cx);
  if (!ToPropertyKey(cx, args[0], &id)) {
    return false;
  }

  Rooted<PropertyDescriptor> desc(cx);
  if (!ToPropertyDescriptor(cx, args[1], false, &desc)) {
    return false;
  }

  if (!DebuggerObject::defineProperty(cx, object, id, desc)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool DebuggerObject::CallData::definePropertiesMethod() {
  if (!args.requireAtLeast(cx, "Debugger.Object.defineProperties", 1)) {
    return false;
  }

  RootedValue arg(cx, args[0]);
  RootedObject props(cx, ToObject(cx, arg));
  if (!props) {
    return false;
  }
  RootedIdVector ids(cx);
  Rooted<PropertyDescriptorVector> descs(cx, PropertyDescriptorVector(cx));
  if (!ReadPropertyDescriptors(cx, props, false, &ids, &descs)) {
    return false;
  }
  Rooted<IdVector> ids2(cx, IdVector(cx));
  if (!ids2.append(ids.begin(), ids.end())) {
    return false;
  }

  if (!DebuggerObject::defineProperties(cx, object, ids2, descs)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/*
 * This does a non-strict delete, as a matter of API design. The case where the
 * property is non-configurable isn't necessarily exceptional here.
 */
bool DebuggerObject::CallData::deletePropertyMethod() {
  RootedId id(cx);
  if (!ToPropertyKey(cx, args.get(0), &id)) {
    return false;
  }

  ObjectOpResult result;
  if (!DebuggerObject::deleteProperty(cx, object, id, result)) {
    return false;
  }

  args.rval().setBoolean(result.ok());
  return true;
}

bool DebuggerObject::CallData::callMethod() {
  RootedValue thisv(cx, args.get(0));

  Rooted<ValueVector> nargs(cx, ValueVector(cx));
  if (args.length() >= 2) {
    if (!nargs.growBy(args.length() - 1)) {
      return false;
    }
    for (size_t i = 1; i < args.length(); ++i) {
      nargs[i - 1].set(args[i]);
    }
  }

  Rooted<Maybe<Completion>> completion(
      cx, DebuggerObject::call(cx, object, thisv, nargs));
  if (!completion.get()) {
    return false;
  }

  return completion->buildCompletionValue(cx, object->owner(), args.rval());
}

bool DebuggerObject::CallData::getPropertyMethod() {
  Debugger* dbg = object->owner();

  RootedId id(cx);
  if (!ToPropertyKey(cx, args.get(0), &id)) {
    return false;
  }

  RootedValue receiver(cx,
                       args.length() < 2 ? ObjectValue(*object) : args.get(1));

  Rooted<Completion> comp(cx);
  JS_TRY_VAR_OR_RETURN_FALSE(cx, comp, getProperty(cx, object, id, receiver));
  return comp.get().buildCompletionValue(cx, dbg, args.rval());
}

bool DebuggerObject::CallData::setPropertyMethod() {
  Debugger* dbg = object->owner();

  RootedId id(cx);
  if (!ToPropertyKey(cx, args.get(0), &id)) {
    return false;
  }

  RootedValue value(cx, args.get(1));

  RootedValue receiver(cx,
                       args.length() < 3 ? ObjectValue(*object) : args.get(2));

  Rooted<Completion> comp(cx);
  JS_TRY_VAR_OR_RETURN_FALSE(cx, comp,
                             setProperty(cx, object, id, value, receiver));
  return comp.get().buildCompletionValue(cx, dbg, args.rval());
}

bool DebuggerObject::CallData::applyMethod() {
  RootedValue thisv(cx, args.get(0));

  Rooted<ValueVector> nargs(cx, ValueVector(cx));
  if (args.length() >= 2 && !args[1].isNullOrUndefined()) {
    if (!args[1].isObject()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_APPLY_ARGS, js_apply_str);
      return false;
    }

    RootedObject argsobj(cx, &args[1].toObject());

    uint64_t argc = 0;
    if (!GetLengthProperty(cx, argsobj, &argc)) {
      return false;
    }
    argc = std::min(argc, uint64_t(ARGS_LENGTH_MAX));

    if (!nargs.growBy(argc) || !GetElements(cx, argsobj, argc, nargs.begin())) {
      return false;
    }
  }

  Rooted<Maybe<Completion>> completion(
      cx, DebuggerObject::call(cx, object, thisv, nargs));
  if (!completion.get()) {
    return false;
  }

  return completion->buildCompletionValue(cx, object->owner(), args.rval());
}

static void EnterDebuggeeObjectRealm(JSContext* cx, Maybe<AutoRealm>& ar,
                                     JSObject* referent) {
  // |referent| may be a cross-compartment wrapper and CCWs normally
  // shouldn't be used with AutoRealm, but here we use an arbitrary realm for
  // now because we don't really have another option.
  ar.emplace(cx, referent->maybeCCWRealm()->maybeGlobal());
}

static bool RequireGlobalObject(JSContext* cx, HandleValue dbgobj,
                                HandleObject referent) {
  RootedObject obj(cx, referent);

  if (!obj->is<GlobalObject>()) {
    const char* isWrapper = "";
    const char* isWindowProxy = "";

    // Help the poor programmer by pointing out wrappers around globals...
    if (obj->is<WrapperObject>()) {
      obj = js::UncheckedUnwrap(obj);
      isWrapper = "a wrapper around ";
    }

    // ... and WindowProxies around Windows.
    if (IsWindowProxy(obj)) {
      obj = ToWindowIfWindowProxy(obj);
      isWindowProxy = "a WindowProxy referring to ";
    }

    if (obj->is<GlobalObject>()) {
      ReportValueError(cx, JSMSG_DEBUG_WRAPPER_IN_WAY, JSDVG_SEARCH_STACK,
                       dbgobj, nullptr, isWrapper, isWindowProxy);
    } else {
      ReportValueError(cx, JSMSG_DEBUG_BAD_REFERENT, JSDVG_SEARCH_STACK, dbgobj,
                       nullptr, "a global object");
    }
    return false;
  }

  return true;
}

bool DebuggerObject::CallData::asEnvironmentMethod() {
  Debugger* dbg = object->owner();

  if (!RequireGlobalObject(cx, args.thisv(), referent)) {
    return false;
  }

  Rooted<Env*> env(cx);
  {
    AutoRealm ar(cx, referent);
    env = GetDebugEnvironmentForGlobalLexicalEnvironment(cx);
    if (!env) {
      return false;
    }
  }

  return dbg->wrapEnvironment(cx, env, args.rval());
}

// Lookup a binding on the referent's global scope and change it to undefined
// if it is an uninitialized lexical, otherwise do nothing. The method's
// JavaScript return value is true _only_ when an uninitialized lexical has been
// altered, otherwise it is false.
bool DebuggerObject::CallData::forceLexicalInitializationByNameMethod() {
  if (!args.requireAtLeast(
          cx, "Debugger.Object.prototype.forceLexicalInitializationByName",
          1)) {
    return false;
  }

  if (!DebuggerObject::requireGlobal(cx, object)) {
    return false;
  }

  RootedId id(cx);
  if (!ValueToIdentifier(cx, args[0], &id)) {
    return false;
  }

  bool result;
  if (!DebuggerObject::forceLexicalInitializationByName(cx, object, id,
                                                        result)) {
    return false;
  }

  args.rval().setBoolean(result);
  return true;
}

bool DebuggerObject::CallData::executeInGlobalMethod() {
  if (!args.requireAtLeast(cx, "Debugger.Object.prototype.executeInGlobal",
                           1)) {
    return false;
  }

  if (!DebuggerObject::requireGlobal(cx, object)) {
    return false;
  }

  AutoStableStringChars stableChars(cx);
  if (!ValueToStableChars(cx, "Debugger.Object.prototype.executeInGlobal",
                          args[0], stableChars)) {
    return false;
  }
  mozilla::Range<const char16_t> chars = stableChars.twoByteRange();

  EvalOptions options;
  if (!ParseEvalOptions(cx, args.get(1), options)) {
    return false;
  }

  Rooted<Completion> comp(cx);
  JS_TRY_VAR_OR_RETURN_FALSE(
      cx, comp,
      DebuggerObject::executeInGlobal(cx, object, chars, nullptr, options));
  return comp.get().buildCompletionValue(cx, object->owner(), args.rval());
}

bool DebuggerObject::CallData::executeInGlobalWithBindingsMethod() {
  if (!args.requireAtLeast(
          cx, "Debugger.Object.prototype.executeInGlobalWithBindings", 2)) {
    return false;
  }

  if (!DebuggerObject::requireGlobal(cx, object)) {
    return false;
  }

  AutoStableStringChars stableChars(cx);
  if (!ValueToStableChars(
          cx, "Debugger.Object.prototype.executeInGlobalWithBindings", args[0],
          stableChars)) {
    return false;
  }
  mozilla::Range<const char16_t> chars = stableChars.twoByteRange();

  RootedObject bindings(cx, RequireObject(cx, args[1]));
  if (!bindings) {
    return false;
  }

  EvalOptions options;
  if (!ParseEvalOptions(cx, args.get(2), options)) {
    return false;
  }

  Rooted<Completion> comp(cx);
  JS_TRY_VAR_OR_RETURN_FALSE(
      cx, comp,
      DebuggerObject::executeInGlobal(cx, object, chars, bindings, options));
  return comp.get().buildCompletionValue(cx, object->owner(), args.rval());
}

// Copy a narrow or wide string to a vector, appending a null terminator.
template <typename T>
static bool CopyStringToVector(JSContext* cx, JSString* str, Vector<T>& chars) {
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }
  if (!chars.appendN(0, linear->length() + 1)) {
    return false;
  }
  CopyChars(chars.begin(), *linear);
  return true;
}

bool DebuggerObject::CallData::createSource() {
  if (!args.requireAtLeast(cx, "Debugger.Object.prototype.createSource", 1)) {
    return false;
  }

  if (!DebuggerObject::requireGlobal(cx, object)) {
    return false;
  }

  Debugger* dbg = object->owner();
  if (!dbg->isDebuggeeUnbarriered(referent->as<GlobalObject>().realm())) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_NOT_DEBUGGEE, "Debugger.Object",
                              "global");
    return false;
  }

  RootedObject options(cx, ToObject(cx, args[0]));
  if (!options) {
    return false;
  }

  RootedValue v(cx);
  if (!JS_GetProperty(cx, options, "text", &v)) {
    return false;
  }

  RootedString text(cx, ToString<CanGC>(cx, v));
  if (!text) {
    return false;
  }

  if (!JS_GetProperty(cx, options, "url", &v)) {
    return false;
  }

  RootedString url(cx, ToString<CanGC>(cx, v));
  if (!url) {
    return false;
  }

  if (!JS_GetProperty(cx, options, "startLine", &v)) {
    return false;
  }

  uint32_t startLine;
  if (!ToUint32(cx, v, &startLine)) {
    return false;
  }

  if (!JS_GetProperty(cx, options, "sourceMapURL", &v)) {
    return false;
  }

  RootedString sourceMapURL(cx);
  if (!v.isUndefined()) {
    sourceMapURL = ToString<CanGC>(cx, v);
    if (!sourceMapURL) {
      return false;
    }
  }

  if (!JS_GetProperty(cx, options, "isScriptElement", &v)) {
    return false;
  }

  bool isScriptElement = ToBoolean(v);

  JS::CompileOptions compileOptions(cx);
  compileOptions.lineno = startLine;

  if (!JS::StringHasLatin1Chars(url)) {
    JS_ReportErrorASCII(cx, "URL must be a narrow string");
    return false;
  }

  Vector<Latin1Char> urlChars(cx);
  if (!CopyStringToVector(cx, url, urlChars)) {
    return false;
  }
  compileOptions.setFile((const char*)urlChars.begin());

  Vector<char16_t> sourceMapURLChars(cx);
  if (sourceMapURL) {
    if (!CopyStringToVector(cx, sourceMapURL, sourceMapURLChars)) {
      return false;
    }
    compileOptions.setSourceMapURL(sourceMapURLChars.begin());
  }

  if (isScriptElement) {
    // The introduction type must be a statically allocated string.
    compileOptions.setIntroductionType("inlineScript");
  }

  Vector<char16_t> textChars(cx);
  if (!CopyStringToVector(cx, text, textChars)) {
    return false;
  }

  JS::SourceText<char16_t> srcBuf;
  if (!srcBuf.init(cx, textChars.begin(), text->length(),
                   JS::SourceOwnership::Borrowed)) {
    return false;
  }

  RootedScript script(cx);
  {
    AutoRealm ar(cx, referent);
    script = JS::Compile(cx, compileOptions, srcBuf);
    if (!script) {
      return false;
    }
  }

  RootedScriptSourceObject sso(cx, script->sourceObject());
  RootedObject wrapped(cx, dbg->wrapSource(cx, sso));
  if (!wrapped) {
    return false;
  }

  args.rval().setObject(*wrapped);
  return true;
}

bool DebuggerObject::CallData::makeDebuggeeValueMethod() {
  if (!args.requireAtLeast(cx, "Debugger.Object.prototype.makeDebuggeeValue",
                           1)) {
    return false;
  }

  return DebuggerObject::makeDebuggeeValue(cx, object, args[0], args.rval());
}

bool DebuggerObject::CallData::makeDebuggeeNativeFunctionMethod() {
  if (!args.requireAtLeast(
          cx, "Debugger.Object.prototype.makeDebuggeeNativeFunction", 1)) {
    return false;
  }

  return DebuggerObject::makeDebuggeeNativeFunction(cx, object, args[0],
                                                    args.rval());
}

bool DebuggerObject::CallData::isSameNativeMethod() {
  if (!args.requireAtLeast(cx, "Debugger.Object.prototype.isSameNative", 1)) {
    return false;
  }

  return DebuggerObject::isSameNative(cx, object, args[0], args.rval());
}

bool DebuggerObject::CallData::unsafeDereferenceMethod() {
  RootedObject result(cx);
  if (!DebuggerObject::unsafeDereference(cx, object, &result)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

bool DebuggerObject::CallData::unwrapMethod() {
  RootedDebuggerObject result(cx);
  if (!DebuggerObject::unwrap(cx, object, &result)) {
    return false;
  }

  args.rval().setObjectOrNull(result);
  return true;
}

struct DebuggerObject::PromiseReactionRecordBuilder
    : js::PromiseReactionRecordBuilder {
  Debugger* dbg;
  HandleArrayObject records;

  PromiseReactionRecordBuilder(Debugger* dbg, HandleArrayObject records)
      : dbg(dbg), records(records) {}

  bool then(JSContext* cx, HandleObject resolve, HandleObject reject,
            HandleObject result) override {
    RootedPlainObject record(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!record) {
      return false;
    }

    if (!setIfNotNull(cx, record, cx->names().resolve, resolve) ||
        !setIfNotNull(cx, record, cx->names().reject, reject) ||
        !setIfNotNull(cx, record, cx->names().result, result)) {
      return false;
    }

    return push(cx, record);
  }

  bool direct(JSContext* cx, Handle<PromiseObject*> unwrappedPromise) override {
    RootedValue v(cx, ObjectValue(*unwrappedPromise));
    return dbg->wrapDebuggeeValue(cx, &v) && push(cx, v);
  }

  bool asyncFunction(
      JSContext* cx,
      Handle<AsyncFunctionGeneratorObject*> unwrappedGenerator) override {
    return pushGenerator(cx, unwrappedGenerator);
  }

  bool asyncGenerator(
      JSContext* cx,
      Handle<AsyncGeneratorObject*> unwrappedGenerator) override {
    return pushGenerator(cx, unwrappedGenerator);
  }

 private:
  bool push(JSContext* cx, HandleObject record) {
    RootedValue recordVal(cx, ObjectValue(*record));
    return push(cx, recordVal);
  }

  bool push(JSContext* cx, HandleValue recordVal) {
    return NewbornArrayPush(cx, records, recordVal);
  }

  bool pushGenerator(JSContext* cx,
                     Handle<AbstractGeneratorObject*> unwrappedGenerator) {
    RootedDebuggerFrame frame(cx);
    return dbg->getFrame(cx, unwrappedGenerator, &frame) && push(cx, frame);
  }

  bool setIfNotNull(JSContext* cx, HandlePlainObject obj,
                    Handle<PropertyName*> name, HandleObject prop) {
    if (!prop) {
      return true;
    }

    RootedValue v(cx, ObjectValue(*prop));
    if (!dbg->wrapDebuggeeValue(cx, &v) ||
        !DefineDataProperty(cx, obj, name, v)) {
      return false;
    }

    return true;
  }
};

bool DebuggerObject::CallData::getPromiseReactionsMethod() {
  Debugger* dbg = object->owner();

  Rooted<PromiseObject*> unwrappedPromise(cx, EnsurePromise(cx, referent));
  if (!unwrappedPromise) {
    return false;
  }

  RootedArrayObject holder(cx, NewDenseEmptyArray(cx));
  if (!holder) {
    return false;
  }

  PromiseReactionRecordBuilder builder(dbg, holder);
  if (!unwrappedPromise->forEachReactionRecord(cx, builder)) {
    return false;
  }

  args.rval().setObject(*builder.records);
  return true;
}

const JSPropertySpec DebuggerObject::properties_[] = {
    JS_DEBUG_PSG("callable", callableGetter),
    JS_DEBUG_PSG("isBoundFunction", isBoundFunctionGetter),
    JS_DEBUG_PSG("isArrowFunction", isArrowFunctionGetter),
    JS_DEBUG_PSG("isGeneratorFunction", isGeneratorFunctionGetter),
    JS_DEBUG_PSG("isAsyncFunction", isAsyncFunctionGetter),
    JS_DEBUG_PSG("isClassConstructor", isClassConstructorGetter),
    JS_DEBUG_PSG("proto", protoGetter),
    JS_DEBUG_PSG("class", classGetter),
    JS_DEBUG_PSG("name", nameGetter),
    JS_DEBUG_PSG("displayName", displayNameGetter),
    JS_DEBUG_PSG("parameterNames", parameterNamesGetter),
    JS_DEBUG_PSG("script", scriptGetter),
    JS_DEBUG_PSG("environment", environmentGetter),
    JS_DEBUG_PSG("boundTargetFunction", boundTargetFunctionGetter),
    JS_DEBUG_PSG("boundThis", boundThisGetter),
    JS_DEBUG_PSG("boundArguments", boundArgumentsGetter),
    JS_DEBUG_PSG("allocationSite", allocationSiteGetter),
    JS_DEBUG_PSG("isError", isErrorGetter),
    JS_DEBUG_PSG("errorMessageName", errorMessageNameGetter),
    JS_DEBUG_PSG("errorNotes", errorNotesGetter),
    JS_DEBUG_PSG("errorLineNumber", errorLineNumberGetter),
    JS_DEBUG_PSG("errorColumnNumber", errorColumnNumberGetter),
    JS_DEBUG_PSG("isProxy", isProxyGetter),
    JS_DEBUG_PSG("proxyTarget", proxyTargetGetter),
    JS_DEBUG_PSG("proxyHandler", proxyHandlerGetter),
    JS_PS_END};

const JSPropertySpec DebuggerObject::promiseProperties_[] = {
    JS_DEBUG_PSG("isPromise", isPromiseGetter),
    JS_DEBUG_PSG("promiseState", promiseStateGetter),
    JS_DEBUG_PSG("promiseValue", promiseValueGetter),
    JS_DEBUG_PSG("promiseReason", promiseReasonGetter),
    JS_DEBUG_PSG("promiseLifetime", promiseLifetimeGetter),
    JS_DEBUG_PSG("promiseTimeToResolution", promiseTimeToResolutionGetter),
    JS_DEBUG_PSG("promiseAllocationSite", promiseAllocationSiteGetter),
    JS_DEBUG_PSG("promiseResolutionSite", promiseResolutionSiteGetter),
    JS_DEBUG_PSG("promiseID", promiseIDGetter),
    JS_DEBUG_PSG("promiseDependentPromises", promiseDependentPromisesGetter),
    JS_PS_END};

const JSFunctionSpec DebuggerObject::methods_[] = {
    JS_DEBUG_FN("isExtensible", isExtensibleMethod, 0),
    JS_DEBUG_FN("isSealed", isSealedMethod, 0),
    JS_DEBUG_FN("isFrozen", isFrozenMethod, 0),
    JS_DEBUG_FN("getProperty", getPropertyMethod, 0),
    JS_DEBUG_FN("setProperty", setPropertyMethod, 0),
    JS_DEBUG_FN("getOwnPropertyNames", getOwnPropertyNamesMethod, 0),
    JS_DEBUG_FN("getOwnPropertySymbols", getOwnPropertySymbolsMethod, 0),
    JS_DEBUG_FN("getOwnPrivateProperties", getOwnPrivatePropertiesMethod, 0),
    JS_DEBUG_FN("getOwnPropertyDescriptor", getOwnPropertyDescriptorMethod, 1),
    JS_DEBUG_FN("preventExtensions", preventExtensionsMethod, 0),
    JS_DEBUG_FN("seal", sealMethod, 0),
    JS_DEBUG_FN("freeze", freezeMethod, 0),
    JS_DEBUG_FN("defineProperty", definePropertyMethod, 2),
    JS_DEBUG_FN("defineProperties", definePropertiesMethod, 1),
    JS_DEBUG_FN("deleteProperty", deletePropertyMethod, 1),
    JS_DEBUG_FN("call", callMethod, 0),
    JS_DEBUG_FN("apply", applyMethod, 0),
    JS_DEBUG_FN("asEnvironment", asEnvironmentMethod, 0),
    JS_DEBUG_FN("forceLexicalInitializationByName",
                forceLexicalInitializationByNameMethod, 1),
    JS_DEBUG_FN("executeInGlobal", executeInGlobalMethod, 1),
    JS_DEBUG_FN("executeInGlobalWithBindings",
                executeInGlobalWithBindingsMethod, 2),
    JS_DEBUG_FN("createSource", createSource, 1),
    JS_DEBUG_FN("makeDebuggeeValue", makeDebuggeeValueMethod, 1),
    JS_DEBUG_FN("makeDebuggeeNativeFunction", makeDebuggeeNativeFunctionMethod,
                1),
    JS_DEBUG_FN("isSameNative", isSameNativeMethod, 1),
    JS_DEBUG_FN("unsafeDereference", unsafeDereferenceMethod, 0),
    JS_DEBUG_FN("unwrap", unwrapMethod, 0),
    JS_DEBUG_FN("getPromiseReactions", getPromiseReactionsMethod, 0),
    JS_FS_END};

/* static */
NativeObject* DebuggerObject::initClass(JSContext* cx,
                                        Handle<GlobalObject*> global,
                                        HandleObject debugCtor) {
  RootedNativeObject objectProto(
      cx, InitClass(cx, debugCtor, nullptr, &class_, construct, 0, properties_,
                    methods_, nullptr, nullptr));

  if (!objectProto) {
    return nullptr;
  }

  if (!DefinePropertiesAndFunctions(cx, objectProto, promiseProperties_,
                                    nullptr)) {
    return nullptr;
  }

  return objectProto;
}

/* static */
DebuggerObject* DebuggerObject::create(JSContext* cx, HandleObject proto,
                                       HandleObject referent,
                                       HandleNativeObject debugger) {
  DebuggerObject* obj =
      IsInsideNursery(referent)
          ? NewObjectWithGivenProto<DebuggerObject>(cx, proto)
          : NewTenuredObjectWithGivenProto<DebuggerObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  obj->setPrivateGCThing(referent);
  obj->setReservedSlot(OWNER_SLOT, ObjectValue(*debugger));

  return obj;
}

bool DebuggerObject::isCallable() const { return referent()->isCallable(); }

bool DebuggerObject::isFunction() const { return referent()->is<JSFunction>(); }

bool DebuggerObject::isDebuggeeFunction() const {
  return referent()->is<JSFunction>() &&
         owner()->observesGlobal(&referent()->as<JSFunction>().global());
}

bool DebuggerObject::isBoundFunction() const {
  MOZ_ASSERT(isDebuggeeFunction());

  return referent()->isBoundFunction();
}

bool DebuggerObject::isArrowFunction() const {
  MOZ_ASSERT(isDebuggeeFunction());

  return referent()->as<JSFunction>().isArrow();
}

bool DebuggerObject::isAsyncFunction() const {
  MOZ_ASSERT(isDebuggeeFunction());

  return referent()->as<JSFunction>().isAsync();
}

bool DebuggerObject::isGeneratorFunction() const {
  MOZ_ASSERT(isDebuggeeFunction());

  return referent()->as<JSFunction>().isGenerator();
}

bool DebuggerObject::isClassConstructor() const {
  MOZ_ASSERT(isDebuggeeFunction());

  return referent()->as<JSFunction>().isClassConstructor();
}

bool DebuggerObject::isGlobal() const { return referent()->is<GlobalObject>(); }

bool DebuggerObject::isScriptedProxy() const {
  return js::IsScriptedProxy(referent());
}

bool DebuggerObject::isPromise() const {
  JSObject* referent = this->referent();

  if (IsCrossCompartmentWrapper(referent)) {
    // We only care about promises, so CheckedUnwrapStatic is OK.
    referent = CheckedUnwrapStatic(referent);
    if (!referent) {
      return false;
    }
  }

  return referent->is<PromiseObject>();
}

bool DebuggerObject::isError() const {
  JSObject* referent = this->referent();

  if (IsCrossCompartmentWrapper(referent)) {
    // We only check for error classes, so CheckedUnwrapStatic is OK.
    referent = CheckedUnwrapStatic(referent);
    if (!referent) {
      return false;
    }
  }

  return referent->is<ErrorObject>();
}

/* static */
bool DebuggerObject::getClassName(JSContext* cx, HandleDebuggerObject object,
                                  MutableHandleString result) {
  RootedObject referent(cx, object->referent());

  const char* className;
  {
    Maybe<AutoRealm> ar;
    EnterDebuggeeObjectRealm(cx, ar, referent);
    className = GetObjectClassName(cx, referent);
  }

  JSAtom* str = Atomize(cx, className, strlen(className));
  if (!str) {
    return false;
  }

  result.set(str);
  return true;
}

JSAtom* DebuggerObject::name(JSContext* cx) const {
  MOZ_ASSERT(isFunction());

  JSAtom* atom = referent()->as<JSFunction>().explicitName();
  if (atom) {
    cx->markAtom(atom);
  }
  return atom;
}

JSAtom* DebuggerObject::displayName(JSContext* cx) const {
  MOZ_ASSERT(isFunction());

  JSAtom* atom = referent()->as<JSFunction>().displayAtom();
  if (atom) {
    cx->markAtom(atom);
  }
  return atom;
}

JS::PromiseState DebuggerObject::promiseState() const {
  return promise()->state();
}

double DebuggerObject::promiseLifetime() const { return promise()->lifetime(); }

double DebuggerObject::promiseTimeToResolution() const {
  MOZ_ASSERT(promiseState() != JS::PromiseState::Pending);

  return promise()->timeToResolution();
}

/* static */
bool DebuggerObject::getBoundTargetFunction(
    JSContext* cx, HandleDebuggerObject object,
    MutableHandleDebuggerObject result) {
  MOZ_ASSERT(object->isBoundFunction());

  RootedFunction referent(cx, &object->referent()->as<JSFunction>());
  Debugger* dbg = object->owner();

  RootedObject target(cx, referent->getBoundFunctionTarget());
  return dbg->wrapDebuggeeObject(cx, target, result);
}

/* static */
bool DebuggerObject::getBoundThis(JSContext* cx, HandleDebuggerObject object,
                                  MutableHandleValue result) {
  MOZ_ASSERT(object->isBoundFunction());

  RootedFunction referent(cx, &object->referent()->as<JSFunction>());
  Debugger* dbg = object->owner();

  result.set(referent->getBoundFunctionThis());
  return dbg->wrapDebuggeeValue(cx, result);
}

/* static */
bool DebuggerObject::getBoundArguments(JSContext* cx,
                                       HandleDebuggerObject object,
                                       MutableHandle<ValueVector> result) {
  MOZ_ASSERT(object->isBoundFunction());

  RootedFunction referent(cx, &object->referent()->as<JSFunction>());
  Debugger* dbg = object->owner();

  size_t length = referent->getBoundFunctionArgumentCount();
  if (!result.resize(length)) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    result[i].set(referent->getBoundFunctionArgument(i));
    if (!dbg->wrapDebuggeeValue(cx, result[i])) {
      return false;
    }
  }
  return true;
}

/* static */
SavedFrame* Debugger::getObjectAllocationSite(JSObject& obj) {
  JSObject* metadata = GetAllocationMetadata(&obj);
  if (!metadata) {
    return nullptr;
  }

  MOZ_ASSERT(!metadata->is<WrapperObject>());
  return metadata->is<SavedFrame>() ? &metadata->as<SavedFrame>() : nullptr;
}

/* static */
bool DebuggerObject::getAllocationSite(JSContext* cx,
                                       HandleDebuggerObject object,
                                       MutableHandleObject result) {
  RootedObject referent(cx, object->referent());

  RootedObject allocSite(cx, Debugger::getObjectAllocationSite(*referent));
  if (!cx->compartment()->wrap(cx, &allocSite)) {
    return false;
  }

  result.set(allocSite);
  return true;
}

/* static */
bool DebuggerObject::getErrorReport(JSContext* cx, HandleObject maybeError,
                                    JSErrorReport*& report) {
  JSObject* obj = maybeError;
  if (IsCrossCompartmentWrapper(obj)) {
    /* We only care about Error objects, so CheckedUnwrapStatic is OK. */
    obj = CheckedUnwrapStatic(obj);
  }

  if (!obj) {
    ReportAccessDenied(cx);
    return false;
  }

  if (!obj->is<ErrorObject>()) {
    report = nullptr;
    return true;
  }

  report = obj->as<ErrorObject>().getErrorReport();
  return true;
}

/* static */
bool DebuggerObject::getErrorMessageName(JSContext* cx,
                                         HandleDebuggerObject object,
                                         MutableHandleString result) {
  RootedObject referent(cx, object->referent());
  JSErrorReport* report;
  if (!getErrorReport(cx, referent, report)) {
    return false;
  }

  if (!report || !report->errorMessageName) {
    result.set(nullptr);
    return true;
  }

  RootedString str(cx, JS_NewStringCopyZ(cx, report->errorMessageName));
  if (!str) {
    return false;
  }
  result.set(str);
  return true;
}

/* static */
bool DebuggerObject::getErrorNotes(JSContext* cx, HandleDebuggerObject object,
                                   MutableHandleValue result) {
  RootedObject referent(cx, object->referent());
  JSErrorReport* report;
  if (!getErrorReport(cx, referent, report)) {
    return false;
  }

  if (!report) {
    result.setUndefined();
    return true;
  }

  RootedObject errorNotesArray(cx, CreateErrorNotesArray(cx, report));
  if (!errorNotesArray) {
    return false;
  }

  if (!cx->compartment()->wrap(cx, &errorNotesArray)) {
    return false;
  }
  result.setObject(*errorNotesArray);
  return true;
}

/* static */
bool DebuggerObject::getErrorLineNumber(JSContext* cx,
                                        HandleDebuggerObject object,
                                        MutableHandleValue result) {
  RootedObject referent(cx, object->referent());
  JSErrorReport* report;
  if (!getErrorReport(cx, referent, report)) {
    return false;
  }

  if (!report) {
    result.setUndefined();
    return true;
  }

  result.setNumber(report->lineno);
  return true;
}

/* static */
bool DebuggerObject::getErrorColumnNumber(JSContext* cx,
                                          HandleDebuggerObject object,
                                          MutableHandleValue result) {
  RootedObject referent(cx, object->referent());
  JSErrorReport* report;
  if (!getErrorReport(cx, referent, report)) {
    return false;
  }

  if (!report) {
    result.setUndefined();
    return true;
  }

  result.setNumber(report->column);
  return true;
}

/* static */
bool DebuggerObject::getPromiseValue(JSContext* cx, HandleDebuggerObject object,
                                     MutableHandleValue result) {
  MOZ_ASSERT(object->promiseState() == JS::PromiseState::Fulfilled);

  result.set(object->promise()->value());
  return object->owner()->wrapDebuggeeValue(cx, result);
}

/* static */
bool DebuggerObject::getPromiseReason(JSContext* cx,
                                      HandleDebuggerObject object,
                                      MutableHandleValue result) {
  MOZ_ASSERT(object->promiseState() == JS::PromiseState::Rejected);

  result.set(object->promise()->reason());
  return object->owner()->wrapDebuggeeValue(cx, result);
}

/* static */
bool DebuggerObject::isExtensible(JSContext* cx, HandleDebuggerObject object,
                                  bool& result) {
  RootedObject referent(cx, object->referent());

  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  ErrorCopier ec(ar);
  return IsExtensible(cx, referent, &result);
}

/* static */
bool DebuggerObject::isSealed(JSContext* cx, HandleDebuggerObject object,
                              bool& result) {
  RootedObject referent(cx, object->referent());

  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  ErrorCopier ec(ar);
  return TestIntegrityLevel(cx, referent, IntegrityLevel::Sealed, &result);
}

/* static */
bool DebuggerObject::isFrozen(JSContext* cx, HandleDebuggerObject object,
                              bool& result) {
  RootedObject referent(cx, object->referent());

  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  ErrorCopier ec(ar);
  return TestIntegrityLevel(cx, referent, IntegrityLevel::Frozen, &result);
}

/* static */
bool DebuggerObject::getPrototypeOf(JSContext* cx, HandleDebuggerObject object,
                                    MutableHandleDebuggerObject result) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  RootedObject proto(cx);
  {
    Maybe<AutoRealm> ar;
    EnterDebuggeeObjectRealm(cx, ar, referent);
    if (!GetPrototype(cx, referent, &proto)) {
      return false;
    }
  }

  return dbg->wrapNullableDebuggeeObject(cx, proto, result);
}

/* static */
bool DebuggerObject::getOwnPropertyNames(JSContext* cx,
                                         HandleDebuggerObject object,
                                         MutableHandle<IdVector> result) {
  RootedObject referent(cx, object->referent());

  RootedIdVector ids(cx);
  {
    Maybe<AutoRealm> ar;
    EnterDebuggeeObjectRealm(cx, ar, referent);

    ErrorCopier ec(ar);
    if (!GetPropertyKeys(cx, referent, JSITER_OWNONLY | JSITER_HIDDEN, &ids)) {
      return false;
    }
  }

  for (size_t i = 0; i < ids.length(); i++) {
    cx->markId(ids[i]);
  }

  return result.append(ids.begin(), ids.end());
}

bool GetSymbolPropertyKeys(JSContext* cx, HandleDebuggerObject object,
                           JS::MutableHandleIdVector props,
                           bool includePrivate) {
  RootedObject referent(cx, object->referent());

  {
    Maybe<AutoRealm> ar;
    EnterDebuggeeObjectRealm(cx, ar, referent);

    ErrorCopier ec(ar);

    unsigned flags =
        JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS | JSITER_SYMBOLSONLY;
    if (includePrivate) {
      flags = flags | JSITER_PRIVATE;
    }
    if (!GetPropertyKeys(cx, referent, flags, props)) {
      return false;
    }
  }

  return true;
}

/* static */
bool DebuggerObject::getOwnPropertySymbols(JSContext* cx,
                                           HandleDebuggerObject object,
                                           MutableHandle<IdVector> result) {
  RootedIdVector ids(cx);
  if (!GetSymbolPropertyKeys(cx, object, &ids, false)) {
    return false;
  }

  for (size_t i = 0; i < ids.length(); i++) {
    cx->markId(ids[i]);
  }

  return result.append(ids.begin(), ids.end());
}

/* static */
bool DebuggerObject::getOwnPrivateProperties(JSContext* cx,
                                             HandleDebuggerObject object,
                                             MutableHandle<IdVector> result) {
  RootedIdVector ids(cx);
  if (!GetSymbolPropertyKeys(cx, object, &ids, true)) {
    return false;
  }

  for (size_t i = 0; i < ids.length(); i++) {
    PropertyKey id = ids[i];

    if (id.isPrivateName()) {
      // Private *methods* create a Private Brand, a special private name
      // stamped onto the symbol, to indicate it is possible to execute private
      // methods from the class on this object. We don't want to return such
      // items here, so we check if we're dealing with a private property, e.g.
      // the Symbol description starts with a "#" character
      JSAtom* privateDescription = id.toSymbol()->description();
      char16_t firstChar;
      if (!privateDescription->getChar(cx, 0, &firstChar)) {
        return false;
      }

      if (firstChar == '#') {
        cx->markId(id);
        if (!result.append(id)) {
          return false;
        }
      }
    }
  }

  return true;
}

/* static */
bool DebuggerObject::getOwnPropertyDescriptor(
    JSContext* cx, HandleDebuggerObject object, HandleId id,
    MutableHandle<Maybe<PropertyDescriptor>> desc_) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  // Bug: This can cause the debuggee to run!
  {
    Maybe<AutoRealm> ar;
    EnterDebuggeeObjectRealm(cx, ar, referent);

    cx->markId(id);

    ErrorCopier ec(ar);
    if (!GetOwnPropertyDescriptor(cx, referent, id, desc_)) {
      return false;
    }
  }

  if (desc_.isSome()) {
    Rooted<PropertyDescriptor> desc(cx, *desc_);

    if (desc.hasValue()) {
      // Rewrap the debuggee values in desc for the debugger.
      if (!dbg->wrapDebuggeeValue(cx, desc.value())) {
        return false;
      }
    }
    if (desc.hasGetter()) {
      RootedValue get(cx, ObjectOrNullValue(desc.getter()));
      if (!dbg->wrapDebuggeeValue(cx, &get)) {
        return false;
      }
      desc.setGetter(get.toObjectOrNull());
    }
    if (desc.hasSetter()) {
      RootedValue set(cx, ObjectOrNullValue(desc.setter()));
      if (!dbg->wrapDebuggeeValue(cx, &set)) {
        return false;
      }
      desc.setSetter(set.toObjectOrNull());
    }

    desc_.set(mozilla::Some(desc.get()));
  }

  return true;
}

/* static */
bool DebuggerObject::preventExtensions(JSContext* cx,
                                       HandleDebuggerObject object) {
  RootedObject referent(cx, object->referent());

  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  ErrorCopier ec(ar);
  return PreventExtensions(cx, referent);
}

/* static */
bool DebuggerObject::seal(JSContext* cx, HandleDebuggerObject object) {
  RootedObject referent(cx, object->referent());

  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  ErrorCopier ec(ar);
  return SetIntegrityLevel(cx, referent, IntegrityLevel::Sealed);
}

/* static */
bool DebuggerObject::freeze(JSContext* cx, HandleDebuggerObject object) {
  RootedObject referent(cx, object->referent());

  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  ErrorCopier ec(ar);
  return SetIntegrityLevel(cx, referent, IntegrityLevel::Frozen);
}

/* static */
bool DebuggerObject::defineProperty(JSContext* cx, HandleDebuggerObject object,
                                    HandleId id,
                                    Handle<PropertyDescriptor> desc_) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  Rooted<PropertyDescriptor> desc(cx, desc_);
  if (!dbg->unwrapPropertyDescriptor(cx, referent, &desc)) {
    return false;
  }
  JS_TRY_OR_RETURN_FALSE(cx, CheckPropertyDescriptorAccessors(cx, desc));

  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  if (!cx->compartment()->wrap(cx, &desc)) {
    return false;
  }
  cx->markId(id);

  ErrorCopier ec(ar);
  return DefineProperty(cx, referent, id, desc);
}

/* static */
bool DebuggerObject::defineProperties(JSContext* cx,
                                      HandleDebuggerObject object,
                                      Handle<IdVector> ids,
                                      Handle<PropertyDescriptorVector> descs_) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  Rooted<PropertyDescriptorVector> descs(cx, PropertyDescriptorVector(cx));
  if (!descs.append(descs_.begin(), descs_.end())) {
    return false;
  }
  for (size_t i = 0; i < descs.length(); i++) {
    if (!dbg->unwrapPropertyDescriptor(cx, referent, descs[i])) {
      return false;
    }
    JS_TRY_OR_RETURN_FALSE(cx, CheckPropertyDescriptorAccessors(cx, descs[i]));
  }

  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  for (size_t i = 0; i < descs.length(); i++) {
    if (!cx->compartment()->wrap(cx, descs[i])) {
      return false;
    }
    cx->markId(ids[i]);
  }

  ErrorCopier ec(ar);
  for (size_t i = 0; i < descs.length(); i++) {
    if (!DefineProperty(cx, referent, ids[i], descs[i])) {
      return false;
    }
  }

  return true;
}

/* static */
bool DebuggerObject::deleteProperty(JSContext* cx, HandleDebuggerObject object,
                                    HandleId id, ObjectOpResult& result) {
  RootedObject referent(cx, object->referent());

  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  cx->markId(id);

  ErrorCopier ec(ar);
  return DeleteProperty(cx, referent, id, result);
}

/* static */
Result<Completion> DebuggerObject::getProperty(JSContext* cx,
                                               HandleDebuggerObject object,
                                               HandleId id,
                                               HandleValue receiver_) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  // Unwrap Debugger.Objects. This happens in the debugger's compartment since
  // that is where any exceptions must be reported.
  RootedValue receiver(cx, receiver_);
  if (!dbg->unwrapDebuggeeValue(cx, &receiver)) {
    return cx->alreadyReportedError();
  }

  // Enter the debuggee compartment and rewrap all input value for that
  // compartment. (Rewrapping always takes place in the destination
  // compartment.)
  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);
  if (!cx->compartment()->wrap(cx, &referent) ||
      !cx->compartment()->wrap(cx, &receiver)) {
    return cx->alreadyReportedError();
  }
  cx->markId(id);

  LeaveDebuggeeNoExecute nnx(cx);

  RootedValue result(cx);
  bool ok = GetProperty(cx, referent, receiver, id, &result);
  return Completion::fromJSResult(cx, ok, result);
}

/* static */
Result<Completion> DebuggerObject::setProperty(JSContext* cx,
                                               HandleDebuggerObject object,
                                               HandleId id, HandleValue value_,
                                               HandleValue receiver_) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  // Unwrap Debugger.Objects. This happens in the debugger's compartment since
  // that is where any exceptions must be reported.
  RootedValue value(cx, value_);
  RootedValue receiver(cx, receiver_);
  if (!dbg->unwrapDebuggeeValue(cx, &value) ||
      !dbg->unwrapDebuggeeValue(cx, &receiver)) {
    return cx->alreadyReportedError();
  }

  // Enter the debuggee compartment and rewrap all input value for that
  // compartment. (Rewrapping always takes place in the destination
  // compartment.)
  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);
  if (!cx->compartment()->wrap(cx, &referent) ||
      !cx->compartment()->wrap(cx, &value) ||
      !cx->compartment()->wrap(cx, &receiver)) {
    return cx->alreadyReportedError();
  }
  cx->markId(id);

  LeaveDebuggeeNoExecute nnx(cx);

  ObjectOpResult opResult;
  bool ok = SetProperty(cx, referent, id, value, receiver, opResult);

  return Completion::fromJSResult(cx, ok, BooleanValue(ok && opResult.ok()));
}

/* static */
Maybe<Completion> DebuggerObject::call(JSContext* cx,
                                       HandleDebuggerObject object,
                                       HandleValue thisv_,
                                       Handle<ValueVector> args) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  if (!referent->isCallable()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "Debugger.Object",
                              "call", referent->getClass()->name);
    return Nothing();
  }

  RootedValue calleev(cx, ObjectValue(*referent));

  // Unwrap Debugger.Objects. This happens in the debugger's compartment since
  // that is where any exceptions must be reported.
  RootedValue thisv(cx, thisv_);
  if (!dbg->unwrapDebuggeeValue(cx, &thisv)) {
    return Nothing();
  }
  Rooted<ValueVector> args2(cx, ValueVector(cx));
  if (!args2.append(args.begin(), args.end())) {
    return Nothing();
  }
  for (size_t i = 0; i < args2.length(); ++i) {
    if (!dbg->unwrapDebuggeeValue(cx, args2[i])) {
      return Nothing();
    }
  }

  // Enter the debuggee compartment and rewrap all input value for that
  // compartment. (Rewrapping always takes place in the destination
  // compartment.)
  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);
  if (!cx->compartment()->wrap(cx, &calleev) ||
      !cx->compartment()->wrap(cx, &thisv)) {
    return Nothing();
  }
  for (size_t i = 0; i < args2.length(); ++i) {
    if (!cx->compartment()->wrap(cx, args2[i])) {
      return Nothing();
    }
  }

  // Call the function.
  LeaveDebuggeeNoExecute nnx(cx);

  RootedValue result(cx);
  bool ok;
  {
    InvokeArgs invokeArgs(cx);

    ok = invokeArgs.init(cx, args2.length());
    if (ok) {
      for (size_t i = 0; i < args2.length(); ++i) {
        invokeArgs[i].set(args2[i]);
      }

      ok = js::Call(cx, calleev, thisv, invokeArgs, &result);
    }
  }

  Rooted<Completion> completion(cx, Completion::fromJSResult(cx, ok, result));
  ar.reset();
  return Some(std::move(completion.get()));
}

/* static */
bool DebuggerObject::forceLexicalInitializationByName(
    JSContext* cx, HandleDebuggerObject object, HandleId id, bool& result) {
  if (!JSID_IS_STRING(id)) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
        "Debugger.Object.prototype.forceLexicalInitializationByName", "string",
        InformalValueTypeName(IdToValue(id)));
    return false;
  }

  MOZ_ASSERT(object->isGlobal());

  Rooted<GlobalObject*> referent(cx, &object->referent()->as<GlobalObject>());

  // Shape::search can end up allocating a new BaseShape in Shape::cachify so
  // we need to be in the right compartment here.
  Maybe<AutoRealm> ar;
  EnterDebuggeeObjectRealm(cx, ar, referent);

  RootedObject globalLexical(cx, &referent->lexicalEnvironment());
  RootedObject pobj(cx);
  PropertyResult prop;
  if (!LookupProperty(cx, globalLexical, id, &pobj, &prop)) {
    return false;
  }

  result = false;
  if (prop.isFound()) {
    MOZ_ASSERT(prop.isNativeProperty());
    PropertyInfo propInfo = prop.propertyInfo();
    Value v = globalLexical->as<NativeObject>().getSlot(propInfo.slot());
    if (propInfo.isDataProperty() && v.isMagic() &&
        v.whyMagic() == JS_UNINITIALIZED_LEXICAL) {
      globalLexical->as<NativeObject>().setSlot(propInfo.slot(),
                                                UndefinedValue());
      result = true;
    }
  }

  return true;
}

/* static */
Result<Completion> DebuggerObject::executeInGlobal(
    JSContext* cx, HandleDebuggerObject object,
    mozilla::Range<const char16_t> chars, HandleObject bindings,
    const EvalOptions& options) {
  MOZ_ASSERT(object->isGlobal());

  Rooted<GlobalObject*> referent(cx, &object->referent()->as<GlobalObject>());
  Debugger* dbg = object->owner();

  RootedObject globalLexical(cx, &referent->lexicalEnvironment());
  return DebuggerGenericEval(cx, chars, bindings, options, dbg, globalLexical,
                             nullptr);
}

/* static */
bool DebuggerObject::makeDebuggeeValue(JSContext* cx,
                                       HandleDebuggerObject object,
                                       HandleValue value_,
                                       MutableHandleValue result) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  RootedValue value(cx, value_);

  // Non-objects are already debuggee values.
  if (value.isObject()) {
    // Enter this Debugger.Object's referent's compartment, and wrap the
    // argument as appropriate for references from there.
    {
      Maybe<AutoRealm> ar;
      EnterDebuggeeObjectRealm(cx, ar, referent);
      if (!cx->compartment()->wrap(cx, &value)) {
        return false;
      }
    }

    // Back in the debugger's compartment, produce a new Debugger.Object
    // instance referring to the wrapped argument.
    if (!dbg->wrapDebuggeeValue(cx, &value)) {
      return false;
    }
  }

  result.set(value);
  return true;
}

static JSFunction* EnsureNativeFunction(const Value& value,
                                        bool allowExtended = true) {
  if (!value.isObject() || !value.toObject().is<JSFunction>()) {
    return nullptr;
  }

  JSFunction* fun = &value.toObject().as<JSFunction>();
  if (!fun->isNativeFun() || (fun->isExtended() && !allowExtended)) {
    return nullptr;
  }

  return fun;
}

/* static */
bool DebuggerObject::makeDebuggeeNativeFunction(JSContext* cx,
                                                HandleDebuggerObject object,
                                                HandleValue value,
                                                MutableHandleValue result) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  // The logic below doesn't work with extended functions, so do not allow them.
  RootedFunction fun(cx, EnsureNativeFunction(value,
                                              /* allowExtended */ false));
  if (!fun) {
    JS_ReportErrorASCII(cx, "Need native function");
    return false;
  }

  RootedValue newValue(cx);
  {
    Maybe<AutoRealm> ar;
    EnterDebuggeeObjectRealm(cx, ar, referent);

    unsigned nargs = fun->nargs();
    RootedAtom name(cx, fun->displayAtom());
    if (name) {
      cx->markAtom(name);
    }
    JSFunction* newFun = NewNativeFunction(cx, fun->native(), nargs, name);
    if (!newFun) {
      return false;
    }

    newValue.setObject(*newFun);
  }

  // Back in the debugger's compartment, produce a new Debugger.Object
  // instance referring to the wrapped argument.
  if (!dbg->wrapDebuggeeValue(cx, &newValue)) {
    return false;
  }

  result.set(newValue);
  return true;
}

static JSAtom* MaybeGetSelfHostedFunctionName(const Value& v) {
  if (!v.isObject() || !v.toObject().is<JSFunction>()) {
    return nullptr;
  }

  JSFunction* fun = &v.toObject().as<JSFunction>();
  if (!fun->isSelfHostedBuiltin()) {
    return nullptr;
  }

  return GetClonedSelfHostedFunctionName(fun);
}

/* static */
bool DebuggerObject::isSameNative(JSContext* cx, HandleDebuggerObject object,
                                  HandleValue value,
                                  MutableHandleValue result) {
  RootedValue referentValue(cx, ObjectValue(*object->referent()));

  RootedValue nonCCWValue(
      cx, value.isObject() ? ObjectValue(*UncheckedUnwrap(&value.toObject()))
                           : value);

  RootedFunction fun(cx, EnsureNativeFunction(nonCCWValue));
  if (!fun) {
    RootedAtom selfHostedName(cx, MaybeGetSelfHostedFunctionName(nonCCWValue));
    if (!selfHostedName) {
      JS_ReportErrorASCII(cx, "Need native function");
      return false;
    }

    result.setBoolean(selfHostedName ==
                      MaybeGetSelfHostedFunctionName(referentValue));
    return true;
  }

  RootedFunction referentFun(cx, EnsureNativeFunction(referentValue));
  result.setBoolean(referentFun && referentFun->native() == fun->native());
  return true;
}

/* static */
bool DebuggerObject::unsafeDereference(JSContext* cx,
                                       HandleDebuggerObject object,
                                       MutableHandleObject result) {
  RootedObject referent(cx, object->referent());

  if (!cx->compartment()->wrap(cx, &referent)) {
    return false;
  }

  // Wrapping should return the WindowProxy.
  MOZ_ASSERT(!IsWindow(referent));

  result.set(referent);
  return true;
}

/* static */
bool DebuggerObject::unwrap(JSContext* cx, HandleDebuggerObject object,
                            MutableHandleDebuggerObject result) {
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();

  RootedObject unwrapped(cx, UnwrapOneCheckedStatic(referent));

  // Don't allow unwrapping to create a D.O whose referent is in an
  // invisible-to-Debugger compartment. (If our referent is a *wrapper* to such,
  // and the wrapper is in a visible compartment, that's fine.)
  if (unwrapped && unwrapped->compartment()->invisibleToDebugger()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_INVISIBLE_COMPARTMENT);
    return false;
  }

  return dbg->wrapNullableDebuggeeObject(cx, unwrapped, result);
}

/* static */
bool DebuggerObject::requireGlobal(JSContext* cx, HandleDebuggerObject object) {
  if (!object->isGlobal()) {
    RootedObject referent(cx, object->referent());

    const char* isWrapper = "";
    const char* isWindowProxy = "";

    // Help the poor programmer by pointing out wrappers around globals...
    if (referent->is<WrapperObject>()) {
      referent = js::UncheckedUnwrap(referent);
      isWrapper = "a wrapper around ";
    }

    // ... and WindowProxies around Windows.
    if (IsWindowProxy(referent)) {
      referent = ToWindowIfWindowProxy(referent);
      isWindowProxy = "a WindowProxy referring to ";
    }

    RootedValue dbgobj(cx, ObjectValue(*object));
    if (referent->is<GlobalObject>()) {
      ReportValueError(cx, JSMSG_DEBUG_WRAPPER_IN_WAY, JSDVG_SEARCH_STACK,
                       dbgobj, nullptr, isWrapper, isWindowProxy);
    } else {
      ReportValueError(cx, JSMSG_DEBUG_BAD_REFERENT, JSDVG_SEARCH_STACK, dbgobj,
                       nullptr, "a global object");
    }
    return false;
  }

  return true;
}

/* static */
bool DebuggerObject::requirePromise(JSContext* cx,
                                    HandleDebuggerObject object) {
  RootedObject referent(cx, object->referent());

  if (IsCrossCompartmentWrapper(referent)) {
    /* We only care about promises, so CheckedUnwrapStatic is OK. */
    referent = CheckedUnwrapStatic(referent);
    if (!referent) {
      ReportAccessDenied(cx);
      return false;
    }
  }

  if (!referent->is<PromiseObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, "Debugger", "Promise",
                              object->getClass()->name);
    return false;
  }

  return true;
}

/* static */
bool DebuggerObject::getScriptedProxyTarget(
    JSContext* cx, HandleDebuggerObject object,
    MutableHandleDebuggerObject result) {
  MOZ_ASSERT(object->isScriptedProxy());
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();
  RootedObject unwrapped(cx, js::GetProxyTargetObject(referent));

  return dbg->wrapNullableDebuggeeObject(cx, unwrapped, result);
}

/* static */
bool DebuggerObject::getScriptedProxyHandler(
    JSContext* cx, HandleDebuggerObject object,
    MutableHandleDebuggerObject result) {
  MOZ_ASSERT(object->isScriptedProxy());
  RootedObject referent(cx, object->referent());
  Debugger* dbg = object->owner();
  RootedObject unwrapped(cx, ScriptedProxyHandler::handlerObject(referent));
  return dbg->wrapNullableDebuggeeObject(cx, unwrapped, result);
}
