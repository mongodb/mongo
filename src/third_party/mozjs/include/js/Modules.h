/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript module (as in, the syntactic construct) operations. */

#ifndef js_Modules_h
#define js_Modules_h

#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/CompileOptions.h"  // JS::ReadOnlyCompileOptions
#include "js/RootingAPI.h"      // JS::{Mutable,}Handle
#include "js/Value.h"           // JS::Value

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;
struct JS_PUBLIC_API JSRuntime;
class JS_PUBLIC_API JSString;

namespace JS {
template <typename UnitT>
class SourceText;
}  // namespace JS

namespace mozilla {
union Utf8Unit;
}

namespace JS {

using ModuleResolveHook = JSObject* (*)(JSContext*, Handle<Value>,
                                        Handle<JSObject*>);

/**
 * Get the HostResolveImportedModule hook for the runtime.
 */
extern JS_PUBLIC_API ModuleResolveHook GetModuleResolveHook(JSRuntime* rt);

/**
 * Set the HostResolveImportedModule hook for the runtime to the given function.
 */
extern JS_PUBLIC_API void SetModuleResolveHook(JSRuntime* rt,
                                               ModuleResolveHook func);

using ModuleMetadataHook = bool (*)(JSContext*, Handle<Value>,
                                    Handle<JSObject*>);

/**
 * Get the hook for populating the import.meta metadata object.
 */
extern JS_PUBLIC_API ModuleMetadataHook GetModuleMetadataHook(JSRuntime* rt);

/**
 * Set the hook for populating the import.meta metadata object to the given
 * function.
 */
extern JS_PUBLIC_API void SetModuleMetadataHook(JSRuntime* rt,
                                                ModuleMetadataHook func);

using ModuleDynamicImportHook = bool (*)(JSContext* cx,
                                         Handle<Value> referencingPrivate,
                                         Handle<JSObject*> moduleRequest,
                                         Handle<JSObject*> promise);

/**
 * Get the HostImportModuleDynamically hook for the runtime.
 */
extern JS_PUBLIC_API ModuleDynamicImportHook
GetModuleDynamicImportHook(JSRuntime* rt);

/**
 * Set the HostImportModuleDynamically hook for the runtime to the given
 * function.
 *
 * If this hook is not set (or set to nullptr) then the JS engine will throw an
 * exception if dynamic module import is attempted.
 */
extern JS_PUBLIC_API void SetModuleDynamicImportHook(
    JSRuntime* rt, ModuleDynamicImportHook func);

/**
 * Passed to FinishDynamicModuleImport to indicate the result of the dynamic
 * import operation.
 */
enum class DynamicImportStatus { Failed = 0, Ok };

/**
 * This must be called after a dynamic import operation is complete.
 *
 * If |evaluationPromise| is rejected, the rejection reason will be used to
 * complete the user's promise.
 */
extern JS_PUBLIC_API bool FinishDynamicModuleImport(
    JSContext* cx, Handle<JSObject*> evaluationPromise,
    Handle<Value> referencingPrivate, Handle<JSObject*> moduleRequest,
    Handle<JSObject*> promise);

/**
 * This must be called after a dynamic import operation is complete.
 *
 * This is used so that Top Level Await functionality can be turned off
 * entirely. It will be removed in bug#1676612.
 *
 * If |status| is Failed, any pending exception on the context will be used to
 * complete the user's promise.
 */
extern JS_PUBLIC_API bool FinishDynamicModuleImport_NoTLA(
    JSContext* cx, DynamicImportStatus status, Handle<Value> referencingPrivate,
    Handle<JSObject*> moduleRequest, Handle<JSObject*> promise);

/**
 * Parse the given source buffer as a module in the scope of the current global
 * of cx and return a source text module record.
 */
extern JS_PUBLIC_API JSObject* CompileModule(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf);

/**
 * Parse the given source buffer as a module in the scope of the current global
 * of cx and return a source text module record.  An error is reported if a
 * UTF-8 encoding error is encountered.
 */
extern JS_PUBLIC_API JSObject* CompileModule(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<mozilla::Utf8Unit>& srcBuf);

/**
 * Set a private value associated with a source text module record.
 */
extern JS_PUBLIC_API void SetModulePrivate(JSObject* module,
                                           const Value& value);

/**
 * Get the private value associated with a source text module record.
 */
extern JS_PUBLIC_API Value GetModulePrivate(JSObject* module);

/*
 * Perform the ModuleInstantiate operation on the given source text module
 * record.
 *
 * This transitively resolves all module dependencies (calling the
 * HostResolveImportedModule hook) and initializes the environment record for
 * the module.
 */
extern JS_PUBLIC_API bool ModuleInstantiate(JSContext* cx,
                                            Handle<JSObject*> moduleRecord);

/*
 * Perform the ModuleEvaluate operation on the given source text module record
 * and returns a bool. A result value is returned in result and is either
 * undefined (and ignored) or a promise (if Top Level Await is enabled).
 *
 * If this module has already been evaluated, it returns the evaluation
 * promise. Otherwise, it transitively evaluates all dependences of this module
 * and then evaluates this module.
 *
 * ModuleInstantiate must have completed prior to calling this.
 */
extern JS_PUBLIC_API bool ModuleEvaluate(JSContext* cx,
                                         Handle<JSObject*> moduleRecord,
                                         MutableHandleValue rval);

/*
 * If a module evaluation fails, unwrap the resulting evaluation promise
 * and rethrow.
 *
 * This does nothing if this module succeeds in evaluation. Otherwise, it
 * takes the reason for the module throwing, unwraps it and throws it as a
 * regular error rather than as an uncaught promise.
 *
 * ModuleEvaluate must have completed prior to calling this.
 */
extern JS_PUBLIC_API bool ThrowOnModuleEvaluationFailure(
    JSContext* cx, Handle<JSObject*> evaluationPromise);

/*
 * Get a list of the module specifiers used by a source text module
 * record to request importation of modules.
 *
 * The result is a JavaScript array of object values.  To extract the individual
 * values use only JS::GetArrayLength and JS_GetElement with indices 0 to length
 * - 1.
 *
 * The element values are objects with the following properties:
 *  - moduleSpecifier: the module specifier string
 *  - lineNumber: the line number of the import in the source text
 *  - columnNumber: the column number of the import in the source text
 *
 * These property values can be extracted with GetRequestedModuleSpecifier() and
 * GetRequestedModuleSourcePos()
 */
extern JS_PUBLIC_API JSObject* GetRequestedModules(
    JSContext* cx, Handle<JSObject*> moduleRecord);

extern JS_PUBLIC_API JSString* GetRequestedModuleSpecifier(
    JSContext* cx, Handle<Value> requestedModuleObject);

extern JS_PUBLIC_API void GetRequestedModuleSourcePos(
    JSContext* cx, Handle<Value> requestedModuleObject, uint32_t* lineNumber,
    uint32_t* columnNumber);

/*
 * Get the top-level script for a module which has not yet been executed.
 */
extern JS_PUBLIC_API JSScript* GetModuleScript(Handle<JSObject*> moduleRecord);

extern JS_PUBLIC_API JSObject* CreateModuleRequest(
    JSContext* cx, Handle<JSString*> specifierArg);
extern JS_PUBLIC_API JSString* GetModuleRequestSpecifier(
    JSContext* cx, Handle<JSObject*> moduleRequestArg);

}  // namespace JS

#endif  // js_Modules_h
