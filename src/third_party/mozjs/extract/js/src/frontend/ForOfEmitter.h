/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ForOfEmitter_h
#define frontend_ForOfEmitter_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stdint.h>

#include "frontend/ForOfLoopControl.h"
#include "frontend/IteratorKind.h"
#include "frontend/JumpList.h"
#include "frontend/TDZCheckCache.h"

namespace js {
namespace frontend {

struct BytecodeEmitter;
class EmitterScope;

// Class for emitting bytecode for for-of loop.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `for (init of iterated) body`
//     // headLexicalEmitterScope: lexical scope for init
//     ForOfEmitter forOf(this, headLexicalEmitterScope);
//     forOf.emitIterated();
//     emit(iterated);
//     forOf.emitInitialize(Some(offset_of_for));
//     emit(init);
//     forOf.emitBody();
//     emit(body);
//     forOf.emitEnd(Some(offset_of_iterated));
//
class MOZ_STACK_CLASS ForOfEmitter {
  BytecodeEmitter* bce_;

#ifdef DEBUG
  // The stack depth before emitting IteratorNext code inside loop.
  int32_t loopDepth_ = 0;
#endif

  bool allowSelfHostedIter_;
  IteratorKind iterKind_;

  mozilla::Maybe<ForOfLoopControl> loopInfo_;

  // The lexical scope to be freshened for each iteration.
  // See the comment in `emitBody` for more details.
  const EmitterScope* headLexicalEmitterScope_;

  // Cache for the iterated value.
  // (The cache for the iteration body is inside `loopInfo_`)
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
  ForOfEmitter(BytecodeEmitter* bce,
               const EmitterScope* headLexicalEmitterScope,
               bool allowSelfHostedIter, IteratorKind iterKind);

  // The offset in the source code for each character below:
  //
  //   for ( var x of obj ) { ... }
  //   ^              ^
  //   |              |
  //   |              iteratedPos
  //   |
  //   forPos
  //
  // Can be Nothing() if not available.
  [[nodiscard]] bool emitIterated();
  [[nodiscard]] bool emitInitialize(const mozilla::Maybe<uint32_t>& forPos);
  [[nodiscard]] bool emitBody();
  [[nodiscard]] bool emitEnd(const mozilla::Maybe<uint32_t>& iteratedPos);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ForOfEmitter_h */
