/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/Assembler-x86.h"

#include "gc/Marking.h"

using namespace js;
using namespace js::jit;

ABIArgGenerator::ABIArgGenerator()
  : stackOffset_(0),
    current_()
{}

ABIArg
ABIArgGenerator::next(MIRType type)
{
    switch (type) {
      case MIRType_Int32:
      case MIRType_Pointer:
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(uint32_t);
        break;
      case MIRType_Float32: // Float32 moves are actually double moves
      case MIRType_Double:
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(uint64_t);
        break;
      case MIRType_Int32x4:
      case MIRType_Float32x4:
        // SIMD values aren't passed in or out of C++, so we can make up
        // whatever internal ABI we like. visitAsmJSPassArg assumes
        // SimdMemoryAlignment.
        stackOffset_ = AlignBytes(stackOffset_, SimdMemoryAlignment);
        current_ = ABIArg(stackOffset_);
        stackOffset_ += Simd128DataSize;
        break;
      default:
        MOZ_CRASH("Unexpected argument type");
    }
    return current_;
}

const Register ABIArgGenerator::NonArgReturnReg0 = ecx;
const Register ABIArgGenerator::NonArgReturnReg1 = edx;
const Register ABIArgGenerator::NonVolatileReg = ebx;
const Register ABIArgGenerator::NonArg_VolatileReg = eax;
const Register ABIArgGenerator::NonReturn_VolatileReg0 = ecx;

void
Assembler::executableCopy(uint8_t* buffer)
{
    AssemblerX86Shared::executableCopy(buffer);

    for (size_t i = 0; i < jumps_.length(); i++) {
        RelativePatch& rp = jumps_[i];
        X86Encoding::SetRel32(buffer + rp.offset, rp.target);
    }
}

class RelocationIterator
{
    CompactBufferReader reader_;
    uint32_t offset_;

  public:
    RelocationIterator(CompactBufferReader& reader)
      : reader_(reader)
    { }

    bool read() {
        if (!reader_.more())
            return false;
        offset_ = reader_.readUnsigned();
        return true;
    }

    uint32_t offset() const {
        return offset_;
    }
};

static inline JitCode*
CodeFromJump(uint8_t* jump)
{
    uint8_t* target = (uint8_t*)X86Encoding::GetRel32Target(jump);
    return JitCode::FromExecutable(target);
}

void
Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    RelocationIterator iter(reader);
    while (iter.read()) {
        JitCode* child = CodeFromJump(code->raw() + iter.offset());
        MarkJitCodeUnbarriered(trc, &child, "rel32");
        MOZ_ASSERT(child == CodeFromJump(code->raw() + iter.offset()));
    }
}

uint32_t
FloatRegister::GetSizeInBytes(const FloatRegisterSet& s)
{
    uint32_t ret = s.size() * sizeof(double);
    return ret;
}

FloatRegisterSet
FloatRegister::ReduceSetForPush(const FloatRegisterSet& s)
{
    return s;
}
uint32_t
FloatRegister::GetPushSizeInBytes(const FloatRegisterSet& s)
{
    return s.size() * sizeof(double);
}
uint32_t
FloatRegister::getRegisterDumpOffsetInBytes()
{
    return code() * sizeof(double);
}
