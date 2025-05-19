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

// A struct with detailed error information when import/export failed.
struct ModuleErrorInfo {
  ModuleErrorInfo(uint32_t lineNumber_, JS::ColumnNumberOneOrigin columnNumber_)
      : lineNumber(lineNumber_), columnNumber(columnNumber_) {}

  void setImportedModule(JSContext* cx, ModuleObject* importedModule);
  void setCircularImport(JSContext* cx, ModuleObject* importedModule);
  void setForAmbiguousImport(JSContext* cx, ModuleObject* importedModule,
                             ModuleObject* module1, ModuleObject* module2);

  uint32_t lineNumber;
  JS::ColumnNumberOneOrigin columnNumber;

  // The filename of the imported module.
  const char* imported;

  // The filenames of the ambiguous entries.
  const char* entry1;
  const char* entry2;

  // A bool to indicate the error is a circular import when it's true.
  bool isCircular = false;
};

ModuleNamespaceObject* GetOrCreateModuleNamespace(JSContext* cx,
                                                  Handle<ModuleObject*> module);

void AsyncModuleExecutionFulfilled(JSContext* cx, Handle<ModuleObject*> module);

void AsyncModuleExecutionRejected(JSContext* cx, Handle<ModuleObject*> module,
                                  HandleValue error);

}  // namespace js

#endif  // vm_Modules_h
