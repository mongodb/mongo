/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_DoWhileEmitter_h
#define frontend_DoWhileEmitter_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stdint.h>

#include "frontend/BytecodeControlStructures.h"

namespace js {
namespace frontend {

struct BytecodeEmitter;

// Class for emitting bytecode for do-while loop.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `do body while (cond);`
//     DoWhileEmitter doWhile(this);
//     doWhile.emitBody(offset_of_do, offset_of_body);
//     emit(body);
//     doWhile.emitCond();
//     emit(cond);
//     doWhile.emitEnd();
//
class MOZ_STACK_CLASS DoWhileEmitter {
  BytecodeEmitter* bce_;

  mozilla::Maybe<LoopControl> loopInfo_;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+ emitBody +------+ emitCond +------+ emitEnd  +-----+
  // | Start |--------->| Body |--------->| Cond |--------->| End |
  // +-------+          +------+          +------+          +-----+
  enum class State {
    // The initial state.
    Start,

    // After calling emitBody.
    Body,

    // After calling emitCond.
    Cond,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif

 public:
  explicit DoWhileEmitter(BytecodeEmitter* bce);

  // Parameters are the offset in the source code for each character below:
  //
  //   do { ... } while ( x < 20 );
  //   ^  ^
  //   |  |
  //   |  bodyPos
  //   |
  //   doPos
  [[nodiscard]] bool emitBody(uint32_t doPos, uint32_t bodyPos);
  [[nodiscard]] bool emitCond();
  [[nodiscard]] bool emitEnd();
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_DoWhileEmitter_h */
