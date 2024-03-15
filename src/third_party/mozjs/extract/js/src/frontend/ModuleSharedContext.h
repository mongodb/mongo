/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ModuleSharedContext_h
#define frontend_ModuleSharedContext_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS

#include "jstypes.h"

#include "frontend/SharedContext.h"  // js::frontend::SharedContext
#include "vm/Scope.h"                // js::ModuleScope

namespace JS {
class JS_PUBLIC_API ReadOnlyCompileOptions;
}

namespace js {

class ModuleBuilder;
struct SourceExtent;

namespace frontend {

class MOZ_STACK_CLASS ModuleSharedContext : public SuspendableContext {
 public:
  ModuleScope::ParserData* bindings;
  ModuleBuilder& builder;

  ModuleSharedContext(FrontendContext* fc,
                      const JS::ReadOnlyCompileOptions& options,
                      ModuleBuilder& builder, SourceExtent extent);
};

inline ModuleSharedContext* SharedContext::asModuleContext() {
  MOZ_ASSERT(isModuleContext());
  return static_cast<ModuleSharedContext*>(this);
}

}  // namespace frontend
}  // namespace js

#endif /* frontend_ModuleSharedContext_h */
