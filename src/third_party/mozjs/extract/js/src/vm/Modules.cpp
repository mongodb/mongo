/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript modules (as in, the syntactic construct) implementation. */

#include "js/Modules.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Utf8.h"        // mozilla::Utf8Unit

#include <stdint.h>  // uint32_t

#include "jsapi.h"    // js::AssertHeapIsIdle
#include "jstypes.h"  // JS_PUBLIC_API

#include "builtin/ModuleObject.h"  // js::FinishDynamicModuleImport, js::{,Requested}ModuleObject
#include "frontend/BytecodeCompiler.h"  // js::frontend::CompileModule
#include "js/RootingAPI.h"              // JS::MutableHandle
#include "js/Value.h"                   // JS::Value
#include "vm/JSContext.h"               // CHECK_THREAD, JSContext
#include "vm/JSObject.h"                // JSObject
#include "vm/Runtime.h"                 // JSRuntime

#include "vm/JSContext-inl.h"  // JSContext::{c,releaseC}heck

using mozilla::Utf8Unit;

using js::AssertHeapIsIdle;
using js::ModuleObject;
using js::RequestedModuleObject;

JS_PUBLIC_API JS::ModuleResolveHook JS::GetModuleResolveHook(JSRuntime* rt) {
  AssertHeapIsIdle();

  return rt->moduleResolveHook;
}

JS_PUBLIC_API void JS::SetModuleResolveHook(JSRuntime* rt,
                                            ModuleResolveHook func) {
  AssertHeapIsIdle();

  rt->moduleResolveHook = func;
}

JS_PUBLIC_API JS::ModuleMetadataHook JS::GetModuleMetadataHook(JSRuntime* rt) {
  AssertHeapIsIdle();

  return rt->moduleMetadataHook;
}

JS_PUBLIC_API void JS::SetModuleMetadataHook(JSRuntime* rt,
                                             ModuleMetadataHook func) {
  AssertHeapIsIdle();

  rt->moduleMetadataHook = func;
}

JS_PUBLIC_API JS::ModuleDynamicImportHook JS::GetModuleDynamicImportHook(
    JSRuntime* rt) {
  AssertHeapIsIdle();

  return rt->moduleDynamicImportHook;
}

JS_PUBLIC_API void JS::SetModuleDynamicImportHook(
    JSRuntime* rt, ModuleDynamicImportHook func) {
  AssertHeapIsIdle();

  rt->moduleDynamicImportHook = func;
}

JS_PUBLIC_API bool JS::FinishDynamicModuleImport(
    JSContext* cx, Handle<JSObject*> evaluationPromise,
    Handle<Value> referencingPrivate, Handle<JSObject*> moduleRequest,
    Handle<JSObject*> promise) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(referencingPrivate, promise);

  return js::FinishDynamicModuleImport(
      cx, evaluationPromise, referencingPrivate, moduleRequest, promise);
}

JS_PUBLIC_API bool JS::FinishDynamicModuleImport_NoTLA(
    JSContext* cx, JS::DynamicImportStatus status,
    Handle<Value> referencingPrivate, Handle<JSObject*> moduleRequest,
    Handle<JSObject*> promise) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(referencingPrivate, promise);

  return js::FinishDynamicModuleImport_NoTLA(cx, status, referencingPrivate,
                                             moduleRequest, promise);
}

template <typename Unit>
static JSObject* CompileModuleHelper(JSContext* cx,
                                     const JS::ReadOnlyCompileOptions& options,
                                     JS::SourceText<Unit>& srcBuf) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return js::frontend::CompileModule(cx, options, srcBuf);
}

JS_PUBLIC_API JSObject* JS::CompileModule(JSContext* cx,
                                          const ReadOnlyCompileOptions& options,
                                          SourceText<char16_t>& srcBuf) {
  return CompileModuleHelper(cx, options, srcBuf);
}

JS_PUBLIC_API JSObject* JS::CompileModule(JSContext* cx,
                                          const ReadOnlyCompileOptions& options,
                                          SourceText<Utf8Unit>& srcBuf) {
  return CompileModuleHelper(cx, options, srcBuf);
}

JS_PUBLIC_API void JS::SetModulePrivate(JSObject* module, const Value& value) {
  JSRuntime* rt = module->zone()->runtimeFromMainThread();
  module->as<ModuleObject>().scriptSourceObject()->setPrivate(rt, value);
}

JS_PUBLIC_API JS::Value JS::GetModulePrivate(JSObject* module) {
  return module->as<ModuleObject>().scriptSourceObject()->canonicalPrivate();
}

JS_PUBLIC_API bool JS::ModuleInstantiate(JSContext* cx,
                                         Handle<JSObject*> moduleArg) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->releaseCheck(moduleArg);

  return ModuleObject::Instantiate(cx, moduleArg.as<ModuleObject>());
}

JS_PUBLIC_API bool JS::ModuleEvaluate(JSContext* cx,
                                      Handle<JSObject*> moduleRecord,
                                      MutableHandle<JS::Value> rval) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->releaseCheck(moduleRecord);

  return ModuleObject::Evaluate(cx, moduleRecord.as<ModuleObject>(), rval);
}

JS_PUBLIC_API bool JS::ThrowOnModuleEvaluationFailure(
    JSContext* cx, Handle<JSObject*> evaluationPromise) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->releaseCheck(evaluationPromise);

  return js::OnModuleEvaluationFailure(cx, evaluationPromise);
}

JS_PUBLIC_API JSObject* JS::GetRequestedModules(JSContext* cx,
                                                Handle<JSObject*> moduleArg) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(moduleArg);

  return &moduleArg->as<ModuleObject>().requestedModules();
}

JS_PUBLIC_API JSString* JS::GetRequestedModuleSpecifier(JSContext* cx,
                                                        Handle<Value> value) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);

  JSObject* obj = &value.toObject();
  return obj->as<RequestedModuleObject>().moduleRequest()->specifier();
}

JS_PUBLIC_API void JS::GetRequestedModuleSourcePos(JSContext* cx,
                                                   JS::HandleValue value,
                                                   uint32_t* lineNumber,
                                                   uint32_t* columnNumber) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);
  MOZ_ASSERT(lineNumber);
  MOZ_ASSERT(columnNumber);

  auto& requested = value.toObject().as<RequestedModuleObject>();
  *lineNumber = requested.lineNumber();
  *columnNumber = requested.columnNumber();
}

JS_PUBLIC_API JSScript* JS::GetModuleScript(JS::HandleObject moduleRecord) {
  AssertHeapIsIdle();

  return moduleRecord->as<ModuleObject>().script();
}

JS_PUBLIC_API JSObject* JS::CreateModuleRequest(
    JSContext* cx, Handle<JSString*> specifierArg) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  js::RootedAtom specifierAtom(cx, AtomizeString(cx, specifierArg));
  if (!specifierAtom) {
    return nullptr;
  }

  return js::ModuleRequestObject::create(cx, specifierAtom);
}

JS_PUBLIC_API JSString* JS::GetModuleRequestSpecifier(
    JSContext* cx, Handle<JSObject*> moduleRequestArg) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(moduleRequestArg);

  return moduleRequestArg->as<js::ModuleRequestObject>().specifier();
}
