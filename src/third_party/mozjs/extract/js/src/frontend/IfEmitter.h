/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_IfEmitter_h
#define frontend_IfEmitter_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stdint.h>

#include "frontend/JumpList.h"
#include "frontend/TDZCheckCache.h"

namespace js {
namespace frontend {

struct BytecodeEmitter;

class MOZ_STACK_CLASS BranchEmitterBase {
 public:
  // Whether the then-clause, the else-clause, or else-if condition may
  // contain declaration or access to lexical variables, which means they
  // should have their own TDZCheckCache.  Basically TDZCheckCache should be
  // created for each basic block, which then-clause, else-clause, and
  // else-if condition are, but for internally used branches which are
  // known not to touch lexical variables we can skip creating TDZCheckCache
  // for them.
  //
  // See the comment for TDZCheckCache class for more details.
  enum class LexicalKind {
    // For syntactic branches (if, if-else, and conditional expression),
    // which basically may contain declaration or accesses to lexical
    // variables inside then-clause, else-clause, and else-if condition.
    MayContainLexicalAccessInBranch,

    // For internally used branches which don't touch lexical variables
    // inside then-clause, else-clause, nor else-if condition.
    NoLexicalAccessInBranch
  };

 protected:
  BytecodeEmitter* bce_;

  // Jump around the then clause, to the beginning of the else clause.
  JumpList jumpAroundThen_;

  // Jump around the else clause, to the end of the entire branch.
  JumpList jumpsAroundElse_;

  // The stack depth before emitting the then block.
  // Used for restoring stack depth before emitting the else block.
  // Also used for assertion to make sure then and else blocks pushed the
  // same number of values.
  int32_t thenDepth_ = 0;

  enum class ConditionKind { Positive, Negative };
  LexicalKind lexicalKind_;

  mozilla::Maybe<TDZCheckCache> tdzCache_;

#ifdef DEBUG
  // The number of values pushed in the then and else blocks.
  int32_t pushed_ = 0;
  bool calculatedPushed_ = false;
#endif

 protected:
  BranchEmitterBase(BytecodeEmitter* bce, LexicalKind lexicalKind);

  [[nodiscard]] bool emitThenInternal(ConditionKind conditionKind);
  void calculateOrCheckPushed();
  [[nodiscard]] bool emitElseInternal();
  [[nodiscard]] bool emitEndInternal();

 public:
#ifdef DEBUG
  // Returns the number of values pushed onto the value stack inside
  // `then_block` and `else_block`.
  // Can be used in assertion after emitting if-then-else.
  int32_t pushed() const { return pushed_; }

  // Returns the number of values popped onto the value stack inside
  // `then_block` and `else_block`.
  // Can be used in assertion after emitting if-then-else.
  int32_t popped() const { return -pushed_; }
#endif
};

// Class for emitting bytecode for blocks like if-then-else.
//
// This class can be used to emit single if-then-else block, or cascading
// else-if blocks.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `if (cond) then_block`
//     IfEmitter ifThen(this);
//     ifThen.emitIf(Some(offset_of_if));
//     emit(cond);
//     ifThen.emitThen();
//     emit(then_block);
//     ifThen.emitEnd();
//
//   `if (!cond) then_block`
//     IfEmitter ifThen(this);
//     ifThen.emitIf(Some(offset_of_if));
//     emit(cond);
//     ifThen.emitThen(IfEmitter::ConditionKind::Negative);
//     emit(then_block);
//     ifThen.emitEnd();
//
//   `if (cond) then_block else else_block`
//     IfEmitter ifThenElse(this);
//     ifThen.emitIf(Some(offset_of_if));
//     emit(cond);
//     ifThenElse.emitThenElse();
//     emit(then_block);
//     ifThenElse.emitElse();
//     emit(else_block);
//     ifThenElse.emitEnd();
//
//   `if (c1) b1 else if (c2) b2 else if (c3) b3 else b4`
//     IfEmitter ifThenElse(this);
//     ifThen.emitIf(Some(offset_of_if));
//     emit(c1);
//     ifThenElse.emitThenElse();
//     emit(b1);
//     ifThenElse.emitElseIf(Some(offset_of_if));
//     emit(c2);
//     ifThenElse.emitThenElse();
//     emit(b2);
//     ifThenElse.emitElseIf(Some(offset_of_if));
//     emit(c3);
//     ifThenElse.emitThenElse();
//     emit(b3);
//     ifThenElse.emitElse();
//     emit(b4);
//     ifThenElse.emitEnd();
//
class MOZ_STACK_CLASS IfEmitter : public BranchEmitterBase {
 public:
  using ConditionKind = BranchEmitterBase::ConditionKind;

 protected:
#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+ emitIf +----+
  // | Start |------->| If |-+
  // +-------+        +----+ |
  //                         |
  //    +--------------------+
  //    |
  //    v emitThen +------+                               emitEnd +-----+
  // +->+--------->| Then |---------------------------->+-------->| End |
  // ^  |          +------+                             ^         +-----+
  // |  |                                               |
  // |  |                                               |
  // |  |                                               |
  // |  | emitThenElse +----------+   emitElse +------+ |
  // |  +------------->| ThenElse |-+--------->| Else |-+
  // |                 +----------+ |          +------+
  // |                              |
  // |                              | emitElseIf +--------+
  // |                              +----------->| ElseIf |-+
  // |                                           +--------+ |
  // |                                                      |
  // +------------------------------------------------------+
  enum class State {
    // The initial state.
    Start,

    // After calling emitIf.
    If,

    // After calling emitThen.
    Then,

    // After calling emitThenElse.
    ThenElse,

    // After calling emitElse.
    Else,

    // After calling emitElseIf.
    ElseIf,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif

 protected:
  // For InternalIfEmitter.
  IfEmitter(BytecodeEmitter* bce, LexicalKind lexicalKind);

 public:
  explicit IfEmitter(BytecodeEmitter* bce);

  // `ifPos` is the offset in the source code for the character below:
  //
  //   if ( cond ) { ... } else if ( cond2 ) { ... }
  //   ^                        ^
  //   |                        |
  //   |                        ifPos for emitElseIf
  //   |
  //   ifPos for emitIf
  //
  // Can be Nothing() if not available.
  [[nodiscard]] bool emitIf(const mozilla::Maybe<uint32_t>& ifPos);

  [[nodiscard]] bool emitThen(
      ConditionKind conditionKind = ConditionKind::Positive);
  [[nodiscard]] bool emitThenElse(
      ConditionKind conditionKind = ConditionKind::Positive);

  [[nodiscard]] bool emitElseIf(const mozilla::Maybe<uint32_t>& ifPos);
  [[nodiscard]] bool emitElse();

  [[nodiscard]] bool emitEnd();
};

// Class for emitting bytecode for blocks like if-then-else which doesn't touch
// lexical variables.
//
// See the comments above NoLexicalAccessInBranch for more details when to use
// this instead of IfEmitter.
// Compared to IfEmitter, this class doesn't have emitIf method, given that
// it doesn't have syntactic `if`, and also the `cond` value can be already
// on the stack.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `if (cond) then_block else else_block` (effectively)
//     emit(cond);
//     InternalIfEmitter ifThenElse(this);
//     ifThenElse.emitThenElse();
//     emit(then_block);
//     ifThenElse.emitElse();
//     emit(else_block);
//     ifThenElse.emitEnd();
//
class MOZ_STACK_CLASS InternalIfEmitter : public IfEmitter {
 public:
  explicit InternalIfEmitter(
      BytecodeEmitter* bce,
      LexicalKind lexicalKind =
          BranchEmitterBase::LexicalKind::NoLexicalAccessInBranch);
};

// Class for emitting bytecode for conditional expression.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `cond ? then_expr : else_expr`
//     CondEmitter condElse(this);
//     condElse.emitCond();
//     emit(cond);
//     condElse.emitThenElse();
//     emit(then_expr);
//     condElse.emitElse();
//     emit(else_expr);
//     condElse.emitEnd();
//
class MOZ_STACK_CLASS CondEmitter : public BranchEmitterBase {
#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+ emitCond +------+ emitThenElse +----------+
  // | Start |--------->| Cond |------------->| ThenElse |-+
  // +-------+          +------+              +----------+ |
  //                                                       |
  //                                     +-----------------+
  //                                     |
  //                                     | emitElse +------+ emitEnd +-----+
  //                                     +--------->| Else |-------->| End |
  //                                                +------+         +-----+
  enum class State {
    // The initial state.
    Start,

    // After calling emitCond.
    Cond,

    // After calling emitThenElse.
    ThenElse,

    // After calling emitElse.
    Else,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif

 public:
  explicit CondEmitter(BytecodeEmitter* bce);

  [[nodiscard]] bool emitCond();
  [[nodiscard]] bool emitThenElse(
      ConditionKind conditionKind = ConditionKind::Positive);
  [[nodiscard]] bool emitElse();
  [[nodiscard]] bool emitEnd();
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_IfEmitter_h */
