/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/LexicalScopeEmitter.h"

using namespace js;
using namespace js::frontend;

LexicalScopeEmitter::LexicalScopeEmitter(BytecodeEmitter* bce) : bce_(bce) {}

bool LexicalScopeEmitter::emitScope(ScopeKind kind,
                                    LexicalScope::ParserData* bindings) {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(bindings);

  tdzCache_.emplace(bce_);
  emitterScope_.emplace(bce_);
  if (!emitterScope_->enterLexical(bce_, kind, bindings)) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Scope;
#endif
  return true;
}

bool LexicalScopeEmitter::emitEmptyScope() {
  MOZ_ASSERT(state_ == State::Start);

  tdzCache_.emplace(bce_);

#ifdef DEBUG
  state_ = State::Scope;
#endif
  return true;
}

bool LexicalScopeEmitter::emitEnd() {
  MOZ_ASSERT(state_ == State::Scope);

  if (emitterScope_) {
    if (!emitterScope_->leave(bce_)) {
      return false;
    }
    emitterScope_.reset();
  }
  tdzCache_.reset();

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
