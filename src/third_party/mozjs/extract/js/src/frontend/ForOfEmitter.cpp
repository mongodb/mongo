/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ForOfEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/EmitterScope.h"
#include "frontend/IfEmitter.h"
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex
#include "frontend/SourceNotes.h"
#include "vm/Opcodes.h"
#include "vm/Scope.h"
#include "vm/StencilEnums.h"  // TryNoteKind

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;
using mozilla::Nothing;

ForOfEmitter::ForOfEmitter(BytecodeEmitter* bce,
                           const EmitterScope* headLexicalEmitterScope,
                           bool allowSelfHostedIter, IteratorKind iterKind)
    : bce_(bce),
      allowSelfHostedIter_(allowSelfHostedIter),
      iterKind_(iterKind),
      headLexicalEmitterScope_(headLexicalEmitterScope) {}

bool ForOfEmitter::emitIterated() {
  MOZ_ASSERT(state_ == State::Start);

  // Evaluate the expression being iterated. The forHeadExpr should use a
  // distinct TDZCheckCache to evaluate since (abstractly) it runs in its
  // own LexicalEnvironment.
  tdzCacheForIteratedValue_.emplace(bce_);

#ifdef DEBUG
  state_ = State::Iterated;
#endif
  return true;
}

bool ForOfEmitter::emitInitialize(const Maybe<uint32_t>& forPos) {
  MOZ_ASSERT(state_ == State::Iterated);

  tdzCacheForIteratedValue_.reset();

  if (iterKind_ == IteratorKind::Async) {
    if (!bce_->emitAsyncIterator()) {
      //            [stack] NEXT ITER
      return false;
    }
  } else {
    if (!bce_->emitIterator()) {
      //            [stack] NEXT ITER
      return false;
    }
  }

  // For-of loops have the iterator next method and the iterator itself on the
  // stack.

  int32_t iterDepth = bce_->bytecodeSection().stackDepth();
  loopInfo_.emplace(bce_, iterDepth, allowSelfHostedIter_, iterKind_);

  if (!loopInfo_->emitLoopHead(bce_, Nothing())) {
    //              [stack] NEXT ITER
    return false;
  }

  // If the loop had an escaping lexical declaration, replace the current
  // environment with an dead zoned one to implement TDZ semantics.
  if (headLexicalEmitterScope_) {
    // The environment chain only includes an environment for the for-of
    // loop head *if* a scope binding is captured, thereby requiring
    // recreation each iteration. If a lexical scope exists for the head,
    // it must be the innermost one. If that scope has closed-over
    // bindings inducing an environment, recreate the current environment.
    MOZ_ASSERT(headLexicalEmitterScope_ == bce_->innermostEmitterScope());
    MOZ_ASSERT(headLexicalEmitterScope_->scope(bce_).kind() ==
               ScopeKind::Lexical);

    if (headLexicalEmitterScope_->hasEnvironment()) {
      if (!bce_->emit1(JSOp::RecreateLexicalEnv)) {
        //          [stack] NEXT ITER
        return false;
      }
    }

    // For uncaptured bindings, put them back in TDZ.
    if (!headLexicalEmitterScope_->deadZoneFrameSlots(bce_)) {
      return false;
    }
  }

#ifdef DEBUG
  loopDepth_ = bce_->bytecodeSection().stackDepth();
#endif

  // Make sure this code is attributed to the "for".
  if (forPos) {
    if (!bce_->updateSourceCoordNotes(*forPos)) {
      return false;
    }
  }

  if (!bce_->emit1(JSOp::Dup2)) {
    //              [stack] NEXT ITER NEXT ITER
    return false;
  }

  if (!bce_->emitIteratorNext(forPos, iterKind_, allowSelfHostedIter_)) {
    //              [stack] NEXT ITER RESULT
    return false;
  }

  if (!bce_->emit1(JSOp::Dup)) {
    //              [stack] NEXT ITER RESULT RESULT
    return false;
  }
  if (!bce_->emitAtomOp(JSOp::GetProp,
                        TaggedParserAtomIndex::WellKnown::done())) {
    //              [stack] NEXT ITER RESULT DONE
    return false;
  }

  // if (done) break;
  MOZ_ASSERT(bce_->innermostNestableControl == loopInfo_.ptr(),
             "must be at the top-level of the loop");
  if (!bce_->emitJump(JSOp::JumpIfTrue, &loopInfo_->breaks)) {
    //              [stack] NEXT ITER RESULT
    return false;
  }

  // Emit code to assign result.value to the iteration variable.
  //
  // Note that ES 13.7.5.13, step 5.c says getting result.value does not
  // call IteratorClose, so start TryNoteKind::ForOfIterClose after the GetProp.
  if (!bce_->emitAtomOp(JSOp::GetProp,
                        TaggedParserAtomIndex::WellKnown::value())) {
    //              [stack] NEXT ITER VALUE
    return false;
  }

  if (!loopInfo_->emitBeginCodeNeedingIteratorClose(bce_)) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Initialize;
#endif
  return true;
}

bool ForOfEmitter::emitBody() {
  MOZ_ASSERT(state_ == State::Initialize);

  MOZ_ASSERT(bce_->bytecodeSection().stackDepth() == loopDepth_ + 1,
             "the stack must be balanced around the initializing "
             "operation");

#ifdef DEBUG
  state_ = State::Body;
#endif
  return true;
}

bool ForOfEmitter::emitEnd(const Maybe<uint32_t>& iteratedPos) {
  MOZ_ASSERT(state_ == State::Body);

  MOZ_ASSERT(bce_->bytecodeSection().stackDepth() == loopDepth_ + 1,
             "the stack must be balanced around the for-of body");

  if (!loopInfo_->emitEndCodeNeedingIteratorClose(bce_)) {
    //              [stack] NEXT ITER VALUE
    return false;
  }

  if (!loopInfo_->emitContinueTarget(bce_)) {
    //              [stack] NEXT ITER VALUE
    return false;
  }

  // We use the iterated value's position to attribute the backedge,
  // which corresponds to the iteration protocol.
  // This is a bit misleading for 2nd and later iterations and might need
  // some fix (bug 1482003).
  if (iteratedPos) {
    if (!bce_->updateSourceCoordNotes(*iteratedPos)) {
      return false;
    }
  }

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack] NEXT ITER
    return false;
  }

  if (!loopInfo_->emitLoopEnd(bce_, JSOp::Goto, TryNoteKind::ForOf)) {
    //              [stack] NEXT ITER
    return false;
  }

  // All jumps/breaks to this point still have an extra value on the stack.
  MOZ_ASSERT(bce_->bytecodeSection().stackDepth() == loopDepth_);
  bce_->bytecodeSection().setStackDepth(bce_->bytecodeSection().stackDepth() +
                                        1);

  if (!bce_->emitPopN(3)) {
    //              [stack]
    return false;
  }

  loopInfo_.reset();

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
