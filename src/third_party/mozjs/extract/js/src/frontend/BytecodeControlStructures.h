/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BytecodeControlStructures_h
#define frontend_BytecodeControlStructures_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include <stdint.h>  // int32_t, uint32_t

#include "ds/Nestable.h"              // Nestable
#include "frontend/BytecodeOffset.h"  // BytecodeOffset
#include "frontend/JumpList.h"        // JumpList, JumpTarget
#include "frontend/ParserAtom.h"      // TaggedParserAtomIndex
#include "frontend/SharedContext.h"  // StatementKind, StatementKindIsLoop, StatementKindIsUnlabeledBreakTarget
#include "frontend/TDZCheckCache.h"  // TDZCheckCache
#include "vm/StencilEnums.h"         // TryNoteKind

namespace js {
namespace frontend {

struct BytecodeEmitter;
class EmitterScope;

class NestableControl : public Nestable<NestableControl> {
  StatementKind kind_;

  // The innermost scope when this was pushed.
  EmitterScope* emitterScope_;

 protected:
  NestableControl(BytecodeEmitter* bce, StatementKind kind);

 public:
  using Nestable<NestableControl>::enclosing;
  using Nestable<NestableControl>::findNearest;

  StatementKind kind() const { return kind_; }

  EmitterScope* emitterScope() const { return emitterScope_; }

  template <typename T>
  bool is() const;

  template <typename T>
  T& as() {
    MOZ_ASSERT(this->is<T>());
    return static_cast<T&>(*this);
  }
};

class BreakableControl : public NestableControl {
 public:
  // Offset of the last break.
  JumpList breaks;

  BreakableControl(BytecodeEmitter* bce, StatementKind kind);

  [[nodiscard]] bool patchBreaks(BytecodeEmitter* bce);
};
template <>
inline bool NestableControl::is<BreakableControl>() const {
  return StatementKindIsUnlabeledBreakTarget(kind_) ||
         kind_ == StatementKind::Label;
}

class LabelControl : public BreakableControl {
  TaggedParserAtomIndex label_;

  // The code offset when this was pushed. Used for effectfulness checking.
  BytecodeOffset startOffset_;

 public:
  LabelControl(BytecodeEmitter* bce, TaggedParserAtomIndex label,
               BytecodeOffset startOffset);

  TaggedParserAtomIndex label() const { return label_; }

  BytecodeOffset startOffset() const { return startOffset_; }
};
template <>
inline bool NestableControl::is<LabelControl>() const {
  return kind_ == StatementKind::Label;
}

class LoopControl : public BreakableControl {
  // Loops' children are emitted in dominance order, so they can always
  // have a TDZCheckCache.
  TDZCheckCache tdzCache_;

  // Here's the basic structure of a loop:
  //
  //   head:
  //     JSOp::LoopHead
  //     {loop condition/body}
  //
  //   continueTarget:
  //     {loop update if present}
  //
  //     # Loop end, backward jump
  //     JSOp::Goto/JSOp::JumpIfTrue head
  //
  //   breakTarget:

  // The bytecode offset of JSOp::LoopHead.
  JumpTarget head_;

  // Stack depth when this loop was pushed on the control stack.
  int32_t stackDepth_;

  // The loop nesting depth. Used as a hint to Ion.
  uint32_t loopDepth_;

 public:
  // Offset of the last continue in the loop.
  JumpList continues;

  LoopControl(BytecodeEmitter* bce, StatementKind loopKind);

  BytecodeOffset headOffset() const { return head_.offset; }

  [[nodiscard]] bool emitContinueTarget(BytecodeEmitter* bce);

  // `nextPos` is the offset in the source code for the character that
  // corresponds to the next instruction after JSOp::LoopHead.
  // Can be Nothing() if not available.
  [[nodiscard]] bool emitLoopHead(BytecodeEmitter* bce,
                                  const mozilla::Maybe<uint32_t>& nextPos);

  [[nodiscard]] bool emitLoopEnd(BytecodeEmitter* bce, JSOp op,
                                 TryNoteKind tryNoteKind);
};
template <>
inline bool NestableControl::is<LoopControl>() const {
  return StatementKindIsLoop(kind_);
}

enum class NonLocalExitKind { Continue, Break, Return };

class TryFinallyContinuation {
 public:
  TryFinallyContinuation(NestableControl* target, NonLocalExitKind kind)
      : target_(target), kind_(kind) {}

  NestableControl* target_;
  NonLocalExitKind kind_;
};

class TryFinallyControl : public NestableControl {
  bool emittingSubroutine_ = false;

 public:
  // Offset of the last jump to this `finally`.
  JumpList finallyJumps_;

  js::Vector<TryFinallyContinuation, 4, SystemAllocPolicy> continuations_;

  TryFinallyControl(BytecodeEmitter* bce, StatementKind kind);

  void setEmittingSubroutine() { emittingSubroutine_ = true; }

  bool emittingSubroutine() const { return emittingSubroutine_; }

  enum SpecialContinuations { Fallthrough, Count };
  bool allocateContinuation(NestableControl* target, NonLocalExitKind kind,
                            uint32_t* idx);
  bool emitContinuations(BytecodeEmitter* bce);
};
template <>
inline bool NestableControl::is<TryFinallyControl>() const {
  return kind_ == StatementKind::Try || kind_ == StatementKind::Finally;
}

class NonLocalExitControl {
  BytecodeEmitter* bce_;
  const uint32_t savedScopeNoteIndex_;
  const int savedDepth_;
  uint32_t openScopeNoteIndex_;
  NonLocalExitKind kind_;

  // The offset of a `JSOp::SetRval` that can be rewritten as a
  // `JSOp::Return` if we don't generate any code for this
  // NonLocalExitControl.
  BytecodeOffset setRvalOffset_ = BytecodeOffset::invalidOffset();

  [[nodiscard]] bool leaveScope(EmitterScope* es);

 public:
  NonLocalExitControl(const NonLocalExitControl&) = delete;
  NonLocalExitControl(BytecodeEmitter* bce, NonLocalExitKind kind);
  ~NonLocalExitControl();

  [[nodiscard]] bool emitNonLocalJump(NestableControl* target,
                                      NestableControl* startingAfter = nullptr);
  [[nodiscard]] bool emitReturn(BytecodeOffset setRvalOffset);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_BytecodeControlStructures_h */
