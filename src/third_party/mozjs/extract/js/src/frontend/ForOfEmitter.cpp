/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ForOfEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/EmitterScope.h"
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex
#include "frontend/UsingEmitter.h"
#include "vm/Opcodes.h"
#include "vm/StencilEnums.h"  // TryNoteKind

using namespace js;
using namespace js::frontend;

using mozilla::Nothing;

ForOfEmitter::ForOfEmitter(BytecodeEmitter* bce,
                           const EmitterScope* headLexicalEmitterScope,
                           SelfHostedIter selfHostedIter, IteratorKind iterKind
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
                           ,
                           HeadUsingDeclarationKind usingDeclarationInHead
#endif
                           )
    : bce_(bce),
      selfHostedIter_(selfHostedIter),
      iterKind_(iterKind),
      headLexicalEmitterScope_(headLexicalEmitterScope)
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
      ,
      usingDeclarationInHead_(usingDeclarationInHead)
#endif
{
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  // The using bindings are closed over and stored in the lexical environment
  // object for headLexicalEmitterScope.
  // Mark that the environment has disposables for them to be disposed on
  // every iteration.
  MOZ_ASSERT_IF(usingDeclarationInHead != HeadUsingDeclarationKind::None,
                headLexicalEmitterScope->hasEnvironment() &&
                    headLexicalEmitterScope == bce_->innermostEmitterScope() &&
                    headLexicalEmitterScope->hasDisposables());
  MOZ_ASSERT_IF(
      headLexicalEmitterScope && headLexicalEmitterScope->hasDisposables(),
      usingDeclarationInHead != HeadUsingDeclarationKind::None);
#endif
}

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

bool ForOfEmitter::emitInitialize(uint32_t forPos) {
  MOZ_ASSERT(state_ == State::Iterated);

  tdzCacheForIteratedValue_.reset();

  //                [stack] # if AllowContentWithNext
  //                [stack] NEXT ITER
  //                [stack] # elif AllowContentWith
  //                [stack] ITERABLE ITERFN SYNC_ITERFN?
  //                [stack] # else
  //                [stack] ITERABLE

  if (iterKind_ == IteratorKind::Async) {
    if (!bce_->emitAsyncIterator(selfHostedIter_)) {
      //            [stack] NEXT ITER
      return false;
    }
  } else {
    if (!bce_->emitIterator(selfHostedIter_)) {
      //            [stack] NEXT ITER
      return false;
    }
  }

  // For-of loops have the iterator next method and the iterator itself on the
  // stack.

  int32_t iterDepth = bce_->bytecodeSection().stackDepth();
  loopInfo_.emplace(bce_, iterDepth, selfHostedIter_, iterKind_);

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
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
      // Before recreation of the lexical environment, we must dispose
      // the disposables of the previous iteration.
      //
      // Emitting the bytecode to dispose over here means
      // that we will have one extra disposal at the start of the loop which
      // is a no op because there arent any disposables added yet.
      //
      // There also wouldn't be a dispose operation for the environment
      // object recreated for the last iteration, where it leaves the loop
      // before evaluating the body statement.
      if (!loopInfo_->prepareForForOfLoopIteration(
              bce_, headLexicalEmitterScope_,
              usingDeclarationInHead_ == HeadUsingDeclarationKind::Async)) {
        return false;
      }
#endif
      if (!bce_->emitInternedScopeOp(headLexicalEmitterScope_->index(),
                                     JSOp::RecreateLexicalEnv)) {
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
  if (!bce_->updateSourceCoordNotes(forPos)) {
    return false;
  }

  if (!bce_->emit1(JSOp::Dup2)) {
    //              [stack] NEXT ITER NEXT ITER
    return false;
  }

  if (!bce_->emitIteratorNext(mozilla::Some(forPos), iterKind_,
                              selfHostedIter_)) {
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

bool ForOfEmitter::emitEnd(uint32_t iteratedPos) {
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
  if (!bce_->updateSourceCoordNotes(iteratedPos)) {
    return false;
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
