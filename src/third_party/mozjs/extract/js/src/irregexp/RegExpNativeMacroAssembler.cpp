/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "irregexp/imported/regexp-macro-assembler-arch.h"
#include "irregexp/imported/regexp-stack.h"
#include "irregexp/imported/special-case.h"
#include "jit/Linker.h"
#include "vm/MatchPairs.h"
#include "vm/Realm.h"

#include "jit/ABIFunctionList-inl.h"
#include "jit/MacroAssembler-inl.h"

namespace v8 {
namespace internal {

using js::MatchPairs;
using js::jit::AbsoluteAddress;
using js::jit::Address;
using js::jit::AllocatableGeneralRegisterSet;
using js::jit::Assembler;
using js::jit::BaseIndex;
using js::jit::CodeLocationLabel;
using js::jit::GeneralRegisterBackwardIterator;
using js::jit::GeneralRegisterForwardIterator;
using js::jit::GeneralRegisterSet;
using js::jit::Imm32;
using js::jit::ImmPtr;
using js::jit::ImmWord;
using js::jit::JitCode;
using js::jit::Linker;
using js::jit::LiveGeneralRegisterSet;
using js::jit::Register;
using js::jit::Registers;
using js::jit::StackMacroAssembler;

SMRegExpMacroAssembler::SMRegExpMacroAssembler(JSContext* cx,
                                               StackMacroAssembler& masm,
                                               Zone* zone, Mode mode,
                                               uint32_t num_capture_registers)
    : NativeRegExpMacroAssembler(cx->isolate.ref(), zone),
      cx_(cx),
      masm_(masm),
      mode_(mode),
      num_registers_(num_capture_registers),
      num_capture_registers_(num_capture_registers) {
  // Each capture has a start and an end register
  MOZ_ASSERT(num_capture_registers_ % 2 == 0);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());

  temp0_ = regs.takeAny();
  temp1_ = regs.takeAny();
  temp2_ = regs.takeAny();
  input_end_pointer_ = regs.takeAny();
  current_character_ = regs.takeAny();
  current_position_ = regs.takeAny();
  backtrack_stack_pointer_ = regs.takeAny();
  savedRegisters_ = js::jit::SavedNonVolatileRegisters(regs);

  masm_.jump(&entry_label_);  // We'll generate the entry code later
  masm_.bind(&start_label_);  // and continue from here.
}

int SMRegExpMacroAssembler::stack_limit_slack() {
  return RegExpStack::kStackLimitSlack;
}

void SMRegExpMacroAssembler::AdvanceCurrentPosition(int by) {
  if (by != 0) {
    masm_.addPtr(Imm32(by * char_size()), current_position_);
  }
}

void SMRegExpMacroAssembler::AdvanceRegister(int reg, int by) {
  MOZ_ASSERT(reg >= 0 && reg < num_registers_);
  if (by != 0) {
    masm_.addPtr(Imm32(by), register_location(reg));
  }
}

void SMRegExpMacroAssembler::Backtrack() {
#ifdef DEBUG
  js::jit::Label bailOut;
  // Check for simulating interrupt
  masm_.branch32(Assembler::NotEqual,
                 AbsoluteAddress(&cx_->isolate->shouldSimulateInterrupt_),
                 Imm32(0), &bailOut);
#endif
  // Check for an interrupt. We have to restart from the beginning if we
  // are interrupted, so we only check for urgent interrupts.
  js::jit::Label noInterrupt;
  masm_.branchTest32(
      Assembler::Zero, AbsoluteAddress(cx_->addressOfInterruptBits()),
      Imm32(uint32_t(js::InterruptReason::CallbackUrgent)), &noInterrupt);
#ifdef DEBUG
  // bailing out if we have simulating interrupt flag set
  masm_.bind(&bailOut);
#endif
  masm_.movePtr(ImmWord(js::RegExpRunStatus_Error), temp0_);
  masm_.jump(&exit_label_);
  masm_.bind(&noInterrupt);

  // Pop code location from backtrack stack and jump to location.
  Pop(temp0_);
  masm_.jump(temp0_);
}

void SMRegExpMacroAssembler::Bind(Label* label) {
  masm_.bind(label->inner());
  if (label->patchOffset_.bound()) {
    AddLabelPatch(label->patchOffset_, label->pos());
  }
}

// Check if current_position + cp_offset is the input start
void SMRegExpMacroAssembler::CheckAtStartImpl(int cp_offset, Label* on_cond,
                                              Assembler::Condition cond) {
  Address addr(current_position_, cp_offset * char_size());
  masm_.computeEffectiveAddress(addr, temp0_);

  masm_.branchPtr(cond, inputStart(), temp0_, LabelOrBacktrack(on_cond));
}

void SMRegExpMacroAssembler::CheckAtStart(int cp_offset, Label* on_at_start) {
  CheckAtStartImpl(cp_offset, on_at_start, Assembler::Equal);
}

void SMRegExpMacroAssembler::CheckNotAtStart(int cp_offset,
                                             Label* on_not_at_start) {
  CheckAtStartImpl(cp_offset, on_not_at_start, Assembler::NotEqual);
}

void SMRegExpMacroAssembler::CheckCharacterImpl(Imm32 c, Label* on_cond,
                                                Assembler::Condition cond) {
  masm_.branch32(cond, current_character_, c, LabelOrBacktrack(on_cond));
}

void SMRegExpMacroAssembler::CheckCharacter(uint32_t c, Label* on_equal) {
  CheckCharacterImpl(Imm32(c), on_equal, Assembler::Equal);
}

void SMRegExpMacroAssembler::CheckNotCharacter(uint32_t c,
                                               Label* on_not_equal) {
  CheckCharacterImpl(Imm32(c), on_not_equal, Assembler::NotEqual);
}

void SMRegExpMacroAssembler::CheckCharacterGT(uc16 c, Label* on_greater) {
  CheckCharacterImpl(Imm32(c), on_greater, Assembler::GreaterThan);
}

void SMRegExpMacroAssembler::CheckCharacterLT(uc16 c, Label* on_less) {
  CheckCharacterImpl(Imm32(c), on_less, Assembler::LessThan);
}

// Bitwise-and the current character with mask and then check for a
// match with c.
void SMRegExpMacroAssembler::CheckCharacterAfterAndImpl(uint32_t c,
                                                        uint32_t mask,
                                                        Label* on_cond,
                                                        bool is_not) {
  if (c == 0) {
    Assembler::Condition cond = is_not ? Assembler::NonZero : Assembler::Zero;
    masm_.branchTest32(cond, current_character_, Imm32(mask),
                       LabelOrBacktrack(on_cond));
  } else {
    Assembler::Condition cond = is_not ? Assembler::NotEqual : Assembler::Equal;
    masm_.move32(Imm32(mask), temp0_);
    masm_.and32(current_character_, temp0_);
    masm_.branch32(cond, temp0_, Imm32(c), LabelOrBacktrack(on_cond));
  }
}

void SMRegExpMacroAssembler::CheckCharacterAfterAnd(uint32_t c, uint32_t mask,
                                                    Label* on_equal) {
  CheckCharacterAfterAndImpl(c, mask, on_equal, /*is_not =*/false);
}

void SMRegExpMacroAssembler::CheckNotCharacterAfterAnd(uint32_t c,
                                                       uint32_t mask,
                                                       Label* on_not_equal) {
  CheckCharacterAfterAndImpl(c, mask, on_not_equal, /*is_not =*/true);
}

// Subtract minus from the current character, then bitwise-and the
// result with mask, then check for a match with c.
void SMRegExpMacroAssembler::CheckNotCharacterAfterMinusAnd(
    uc16 c, uc16 minus, uc16 mask, Label* on_not_equal) {
  masm_.computeEffectiveAddress(Address(current_character_, -minus), temp0_);
  if (c == 0) {
    masm_.branchTest32(Assembler::NonZero, temp0_, Imm32(mask),
                       LabelOrBacktrack(on_not_equal));
  } else {
    masm_.and32(Imm32(mask), temp0_);
    masm_.branch32(Assembler::NotEqual, temp0_, Imm32(c),
                   LabelOrBacktrack(on_not_equal));
  }
}

// If the current position matches the position stored on top of the backtrack
// stack, pops the backtrack stack and branches to the given label.
void SMRegExpMacroAssembler::CheckGreedyLoop(Label* on_equal) {
  js::jit::Label fallthrough;
  masm_.branchPtr(Assembler::NotEqual, Address(backtrack_stack_pointer_, 0),
                  current_position_, &fallthrough);
  masm_.addPtr(Imm32(sizeof(void*)), backtrack_stack_pointer_);  // Pop.
  JumpOrBacktrack(on_equal);
  masm_.bind(&fallthrough);
}

void SMRegExpMacroAssembler::CheckCharacterInRangeImpl(
    uc16 from, uc16 to, Label* on_cond, Assembler::Condition cond) {
  // x is in [from,to] if unsigned(x - from) <= to - from
  masm_.computeEffectiveAddress(Address(current_character_, -from), temp0_);
  masm_.branch32(cond, temp0_, Imm32(to - from), LabelOrBacktrack(on_cond));
}

void SMRegExpMacroAssembler::CheckCharacterInRange(uc16 from, uc16 to,
                                                   Label* on_in_range) {
  CheckCharacterInRangeImpl(from, to, on_in_range, Assembler::BelowOrEqual);
}

void SMRegExpMacroAssembler::CheckCharacterNotInRange(uc16 from, uc16 to,
                                                      Label* on_not_in_range) {
  CheckCharacterInRangeImpl(from, to, on_not_in_range, Assembler::Above);
}

void SMRegExpMacroAssembler::CheckBitInTable(Handle<ByteArray> table,
                                             Label* on_bit_set) {
  // Claim ownership of the ByteArray from the current HandleScope.
  // ByteArrays are allocated on the C++ heap and are (eventually)
  // owned by the RegExpShared.
  PseudoHandle<ByteArrayData> rawTable = table->takeOwnership(isolate());

  masm_.movePtr(ImmPtr(rawTable->data()), temp0_);

  masm_.move32(Imm32(kTableMask), temp1_);
  masm_.and32(current_character_, temp1_);

  masm_.load8ZeroExtend(BaseIndex(temp0_, temp1_, js::jit::TimesOne), temp0_);
  masm_.branchTest32(Assembler::NonZero, temp0_, temp0_,
                     LabelOrBacktrack(on_bit_set));

  // Transfer ownership of |rawTable| to the |tables_| vector.
  AddTable(std::move(rawTable));
}

void SMRegExpMacroAssembler::CheckNotBackReferenceImpl(int start_reg,
                                                       bool read_backward,
                                                       bool unicode,
                                                       Label* on_no_match,
                                                       bool ignore_case) {
  js::jit::Label fallthrough;

  // Captures are stored as a sequential pair of registers.
  // Find the length of the back-referenced capture and load the
  // capture's start index into current_character_.
  masm_.loadPtr(register_location(start_reg),  // index of start
                current_character_);
  masm_.loadPtr(register_location(start_reg + 1), temp0_);  // index of end
  masm_.subPtr(current_character_, temp0_);                 // length of capture

  // Capture registers are either both set or both cleared.
  // If the capture length is zero, then the capture is either empty or cleared.
  // Fall through in both cases.
  masm_.branchPtr(Assembler::Equal, temp0_, ImmWord(0), &fallthrough);

  // Check that there are sufficient characters left in the input.
  if (read_backward) {
    // If start + len > current, there isn't enough room for a
    // lookbehind backreference.
    masm_.loadPtr(inputStart(), temp1_);
    masm_.addPtr(temp0_, temp1_);
    masm_.branchPtr(Assembler::GreaterThan, temp1_, current_position_,
                    LabelOrBacktrack(on_no_match));
  } else {
    // current_position_ is the negative offset from the end.
    // If current + len > 0, there isn't enough room for a backreference.
    masm_.movePtr(current_position_, temp1_);
    masm_.addPtr(temp0_, temp1_);
    masm_.branchPtr(Assembler::GreaterThan, temp1_, ImmWord(0),
                    LabelOrBacktrack(on_no_match));
  }

  if (mode_ == UC16 && ignore_case) {
    // We call a helper function for case-insensitive non-latin1 strings.

    // Save volatile regs. temp1_, temp2_, and current_character_
    // don't need to be saved.  current_position_ needs to be saved
    // even if it's non-volatile, because we modify it to use as an argument.
    LiveGeneralRegisterSet volatileRegs(GeneralRegisterSet::Volatile());
    volatileRegs.addUnchecked(current_position_);
    volatileRegs.takeUnchecked(temp1_);
    volatileRegs.takeUnchecked(temp2_);
    volatileRegs.takeUnchecked(current_character_);
    masm_.PushRegsInMask(volatileRegs);

    // Parameters are
    //   Address captured - Address of captured substring's start.
    //   Address current - Address of current character position.
    //   size_t byte_length - length of capture (in bytes)

    // Compute |captured|
    masm_.addPtr(input_end_pointer_, current_character_);

    // Compute |current|
    masm_.addPtr(input_end_pointer_, current_position_);
    if (read_backward) {
      // Offset by length when matching backwards.
      masm_.subPtr(temp0_, current_position_);
    }

    using Fn = uint32_t (*)(const char16_t*, const char16_t*, size_t);
    masm_.setupUnalignedABICall(temp1_);
    masm_.passABIArg(current_character_);
    masm_.passABIArg(current_position_);
    masm_.passABIArg(temp0_);

    if (unicode) {
      masm_.callWithABI<Fn, ::js::irregexp::CaseInsensitiveCompareUnicode>();
    } else {
      masm_.callWithABI<Fn, ::js::irregexp::CaseInsensitiveCompareNonUnicode>();
    }
    masm_.storeCallInt32Result(temp1_);
    masm_.PopRegsInMask(volatileRegs);
    masm_.branchTest32(Assembler::Zero, temp1_, temp1_,
                       LabelOrBacktrack(on_no_match));

    // On success, advance position by length of capture
    if (read_backward) {
      masm_.subPtr(temp0_, current_position_);
    } else {
      masm_.addPtr(temp0_, current_position_);
    }

    masm_.bind(&fallthrough);
    return;
  }

  // We will be modifying current_position_. Save it in case the match fails.
  masm_.push(current_position_);

  // Compute start of capture string
  masm_.addPtr(input_end_pointer_, current_character_);

  // Compute start of match string
  masm_.addPtr(input_end_pointer_, current_position_);
  if (read_backward) {
    // Offset by length when matching backwards.
    masm_.subPtr(temp0_, current_position_);
  }

  // Compute end of match string
  masm_.addPtr(current_position_, temp0_);

  js::jit::Label success;
  js::jit::Label fail;
  js::jit::Label loop;
  masm_.bind(&loop);

  // Load next character from each string.
  if (mode_ == LATIN1) {
    masm_.load8ZeroExtend(Address(current_character_, 0), temp1_);
    masm_.load8ZeroExtend(Address(current_position_, 0), temp2_);
  } else {
    masm_.load16ZeroExtend(Address(current_character_, 0), temp1_);
    masm_.load16ZeroExtend(Address(current_position_, 0), temp2_);
  }

  if (ignore_case) {
    MOZ_ASSERT(mode_ == LATIN1);
    // Try exact match.
    js::jit::Label loop_increment;
    masm_.branch32(Assembler::Equal, temp1_, temp2_, &loop_increment);

    // Mismatch. Try case-insensitive match.
    // Force the capture character to lower case (by setting bit 0x20)
    // then check to see if it is a letter.
    js::jit::Label convert_match;
    masm_.or32(Imm32(0x20), temp1_);

    // Check if it is in [a,z].
    masm_.computeEffectiveAddress(Address(temp1_, -'a'), temp2_);
    masm_.branch32(Assembler::BelowOrEqual, temp2_, Imm32('z' - 'a'),
                   &convert_match);
    // Check for values in range [224,254].
    // Exclude 247 (U+00F7 DIVISION SIGN).
    masm_.sub32(Imm32(224 - 'a'), temp2_);
    masm_.branch32(Assembler::Above, temp2_, Imm32(254 - 224), &fail);
    masm_.branch32(Assembler::Equal, temp2_, Imm32(247 - 224), &fail);

    // Capture character is lower case. Convert match character
    // to lower case and compare.
    masm_.bind(&convert_match);
    masm_.load8ZeroExtend(Address(current_position_, 0), temp2_);
    masm_.or32(Imm32(0x20), temp2_);
    masm_.branch32(Assembler::NotEqual, temp1_, temp2_, &fail);

    masm_.bind(&loop_increment);
  } else {
    // Fail if characters do not match.
    masm_.branch32(Assembler::NotEqual, temp1_, temp2_, &fail);
  }

  // Increment pointers into match and capture strings.
  masm_.addPtr(Imm32(char_size()), current_character_);
  masm_.addPtr(Imm32(char_size()), current_position_);

  // Loop if we have not reached the end of the match string.
  masm_.branchPtr(Assembler::Below, current_position_, temp0_, &loop);
  masm_.jump(&success);

  // If we fail, restore current_position_ and branch.
  masm_.bind(&fail);
  masm_.pop(current_position_);
  JumpOrBacktrack(on_no_match);

  masm_.bind(&success);

  // Drop saved value of current_position_
  masm_.addToStackPtr(Imm32(sizeof(uintptr_t)));

  // current_position_ is a pointer. Convert it back to an offset.
  masm_.subPtr(input_end_pointer_, current_position_);
  if (read_backward) {
    // Subtract match length if we matched backward
    masm_.addPtr(register_location(start_reg), current_position_);
    masm_.subPtr(register_location(start_reg + 1), current_position_);
  }

  masm_.bind(&fallthrough);
}

// Branch if a back-reference does not match a previous capture.
void SMRegExpMacroAssembler::CheckNotBackReference(int start_reg,
                                                   bool read_backward,
                                                   Label* on_no_match) {
  CheckNotBackReferenceImpl(start_reg, read_backward, /*unicode = */ false,
                            on_no_match, /*ignore_case = */ false);
}

void SMRegExpMacroAssembler::CheckNotBackReferenceIgnoreCase(
    int start_reg, bool read_backward, bool unicode, Label* on_no_match) {
  CheckNotBackReferenceImpl(start_reg, read_backward, unicode, on_no_match,
                            /*ignore_case = */ true);
}

// Checks whether the given offset from the current position is
// inside the input string.
void SMRegExpMacroAssembler::CheckPosition(int cp_offset,
                                           Label* on_outside_input) {
  // Note: current_position_ is a (negative) byte offset relative to
  // the end of the input string.
  if (cp_offset >= 0) {
    //      end + current + offset >= end
    // <=>        current + offset >= 0
    // <=>        current          >= -offset
    masm_.branchPtr(Assembler::GreaterThanOrEqual, current_position_,
                    ImmWord(-cp_offset * char_size()),
                    LabelOrBacktrack(on_outside_input));
  } else {
    // Compute offset position
    masm_.computeEffectiveAddress(
        Address(current_position_, cp_offset * char_size()), temp0_);

    // Compare to start of input.
    masm_.branchPtr(Assembler::GreaterThan, inputStart(), temp0_,
                    LabelOrBacktrack(on_outside_input));
  }
}

// This function attempts to generate special case code for character classes.
// Returns true if a special case is generated.
// Otherwise returns false and generates no code.
bool SMRegExpMacroAssembler::CheckSpecialCharacterClass(uc16 type,
                                                        Label* on_no_match) {
  js::jit::Label* no_match = LabelOrBacktrack(on_no_match);

  // Note: throughout this function, range checks (c in [min, max])
  // are implemented by an unsigned (c - min) <= (max - min) check.
  switch (type) {
    case 's': {
      // Match space-characters
      if (mode_ != LATIN1) {
        return false;
      }
      js::jit::Label success;
      // One byte space characters are ' ', '\t'..'\r', and '\u00a0' (NBSP).

      // Check ' '
      masm_.branch32(Assembler::Equal, current_character_, Imm32(' '),
                     &success);

      // Check '\t'..'\r'
      masm_.computeEffectiveAddress(Address(current_character_, -'\t'), temp0_);
      masm_.branch32(Assembler::BelowOrEqual, temp0_, Imm32('\r' - '\t'),
                     &success);

      // Check \u00a0.
      masm_.branch32(Assembler::NotEqual, temp0_, Imm32(0x00a0 - '\t'),
                     no_match);

      masm_.bind(&success);
      return true;
    }
    case 'S':
      // The emitted code for generic character classes is good enough.
      return false;
    case 'd':
      // Match latin1 digits ('0'-'9')
      masm_.computeEffectiveAddress(Address(current_character_, -'0'), temp0_);
      masm_.branch32(Assembler::Above, temp0_, Imm32('9' - '0'), no_match);
      return true;
    case 'D':
      // Match anything except latin1 digits ('0'-'9')
      masm_.computeEffectiveAddress(Address(current_character_, -'0'), temp0_);
      masm_.branch32(Assembler::BelowOrEqual, temp0_, Imm32('9' - '0'),
                     no_match);
      return true;
    case '.':
      // Match non-newlines. This excludes '\n' (0x0a), '\r' (0x0d),
      // U+2028 LINE SEPARATOR, and U+2029 PARAGRAPH SEPARATOR.
      // See https://tc39.es/ecma262/#prod-LineTerminator

      // To test for 0x0a and 0x0d efficiently, we XOR the input with 1.
      // This converts 0x0a to 0x0b, and 0x0d to 0x0c, allowing us to
      // test for the contiguous range 0x0b..0x0c.
      masm_.move32(current_character_, temp0_);
      masm_.xor32(Imm32(0x01), temp0_);
      masm_.sub32(Imm32(0x0b), temp0_);
      masm_.branch32(Assembler::BelowOrEqual, temp0_, Imm32(0x0c - 0x0b),
                     no_match);

      if (mode_ == UC16) {
        // Compare original value to 0x2028 and 0x2029, using the already
        // computed (current_char ^ 0x01 - 0x0b). I.e., check for
        // 0x201d (0x2028 - 0x0b) or 0x201e.
        masm_.sub32(Imm32(0x2028 - 0x0b), temp0_);
        masm_.branch32(Assembler::BelowOrEqual, temp0_, Imm32(0x2029 - 0x2028),
                       no_match);
      }
      return true;
    case 'w':
      // \w matches the set of 63 characters defined in Runtime Semantics:
      // WordCharacters. We use a static lookup table, which is defined in
      // regexp-macro-assembler.cc.
      // Note: if both Unicode and IgnoreCase are true, \w matches a
      // larger set of characters. That case is handled elsewhere.
      if (mode_ != LATIN1) {
        masm_.branch32(Assembler::Above, current_character_, Imm32('z'),
                       no_match);
      }
      static_assert(arraysize(word_character_map) > unibrow::Latin1::kMaxChar);
      masm_.movePtr(ImmPtr(word_character_map), temp0_);
      masm_.load8ZeroExtend(
          BaseIndex(temp0_, current_character_, js::jit::TimesOne), temp0_);
      masm_.branchTest32(Assembler::Zero, temp0_, temp0_, no_match);
      return true;
    case 'W': {
      // See 'w' above.
      js::jit::Label done;
      if (mode_ != LATIN1) {
        masm_.branch32(Assembler::Above, current_character_, Imm32('z'), &done);
      }
      static_assert(arraysize(word_character_map) > unibrow::Latin1::kMaxChar);
      masm_.movePtr(ImmPtr(word_character_map), temp0_);
      masm_.load8ZeroExtend(
          BaseIndex(temp0_, current_character_, js::jit::TimesOne), temp0_);
      masm_.branchTest32(Assembler::NonZero, temp0_, temp0_, no_match);
      if (mode_ != LATIN1) {
        masm_.bind(&done);
      }
      return true;
    }
      ////////////////////////////////////////////////////////////////////////
      // Non-standard classes (with no syntactic shorthand) used internally //
      ////////////////////////////////////////////////////////////////////////
    case '*':
      // Match any character
      return true;
    case 'n':
      // Match newlines. The opposite of '.'. See '.' above.
      masm_.move32(current_character_, temp0_);
      masm_.xor32(Imm32(0x01), temp0_);
      masm_.sub32(Imm32(0x0b), temp0_);
      if (mode_ == LATIN1) {
        masm_.branch32(Assembler::Above, temp0_, Imm32(0x0c - 0x0b), no_match);
      } else {
        MOZ_ASSERT(mode_ == UC16);
        js::jit::Label done;
        masm_.branch32(Assembler::BelowOrEqual, temp0_, Imm32(0x0c - 0x0b),
                       &done);

        // Compare original value to 0x2028 and 0x2029, using the already
        // computed (current_char ^ 0x01 - 0x0b). I.e., check for
        // 0x201d (0x2028 - 0x0b) or 0x201e.
        masm_.sub32(Imm32(0x2028 - 0x0b), temp0_);
        masm_.branch32(Assembler::Above, temp0_, Imm32(0x2029 - 0x2028),
                       no_match);
        masm_.bind(&done);
      }
      return true;

      // No custom implementation
    default:
      return false;
  }
}

void SMRegExpMacroAssembler::Fail() {
  masm_.movePtr(ImmWord(js::RegExpRunStatus_Success_NotFound), temp0_);
  masm_.jump(&exit_label_);
}

void SMRegExpMacroAssembler::GoTo(Label* to) {
  masm_.jump(LabelOrBacktrack(to));
}

void SMRegExpMacroAssembler::IfRegisterGE(int reg, int comparand,
                                          Label* if_ge) {
  masm_.branchPtr(Assembler::GreaterThanOrEqual, register_location(reg),
                  ImmWord(comparand), LabelOrBacktrack(if_ge));
}

void SMRegExpMacroAssembler::IfRegisterLT(int reg, int comparand,
                                          Label* if_lt) {
  masm_.branchPtr(Assembler::LessThan, register_location(reg),
                  ImmWord(comparand), LabelOrBacktrack(if_lt));
}

void SMRegExpMacroAssembler::IfRegisterEqPos(int reg, Label* if_eq) {
  masm_.branchPtr(Assembler::Equal, register_location(reg), current_position_,
                  LabelOrBacktrack(if_eq));
}

// This is a word-for-word identical copy of the V8 code, which is
// duplicated in at least nine different places in V8 (one per
// supported architecture) with no differences outside of comments and
// formatting. It should be hoisted into the superclass. Once that is
// done upstream, this version can be deleted.
void SMRegExpMacroAssembler::LoadCurrentCharacterImpl(int cp_offset,
                                                      Label* on_end_of_input,
                                                      bool check_bounds,
                                                      int characters,
                                                      int eats_at_least) {
  // It's possible to preload a small number of characters when each success
  // path requires a large number of characters, but not the reverse.
  MOZ_ASSERT(eats_at_least >= characters);
  MOZ_ASSERT(cp_offset < (1 << 30));  // Be sane! (And ensure negation works)

  if (check_bounds) {
    if (cp_offset >= 0) {
      CheckPosition(cp_offset + eats_at_least - 1, on_end_of_input);
    } else {
      CheckPosition(cp_offset, on_end_of_input);
    }
  }
  LoadCurrentCharacterUnchecked(cp_offset, characters);
}

// Load the character (or characters) at the specified offset from the
// current position. Zero-extend to 32 bits.
void SMRegExpMacroAssembler::LoadCurrentCharacterUnchecked(int cp_offset,
                                                           int characters) {
  BaseIndex address(input_end_pointer_, current_position_, js::jit::TimesOne,
                    cp_offset * char_size());
  if (mode_ == LATIN1) {
    if (characters == 4) {
      masm_.load32(address, current_character_);
    } else if (characters == 2) {
      masm_.load16ZeroExtend(address, current_character_);
    } else {
      MOZ_ASSERT(characters == 1);
      masm_.load8ZeroExtend(address, current_character_);
    }
  } else {
    MOZ_ASSERT(mode_ == UC16);
    if (characters == 2) {
      masm_.load32(address, current_character_);
    } else {
      MOZ_ASSERT(characters == 1);
      masm_.load16ZeroExtend(address, current_character_);
    }
  }
}

void SMRegExpMacroAssembler::PopCurrentPosition() { Pop(current_position_); }

void SMRegExpMacroAssembler::PopRegister(int register_index) {
  Pop(temp0_);
  masm_.storePtr(temp0_, register_location(register_index));
}

void SMRegExpMacroAssembler::PushBacktrack(Label* label) {
  MOZ_ASSERT(!label->is_bound());
  MOZ_ASSERT(!label->patchOffset_.bound());
  label->patchOffset_ = masm_.movWithPatch(ImmPtr(nullptr), temp0_);
  MOZ_ASSERT(label->patchOffset_.bound());

  Push(temp0_);

  CheckBacktrackStackLimit();
}

void SMRegExpMacroAssembler::PushCurrentPosition() { Push(current_position_); }

void SMRegExpMacroAssembler::PushRegister(int register_index,
                                          StackCheckFlag check_stack_limit) {
  masm_.loadPtr(register_location(register_index), temp0_);
  Push(temp0_);
  if (check_stack_limit) {
    CheckBacktrackStackLimit();
  }
}

void SMRegExpMacroAssembler::ReadCurrentPositionFromRegister(int reg) {
  masm_.loadPtr(register_location(reg), current_position_);
}

void SMRegExpMacroAssembler::WriteCurrentPositionToRegister(int reg,
                                                            int cp_offset) {
  if (cp_offset == 0) {
    masm_.storePtr(current_position_, register_location(reg));
  } else {
    Address addr(current_position_, cp_offset * char_size());
    masm_.computeEffectiveAddress(addr, temp0_);
    masm_.storePtr(temp0_, register_location(reg));
  }
}

// Note: The backtrack stack pointer is stored in a register as an
// offset from the stack top, not as a bare pointer, so that it is not
// corrupted if the backtrack stack grows (and therefore moves).
void SMRegExpMacroAssembler::ReadStackPointerFromRegister(int reg) {
  masm_.loadPtr(register_location(reg), backtrack_stack_pointer_);
  masm_.addPtr(backtrackStackBase(), backtrack_stack_pointer_);
}
void SMRegExpMacroAssembler::WriteStackPointerToRegister(int reg) {
  masm_.movePtr(backtrack_stack_pointer_, temp0_);
  masm_.subPtr(backtrackStackBase(), temp0_);
  masm_.storePtr(temp0_, register_location(reg));
}

// When matching a regexp that is anchored at the end, this operation
// is used to try skipping the beginning of long strings. If the
// maximum length of a match is less than the length of the string, we
// can skip the initial len - max_len bytes.
void SMRegExpMacroAssembler::SetCurrentPositionFromEnd(int by) {
  js::jit::Label after_position;
  masm_.branchPtr(Assembler::GreaterThanOrEqual, current_position_,
                  ImmWord(-by * char_size()), &after_position);
  masm_.movePtr(ImmWord(-by * char_size()), current_position_);

  // On RegExp code entry (where this operation is used), the character before
  // the current position is expected to be already loaded.
  // We have advanced the position, so it's safe to read backwards.
  LoadCurrentCharacterUnchecked(-1, 1);
  masm_.bind(&after_position);
}

void SMRegExpMacroAssembler::SetRegister(int register_index, int to) {
  MOZ_ASSERT(register_index >= num_capture_registers_);
  masm_.storePtr(ImmWord(to), register_location(register_index));
}

// Returns true if a regexp match can be restarted (aka the regexp is global).
// The return value is not used anywhere, but we implement it to be safe.
bool SMRegExpMacroAssembler::Succeed() {
  masm_.jump(&success_label_);
  return global();
}

// Capture registers are initialized to input[-1]
void SMRegExpMacroAssembler::ClearRegisters(int reg_from, int reg_to) {
  MOZ_ASSERT(reg_from <= reg_to);
  masm_.loadPtr(inputStart(), temp0_);
  masm_.subPtr(Imm32(char_size()), temp0_);
  for (int reg = reg_from; reg <= reg_to; reg++) {
    masm_.storePtr(temp0_, register_location(reg));
  }
}

void SMRegExpMacroAssembler::Push(Register source) {
  MOZ_ASSERT(source != backtrack_stack_pointer_);

  masm_.subPtr(Imm32(sizeof(void*)), backtrack_stack_pointer_);
  masm_.storePtr(source, Address(backtrack_stack_pointer_, 0));
}

void SMRegExpMacroAssembler::Pop(Register target) {
  MOZ_ASSERT(target != backtrack_stack_pointer_);

  masm_.loadPtr(Address(backtrack_stack_pointer_, 0), target);
  masm_.addPtr(Imm32(sizeof(void*)), backtrack_stack_pointer_);
}

void SMRegExpMacroAssembler::JumpOrBacktrack(Label* to) {
  if (to) {
    masm_.jump(to->inner());
  } else {
    Backtrack();
  }
}

// Generate a quick inline test for backtrack stack overflow.
// If the test fails, call an OOL handler to try growing the stack.
void SMRegExpMacroAssembler::CheckBacktrackStackLimit() {
  js::jit::Label no_stack_overflow;
  masm_.branchPtr(
      Assembler::BelowOrEqual,
      AbsoluteAddress(isolate()->regexp_stack()->limit_address_address()),
      backtrack_stack_pointer_, &no_stack_overflow);

  masm_.call(&stack_overflow_label_);

  // Exit with an exception if the call failed
  masm_.branchTest32(Assembler::Zero, temp0_, temp0_,
                     &exit_with_exception_label_);

  masm_.bind(&no_stack_overflow);
}

// This is used to sneak an OOM through the V8 layer.
static Handle<HeapObject> DummyCode() {
  return Handle<HeapObject>::fromHandleValue(JS::UndefinedHandleValue);
}

// Finalize code. This is called last, so that we know how many
// registers we need.
Handle<HeapObject> SMRegExpMacroAssembler::GetCode(Handle<String> source) {
  if (!cx_->realm()->ensureJitRealmExists(cx_)) {
    return DummyCode();
  }

  masm_.bind(&entry_label_);

  createStackFrame();
  initFrameAndRegs();

  masm_.jump(&start_label_);

  successHandler();
  exitHandler();
  backtrackHandler();
  stackOverflowHandler();

  Linker linker(masm_);
  JitCode* code = linker.newCode(cx_, js::jit::CodeKind::RegExp);
  if (!code) {
    return DummyCode();
  }

  for (LabelPatch& lp : labelPatches_) {
    Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, lp.patchOffset_),
                                       ImmPtr(code->raw() + lp.labelOffset_),
                                       ImmPtr(nullptr));
  }

  return Handle<HeapObject>(JS::PrivateGCThingValue(code), isolate());
}

/*
 * The stack will have the following structure:
 *  sp-> - FrameData
 *         - inputStart
 *         - backtrack stack base
 *         - matches
 *         - numMatches
 *       - Registers
 *         - Capture positions
 *         - Scratch registers
 *       --- frame alignment ---
 *       - Saved register area
 *       - Return address
 */
void SMRegExpMacroAssembler::createStackFrame() {
#ifdef JS_CODEGEN_ARM64
  // ARM64 communicates stack address via SP, but uses a pseudo-sp (PSP) for
  // addressing.  The register we use for PSP may however also be used by
  // calling code, and it is nonvolatile, so save it.  Do this as a special
  // case first because the generic save/restore code needs the PSP to be
  // initialized already.
  MOZ_ASSERT(js::jit::PseudoStackPointer64.Is(masm_.GetStackPointer64()));
  masm_.Str(js::jit::PseudoStackPointer64,
            vixl::MemOperand(js::jit::sp, -16, vixl::PreIndex));

  // Initialize the PSP from the SP.
  masm_.initPseudoStackPtr();
#endif

  // Push non-volatile registers which might be modified by jitcode.
  size_t pushedNonVolatileRegisters = 0;
  for (GeneralRegisterForwardIterator iter(savedRegisters_); iter.more();
       ++iter) {
    masm_.Push(*iter);
    pushedNonVolatileRegisters++;
  }

  // The pointer to InputOutputData is passed as the first argument.
  // On x86 we have to load it off the stack into temp0_.
  // On other platforms it is already in a register.
#ifdef JS_CODEGEN_X86
  Address ioDataAddr(masm_.getStackPointer(),
                     (pushedNonVolatileRegisters + 1) * sizeof(void*));
  masm_.loadPtr(ioDataAddr, temp0_);
#else
  if (js::jit::IntArgReg0 != temp0_) {
    masm_.movePtr(js::jit::IntArgReg0, temp0_);
  }
#endif

  // Start a new stack frame.
  size_t frameBytes = sizeof(FrameData) + num_registers_ * sizeof(void*);
  frameSize_ = js::jit::StackDecrementForCall(js::jit::ABIStackAlignment,
                                              masm_.framePushed(), frameBytes);
  masm_.reserveStack(frameSize_);
  masm_.checkStackAlignment();

  // Check if we have space on the stack. Use the *NoInterrupt stack limit to
  // avoid failing repeatedly when the regex code is called from Ion JIT code.
  // (See bug 1208819)
  js::jit::Label stack_ok;
  AbsoluteAddress limit_addr(cx_->addressOfJitStackLimitNoInterrupt());
  masm_.branchStackPtrRhs(Assembler::Below, limit_addr, &stack_ok);

  // There is not enough space on the stack. Exit with an exception.
  masm_.movePtr(ImmWord(js::RegExpRunStatus_Error), temp0_);
  masm_.jump(&exit_label_);

  masm_.bind(&stack_ok);
}

void SMRegExpMacroAssembler::initFrameAndRegs() {
  // At this point, an uninitialized stack frame has been created,
  // and the address of the InputOutputData is in temp0_.
  Register ioDataReg = temp0_;

  Register matchesReg = temp1_;
  masm_.loadPtr(Address(ioDataReg, offsetof(InputOutputData, matches)),
                matchesReg);

  // Initialize output registers
  masm_.loadPtr(Address(matchesReg, MatchPairs::offsetOfPairs()), temp2_);
  masm_.storePtr(temp2_, matches());
  masm_.load32(Address(matchesReg, MatchPairs::offsetOfPairCount()), temp2_);
  masm_.store32(temp2_, numMatches());

#ifdef DEBUG
  // Bounds-check numMatches.
  js::jit::Label enoughRegisters;
  masm_.branchPtr(Assembler::GreaterThanOrEqual, temp2_,
                  ImmWord(num_capture_registers_ / 2), &enoughRegisters);
  masm_.assumeUnreachable("Not enough output pairs for RegExp");
  masm_.bind(&enoughRegisters);
#endif

  // Load input start pointer.
  masm_.loadPtr(Address(ioDataReg, offsetof(InputOutputData, inputStart)),
                current_position_);

  // Load input end pointer
  masm_.loadPtr(Address(ioDataReg, offsetof(InputOutputData, inputEnd)),
                input_end_pointer_);

  // Set up input position to be negative offset from string end.
  masm_.subPtr(input_end_pointer_, current_position_);

  // Store inputStart
  masm_.storePtr(current_position_, inputStart());

  // Load start index
  Register startIndexReg = temp1_;
  masm_.loadPtr(Address(ioDataReg, offsetof(InputOutputData, startIndex)),
                startIndexReg);
  masm_.computeEffectiveAddress(
      BaseIndex(current_position_, startIndexReg, factor()), current_position_);

  // Initialize current_character_.
  // Load newline if index is at start, or previous character otherwise.
  js::jit::Label start_regexp;
  js::jit::Label load_previous_character;
  masm_.branchPtr(Assembler::NotEqual, startIndexReg, ImmWord(0),
                  &load_previous_character);
  masm_.movePtr(ImmWord('\n'), current_character_);
  masm_.jump(&start_regexp);

  masm_.bind(&load_previous_character);
  LoadCurrentCharacterUnchecked(-1, 1);
  masm_.bind(&start_regexp);

  // Initialize captured registers with inputStart - 1
  MOZ_ASSERT(num_capture_registers_ > 0);
  Register inputStartMinusOneReg = temp2_;
  masm_.loadPtr(inputStart(), inputStartMinusOneReg);
  masm_.subPtr(Imm32(char_size()), inputStartMinusOneReg);
  if (num_capture_registers_ > 8) {
    masm_.movePtr(ImmWord(register_offset(0)), temp1_);
    js::jit::Label init_loop;
    masm_.bind(&init_loop);
    masm_.storePtr(inputStartMinusOneReg, BaseIndex(masm_.getStackPointer(),
                                                    temp1_, js::jit::TimesOne));
    masm_.addPtr(ImmWord(sizeof(void*)), temp1_);
    masm_.branchPtr(Assembler::LessThan, temp1_,
                    ImmWord(register_offset(num_capture_registers_)),
                    &init_loop);
  } else {
    // Unroll the loop
    for (int i = 0; i < num_capture_registers_; i++) {
      masm_.storePtr(inputStartMinusOneReg, register_location(i));
    }
  }

  // Initialize backtrack stack pointer
  masm_.loadPtr(AbsoluteAddress(isolate()->top_of_regexp_stack()),
                backtrack_stack_pointer_);
  masm_.storePtr(backtrack_stack_pointer_, backtrackStackBase());
}

// Called when we find a match. May not be generated if we can
// determine ahead of time that a regexp cannot match: for example,
// when compiling /\u1e9e/ for latin-1 inputs.
void SMRegExpMacroAssembler::successHandler() {
  if (!success_label_.used()) {
    return;
  }
  masm_.bind(&success_label_);

  // Copy captures to the MatchPairs pointed to by the InputOutputData.
  // Captures are stored as positions, which are negative byte offsets
  // from the end of the string.  We must convert them to actual
  // indices.
  //
  // Index:        [ 0 ][ 1 ][ 2 ][ 3 ][ 4 ][ 5 ][END]
  // Pos (1-byte): [-6 ][-5 ][-4 ][-3 ][-2 ][-1 ][ 0 ] // IS = -6
  // Pos (2-byte): [-12][-10][-8 ][-6 ][-4 ][-2 ][ 0 ] // IS = -12
  //
  // To convert a position to an index, we subtract InputStart, and
  // divide the result by char_size.
  Register matchesReg = temp1_;
  masm_.loadPtr(matches(), matchesReg);

  Register inputStartReg = temp2_;
  masm_.loadPtr(inputStart(), inputStartReg);

  for (int i = 0; i < num_capture_registers_; i++) {
    masm_.loadPtr(register_location(i), temp0_);
    masm_.subPtr(inputStartReg, temp0_);
    if (mode_ == UC16) {
      masm_.rshiftPtrArithmetic(Imm32(1), temp0_);
    }
    masm_.store32(temp0_, Address(matchesReg, i * sizeof(int32_t)));
  }

  masm_.movePtr(ImmWord(js::RegExpRunStatus_Success), temp0_);
  // This falls through to the exit handler.
}

void SMRegExpMacroAssembler::exitHandler() {
  masm_.bind(&exit_label_);

  if (temp0_ != js::jit::ReturnReg) {
    masm_.movePtr(temp0_, js::jit::ReturnReg);
  }

  masm_.freeStack(frameSize_);

  // Restore registers which were saved on entry
  for (GeneralRegisterBackwardIterator iter(savedRegisters_); iter.more();
       ++iter) {
    masm_.Pop(*iter);
  }

#ifdef JS_CODEGEN_ARM64
  // Now restore the value that was in the PSP register on entry, and return.

  // Obtain the correct SP from the PSP.
  masm_.Mov(js::jit::sp, js::jit::PseudoStackPointer64);

  // Restore the saved value of the PSP register, this value is whatever the
  // caller had saved in it, not any actual SP value, and it must not be
  // overwritten subsequently.
  masm_.Ldr(js::jit::PseudoStackPointer64,
            vixl::MemOperand(js::jit::sp, 16, vixl::PostIndex));

  // Perform a plain Ret(), as abiret() will move SP <- PSP and that is wrong.
  masm_.Ret(vixl::lr);
#else
  masm_.abiret();
#endif

  if (exit_with_exception_label_.used()) {
    masm_.bind(&exit_with_exception_label_);

    // Exit with an error result to signal thrown exception
    masm_.movePtr(ImmWord(js::RegExpRunStatus_Error), temp0_);
    masm_.jump(&exit_label_);
  }
}

void SMRegExpMacroAssembler::backtrackHandler() {
  if (!backtrack_label_.used()) {
    return;
  }
  masm_.bind(&backtrack_label_);
  Backtrack();
}

void SMRegExpMacroAssembler::stackOverflowHandler() {
  if (!stack_overflow_label_.used()) {
    return;
  }

  // Called if the backtrack-stack limit has been hit.
  masm_.bind(&stack_overflow_label_);

  // Load argument
  masm_.movePtr(ImmPtr(isolate()->regexp_stack()), temp1_);

  // Save registers before calling C function
  LiveGeneralRegisterSet volatileRegs(GeneralRegisterSet::Volatile());

#ifdef JS_USE_LINK_REGISTER
  masm_.pushReturnAddress();
#endif

  // Adjust for the return address on the stack.
  size_t frameOffset = sizeof(void*);

  volatileRegs.takeUnchecked(temp0_);
  volatileRegs.takeUnchecked(temp1_);
  masm_.PushRegsInMask(volatileRegs);

  using Fn = bool (*)(RegExpStack * regexp_stack);
  masm_.setupUnalignedABICall(temp0_);
  masm_.passABIArg(temp1_);
  masm_.callWithABI<Fn, ::js::irregexp::GrowBacktrackStack>();
  masm_.storeCallBoolResult(temp0_);

  masm_.PopRegsInMask(volatileRegs);

  // If GrowBacktrackStack returned false, we have failed to grow the
  // stack, and must exit with a stack-overflow exception. Do this in
  // the caller so that the stack is adjusted by our return instruction.
  js::jit::Label overflow_return;
  masm_.branchTest32(Assembler::Zero, temp0_, temp0_, &overflow_return);

  // Otherwise, store the new backtrack stack base and recompute the new
  // top of the stack.
  Address bsbAddress(masm_.getStackPointer(),
                     offsetof(FrameData, backtrackStackBase) + frameOffset);
  masm_.subPtr(bsbAddress, backtrack_stack_pointer_);

  masm_.loadPtr(AbsoluteAddress(isolate()->top_of_regexp_stack()), temp1_);
  masm_.storePtr(temp1_, bsbAddress);
  masm_.addPtr(temp1_, backtrack_stack_pointer_);

  // Resume execution in calling code.
  masm_.bind(&overflow_return);
  masm_.ret();
}

// This is only used by tracing code.
// The return value doesn't matter.
RegExpMacroAssembler::IrregexpImplementation
SMRegExpMacroAssembler::Implementation() {
  return kBytecodeImplementation;
}

// Compare two strings in `/i` mode (ignoreCase, but not unicode).
/*static */
uint32_t SMRegExpMacroAssembler::CaseInsensitiveCompareNonUnicode(
    const char16_t* substring1, const char16_t* substring2, size_t byteLength) {
  js::AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(byteLength % sizeof(char16_t) == 0);
  size_t length = byteLength / sizeof(char16_t);

  for (size_t i = 0; i < length; i++) {
    char16_t c1 = substring1[i];
    char16_t c2 = substring2[i];
    if (c1 != c2) {
#ifdef JS_HAS_INTL_API
      // Non-unicode regexps have weird case-folding rules.
      c1 = RegExpCaseFolding::Canonicalize(c1);
      c2 = RegExpCaseFolding::Canonicalize(c2);
#else
      // If we aren't building with ICU, fall back to `/iu` mode. The only
      // differences are in corner cases.
      c1 = js::unicode::FoldCase(c1);
      c2 = js::unicode::FoldCase(c2);
#endif
      if (c1 != c2) {
        return 0;
      }
    }
  }

  return 1;
}

// Compare two strings in `/iu` mode (ignoreCase and unicode).
/*static */
uint32_t SMRegExpMacroAssembler::CaseInsensitiveCompareUnicode(
    const char16_t* substring1, const char16_t* substring2, size_t byteLength) {
  js::AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(byteLength % sizeof(char16_t) == 0);
  size_t length = byteLength / sizeof(char16_t);

  for (size_t i = 0; i < length; i++) {
    char16_t c1 = substring1[i];
    char16_t c2 = substring2[i];
    if (c1 != c2) {
      // Unicode regexps use the common and simple case-folding
      // mappings of the Unicode Character Database.
      c1 = js::unicode::FoldCase(c1);
      c2 = js::unicode::FoldCase(c2);
      if (c1 != c2) {
        return 0;
      }
    }
  }

  return 1;
}

/* static */
bool SMRegExpMacroAssembler::GrowBacktrackStack(RegExpStack* regexp_stack) {
  js::AutoUnsafeCallWithABI unsafe;
  size_t size = regexp_stack->stack_capacity();
  return !!regexp_stack->EnsureCapacity(size * 2);
}

bool SMRegExpMacroAssembler::CanReadUnaligned() {
#if defined(JS_CODEGEN_ARM)
  return !js::jit::HasAlignmentFault();
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  return false;
#else
  return true;
#endif
}

}  // namespace internal
}  // namespace v8
