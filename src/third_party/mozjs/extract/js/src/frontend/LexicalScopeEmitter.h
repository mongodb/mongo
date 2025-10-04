/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_LexicalScopeEmitter_h
#define frontend_LexicalScopeEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // Maybe

#include "frontend/EmitterScope.h"   // EmitterScope
#include "frontend/TDZCheckCache.h"  // TDZCheckCache
#include "vm/Scope.h"                // ScopeKind, LexicalScope

namespace js {
namespace frontend {

struct BytecodeEmitter;

// Class for emitting bytecode for lexical scope.
//
// In addition to emitting code for entering and leaving a scope, this RAII
// guard affects the code emitted for `break` and other non-structured
// control flow. See NonLocalExitControl::prepareForNonLocalJump().
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `{ ... }` -- lexical scope with no bindings
//     LexicalScopeEmitter lse(this);
//     lse.emitEmptyScope();
//     emit(scopeBody);
//     lse.emitEnd();
//
//   `{ let a; body }`
//     LexicalScopeEmitter lse(this);
//     lse.emitScope(ScopeKind::Lexical, scopeBinding);
//     emit(let_and_body);
//     lse.emitEnd();
//
//   `catch (e) { body }`
//     LexicalScopeEmitter lse(this);
//     lse.emitScope(ScopeKind::SimpleCatch, scopeBinding);
//     emit(body);
//     lse.emitEnd();
//
//   `catch ([a, b]) { body }`
//     LexicalScopeEmitter lse(this);
//     lse.emitScope(ScopeKind::Catch, scopeBinding);
//     emit(body);
//     lse.emitEnd();
class MOZ_STACK_CLASS LexicalScopeEmitter {
  BytecodeEmitter* bce_;

  mozilla::Maybe<TDZCheckCache> tdzCache_;
  mozilla::Maybe<EmitterScope> emitterScope_;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+ emitScope  +-------+ emitEnd  +-----+
  // | Start |----------->| Scope |--------->| End |
  // +-------+            +-------+          +-----+
  enum class State {
    // The initial state.
    Start,

    // After calling emitScope/emitEmptyScope.
    Scope,

    // After calling emitEnd.
    End,
  };
  State state_ = State::Start;
#endif

 public:
  explicit LexicalScopeEmitter(BytecodeEmitter* bce);

  // Returns the scope object for non-empty scope.
  const EmitterScope& emitterScope() const { return *emitterScope_; }

  [[nodiscard]] bool emitScope(ScopeKind kind,
                               LexicalScope::ParserData* bindings);
  [[nodiscard]] bool emitEmptyScope();

  [[nodiscard]] bool emitEnd();
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_LexicalScopeEmitter_h */
