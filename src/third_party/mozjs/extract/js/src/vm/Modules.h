/* -*- Mode: javascript; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4
 * -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Modules_h
#define vm_Modules_h

#include "NamespaceImports.h"

#include "builtin/ModuleObject.h"
#include "js/AllocPolicy.h"
#include "js/GCVector.h"
#include "js/RootingAPI.h"

struct JSContext;

namespace js {

using ModuleVector = GCVector<ModuleObject*, 0, SystemAllocPolicy>;

bool ModuleResolveExport(JSContext* cx, Handle<ModuleObject*> module,
                         Handle<JSAtom*> exportName,
                         MutableHandle<Value> result);

ModuleNamespaceObject* GetOrCreateModuleNamespace(JSContext* cx,
                                                  Handle<ModuleObject*> module);

bool ModuleInitializeEnvironment(JSContext* cx, Handle<ModuleObject*> module);

bool ModuleLink(JSContext* cx, Handle<ModuleObject*> module);

// Start evaluating the module. If TLA is enabled, result will be a promise.
bool ModuleEvaluate(JSContext* cx, Handle<ModuleObject*> module,
                    MutableHandle<Value> result);

void AsyncModuleExecutionFulfilled(JSContext* cx, Handle<ModuleObject*> module);

void AsyncModuleExecutionRejected(JSContext* cx, Handle<ModuleObject*> module,
                                  HandleValue error);

}  // namespace js

#endif  // vm_Modules_h
