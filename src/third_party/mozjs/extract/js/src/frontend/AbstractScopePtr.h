/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_AbstractScopePtr_h
#define frontend_AbstractScopePtr_h

#include <type_traits>

#include "frontend/ScopeIndex.h"
#include "vm/ScopeKind.h"  // For ScopeKind

namespace js {
class Scope;
class GlobalScope;
class EvalScope;

namespace frontend {
struct CompilationState;
class ScopeStencil;
}  // namespace frontend

// An interface class to support Scope queries in the frontend without requiring
// a GC Allocated scope to necessarily exist.
//
// This abstracts Scope* and a ScopeStencil type used within the frontend before
// the Scope is allocated.
//
// Queries to GC Scope should be pre-calculated and stored into ScopeContext.
class AbstractScopePtr {
 private:
  // ScopeIndex::invalid() if this points CompilationInput.enclosingScope.
  ScopeIndex index_;

  frontend::CompilationState& compilationState_;

 public:
  friend class js::Scope;

  AbstractScopePtr(frontend::CompilationState& compilationState,
                   ScopeIndex index)
      : index_(index), compilationState_(compilationState) {}

  static AbstractScopePtr compilationEnclosingScope(
      frontend::CompilationState& compilationState) {
    return AbstractScopePtr(compilationState, ScopeIndex::invalid());
  }

 private:
  bool isScopeStencil() const { return index_.isValid(); }

  frontend::ScopeStencil& scopeData() const;

 public:
  // This allows us to check whether or not this provider wraps
  // or otherwise would reify to a particular scope type.
  template <typename T>
  bool is() const {
    static_assert(std::is_base_of_v<Scope, T>,
                  "Trying to ask about non-Scope type");
    return kind() == T::classScopeKind_;
  }

  ScopeKind kind() const;
  AbstractScopePtr enclosing() const;
  bool hasEnvironment() const;
  // Valid iff is<FunctionScope>
  bool isArrow() const;

#ifdef DEBUG
  bool hasNonSyntacticScopeOnChain() const;
#endif
};

// Specializations of AbstractScopePtr::is
template <>
inline bool AbstractScopePtr::is<GlobalScope>() const {
  return kind() == ScopeKind::Global || kind() == ScopeKind::NonSyntactic;
}

template <>
inline bool AbstractScopePtr::is<EvalScope>() const {
  return kind() == ScopeKind::Eval || kind() == ScopeKind::StrictEval;
}

}  // namespace js

#endif  // frontend_AbstractScopePtr_h
