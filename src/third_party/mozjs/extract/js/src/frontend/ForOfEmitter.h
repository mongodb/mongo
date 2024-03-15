/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ForOfEmitter_h
#define frontend_ForOfEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include <stdint.h>  // int32_t

#include "frontend/ForOfLoopControl.h"  // ForOfLoopControl
#include "frontend/IteratorKind.h"      // IteratorKind
#include "frontend/SelfHostedIter.h"    // SelfHostedIter
#include "frontend/TDZCheckCache.h"     // TDZCheckCache

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
//     forOf.emitInitialize(offset_of_for);
//     emit(init);
//     forOf.emitBody();
//     emit(body);
//     forOf.emitEnd(offset_of_iterated);
//
class MOZ_STACK_CLASS ForOfEmitter {
  BytecodeEmitter* bce_;

#ifdef DEBUG
  // The stack depth before emitting IteratorNext code inside loop.
  int32_t loopDepth_ = 0;
#endif

  SelfHostedIter selfHostedIter_;
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
               SelfHostedIter selfHostedIter, IteratorKind iterKind);

  // The offset in the source code for each character below:
  //
  //   for ( var x of obj ) { ... }
  //   ^              ^
  //   |              |
  //   |              iteratedPos
  //   |
  //   forPos
  [[nodiscard]] bool emitIterated();
  [[nodiscard]] bool emitInitialize(uint32_t forPos,
                                    bool isIteratorMethodOnStack);
  [[nodiscard]] bool emitBody();
  [[nodiscard]] bool emitEnd(uint32_t iteratedPos);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ForOfEmitter_h */
