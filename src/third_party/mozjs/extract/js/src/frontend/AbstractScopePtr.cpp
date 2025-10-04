/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/AbstractScopePtr.h"

#include "mozilla/Assertions.h"

#include "frontend/CompilationStencil.h"  // CompilationState
#include "frontend/Stencil.h"
#include "js/Vector.h"
#include "vm/Scope.h"  // for FunctionScope

using namespace js;
using namespace js::frontend;

ScopeStencil& AbstractScopePtr::scopeData() const {
  MOZ_ASSERT(isScopeStencil());
  return compilationState_.scopeData[index_];
}

ScopeKind AbstractScopePtr::kind() const {
  if (isScopeStencil()) {
    return scopeData().kind();
  }
  return compilationState_.scopeContext.enclosingScopeKind;
}

AbstractScopePtr AbstractScopePtr::enclosing() const {
  MOZ_ASSERT(isScopeStencil());
  return scopeData().enclosing(compilationState_);
}

bool AbstractScopePtr::hasEnvironment() const {
  if (isScopeStencil()) {
    return scopeData().hasEnvironment();
  }
  return compilationState_.scopeContext.enclosingScopeHasEnvironment;
}

bool AbstractScopePtr::isArrow() const {
  MOZ_ASSERT(is<FunctionScope>());
  if (isScopeStencil()) {
    return scopeData().isArrow();
  }
  return compilationState_.scopeContext.enclosingScopeIsArrow;
}

#ifdef DEBUG
bool AbstractScopePtr::hasNonSyntacticScopeOnChain() const {
  if (isScopeStencil()) {
    if (kind() == ScopeKind::NonSyntactic) {
      return true;
    }
    return enclosing().hasNonSyntacticScopeOnChain();
  }
  return compilationState_.scopeContext.hasNonSyntacticScopeOnChain;
}
#endif
