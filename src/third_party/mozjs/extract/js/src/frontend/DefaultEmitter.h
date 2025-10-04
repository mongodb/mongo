/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_DefaultEmitter_h
#define frontend_DefaultEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // Maybe

#include "frontend/IfEmitter.h"  // IfEmitter

namespace js {
namespace frontend {

struct BytecodeEmitter;

// Class for emitting default parameter or default value.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `x = 10` in `function (x = 10) {}`
//     // the value of arguments[0] is on the stack
//     DefaultEmitter de(this);
//     de.prepareForDefault();
//     emit(10);
//     de.emitEnd();
//
class MOZ_STACK_CLASS DefaultEmitter {
  BytecodeEmitter* bce_;

  mozilla::Maybe<IfEmitter> ifUndefined_;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+ prepareForDefault +---------+ emitEnd +-----+
  // | Start |------------------>| Default |-------->| End |
  // +-------+                   +---------+         +-----+
  enum class State {
    // The initial state.
    Start,

    // After calling prepareForDefault.
    Default,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif

 public:
  explicit DefaultEmitter(BytecodeEmitter* bce);

  [[nodiscard]] bool prepareForDefault();
  [[nodiscard]] bool emitEnd();
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_LabelEmitter_h */
