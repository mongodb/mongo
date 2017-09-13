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

#ifndef V8_REGEXP_MACRO_ASSEMBLER_H_
#define V8_REGEXP_MACRO_ASSEMBLER_H_

#include "irregexp/RegExpAST.h"
#include "irregexp/RegExpEngine.h"
#include "jit/MacroAssembler.h"

namespace js {
namespace irregexp {

class MOZ_STACK_CLASS RegExpMacroAssembler
{
  public:
    RegExpMacroAssembler(LifoAlloc& alloc, RegExpShared* shared, size_t numSavedRegisters)
      : slow_safe_compiler_(false),
        global_mode_(NOT_GLOBAL),
        alloc_(alloc),
        num_registers_(numSavedRegisters),
        num_saved_registers_(numSavedRegisters),
        shared(shared)
    {}

    enum StackCheckFlag {
        kNoStackLimitCheck = false,
        kCheckStackLimit = true
    };

    // The implementation must be able to handle at least:
    static const int kMaxRegister = (1 << 16) - 1;
    static const int kMaxCPOffset = (1 << 15) - 1;
    static const int kMinCPOffset = -(1 << 15);

    static const int kTableSizeBits = 7;
    static const int kTableSize = 1 << kTableSizeBits;
    static const int kTableMask = kTableSize - 1;

    // Controls the generation of large inlined constants in the code.
    void set_slow_safe(bool ssc) { slow_safe_compiler_ = ssc; }
    bool slow_safe() { return slow_safe_compiler_; }

    enum GlobalMode { NOT_GLOBAL, GLOBAL, GLOBAL_NO_ZERO_LENGTH_CHECK };

    // Set whether the regular expression has the global flag.  Exiting due to
    // a failure in a global regexp may still mean success overall.
    inline void set_global_mode(GlobalMode mode) { global_mode_ = mode; }
    inline bool global() { return global_mode_ != NOT_GLOBAL; }
    inline bool global_with_zero_length_check() {
        return global_mode_ == GLOBAL;
    }

    LifoAlloc& alloc() { return alloc_; }

    virtual RegExpCode GenerateCode(JSContext* cx, bool match_only) = 0;

    // The maximal number of pushes between stack checks. Users must supply
    // kCheckStackLimit flag to push operations (instead of kNoStackLimitCheck)
    // at least once for every stack_limit() pushes that are executed.
    virtual int stack_limit_slack() = 0;

    virtual bool CanReadUnaligned() { return false; }

    virtual void AdvanceCurrentPosition(int by) = 0;  // Signed cp change.
    virtual void AdvanceRegister(int reg, int by) = 0;  // r[reg] += by.

    // Continues execution from the position pushed on the top of the backtrack
    // stack by an earlier PushBacktrack.
    virtual void Backtrack() = 0;

    virtual void Bind(jit::Label* label) = 0;
    virtual void CheckAtStart(jit::Label* on_at_start) = 0;

    // Dispatch after looking the current character up in a 2-bits-per-entry
    // map.  The destinations vector has up to 4 labels.
    virtual void CheckCharacter(unsigned c, jit::Label* on_equal) = 0;

    // Bitwise and the current character with the given constant and then
    // check for a match with c.
    virtual void CheckCharacterAfterAnd(unsigned c, unsigned and_with, jit::Label* on_equal) = 0;

    virtual void CheckCharacterGT(char16_t limit, jit::Label* on_greater) = 0;
    virtual void CheckCharacterLT(char16_t limit, jit::Label* on_less) = 0;
    virtual void CheckGreedyLoop(jit::Label* on_tos_equals_current_position) = 0;
    virtual void CheckNotAtStart(jit::Label* on_not_at_start) = 0;
    virtual void CheckNotBackReference(int start_reg, jit::Label* on_no_match) = 0;
    virtual void CheckNotBackReferenceIgnoreCase(int start_reg, jit::Label* on_no_match) = 0;

    // Check the current character for a match with a literal character.  If we
    // fail to match then goto the on_failure label.  End of input always
    // matches.  If the label is nullptr then we should pop a backtrack address off
    // the stack and go to that.
    virtual void CheckNotCharacter(unsigned c, jit::Label* on_not_equal) = 0;
    virtual void CheckNotCharacterAfterAnd(unsigned c, unsigned and_with, jit::Label* on_not_equal) = 0;

    // Subtract a constant from the current character, then and with the given
    // constant and then check for a match with c.
    virtual void CheckNotCharacterAfterMinusAnd(char16_t c,
                                        char16_t minus,
                                        char16_t and_with,
                                        jit::Label* on_not_equal) = 0;

    virtual void CheckCharacterInRange(char16_t from, char16_t to,  // Both inclusive.
                               jit::Label* on_in_range) = 0;

    virtual void CheckCharacterNotInRange(char16_t from, char16_t to,  // Both inclusive.
                                  jit::Label* on_not_in_range) = 0;

    // The current character (modulus the kTableSize) is looked up in the byte
    // array, and if the found byte is non-zero, we jump to the on_bit_set label.
    virtual void CheckBitInTable(uint8_t* table, jit::Label* on_bit_set) = 0;

    // Checks whether the given offset from the current position is before
    // the end of the string. May overwrite the current character.
    virtual void CheckPosition(int cp_offset, jit::Label* on_outside_input) {
        LoadCurrentCharacter(cp_offset, on_outside_input, true);
    }

    // Jump to either the target label or the top of the backtrack stack.
    virtual void JumpOrBacktrack(jit::Label* to) = 0;

    // Check whether a standard/default character class matches the current
    // character. Returns false if the type of special character class does
    // not have custom support.
    // May clobber the current loaded character.
    virtual bool CheckSpecialCharacterClass(char16_t type, jit::Label* on_no_match) {
        return false;
    }

    virtual void Fail() = 0;

    // Check whether a register is >= a given constant and go to a label if it
    // is.  Backtracks instead if the label is nullptr.
    virtual void IfRegisterGE(int reg, int comparand, jit::Label* if_ge) = 0;

    // Check whether a register is < a given constant and go to a label if it is.
    // Backtracks instead if the label is nullptr.
    virtual void IfRegisterLT(int reg, int comparand, jit::Label* if_lt) = 0;

    // Check whether a register is == to the current position and go to a
    // label if it is.
    virtual void IfRegisterEqPos(int reg, jit::Label* if_eq) = 0;

    virtual void LoadCurrentCharacter(int cp_offset,
                                      jit::Label* on_end_of_input,
                                      bool check_bounds = true,
                                      int characters = 1) = 0;
    virtual void PopCurrentPosition() = 0;
    virtual void PopRegister(int register_index) = 0;

    virtual void PushCurrentPosition() = 0;
    virtual void PushRegister(int register_index, StackCheckFlag check_stack_limit) = 0;
    virtual void ReadCurrentPositionFromRegister(int reg) = 0;
    virtual void ReadBacktrackStackPointerFromRegister(int reg) = 0;
    virtual void SetCurrentPositionFromEnd(int by) = 0;
    virtual void SetRegister(int register_index, int to) = 0;

    // Return whether the matching (with a global regexp) will be restarted.
    virtual bool Succeed() = 0;

    virtual void WriteCurrentPositionToRegister(int reg, int cp_offset) = 0;
    virtual void ClearRegisters(int reg_from, int reg_to) = 0;
    virtual void WriteBacktrackStackPointerToRegister(int reg) = 0;

    // Pushes the label on the backtrack stack, so that a following Backtrack
    // will go to this label. Always checks the backtrack stack limit.
    virtual void PushBacktrack(jit::Label* label) = 0;

    // Bind a label that was previously used by PushBacktrack.
    virtual void BindBacktrack(jit::Label* label) = 0;

  private:
    bool slow_safe_compiler_;
    GlobalMode global_mode_;
    LifoAlloc& alloc_;

  protected:
    int num_registers_;
    int num_saved_registers_;

    void checkRegister(int reg) {
        MOZ_ASSERT(reg >= 0);
        MOZ_ASSERT(reg <= kMaxRegister);
        if (num_registers_ <= reg)
            num_registers_ = reg + 1;
    }

  public:
    RegExpShared* shared;
};

template <typename CharT>
int
CaseInsensitiveCompareStrings(const CharT* substring1, const CharT* substring2, size_t byteLength);

class MOZ_STACK_CLASS InterpretedRegExpMacroAssembler : public RegExpMacroAssembler
{
  public:
    InterpretedRegExpMacroAssembler(LifoAlloc* alloc, RegExpShared* shared, size_t numSavedRegisters);
    ~InterpretedRegExpMacroAssembler();

    // Inherited virtual methods.
    RegExpCode GenerateCode(JSContext* cx, bool match_only);
    void AdvanceCurrentPosition(int by);
    void AdvanceRegister(int reg, int by);
    void Backtrack();
    void Bind(jit::Label* label);
    void CheckAtStart(jit::Label* on_at_start);
    void CheckCharacter(unsigned c, jit::Label* on_equal);
    void CheckCharacterAfterAnd(unsigned c, unsigned and_with, jit::Label* on_equal);
    void CheckCharacterGT(char16_t limit, jit::Label* on_greater);
    void CheckCharacterLT(char16_t limit, jit::Label* on_less);
    void CheckGreedyLoop(jit::Label* on_tos_equals_current_position);
    void CheckNotAtStart(jit::Label* on_not_at_start);
    void CheckNotBackReference(int start_reg, jit::Label* on_no_match);
    void CheckNotBackReferenceIgnoreCase(int start_reg, jit::Label* on_no_match);
    void CheckNotCharacter(unsigned c, jit::Label* on_not_equal);
    void CheckNotCharacterAfterAnd(unsigned c, unsigned and_with, jit::Label* on_not_equal);
    void CheckNotCharacterAfterMinusAnd(char16_t c, char16_t minus, char16_t and_with,
                                        jit::Label* on_not_equal);
    void CheckCharacterInRange(char16_t from, char16_t to,
                               jit::Label* on_in_range);
    void CheckCharacterNotInRange(char16_t from, char16_t to,
                                  jit::Label* on_not_in_range);
    void CheckBitInTable(uint8_t* table, jit::Label* on_bit_set);
    void JumpOrBacktrack(jit::Label* to);
    void Fail();
    void IfRegisterGE(int reg, int comparand, jit::Label* if_ge);
    void IfRegisterLT(int reg, int comparand, jit::Label* if_lt);
    void IfRegisterEqPos(int reg, jit::Label* if_eq);
    void LoadCurrentCharacter(int cp_offset, jit::Label* on_end_of_input,
                              bool check_bounds = true, int characters = 1);
    void PopCurrentPosition();
    void PopRegister(int register_index);
    void PushCurrentPosition();
    void PushRegister(int register_index, StackCheckFlag check_stack_limit);
    void ReadCurrentPositionFromRegister(int reg);
    void ReadBacktrackStackPointerFromRegister(int reg);
    void SetCurrentPositionFromEnd(int by);
    void SetRegister(int register_index, int to);
    bool Succeed();
    void WriteCurrentPositionToRegister(int reg, int cp_offset);
    void ClearRegisters(int reg_from, int reg_to);
    void WriteBacktrackStackPointerToRegister(int reg);
    void PushBacktrack(jit::Label* label);
    void BindBacktrack(jit::Label* label);

    // The byte-code interpreter checks on each push anyway.
    int stack_limit_slack() { return 1; }

  private:
    void Expand();

    // Code and bitmap emission.
    void EmitOrLink(jit::Label* label);
    void Emit32(uint32_t x);
    void Emit16(uint32_t x);
    void Emit8(uint32_t x);
    void Emit(uint32_t bc, uint32_t arg);

    jit::Label backtrack_;

    // The program counter.
    int pc_;

    int advance_current_start_;
    int advance_current_offset_;
    int advance_current_end_;

    static const int kInvalidPC = -1;

    uint8_t* buffer_;
    int length_;
};

} }  // namespace js::irregexp

#endif  // V8_REGEXP_MACRO_ASSEMBLER_H_
