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

#include "js/AllocPolicy.h"     // js::SystemAllocPolicy
#include "js/ColumnNumber.h"    // JS::ColumnNumberOneOrigin
#include "js/CompileOptions.h"  // JS::ReadOnlyCompileOptions
#include "js/RootingAPI.h"      // JS::{Mutable,}Handle
#include "js/Value.h"           // JS::Value
#include "js/Vector.h"          // js::Vector

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

// This enum is used to index into an array, and we assume that we have
// sequential numbers starting at zero for the unknown type.
enum class ModuleType : uint32_t {
  Unknown = 0,
  JavaScript,
  JSON,

  Limit = JSON,
};

/**
 * The HostResolveImportedModule hook.
 *
 * See: https://tc39.es/ecma262/#sec-hostresolveimportedmodule
 *
 * This embedding-defined hook is used to implement module loading. It is called
 * to get or create a module object corresponding to |moduleRequest| occurring
 * in the context of the script or module with private value
 * |referencingPrivate|.
 *
 * The module specifier string for the request can be obtained by calling
 * JS::GetModuleRequestSpecifier.
 *
 * The private value for a script or module is set with JS::SetScriptPrivate or
 * JS::SetModulePrivate. It's assumed that the embedding can handle receiving
 * either here.
 *
 * This hook must obey the restrictions defined in the spec:
 *  - Each time the hook is called with the same arguemnts, the same module must
 *    be returned.
 *  - If a module cannot be created for the given arguments, an exception must
 *    be thrown.
 *
 * This is a synchronous operation.
 */
using ModuleResolveHook = JSObject* (*)(JSContext* cx,
                                        Handle<Value> referencingPrivate,
                                        Handle<JSObject*> moduleRequest);

/**
 * Get the HostResolveImportedModule hook for the runtime.
 */
extern JS_PUBLIC_API ModuleResolveHook GetModuleResolveHook(JSRuntime* rt);

/**
 * Set the HostResolveImportedModule hook for the runtime to the given function.
 */
extern JS_PUBLIC_API void SetModuleResolveHook(JSRuntime* rt,
                                               ModuleResolveHook func);

/**
 * The module metadata hook.
 *
 * See: https://tc39.es/ecma262/#sec-hostgetimportmetaproperties
 *
 * Populate the |metaObject| object returned when import.meta is evaluated in
 * the context of the script or module with private value |privateValue|.
 *
 * This is based on the spec's HostGetImportMetaProperties hook but defines
 * properties on the meta object directly rather than returning a list.
 */
using ModuleMetadataHook = bool (*)(JSContext* cx, Handle<Value> privateValue,
                                    Handle<JSObject*> metaObject);

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

/**
 * The HostImportModuleDynamically hook.
 *
 * See https://tc39.es/ecma262/#sec-hostimportmoduledynamically
 *
 * Used to implement dynamic module import. Called when evaluating import()
 * expressions.
 *
 * This starts an asynchronous operation. Some time after this hook is called
 * the embedding must call JS::FinishDynamicModuleImport() passing the
 * |referencingPrivate|, |moduleRequest| and |promise| arguments from the
 * call. This must happen for both success and failure cases.
 *
 * In the meantime the embedding can take whatever steps it needs to make the
 * module available. If successful, after calling FinishDynamicModuleImport()
 * the module should be returned by the resolve hook when passed
 * |referencingPrivate| and |moduleRequest|.
 */
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
 * Parse the given source buffer as a JSON module in the scope of the current
 * global of cx and return a synthetic module record.
 */
extern JS_PUBLIC_API JSObject* CompileJsonModule(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf);

/**
 * Parse the given source buffer as a JSON module in the scope of the current
 * global of cx and return a synthetic module record. An error is reported if a
 * UTF-8 encoding error is encountered.
 */
extern JS_PUBLIC_API JSObject* CompileJsonModule(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<mozilla::Utf8Unit>& srcBuf);

/**
 * Set a private value associated with a source text module record.
 */
extern JS_PUBLIC_API void SetModulePrivate(JSObject* module,
                                           const Value& value);
/**
 * Clear the private value associated with a source text module record.
 *
 * This is used during unlinking and can be called on a gray module, skipping
 * the usual checks.
 */
extern JS_PUBLIC_API void ClearModulePrivate(JSObject* module);

/**
 * Get the private value associated with a source text module record.
 */
extern JS_PUBLIC_API Value GetModulePrivate(JSObject* module);

/**
 * Checks if the given module is a cyclic module.
 */
extern JS_PUBLIC_API bool IsCyclicModule(JSObject* module);

/*
 * Perform the ModuleLink operation on the given source text module record.
 *
 * This transitively resolves all module dependencies (calling the
 * HostResolveImportedModule hook) and initializes the environment record for
 * the module.
 */
extern JS_PUBLIC_API bool ModuleLink(JSContext* cx,
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
 * ModuleLink must have completed prior to calling this.
 */
extern JS_PUBLIC_API bool ModuleEvaluate(JSContext* cx,
                                         Handle<JSObject*> moduleRecord,
                                         MutableHandleValue rval);

enum ModuleErrorBehaviour {
  // Report module evaluation errors asynchronously when the evaluation promise
  // is rejected. This is used for web content.
  ReportModuleErrorsAsync,

  // Throw module evaluation errors synchronously by setting an exception on the
  // context. Does not support modules that use top-level await.
  ThrowModuleErrorsSync
};

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
    JSContext* cx, Handle<JSObject*> evaluationPromise,
    ModuleErrorBehaviour errorBehaviour = ReportModuleErrorsAsync);

/*
 * Functions to access the module specifiers of a source text module record used
 * to request module imports.
 *
 * Clients can use GetRequestedModulesCount() to get the number of specifiers
 * and GetRequestedModuleSpecifier() / GetRequestedModuleSourcePos() to get the
 * individual elements.
 */
extern JS_PUBLIC_API uint32_t
GetRequestedModulesCount(JSContext* cx, Handle<JSObject*> moduleRecord);

extern JS_PUBLIC_API JSString* GetRequestedModuleSpecifier(
    JSContext* cx, Handle<JSObject*> moduleRecord, uint32_t index);

/*
 * Get the position of a requested module's name in the source.
 */
extern JS_PUBLIC_API void GetRequestedModuleSourcePos(
    JSContext* cx, Handle<JSObject*> moduleRecord, uint32_t index,
    uint32_t* lineNumber, JS::ColumnNumberOneOrigin* columnNumber);

/*
 * Get the module type of a requested module.
 */
extern JS_PUBLIC_API ModuleType GetRequestedModuleType(
    JSContext* cx, Handle<JSObject*> moduleRecord, uint32_t index);

/*
 * Get the top-level script for a module which has not yet been executed.
 */
extern JS_PUBLIC_API JSScript* GetModuleScript(Handle<JSObject*> moduleRecord);

extern JS_PUBLIC_API JSObject* CreateModuleRequest(
    JSContext* cx, Handle<JSString*> specifierArg, ModuleType moduleType);
extern JS_PUBLIC_API JSString* GetModuleRequestSpecifier(
    JSContext* cx, Handle<JSObject*> moduleRequestArg);

/*
 * Get the module type of the specified module request.
 */
extern JS_PUBLIC_API ModuleType
GetModuleRequestType(JSContext* cx, Handle<JSObject*> moduleRequestArg);

/*
 * Get the module record for a module script.
 */
extern JS_PUBLIC_API JSObject* GetModuleObject(Handle<JSScript*> moduleScript);

/*
 * Get the namespace object for a module.
 */
extern JS_PUBLIC_API JSObject* GetModuleNamespace(
    JSContext* cx, Handle<JSObject*> moduleRecord);

extern JS_PUBLIC_API JSObject* GetModuleForNamespace(
    JSContext* cx, Handle<JSObject*> moduleNamespace);

extern JS_PUBLIC_API JSObject* GetModuleEnvironment(
    JSContext* cx, Handle<JSObject*> moduleObj);

/*
 * Clear all bindings in a module's environment. Used during shutdown.
 */
extern JS_PUBLIC_API void ClearModuleEnvironment(JSObject* moduleObj);

extern JS_PUBLIC_API bool ModuleIsLinked(JSObject* moduleObj);

}  // namespace JS

#endif  // js_Modules_h
