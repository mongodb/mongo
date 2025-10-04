/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SwitchEmitter_h
#define frontend_SwitchEmitter_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include <stddef.h>  // size_t
#include <stdint.h>  // int32_t, uint32_t

#include "frontend/BytecodeControlStructures.h"  // BreakableControl
#include "frontend/EmitterScope.h"               // EmitterScope
#include "frontend/JumpList.h"                   // JumpList, JumpTarget
#include "frontend/TDZCheckCache.h"              // TDZCheckCache
#include "js/AllocPolicy.h"                      // SystemAllocPolicy
#include "js/Value.h"                            // JSVAL_INT_MAX, JSVAL_INT_MIN
#include "js/Vector.h"                           // Vector
#include "vm/Scope.h"                            // LexicalScope

namespace js {
namespace frontend {

struct BytecodeEmitter;

// Class for emitting bytecode for switch-case-default block.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `switch (discriminant) { case c1_expr: c1_body; }`
//     SwitchEmitter se(this);
//     se.emitDiscriminant(offset_of_switch);
//     emit(discriminant);
//
//     se.validateCaseCount(1);
//     se.emitCond();
//
//     se.prepareForCaseValue();
//     emit(c1_expr);
//     se.emitCaseJump();
//
//     se.emitCaseBody();
//     emit(c1_body);
//
//     se.emitEnd();
//
//   `switch (discriminant) { case c1_expr: c1_body; case c2_expr: c2_body;
//                            default: def_body; }`
//     SwitchEmitter se(this);
//     se.emitDiscriminant(offset_of_switch);
//     emit(discriminant);
//
//     se.validateCaseCount(2);
//     se.emitCond();
//
//     se.prepareForCaseValue();
//     emit(c1_expr);
//     se.emitCaseJump();
//
//     se.prepareForCaseValue();
//     emit(c2_expr);
//     se.emitCaseJump();
//
//     se.emitCaseBody();
//     emit(c1_body);
//
//     se.emitCaseBody();
//     emit(c2_body);
//
//     se.emitDefaultBody();
//     emit(def_body);
//
//     se.emitEnd();
//
//   `switch (discriminant) { case c1_expr: c1_body; case c2_expr: c2_body; }`
//   with Table Switch
//     SwitchEmitter::TableGenerator tableGen(this);
//     tableGen.addNumber(c1_expr_value);
//     tableGen.addNumber(c2_expr_value);
//     tableGen.finish(2);
//
//     // If `!tableGen.isValid()` here, `emitCond` should be used instead.
//
//     SwitchEmitter se(this);
//     se.emitDiscriminant(offset_of_switch);
//     emit(discriminant);
//     se.validateCaseCount(2);
//     se.emitTable(tableGen);
//
//     se.emitCaseBody(c1_expr_value, tableGen);
//     emit(c1_body);
//
//     se.emitCaseBody(c2_expr_value, tableGen);
//     emit(c2_body);
//
//     se.emitEnd();
//
//   `switch (discriminant) { case c1_expr: c1_body; case c2_expr: c2_body;
//                            default: def_body; }`
//   with Table Switch
//     SwitchEmitter::TableGenerator tableGen(bce);
//     tableGen.addNumber(c1_expr_value);
//     tableGen.addNumber(c2_expr_value);
//     tableGen.finish(2);
//
//     // If `!tableGen.isValid()` here, `emitCond` should be used instead.
//
//     SwitchEmitter se(this);
//     se.emitDiscriminant(offset_of_switch);
//     emit(discriminant);
//     se.validateCaseCount(2);
//     se.emitTable(tableGen);
//
//     se.emitCaseBody(c1_expr_value, tableGen);
//     emit(c1_body);
//
//     se.emitCaseBody(c2_expr_value, tableGen);
//     emit(c2_body);
//
//     se.emitDefaultBody();
//     emit(def_body);
//
//     se.emitEnd();
//
//   `switch (discriminant) { case c1_expr: c1_body; }`
//   in case c1_body contains lexical bindings
//     SwitchEmitter se(this);
//     se.emitDiscriminant(offset_of_switch);
//     emit(discriminant);
//
//     se.validateCaseCount(1);
//
//     se.emitLexical(bindings);
//
//     se.emitCond();
//
//     se.prepareForCaseValue();
//     emit(c1_expr);
//     se.emitCaseJump();
//
//     se.emitCaseBody();
//     emit(c1_body);
//
//     se.emitEnd();
//
//   `switch (discriminant) { case c1_expr: c1_body; }`
//   in case c1_body contains hosted functions
//     SwitchEmitter se(this);
//     se.emitDiscriminant(offset_of_switch);
//     emit(discriminant);
//
//     se.validateCaseCount(1);
//
//     se.emitLexical(bindings);
//     emit(hosted functions);
//
//     se.emitCond();
//
//     se.prepareForCaseValue();
//     emit(c1_expr);
//     se.emitCaseJump();
//
//     se.emitCaseBody();
//     emit(c1_body);
//
//     se.emitEnd();
//
class MOZ_STACK_CLASS SwitchEmitter {
  // Bytecode for each case.
  //
  // Cond Switch (uses an equality comparison for each case)
  //     {discriminant}
  //
  //     {c1_expr}
  //     JSOp::Case c1
  //
  //     JSOp::JumpTarget
  //     {c2_expr}
  //     JSOp::Case c2
  //
  //     ...
  //
  //     JSOp::JumpTarget
  //     JSOp::Default default
  //
  //   c1:
  //     JSOp::JumpTarget
  //     {c1_body}
  //     JSOp::Goto end
  //
  //   c2:
  //     JSOp::JumpTarget
  //     {c2_body}
  //     JSOp::Goto end
  //
  //   default:
  //   end:
  //     JSOp::JumpTarget
  //
  // Table Switch
  //     {discriminant}
  //     JSOp::TableSwitch c1, c2, ...
  //
  //   c1:
  //     JSOp::JumpTarget
  //     {c1_body}
  //     JSOp::Goto end
  //
  //   c2:
  //     JSOp::JumpTarget
  //     {c2_body}
  //     JSOp::Goto end
  //
  //   ...
  //
  //   end:
  //     JSOp::JumpTarget

 public:
  enum class Kind { Table, Cond };

  // Class for generating optimized table switch data.
  class MOZ_STACK_CLASS TableGenerator {
    BytecodeEmitter* bce_;

    // Bit array for given numbers.
    mozilla::Maybe<js::Vector<size_t, 128, SystemAllocPolicy>> intmap_;

    // The length of the intmap_.
    int32_t intmapBitLength_ = 0;

    // The length of the table.
    uint32_t tableLength_ = 0;

    // The lower and higher bounds of the table.
    int32_t low_ = JSVAL_INT_MAX, high_ = JSVAL_INT_MIN;

    // Whether the table is still valid.
    bool valid_ = true;

#ifdef DEBUG
    bool finished_ = false;
#endif

   public:
    explicit TableGenerator(BytecodeEmitter* bce) : bce_(bce) {}

    void setInvalid() { valid_ = false; }
    [[nodiscard]] bool isValid() const { return valid_; }
    [[nodiscard]] bool isInvalid() const { return !valid_; }

    // Add the given number to the table.  The number is the value of
    // `expr` for `case expr:` syntax.
    [[nodiscard]] bool addNumber(int32_t caseValue);

    // Finish generating the table.
    // `caseCount` should be the number of cases in the switch statement,
    // excluding the default case.
    void finish(uint32_t caseCount);

   private:
    friend SwitchEmitter;

    // The following methods can be used only after calling `finish`.

    // Returns the lower bound of the added numbers.
    int32_t low() const {
      MOZ_ASSERT(finished_);
      return low_;
    }

    // Returns the higher bound of the numbers.
    int32_t high() const {
      MOZ_ASSERT(finished_);
      return high_;
    }

    // Returns the index in SwitchEmitter.caseOffsets_ for table switch.
    uint32_t toCaseIndex(int32_t caseValue) const;

    // Returns the length of the table.
    // This method can be called only if `isValid()` is true.
    uint32_t tableLength() const;
  };

 private:
  BytecodeEmitter* bce_;

  // `kind_` should be set to the correct value in emitCond/emitTable.
  Kind kind_ = Kind::Cond;

  // True if there's explicit default case.
  bool hasDefault_ = false;

  // The number of cases in the switch statement, excluding the default case.
  uint32_t caseCount_ = 0;

  // Internal index for case jump and case body, used by cond switch.
  uint32_t caseIndex_ = 0;

  // Bytecode offset after emitting `discriminant`.
  BytecodeOffset top_;

  // Bytecode offset of the previous JSOp::Case.
  BytecodeOffset lastCaseOffset_;

  // Bytecode offset of the JSOp::JumpTarget for default body.
  JumpTarget defaultJumpTargetOffset_;

  // Bytecode offset of the JSOp::Default.
  JumpList condSwitchDefaultOffset_;

  // Instantiated when there's lexical scope for entire switch.
  mozilla::Maybe<TDZCheckCache> tdzCacheLexical_;
  mozilla::Maybe<EmitterScope> emitterScope_;

  // Instantiated while emitting case expression and case/default body.
  mozilla::Maybe<TDZCheckCache> tdzCacheCaseAndBody_;

  // Control for switch.
  mozilla::Maybe<BreakableControl> controlInfo_;

  uint32_t switchPos_ = 0;

  // Cond Switch:
  //   Offset of each JSOp::Case.
  // Table Switch:
  //   Offset of each JSOp::JumpTarget for case.
  js::Vector<BytecodeOffset, 32, SystemAllocPolicy> caseOffsets_;

  // The state of this emitter.
  //
  // +-------+ emitDiscriminant +--------------+
  // | Start |----------------->| Discriminant |-+
  // +-------+                  +--------------+ |
  //                                             |
  // +-------------------------------------------+
  // |
  // |                              validateCaseCount +-----------+
  // +->+------------------------>+------------------>| CaseCount |-+
  //    |                         ^                   +-----------+ |
  //    | emitLexical +---------+ |                                 |
  //    +------------>| Lexical |-+                                 |
  //                  +---------+                                   |
  //                                                                |
  // +--------------------------------------------------------------+
  // |
  // | emitTable +-------+
  // +---------->| Table |----------------------------------->+-+
  // |           +-------+                                    ^ |
  // |                                                        | |
  // | emitCond  +------+                                     | |
  // +---------->| Cond |-+------------------------------->+->+ |
  //             +------+ |                                ^    |
  //                      |                                |    |
  //   +------------------+                                |    |
  //   |                                                   |    |
  //   |prepareForCaseValue  +-----------+                 |    |
  //   +----------+--------->| CaseValue |                 |    |
  //              ^          +-----------+                 |    |
  //              |             |                          |    |
  //              |             | emitCaseJump +------+    |    |
  //              |             +------------->| Case |->+-+    |
  //              |                            +------+  |      |
  //              |                                      |      |
  //              +--------------------------------------+      |
  //                                                            |
  // +----------------------------------------------------------+
  // |
  // |                                              emitEnd +-----+
  // +-+----------------------------------------->+-------->| End |
  //   |                                          ^         +-----+
  //   |      emitCaseBody    +----------+        |
  //   +->+-+---------------->| CaseBody |--->+-+-+
  //      ^ |                 +----------+    ^ |
  //      | |                                 | |
  //      | | emitDefaultBody +-------------+ | |
  //      | +---------------->| DefaultBody |-+ |
  //      |                   +-------------+   |
  //      |                                     |
  //      +-------------------------------------+
  //
 protected:
  enum class State {
    // The initial state.
    Start,

    // After calling emitDiscriminant.
    Discriminant,

    // After calling validateCaseCount.
    CaseCount,

    // After calling emitLexical.
    Lexical,

    // After calling emitCond.
    Cond,

    // After calling emitTable.
    Table,

    // After calling prepareForCaseValue.
    CaseValue,

    // After calling emitCaseJump.
    Case,

    // After calling emitCaseBody.
    CaseBody,

    // After calling emitDefaultBody.
    DefaultBody,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;

 public:
  explicit SwitchEmitter(BytecodeEmitter* bce);

  // `switchPos` is the offset in the source code for the character below:
  //
  //   switch ( cond ) { ... }
  //   ^
  //   |
  //   switchPos
  [[nodiscard]] bool emitDiscriminant(uint32_t switchPos);

  // `caseCount` should be the number of cases in the switch statement,
  // excluding the default case.
  [[nodiscard]] bool validateCaseCount(uint32_t caseCount);

  // `bindings` is a lexical scope for the entire switch, in case there's
  // let/const effectively directly under case or default blocks.
  [[nodiscard]] bool emitLexical(LexicalScope::ParserData* bindings);

  [[nodiscard]] bool emitCond();
  [[nodiscard]] bool emitTable(const TableGenerator& tableGen);

  [[nodiscard]] bool prepareForCaseValue();
  [[nodiscard]] bool emitCaseJump();

  [[nodiscard]] bool emitCaseBody();
  [[nodiscard]] bool emitCaseBody(int32_t caseValue,
                                  const TableGenerator& tableGen);
  [[nodiscard]] bool emitDefaultBody();
  [[nodiscard]] bool emitEnd();

 private:
  [[nodiscard]] bool emitCaseOrDefaultJump(uint32_t caseIndex, bool isDefault);
  [[nodiscard]] bool emitImplicitDefault();
};

// Class for emitting bytecode for switch-case-default block that doesn't
// correspond to a syntactic `switch`.
// Compared to SwitchEmitter, this class doesn't require `emitDiscriminant`,
// and the discriminant can already be on the stack. Usage is otherwise
// the same as SwitchEmitter.
class MOZ_STACK_CLASS InternalSwitchEmitter : public SwitchEmitter {
 public:
  explicit InternalSwitchEmitter(BytecodeEmitter* bce);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_SwitchEmitter_h */
