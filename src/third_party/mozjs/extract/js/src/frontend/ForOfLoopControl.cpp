/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ForOfLoopControl.h"

#include "frontend/BytecodeEmitter.h"  // BytecodeEmitter
#include "frontend/EmitterScope.h"     // EmitterScope
#include "frontend/IfEmitter.h"        // InternalIfEmitter
#include "vm/CompletionKind.h"         // CompletionKind
#include "vm/Opcodes.h"                // JSOp

using namespace js;
using namespace js::frontend;

ForOfLoopControl::ForOfLoopControl(BytecodeEmitter* bce, int32_t iterDepth,
                                   SelfHostedIter selfHostedIter,
                                   IteratorKind iterKind)
    : LoopControl(bce, StatementKind::ForOfLoop),
      iterDepth_(iterDepth),
      numYieldsAtBeginCodeNeedingIterClose_(UINT32_MAX),
      selfHostedIter_(selfHostedIter),
      iterKind_(iterKind) {}

bool ForOfLoopControl::emitBeginCodeNeedingIteratorClose(BytecodeEmitter* bce) {
  tryCatch_.emplace(bce, TryEmitter::Kind::TryCatch,
                    TryEmitter::ControlKind::NonSyntactic);

  if (!tryCatch_->emitTry()) {
    return false;
  }

  MOZ_ASSERT(numYieldsAtBeginCodeNeedingIterClose_ == UINT32_MAX);
  numYieldsAtBeginCodeNeedingIterClose_ = bce->bytecodeSection().numYields();

  return true;
}

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
bool ForOfLoopControl::prepareForForOfLoopIteration(
    BytecodeEmitter* bce, const EmitterScope* headLexicalEmitterScope,
    bool hasAwaitUsing) {
  MOZ_ASSERT(headLexicalEmitterScope);
  if (headLexicalEmitterScope->hasDisposables()) {
    forOfDisposalEmitter_.emplace(bce, hasAwaitUsing);
    return forOfDisposalEmitter_->prepareForForOfLoopIteration();
  }
  return true;
}
#endif

bool ForOfLoopControl::emitEndCodeNeedingIteratorClose(BytecodeEmitter* bce) {
  if (!tryCatch_->emitCatch(TryEmitter::ExceptionStack::Yes)) {
    //              [stack] ITER ... EXCEPTION STACK
    return false;
  }

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  // Explicit Resource Management Proposal
  // https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-runtime-semantics-forin-div-ofbodyevaluation-lhs-stmt-iterator-lhskind-labelset
  // Step 9.i.i.1 Set result to
  // Completion(DisposeResources(iterationEnv.[[DisposeCapability]], result)).
  if (forOfDisposalEmitter_.isSome()) {
    if (!forOfDisposalEmitter_->emitEnd()) {
      //              [stack] ITER ... EXCEPTION STACK
      return false;
    }
  }
#endif

  unsigned slotFromTop = bce->bytecodeSection().stackDepth() - iterDepth_;
  if (!bce->emitDupAt(slotFromTop)) {
    //              [stack] ITER ... EXCEPTION STACK ITER
    return false;
  }

  if (!emitIteratorCloseInInnermostScopeWithTryNote(bce,
                                                    CompletionKind::Throw)) {
    return false;  // ITER ... EXCEPTION STACK
  }

  if (!bce->emit1(JSOp::ThrowWithStack)) {
    //              [stack] ITER ...
    return false;
  }

  // If any yields were emitted, then this for-of loop is inside a star
  // generator and must handle the case of Generator.return. Like in
  // yield*, it is handled with a finally block. If the generator is
  // closing, then the exception/resumeindex value (third value on
  // the stack) will be a magic JS_GENERATOR_CLOSING value.
  // TODO: Refactor this to eliminate the swaps.
  uint32_t numYieldsEmitted = bce->bytecodeSection().numYields();
  if (numYieldsEmitted > numYieldsAtBeginCodeNeedingIterClose_) {
    if (!tryCatch_->emitFinally()) {
      return false;
    }
    //              [stack] ITER ... FVALUE FSTACK FTHROWING
    InternalIfEmitter ifGeneratorClosing(bce);
    if (!bce->emitPickN(2)) {
      //            [stack] ITER ... FSTACK FTHROWING FVALUE
      return false;
    }
    if (!bce->emit1(JSOp::IsGenClosing)) {
      //            [stack] ITER ... FSTACK FTHROWING FVALUE CLOSING
      return false;
    }
    if (!ifGeneratorClosing.emitThen()) {
      //            [stack] ITER ... FSTACK FTHROWING FVALUE
      return false;
    }
    if (!bce->emitDupAt(slotFromTop + 1)) {
      //            [stack] ITER ... FSTACK FTHROWING FVALUE ITER
      return false;
    }
    if (!emitIteratorCloseInInnermostScopeWithTryNote(bce,
                                                      CompletionKind::Normal)) {
      //            [stack] ITER ... FSTACK FTHROWING FVALUE
      return false;
    }
    if (!ifGeneratorClosing.emitEnd()) {
      //            [stack] ITER ... FSTACK FTHROWING FVALUE
      return false;
    }
    if (!bce->emitUnpickN(2)) {
      //            [stack] ITER ... FVALUE FSTACK FTHROWING
      return false;
    }
  }

  if (!tryCatch_->emitEnd()) {
    return false;
  }

  tryCatch_.reset();
  numYieldsAtBeginCodeNeedingIterClose_ = UINT32_MAX;

  return true;
}

bool ForOfLoopControl::emitIteratorCloseInInnermostScopeWithTryNote(
    BytecodeEmitter* bce,
    CompletionKind completionKind /* = CompletionKind::Normal */) {
  BytecodeOffset start = bce->bytecodeSection().offset();
  if (!emitIteratorCloseInScope(bce, *bce->innermostEmitterScope(),
                                completionKind)) {
    return false;
  }
  BytecodeOffset end = bce->bytecodeSection().offset();
  return bce->addTryNote(TryNoteKind::ForOfIterClose, 0, start, end);
}

bool ForOfLoopControl::emitIteratorCloseInScope(
    BytecodeEmitter* bce, EmitterScope& currentScope,
    CompletionKind completionKind /* = CompletionKind::Normal */) {
  return bce->emitIteratorCloseInScope(currentScope, iterKind_, completionKind,
                                       selfHostedIter_);
}

// Since we're in the middle of emitting code that will leave
// |bce->innermostEmitterScope()|, passing the innermost emitter scope to
// emitIteratorCloseInScope and looking up .generator there would be very,
// very wrong.  We'd find .generator in the function environment, and we'd
// compute a NameLocation with the correct slot, but we'd compute a
// hop-count to the function environment that was too big.  At runtime we'd
// either crash, or we'd find a user-controllable value in that slot, and
// Very Bad Things would ensue as we reinterpreted that value as an
// iterator.
bool ForOfLoopControl::emitPrepareForNonLocalJumpFromScope(
    BytecodeEmitter* bce, EmitterScope& currentScope, bool isTarget,
    BytecodeOffset* tryNoteStart) {
  // Pop unnecessary value from the stack.  Effectively this means
  // leaving try-catch block.  However, the performing IteratorClose can
  // reach the depth for try-catch, and effectively re-enter the
  // try-catch block.
  if (!bce->emit1(JSOp::Pop)) {
    //              [stack] NEXT ITER
    return false;
  }

  // Pop the iterator's next method.
  if (!bce->emit1(JSOp::Swap)) {
    //              [stack] ITER NEXT
    return false;
  }
  if (!bce->emit1(JSOp::Pop)) {
    //              [stack] ITER
    return false;
  }

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  // Explicit Resource Management Proposal
  // https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-runtime-semantics-forin-div-ofbodyevaluation-lhs-stmt-iterator-lhskind-labelset
  // Step 9.k.i. Set result to
  // Completion(DisposeResources(iterationEnv.[[DisposeCapability]], result)).
  NonLocalIteratorCloseUsingEmitter disposeBeforeIterClose(bce);

  if (!disposeBeforeIterClose.prepareForIteratorClose(currentScope)) {
    //              [stack] EXC-DISPOSE? DISPOSE-THROWING? ITER
    return false;
  }
#endif

  if (!bce->emit1(JSOp::Dup)) {
    //              [stack] EXC-DISPOSE? DISPOSE-THROWING? ITER ITER
    return false;
  }

  *tryNoteStart = bce->bytecodeSection().offset();
  if (!emitIteratorCloseInScope(bce, currentScope, CompletionKind::Normal)) {
    //              [stack] EXC-DISPOSE? DISPOSE-THROWING? ITER
    return false;
  }

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  if (!disposeBeforeIterClose.emitEnd()) {
    //              [stack] ITER
    return false;
  }
#endif

  if (isTarget) {
    // At the level of the target block, there's bytecode after the
    // loop that will pop the next method, the iterator, and the
    // value, so push two undefineds to balance the stack.
    if (!bce->emit1(JSOp::Undefined)) {
      //            [stack] ITER UNDEF
      return false;
    }
    if (!bce->emit1(JSOp::Undefined)) {
      //            [stack] ITER UNDEF UNDEF
      return false;
    }
  } else {
    if (!bce->emit1(JSOp::Pop)) {
      //            [stack]
      return false;
    }
  }

  return true;
}
