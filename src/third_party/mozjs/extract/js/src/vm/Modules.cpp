/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript modules (as in, the syntactic construct) implementation. */

#include "vm/Modules.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Utf8.h"        // mozilla::Utf8Unit

#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "builtin/ModuleObject.h"  // js::FinishDynamicModuleImport, js::{,Requested}ModuleObject
#include "ds/Sort.h"
#include "frontend/BytecodeCompiler.h"  // js::frontend::CompileModule
#include "frontend/FrontendContext.h"   // js::AutoReportFrontendContext
#include "js/Context.h"                 // js::AssertHeapIsIdle
#include "js/RootingAPI.h"              // JS::MutableHandle
#include "js/Value.h"                   // JS::Value
#include "vm/EnvironmentObject.h"       // js::ModuleEnvironmentObject
#include "vm/JSContext.h"               // CHECK_THREAD, JSContext
#include "vm/JSObject.h"                // JSObject
#include "vm/List.h"                    // ListObject
#include "vm/Runtime.h"                 // JSRuntime

#include "vm/JSAtom-inl.h"
#include "vm/JSContext-inl.h"  // JSContext::{c,releaseC}heck

using namespace js;

using mozilla::Utf8Unit;

////////////////////////////////////////////////////////////////////////////////
// Public API

JS_PUBLIC_API void JS::SetSupportedImportAssertions(
    JSRuntime* rt, const ImportAssertionVector& assertions) {
  AssertHeapIsIdle();
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
  MOZ_ASSERT(rt->supportedImportAssertions.ref().empty());

  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!rt->supportedImportAssertions.ref().appendAll(assertions)) {
    oomUnsafe.crash("SetSupportedImportAssertions");
  }
}

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

template <typename Unit>
static JSObject* CompileModuleHelper(JSContext* cx,
                                     const JS::ReadOnlyCompileOptions& options,
                                     JS::SourceText<Unit>& srcBuf) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  JS::Rooted<JSObject*> mod(cx);
  {
    AutoReportFrontendContext fc(cx);
    mod = frontend::CompileModule(cx, &fc, options, srcBuf);
  }
  return mod;
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

JS_PUBLIC_API void JS::ClearModulePrivate(JSObject* module) {
  // |module| may be gray, be careful not to create edges to it.
  JSRuntime* rt = module->zone()->runtimeFromMainThread();
  module->as<ModuleObject>().scriptSourceObject()->clearPrivate(rt);
}

JS_PUBLIC_API JS::Value JS::GetModulePrivate(JSObject* module) {
  return module->as<ModuleObject>().scriptSourceObject()->getPrivate();
}

JS_PUBLIC_API bool JS::ModuleLink(JSContext* cx, Handle<JSObject*> moduleArg) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->releaseCheck(moduleArg);

  return js::ModuleLink(cx, moduleArg.as<ModuleObject>());
}

JS_PUBLIC_API bool JS::ModuleEvaluate(JSContext* cx,
                                      Handle<JSObject*> moduleRecord,
                                      MutableHandle<JS::Value> rval) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->releaseCheck(moduleRecord);

  return js::ModuleEvaluate(cx, moduleRecord.as<ModuleObject>(), rval);
}

JS_PUBLIC_API bool JS::ThrowOnModuleEvaluationFailure(
    JSContext* cx, Handle<JSObject*> evaluationPromise,
    ModuleErrorBehaviour errorBehaviour) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->releaseCheck(evaluationPromise);

  return OnModuleEvaluationFailure(cx, evaluationPromise, errorBehaviour);
}

JS_PUBLIC_API uint32_t
JS::GetRequestedModulesCount(JSContext* cx, Handle<JSObject*> moduleRecord) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(moduleRecord);

  return moduleRecord->as<ModuleObject>().requestedModules().Length();
}

JS_PUBLIC_API JSString* JS::GetRequestedModuleSpecifier(
    JSContext* cx, Handle<JSObject*> moduleRecord, uint32_t index) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(moduleRecord);

  auto& module = moduleRecord->as<ModuleObject>();
  return module.requestedModules()[index].moduleRequest()->specifier();
}

JS_PUBLIC_API void JS::GetRequestedModuleSourcePos(
    JSContext* cx, Handle<JSObject*> moduleRecord, uint32_t index,
    uint32_t* lineNumber, uint32_t* columnNumber) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(moduleRecord);
  MOZ_ASSERT(lineNumber);
  MOZ_ASSERT(columnNumber);

  auto& module = moduleRecord->as<ModuleObject>();
  *lineNumber = module.requestedModules()[index].lineNumber();
  *columnNumber = module.requestedModules()[index].columnNumber();
}

JS_PUBLIC_API JSScript* JS::GetModuleScript(JS::HandleObject moduleRecord) {
  AssertHeapIsIdle();

  return moduleRecord->as<ModuleObject>().script();
}

JS_PUBLIC_API JSObject* JS::GetModuleObject(HandleScript moduleScript) {
  AssertHeapIsIdle();
  MOZ_ASSERT(moduleScript->isModule());

  return moduleScript->module();
}

JS_PUBLIC_API JSObject* JS::GetModuleNamespace(JSContext* cx,
                                               HandleObject moduleRecord) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(moduleRecord);
  MOZ_ASSERT(moduleRecord->is<ModuleObject>());

  return GetOrCreateModuleNamespace(cx, moduleRecord.as<ModuleObject>());
}

JS_PUBLIC_API JSObject* JS::GetModuleForNamespace(
    JSContext* cx, HandleObject moduleNamespace) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(moduleNamespace);
  MOZ_ASSERT(moduleNamespace->is<ModuleNamespaceObject>());

  return &moduleNamespace->as<ModuleNamespaceObject>().module();
}

JS_PUBLIC_API JSObject* JS::GetModuleEnvironment(JSContext* cx,
                                                 Handle<JSObject*> moduleObj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(moduleObj);
  MOZ_ASSERT(moduleObj->is<ModuleObject>());

  return moduleObj->as<ModuleObject>().environment();
}

JS_PUBLIC_API JSObject* JS::CreateModuleRequest(
    JSContext* cx, Handle<JSString*> specifierArg) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  Rooted<JSAtom*> specifierAtom(cx, AtomizeString(cx, specifierArg));
  if (!specifierAtom) {
    return nullptr;
  }

  return ModuleRequestObject::create(cx, specifierAtom, nullptr);
}

JS_PUBLIC_API JSString* JS::GetModuleRequestSpecifier(
    JSContext* cx, Handle<JSObject*> moduleRequestArg) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(moduleRequestArg);

  return moduleRequestArg->as<ModuleRequestObject>().specifier();
}

JS_PUBLIC_API void JS::ClearModuleEnvironment(JSObject* moduleObj) {
  MOZ_ASSERT(moduleObj);
  AssertHeapIsIdle();

  js::ModuleEnvironmentObject* env =
      moduleObj->as<js::ModuleObject>().environment();
  if (!env) {
    return;
  }

  const JSClass* clasp = env->getClass();
  uint32_t numReserved = JSCLASS_RESERVED_SLOTS(clasp);
  uint32_t numSlots = env->slotSpan();
  for (uint32_t i = numReserved; i < numSlots; i++) {
    env->setSlot(i, UndefinedValue());
  }
}

////////////////////////////////////////////////////////////////////////////////
// Internal implementation

class ResolveSetEntry {
  ModuleObject* module_;
  JSAtom* exportName_;

 public:
  ResolveSetEntry(ModuleObject* module, JSAtom* exportName)
      : module_(module), exportName_(exportName) {}

  ModuleObject* module() const { return module_; }
  JSAtom* exportName() const { return exportName_; }

  void trace(JSTracer* trc) {
    TraceRoot(trc, &module_, "ResolveSetEntry::module_");
    TraceRoot(trc, &exportName_, "ResolveSetEntry::exportName_");
  }
};

using ResolveSet = GCVector<ResolveSetEntry, 0, SystemAllocPolicy>;

using ModuleSet =
    GCHashSet<ModuleObject*, DefaultHasher<ModuleObject*>, SystemAllocPolicy>;

static ModuleObject* HostResolveImportedModule(
    JSContext* cx, Handle<ModuleObject*> module,
    Handle<ModuleRequestObject*> moduleRequest,
    ModuleStatus expectedMinimumStatus);
static bool ModuleResolveExport(JSContext* cx, Handle<ModuleObject*> module,
                                Handle<JSAtom*> exportName,
                                MutableHandle<ResolveSet> resolveSet,
                                MutableHandle<Value> result);
static ModuleNamespaceObject* ModuleNamespaceCreate(
    JSContext* cx, Handle<ModuleObject*> module,
    MutableHandle<UniquePtr<ExportNameVector>> exports);
static bool InnerModuleLinking(JSContext* cx, Handle<ModuleObject*> module,
                               MutableHandle<ModuleVector> stack, size_t index,
                               size_t* indexOut);
static bool InnerModuleEvaluation(JSContext* cx, Handle<ModuleObject*> module,
                                  MutableHandle<ModuleVector> stack,
                                  size_t index, size_t* indexOut);
static bool ExecuteAsyncModule(JSContext* cx, Handle<ModuleObject*> module);
static bool GatherAvailableModuleAncestors(
    JSContext* cx, Handle<ModuleObject*> module,
    MutableHandle<ModuleVector> execList);

static const char* ModuleStatusName(ModuleStatus status) {
  switch (status) {
    case ModuleStatus::Unlinked:
      return "Unlinked";
    case ModuleStatus::Linking:
      return "Linking";
    case ModuleStatus::Linked:
      return "Linked";
    case ModuleStatus::Evaluating:
      return "Evaluating";
    case ModuleStatus::EvaluatingAsync:
      return "EvaluatingAsync";
    case ModuleStatus::Evaluated:
      return "Evaluated";
    default:
      MOZ_CRASH("Unexpected ModuleStatus");
  }
}

static bool ContainsElement(Handle<ExportNameVector> list, JSAtom* atom) {
  for (JSAtom* a : list) {
    if (a == atom) {
      return true;
    }
  }

  return false;
}

static bool ContainsElement(Handle<ModuleVector> stack, ModuleObject* module) {
  for (ModuleObject* m : stack) {
    if (m == module) {
      return true;
    }
  }

  return false;
}

#ifdef DEBUG
static size_t CountElements(Handle<ModuleVector> stack, ModuleObject* module) {
  size_t count = 0;
  for (ModuleObject* m : stack) {
    if (m == module) {
      count++;
    }
  }

  return count;
}
#endif

// https://tc39.es/ecma262/#sec-getexportednames
// ES2023 16.2.1.6.2 GetExportedNames
static bool ModuleGetExportedNames(
    JSContext* cx, Handle<ModuleObject*> module,
    MutableHandle<ModuleSet> exportStarSet,
    MutableHandle<ExportNameVector> exportedNames) {
  // Step 4. Let exportedNames be a new empty List.
  MOZ_ASSERT(exportedNames.empty());

  // Step 2. If exportStarSet contains module, then:
  if (exportStarSet.has(module)) {
    // Step 2.a. We've reached the starting point of an export * circularity.
    // Step 2.b. Return a new empty List.
    return true;
  }

  // Step 3. Append module to exportStarSet.
  if (!exportStarSet.put(module)) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Step 5. For each ExportEntry Record e of module.[[LocalExportEntries]], do:
  for (const ExportEntry& e : module->localExportEntries()) {
    // Step 5.a. Assert: module provides the direct binding for this export.
    // Step 5.b. Append e.[[ExportName]] to exportedNames.
    if (!exportedNames.append(e.exportName())) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Step 6. For each ExportEntry Record e of module.[[IndirectExportEntries]],
  //         do:
  for (const ExportEntry& e : module->indirectExportEntries()) {
    // Step 6.a. Assert: module imports a specific binding for this export.
    // Step 6.b. Append e.[[ExportName]] to exportedNames.
    if (!exportedNames.append(e.exportName())) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Step 7. For each ExportEntry Record e of module.[[StarExportEntries]], do:
  Rooted<ModuleRequestObject*> moduleRequest(cx);
  Rooted<ModuleObject*> requestedModule(cx);
  Rooted<JSAtom*> name(cx);
  for (const ExportEntry& e : module->starExportEntries()) {
    // Step 7.a. Let requestedModule be ? HostResolveImportedModule(module,
    //           e.[[ModuleRequest]]).
    moduleRequest = e.moduleRequest();
    requestedModule = HostResolveImportedModule(cx, module, moduleRequest,
                                                ModuleStatus::Unlinked);
    if (!requestedModule) {
      return false;
    }

    // Step 7.b. Let starNames be ?
    //           requestedModule.GetExportedNames(exportStarSet).
    Rooted<ExportNameVector> starNames(cx);
    if (!ModuleGetExportedNames(cx, requestedModule, exportStarSet,
                                &starNames)) {
      return false;
    }

    // Step 7.c. For each element n of starNames, do:
    for (JSAtom* name : starNames) {
      // Step 7.c.i. If SameValue(n, "default") is false, then:
      if (name != cx->names().default_) {
        // Step 7.c.i.1. If n is not an element of exportedNames, then:
        if (!ContainsElement(exportedNames, name)) {
          // Step 7.c.i.1.a. Append n to exportedNames.
          if (!exportedNames.append(name)) {
            ReportOutOfMemory(cx);
            return false;
          }
        }
      }
    }
  }

  // Step 8. Return exportedNames.
  return true;
}

static void ThrowUnexpectedModuleStatus(JSContext* cx, ModuleStatus status) {
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_BAD_MODULE_STATUS, ModuleStatusName(status));
}

static ModuleObject* HostResolveImportedModule(
    JSContext* cx, Handle<ModuleObject*> module,
    Handle<ModuleRequestObject*> moduleRequest,
    ModuleStatus expectedMinimumStatus) {
  MOZ_ASSERT(module);
  MOZ_ASSERT(moduleRequest);

  Rooted<Value> referencingPrivate(cx, JS::GetModulePrivate(module));
  Rooted<ModuleObject*> requestedModule(cx);
  requestedModule =
      CallModuleResolveHook(cx, referencingPrivate, moduleRequest);
  if (!requestedModule) {
    return nullptr;
  }

  if (requestedModule->status() < expectedMinimumStatus) {
    ThrowUnexpectedModuleStatus(cx, requestedModule->status());
    return nullptr;
  }

  return requestedModule;
}

// https://tc39.es/ecma262/#sec-resolveexport
// ES2023 16.2.1.6.3 ResolveExport
//
// Returns an value describing the location of the resolved export or indicating
// a failure.
//
// On success this returns a resolved binding record: { module, bindingName }
//
// There are two failure cases:
//
//  - If no definition was found or the request is found to be circular, *null*
//    is returned.
//
//  - If the request is found to be ambiguous, the string `"ambiguous"` is
//    returned.
//
bool js::ModuleResolveExport(JSContext* cx, Handle<ModuleObject*> module,
                             Handle<JSAtom*> exportName,
                             MutableHandle<Value> result) {
  // Step 1. If resolveSet is not present, set resolveSet to a new empty List.
  Rooted<ResolveSet> resolveSet(cx);

  return ::ModuleResolveExport(cx, module, exportName, &resolveSet, result);
}

static bool CreateResolvedBindingObject(JSContext* cx,
                                        Handle<ModuleObject*> module,
                                        Handle<JSAtom*> bindingName,
                                        MutableHandle<Value> result) {
  Rooted<ResolvedBindingObject*> obj(
      cx, ResolvedBindingObject::create(cx, module, bindingName));
  if (!obj) {
    return false;
  }

  result.setObject(*obj);
  return true;
}

static bool ModuleResolveExport(JSContext* cx, Handle<ModuleObject*> module,
                                Handle<JSAtom*> exportName,
                                MutableHandle<ResolveSet> resolveSet,
                                MutableHandle<Value> result) {
  // Step 2. For each Record { [[Module]], [[ExportName]] } r of resolveSet, do:
  for (const auto& entry : resolveSet) {
    // Step 2.a. If module and r.[[Module]] are the same Module Record and
    //           SameValue(exportName, r.[[ExportName]]) is true, then:
    if (entry.module() == module && entry.exportName() == exportName) {
      // Step 2.a.i. Assert: This is a circular import request.
      // Step 2.a.ii. Return null.
      result.setNull();
      return true;
    }
  }

  // Step 3. Append the Record { [[Module]]: module, [[ExportName]]: exportName
  // } to resolveSet.
  if (!resolveSet.emplaceBack(module, exportName)) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Step 4. For each ExportEntry Record e of module.[[LocalExportEntries]], do:
  for (const ExportEntry& e : module->localExportEntries()) {
    // Step 4.a. If SameValue(exportName, e.[[ExportName]]) is true, then:
    if (exportName == e.exportName()) {
      // Step 4.a.i. Assert: module provides the direct binding for this export.
      // Step 4.a.ii. Return ResolvedBinding Record { [[Module]]: module,
      //              [[BindingName]]: e.[[LocalName]] }.
      Rooted<JSAtom*> localName(cx, e.localName());
      return CreateResolvedBindingObject(cx, module, localName, result);
    }
  }

  // Step 5. For each ExportEntry Record e of module.[[IndirectExportEntries]],
  //         do:
  Rooted<ModuleRequestObject*> moduleRequest(cx);
  Rooted<ModuleObject*> importedModule(cx);
  Rooted<JSAtom*> name(cx);
  for (const ExportEntry& e : module->indirectExportEntries()) {
    // Step 5.a. If SameValue(exportName, e.[[ExportName]]) is true, then:
    if (exportName == e.exportName()) {
      // Step 5.a.i. Let importedModule be ? HostResolveImportedModule(module,
      //             e.[[ModuleRequest]]).
      moduleRequest = e.moduleRequest();
      importedModule = HostResolveImportedModule(cx, module, moduleRequest,
                                                 ModuleStatus::Unlinked);
      if (!importedModule) {
        return false;
      }

      // Step 5.a.ii. If e.[[ImportName]] is all, then:
      if (!e.importName()) {
        // Step 5.a.ii.1. Assert: module does not provide the direct binding for
        //                this export.
        // Step 5.a.ii.2. Return ResolvedBinding Record { [[Module]]:
        //                importedModule, [[BindingName]]: namespace }.
        name = cx->names().starNamespaceStar;
        return CreateResolvedBindingObject(cx, importedModule, name, result);
      } else {
        // Step 5.a.iii.1. Assert: module imports a specific binding for this
        //                 export.
        // Step 5.a.iii.2. Return ?
        // importedModule.ResolveExport(e.[[ImportName]],
        //                 resolveSet).
        name = e.importName();
        return ModuleResolveExport(cx, importedModule, name, resolveSet,
                                   result);
      }
    }
  }

  // Step 6. If SameValue(exportName, "default") is true, then:
  if (exportName == cx->names().default_) {
    // Step 6.a. Assert: A default export was not explicitly defined by this
    //           module.
    // Step 6.b. Return null.
    // Step 6.c. NOTE: A default export cannot be provided by an export * from
    //           "mod" declaration.
    result.setNull();
    return true;
  }

  // Step 7. Let starResolution be null.
  Rooted<ResolvedBindingObject*> starResolution(cx);

  // Step 8. For each ExportEntry Record e of module.[[StarExportEntries]], do:
  Rooted<Value> resolution(cx);
  Rooted<ResolvedBindingObject*> binding(cx);
  for (const ExportEntry& e : module->starExportEntries()) {
    // Step 8.a. Let importedModule be ? HostResolveImportedModule(module,
    //           e.[[ModuleRequest]]).
    moduleRequest = e.moduleRequest();
    importedModule = HostResolveImportedModule(cx, module, moduleRequest,
                                               ModuleStatus::Unlinked);
    if (!importedModule) {
      return false;
    }

    // Step 8.b. Let resolution be ? importedModule.ResolveExport(exportName,
    //           resolveSet).
    if (!ModuleResolveExport(cx, importedModule, exportName, resolveSet,
                             &resolution)) {
      return false;
    }

    // Step 8.c. If resolution is ambiguous, return ambiguous.
    if (resolution == StringValue(cx->names().ambiguous)) {
      result.set(resolution);
      return true;
    }

    // Step 8.d. If resolution is not null, then:
    if (!resolution.isNull()) {
      // Step 8.d.i. Assert: resolution is a ResolvedBinding Record.
      binding = &resolution.toObject().as<ResolvedBindingObject>();

      // Step 8.d.ii. If starResolution is null, set starResolution to
      // resolution.
      if (!starResolution) {
        starResolution = binding;
      } else {
        // Step 8.d.iii. Else:
        // Step 8.d.iii.1. Assert: There is more than one * import that includes
        //                 the requested name.
        // Step 8.d.iii.2. If resolution.[[Module]] and
        //                 starResolution.[[Module]] are not the same Module
        //                 Record, return ambiguous.
        // Step 8.d.iii.3. If resolution.[[BindingName]] is namespace and
        //                 starResolution.[[BindingName]] is not namespace, or
        //                 if resolution.[[BindingName]] is not namespace and
        //                 starResolution.[[BindingName]] is namespace, return
        //                 ambiguous.
        // Step 8.d.iii.4. If resolution.[[BindingName]] is a String,
        //                 starResolution.[[BindingName]] is a String, and
        //                 SameValue(resolution.[[BindingName]],
        //                 starResolution.[[BindingName]]) is false, return
        //                 ambiguous.
        if (binding->module() != starResolution->module() ||
            binding->bindingName() != starResolution->bindingName()) {
          result.set(StringValue(cx->names().ambiguous));
          return true;
        }
      }
    }
  }

  // Step 9. Return starResolution.
  result.setObjectOrNull(starResolution);
  return true;
}

// https://tc39.es/ecma262/#sec-getmodulenamespace
// ES2023 16.2.1.10 GetModuleNamespace
ModuleNamespaceObject* js::GetOrCreateModuleNamespace(
    JSContext* cx, Handle<ModuleObject*> module) {
  // Step 1. Assert: If module is a Cyclic Module Record, then module.[[Status]]
  //         is not unlinked.
  MOZ_ASSERT(module->status() != ModuleStatus::Unlinked);

  // Step 2. Let namespace be module.[[Namespace]].
  Rooted<ModuleNamespaceObject*> ns(cx, module->namespace_());

  // Step 3. If namespace is empty, then:
  if (!ns) {
    // Step 3.a. Let exportedNames be ? module.GetExportedNames().
    Rooted<ModuleSet> exportStarSet(cx);
    Rooted<ExportNameVector> exportedNames(cx);
    if (!ModuleGetExportedNames(cx, module, &exportStarSet, &exportedNames)) {
      return nullptr;
    }

    // Step 3.b. Let unambiguousNames be a new empty List.
    Rooted<UniquePtr<ExportNameVector>> unambiguousNames(
        cx, cx->make_unique<ExportNameVector>());
    if (!unambiguousNames) {
      return nullptr;
    }

    // Step 3.c. For each element name of exportedNames, do:
    Rooted<JSAtom*> name(cx);
    Rooted<Value> resolution(cx);
    for (JSAtom* atom : exportedNames) {
      name = atom;

      // Step 3.c.i. Let resolution be ? module.ResolveExport(name).
      if (!ModuleResolveExport(cx, module, name, &resolution)) {
        return nullptr;
      }

      // Step 3.c.ii. If resolution is a ResolvedBinding Record, append name to
      //              unambiguousNames.
      if (resolution.isObject() && !unambiguousNames->append(name)) {
        ReportOutOfMemory(cx);
        return nullptr;
      }
    }

    // Step 3.d. Set namespace to ModuleNamespaceCreate(module,
    //           unambiguousNames).
    ns = ModuleNamespaceCreate(cx, module, &unambiguousNames);
  }

  // Step 4. Return namespace.
  return ns;
}

static bool IsResolvedBinding(JSContext* cx, Handle<Value> resolution) {
  MOZ_ASSERT(resolution.isObjectOrNull() ||
             resolution.toString() == cx->names().ambiguous);
  return resolution.isObject();
}

static void InitNamespaceBinding(JSContext* cx,
                                 Handle<ModuleEnvironmentObject*> env,
                                 Handle<JSAtom*> name,
                                 Handle<ModuleNamespaceObject*> ns) {
  // The property already exists in the evironment but is not writable, so set
  // the slot directly.
  RootedId id(cx, AtomToId(name));
  mozilla::Maybe<PropertyInfo> prop = env->lookup(cx, id);
  MOZ_ASSERT(prop.isSome());
  env->setSlot(prop->slot(), ObjectValue(*ns));
}

struct AtomComparator {
  bool operator()(JSAtom* a, JSAtom* b, bool* lessOrEqualp) {
    int32_t result = CompareStrings(a, b);
    *lessOrEqualp = (result <= 0);
    return true;
  }
};

// https://tc39.es/ecma262/#sec-modulenamespacecreate
// ES2023 10.4.6.12 ModuleNamespaceCreate
static ModuleNamespaceObject* ModuleNamespaceCreate(
    JSContext* cx, Handle<ModuleObject*> module,
    MutableHandle<UniquePtr<ExportNameVector>> exports) {
  // Step 1. Assert: module.[[Namespace]] is empty.
  MOZ_ASSERT(!module->namespace_());

  // Step 6. Let sortedExports be a List whose elements are the elements of
  //         exports ordered as if an Array of the same values had been sorted
  //         using %Array.prototype.sort% using undefined as comparefn.
  ExportNameVector scratch;
  if (!scratch.resize(exports->length())) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  MOZ_ALWAYS_TRUE(MergeSort(exports->begin(), exports->length(),
                            scratch.begin(), AtomComparator()));

  // Steps 2 - 5.
  Rooted<ModuleNamespaceObject*> ns(
      cx, ModuleObject::createNamespace(cx, module, exports));
  if (!ns) {
    return nullptr;
  }

  // Pre-compute all binding mappings now instead of on each access.
  // See:
  // https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-get-p-receiver
  // ES2023 10.4.6.8 Module Namespace Exotic Object [[Get]]
  Rooted<JSAtom*> name(cx);
  Rooted<Value> resolution(cx);
  Rooted<ResolvedBindingObject*> binding(cx);
  Rooted<ModuleObject*> importedModule(cx);
  Rooted<ModuleNamespaceObject*> importedNamespace(cx);
  Rooted<JSAtom*> bindingName(cx);
  for (JSAtom* atom : ns->exports()) {
    name = atom;

    if (!ModuleResolveExport(cx, module, name, &resolution)) {
      return nullptr;
    }

    MOZ_ASSERT(IsResolvedBinding(cx, resolution));
    binding = &resolution.toObject().as<ResolvedBindingObject>();
    importedModule = binding->module();
    bindingName = binding->bindingName();

    if (bindingName == cx->names().starNamespaceStar) {
      importedNamespace = GetOrCreateModuleNamespace(cx, importedModule);
      if (!importedNamespace) {
        return nullptr;
      }

      // The spec uses an immutable binding here but we have already generated
      // bytecode for an indirect binding. Instead, use an indirect binding to
      // "*namespace*" slot of the target environment.
      Rooted<ModuleEnvironmentObject*> env(
          cx, &importedModule->initialEnvironment());
      InitNamespaceBinding(cx, env, bindingName, importedNamespace);
    }

    if (!ns->addBinding(cx, name, importedModule, bindingName)) {
      return nullptr;
    }
  }

  // Step 10. Return M.
  return ns;
}

static void ThrowResolutionError(JSContext* cx, Handle<ModuleObject*> module,
                                 Handle<Value> resolution, bool isDirectImport,
                                 Handle<JSAtom*> name, uint32_t line,
                                 uint32_t column) {
  MOZ_ASSERT(line != 0);

  bool isAmbiguous = resolution == StringValue(cx->names().ambiguous);

  static constexpr unsigned ErrorNumbers[2][2] = {
      {JSMSG_AMBIGUOUS_IMPORT, JSMSG_MISSING_IMPORT},
      {JSMSG_AMBIGUOUS_INDIRECT_EXPORT, JSMSG_MISSING_INDIRECT_EXPORT}};
  unsigned errorNumber = ErrorNumbers[isDirectImport][isAmbiguous];

  const JSErrorFormatString* errorString =
      GetErrorMessage(nullptr, errorNumber);
  MOZ_ASSERT(errorString);

  MOZ_ASSERT(errorString->argCount == 0);
  Rooted<JSString*> message(cx, JS_NewStringCopyZ(cx, errorString->format));
  if (!message) {
    return;
  }

  Rooted<JSString*> separator(cx, JS_NewStringCopyZ(cx, ": "));
  if (!separator) {
    return;
  }

  message = ConcatStrings<CanGC>(cx, message, separator);
  if (!message) {
    return;
  }

  message = ConcatStrings<CanGC>(cx, message, name);
  if (!message) {
    return;
  }

  RootedString filename(cx);
  if (const char* chars = module->script()->filename()) {
    filename =
        JS_NewStringCopyUTF8Z(cx, JS::ConstUTF8CharsZ(chars, strlen(chars)));
  } else {
    filename = cx->names().empty;
  }
  if (!filename) {
    return;
  }

  RootedValue error(cx);
  if (!JS::CreateError(cx, JSEXN_SYNTAXERR, nullptr, filename, line, column,
                       nullptr, message, JS::NothingHandleValue, &error)) {
    return;
  }

  cx->setPendingException(error, nullptr);
}

// https://tc39.es/ecma262/#sec-source-text-module-record-initialize-environment
// ES2023 16.2.1.6.4 InitializeEnvironment
bool js::ModuleInitializeEnvironment(JSContext* cx,
                                     Handle<ModuleObject*> module) {
  MOZ_ASSERT(module->status() == ModuleStatus::Linking);

  // Step 1. For each ExportEntry Record e of module.[[IndirectExportEntries]],
  //         do:
  Rooted<JSAtom*> exportName(cx);
  Rooted<Value> resolution(cx);
  for (const ExportEntry& e : module->indirectExportEntries()) {
    // Step 1.a. Let resolution be ? module.ResolveExport(e.[[ExportName]]).
    exportName = e.exportName();
    if (!ModuleResolveExport(cx, module, exportName, &resolution)) {
      return false;
    }

    // Step 1.b. If resolution is null or ambiguous, throw a SyntaxError
    //           exception.
    if (!IsResolvedBinding(cx, resolution)) {
      ThrowResolutionError(cx, module, resolution, false, exportName,
                           e.lineNumber(), e.columnNumber());
      return false;
    }
  }

  // Step 5. Let env be NewModuleEnvironment(realm.[[GlobalEnv]]).
  // Step 6. Set module.[[Environment]] to env.
  // Note that we have already created the environment by this point.
  Rooted<ModuleEnvironmentObject*> env(cx, &module->initialEnvironment());

  // Step 7. For each ImportEntry Record in of module.[[ImportEntries]], do:
  Rooted<ModuleRequestObject*> moduleRequest(cx);
  Rooted<ModuleObject*> importedModule(cx);
  Rooted<JSAtom*> importName(cx);
  Rooted<JSAtom*> localName(cx);
  Rooted<ModuleObject*> sourceModule(cx);
  Rooted<JSAtom*> bindingName(cx);
  for (const ImportEntry& in : module->importEntries()) {
    // Step 7.a. Let importedModule be ! HostResolveImportedModule(module,
    //           in.[[ModuleRequest]]).
    moduleRequest = in.moduleRequest();
    importedModule = HostResolveImportedModule(cx, module, moduleRequest,
                                               ModuleStatus::Linking);
    if (!importedModule) {
      return false;
    }

    localName = in.localName();
    importName = in.importName();

    // Step 7.c. If in.[[ImportName]] is namespace-object, then:
    if (!importName) {
      // Step 7.c.i. Let namespace be ? GetModuleNamespace(importedModule).
      Rooted<ModuleNamespaceObject*> ns(
          cx, GetOrCreateModuleNamespace(cx, importedModule));
      if (!ns) {
        return false;
      }

      // Step 7.c.ii. Perform ! env.CreateImmutableBinding(in.[[LocalName]],
      // true). This happens when the environment is created.

      // Step 7.c.iii. Perform ! env.InitializeBinding(in.[[LocalName]],
      // namespace).
      InitNamespaceBinding(cx, env, localName, ns);
    } else {
      // Step 7.d. Else:
      // Step 7.d.i. Let resolution be ?
      // importedModule.ResolveExport(in.[[ImportName]]).
      if (!ModuleResolveExport(cx, importedModule, importName, &resolution)) {
        return false;
      }

      // Step 7.d.ii. If resolution is null or ambiguous, throw a SyntaxError
      //              exception.
      if (!IsResolvedBinding(cx, resolution)) {
        ThrowResolutionError(cx, module, resolution, true, importName,
                             in.lineNumber(), in.columnNumber());
        return false;
      }

      auto* binding = &resolution.toObject().as<ResolvedBindingObject>();
      sourceModule = binding->module();
      bindingName = binding->bindingName();

      // Step 7.d.iii. If resolution.[[BindingName]] is namespace, then:
      if (bindingName == cx->names().starNamespaceStar) {
        // Step 7.d.iii.1. Let namespace be ?
        //                 GetModuleNamespace(resolution.[[Module]]).
        Rooted<ModuleNamespaceObject*> ns(
            cx, GetOrCreateModuleNamespace(cx, sourceModule));
        if (!ns) {
          return false;
        }

        // Step 7.d.iii.2. Perform !
        //                 env.CreateImmutableBinding(in.[[LocalName]], true).
        // Step 7.d.iii.3. Perform ! env.InitializeBinding(in.[[LocalName]],
        //                 namespace).
        //
        // This should be InitNamespaceBinding, but we have already generated
        // bytecode assuming an indirect binding. Instead, ensure a special
        // "*namespace*"" binding exists on the target module's environment. We
        // then generate an indirect binding to this synthetic binding.
        Rooted<ModuleEnvironmentObject*> sourceEnv(
            cx, &sourceModule->initialEnvironment());
        InitNamespaceBinding(cx, sourceEnv, bindingName, ns);
        if (!env->createImportBinding(cx, localName, sourceModule,
                                      bindingName)) {
          return false;
        }
      } else {
        // Step 7.d.iv. Else:
        // Step 7.d.iv.1. 1. Perform env.CreateImportBinding(in.[[LocalName]],
        //                   resolution.[[Module]], resolution.[[BindingName]]).
        if (!env->createImportBinding(cx, localName, sourceModule,
                                      bindingName)) {
          return false;
        }
      }
    }
  }

  // Steps 8-26.
  //
  // Some of these do not need to happen for practical purposes. For steps
  // 21-23, the bindings that can be handled in a similar way to regulars
  // scripts are done separately. Function Declarations are special due to
  // hoisting and are handled within this function. See ModuleScope and
  // ModuleEnvironmentObject for further details.

  // Step 24. For each element d of lexDeclarations, do:
  // Step 24.a. For each element dn of the BoundNames of d, do:
  // Step 24.a.iii. If d is a FunctionDeclaration, a GeneratorDeclaration, an
  //                AsyncFunctionDeclaration, or an AsyncGeneratorDeclaration,
  //                then:
  // Step 24.a.iii.1 Let fo be InstantiateFunctionObject of d with arguments env
  //                 and privateEnv.
  // Step 24.a.iii.2. Perform ! env.InitializeBinding(dn, fo).
  return ModuleObject::instantiateFunctionDeclarations(cx, module);
}

// https://tc39.es/ecma262/#sec-moduledeclarationlinking
// ES2023 16.2.1.5.1 Link
bool js::ModuleLink(JSContext* cx, Handle<ModuleObject*> module) {
  // Step 1. Assert: module.[[Status]] is not linking or evaluating.
  ModuleStatus status = module->status();
  if (status == ModuleStatus::Linking || status == ModuleStatus::Evaluating) {
    ThrowUnexpectedModuleStatus(cx, status);
    return false;
  }

  // Step 2. Let stack be a new empty List.
  Rooted<ModuleVector> stack(cx);

  // Step 3. Let result be Completion(InnerModuleLinking(module, stack, 0)).
  size_t ignored;
  bool ok = InnerModuleLinking(cx, module, &stack, 0, &ignored);

  // Step 4. If result is an abrupt completion, then:
  if (!ok) {
    // Step 4.a. For each Cyclic Module Record m of stack, do:
    for (ModuleObject* m : stack) {
      // Step 4.a.i. Assert: m.[[Status]] is linking.
      MOZ_ASSERT(m->status() == ModuleStatus::Linking);
      // Step 4.a.ii. Set m.[[Status]] to unlinked.
      m->setStatus(ModuleStatus::Unlinked);
      m->clearDfsIndexes();
    }

    // Step 4.b. Assert: module.[[Status]] is unlinked.
    MOZ_ASSERT(module->status() == ModuleStatus::Unlinked);

    // Step 4.c.
    return false;
  }

  // Step 5. Assert: module.[[Status]] is linked, evaluating-async, or
  //         evaluated.
  MOZ_ASSERT(module->status() == ModuleStatus::Linked ||
             module->status() == ModuleStatus::EvaluatingAsync ||
             module->status() == ModuleStatus::Evaluated);

  // Step 6. Assert: stack is empty.
  MOZ_ASSERT(stack.empty());

  // Step 7. Return unused.
  return true;
}

// https://tc39.es/ecma262/#sec-InnerModuleLinking
// ES2023 16.2.1.5.1.1 InnerModuleLinking
static bool InnerModuleLinking(JSContext* cx, Handle<ModuleObject*> module,
                               MutableHandle<ModuleVector> stack, size_t index,
                               size_t* indexOut) {
  // Step 2. If module.[[Status]] is linking, linked, evaluating-async, or
  //         evaluated, then:
  if (module->status() == ModuleStatus::Linking ||
      module->status() == ModuleStatus::Linked ||
      module->status() == ModuleStatus::EvaluatingAsync ||
      module->status() == ModuleStatus::Evaluated) {
    // Step 2.a. Return index.
    *indexOut = index;
    return true;
  }

  // Step 3. Assert: module.[[Status]] is unlinked.
  if (module->status() != ModuleStatus::Unlinked) {
    ThrowUnexpectedModuleStatus(cx, module->status());
    return false;
  }

  // Step 8. Append module to stack.
  // Do this before changing the status so that we can recover on failure.
  if (!stack.append(module)) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Step 4. Set module.[[Status]] to linking.
  module->setStatus(ModuleStatus::Linking);

  // Step 5. Set module.[[DFSIndex]] to index.
  module->setDfsIndex(index);

  // Step 6. Set module.[[DFSAncestorIndex]] to index.
  module->setDfsAncestorIndex(index);

  // Step 7. Set index to index + 1.
  index++;

  // Step 9. For each String required that is an element of
  //         module.[[RequestedModules]], do:
  Rooted<ModuleRequestObject*> moduleRequest(cx);
  Rooted<ModuleObject*> requiredModule(cx);
  for (const RequestedModule& request : module->requestedModules()) {
    moduleRequest = request.moduleRequest();

    // Step 9.a. Let requiredModule be ? HostResolveImportedModule(module,
    //           required).
    requiredModule = HostResolveImportedModule(cx, module, moduleRequest,
                                               ModuleStatus::Unlinked);
    if (!requiredModule) {
      return false;
    }

    // Step 9.b. Set index to ? InnerModuleLinking(requiredModule, stack,
    //           index).
    if (!InnerModuleLinking(cx, requiredModule, stack, index, &index)) {
      return false;
    }

    // Step 9.c. If requiredModule is a Cyclic Module Record, then:
    // Step 9.c.i. Assert: requiredModule.[[Status]] is either linking, linked,
    //             evaluating-async, or evaluated.
    MOZ_ASSERT(requiredModule->status() == ModuleStatus::Linking ||
               requiredModule->status() == ModuleStatus::Linked ||
               requiredModule->status() == ModuleStatus::EvaluatingAsync ||
               requiredModule->status() == ModuleStatus::Evaluated);

    // Step 9.c.ii. Assert: requiredModule.[[Status]] is linking if and only if
    //              requiredModule is in stack.
    MOZ_ASSERT((requiredModule->status() == ModuleStatus::Linking) ==
               ContainsElement(stack, requiredModule));

    // Step 9.c.iii. If requiredModule.[[Status]] is linking, then:
    if (requiredModule->status() == ModuleStatus::Linking) {
      // Step 9.c.iii.1. Set module.[[DFSAncestorIndex]] to
      //                 min(module.[[DFSAncestorIndex]],
      //                 requiredModule.[[DFSAncestorIndex]]).
      module->setDfsAncestorIndex(std::min(module->dfsAncestorIndex(),
                                           requiredModule->dfsAncestorIndex()));
    }
  }

  // Step 10. Perform ? module.InitializeEnvironment().
  if (!ModuleInitializeEnvironment(cx, module)) {
    return false;
  }

  // Step 11. Assert: module occurs exactly once in stack.
  MOZ_ASSERT(CountElements(stack, module) == 1);

  // Step 12. Assert: module.[[DFSAncestorIndex]] <= module.[[DFSIndex]].
  MOZ_ASSERT(module->dfsAncestorIndex() <= module->dfsIndex());

  // Step 13. If module.[[DFSAncestorIndex]] = module.[[DFSIndex]], then
  if (module->dfsAncestorIndex() == module->dfsIndex()) {
    // Step 13.a.
    bool done = false;

    // Step 13.b. Repeat, while done is false:
    while (!done) {
      // Step 13.b.i. Let requiredModule be the last element in stack.
      // Step 13.b.ii. Remove the last element of stack.
      requiredModule = stack.popCopy();

      // Step 13.b.iv. Set requiredModule.[[Status]] to linked.
      requiredModule->setStatus(ModuleStatus::Linked);

      // Step 13.b.v. If requiredModule and module are the same Module Record,
      //              set done to true.
      done = requiredModule == module;
    }
  }

  // Step 14. Return index.
  *indexOut = index;
  return true;
}

// https://tc39.es/ecma262/#sec-moduleevaluation
// ES2023 16.2.1.5.2 Evaluate
bool js::ModuleEvaluate(JSContext* cx, Handle<ModuleObject*> moduleArg,
                        MutableHandle<Value> result) {
  Rooted<ModuleObject*> module(cx, moduleArg);

  // Step 2. Assert: module.[[Status]] is linked, evaluating-async, or
  //         evaluated.
  ModuleStatus status = module->status();
  if (status != ModuleStatus::Linked &&
      status != ModuleStatus::EvaluatingAsync &&
      status != ModuleStatus::Evaluated) {
    ThrowUnexpectedModuleStatus(cx, status);
    return false;
  }

  // Note: we return early in the error case, as the spec assumes we can get the
  // cycle root of |module| which may not be available.
  if (module->hadEvaluationError()) {
    Rooted<PromiseObject*> capability(cx);
    if (!module->hasTopLevelCapability()) {
      capability = ModuleObject::createTopLevelCapability(cx, module);
      if (!capability) {
        return false;
      }

      Rooted<Value> error(cx, module->evaluationError());
      if (!ModuleObject::topLevelCapabilityReject(cx, module, error)) {
        return false;
      }
    }

    capability = module->topLevelCapability();
    MOZ_ASSERT(JS::GetPromiseState(capability) == JS::PromiseState::Rejected);
    MOZ_ASSERT(JS::GetPromiseResult(capability) == module->evaluationError());
    result.set(ObjectValue(*capability));
    return true;
  }

  // Step 3. If module.[[Status]] is evaluating-async or evaluated, set module
  //         to module.[[CycleRoot]].
  if (module->status() == ModuleStatus::EvaluatingAsync ||
      module->status() == ModuleStatus::Evaluated) {
    module = module->getCycleRoot();
  }

  // Step 4. If module.[[TopLevelCapability]] is not empty, then:
  if (module->hasTopLevelCapability()) {
    // Step 4.a. Return module.[[TopLevelCapability]].[[Promise]].
    result.set(ObjectValue(*module->topLevelCapability()));
    return true;
  }

  // Step 5. Let stack be a new empty List.
  Rooted<ModuleVector> stack(cx);

  // Step 6. Let capability be ! NewPromiseCapability(%Promise%).
  // Step 7. Set module.[[TopLevelCapability]] to capability.
  Rooted<PromiseObject*> capability(
      cx, ModuleObject::createTopLevelCapability(cx, module));
  if (!capability) {
    return false;
  }

  // Step 8. Let result be Completion(InnerModuleEvaluation(module, stack, 0)).
  size_t ignored;
  bool ok = InnerModuleEvaluation(cx, module, &stack, 0, &ignored);

  // Step 9. f result is an abrupt completion, then:
  if (!ok) {
    // Attempt to take any pending exception, but make sure we still handle
    // uncatchable exceptions.
    Rooted<Value> error(cx);
    if (cx->isExceptionPending()) {
      std::ignore = cx->getPendingException(&error);
      cx->clearPendingException();
    }

    // Step 9.a. For each Cyclic Module Record m of stack, do
    for (ModuleObject* m : stack) {
      // Step 9.a.i. Assert: m.[[Status]] is evaluating.
      MOZ_ASSERT(m->status() == ModuleStatus::Evaluating);

      // Step 9.a.ii. Set m.[[Status]] to evaluated.
      // Step 9.a.iii. Set m.[[EvaluationError]] to result.
      m->setEvaluationError(error);
    }

    // Handle OOM when appending to the stack or over-recursion errors.
    if (stack.empty() && !module->hadEvaluationError()) {
      module->setEvaluationError(error);
    }

    // Step 9.b. Assert: module.[[Status]] is evaluated.
    MOZ_ASSERT(module->status() == ModuleStatus::Evaluated);

    // Step 9.c. Assert: module.[[EvaluationError]] is result.
    MOZ_ASSERT(module->evaluationError() == error);

    // Step 9.d. Perform ! Call(capability.[[Reject]], undefined,
    //           result.[[Value]]).
    if (!ModuleObject::topLevelCapabilityReject(cx, module, error)) {
      return false;
    }
  } else {
    // Step 10. Else:
    // Step 10.a. Assert: module.[[Status]] is evaluating-async or evaluated.
    MOZ_ASSERT(module->status() == ModuleStatus::EvaluatingAsync ||
               module->status() == ModuleStatus::Evaluated);

    // Step 10.b. Assert: module.[[EvaluationError]] is empty.
    MOZ_ASSERT(!module->hadEvaluationError());

    // Step 10.c. If module.[[AsyncEvaluation]] is false, then:
    if (module->status() == ModuleStatus::Evaluated) {
      // Step 10.c.ii. Perform ! Call(capability.[[Resolve]], undefined,
      //               undefined).
      if (!ModuleObject::topLevelCapabilityResolve(cx, module)) {
        return false;
      }
    }

    // Step 10.d. Assert: stack is empty.
    MOZ_ASSERT(stack.empty());
  }

  // Step 11. Return capability.[[Promise]].
  result.set(ObjectValue(*capability));
  return true;
}

// https://tc39.es/ecma262/#sec-innermoduleevaluation
// 16.2.1.5.2.1 InnerModuleEvaluation
static bool InnerModuleEvaluation(JSContext* cx, Handle<ModuleObject*> module,
                                  MutableHandle<ModuleVector> stack,
                                  size_t index, size_t* indexOut) {
  // Step 2. If module.[[Status]] is evaluating-async or evaluated, then:
  if (module->status() == ModuleStatus::EvaluatingAsync ||
      module->status() == ModuleStatus::Evaluated) {
    // Step 2.a. If module.[[EvaluationError]] is empty, return index.
    if (!module->hadEvaluationError()) {
      *indexOut = index;
      return true;
    }

    // Step 2.b. Otherwise, return ? module.[[EvaluationError]].
    Rooted<Value> error(cx, module->evaluationError());
    cx->setPendingException(error, ShouldCaptureStack::Maybe);
    return false;
  }

  // Step 3. If module.[[Status]] is evaluating, return index.
  if (module->status() == ModuleStatus::Evaluating) {
    *indexOut = index;
    return true;
  }

  // Step 4. Assert: module.[[Status]] is linked.
  MOZ_ASSERT(module->status() == ModuleStatus::Linked);

  // Step 10. Append module to stack.
  // Do this before changing the status so that we can recover on failure.
  if (!stack.append(module)) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Step 5. Set module.[[Status]] to evaluating.
  module->setStatus(ModuleStatus::Evaluating);

  // Step 6. Set module.[[DFSIndex]] to index.
  module->setDfsIndex(index);

  // Step 7. Set module.[[DFSAncestorIndex]] to index.
  module->setDfsAncestorIndex(index);

  // Step 8. Set module.[[PendingAsyncDependencies]] to 0.
  module->setPendingAsyncDependencies(0);

  // Step 9. Set index to index + 1.
  index++;

  // Step 11. For each String required of module.[[RequestedModules]], do:
  Rooted<ModuleRequestObject*> required(cx);
  Rooted<ModuleObject*> requiredModule(cx);
  for (const RequestedModule& request : module->requestedModules()) {
    required = request.moduleRequest();

    // Step 11.a. Let requiredModule be ! HostResolveImportedModule(module,
    //            required).
    // Step 11.b. NOTE: Link must be completed successfully prior to invoking
    //            this method, so every requested module is guaranteed to
    //            resolve successfully.
    requiredModule =
        HostResolveImportedModule(cx, module, required, ModuleStatus::Linked);
    if (!requiredModule) {
      return false;
    }

    // Step 11.c. Set index to ? InnerModuleEvaluation(requiredModule, stack,
    //            index).
    if (!InnerModuleEvaluation(cx, requiredModule, stack, index, &index)) {
      return false;
    }

    // Step 11.d. If requiredModule is a Cyclic Module Record, then:
    // Step 11.d.i. Assert: requiredModule.[[Status]] is either evaluating,
    //              evaluating-async, or evaluated.
    MOZ_ASSERT(requiredModule->status() == ModuleStatus::Evaluating ||
               requiredModule->status() == ModuleStatus::EvaluatingAsync ||
               requiredModule->status() == ModuleStatus::Evaluated);

    // Step 11.d.ii. Assert: requiredModule.[[Status]] is evaluating if and only
    //               if requiredModule is in stack.
    MOZ_ASSERT((requiredModule->status() == ModuleStatus::Evaluating) ==
               ContainsElement(stack, requiredModule));

    // Step 11.d.iii. If requiredModule.[[Status]] is evaluating, then:
    if (requiredModule->status() == ModuleStatus::Evaluating) {
      // Step 11.d.iii.1. Set module.[[DFSAncestorIndex]] to
      //                  min(module.[[DFSAncestorIndex]],
      //                  requiredModule.[[DFSAncestorIndex]]).
      module->setDfsAncestorIndex(std::min(module->dfsAncestorIndex(),
                                           requiredModule->dfsAncestorIndex()));
    } else {
      // Step 11.d.iv. Else:
      // Step 11.d.iv.1. Set requiredModule to requiredModule.[[CycleRoot]].
      requiredModule = requiredModule->getCycleRoot();

      // Step 11.d.iv.2. Assert: requiredModule.[[Status]] is evaluating-async
      //                 or evaluated.
      MOZ_ASSERT(requiredModule->status() >= ModuleStatus::EvaluatingAsync ||
                 requiredModule->status() == ModuleStatus::Evaluated);

      // Step 11.d.iv.3. If requiredModule.[[EvaluationError]] is not empty,
      //                 return ? requiredModule.[[EvaluationError]].
      if (requiredModule->hadEvaluationError()) {
        Rooted<Value> error(cx, requiredModule->evaluationError());
        cx->setPendingException(error, ShouldCaptureStack::Maybe);
        return false;
      }
    }

    // Step 11.d.v. If requiredModule.[[AsyncEvaluation]] is true, then:
    if (requiredModule->isAsyncEvaluating() &&
        requiredModule->status() != ModuleStatus::Evaluated) {
      // Step 11.d.v.2. Append module to requiredModule.[[AsyncParentModules]].
      if (!ModuleObject::appendAsyncParentModule(cx, requiredModule, module)) {
        return false;
      }

      // Step 11.d.v.1. Set module.[[PendingAsyncDependencies]] to
      //                module.[[PendingAsyncDependencies]] + 1.
      module->setPendingAsyncDependencies(module->pendingAsyncDependencies() +
                                          1);
    }
  }

  // Step 12. If module.[[PendingAsyncDependencies]] > 0 or module.[[HasTLA]] is
  //          true, then:
  if (module->pendingAsyncDependencies() > 0 || module->hasTopLevelAwait()) {
    // Step 12.a. Assert: module.[[AsyncEvaluation]] is false and was never
    //            previously set to true.
    MOZ_ASSERT(!module->isAsyncEvaluating());

    // Step 12.b. Set module.[[AsyncEvaluation]] to true.
    // Step 12.c. NOTE: The order in which module records have their
    //            [[AsyncEvaluation]] fields transition to true is
    //            significant. (See 16.2.1.5.2.4.)
    module->setAsyncEvaluating();

    // Step 12.d. If module.[[PendingAsyncDependencies]] is 0, perform
    //            ExecuteAsyncModule(module).
    if (module->pendingAsyncDependencies() == 0) {
      if (!ExecuteAsyncModule(cx, module)) {
        return false;
      }
    }
  } else {
    // Step 13. Otherwise, perform ? module.ExecuteModule().
    if (!ModuleObject::execute(cx, module)) {
      return false;
    }
  }

  // Step 14. Assert: module occurs exactly once in stack.
  MOZ_ASSERT(CountElements(stack, module) == 1);

  // Step 15. Assert: module.[[DFSAncestorIndex]] <= module.[[DFSIndex]].
  MOZ_ASSERT(module->dfsAncestorIndex() <= module->dfsIndex());

  // Step 16. If module.[[DFSAncestorIndex]] = module.[[DFSIndex]], then:
  if (module->dfsAncestorIndex() == module->dfsIndex()) {
    // Step 16.a. Let done be false.
    bool done = false;

    // Step 16.b. Repeat, while done is false:
    while (!done) {
      // Step 16.b.i. Let requiredModule be the last element in stack.
      // Step 16.b.ii. Remove the last element of stack.
      requiredModule = stack.popCopy();

      // Step 16.b.iv. If requiredModule.[[AsyncEvaluation]] is false, set
      //               requiredModule.[[Status]] to evaluated.
      if (!requiredModule->isAsyncEvaluating()) {
        requiredModule->setStatus(ModuleStatus::Evaluated);
      } else {
        // Step 16.b.v. Otherwise, set requiredModule.[[Status]] to
        //              evaluating-async.
        requiredModule->setStatus(ModuleStatus::EvaluatingAsync);
      }

      // Step 16.b.vi. If requiredModule and module are the same Module Record,
      //               set done to true.
      done = requiredModule == module;

      // Step 16.b.vii. Set requiredModule.[[CycleRoot]] to module.
      requiredModule->setCycleRoot(module);
    }
  }

  // Step 17. Return index.
  *indexOut = index;
  return true;
}

// https://tc39.es/ecma262/#sec-execute-async-module
// ES2023 16.2.1.5.2.2 ExecuteAsyncModule
static bool ExecuteAsyncModule(JSContext* cx, Handle<ModuleObject*> module) {
  // Step 1. Assert: module.[[Status]] is evaluating or evaluating-async.
  MOZ_ASSERT(module->status() == ModuleStatus::Evaluating ||
             module->status() == ModuleStatus::EvaluatingAsync);

  // Step 2. Assert: module.[[HasTLA]] is true.
  MOZ_ASSERT(module->hasTopLevelAwait());

  // Steps 3 - 8 are performed by the AsyncAwait opcode.

  // Step 9. Perform ! module.ExecuteModule(capability).
  // Step 10. Return unused.
  return ModuleObject::execute(cx, module);
}

// https://tc39.es/ecma262/#sec-gather-available-ancestors
// ES2023 16.2.1.5.2.3 GatherAvailableAncestors
static bool GatherAvailableModuleAncestors(
    JSContext* cx, Handle<ModuleObject*> module,
    MutableHandle<ModuleVector> execList) {
  MOZ_ASSERT(module->status() == ModuleStatus::EvaluatingAsync);

  // Step 1. For each Cyclic Module Record m of module.[[AsyncParentModules]],
  //         do:
  Rooted<ListObject*> asyncParentModules(cx, module->asyncParentModules());
  Rooted<ModuleObject*> m(cx);
  for (uint32_t i = 0; i != asyncParentModules->length(); i++) {
    m = &asyncParentModules->getDenseElement(i).toObject().as<ModuleObject>();

    // Step 1.a. If execList does not contain m and
    //           m.[[CycleRoot]].[[EvaluationError]] is empty, then:
    //
    // Note: we also check whether m.[[EvaluationError]] is empty since an error
    // in synchronous execution can prevent the CycleRoot field from being set.
    if (!m->hadEvaluationError() && !m->getCycleRoot()->hadEvaluationError() &&
        !ContainsElement(execList, m)) {
      // Step 1.a.i. Assert: m.[[Status]] is evaluating-async.
      MOZ_ASSERT(m->status() == ModuleStatus::EvaluatingAsync);

      // Step 1.a.ii. Assert: m.[[EvaluationError]] is empty.
      MOZ_ASSERT(!m->hadEvaluationError());

      // Step 1.a.iii. Assert: m.[[AsyncEvaluation]] is true.
      MOZ_ASSERT(m->isAsyncEvaluating());

      // Step 1.a.iv. Assert: m.[[PendingAsyncDependencies]] > 0.
      MOZ_ASSERT(m->pendingAsyncDependencies() > 0);

      // Step 1.a.v. Set m.[[PendingAsyncDependencies]] to
      // m.[[PendingAsyncDependencies]] - 1.
      m->setPendingAsyncDependencies(m->pendingAsyncDependencies() - 1);

      // Step 1.a.vi. If m.[[PendingAsyncDependencies]] = 0, then:
      if (m->pendingAsyncDependencies() == 0) {
        // Step 1.a.vi.1. Append m to execList.
        if (!execList.append(m)) {
          return false;
        }

        // Step 1.a.vi.2. If m.[[HasTLA]] is false, perform
        //                GatherAvailableAncestors(m, execList).
        if (!m->hasTopLevelAwait() &&
            !GatherAvailableModuleAncestors(cx, m, execList)) {
          return false;
        }
      }
    }
  }

  // Step 2. Return unused.
  return true;
}

struct EvalOrderComparator {
  bool operator()(ModuleObject* a, ModuleObject* b, bool* lessOrEqualp) {
    int32_t result = int32_t(a->getAsyncEvaluatingPostOrder()) -
                     int32_t(b->getAsyncEvaluatingPostOrder());
    *lessOrEqualp = (result <= 0);
    return true;
  }
};

static void RejectExecutionWithPendingException(JSContext* cx,
                                                Handle<ModuleObject*> module) {
  // If there is no exception pending then we have been interrupted or have
  // OOM'd and all bets are off. We reject the execution by throwing
  // undefined. Not much more we can do.
  RootedValue exception(cx);
  if (cx->isExceptionPending()) {
    std::ignore = cx->getPendingException(&exception);
  }
  cx->clearPendingException();
  AsyncModuleExecutionRejected(cx, module, exception);
}

// https://tc39.es/ecma262/#sec-async-module-execution-fulfilled
// ES2023 16.2.1.5.2.4 AsyncModuleExecutionFulfilled
void js::AsyncModuleExecutionFulfilled(JSContext* cx,
                                       Handle<ModuleObject*> module) {
  // Step 1. If module.[[Status]] is evaluated, then:
  if (module->status() == ModuleStatus::Evaluated) {
    // Step 1.a. Assert: module.[[EvaluationError]] is not empty.
    MOZ_ASSERT(module->hadEvaluationError());

    // Step 1.b. Return unused.
    return;
  }

  // Step 2. Assert: module.[[Status]] is evaluating-async.
  MOZ_ASSERT(module->status() == ModuleStatus::EvaluatingAsync);

  // Step 3. Assert: module.[[AsyncEvaluation]] is true.
  MOZ_ASSERT(module->isAsyncEvaluating());

  // Step 4. Assert: module.[[EvaluationError]] is empty.
  MOZ_ASSERT(!module->hadEvaluationError());

  // The following steps are performed in a different order from the
  // spec. Gather available module ancestors before mutating the module object
  // as this can fail in our implementation.

  // Step 8. Let execList be a new empty List.
  Rooted<ModuleVector> execList(cx);

  // Step 9. Perform GatherAvailableAncestors(module, execList).
  if (!GatherAvailableModuleAncestors(cx, module, &execList)) {
    RejectExecutionWithPendingException(cx, module);
    return;
  }

  // Step 10. Let sortedExecList be a List whose elements are the elements of
  //          execList, in the order in which they had their [[AsyncEvaluation]]
  //          fields set to true in InnerModuleEvaluation.

  Rooted<ModuleVector> scratch(cx);
  if (!scratch.resize(execList.length())) {
    ReportOutOfMemory(cx);
    RejectExecutionWithPendingException(cx, module);
    return;
  }

  MOZ_ALWAYS_TRUE(MergeSort(execList.begin(), execList.length(),
                            scratch.begin(), EvalOrderComparator()));

  // Step 11. Assert: All elements of sortedExecList have their
  //          [[AsyncEvaluation]] field set to true,
  //          [[PendingAsyncDependencies]] field set to 0, and
  //          [[EvaluationError]] field set to empty.
#ifdef DEBUG
  for (ModuleObject* m : execList) {
    MOZ_ASSERT(m->isAsyncEvaluating());
    MOZ_ASSERT(m->pendingAsyncDependencies() == 0);
    MOZ_ASSERT(!m->hadEvaluationError());
  }
#endif

  // Return to original order of steps.

  ModuleObject::onTopLevelEvaluationFinished(module);

  // Step 6. Set module.[[Status]] to evaluated.
  module->setStatus(ModuleStatus::Evaluated);
  module->clearAsyncEvaluatingPostOrder();

  // Step 7. If module.[[TopLevelCapability]] is not empty, then:
  if (module->hasTopLevelCapability()) {
    // Step 7.a. Assert: module.[[CycleRoot]] is module.
    MOZ_ASSERT(module->getCycleRoot() == module);

    // Step 7.b. Perform ! Call(module.[[TopLevelCapability]].[[Resolve]],
    //           undefined, undefined).
    if (!ModuleObject::topLevelCapabilityResolve(cx, module)) {
      // If Resolve fails, there's nothing more we can do here.
      cx->clearPendingException();
    }
  }

  // Step 12. For each Cyclic Module Record m of sortedExecList, do:
  Rooted<ModuleObject*> m(cx);
  for (ModuleObject* obj : execList) {
    m = obj;

    // Step 12.a. If m.[[Status]] is evaluated, then:
    if (m->status() == ModuleStatus::Evaluated) {
      // Step 12.a.i. Assert: m.[[EvaluationError]] is not empty.
      MOZ_ASSERT(m->hadEvaluationError());
    } else if (m->hasTopLevelAwait()) {
      // Step 12.b. Else if m.[[HasTLA]] is true, then:
      // Step 12.b.i. Perform ExecuteAsyncModule(m).
      MOZ_ALWAYS_TRUE(ExecuteAsyncModule(cx, m));
    } else {
      // Step 12.c. Else:
      // Step 12.c.i. Let result be m.ExecuteModule().
      bool ok = ModuleObject::execute(cx, m);

      // Step 12.c.ii. If result is an abrupt completion, then:
      if (!ok) {
        // Step 12.c.ii.1. Perform AsyncModuleExecutionRejected(m,
        //                 result.[[Value]]).
        RejectExecutionWithPendingException(cx, m);
      } else {
        // Step 12.c.iii. Else:
        // Step 12.c.iii.1. Set m.[[Status]] to evaluated.
        m->setStatus(ModuleStatus::Evaluated);
        m->clearAsyncEvaluatingPostOrder();

        // Step 12.c.iii.2. If m.[[TopLevelCapability]] is not empty, then:
        if (m->hasTopLevelCapability()) {
          // Step 12.c.iii.2.a. Assert: m.[[CycleRoot]] is m.
          MOZ_ASSERT(m->getCycleRoot() == m);

          // Step 12.c.iii.2.b. Perform !
          //                    Call(m.[[TopLevelCapability]].[[Resolve]],
          //                    undefined, undefined).
          if (!ModuleObject::topLevelCapabilityResolve(cx, m)) {
            // If Resolve fails, there's nothing more we can do here.
            cx->clearPendingException();
          }
        }
      }
    }
  }

  // Step 13. Return unused.
}

// https://tc39.es/ecma262/#sec-async-module-execution-rejected
// ES2023 16.2.1.5.2.5 AsyncModuleExecutionRejected
void js::AsyncModuleExecutionRejected(JSContext* cx,
                                      Handle<ModuleObject*> module,
                                      HandleValue error) {
  // Step 1. If module.[[Status]] is evaluated, then:
  if (module->status() == ModuleStatus::Evaluated) {
    // Step 1.a. Assert: module.[[EvaluationError]] is not empty
    MOZ_ASSERT(module->hadEvaluationError());

    // Step 1.b. Return unused.
    return;
  }

  // Step 2. Assert: module.[[Status]] is evaluating-async.
  MOZ_ASSERT(module->status() == ModuleStatus::EvaluatingAsync);

  // Step 3. Assert: module.[[AsyncEvaluation]] is true.
  MOZ_ASSERT(module->isAsyncEvaluating());

  // Step 4. 4. Assert: module.[[EvaluationError]] is empty.
  MOZ_ASSERT(!module->hadEvaluationError());

  ModuleObject::onTopLevelEvaluationFinished(module);

  // Step 5. Set module.[[EvaluationError]] to ThrowCompletion(error).
  module->setEvaluationError(error);

  // Step 6. Set module.[[Status]] to evaluated.
  MOZ_ASSERT(module->status() == ModuleStatus::Evaluated);

  module->clearAsyncEvaluatingPostOrder();

  // Step 7. For each Cyclic Module Record m of module.[[AsyncParentModules]],
  //         do:
  Rooted<ListObject*> parents(cx, module->asyncParentModules());
  Rooted<ModuleObject*> parent(cx);
  for (uint32_t i = 0; i < parents->length(); i++) {
    parent = &parents->get(i).toObject().as<ModuleObject>();

    // Step 7.a. Perform AsyncModuleExecutionRejected(m, error).
    AsyncModuleExecutionRejected(cx, parent, error);
  }

  // Step 8. If module.[[TopLevelCapability]] is not empty, then:
  if (module->hasTopLevelCapability()) {
    // Step 8.a. Assert: module.[[CycleRoot]] is module.
    MOZ_ASSERT(module->getCycleRoot() == module);

    // Step 8.b. Perform ! Call(module.[[TopLevelCapability]].[[Reject]],
    //           undefined, error).
    if (!ModuleObject::topLevelCapabilityReject(cx, module, error)) {
      // If Reject fails, there's nothing more we can do here.
      cx->clearPendingException();
    }
  }

  // Step 9. Return unused.
}
