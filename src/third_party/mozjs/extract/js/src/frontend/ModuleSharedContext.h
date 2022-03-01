/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ModuleSharedContext_h
#define frontend_ModuleSharedContext_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS

#include "builtin/ModuleObject.h"    // js::ModuleObject
#include "frontend/SharedContext.h"  // js::frontend::SharedContext
#include "js/CompileOptions.h"       // JS::ReadOnlyCompileOptions
#include "js/RootingAPI.h"           // JS::Handle, JS::Rooted
#include "vm/Scope.h"                // js::{Module,}Scope
#include "vm/StencilEnums.h"         // ImmutableScriptFlagsEnum

namespace js {

class ModuleBuilder;

namespace frontend {

struct CompilationStencil;

class MOZ_STACK_CLASS ModuleSharedContext : public SuspendableContext {
 public:
  ModuleScope::ParserData* bindings;
  ModuleBuilder& builder;

  ModuleSharedContext(JSContext* cx, const JS::ReadOnlyCompileOptions& options,
                      ModuleBuilder& builder, SourceExtent extent);
};

inline ModuleSharedContext* SharedContext::asModuleContext() {
  MOZ_ASSERT(isModuleContext());
  return static_cast<ModuleSharedContext*>(this);
}

}  // namespace frontend
}  // namespace js

#endif /* frontend_ModuleSharedContext_h */
