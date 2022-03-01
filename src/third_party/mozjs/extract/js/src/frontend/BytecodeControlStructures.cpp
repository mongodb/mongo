/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/BytecodeControlStructures.h"

#include "frontend/BytecodeEmitter.h"  // BytecodeEmitter
#include "frontend/EmitterScope.h"     // EmitterScope
#include "vm/Opcodes.h"                // JSOp

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;

NestableControl::NestableControl(BytecodeEmitter* bce, StatementKind kind)
    : Nestable<NestableControl>(&bce->innermostNestableControl),
      kind_(kind),
      emitterScope_(bce->innermostEmitterScopeNoCheck()) {}

BreakableControl::BreakableControl(BytecodeEmitter* bce, StatementKind kind)
    : NestableControl(bce, kind) {
  MOZ_ASSERT(is<BreakableControl>());
}

bool BreakableControl::patchBreaks(BytecodeEmitter* bce) {
  return bce->emitJumpTargetAndPatch(breaks);
}

LabelControl::LabelControl(BytecodeEmitter* bce, TaggedParserAtomIndex label,
                           BytecodeOffset startOffset)
    : BreakableControl(bce, StatementKind::Label),
      label_(label),
      startOffset_(startOffset) {}

LoopControl::LoopControl(BytecodeEmitter* bce, StatementKind loopKind)
    : BreakableControl(bce, loopKind), tdzCache_(bce) {
  MOZ_ASSERT(is<LoopControl>());

  LoopControl* enclosingLoop = findNearest<LoopControl>(enclosing());

  stackDepth_ = bce->bytecodeSection().stackDepth();
  loopDepth_ = enclosingLoop ? enclosingLoop->loopDepth_ + 1 : 1;
}

bool LoopControl::emitContinueTarget(BytecodeEmitter* bce) {
  // Note: this is always called after emitting the loop body so we must have
  // emitted all 'continues' by now.
  return bce->emitJumpTargetAndPatch(continues);
}

bool LoopControl::emitLoopHead(BytecodeEmitter* bce,
                               const Maybe<uint32_t>& nextPos) {
  // Insert a Nop if needed to ensure the script does not start with a
  // JSOp::LoopHead. This avoids JIT issues with prologue code + try notes
  // or OSR. See bug 1602390 and bug 1602681.
  if (bce->bytecodeSection().offset().toUint32() == 0) {
    if (!bce->emit1(JSOp::Nop)) {
      return false;
    }
  }

  if (nextPos) {
    if (!bce->updateSourceCoordNotes(*nextPos)) {
      return false;
    }
  }

  MOZ_ASSERT(loopDepth_ > 0);

  head_ = {bce->bytecodeSection().offset()};

  BytecodeOffset off;
  if (!bce->emitJumpTargetOp(JSOp::LoopHead, &off)) {
    return false;
  }
  SetLoopHeadDepthHint(bce->bytecodeSection().code(off), loopDepth_);

  return true;
}

bool LoopControl::emitLoopEnd(BytecodeEmitter* bce, JSOp op,
                              TryNoteKind tryNoteKind) {
  JumpList jump;
  if (!bce->emitJumpNoFallthrough(op, &jump)) {
    return false;
  }
  bce->patchJumpsToTarget(jump, head_);

  // Create a fallthrough for closing iterators, and as a target for break
  // statements.
  JumpTarget breakTarget;
  if (!bce->emitJumpTarget(&breakTarget)) {
    return false;
  }
  if (!patchBreaks(bce)) {
    return false;
  }
  if (!bce->addTryNote(tryNoteKind, bce->bytecodeSection().stackDepth(),
                       headOffset(), breakTarget.offset)) {
    return false;
  }
  return true;
}

TryFinallyControl::TryFinallyControl(BytecodeEmitter* bce, StatementKind kind)
    : NestableControl(bce, kind), emittingSubroutine_(false) {
  MOZ_ASSERT(is<TryFinallyControl>());
}
