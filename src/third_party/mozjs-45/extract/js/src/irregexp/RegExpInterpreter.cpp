/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */

// Copyright 2011 the V8 project authors. All rights reserved.
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

// A simple interpreter for the Irregexp byte code.

#include "irregexp/RegExpBytecode.h"
#include "irregexp/RegExpMacroAssembler.h"
#include "vm/MatchPairs.h"

using namespace js;
using namespace js::irregexp;

static const size_t kBitsPerByte = 8;
static const size_t kBitsPerByteLog2 = 3;

class MOZ_STACK_CLASS RegExpStackCursor
{
  public:
    explicit RegExpStackCursor(JSContext* cx)
      : cx(cx), cursor(nullptr)
    {}

    bool init() {
        if (!stack.init()) {
            ReportOutOfMemory(cx);
            return false;
        }
        cursor = base();
        return true;
    }

    bool push(int32_t value) {
        *cursor++ = value;
        if (cursor >= stack.limit()) {
            int32_t pos = position();
            if (!stack.grow()) {
                ReportOverRecursed(cx);
                return false;
            }
            setPosition(pos);
        }
        return true;
    }

    int32_t pop() {
        MOZ_ASSERT(cursor > base());
        return *--cursor;
    }

    int32_t peek() {
        MOZ_ASSERT(cursor > base());
        return *(cursor - 1);
    }

    int32_t position() {
        MOZ_ASSERT(cursor >= base());
        return cursor - base();
    }

    void setPosition(int32_t position) {
        cursor = base() + position;
        MOZ_ASSERT(cursor < stack.limit());
    }

  private:
    JSContext* cx;
    RegExpStack stack;

    int32_t* cursor;

    int32_t* base() { return (int32_t*) stack.base(); }
};

static int32_t
Load32Aligned(const uint8_t* pc)
{
    MOZ_ASSERT((reinterpret_cast<uintptr_t>(pc) & 3) == 0);
    return *reinterpret_cast<const int32_t*>(pc);
}

static int32_t
Load16Aligned(const uint8_t* pc)
{
    MOZ_ASSERT((reinterpret_cast<uintptr_t>(pc) & 1) == 0);
    return *reinterpret_cast<const uint16_t*>(pc);
}

#define BYTECODE(name)  case BC_##name:

template <typename CharT>
RegExpRunStatus
irregexp::InterpretCode(JSContext* cx, const uint8_t* byteCode, const CharT* chars, size_t current,
                        size_t length, MatchPairs* matches)
{
    const uint8_t* pc = byteCode;

    uint32_t current_char = current ? chars[current - 1] : '\n';

    RegExpStackCursor stack(cx);

    if (!stack.init())
        return RegExpRunStatus_Error;

    int32_t numRegisters = Load32Aligned(pc);
    pc += 4;

    Vector<int32_t, 0, SystemAllocPolicy> registers;
    if (!registers.growByUninitialized(numRegisters))
        return RegExpRunStatus_Error;
    for (size_t i = 0; i < (size_t) numRegisters; i++)
        registers[i] = -1;

    while (true) {
        int32_t insn = Load32Aligned(pc);
        switch (insn & BYTECODE_MASK) {
          BYTECODE(BREAK)
            MOZ_CRASH("Bad bytecode: BREAK");
          BYTECODE(PUSH_CP)
            if (!stack.push(current))
                return RegExpRunStatus_Error;
            pc += BC_PUSH_CP_LENGTH;
            break;
          BYTECODE(PUSH_BT)
            if (!stack.push(Load32Aligned(pc + 4)))
                return RegExpRunStatus_Error;
            pc += BC_PUSH_BT_LENGTH;
            break;
          BYTECODE(PUSH_REGISTER)
            if (!stack.push(registers[insn >> BYTECODE_SHIFT]))
                return RegExpRunStatus_Error;
            pc += BC_PUSH_REGISTER_LENGTH;
            break;
          BYTECODE(SET_REGISTER)
            registers[insn >> BYTECODE_SHIFT] = Load32Aligned(pc + 4);
            pc += BC_SET_REGISTER_LENGTH;
            break;
          BYTECODE(ADVANCE_REGISTER)
            registers[insn >> BYTECODE_SHIFT] += Load32Aligned(pc + 4);
            pc += BC_ADVANCE_REGISTER_LENGTH;
            break;
          BYTECODE(SET_REGISTER_TO_CP)
            registers[insn >> BYTECODE_SHIFT] = current + Load32Aligned(pc + 4);
            pc += BC_SET_REGISTER_TO_CP_LENGTH;
            break;
          BYTECODE(SET_CP_TO_REGISTER)
            current = registers[insn >> BYTECODE_SHIFT];
            pc += BC_SET_CP_TO_REGISTER_LENGTH;
            break;
          BYTECODE(SET_REGISTER_TO_SP)
            registers[insn >> BYTECODE_SHIFT] = stack.position();
            pc += BC_SET_REGISTER_TO_SP_LENGTH;
            break;
          BYTECODE(SET_SP_TO_REGISTER)
            stack.setPosition(registers[insn >> BYTECODE_SHIFT]);
            pc += BC_SET_SP_TO_REGISTER_LENGTH;
            break;
          BYTECODE(POP_CP)
            current = stack.pop();
            pc += BC_POP_CP_LENGTH;
            break;
          BYTECODE(POP_BT)
            if (!CheckForInterrupt(cx))
                return RegExpRunStatus_Error;
            pc = byteCode + stack.pop();
            break;
          BYTECODE(POP_REGISTER)
            registers[insn >> BYTECODE_SHIFT] = stack.pop();
            pc += BC_POP_REGISTER_LENGTH;
            break;
          BYTECODE(FAIL)
            return RegExpRunStatus_Success_NotFound;
          BYTECODE(SUCCEED)
            if (matches)
                memcpy(matches->pairsRaw(), registers.begin(), matches->length() * 2 * sizeof(int32_t));
            return RegExpRunStatus_Success;
          BYTECODE(ADVANCE_CP)
            current += insn >> BYTECODE_SHIFT;
            pc += BC_ADVANCE_CP_LENGTH;
            break;
          BYTECODE(GOTO)
            pc = byteCode + Load32Aligned(pc + 4);
            break;
          BYTECODE(ADVANCE_CP_AND_GOTO)
            current += insn >> BYTECODE_SHIFT;
            pc = byteCode + Load32Aligned(pc + 4);
            break;
          BYTECODE(CHECK_GREEDY)
            if ((int32_t)current == stack.peek()) {
                stack.pop();
                pc = byteCode + Load32Aligned(pc + 4);
            } else {
                pc += BC_CHECK_GREEDY_LENGTH;
            }
            break;
          BYTECODE(LOAD_CURRENT_CHAR) {
            size_t pos = current + (insn >> BYTECODE_SHIFT);
            if (pos >= length) {
                pc = byteCode + Load32Aligned(pc + 4);
            } else {
                current_char = chars[pos];
                pc += BC_LOAD_CURRENT_CHAR_LENGTH;
            }
            break;
          }
          BYTECODE(LOAD_CURRENT_CHAR_UNCHECKED) {
            int pos = current + (insn >> BYTECODE_SHIFT);
            current_char = chars[pos];
            pc += BC_LOAD_CURRENT_CHAR_UNCHECKED_LENGTH;
            break;
          }
          BYTECODE(LOAD_2_CURRENT_CHARS) {
            size_t pos = current + (insn >> BYTECODE_SHIFT);
            if (pos + 2 > length) {
                pc = byteCode + Load32Aligned(pc + 4);
            } else {
                CharT next = chars[pos + 1];
                current_char = (chars[pos] | (next << (kBitsPerByte * sizeof(CharT))));
                pc += BC_LOAD_2_CURRENT_CHARS_LENGTH;
            }
            break;
          }
          BYTECODE(LOAD_2_CURRENT_CHARS_UNCHECKED) {
            int pos = current + (insn >> BYTECODE_SHIFT);
            char16_t next = chars[pos + 1];
            current_char = (chars[pos] | (next << (kBitsPerByte * sizeof(char16_t))));
            pc += BC_LOAD_2_CURRENT_CHARS_UNCHECKED_LENGTH;
            break;
          }
          BYTECODE(LOAD_4_CURRENT_CHARS)
            MOZ_CRASH("ASCII handling implemented");
          BYTECODE(LOAD_4_CURRENT_CHARS_UNCHECKED)
            MOZ_CRASH("ASCII handling implemented");
          BYTECODE(CHECK_4_CHARS) {
            uint32_t c = Load32Aligned(pc + 4);
            if (c == current_char)
                pc = byteCode + Load32Aligned(pc + 8);
            else
                pc += BC_CHECK_4_CHARS_LENGTH;
            break;
          }
          BYTECODE(CHECK_CHAR) {
            uint32_t c = (insn >> BYTECODE_SHIFT);
            if (c == current_char)
                pc = byteCode + Load32Aligned(pc + 4);
            else
                pc += BC_CHECK_CHAR_LENGTH;
            break;
          }
          BYTECODE(CHECK_NOT_4_CHARS) {
            uint32_t c = Load32Aligned(pc + 4);
            if (c != current_char)
                pc = byteCode + Load32Aligned(pc + 8);
            else
                pc += BC_CHECK_NOT_4_CHARS_LENGTH;
            break;
          }
          BYTECODE(CHECK_NOT_CHAR) {
            uint32_t c = (insn >> BYTECODE_SHIFT);
            if (c != current_char)
                pc = byteCode + Load32Aligned(pc + 4);
            else
                pc += BC_CHECK_NOT_CHAR_LENGTH;
            break;
          }
          BYTECODE(AND_CHECK_4_CHARS) {
            uint32_t c = Load32Aligned(pc + 4);
            if (c == (current_char & Load32Aligned(pc + 8)))
                pc = byteCode + Load32Aligned(pc + 12);
            else
                pc += BC_AND_CHECK_4_CHARS_LENGTH;
            break;
          }
          BYTECODE(AND_CHECK_CHAR) {
            uint32_t c = (insn >> BYTECODE_SHIFT);
            if (c == (current_char & Load32Aligned(pc + 4)))
                pc = byteCode + Load32Aligned(pc + 8);
            else
                pc += BC_AND_CHECK_CHAR_LENGTH;
            break;
          }
          BYTECODE(AND_CHECK_NOT_4_CHARS) {
            uint32_t c = Load32Aligned(pc + 4);
            if (c != (current_char & Load32Aligned(pc + 8)))
                pc = byteCode + Load32Aligned(pc + 12);
            else
                pc += BC_AND_CHECK_NOT_4_CHARS_LENGTH;
            break;
          }
          BYTECODE(AND_CHECK_NOT_CHAR) {
            uint32_t c = (insn >> BYTECODE_SHIFT);
            if (c != (current_char & Load32Aligned(pc + 4)))
                pc = byteCode + Load32Aligned(pc + 8);
            else
                pc += BC_AND_CHECK_NOT_CHAR_LENGTH;
            break;
          }
          BYTECODE(MINUS_AND_CHECK_NOT_CHAR) {
            uint32_t c = (insn >> BYTECODE_SHIFT);
            uint32_t minus = Load16Aligned(pc + 4);
            uint32_t mask = Load16Aligned(pc + 6);
            if (c != ((current_char - minus) & mask))
                pc = byteCode + Load32Aligned(pc + 8);
            else
                pc += BC_MINUS_AND_CHECK_NOT_CHAR_LENGTH;
            break;
          }
          BYTECODE(CHECK_CHAR_IN_RANGE) {
            uint32_t from = Load16Aligned(pc + 4);
            uint32_t to = Load16Aligned(pc + 6);
            if (from <= current_char && current_char <= to)
                pc = byteCode + Load32Aligned(pc + 8);
            else
                pc += BC_CHECK_CHAR_IN_RANGE_LENGTH;
            break;
          }
          BYTECODE(CHECK_CHAR_NOT_IN_RANGE) {
            uint32_t from = Load16Aligned(pc + 4);
            uint32_t to = Load16Aligned(pc + 6);
            if (from > current_char || current_char > to)
                pc = byteCode + Load32Aligned(pc + 8);
            else
                pc += BC_CHECK_CHAR_NOT_IN_RANGE_LENGTH;
            break;
          }
          BYTECODE(CHECK_BIT_IN_TABLE) {
            int mask = RegExpMacroAssembler::kTableMask;
            uint8_t b = pc[8 + ((current_char & mask) >> kBitsPerByteLog2)];
            int bit = (current_char & (kBitsPerByte - 1));
            if ((b & (1 << bit)) != 0)
                pc = byteCode + Load32Aligned(pc + 4);
            else
                pc += BC_CHECK_BIT_IN_TABLE_LENGTH;
            break;
          }
          BYTECODE(CHECK_LT) {
            uint32_t limit = (insn >> BYTECODE_SHIFT);
            if (current_char < limit)
                pc = byteCode + Load32Aligned(pc + 4);
            else
                pc += BC_CHECK_LT_LENGTH;
            break;
          }
          BYTECODE(CHECK_GT) {
            uint32_t limit = (insn >> BYTECODE_SHIFT);
            if (current_char > limit)
                pc = byteCode + Load32Aligned(pc + 4);
            else
                pc += BC_CHECK_GT_LENGTH;
            break;
          }
          BYTECODE(CHECK_REGISTER_LT)
            if (registers[insn >> BYTECODE_SHIFT] < Load32Aligned(pc + 4))
                pc = byteCode + Load32Aligned(pc + 8);
            else
                pc += BC_CHECK_REGISTER_LT_LENGTH;
            break;
          BYTECODE(CHECK_REGISTER_GE)
            if (registers[insn >> BYTECODE_SHIFT] >= Load32Aligned(pc + 4))
                pc = byteCode + Load32Aligned(pc + 8);
            else
                pc += BC_CHECK_REGISTER_GE_LENGTH;
            break;
          BYTECODE(CHECK_REGISTER_EQ_POS)
            if (registers[insn >> BYTECODE_SHIFT] == (int32_t) current)
                pc = byteCode + Load32Aligned(pc + 4);
            else
                pc += BC_CHECK_REGISTER_EQ_POS_LENGTH;
            break;
          BYTECODE(CHECK_NOT_REGS_EQUAL)
            if (registers[insn >> BYTECODE_SHIFT] == registers[Load32Aligned(pc + 4)])
                pc += BC_CHECK_NOT_REGS_EQUAL_LENGTH;
            else
                pc = byteCode + Load32Aligned(pc + 8);
            break;
          BYTECODE(CHECK_NOT_BACK_REF) {
            int from = registers[insn >> BYTECODE_SHIFT];
            int len = registers[(insn >> BYTECODE_SHIFT) + 1] - from;
            if (from < 0 || len <= 0) {
                pc += BC_CHECK_NOT_BACK_REF_LENGTH;
                break;
            }
            if (current + len > length) {
                pc = byteCode + Load32Aligned(pc + 4);
                break;
            } else {
                int i;
                for (i = 0; i < len; i++) {
                    if (chars[from + i] != chars[current + i]) {
                        pc = byteCode + Load32Aligned(pc + 4);
                        break;
                    }
                }
                if (i < len) break;
                current += len;
            }
            pc += BC_CHECK_NOT_BACK_REF_LENGTH;
            break;
          }
          BYTECODE(CHECK_NOT_BACK_REF_NO_CASE) {
            int from = registers[insn >> BYTECODE_SHIFT];
            int len = registers[(insn >> BYTECODE_SHIFT) + 1] - from;
            if (from < 0 || len <= 0) {
                pc += BC_CHECK_NOT_BACK_REF_NO_CASE_LENGTH;
                break;
            }
            if (current + len > length) {
                pc = byteCode + Load32Aligned(pc + 4);
                break;
            }
            if (CaseInsensitiveCompareStrings(chars + from, chars + current, len * sizeof(CharT))) {
                current += len;
                pc += BC_CHECK_NOT_BACK_REF_NO_CASE_LENGTH;
            } else {
                pc = byteCode + Load32Aligned(pc + 4);
            }
            break;
          }
          BYTECODE(CHECK_AT_START)
            if (current == 0)
                pc = byteCode + Load32Aligned(pc + 4);
            else
                pc += BC_CHECK_AT_START_LENGTH;
            break;
          BYTECODE(CHECK_NOT_AT_START)
            if (current == 0)
                pc += BC_CHECK_NOT_AT_START_LENGTH;
            else
                pc = byteCode + Load32Aligned(pc + 4);
            break;
          BYTECODE(SET_CURRENT_POSITION_FROM_END) {
            size_t by = static_cast<uint32_t>(insn) >> BYTECODE_SHIFT;
            if (length - current > by) {
                current = length - by;
                current_char = chars[current - 1];
            }
            pc += BC_SET_CURRENT_POSITION_FROM_END_LENGTH;
            break;
          }
          default:
            MOZ_CRASH("Bad bytecode");
        }
    }
}

template RegExpRunStatus
irregexp::InterpretCode(JSContext* cx, const uint8_t* byteCode, const Latin1Char* chars, size_t current,
                        size_t length, MatchPairs* matches);

template RegExpRunStatus
irregexp::InterpretCode(JSContext* cx, const uint8_t* byteCode, const char16_t* chars, size_t current,
                        size_t length, MatchPairs* matches);
