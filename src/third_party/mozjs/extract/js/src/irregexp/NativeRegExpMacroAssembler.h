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

#ifndef V8_NATIVE_REGEXP_MACRO_ASSEMBLER_H_
#define V8_NATIVE_REGEXP_MACRO_ASSEMBLER_H_

#include "irregexp/RegExpMacroAssembler.h"

namespace js {
namespace irregexp {

struct InputOutputData
{
    const void* inputStart;
    const void* inputEnd;

    // Index into inputStart (in chars) at which to begin matching.
    size_t startIndex;
    size_t* endIndex;

    MatchPairs* matches;

    // RegExpMacroAssembler::Result for non-global regexps, number of captures
    // for global regexps.
    int32_t result;

    template <typename CharT>
    InputOutputData(const CharT* inputStart, const CharT* inputEnd,
                    size_t startIndex, MatchPairs* matches, size_t* endIndex)
      : inputStart(inputStart),
        inputEnd(inputEnd),
        startIndex(startIndex),
        endIndex(endIndex),
        matches(matches),
        result(0)
    {}
};

struct FrameData
{
    // Copy of the input/output data's data.
    char16_t* inputStart;
    size_t startIndex;
    size_t* endIndex;

    // Pointer to the character before the input start.
    char16_t* inputStartMinusOne;

    // Copy of the input MatchPairs registers, may be modified by JIT code.
    int32_t* outputRegisters;
    int32_t numOutputRegisters;

    int32_t successfulCaptures;

    void* backtrackStackBase;
};

class MOZ_STACK_CLASS NativeRegExpMacroAssembler final : public RegExpMacroAssembler
{
  public:
    // Type of input string to generate code for.
    enum Mode { LATIN1 = 1, CHAR16 = 2 };

    NativeRegExpMacroAssembler(JSContext* cx, LifoAlloc* alloc, Mode mode, int registers_to_save,
                               RegExpShared::JitCodeTables& tables);

    // Inherited virtual methods.
    RegExpCode GenerateCode(JSContext* cx, bool match_only) override;
    int stack_limit_slack() override;
    bool CanReadUnaligned() override;
    void AdvanceCurrentPosition(int by) override;
    void AdvanceRegister(int reg, int by) override;
    void Backtrack() override;
    void Bind(jit::Label* label) override;
    void CheckAtStart(jit::Label* on_at_start) override;
    void CheckCharacter(unsigned c, jit::Label* on_equal) override;
    void CheckCharacterAfterAnd(unsigned c, unsigned and_with, jit::Label* on_equal) override;
    void CheckCharacterGT(char16_t limit, jit::Label* on_greater) override;
    void CheckCharacterLT(char16_t limit, jit::Label* on_less) override;
    void CheckGreedyLoop(jit::Label* on_tos_equals_current_position) override;
    void CheckNotAtStart(jit::Label* on_not_at_start) override;
    void CheckNotBackReference(int start_reg, jit::Label* on_no_match) override;
    void CheckNotBackReferenceIgnoreCase(int start_reg, jit::Label* on_no_match, bool unicode) override;
    void CheckNotCharacter(unsigned c, jit::Label* on_not_equal) override;
    void CheckNotCharacterAfterAnd(unsigned c, unsigned and_with, jit::Label* on_not_equal) override;
    void CheckNotCharacterAfterMinusAnd(char16_t c, char16_t minus, char16_t and_with,
                                        jit::Label* on_not_equal) override;
    void CheckCharacterInRange(char16_t from, char16_t to,
                               jit::Label* on_in_range) override;
    void CheckCharacterNotInRange(char16_t from, char16_t to,
                                  jit::Label* on_not_in_range) override;
    void CheckBitInTable(RegExpShared::JitCodeTable table, jit::Label* on_bit_set) override;
    void CheckPosition(int cp_offset, jit::Label* on_outside_input) override;
    void JumpOrBacktrack(jit::Label* to) override;
    bool CheckSpecialCharacterClass(char16_t type, jit::Label* on_no_match) override;
    void Fail() override;
    void IfRegisterGE(int reg, int comparand, jit::Label* if_ge) override;
    void IfRegisterLT(int reg, int comparand, jit::Label* if_lt) override;
    void IfRegisterEqPos(int reg, jit::Label* if_eq) override;
    void LoadCurrentCharacter(int cp_offset, jit::Label* on_end_of_input,
                              bool check_bounds = true, int characters = 1) override;
    void PopCurrentPosition() override;
    void PopRegister(int register_index) override;
    void PushCurrentPosition() override;
    void PushRegister(int register_index, StackCheckFlag check_stack_limit) override;
    void ReadCurrentPositionFromRegister(int reg) override;
    void ReadBacktrackStackPointerFromRegister(int reg) override;
    void SetCurrentPositionFromEnd(int by) override;
    void SetRegister(int register_index, int to) override;
    bool Succeed() override;
    void WriteCurrentPositionToRegister(int reg, int cp_offset) override;
    void ClearRegisters(int reg_from, int reg_to) override;
    void WriteBacktrackStackPointerToRegister(int reg) override;
    void PushBacktrack(jit::Label* label) override;
    void BindBacktrack(jit::Label* label) override;

    // Compares two-byte strings case insensitively.
    // Called from generated RegExp code.
    static int CaseInsensitiveCompareUC16(jit::Address byte_offset1,
                                          jit::Address byte_offset2,
                                          size_t byte_length);

    // Byte map of one byte characters with a 0xff if the character is a word
    // character (digit, letter or underscore) and 0x00 otherwise.
    // Used by generated RegExp code.
    static const uint8_t word_character_map[256];

    // Byte size of chars in the string to match (decided by the Mode argument)
    inline int char_size() { return static_cast<int>(mode_); }
    inline jit::Scale factor() { return mode_ == CHAR16 ? jit::TimesTwo : jit::TimesOne; }

    jit::Label* BranchOrBacktrack(jit::Label* branch);

    // Pushes a register or constant on the backtrack stack. Decrements the
    // stack pointer by a word size and stores the register's value there.
    void PushBacktrack(jit::Register value);
    void PushBacktrack(int32_t value);

    // Pop a value from the backtrack stack.
    void PopBacktrack(jit::Register target);

    // Check whether we are exceeding the stack limit on the backtrack stack.
    void CheckBacktrackStackLimit();

    void LoadCurrentCharacterUnchecked(int cp_offset, int characters);

  private:
    jit::MacroAssembler masm;
    RegExpShared::JitCodeTables& tables;

    JSContext* cx;
    Mode mode_;
    jit::Label entry_label_;
    jit::Label start_label_;
    jit::Label backtrack_label_;
    jit::Label success_label_;
    jit::Label exit_label_;
    jit::Label stack_overflow_label_;
    jit::Label exit_with_exception_label_;

    // Set of registers which are used by the code generator, and as such which
    // are saved.
    jit::LiveGeneralRegisterSet savedNonVolatileRegisters;

    struct LabelPatch {
        // Once it is bound via BindBacktrack, |label| becomes null and
        // |labelOffset| is set.
        jit::Label* label;
        size_t labelOffset;

        jit::CodeOffset patchOffset;

        LabelPatch(jit::Label* label, jit::CodeOffset patchOffset)
          : label(label), labelOffset(0), patchOffset(patchOffset)
        {}
    };

    Vector<LabelPatch, 4, SystemAllocPolicy> labelPatches;

    // See RegExpMacroAssembler.cpp for the meaning of these registers.
    jit::Register input_end_pointer;
    jit::Register current_character;
    jit::Register current_position;
    jit::Register backtrack_stack_pointer;
    jit::Register temp0, temp1, temp2;

    // The frame_pointer-relative location of a regexp register.
    jit::Address register_location(int register_index) {
        checkRegister(register_index);
        return jit::Address(masm.getStackPointer(), register_offset(register_index));
    }

    int32_t register_offset(int register_index) {
        return sizeof(FrameData) + register_index * sizeof(void*);
    }
};

} }  // namespace js::irregexp

#endif  // V8_NATIVE_REGEXP_MACRO_ASSEMBLER_H_
