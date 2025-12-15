/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/UsingEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/EmitterScope.h"
#include "frontend/IfEmitter.h"
#include "frontend/TryEmitter.h"
#include "frontend/WhileEmitter.h"

using namespace js;
using namespace js::frontend;

UsingEmitter::UsingEmitter(BytecodeEmitter* bce) : bce_(bce) {}

bool UsingEmitter::emitTakeDisposeCapability() {
  if (!bce_->emit1(JSOp::TakeDisposeCapability)) {
    // [stack] RESOURCES-OR-UNDEF
    return false;
  }

  if (!bce_->emit1(JSOp::IsNullOrUndefined)) {
    // [stack] RESOURCES-OR-UNDEF IS-UNDEF
    return false;
  }

  InternalIfEmitter ifUndefined(bce_);

  if (!ifUndefined.emitThenElse()) {
    // [stack] UNDEFINED
    return false;
  }

  if (!bce_->emit1(JSOp::Zero)) {
    // [stack] UNDEFINED 0
    return false;
  }

  if (!ifUndefined.emitElse()) {
    // [stack] RESOURCES
    return false;
  }

  if (!bce_->emit1(JSOp::Dup)) {
    // [stack] RESOURCES RESOURCES
    return false;
  }

  if (!bce_->emitAtomOp(JSOp::GetProp,
                        TaggedParserAtomIndex::WellKnown::length())) {
    // [stack] RESOURCES COUNT
    return false;
  }

  if (!ifUndefined.emitEnd()) {
    // [stack] RESOURCES COUNT
    return false;
  }

  return true;
}

bool UsingEmitter::emitThrowIfException() {
  // [stack] EXC THROWING

  InternalIfEmitter ifThrow(bce_);

  if (!ifThrow.emitThenElse()) {
    // [stack] EXC
    return false;
  }

  if (!bce_->emit1(JSOp::Throw)) {
    // [stack]
    return false;
  }

  if (!ifThrow.emitElse()) {
    // [stack] EXC
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack]
    return false;
  }

  if (!ifThrow.emitEnd()) {
    // [stack]
    return false;
  }

  return true;
}

bool DisposalEmitter::emitResourcePropertyAccess(TaggedParserAtomIndex prop,
                                                 unsigned resourcesFromTop) {
  // [stack] # if resourcesFromTop == 1
  // [stack] RESOURCES INDEX
  // [stack] # if resourcesFromTop > 1
  // [stack] RESOURCES INDEX ... (resourcesFromTop - 1 values)
  MOZ_ASSERT(resourcesFromTop >= 1);

  if (!bce_->emitDupAt(resourcesFromTop, 2)) {
    // [stack] RESOURCES INDEX ... RESOURCES INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::GetElem)) {
    // [stack] RESOURCES INDEX ... RESOURCE
    return false;
  }

  if (!bce_->emitAtomOp(JSOp::GetProp, prop)) {
    // [stack] RESOURCES INDEX ... VALUE
    return false;
  }

  return true;
}

// Explicit Resource Management Proposal
// DisposeResources ( disposeCapability, completion )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-disposeresources
// Steps 1-2.
bool DisposalEmitter::prepareForDisposeCapability() {
  MOZ_ASSERT(state_ == State::Start);

  // [stack] THROWING EXC

  if (hasAsyncDisposables_) {
    // Step 1. Let needsAwait be false.
    if (!bce_->emit1(JSOp::False)) {
      // [stack] THROWING EXC NEEDS-AWAIT
      return false;
    }

    // Step 2. Let hasAwaited be false.
    if (!bce_->emit1(JSOp::False)) {
      // [stack] THROWING EXC NEEDS-AWAIT HAS-AWAITED
      return false;
    }

    if (!bce_->emitPickN(3)) {
      // [stack] EXC NEEDS-AWAIT HAS-AWAITED THROWING
      return false;
    }

    if (!bce_->emitPickN(3)) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC
      return false;
    }
  }

  // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC

#ifdef DEBUG
  state_ = State::DisposeCapability;
#endif
  return true;
}

// Explicit Resource Management Proposal
// DisposeResources ( disposeCapability, completion )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-disposeresources
// Steps 3-5, 7.
//
// This implementation of DisposeResources is designed for using and await using
// syntax as well as for disposals in DisposableStack and AsyncDisposableStack,
// it covers the complete algorithm as defined in the spec for both sync and
// async disposals as necessary in bytecode.
bool DisposalEmitter::emitEnd(EmitterScope& es) {
  MOZ_ASSERT(state_ == State::DisposeCapability);

  // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC RESOURCES COUNT

  // For the purpose of readbility some values are omitted from
  // the stack comments and are assumed to be present,
  // we mention the values in the comments as and when they are being
  // operated upon.

  // [stack] ... RESOURCES COUNT

  // We do the iteration in reverse order as per spec,
  // there can be the case when count is 0 and hence index
  // below becomes -1 but the loop condition will ensure
  // no code is executed in that case.
  if (!bce_->emit1(JSOp::Dec)) {
    // [stack] ... RESOURCES INDEX
    return false;
  }

  InternalWhileEmitter wh(bce_);

  // Step 3. For each element resource of
  // disposeCapability.[[DisposableResourceStack]], in reverse list order, do
  if (!wh.emitCond()) {
    // [stack] ... RESOURCES INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::Dup)) {
    // [stack] ... RESOURCES INDEX INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::Zero)) {
    // [stack] ... RESOURCES INDEX INDEX 0
    return false;
  }

  if (!bce_->emit1(JSOp::Ge)) {
    // [stack] ... RESOURCES INDEX BOOL
    return false;
  }

  if (!wh.emitBody()) {
    // [stack] ... RESOURCES INDEX
    return false;
  }

  // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC RESOURCES INDEX

  if (hasAsyncDisposables_) {
    // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX

    // Step 3.b. Let hint be resource.[[Hint]].
    // (reordered)
    // Step 3.d. If hint is sync-dispose and needsAwait is true and hasAwaited
    // is false, then
    if (!emitResourcePropertyAccess(TaggedParserAtomIndex::WellKnown::hint())) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX HINT
      return false;
    }

    // [stack] NEEDS-AWAIT HAS-AWAITED ... HINT

    static_assert(uint8_t(UsingHint::Sync) == 0, "Sync hint must be 0");
    static_assert(uint8_t(UsingHint::Async) == 1, "Async hint must be 1");
    if (!bce_->emit1(JSOp::Not)) {
      // [stack] NEEDS-AWAIT HAS-AWAITED ... IS-SYNC
      return false;
    }

    if (!bce_->emitDupAt(6, 2)) {
      // [stack] NEEDS-AWAIT HAS-AWAITED ... IS-SYNC NEEDS-AWAIT HAS-AWAITED
      return false;
    }

    // [stack] ... IS-SYNC NEEDS-AWAIT HAS-AWAITED

    if (!bce_->emit1(JSOp::Not)) {
      // [stack] ... IS-SYNC NEEDS-AWAIT (!HAS-AWAITED)
      return false;
    }

    // The use of BitAnd is a simple optimisation to avoid having
    // jumps if we were to implement this using && operator. The value
    // IS-SYNC is integer 0 or 1 (see static_assert above) and
    // NEEDS-AWAIT and HAS-AWAITED are boolean values. thus
    // the result of the operation is either 0 or 1 which is
    // truthy value that can be consumed by the IfEmitter.
    if (!bce_->emit1(JSOp::BitAnd)) {
      // [stack] ... IS-SYNC (NEEDS-AWAIT & !HAS-AWAITED)
      return false;
    }

    if (!bce_->emit1(JSOp::BitAnd)) {
      // [stack] ... (IS-SYNC & NEEDS-AWAIT & !HAS-AWAITED)
      return false;
    }

    // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX AWAIT-NEEDED

    InternalIfEmitter ifNeedsSyncDisposeUndefinedAwaited(bce_);

    if (!ifNeedsSyncDisposeUndefinedAwaited.emitThen()) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX
      return false;
    }

    // Step 3.d.i. Perform ! Await(undefined).
    if (!bce_->emit1(JSOp::Undefined)) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX UNDEF
      return false;
    }

    if (!bce_->emitAwaitInScope(es)) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX RESOLVED
      return false;
    }

    // Step 3.d.ii. Set needsAwait to false.
    if (!bce_->emitPickN(6)) {
      // [stack] HAS-AWAITED THROWING EXC RESOURCES INDEX RESOLVED NEEDS-AWAIT
      return false;
    }

    if (!bce_->emitPopN(2)) {
      // [stack] HAS-AWAITED THROWING EXC RESOURCES INDEX
      return false;
    }

    if (!bce_->emit1(JSOp::False)) {
      // [stack] HAS-AWAITED THROWING EXC RESOURCES INDEX NEEDS-AWAIT
      return false;
    }

    if (!bce_->emitUnpickN(5)) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX
      return false;
    }

    if (!ifNeedsSyncDisposeUndefinedAwaited.emitEnd()) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX
      return false;
    }
  }

  // [stack] ... RESOURCES INDEX

  // Step 3.c. Let method be resource.[[DisposeMethod]].
  // (reordered)
  // Step 3.e. If method is not undefined, then
  if (!emitResourcePropertyAccess(TaggedParserAtomIndex::WellKnown::method())) {
    // [stack] ... RESOURCES INDEX METHOD
    return false;
  }

  if (!bce_->emit1(JSOp::IsNullOrUndefined)) {
    // [stack] ... RESOURCES INDEX METHOD IS-UNDEF
    return false;
  }

  InternalIfEmitter ifMethodNotUndefined(bce_);

  if (!ifMethodNotUndefined.emitThenElse(IfEmitter::ConditionKind::Negative)) {
    // [stack] ... RESOURCES INDEX METHOD
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack] ... RESOURCES INDEX
    return false;
  }

  TryEmitter tryCall(bce_, TryEmitter::Kind::TryCatch,
                     TryEmitter::ControlKind::NonSyntactic);

  if (!tryCall.emitTry()) {
    // [stack] ... RESOURCES INDEX
    return false;
  }

  // Step 3.c. Let method be resource.[[DisposeMethod]].
  // (reordered)
  if (!emitResourcePropertyAccess(TaggedParserAtomIndex::WellKnown::method())) {
    // [stack] ... RESOURCES INDEX METHOD
    return false;
  }

  // Step 3.a. Let value be resource.[[ResourceValue]].
  // (reordered)
  if (!emitResourcePropertyAccess(TaggedParserAtomIndex::WellKnown::value(),
                                  2)) {
    // [stack] ... RESOURCES INDEX METHOD VALUE
    return false;
  }

  // Step 3.e.i. Let result be Completion(Call(method, value)).
  if (!bce_->emitCall(JSOp::Call, 0)) {
    // [stack] ... RESOURCES INDEX RESULT
    return false;
  }

  if (hasAsyncDisposables_) {
    // Step 3.e.ii. If result is a normal completion and hint is async-dispose,
    // then
    if (!emitResourcePropertyAccess(TaggedParserAtomIndex::WellKnown::hint(),
                                    2)) {
      // [stack] ... RESOURCES INDEX RESULT HINT
      return false;
    }

    // Hint value is either 0 or 1, which can be consumed by the IfEmitter,
    // see static_assert above.
    // [stack] ... RESOURCES INDEX RESULT IS-ASYNC

    InternalIfEmitter ifAsyncDispose(bce_);

    if (!ifAsyncDispose.emitThen()) {
      // [stack] ... RESOURCES INDEX RESULT
      return false;
    }

    // [stack] NEEDS-AWAIT THROWING EXC RESOURCES INDEX RESULT HAS-AWAITED

    // Step 3.e.ii.2. Set hasAwaited to true. (reordered)
    if (!bce_->emitPickN(5)) {
      // [stack] NEEDS-AWAIT THROWING EXC RESOURCES INDEX RESULT HAS-AWAITED
      return false;
    }

    if (!bce_->emit1(JSOp::Pop)) {
      // [stack] NEEDS-AWAIT THROWING EXC RESOURCES INDEX RESULT
      return false;
    }

    if (!bce_->emit1(JSOp::True)) {
      // [stack] NEEDS-AWAIT THROWING EXC RESOURCES INDEX RESULT HAS-AWAITED
      return false;
    }

    if (!bce_->emitUnpickN(5)) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX RESULT
      return false;
    }

    // Step 3.e.ii.1. Set result to Completion(Await(result.[[Value]])).
    if (!bce_->emitAwaitInScope(es)) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX RESOLVED
      return false;
    }

    if (!ifAsyncDispose.emitEnd()) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX RESULT
      return false;
    }
  }

  // [stack] ... THROWING EXC RESOURCES INDEX RESULT

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack] ... THROWING EXC RESOURCES INDEX
    return false;
  }

  // Step 3.e.iii. If result is a throw completion, then
  if (!tryCall.emitCatch()) {
    // [stack] ... THROWING EXC RESOURCES INDEX EXC2
    return false;
  }

  if (!bce_->emitPickN(3)) {
    // [stack] ... THROWING RESOURCES INDEX EXC2 EXC
    return false;
  }

  if (bce_->sc->isSuspendableContext() &&
      bce_->sc->asSuspendableContext()->isGenerator()) {
    // [stack] ... THROWING RESOURCES INDEX EXC2 EXC

    // Generator closure is implemented by throwing a magic value
    // thus when we have a throw completion we must check whether
    // the pending exception is a generator closing exception and overwrite
    // it with the normal exception here or else we will end up exposing
    // the magic value to user program.
    if (!bce_->emit1(JSOp::IsGenClosing)) {
      // [stack] ... THROWING RESOURCES INDEX EXC2 EXC GEN-CLOSING
      return false;
    }

    if (!bce_->emit1(JSOp::Not)) {
      // [stack] ... THROWING RESOURCES INDEX EXC2 EXC !GEN-CLOSING
      return false;
    }

    if (!bce_->emitPickN(5)) {
      // [stack] ... RESOURCES INDEX EXC2 EXC (!GEN-CLOSING) THROWING
      return false;
    }

    if (!bce_->emit1(JSOp::BitAnd)) {
      // [stack] ... RESOURCES INDEX EXC2 EXC (!GEN-CLOSING & THROWING)
      return false;
    }
  } else {
    if (!bce_->emitPickN(4)) {
      // [stack] ... RESOURCES INDEX EXC2 EXC THROWING
      return false;
    }
  }

  // [stack] NEEDS-AWAIT? HAS-AWAITED? RESOURCES INDEX EXC2 EXC THROWING

  InternalIfEmitter ifException(bce_);

  // Step 3.e.iii.1. If completion is a throw completion, then
  if (!ifException.emitThenElse()) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? RESOURCES INDEX EXC2 EXC
    return false;
  }

  // Step 3.e.iii.1.a-f
  if (!bce_->emit1(JSOp::CreateSuppressedError)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? RESOURCES INDEX SUPPRESSED
    return false;
  }

  if (!bce_->emitUnpickN(2)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? SUPPRESSED RESOURCED INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::True)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? SUPPRESSED RESOURCED INDEX THROWING
    return false;
  }

  if (!bce_->emitUnpickN(3)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING SUPPRESSED RESOURCED INDEX
    return false;
  }

  // Step 3.e.iii.2. Else,
  // Step 3.e.iii.2.a. Set completion to result.
  if (!ifException.emitElse()) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? RESOURCES INDEX EXC2 EXC
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? RESOURCES INDEX EXC2
    return false;
  }

  if (!bce_->emitUnpickN(2)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? EXC2 RESOURCES INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::True)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? EXC2 RESOURCES INDEX THROWING
    return false;
  }

  if (!bce_->emitUnpickN(3)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC2 RESOURCES INDEX
    return false;
  }

  if (!ifException.emitEnd()) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC RESOURCES INDEX
    return false;
  }

  if (!tryCall.emitEnd()) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC RESOURCES INDEX
    return false;
  }

  // [stack] ... THROWING EXC RESOURCES INDEX

  // Step 3.f. Else,
  // Step 3.f.i. Assert: hint is async-dispose.
  // (implicit)
  if (!ifMethodNotUndefined.emitElse()) {
    // [stack] ... THROWING EXC RESOURCES INDEX METHOD
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack] ... THROWING EXC RESOURCES INDEX
    return false;
  }

  if (hasAsyncDisposables_) {
    // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX

    // Step 3.f.ii. Set needsAwait to true.
    if (!bce_->emitPickN(5)) {
      // [stack] HAS-AWAITED THROWING EXC RESOURCES INDEX NEEDS-AWAIT
      return false;
    }

    if (!bce_->emit1(JSOp::Pop)) {
      // [stack] HAS-AWAITED THROWING EXC RESOURCES INDEX
      return false;
    }

    if (!bce_->emit1(JSOp::True)) {
      // [stack] HAS-AWAITED THROWING EXC RESOURCES INDEX NEEDS-AWAIT
      return false;
    }

    if (!bce_->emitUnpickN(5)) {
      // [stack] NEEDS-AWAIT HAS-AWAITED THROWING EXC RESOURCES INDEX
      return false;
    }
  }

  if (!ifMethodNotUndefined.emitEnd()) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC RESOURCES INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::Dec)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC RESOURCES INDEX
    return false;
  }

  if (!wh.emitEnd()) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC RESOURCES INDEX
    return false;
  }

  if (!bce_->emitPopN(2)) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC
    return false;
  }

  if (hasAsyncDisposables_) {
    // Step 4. If needsAwait is true and hasAwaited is false, then
    if (!bce_->emitPickN(3)) {
      // [stack] HAS-AWAITED THROWING EXC NEEDS-AWAIT
      return false;
    }

    if (!bce_->emitPickN(3)) {
      // [stack] THROWING EXC NEEDS-AWAIT HAS-AWAITED
      return false;
    }

    if (!bce_->emit1(JSOp::Not)) {
      // [stack] THROWING EXC NEEDS-AWAIT (!HAS-AWAITED)
      return false;
    }

    if (!bce_->emit1(JSOp::BitAnd)) {
      // [stack] THROWING EXC (NEEDS-AWAIT & !HAS-AWAITED)
      return false;
    }

    InternalIfEmitter ifNeedsUndefinedAwait(bce_);

    if (!ifNeedsUndefinedAwait.emitThen()) {
      // [stack] THROWING EXC
      return false;
    }

    if (!bce_->emit1(JSOp::Undefined)) {
      // [stack] THROWING EXC UNDEF
      return false;
    }

    if (!bce_->emitAwaitInScope(es)) {
      // [stack] THROWING EXC RESOLVED
      return false;
    }

    if (!bce_->emit1(JSOp::Pop)) {
      // [stack] THROWING EXC
      return false;
    }

    if (!ifNeedsUndefinedAwait.emitEnd()) {
      // [stack] THROWING EXC
      return false;
    }
  }

  // Step 7. Return ? completion.
  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] EXC THROWING
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool UsingEmitter::emitDisposeResourcesForEnvironment(EmitterScope& es) {
  // [stack] THROWING EXC

  DisposalEmitter de(bce_, hasAwaitUsing_);
  if (!de.prepareForDisposeCapability()) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC
    return false;
  }

  // Explicit Resource Management Proposal
  // DisposeResources ( disposeCapability, completion )
  // https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-disposeresources
  //
  // Step 6. Set disposeCapability.[[DisposableResourceStack]] to a new empty
  // List.
  if (!emitTakeDisposeCapability()) {
    // [stack] NEEDS-AWAIT? HAS-AWAITED? THROWING EXC RESOURCES COUNT
    return false;
  }

  if (!de.emitEnd(es)) {
    // [stack] EXC THROWING
    return false;
  }

  return true;
}

bool UsingEmitter::prepareForDisposableScopeBody(BlockKind blockKind) {
  MOZ_ASSERT(state_ == State::Start);

  // For-of loops are already wrapped in try-catch and have special case
  // handling for the same.
  // See ForOfLoopControl::emitEndCodeNeedingIteratorClose.
  if (blockKind != BlockKind::ForOf) {
    tryEmitter_.emplace(bce_, TryEmitter::Kind::TryFinally,
                        TryEmitter::ControlKind::Disposal);
    if (!tryEmitter_->emitTry()) {
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::DisposableScopeBody;
#endif
  return true;
}

// Explicit Resource Management Proposal
// GetDisposeMethod ( V, hint )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-getdisposemethod
// Steps 1.a-1.b.i., 2-3.
bool UsingEmitter::emitGetDisposeMethod(UsingHint hint) {
  // [stack] VAL

  // Step 1. If hint is async-dispose, then
  if (hint == UsingHint::Async) {
    // Step 1.a. Let method be ? GetMethod(V, @@asyncDispose).
    if (!bce_->emit1(JSOp::Dup)) {
      // [stack] VAL VAL
      return false;
    }

    if (!bce_->emit1(JSOp::Dup)) {
      // [stack] VAL VAL VAL
      return false;
    }

    if (!bce_->emit2(JSOp::Symbol, uint8_t(JS::SymbolCode::asyncDispose))) {
      // [stack] VAL VAL VAL SYMBOL
      return false;
    }

    if (!bce_->emit1(JSOp::GetElem)) {
      // [stack] VAL VAL ASYNC-DISPOSE
      return false;
    }

    // Step 1.b. If method is undefined, then
    // GetMethod returns undefined if the function is null but
    // since we do not do the conversion here we check for
    // null or undefined here.
    if (!bce_->emit1(JSOp::IsNullOrUndefined)) {
      // [stack] VAL VAL ASYNC-DISPOSE IS-NULL-OR-UNDEF
      return false;
    }

    InternalIfEmitter ifAsyncDisposeNullOrUndefined(bce_);

    if (!ifAsyncDisposeNullOrUndefined.emitThenElse()) {
      // [stack] VAL VAL ASYNC-DISPOSE
      return false;
    }

    if (!bce_->emit1(JSOp::Pop)) {
      // [stack] VAL VAL
      return false;
    }

    if (!bce_->emit1(JSOp::Dup)) {
      // [stack] VAL VAL VAL
      return false;
    }

    if (!bce_->emit2(JSOp::Symbol, uint8_t(JS::SymbolCode::dispose))) {
      // [stack] VAL VAL VAL SYMBOL
      return false;
    }

    // Step 1.b.i. Set method to ? GetMethod(V, @@dispose).
    if (!bce_->emit1(JSOp::GetElem)) {
      // [stack] VAL VAL DISPOSE
      return false;
    }

    if (!bce_->emit1(JSOp::True)) {
      // [stack] VAL VAL DISPOSE NEEDS-CLOSURE
      return false;
    }

    if (!ifAsyncDisposeNullOrUndefined.emitElse()) {
      // [stack] VAL VAL ASYNC-DISPOSE
      return false;
    }

    if (!bce_->emit1(JSOp::False)) {
      // [stack] VAL VAL ASYNC-DISPOSE NEEDS-CLOSURE
      return false;
    }

    if (!ifAsyncDisposeNullOrUndefined.emitEnd()) {
      // [stack] VAL VAL METHOD NEEDS-CLOSURE
      return false;
    }

  } else {
    MOZ_ASSERT(hint == UsingHint::Sync);

    // Step 2. Else,
    // Step 2.a. Let method be ? GetMethod(V, @@dispose).
    if (!bce_->emit1(JSOp::Dup)) {
      // [stack] VAL VAL
      return false;
    }

    if (!bce_->emit1(JSOp::Dup)) {
      // [stack] VAL VAL VAL
      return false;
    }

    if (!bce_->emit2(JSOp::Symbol, uint8_t(JS::SymbolCode::dispose))) {
      // [stack] VAL VAL VAL SYMBOL
      return false;
    }

    if (!bce_->emit1(JSOp::GetElem)) {
      // [stack] VAL VAL DISPOSE
      return false;
    }

    if (!bce_->emit1(JSOp::False)) {
      // [stack] VAL VAL DISPOSE NEEDS-CLOSURE
      return false;
    }
  }

  if (!bce_->emitDupAt(1)) {
    // [stack] VAL VAL METHOD NEEDS-CLOSURE METHOD
    return false;
  }

  // According to spec GetMethod throws TypeError if the method is not callable
  // and returns undefined if the value is either undefined or null.
  // but the caller of this function, emitCreateDisposableResource is
  // supposed to throw TypeError as well if returned value is undefined,
  // thus we combine the steps here.
  if (!bce_->emitCheckIsCallable()) {
    // [stack] VAL VAL METHOD NEEDS-CLOSURE METHOD IS-CALLABLE
    return false;
  }

  InternalIfEmitter ifMethodNotCallable(bce_);

  if (!ifMethodNotCallable.emitThen(IfEmitter::ConditionKind::Negative)) {
    // [stack] VAL VAL METHOD NEEDS-CLOSURE METHOD
    return false;
  }

  if (!bce_->emit2(JSOp::ThrowMsg, uint8_t(ThrowMsgKind::DisposeNotCallable))) {
    // [stack] VAL VAL METHOD NEEDS-CLOSURE METHOD
    return false;
  }

  if (!ifMethodNotCallable.emitEnd()) {
    // [stack] VAL VAL METHOD NEEDS-CLOSURE METHOD
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack] VAL VAL METHOD NEEDS-CLOSURE
    return false;
  }

  return true;
}

// Explicit Resource Management Proposal
// CreateDisposableResource ( V, hint [ , method ] )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-createdisposableresource
bool UsingEmitter::emitCreateDisposableResource(UsingHint hint) {
  // [stack] VAL

  // Step 1. If method is not present, then (implicit)
  // Step 1.a. If V is either null or undefined, then
  if (!bce_->emit1(JSOp::IsNullOrUndefined)) {
    // [stack] VAL IS-NULL-OR-UNDEF
    return false;
  }

  InternalIfEmitter ifNullUndefined(bce_);

  if (!ifNullUndefined.emitThenElse()) {
    // [stack] VAL
    return false;
  }

  // Step 1.a.i. Set V to undefined.
  if (!bce_->emit1(JSOp::Undefined)) {
    // [stack] VAL UNDEF
    return false;
  }

  // Step 1.a.ii. Set method to undefined.
  if (!bce_->emit1(JSOp::Undefined)) {
    // [stack] VAL UNDEF UNDEF
    return false;
  }

  if (!bce_->emit1(JSOp::False)) {
    // [stack] VAL UNDEF UNDEF NEEDS-CLOSURE
    return false;
  }

  // Step 1.b. Else,
  if (!ifNullUndefined.emitElse()) {
    // [stack] VAL
    return false;
  }

  // Step 1.b.i. If V is not an Object, throw a TypeError exception.
  if (!bce_->emitCheckIsObj(CheckIsObjectKind::Disposable)) {
    // [stack] VAL
    return false;
  }

  // Step 1.b.ii. Set method to ? GetDisposeMethod(V, hint).
  // Step 1.b.iii. If method is undefined, throw a TypeError exception.
  if (!emitGetDisposeMethod(hint)) {
    // [stack] VAL VAL METHOD NEEDS-CLOSURE
    return false;
  }

  if (!ifNullUndefined.emitEnd()) {
    // [stack] VAL VAL METHOD NEEDS-CLOSURE
    return false;
  }

  return true;
}

// Explicit Resource Management Proposal
// 7.5.4 AddDisposableResource ( disposeCapability, V, hint [ , method ] )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-adddisposableresource
// Steps 1, 3-4.
bool UsingEmitter::prepareForAssignment(UsingHint hint) {
  MOZ_ASSERT(state_ == State::DisposableScopeBody);
  MOZ_ASSERT(bce_->innermostEmitterScope()->hasDisposables());

  if (hint == UsingHint::Async) {
    setHasAwaitUsing(true);
  }

  // [stack] VAL

  // Step 1. If method is not present, then (implicit)
  // Step 1.a. If V is either null or undefined and hint is sync-dispose, return
  // unused.
  if (hint == UsingHint::Sync) {
    if (!bce_->emit1(JSOp::IsNullOrUndefined)) {
      // [stack] VAL IS-NULL-OR-UNDEF
      return false;
    }

    if (!bce_->emit1(JSOp::Not)) {
      // [stack] VAL !IS-NULL-OR-UNDEF
      return false;
    }
  } else {
    MOZ_ASSERT(hint == UsingHint::Async);
    if (!bce_->emit1(JSOp::True)) {
      // [stack] VAL TRUE
      return false;
    }
  }

  // [stack] VAL SHOULD-CREATE-RESOURCE

  InternalIfEmitter ifCreateResource(bce_);

  if (!ifCreateResource.emitThen()) {
    // [stack] VAL
    return false;
  }

  // Step 1.c. Let resource be ? CreateDisposableResource(V, hint).
  if (!emitCreateDisposableResource(hint)) {
    // [stack] VAL VAL METHOD NEEDS-CLOSURE
    return false;
  }

  // Step 3. Append resource to disposeCapability.[[DisposableResourceStack]].
  if (!bce_->emit2(JSOp::AddDisposable, uint8_t(hint))) {
    // [stack] VAL
    return false;
  }

  if (!ifCreateResource.emitEnd()) {
    // [stack] VAL
    return false;
  }

  // Step 4. Return unused.
  return true;
}

bool ForOfDisposalEmitter::prepareForForOfLoopIteration() {
  MOZ_ASSERT(state_ == State::Start);
  EmitterScope* es = bce_->innermostEmitterScopeNoCheck();
  MOZ_ASSERT(es->hasDisposables());

  if (!bce_->emit1(JSOp::False)) {
    // [stack] THROWING
    return false;
  }

  if (!bce_->emit1(JSOp::Undefined)) {
    // [stack] THROWING UNDEF
    return false;
  }

  if (!emitDisposeResourcesForEnvironment(*es)) {
    // [stack] EXC THROWING
    return false;
  }

  if (!emitThrowIfException()) {
    // [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::Iteration;
#endif
  return true;
}

bool ForOfDisposalEmitter::emitEnd() {
  MOZ_ASSERT(state_ == State::Iteration);
  EmitterScope* es = bce_->innermostEmitterScopeNoCheck();
  MOZ_ASSERT(es->hasDisposables());

  // [stack] EXC STACK

  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] STACK EXC
    return false;
  }

  if (!bce_->emit1(JSOp::True)) {
    // [stack] STACK EXC THROWING
    return false;
  }

  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] STACK THROWING EXC
    return false;
  }

  if (!emitDisposeResourcesForEnvironment(*es)) {
    // [stack] STACK EXC THROWING
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack] STACK EXC
    return false;
  }

  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] EXC STACK
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool UsingEmitter::emitEnd() {
  MOZ_ASSERT(state_ == State::DisposableScopeBody);
  EmitterScope* es = bce_->innermostEmitterScopeNoCheck();
  MOZ_ASSERT(es->hasDisposables());
  MOZ_ASSERT(tryEmitter_.isSome());

  if (!tryEmitter_->emitFinally()) {
    // [stack] EXC-OR-RESUME STACK THROWING RVAL?
    return false;
  }

  if (!bce_->emitDupAt(tryEmitter_->shouldUpdateRval() ? 1 : 0)) {
    // [stack] EXC-OR-RESUME STACK THROWING RVAL? THROWING
    return false;
  }

  InternalIfEmitter ifThrowing(bce_);

  if (!ifThrowing.emitThenElse()) {
    // [stack] EXC STACK THROWING RVAL?
    return false;
  }

  if (!bce_->emit1(JSOp::True)) {
    // [stack] EXC STACK THROWING RVAL? THROWING
    return false;
  }

  if (!bce_->emitDupAt(tryEmitter_->shouldUpdateRval() ? 4 : 3)) {
    // [stack] EXC STACK THROWING RVAL? THROWING EXC
    return false;
  }

  if (!ifThrowing.emitElse()) {
    // [stack] RESUME STACK THROWING RVAL?
    return false;
  }

  if (!bce_->emit1(JSOp::False)) {
    // [stack] RESUME STACK THROWING RVAL? THROWING
    return false;
  }

  if (!bce_->emit1(JSOp::Undefined)) {
    // [stack] RESUME STACK THROWING RVAL? THROWING UNDEF
    return false;
  }

  if (!ifThrowing.emitEnd()) {
    // [stack] EXC-OR-RESUME STACK THROWING RVAL? THROWING EXC-OR-UNDEF
    return false;
  }

  if (!emitDisposeResourcesForEnvironment(*es)) {
    // [stack] EXC-OR-RESUME STACK THROWING RVAL? DISPOSAL-EXC DISPOSAL-THROWING
    return false;
  }

  if (bce_->sc->isSuspendableContext() &&
      bce_->sc->asSuspendableContext()->isGenerator()) {
    // [stack] ... DISP-EXC DISP-THROWING

    if (!bce_->emit1(JSOp::Swap)) {
      // [stack] ... DISP-THROWING DISP-EXC
      return false;
    }

    if (!bce_->emit1(JSOp::IsGenClosing)) {
      // [stack] ... DISP-THROWING DISP-EXC GEN-CLOSING
      return false;
    }

    if (!bce_->emit1(JSOp::Not)) {
      // [stack] ... DISP-THROWING DISP-EXC !GEN-CLOSING
      return false;
    }

    if (!bce_->emitPickN(2)) {
      // [stack] ... DISP-EXC !GEN-CLOSING DISP-THROWING
      return false;
    }

    if (!bce_->emit1(JSOp::BitAnd)) {
      // [stack] ... DISP-EXC (DISP-THROWING & !GEN-CLOSING)
      return false;
    }
  }

  if (!emitThrowIfException()) {
    // [stack] EXC-OR-RESUME STACK THROWING RVAL?
    return false;
  }

  if (!tryEmitter_->emitEnd()) {
    // [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

// Explicit Resource Management Proposal
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-runtime-semantics-forin-div-ofbodyevaluation-lhs-stmt-iterator-lhskind-labelset
// Step 9.k.i.
bool NonLocalIteratorCloseUsingEmitter::prepareForIteratorClose(
    EmitterScope& es) {
  MOZ_ASSERT(state_ == State::Start);
  // In this function we prepare for the closure of the iterator but first
  // emitting the dispose loop and preseving exceptions on the stack and after
  // that emitting a try to wrap the iterator closure code that shall come after
  // this.
  if (!es.hasDisposables()) {
#ifdef DEBUG
    state_ = State::IteratorClose;
#endif
    return true;
  }

  setHasAwaitUsing(es.hasAsyncDisposables());

  // [stack] ITER

  if (!bce_->emit1(JSOp::False)) {
    // [stack] THROWING
    return false;
  }

  if (!bce_->emit1(JSOp::Undefined)) {
    // [stack] THROWING UNDEF
    return false;
  }

  if (!emitDisposeResourcesForEnvironment(es)) {
    // [stack] ITER EXC-DISPOSE DISPOSE-THROWING
    return false;
  }

  if (!bce_->emitPickN(2)) {
    // [stack] EXC-DISPOSE DISPOSE-THROWING ITER
    return false;
  }

  tryClosingIterator_.emplace(bce_, TryEmitter::Kind::TryCatch,
                              TryEmitter::ControlKind::NonSyntactic);

  if (!tryClosingIterator_->emitTry()) {
    // [stack] EXC-DISPOSE DISPOSE-THROWING ITER
    return false;
  }

#ifdef DEBUG
  state_ = State::IteratorClose;
#endif
  return true;
}

// Explicit Resource Management Proposal
// 7.4.9 IteratorClose ( iteratorRecord, completion )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-iteratorclose&secAll=true
// Steps 5-6.
//
// 7.4.11 AsyncIteratorClose ( iteratorRecord, completion )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-asynciteratorclose&secAll=true
// Steps 5-6.
bool NonLocalIteratorCloseUsingEmitter::emitEnd() {
  // This function handles the steps after the iterator close operation which
  // may or may not have thrown. note that prepareForIteratorClose would have
  // already wrapped the iterator closure with a try and have preserved any
  // exception by the disposal operation on the stack now this function does the
  // equivalent of the following pseudocode (consider excDispose and
  // disposeThrowing and iter equal to corresponding values left on stack by
  // prepareForIteratorClose):
  //
  //
  // let excToThrow, throwing = disposeThrowing;
  // try {
  //   IteratorClose(iter);
  // } catch (excIterClose) {
  //   throwing = true;
  //   if (disposeThrowing) {
  //     excToThrow = excDispose;
  //   } else {
  //     excToThrow = excIterClose;
  //   }
  // }
  // if (throwing) {
  //   throw excToThrow;
  // }
  //
  MOZ_ASSERT(state_ == State::IteratorClose);

  if (!tryClosingIterator_.isSome()) {
#ifdef DEBUG
    state_ = State::End;
#endif
    return true;
  }

  // [stack] EXC-DISPOSE DISPOSE-THROWING ITER

  if (!tryClosingIterator_->emitCatch()) {
    // [stack] EXC-DISPOSE DISPOSE-THROWING ITER EXC-ITER-CLOSE
    return false;
  }

  if (!bce_->emitPickN(2)) {
    // [stack] EXC-DISPOSE ITER EXC-ITER-CLOSE DISPOSE-THROWING
    return false;
  }

  InternalIfEmitter ifDisposeWasThrowing(bce_);

  if (!ifDisposeWasThrowing.emitThenElse()) {
    // [stack] EXC-DISPOSE ITER EXC-ITER-CLOSE
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack] EXC-DISPOSE ITER
    return false;
  }

  if (!bce_->emit1(JSOp::True)) {
    // [stack] EXC-DISPOSE ITER THROWING
    return false;
  }

  // This swap operation is to make the stack state consistent with the
  // the non-throwing case.
  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] EXC-DISPOSE THROWING ITER
    return false;
  }

  if (!ifDisposeWasThrowing.emitElse()) {
    // [stack] EXC-DISPOSE ITER EXC-ITER-CLOSE
    return false;
  }

  if (!bce_->emitPickN(2)) {
    // [stack] ITER EXC-ITER-CLOSE EXC-DISPOSE
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack] ITER EXC-ITER-CLOSE
    return false;
  }

  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] EXC-ITER-CLOSE ITER
    return false;
  }

  if (!bce_->emit1(JSOp::True)) {
    // [stack] EXC-ITER-CLOSE ITER THROWING
    return false;
  }

  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] EXC-ITER-CLOSE THROWING ITER
    return false;
  }

  if (!ifDisposeWasThrowing.emitEnd()) {
    // [stack] EXC THROWING ITER
    return false;
  }

  if (!tryClosingIterator_->emitEnd()) {
    // [stack] EXC THROWING ITER
    return false;
  }

  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] EXC ITER THROWING
    return false;
  }

  InternalIfEmitter ifThrowing(bce_);

  if (!ifThrowing.emitThenElse()) {
    // [stack] EXC ITER
    return false;
  }

  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] ITER EXC
    return false;
  }

  if (!bce_->emit1(JSOp::Throw)) {
    // [stack] ITER
    return false;
  }

  if (!ifThrowing.emitElse()) {
    // [stack] EXC ITER
    return false;
  }

  if (!bce_->emit1(JSOp::Swap)) {
    // [stack] ITER EXC
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    // [stack] ITER
    return false;
  }

  if (!ifThrowing.emitEnd()) {
    // [stack] ITER
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
