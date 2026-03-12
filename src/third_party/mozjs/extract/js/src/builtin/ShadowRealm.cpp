/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/ShadowRealm.h"

#include "mozilla/Assertions.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "builtin/ModuleObject.h"
#include "builtin/Promise.h"
#include "builtin/WrappedFunctionObject.h"
#include "frontend/BytecodeCompiler.h"  // CompileEvalScript
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/GlobalObject.h"
#include "js/Principals.h"
#include "js/Promise.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/ShadowRealmCallbacks.h"
#include "js/SourceText.h"
#include "js/StableStringChars.h"
#include "js/StructuredClone.h"
#include "js/TypeDecls.h"
#include "js/Wrapper.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSObject.h"
#include "vm/ObjectOperations.h"

#include "builtin/HandlerFunction-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Realm-inl.h"

using namespace js;

using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::SourceText;

static JSObject* DefaultNewShadowRealmGlobal(JSContext* cx,
                                             JS::RealmOptions& options,
                                             JSPrincipals* principals,
                                             Handle<JSObject*> unused) {
  static const JSClass shadowRealmGlobal = {
      "ShadowRealmGlobal",
      JSCLASS_GLOBAL_FLAGS,
      &JS::DefaultGlobalClassOps,
  };

  return JS_NewGlobalObject(cx, &shadowRealmGlobal, principals,
                            JS::FireOnNewGlobalHook, options);
}

// https://tc39.es/proposal-shadowrealm/#sec-shadowrealm-constructor
/*static*/
bool ShadowRealmObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. If NewTarget is undefined, throw a TypeError exception.
  if (!args.isConstructing()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_CONSTRUCTOR, "ShadowRealm");
    return false;
  }

  // Step 2. Let O be ? OrdinaryCreateFromConstructor(NewTarget,
  // "%ShadowRealm.prototype%", « [[ShadowRealm]], [[ExecutionContext]] »).
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_ShadowRealm,
                                          &proto)) {
    return false;
  }

  Rooted<ShadowRealmObject*> shadowRealmObj(
      cx, NewObjectWithClassProto<ShadowRealmObject>(cx, proto));
  if (!shadowRealmObj) {
    return false;
  }

  // Instead of managing Realms, spidermonkey associates a realm with a global
  // object, and so we will manage and store a global.

  // Step 3. Let realmRec be CreateRealm().

  // Initially steal creation options from current realm:
  JS::RealmOptions options(cx->realm()->creationOptions(),
                           cx->realm()->behaviors());

  // We don't want to have to deal with CCWs in addition to
  // WrappedFunctionObjects.
  options.creationOptions().setExistingCompartment(cx->compartment());

  JS::GlobalCreationCallback newGlobal =
      cx->runtime()->getShadowRealmGlobalCreationCallback();
  // If an embedding didn't provide a callback to initialize the global,
  // use the basic default one.
  if (!newGlobal) {
    newGlobal = DefaultNewShadowRealmGlobal;
  }

  // Our shadow realm inherits the principals of the current realm,
  // but is otherwise constrained.
  JSPrincipals* principals = JS::GetRealmPrincipals(cx->realm());

  // Steps 5-11: In SpiderMonkey these fall under the aegis of the global
  //             creation. It's worth noting that the newGlobal callback
  //             needs to respect the SetRealmGlobalObject call below, which
  //             sets the global to
  //             OrdinaryObjectCreate(intrinsics.[[%Object.prototype%]]).
  //
  // Step 5. Let context be a new execution context.
  // Step 6. Set the Function of context to null.
  // Step 7. Set the Realm of context to realmRec.
  // Step 8. Set the ScriptOrModule of context to null.
  // Step 9. Set O.[[ExecutionContext]] to context.
  // Step 10. Perform ? SetRealmGlobalObject(realmRec, undefined, undefined).
  // Step 11. Perform ? SetDefaultGlobalBindings(O.[[ShadowRealm]]).
  Rooted<JSObject*> global(cx,
                           newGlobal(cx, options, principals, cx->global()));
  if (!global) {
    return false;
  }

  // Make sure the new global hook obeyed our request in the
  // creation options to have a same compartment global.
  MOZ_RELEASE_ASSERT(global->compartment() == cx->compartment());

  // Step 4. Set O.[[ShadowRealm]] to realmRec.
  shadowRealmObj->initFixedSlot(GlobalSlot, ObjectValue(*global));

  // Step 12. Perform ? HostInitializeShadowRealm(O.[[ShadowRealm]]).
  JS::GlobalInitializeCallback hostInitializeShadowRealm =
      cx->runtime()->getShadowRealmInitializeGlobalCallback();
  if (hostInitializeShadowRealm) {
    if (!hostInitializeShadowRealm(cx, global)) {
      return false;
    }
  }

  // Step 13. Return O.
  args.rval().setObject(*shadowRealmObj);
  return true;
}

// https://tc39.es/proposal-shadowrealm/#sec-validateshadowrealmobject
// (slightly modified into a cast operator too)
static ShadowRealmObject* ValidateShadowRealmObject(JSContext* cx,
                                                    Handle<Value> value) {
  // Step 1. Perform ? RequireInternalSlot(O, [[ShadowRealm]]).
  // Step 2. Perform ? RequireInternalSlot(O, [[ExecutionContext]]).
  return UnwrapAndTypeCheckValue<ShadowRealmObject>(cx, value, [cx]() {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_SHADOW_REALM);
  });
}

void js::ReportPotentiallyDetailedMessage(JSContext* cx,
                                          const unsigned detailedError,
                                          const unsigned genericError) {
  // Return for non-catchable exceptions like interrupt requests.
  if (!cx->isExceptionPending()) {
    return;
  }

  Rooted<Value> exception(cx);
  if (!cx->getPendingException(&exception)) {
    return;
  }
  cx->clearPendingException();

  JS::ErrorReportBuilder jsReport(cx);
  JS::ExceptionStack exnStack(cx, exception, nullptr);
  if (!jsReport.init(cx, exnStack, JS::ErrorReportBuilder::NoSideEffects)) {
    cx->clearPendingException();
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, genericError);
    return;
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, detailedError,
                           jsReport.toStringResult().c_str());
}

//  PerformShadowRealmEval ( sourceText: a String, callerRealm: a Realm Record,
//                          evalRealm: a Realm Record, )
//
// https://tc39.es/proposal-shadowrealm/#sec-performshadowrealmeval
static bool PerformShadowRealmEval(JSContext* cx, Handle<JSString*> sourceText,
                                   Realm* callerRealm, Realm* evalRealm,
                                   MutableHandle<Value> rval) {
  MOZ_ASSERT(callerRealm != evalRealm);

  // Step 1. Perform ? HostEnsureCanCompileStrings(callerRealm, evalRealm).
  JS::RootedVector<JSString*> parameterStrings(cx);
  JS::RootedVector<Value> parameterArgs(cx);
  bool canCompileStrings = false;
  if (!cx->isRuntimeCodeGenEnabled(JS::RuntimeCode::JS, sourceText,
                                   JS::CompilationType::Undefined,
                                   parameterStrings, nullptr, parameterArgs,
                                   NullHandleValue, &canCompileStrings)) {
    return false;
  }
  if (!canCompileStrings) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_SHADOWREALM);
    return false;
  }

  // Need to compile the script into the realm we will execute into.
  //
  // We hoist the error handling out however to ensure that errors
  // are thrown from the correct realm.
  bool compileSuccess = false;
  bool evalSuccess = false;

  do {
    Rooted<GlobalObject*> evalRealmGlobal(cx, evalRealm->maybeGlobal());
    AutoRealm ar(cx, evalRealmGlobal);

    // Step 2. Perform the following substeps in an implementation-defined
    // order, possibly interleaving parsing and error detection:
    //     a. Let script be ParseText(! StringToCodePoints(sourceText), Script).
    //     b. If script is a List of errors, throw a SyntaxError exception.
    //     c. If script Contains ScriptBody is false, return undefined.
    //     d. Let body be the ScriptBody of script.
    //     e. If body Contains NewTarget is true, throw a SyntaxError exception.
    //     f. If body Contains SuperProperty is true, throw a SyntaxError
    //     exception. g. If body Contains SuperCall is true, throw a SyntaxError
    //     exception.

    AutoStableStringChars linearChars(cx);
    if (!linearChars.initTwoByte(cx, sourceText)) {
      return false;
    }
    SourceText<char16_t> srcBuf;
    if (!srcBuf.initMaybeBorrowed(cx, linearChars)) {
      return false;
    }

    // Lets propagate some information into the compilation here.
    //
    // We may need to censor the stacks eventually, see
    // https://bugzilla.mozilla.org/show_bug.cgi?id=1770017
    RootedScript callerScript(cx);
    const char* filename;
    uint32_t lineno;
    uint32_t pcOffset;
    bool mutedErrors;
    DescribeScriptedCallerForCompilation(cx, &callerScript, &filename, &lineno,
                                         &pcOffset, &mutedErrors);

    CompileOptions options(cx);
    options.setIsRunOnce(true)
        .setNoScriptRval(false)
        .setMutedErrors(mutedErrors)
        .setFileAndLine(filename, lineno);

    Rooted<Scope*> enclosing(cx, &evalRealmGlobal->emptyGlobalScope());
    RootedScript script(
        cx, frontend::CompileEvalScript(cx, options, srcBuf, enclosing,
                                        evalRealmGlobal));

    compileSuccess = !!script;
    if (!compileSuccess) {
      break;
    }

    // Step 3. Let strictEval be IsStrict of script.
    // Step 4. Let runningContext be the running execution context.
    // Step 5. Let lexEnv be NewDeclarativeEnvironment(evalRealm.[[GlobalEnv]]).
    // Step 6. Let varEnv be evalRealm.[[GlobalEnv]].
    // Step 7. If strictEval is true, set varEnv to lexEnv.
    // Step 8. If runningContext is not already suspended, suspend
    // runningContext. Step 9. Let evalContext be a new ECMAScript code
    // execution context. Step 10. Set evalContext's Function to null. Step 11.
    // Set evalContext's Realm to evalRealm. Step 12. Set evalContext's
    // ScriptOrModule to null. Step 13. Set evalContext's VariableEnvironment to
    // varEnv. Step 14. Set evalContext's LexicalEnvironment to lexEnv. Step 15.
    // Push evalContext onto the execution context stack; evalContext is
    //          now the running execution context.
    // Step 16. Let result be  EvalDeclarationInstantiation(body, varEnv,
    //            lexEnv,  null, strictEval).
    // Step 17. If result.[[Type]] is normal, then
    //     a. Set result to the result of evaluating body.
    // Step 18. If result.[[Type]] is normal and result.[[Value]] is empty, then
    //     a. Set result to NormalCompletion(undefined).

    // Step 19. Suspend evalContext and remove it from the execution context
    // stack.
    // Step 20. Resume the context that is now on the top of the execution
    // context stack as the running execution context.
    Rooted<JSObject*> environment(cx, &evalRealmGlobal->lexicalEnvironment());
    evalSuccess = ExecuteKernel(cx, script, environment,
                                /* evalInFrame = */ NullFramePtr(), rval);
  } while (false);  // AutoRealm

  if (!compileSuccess) {
    if (!cx->isExceptionPending()) {
      return false;
    }

    // Clone the exception into the current global and re-throw, as the
    // exception has to come from the current global.
    Rooted<Value> exception(cx);
    if (!cx->getPendingException(&exception)) {
      return false;
    }

    // Clear our exception now that we've got it, so that we don't
    // do the following call with an exception already pending.
    cx->clearPendingException();

    Rooted<Value> clonedException(cx);
    if (!JS_StructuredClone(cx, exception, &clonedException, nullptr,
                            nullptr)) {
      return false;
    }

    cx->setPendingException(clonedException, ShouldCaptureStack::Always);
    return false;
  }

  if (!evalSuccess) {
    // Step 21. If result.[[Type]]  is not normal, throw a TypeError
    // exception.
    //
    // The type error here needs to come from the calling global, so has to
    // happen outside the AutoRealm above.
    ReportPotentiallyDetailedMessage(cx,
                                     JSMSG_SHADOW_REALM_EVALUATE_FAILURE_DETAIL,
                                     JSMSG_SHADOW_REALM_EVALUATE_FAILURE);

    return false;
  }

  // Wrap |rval| into the current compartment.
  if (!cx->compartment()->wrap(cx, rval)) {
    return false;
  }

  // Step 22. Return ? GetWrappedValue(callerRealm, result.[[Value]]).
  return GetWrappedValue(cx, callerRealm, rval, rval);
}

// ShadowRealm.prototype.evaluate ( sourceText )
// https://tc39.es/proposal-shadowrealm/#sec-shadowrealm.prototype.evaluate
static bool ShadowRealm_evaluate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. Let O be this value.
  HandleValue obj = args.thisv();

  // Step 2. Perform ? ValidateShadowRealmObject(O)
  Rooted<ShadowRealmObject*> shadowRealm(cx,
                                         ValidateShadowRealmObject(cx, obj));
  if (!shadowRealm) {
    return false;
  }

  // Step 3. If Type(sourceText) is not String, throw a TypeError exception.
  if (!args.get(0).isString()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SHADOW_REALM_EVALUATE_NOT_STRING);
    return false;
  }
  Rooted<JSString*> sourceText(cx, args.get(0).toString());

  // Step 4. Let callerRealm be the current Realm Record.
  Realm* callerRealm = cx->realm();

  // Step 5. Let evalRealm be O.[[ShadowRealm]].
  Realm* evalRealm = shadowRealm->getShadowRealm();
  // Step 6. Return ? PerformShadowRealmEval(sourceText, callerRealm,
  // evalRealm).
  return PerformShadowRealmEval(cx, sourceText, callerRealm, evalRealm,
                                args.rval());
}

enum class ImportValueIndices : uint32_t {
  CalleRealm = 0,

  ExportNameString,

  Length,
};

// MG:XXX: Cribbed/Overlapping with StartDynamicModuleImport; may need to
// refactor to share.
// https://tc39.es/proposal-shadowrealm/#sec-shadowrealmimportvalue
static JSObject* ShadowRealmImportValue(JSContext* cx,
                                        Handle<JSString*> specifierString,
                                        Handle<JSString*> exportName,
                                        Realm* callerRealm, Realm* evalRealm) {
  // Step 1. Assert: evalContext is an execution context associated to a
  // ShadowRealm instance's [[ExecutionContext]].

  // Step 2. Let innerCapability be ! NewPromiseCapability(%Promise%).
  Rooted<JSObject*> promiseConstructor(cx, JS::GetPromiseConstructor(cx));
  if (!promiseConstructor) {
    return nullptr;
  }

  Rooted<JSObject*> promiseObject(cx, JS::NewPromiseObject(cx, nullptr));
  if (!promiseObject) {
    return nullptr;
  }

  Handle<PromiseObject*> promise = promiseObject.as<PromiseObject>();

  JS::ModuleDynamicImportHook importHook =
      cx->runtime()->moduleDynamicImportHook;

  if (!importHook) {
    // Dynamic import can be disabled by a pref and is not supported in all
    // contexts (e.g. web workers).
    JS_ReportErrorASCII(
        cx,
        "Dynamic module import is disabled or not supported in this context");
    if (!RejectPromiseWithPendingError(cx, promise)) {
      return nullptr;
    }
    return promise;
  }

  {
    // Step 3. Let runningContext be the running execution context. (Implicit)
    // Step 4. If runningContext is not already suspended, suspend
    //         runningContext. (Implicit)
    // Step 5. Push evalContext onto the execution context stack; evalContext is
    //         now the running execution context. (Implicit)
    Rooted<GlobalObject*> evalRealmGlobal(cx, evalRealm->maybeGlobal());
    AutoRealm ar(cx, evalRealmGlobal);

    // Not Speced: Get referencing private to pass to importHook.
    RootedScript script(cx);
    const char* filename;
    uint32_t lineno;
    uint32_t pcOffset;
    bool mutedErrors;
    DescribeScriptedCallerForCompilation(cx, &script, &filename, &lineno,
                                         &pcOffset, &mutedErrors);

    MOZ_ASSERT(script);

    Rooted<JSAtom*> specifierAtom(cx, AtomizeString(cx, specifierString));
    if (!specifierAtom) {
      if (!RejectPromiseWithPendingError(cx, promise)) {
        return nullptr;
      }
      return promise;
    }

    Rooted<ImportAttributeVector> attributes(cx);
    Rooted<JSObject*> moduleRequest(
        cx, ModuleRequestObject::create(cx, specifierAtom, attributes));
    if (!moduleRequest) {
      if (!RejectPromiseWithPendingError(cx, promise)) {
        return nullptr;
      }
      return promise;
    }

    // Step 6. Perform ! HostImportModuleDynamically(null, specifierString,
    // innerCapability).
    //
    // By specification, this is supposed to take ReferencingScriptOrModule as
    // null, see first parameter above. However, if we do that, we don't end up
    // with a script reference, which is used to figure out what the base-URI
    // should be So then we end up using the default one for the module loader;
    // which because of the way we set the parent module loader up, means we end
    // up having the incorrect base URI, as the module loader ends up just using
    // the document's base URI.
    //
    // I have filed https://github.com/tc39/proposal-shadowrealm/issues/363 to
    // discuss this.
    Rooted<Value> referencingPrivate(cx, script->sourceObject()->getPrivate());
    if (!importHook(cx, referencingPrivate, moduleRequest, promise)) {
      // If there's no exception pending then the script is terminating
      // anyway, so just return nullptr.
      if (!cx->isExceptionPending() ||
          !RejectPromiseWithPendingError(cx, promise)) {
        return nullptr;
      }
      return promise;
    }

    // Step 7. Suspend evalContext and remove it from the execution context
    //         stack. (Implicit)
    // Step 8. Resume the context that is now on the top of the execution
    //         context stack as the running execution context (Implicit)
  }

  // Step 9.  Let steps be the steps of an ExportGetter function as described
  //          below.
  // Step 10. Let onFulfilled be ! CreateBuiltinFunction(steps, 1, "", «
  //          [[ExportNameString]] », callerRealm).

  // The handler can only hold onto a single object, so we pack that into a new
  // array, and store there.
  Rooted<ArrayObject*> handlerObject(
      cx,
      NewDenseFullyAllocatedArray(cx, uint32_t(ImportValueIndices::Length)));
  if (!handlerObject) {
    return nullptr;
  }

  handlerObject->setDenseInitializedLength(
      uint32_t(ImportValueIndices::Length));
  handlerObject->initDenseElement(uint32_t(ImportValueIndices::CalleRealm),
                                  PrivateValue(callerRealm));
  handlerObject->initDenseElement(
      uint32_t(ImportValueIndices::ExportNameString), StringValue(exportName));

  Rooted<JSFunction*> onFulfilled(
      cx,
      NewHandlerWithExtra(
          cx,
          [](JSContext* cx, unsigned argc, Value* vp) {
            // This is the export getter function from
            // https://tc39.es/proposal-shadowrealm/#sec-shadowrealmimportvalue
            CallArgs args = CallArgsFromVp(argc, vp);
            MOZ_ASSERT(args.length() == 1);

            auto* handlerObject = ExtraFromHandler<ArrayObject>(args);

            Rooted<Value> realmValue(
                cx, handlerObject->getDenseElement(
                        uint32_t(ImportValueIndices::CalleRealm)));
            Rooted<Value> exportNameValue(
                cx, handlerObject->getDenseElement(
                        uint32_t(ImportValueIndices::ExportNameString)));

            // Step 1. Assert: exports is a module namespace exotic object.
            Handle<Value> exportsValue = args[0];
            MOZ_ASSERT(exportsValue.isObject() &&
                       exportsValue.toObject().is<ModuleNamespaceObject>());

            Rooted<ModuleNamespaceObject*> exports(
                cx, &exportsValue.toObject().as<ModuleNamespaceObject>());

            // Step 2. Let f be the active function object. (not implemented
            // this way)
            //
            // Step 3. Let string be f.[[ExportNameString]]. Step 4.
            // Assert: Type(string) is String.
            MOZ_ASSERT(exportNameValue.isString());

            Rooted<JSAtom*> stringAtom(
                cx, AtomizeString(cx, exportNameValue.toString()));
            if (!stringAtom) {
              return false;
            }
            Rooted<jsid> stringId(cx, AtomToId(stringAtom));

            // Step 5. Let hasOwn be ? HasOwnProperty(exports, string).
            bool hasOwn = false;
            if (!HasOwnProperty(cx, exports, stringId, &hasOwn)) {
              return false;
            }

            // Step 6. If hasOwn is false, throw a TypeError exception.
            if (!hasOwn) {
              JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                        JSMSG_SHADOW_REALM_VALUE_NOT_EXPORTED);
              return false;
            }

            // Step 7. Let value be ? Get(exports, string).
            Rooted<Value> value(cx);
            if (!GetProperty(cx, exports, exports, stringId, &value)) {
              return false;
            }

            // Step 8. Let realm be f.[[Realm]].
            Realm* callerRealm = static_cast<Realm*>(realmValue.toPrivate());

            // Step 9. Return ? GetWrappedValue(realm, value).
            return GetWrappedValue(cx, callerRealm, value, args.rval());
          },
          promise, handlerObject));
  if (!onFulfilled) {
    return nullptr;
  }

  Rooted<JSFunction*> onRejected(
      cx, NewHandler(
              cx,
              [](JSContext* cx, unsigned argc, Value* vp) {
                JS_ReportErrorNumberASCII(
                    cx, GetErrorMessage, nullptr,
                    JSMSG_SHADOW_REALM_IMPORTVALUE_FAILED);
                return false;
              },
              promise));
  if (!onRejected) {
    return nullptr;
  }

  // Step 11. Set onFulfilled.[[ExportNameString]] to exportNameString.
  // Step 12. Let promiseCapability be ! NewPromiseCapability(%Promise%).
  // Step 13. Return ! PerformPromiseThen(innerCapability.[[Promise]],
  //           onFulfilled, callerRealm.[[Intrinsics]].[[%ThrowTypeError%]],
  //           promiseCapability).
  return OriginalPromiseThen(cx, promise, onFulfilled, onRejected);
}

//  ShadowRealm.prototype.importValue ( specifier, exportName )
// https://tc39.es/proposal-shadowrealm/#sec-shadowrealm.prototype.importvalue
static bool ShadowRealm_importValue(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. Let O be this value.
  HandleValue obj = args.thisv();

  // Step 2. Perform ? ValidateShadowRealmObject(O).
  Rooted<ShadowRealmObject*> shadowRealm(cx,
                                         ValidateShadowRealmObject(cx, obj));
  if (!shadowRealm) {
    return false;
  }

  // Step 3. Let specifierString be ? ToString(specifier).
  Rooted<JSString*> specifierString(cx, ToString<CanGC>(cx, args.get(0)));
  if (!specifierString) {
    return false;
  }

  // Step 4. If Type(exportName) is not String, throw a TypeError exception.
  if (!args.get(1).isString()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SHADOW_REALM_EXPORT_NOT_STRING);
    return false;
  }

  Rooted<JSString*> exportName(cx, args.get(1).toString());
  if (!exportName) {
    return false;
  }

  // Step 5. Let callerRealm be the current Realm Record.
  Realm* callerRealm = cx->realm();

  // Step 6. Let evalRealm be O.[[ShadowRealm]].
  Realm* evalRealm = shadowRealm->getShadowRealm();

  // Step 7. Let evalContext be O.[[ExecutionContext]]
  // (we dont' pass this explicitly, instead using the realm+global to
  // represent)

  // Step 8. Return ?
  // ShadowRealmImportValue(specifierString, exportName,
  //                                         callerRealm, evalRealm,
  //                                         evalContext).

  JSObject* res = ShadowRealmImportValue(cx, specifierString, exportName,
                                         callerRealm, evalRealm);
  if (!res) {
    return false;
  }

  args.rval().set(ObjectValue(*res));
  return true;
}

static const JSFunctionSpec shadowrealm_methods[] = {
    JS_FN("evaluate", ShadowRealm_evaluate, 1, 0),
    JS_FN("importValue", ShadowRealm_importValue, 2, 0),
    JS_FS_END,
};

static const JSPropertySpec shadowrealm_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "ShadowRealm", JSPROP_READONLY),
    JS_PS_END,
};

static const ClassSpec ShadowRealmObjectClassSpec = {
    GenericCreateConstructor<ShadowRealmObject::construct, 0,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<ShadowRealmObject>,
    nullptr,                 // Static methods
    nullptr,                 // Static properties
    shadowrealm_methods,     // Methods
    shadowrealm_properties,  // Properties
};

const JSClass ShadowRealmObject::class_ = {
    "ShadowRealm",
    JSCLASS_HAS_CACHED_PROTO(JSProto_ShadowRealm) |
        JSCLASS_HAS_RESERVED_SLOTS(ShadowRealmObject::SlotCount),
    JS_NULL_CLASS_OPS,
    &ShadowRealmObjectClassSpec,
};

const JSClass ShadowRealmObject::protoClass_ = {
    "ShadowRealm.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_ShadowRealm),
    JS_NULL_CLASS_OPS,
    &ShadowRealmObjectClassSpec,
};
