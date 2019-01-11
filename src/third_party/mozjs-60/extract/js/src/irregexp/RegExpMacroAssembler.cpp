/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */

// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "irregexp/RegExpMacroAssembler.h"

#include "irregexp/RegExpBytecode.h"

using namespace js;
using namespace js::irregexp;

template <typename CharT>
int
irregexp::CaseInsensitiveCompareStrings(const CharT* substring1, const CharT* substring2,
					size_t byteLength)
{
    AutoUnsafeCallWithABI unsafe;

    MOZ_ASSERT(byteLength % sizeof(CharT) == 0);
    size_t length = byteLength / sizeof(CharT);

    for (size_t i = 0; i < length; i++) {
        char16_t c1 = substring1[i];
        char16_t c2 = substring2[i];
        if (c1 != c2) {
            c1 = unicode::ToLowerCase(c1);
            c2 = unicode::ToLowerCase(c2);
            if (c1 != c2)
                return 0;
        }
    }

    return 1;
}

template int
irregexp::CaseInsensitiveCompareStrings(const Latin1Char* substring1, const Latin1Char* substring2,
					size_t byteLength);

template int
irregexp::CaseInsensitiveCompareStrings(const char16_t* substring1, const char16_t* substring2,
					size_t byteLength);

template <typename CharT>
int
irregexp::CaseInsensitiveCompareUCStrings(const CharT* substring1, const CharT* substring2,
                                          size_t byteLength)
{
    AutoUnsafeCallWithABI unsafe;

    MOZ_ASSERT(byteLength % sizeof(CharT) == 0);
    size_t length = byteLength / sizeof(CharT);

    for (size_t i = 0; i < length; i++) {
        char16_t c1 = substring1[i];
        char16_t c2 = substring2[i];
        if (c1 != c2) {
            c1 = unicode::FoldCase(c1);
            c2 = unicode::FoldCase(c2);
            if (c1 != c2)
                return 0;
        }
    }

    return 1;
}

template int
irregexp::CaseInsensitiveCompareUCStrings(const Latin1Char* substring1,
                                          const Latin1Char* substring2,
                                          size_t byteLength);

template int
irregexp::CaseInsensitiveCompareUCStrings(const char16_t* substring1,
                                          const char16_t* substring2,
                                          size_t byteLength);

InterpretedRegExpMacroAssembler::InterpretedRegExpMacroAssembler(JSContext* cx, LifoAlloc* alloc,
                                                                 size_t numSavedRegisters)
  : RegExpMacroAssembler(cx, *alloc, numSavedRegisters),
    pc_(0),
    advance_current_start_(0),
    advance_current_offset_(0),
    advance_current_end_(kInvalidPC),
    buffer_(nullptr),
    length_(0)
{
    // The first int32 word is the number of registers.
    Emit32(0);
}

InterpretedRegExpMacroAssembler::~InterpretedRegExpMacroAssembler()
{
    js_free(buffer_);
}

RegExpCode
InterpretedRegExpMacroAssembler::GenerateCode(JSContext* cx, bool match_only)
{
    Bind(&backtrack_);
    Emit(BC_POP_BT, 0);

    // Update the number of registers.
    *(int32_t*)buffer_ = num_registers_;

    RegExpCode res;
    res.byteCode = buffer_;
    buffer_ = nullptr;
    return res;
}

void
InterpretedRegExpMacroAssembler::AdvanceCurrentPosition(int by)
{
    MOZ_ASSERT(by >= kMinCPOffset);
    MOZ_ASSERT(by <= kMaxCPOffset);
    advance_current_start_ = pc_;
    advance_current_offset_ = by;
    Emit(BC_ADVANCE_CP, by);
    advance_current_end_ = pc_;
}

void
InterpretedRegExpMacroAssembler::AdvanceRegister(int reg, int by)
{
    checkRegister(reg);
    Emit(BC_ADVANCE_REGISTER, reg);
    Emit32(by);
}

void
InterpretedRegExpMacroAssembler::Backtrack()
{
    Emit(BC_POP_BT, 0);
}

static const int32_t INVALID_OFFSET = -1;

void
InterpretedRegExpMacroAssembler::Bind(jit::Label* label)
{
    advance_current_end_ = kInvalidPC;
    MOZ_ASSERT(!label->bound());
    if (label->used()) {
        int pos = label->offset();
        MOZ_ASSERT(pos >= 0);
        do {
            int fixup = pos;
            pos = *reinterpret_cast<int32_t*>(buffer_ + fixup);
            *reinterpret_cast<uint32_t*>(buffer_ + fixup) = pc_;
        } while (pos != INVALID_OFFSET);
    }
    label->bind(pc_);
}

void
InterpretedRegExpMacroAssembler::CheckAtStart(jit::Label* on_at_start)
{
    Emit(BC_CHECK_AT_START, 0);
    EmitOrLink(on_at_start);
}

void
InterpretedRegExpMacroAssembler::CheckCharacter(unsigned c, jit::Label* on_equal)
{
    if (c > MAX_FIRST_ARG) {
        Emit(BC_CHECK_4_CHARS, 0);
        Emit32(c);
    } else {
        Emit(BC_CHECK_CHAR, c);
    }
    EmitOrLink(on_equal);
}

void
InterpretedRegExpMacroAssembler::CheckCharacterAfterAnd(unsigned c, unsigned and_with, jit::Label* on_equal)
{
    if (c > MAX_FIRST_ARG) {
        Emit(BC_AND_CHECK_4_CHARS, 0);
        Emit32(c);
    } else {
        Emit(BC_AND_CHECK_CHAR, c);
    }
    Emit32(and_with);
    EmitOrLink(on_equal);
}

void
InterpretedRegExpMacroAssembler::CheckCharacterGT(char16_t limit, jit::Label* on_greater)
{
    Emit(BC_CHECK_GT, limit);
    EmitOrLink(on_greater);
}

void
InterpretedRegExpMacroAssembler::CheckCharacterLT(char16_t limit, jit::Label* on_less)
{
    Emit(BC_CHECK_LT, limit);
    EmitOrLink(on_less);
}

void
InterpretedRegExpMacroAssembler::CheckGreedyLoop(jit::Label* on_tos_equals_current_position)
{
    Emit(BC_CHECK_GREEDY, 0);
    EmitOrLink(on_tos_equals_current_position);
}

void
InterpretedRegExpMacroAssembler::CheckNotAtStart(jit::Label* on_not_at_start)
{
    Emit(BC_CHECK_NOT_AT_START, 0);
    EmitOrLink(on_not_at_start);
}

void
InterpretedRegExpMacroAssembler::CheckNotBackReference(int start_reg, jit::Label* on_no_match)
{
    MOZ_ASSERT(start_reg >= 0);
    MOZ_ASSERT(start_reg <= kMaxRegister);
    Emit(BC_CHECK_NOT_BACK_REF, start_reg);
    EmitOrLink(on_no_match);
}

void
InterpretedRegExpMacroAssembler::CheckNotBackReferenceIgnoreCase(int start_reg,
                                                                 jit::Label* on_no_match,
                                                                 bool unicode)
{
    MOZ_ASSERT(start_reg >= 0);
    MOZ_ASSERT(start_reg <= kMaxRegister);
    if (unicode)
        Emit(BC_CHECK_NOT_BACK_REF_NO_CASE_UNICODE, start_reg);
    else
        Emit(BC_CHECK_NOT_BACK_REF_NO_CASE, start_reg);
    EmitOrLink(on_no_match);
}

void
InterpretedRegExpMacroAssembler::CheckNotCharacter(unsigned c, jit::Label* on_not_equal)
{
    if (c > MAX_FIRST_ARG) {
        Emit(BC_CHECK_NOT_4_CHARS, 0);
        Emit32(c);
    } else {
        Emit(BC_CHECK_NOT_CHAR, c);
    }
    EmitOrLink(on_not_equal);
}

void
InterpretedRegExpMacroAssembler::CheckNotCharacterAfterAnd(unsigned c, unsigned and_with,
                                                           jit::Label* on_not_equal)
{
    if (c > MAX_FIRST_ARG) {
        Emit(BC_AND_CHECK_NOT_4_CHARS, 0);
        Emit32(c);
    } else {
        Emit(BC_AND_CHECK_NOT_CHAR, c);
    }
    Emit32(and_with);
    EmitOrLink(on_not_equal);
}

void
InterpretedRegExpMacroAssembler::CheckNotCharacterAfterMinusAnd(char16_t c, char16_t minus, char16_t and_with,
                                                                jit::Label* on_not_equal)
{
    Emit(BC_MINUS_AND_CHECK_NOT_CHAR, c);
    Emit16(minus);
    Emit16(and_with);
    EmitOrLink(on_not_equal);
}

void
InterpretedRegExpMacroAssembler::CheckCharacterInRange(char16_t from, char16_t to,
                                                       jit::Label* on_in_range)
{
    Emit(BC_CHECK_CHAR_IN_RANGE, 0);
    Emit16(from);
    Emit16(to);
    EmitOrLink(on_in_range);
}

void
InterpretedRegExpMacroAssembler::CheckCharacterNotInRange(char16_t from, char16_t to,
                                                          jit::Label* on_not_in_range)
{
    Emit(BC_CHECK_CHAR_NOT_IN_RANGE, 0);
    Emit16(from);
    Emit16(to);
    EmitOrLink(on_not_in_range);
}

void
InterpretedRegExpMacroAssembler::CheckBitInTable(RegExpShared::JitCodeTable table,
                                                 jit::Label* on_bit_set)
{
    static const int kBitsPerByte = 8;

    Emit(BC_CHECK_BIT_IN_TABLE, 0);
    EmitOrLink(on_bit_set);
    for (int i = 0; i < kTableSize; i += kBitsPerByte) {
        int byte = 0;
        for (int j = 0; j < kBitsPerByte; j++) {
            if (table[i + j] != 0)
                byte |= 1 << j;
        }
        Emit8(byte);
    }
}

void
InterpretedRegExpMacroAssembler::JumpOrBacktrack(jit::Label* to)
{
    if (advance_current_end_ == pc_) {
        // Combine advance current and goto.
        pc_ = advance_current_start_;
        Emit(BC_ADVANCE_CP_AND_GOTO, advance_current_offset_);
        EmitOrLink(to);
        advance_current_end_ = kInvalidPC;
    } else {
        // Regular goto.
        Emit(BC_GOTO, 0);
        EmitOrLink(to);
    }
}

void
InterpretedRegExpMacroAssembler::Fail()
{
    Emit(BC_FAIL, 0);
}

void
InterpretedRegExpMacroAssembler::IfRegisterGE(int reg, int comparand, jit::Label* if_ge)
{
    checkRegister(reg);
    Emit(BC_CHECK_REGISTER_GE, reg);
    Emit32(comparand);
    EmitOrLink(if_ge);
}

void
InterpretedRegExpMacroAssembler::IfRegisterLT(int reg, int comparand, jit::Label* if_lt)
{
    checkRegister(reg);
    Emit(BC_CHECK_REGISTER_LT, reg);
    Emit32(comparand);
    EmitOrLink(if_lt);
}

void
InterpretedRegExpMacroAssembler::IfRegisterEqPos(int reg, jit::Label* if_eq)
{
    checkRegister(reg);
    Emit(BC_CHECK_REGISTER_EQ_POS, reg);
    EmitOrLink(if_eq);
}

void
InterpretedRegExpMacroAssembler::LoadCurrentCharacter(int cp_offset, jit::Label* on_end_of_input,
                                                      bool check_bounds, int characters)
{
    MOZ_ASSERT(cp_offset >= kMinCPOffset);
    MOZ_ASSERT(cp_offset <= kMaxCPOffset);
    int bytecode;
    if (check_bounds) {
        if (characters == 4) {
            bytecode = BC_LOAD_4_CURRENT_CHARS;
        } else if (characters == 2) {
            bytecode = BC_LOAD_2_CURRENT_CHARS;
        } else {
            MOZ_ASSERT(characters == 1);
            bytecode = BC_LOAD_CURRENT_CHAR;
        }
    } else {
        if (characters == 4) {
            bytecode = BC_LOAD_4_CURRENT_CHARS_UNCHECKED;
        } else if (characters == 2) {
            bytecode = BC_LOAD_2_CURRENT_CHARS_UNCHECKED;
        } else {
            MOZ_ASSERT(characters == 1);
            bytecode = BC_LOAD_CURRENT_CHAR_UNCHECKED;
        }
    }
    Emit(bytecode, cp_offset);
    if (check_bounds)
        EmitOrLink(on_end_of_input);
}

void
InterpretedRegExpMacroAssembler::PopCurrentPosition()
{
    Emit(BC_POP_CP, 0);
}

void
InterpretedRegExpMacroAssembler::PopRegister(int reg)
{
    checkRegister(reg);
    Emit(BC_POP_REGISTER, reg);
}

void
InterpretedRegExpMacroAssembler::PushCurrentPosition()
{
    Emit(BC_PUSH_CP, 0);
}

void
InterpretedRegExpMacroAssembler::PushRegister(int reg, StackCheckFlag check_stack_limit)
{
    checkRegister(reg);
    Emit(BC_PUSH_REGISTER, reg);
}

void
InterpretedRegExpMacroAssembler::ReadCurrentPositionFromRegister(int reg)
{
    checkRegister(reg);
    Emit(BC_SET_CP_TO_REGISTER, reg);
}

void
InterpretedRegExpMacroAssembler::ReadBacktrackStackPointerFromRegister(int reg)
{
    checkRegister(reg);
    Emit(BC_SET_SP_TO_REGISTER, reg);
}

void
InterpretedRegExpMacroAssembler::SetCurrentPositionFromEnd(int by)
{
    MOZ_ASSERT(by >= 0 && by < (1 << 24));
    Emit(BC_SET_CURRENT_POSITION_FROM_END, by);
}

void
InterpretedRegExpMacroAssembler::SetRegister(int reg, int to)
{
    checkRegister(reg);
    Emit(BC_SET_REGISTER, reg);
    Emit32(to);
}

bool
InterpretedRegExpMacroAssembler::Succeed()
{
    Emit(BC_SUCCEED, 0);

    // Restart matching for global regexp not supported.
    return false;
}

void
InterpretedRegExpMacroAssembler::WriteCurrentPositionToRegister(int reg, int cp_offset)
{
    checkRegister(reg);
    Emit(BC_SET_REGISTER_TO_CP, reg);
    Emit32(cp_offset);  // Current position offset.
}

void
InterpretedRegExpMacroAssembler::ClearRegisters(int reg_from, int reg_to)
{
    MOZ_ASSERT(reg_from <= reg_to);
    for (int reg = reg_from; reg <= reg_to; reg++)
        SetRegister(reg, -1);
}

void
InterpretedRegExpMacroAssembler::WriteBacktrackStackPointerToRegister(int reg)
{
    checkRegister(reg);
    Emit(BC_SET_REGISTER_TO_SP, reg);
}

void
InterpretedRegExpMacroAssembler::PushBacktrack(jit::Label* label)
{
    Emit(BC_PUSH_BT, 0);
    EmitOrLink(label);
}

void
InterpretedRegExpMacroAssembler::BindBacktrack(jit::Label* label)
{
    Bind(label);
}

void
InterpretedRegExpMacroAssembler::EmitOrLink(jit::Label* label)
{
    if (label == nullptr)
        label = &backtrack_;
    if (label->bound()) {
        Emit32(label->offset());
    } else {
        int pos = label->used() ? label->offset() : INVALID_OFFSET;
        label->use(pc_);
        Emit32(pos);
    }
}

void
InterpretedRegExpMacroAssembler::Emit(uint32_t byte, uint32_t twenty_four_bits)
{
    uint32_t word = ((twenty_four_bits << BYTECODE_SHIFT) | byte);
    Emit32(word);
}

void
InterpretedRegExpMacroAssembler::Emit32(uint32_t word)
{
    MOZ_ASSERT(pc_ <= length_);
    if (pc_  + 3 >= length_)
        Expand();
    *reinterpret_cast<uint32_t*>(buffer_ + pc_) = word;
    pc_ += 4;
}

void
InterpretedRegExpMacroAssembler::Emit16(uint32_t word)
{
    MOZ_ASSERT(pc_ <= length_);
    if (pc_ + 1 >= length_)
        Expand();
    *reinterpret_cast<uint16_t*>(buffer_ + pc_) = word;
    pc_ += 2;
}

void
InterpretedRegExpMacroAssembler::Emit8(uint32_t word)
{
    MOZ_ASSERT(pc_ <= length_);
    if (pc_ == length_)
        Expand();
    *reinterpret_cast<unsigned char*>(buffer_ + pc_) = word;
    pc_ += 1;
}

void
InterpretedRegExpMacroAssembler::Expand()
{
    AutoEnterOOMUnsafeRegion oomUnsafe;

    int newLength = Max(100, length_ * 2);
    if (newLength < length_ + 4)
        oomUnsafe.crash("InterpretedRegExpMacroAssembler::Expand");

    buffer_ = (uint8_t*) js_realloc(buffer_, newLength);
    if (!buffer_)
        oomUnsafe.crash("InterpretedRegExpMacroAssembler::Expand");
    length_ = newLength;
}
