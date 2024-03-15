/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ForInEmitter_h
#define frontend_ForInEmitter_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stdint.h>

#include "frontend/BytecodeControlStructures.h"
#include "frontend/TDZCheckCache.h"

namespace js {
namespace frontend {

struct BytecodeEmitter;
class EmitterScope;

// Class for emitting bytecode for for-in loop.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `for (init in iterated) body`
//     // headLexicalEmitterScope: lexical scope for init
//     ForInEmitter forIn(this, headLexicalEmitterScope);
//     forIn.emitIterated();
//     emit(iterated);
//     forIn.emitInitialize();
//     emit(init);
//     forIn.emitBody();
//     emit(body);
//     forIn.emitEnd(offset_of_for);
//
class MOZ_STACK_CLASS ForInEmitter {
  BytecodeEmitter* bce_;

#ifdef DEBUG
  // The stack depth before emitting initialize code inside loop.
  int32_t loopDepth_ = 0;
#endif

  mozilla::Maybe<LoopControl> loopInfo_;

  // The lexical scope to be freshened for each iteration.  See the comment
  // in `emitBody` for more details.  Can be nullptr if there's no lexical
  // scope.
  const EmitterScope* headLexicalEmitterScope_;

  // Cache for the iterated value.
  // (The cache for the iteration body is inside `loopInfo_`)
  //
  // The iterated value needs its own TDZCheckCache, separated from both the
  // enclosing block and the iteration body, in order to make the sanity check
  // in Ion work properly.
  // In term of the execution order, the TDZCheckCache for the iterated value
  // dominates the one for the iteration body, that means the checks in the
  // iteration body is dead, and we can optimize them away.  But the sanity
  // check in Ion doesn't know it's dead.
  // (see bug 1368360 for more context)
  mozilla::Maybe<TDZCheckCache> tdzCacheForIteratedValue_;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+ emitIterated +----------+ emitInitialize +------------+
  // | Start |------------->| Iterated |--------------->| Initialize |-+
  // +-------+              +----------+                +------------+ |
  //                                                                   |
  //                                +----------------------------------+
  //                                |
  //                                | emitBody +------+ emitEnd  +-----+
  //                                +----------| Body |--------->| End |
  //                                           +------+          +-----+
  enum class State {
    // The initial state.
    Start,

    // After calling emitIterated.
    Iterated,

    // After calling emitInitialize.
    Initialize,

    // After calling emitBody.
    Body,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif

 public:
  ForInEmitter(BytecodeEmitter* bce,
               const EmitterScope* headLexicalEmitterScope);

  // Parameters are the offset in the source code for each character below:
  //
  //   for ( var x in obj ) { ... }
  //   ^
  //   |
  //   forPos
  [[nodiscard]] bool emitIterated();
  [[nodiscard]] bool emitInitialize();
  [[nodiscard]] bool emitBody();
  [[nodiscard]] bool emitEnd(uint32_t forPos);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ForInEmitter_h */
