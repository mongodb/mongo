/* -*- Mode: javascript; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function CallModuleResolveHook(module, specifier, expectedMinimumStatus)
{
    let requestedModule = HostResolveImportedModule(module, specifier);
    if (requestedModule.status < expectedMinimumStatus)
        ThrowInternalError(JSMSG_BAD_MODULE_STATUS);

    return requestedModule;
}

// 15.2.1.16.2 GetExportedNames(exportStarSet)
function ModuleGetExportedNames(exportStarSet = [])
{
    if (!IsObject(this) || !IsModule(this)) {
        return callFunction(CallModuleMethodIfWrapped, this, exportStarSet,
                            "ModuleGetExportedNames");
    }

    // Step 1
    let module = this;

    // Step 2
    if (callFunction(ArrayIncludes, exportStarSet, module))
        return [];

    // Step 3
    _DefineDataProperty(exportStarSet, exportStarSet.length, module);

    // Step 4
    let exportedNames = [];
    let namesCount = 0;

    // Step 5
    let localExportEntries = module.localExportEntries;
    for (let i = 0; i < localExportEntries.length; i++) {
        let e = localExportEntries[i];
        _DefineDataProperty(exportedNames, namesCount++, e.exportName);
    }

    // Step 6
    let indirectExportEntries = module.indirectExportEntries;
    for (let i = 0; i < indirectExportEntries.length; i++) {
        let e = indirectExportEntries[i];
        _DefineDataProperty(exportedNames, namesCount++, e.exportName);
    }

    // Step 7
    let starExportEntries = module.starExportEntries;
    for (let i = 0; i < starExportEntries.length; i++) {
        let e = starExportEntries[i];
        let requestedModule = CallModuleResolveHook(module, e.moduleRequest,
                                                    MODULE_STATUS_INSTANTIATING);
        let starNames = callFunction(requestedModule.getExportedNames, requestedModule,
                                     exportStarSet);
        for (let j = 0; j < starNames.length; j++) {
            let n = starNames[j];
            if (n !== "default" && !callFunction(ArrayIncludes, exportedNames, n))
                _DefineDataProperty(exportedNames, namesCount++, n);
        }
    }

    return exportedNames;
}

function ModuleSetStatus(module, newStatus)
{
    assert(newStatus >= MODULE_STATUS_UNINSTANTIATED &&
           newStatus <= MODULE_STATUS_EVALUATED_ERROR,
           "Bad new module status in ModuleSetStatus");

    // Note that under OOM conditions we can fail the module instantiation
    // process even after modules have been marked as instantiated.
    assert((module.status <= MODULE_STATUS_INSTANTIATED &&
            newStatus === MODULE_STATUS_UNINSTANTIATED) ||
           newStatus > module.status,
           "New module status inconsistent with current status");

    UnsafeSetReservedSlot(module, MODULE_OBJECT_STATUS_SLOT, newStatus);
}

// 15.2.1.16.3 ResolveExport(exportName, resolveSet)
//
// Returns an object describing the location of the resolved export or
// indicating a failure.
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
function ModuleResolveExport(exportName, resolveSet = [])
{
    if (!IsObject(this) || !IsModule(this)) {
        return callFunction(CallModuleMethodIfWrapped, this, exportName, resolveSet,
                            "ModuleResolveExport");
    }

    // Step 1
    let module = this;

    // Step 2
    for (let i = 0; i < resolveSet.length; i++) {
        let r = resolveSet[i];
        if (r.module === module && r.exportName === exportName) {
            // This is a circular import request.
            return null;
        }
    }

    // Step 3
    _DefineDataProperty(resolveSet, resolveSet.length, {module, exportName});

    // Step 4
    let localExportEntries = module.localExportEntries;
    for (let i = 0; i < localExportEntries.length; i++) {
        let e = localExportEntries[i];
        if (exportName === e.exportName)
            return {module, bindingName: e.localName};
    }

    // Step 5
    let indirectExportEntries = module.indirectExportEntries;
    for (let i = 0; i < indirectExportEntries.length; i++) {
        let e = indirectExportEntries[i];
        if (exportName === e.exportName) {
            let importedModule = CallModuleResolveHook(module, e.moduleRequest,
                                                       MODULE_STATUS_UNINSTANTIATED);
            return callFunction(importedModule.resolveExport, importedModule, e.importName,
                                resolveSet);
        }
    }

    // Step 6
    if (exportName === "default") {
        // A default export cannot be provided by an export *.
        return null;
    }

    // Step 7
    let starResolution = null;

    // Step 8
    let starExportEntries = module.starExportEntries;
    for (let i = 0; i < starExportEntries.length; i++) {
        let e = starExportEntries[i];
        let importedModule = CallModuleResolveHook(module, e.moduleRequest,
                                                   MODULE_STATUS_UNINSTANTIATED);
        let resolution = callFunction(importedModule.resolveExport, importedModule, exportName,
                                      resolveSet);
        if (resolution === "ambiguous")
            return resolution;
        if (resolution !== null) {
            if (starResolution === null) {
                starResolution = resolution;
            } else {
                if (resolution.module !== starResolution.module ||
                    resolution.bindingName !== starResolution.bindingName)
                {
                    return "ambiguous";
                }
            }
        }
    }

    // Step 9
    return starResolution;
}

function IsResolvedBinding(resolution)
{
    assert(resolution === "ambiguous" || typeof resolution === "object",
           "Bad module resolution result");
    return typeof resolution === "object" && resolution !== null;
}

// 15.2.1.18 GetModuleNamespace(module)
function GetModuleNamespace(module)
{
    // Step 1
    assert(IsModule(module), "GetModuleNamespace called with non-module");

    // Steps 2-3
    assert(module.status !== MODULE_STATUS_UNINSTANTIATED &&
           module.status !== MODULE_STATUS_EVALUATED_ERROR,
           "Bad module state in GetModuleNamespace");

    // Step 4
    let namespace = module.namespace;

    // Step 3
    if (typeof namespace === "undefined") {
        let exportedNames = callFunction(module.getExportedNames, module);
        let unambiguousNames = [];
        for (let i = 0; i < exportedNames.length; i++) {
            let name = exportedNames[i];
            let resolution = callFunction(module.resolveExport, module, name);
            if (IsResolvedBinding(resolution))
                _DefineDataProperty(unambiguousNames, unambiguousNames.length, name);
        }
        namespace = ModuleNamespaceCreate(module, unambiguousNames);
    }

    // Step 4
    return namespace;
}

// 9.4.6.13 ModuleNamespaceCreate(module, exports)
function ModuleNamespaceCreate(module, exports)
{
    callFunction(ArraySort, exports);

    let ns = NewModuleNamespace(module, exports);

    // Pre-compute all bindings now rather than calling ResolveExport() on every
    // access.
    for (let i = 0; i < exports.length; i++) {
        let name = exports[i];
        let binding = callFunction(module.resolveExport, module, name);
        assert(IsResolvedBinding(binding), "Failed to resolve binding");
        AddModuleNamespaceBinding(ns, name, binding.module, binding.bindingName);
    }

    return ns;
}

function GetModuleEnvironment(module)
{
    assert(IsModule(module), "Non-module passed to GetModuleEnvironment");

    assert(module.status >= MODULE_STATUS_INSTANTIATING,
           "Attempt to access module environement before instantation");

    let env = UnsafeGetReservedSlot(module, MODULE_OBJECT_ENVIRONMENT_SLOT);
    assert(IsModuleEnvironment(env),
           "Module environment slot contains unexpected value");

    return env;
}

function CountArrayValues(array, value)
{
    let count = 0;
    for (let i = 0; i < array.length; i++) {
        if (array[i] === value)
            count++;
    }
    return count;
}

function ArrayContains(array, value)
{
    for (let i = 0; i < array.length; i++) {
        if (array[i] === value)
            return true;
    }
    return false;
}

function HandleModuleInstantiationFailure(module)
{
    // Reset the module to the "uninstantiated" state. Don't reset the
    // environment slot as the environment object will be required by any
    // possible future instantiation attempt.
    ModuleSetStatus(module, MODULE_STATUS_UNINSTANTIATED);
    UnsafeSetReservedSlot(module, MODULE_OBJECT_DFS_INDEX_SLOT, undefined);
    UnsafeSetReservedSlot(module, MODULE_OBJECT_DFS_ANCESTOR_INDEX_SLOT, undefined);
}

// 15.2.1.16.4 ModuleInstantiate()
function ModuleInstantiate()
{
    if (!IsObject(this) || !IsModule(this))
        return callFunction(CallModuleMethodIfWrapped, this, "ModuleInstantiate");

    // Step 1
    let module = this;

    // Step 2
    if (module.status === MODULE_STATUS_INSTANTIATING ||
        module.status === MODULE_STATUS_EVALUATING)
    {
        ThrowInternalError(JSMSG_BAD_MODULE_STATUS);
    }

    // Step 3
    let stack = [];

    // Steps 4-5
    try {
        InnerModuleInstantiation(module, stack, 0);
    } catch (error) {
        for (let i = 0; i < stack.length; i++) {
            let m = stack[i];
            assert(m.status === MODULE_STATUS_INSTANTIATING,
                   "Expected instantiating status during failed instantiation");
            HandleModuleInstantiationFailure(m);
        }

        // Handle OOM when appending to the stack or over-recursion errors.
        if (stack.length === 0)
            HandleModuleInstantiationFailure(module);

        assert(module.status === MODULE_STATUS_UNINSTANTIATED,
               "Expected uninstantiated status after failed instantiation");

        throw error;
    }

    // Step 6
    assert(module.status === MODULE_STATUS_INSTANTIATED ||
           module.status === MODULE_STATUS_EVALUATED ||
           module.status === MODULE_STATUS_EVALUATED_ERROR,
           "Bad module status after successful instantiation");

    // Step 7
    assert(stack.length === 0,
           "Stack should be empty after successful instantiation");

    // Step 8
    return undefined;
}
_SetCanonicalName(ModuleInstantiate, "ModuleInstantiate");

// 15.2.1.16.4.1 InnerModuleInstantiation(module, stack, index)
function InnerModuleInstantiation(module, stack, index)
{
    // Step 1
    // TODO: Support module records other than source text module records.

    // Step 2
    if (module.status === MODULE_STATUS_INSTANTIATING ||
        module.status === MODULE_STATUS_INSTANTIATED ||
        module.status === MODULE_STATUS_EVALUATED ||
        module.status === MODULE_STATUS_EVALUATED_ERROR)
    {
        return index;
    }

    // Step 3
    assert(module.status === MODULE_STATUS_UNINSTANTIATED,
          "Bad module status in ModuleDeclarationInstantiation");

    // Steps 4
    ModuleSetStatus(module, MODULE_STATUS_INSTANTIATING);

    // Step 5-7
    UnsafeSetReservedSlot(module, MODULE_OBJECT_DFS_INDEX_SLOT, index);
    UnsafeSetReservedSlot(module, MODULE_OBJECT_DFS_ANCESTOR_INDEX_SLOT, index);
    index++;

    // Step 8
    _DefineDataProperty(stack, stack.length, module);

    // Step 9
    let requestedModules = module.requestedModules;
    for (let i = 0; i < requestedModules.length; i++) {
        let required = requestedModules[i].moduleSpecifier;
        let requiredModule = CallModuleResolveHook(module, required, MODULE_STATUS_UNINSTANTIATED);

        index = InnerModuleInstantiation(requiredModule, stack, index);

        assert(requiredModule.status === MODULE_STATUS_INSTANTIATING ||
               requiredModule.status === MODULE_STATUS_INSTANTIATED ||
               requiredModule.status === MODULE_STATUS_EVALUATED ||
               requiredModule.status === MODULE_STATUS_EVALUATED_ERROR,
               "Bad required module status after InnerModuleInstantiation");

        assert((requiredModule.status === MODULE_STATUS_INSTANTIATING) ===
               ArrayContains(stack, requiredModule),
              "Required module should be in the stack iff it is currently being instantiated");

        assert(typeof requiredModule.dfsIndex === "number", "Bad dfsIndex");
        assert(typeof requiredModule.dfsAncestorIndex === "number", "Bad dfsAncestorIndex");

        if (requiredModule.status === MODULE_STATUS_INSTANTIATING) {
            UnsafeSetReservedSlot(module, MODULE_OBJECT_DFS_ANCESTOR_INDEX_SLOT,
                                  std_Math_min(module.dfsAncestorIndex,
                                               requiredModule.dfsAncestorIndex));
        }
    }

    // Step 10
    ModuleDeclarationEnvironmentSetup(module);

    // Steps 11-12
    assert(CountArrayValues(stack, module) === 1,
           "Current module should appear exactly once in the stack");
    assert(module.dfsAncestorIndex <= module.dfsIndex,
           "Bad DFS ancestor index");

    // Step 13
    if (module.dfsAncestorIndex === module.dfsIndex) {
        let requiredModule;
        do {
            requiredModule = callFunction(std_Array_pop, stack);
            ModuleSetStatus(requiredModule, MODULE_STATUS_INSTANTIATED);
        } while (requiredModule !== module);
    }

    // Step 15
    return index;
}

// 15.2.1.16.4.2 ModuleDeclarationEnvironmentSetup(module)
function ModuleDeclarationEnvironmentSetup(module)
{
    // Step 1
    let indirectExportEntries = module.indirectExportEntries;
    for (let i = 0; i < indirectExportEntries.length; i++) {
        let e = indirectExportEntries[i];
        let resolution = callFunction(module.resolveExport, module, e.exportName);
        if (!IsResolvedBinding(resolution)) {
            ThrowResolutionError(module, resolution, "indirectExport", e.exportName,
                                 e.lineNumber, e.columnNumber);
        }
    }

    // Steps 5-6
    // Note that we have already created the environment by this point.
    let env = GetModuleEnvironment(module);

    // Step 8
    let importEntries = module.importEntries;
    for (let i = 0; i < importEntries.length; i++) {
        let imp = importEntries[i];
        let importedModule = CallModuleResolveHook(module, imp.moduleRequest,
                                                   MODULE_STATUS_INSTANTIATING);
        if (imp.importName === "*") {
            let namespace = GetModuleNamespace(importedModule);
            CreateNamespaceBinding(env, imp.localName, namespace);
        } else {
            let resolution = callFunction(importedModule.resolveExport, importedModule,
                                          imp.importName);
            if (!IsResolvedBinding(resolution)) {
                ThrowResolutionError(module, resolution, "import", imp.importName,
                                     imp.lineNumber, imp.columnNumber);
            }

            CreateImportBinding(env, imp.localName, resolution.module, resolution.bindingName);
        }
    }

    InstantiateModuleFunctionDeclarations(module);
}

function ThrowResolutionError(module, resolution, kind, name, line, column)
{
    assert(module.status === MODULE_STATUS_INSTANTIATING,
           "Unexpected module status in ThrowResolutionError");

    assert(kind === "import" || kind === "indirectExport",
           "Unexpected kind in ThrowResolutionError");

    assert(line > 0,
           "Line number should be present for all imports and indirect exports");

    let ambiguous = resolution === "ambiguous";

    let errorNumber;
    if (kind === "import")
        errorNumber = ambiguous ? JSMSG_AMBIGUOUS_IMPORT : JSMSG_MISSING_IMPORT;
    else
        errorNumber = ambiguous ? JSMSG_AMBIGUOUS_INDIRECT_EXPORT : JSMSG_MISSING_INDIRECT_EXPORT;

    let message = GetErrorMessage(errorNumber) + ": " + name;
    let error = CreateModuleSyntaxError(module, line, column, message);
    throw error;
}

function GetModuleEvaluationError(module)
{
    assert(IsObject(module) && IsModule(module),
           "Non-module passed to GetModuleEvaluationError");
    assert(module.status === MODULE_STATUS_EVALUATED_ERROR,
           "Bad module status in GetModuleEvaluationError");
    return UnsafeGetReservedSlot(module, MODULE_OBJECT_EVALUATION_ERROR_SLOT);
}

function RecordModuleEvaluationError(module, error)
{
    // Set the module's EvaluationError slot to indicate a failed module
    // evaluation.

    assert(IsObject(module) && IsModule(module),
           "Non-module passed to RecordModuleEvaluationError");

    if (module.status === MODULE_STATUS_EVALUATED_ERROR) {
        // It would be nice to assert that |error| is the same as the one we
        // previously recorded, but that's not always true in the case of out of
        // memory and over-recursion errors.
        return;
    }

    ModuleSetStatus(module, MODULE_STATUS_EVALUATED_ERROR);
    UnsafeSetReservedSlot(module, MODULE_OBJECT_EVALUATION_ERROR_SLOT, error);
}

// 15.2.1.16.5 ModuleEvaluate()
function ModuleEvaluate()
{
    if (!IsObject(this) || !IsModule(this))
        return callFunction(CallModuleMethodIfWrapped, this, "ModuleEvaluate");

    // Step 1
    let module = this;

    // Step 2
    if (module.status !== MODULE_STATUS_INSTANTIATED &&
        module.status !== MODULE_STATUS_EVALUATED &&
        module.status !== MODULE_STATUS_EVALUATED_ERROR)
    {
        ThrowInternalError(JSMSG_BAD_MODULE_STATUS);
    }

    // Step 3
    let stack = [];

    // Steps 4-5
    try {
        InnerModuleEvaluation(module, stack, 0);
    } catch (error) {
        for (let i = 0; i < stack.length; i++) {
            let m = stack[i];
            assert(m.status === MODULE_STATUS_EVALUATING,
                   "Bad module status after failed evaluation");
            RecordModuleEvaluationError(m, error);
        }

        // Handle OOM when appending to the stack or over-recursion errors.
        if (stack.length === 0)
            RecordModuleEvaluationError(module, error);

        assert(module.status === MODULE_STATUS_EVALUATED_ERROR,
               "Bad module status after failed evaluation");

        throw error;
    }

    assert(module.status === MODULE_STATUS_EVALUATED,
           "Bad module status after successful evaluation");
    assert(stack.length === 0,
           "Stack should be empty after successful evaluation");

    return undefined;
}
_SetCanonicalName(ModuleEvaluate, "ModuleEvaluate");

// 15.2.1.16.5.1 InnerModuleEvaluation(module, stack, index)
function InnerModuleEvaluation(module, stack, index)
{
    // Step 1
    // TODO: Support module records other than source text module records.

    // Step 2
    if (module.status === MODULE_STATUS_EVALUATED_ERROR)
        throw GetModuleEvaluationError(module);

    if (module.status === MODULE_STATUS_EVALUATED)
        return index;

    // Step 3
    if (module.status === MODULE_STATUS_EVALUATING)
        return index;

    // Step 4
    assert(module.status === MODULE_STATUS_INSTANTIATED,
          "Bad module status in InnerModuleEvaluation");

    // Step 5
    ModuleSetStatus(module, MODULE_STATUS_EVALUATING);

    // Steps 6-8
    UnsafeSetReservedSlot(module, MODULE_OBJECT_DFS_INDEX_SLOT, index);
    UnsafeSetReservedSlot(module, MODULE_OBJECT_DFS_ANCESTOR_INDEX_SLOT, index);
    index++;

    // Step 9
    _DefineDataProperty(stack, stack.length, module);

    // Step 10
    let requestedModules = module.requestedModules;
    for (let i = 0; i < requestedModules.length; i++) {
        let required = requestedModules[i].moduleSpecifier;
        let requiredModule =
            CallModuleResolveHook(module, required, MODULE_STATUS_INSTANTIATED);

        index = InnerModuleEvaluation(requiredModule, stack, index);

        assert(requiredModule.status === MODULE_STATUS_EVALUATING ||
               requiredModule.status === MODULE_STATUS_EVALUATED,
              "Bad module status after InnerModuleEvaluation");

        assert((requiredModule.status === MODULE_STATUS_EVALUATING) ===
               ArrayContains(stack, requiredModule),
               "Required module should be in the stack iff it is currently being evaluated");

        assert(typeof requiredModule.dfsIndex === "number", "Bad dfsIndex");
        assert(typeof requiredModule.dfsAncestorIndex === "number", "Bad dfsAncestorIndex");

        if (requiredModule.status === MODULE_STATUS_EVALUATING) {
            UnsafeSetReservedSlot(module, MODULE_OBJECT_DFS_ANCESTOR_INDEX_SLOT,
                                  std_Math_min(module.dfsAncestorIndex,
                                               requiredModule.dfsAncestorIndex));
        }
    }

    // Step 11
    ExecuteModule(module);

    // Step 12
    assert(CountArrayValues(stack, module) === 1,
           "Current module should appear exactly once in the stack");

    // Step 13
    assert(module.dfsAncestorIndex <= module.dfsIndex,
           "Bad DFS ancestor index");

    // Step 14
    if (module.dfsAncestorIndex === module.dfsIndex) {
        let requiredModule;
        do {
            requiredModule = callFunction(std_Array_pop, stack);
            ModuleSetStatus(requiredModule, MODULE_STATUS_EVALUATED);
        } while (requiredModule !== module);
    }

    // Step 15
    return index;
}
