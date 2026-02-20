/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips64/Assembler-mips64.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include "jit/AutoWritableJitCode.h"

using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

ABIArgGenerator::ABIArgGenerator()
    : regIndex_(0), stackOffset_(0), current_() {}

ABIArg ABIArgGenerator::next(MIRType type) {
  static_assert(NumIntArgRegs == NumFloatArgRegs);
  if (regIndex_ == NumIntArgRegs) {
    if (type != MIRType::Simd128) {
      current_ = ABIArg(stackOffset_);
      stackOffset_ += sizeof(uint64_t);
    } else {
      // Mips platform does not support simd yet.
      MOZ_CRASH("Unexpected argument type");
    }
    return current_;
  }
  switch (type) {
    case MIRType::Int32:
    case MIRType::Int64:
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
    case MIRType::WasmArrayData:
    case MIRType::StackResults: {
      Register destReg;
      GetIntArgReg(regIndex_++, &destReg);
      current_ = ABIArg(destReg);
      break;
    }
    case MIRType::Float32:
    case MIRType::Double: {
      FloatRegister::ContentType contentType;
      contentType = (type == MIRType::Double) ? FloatRegisters::Double
                                              : FloatRegisters::Single;
      FloatRegister destFReg;
      GetFloatArgReg(regIndex_++, &destFReg);
      current_ = ABIArg(FloatRegister(destFReg.id(), contentType));
      break;
    }
    default:
      MOZ_CRASH("Unexpected argument type");
  }
  return current_;
}

uint32_t js::jit::RT(FloatRegister r) {
  MOZ_ASSERT(r.id() < FloatRegisters::TotalPhys);
  return r.id() << RTShift;
}

uint32_t js::jit::RD(FloatRegister r) {
  MOZ_ASSERT(r.id() < FloatRegisters::TotalPhys);
  return r.id() << RDShift;
}

uint32_t js::jit::RZ(FloatRegister r) {
  MOZ_ASSERT(r.id() < FloatRegisters::TotalPhys);
  return r.id() << RZShift;
}

uint32_t js::jit::SA(FloatRegister r) {
  MOZ_ASSERT(r.id() < FloatRegisters::TotalPhys);
  return r.id() << SAShift;
}

void Assembler::executableCopy(uint8_t* buffer) {
  MOZ_ASSERT(isFinished);
  m_buffer.executableCopy(buffer);
}

uintptr_t Assembler::GetPointer(uint8_t* instPtr) {
  Instruction* inst = (Instruction*)instPtr;
  return Assembler::ExtractLoad64Value(inst);
}

static JitCode* CodeFromJump(Instruction* jump) {
  uint8_t* target = (uint8_t*)Assembler::ExtractLoad64Value(jump);
  return JitCode::FromExecutable(target);
}

void Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  while (reader.more()) {
    JitCode* child =
        CodeFromJump((Instruction*)(code->raw() + reader.readUnsigned()));
    TraceManuallyBarrieredEdge(trc, &child, "rel32");
  }
}

static void TraceOneDataRelocation(JSTracer* trc,
                                   mozilla::Maybe<AutoWritableJitCode>& awjc,
                                   JitCode* code, Instruction* inst) {
  void* ptr = (void*)Assembler::ExtractLoad64Value(inst);
  void* prior = ptr;

  // Data relocations can be for Values or for raw pointers. If a Value is
  // zero-tagged, we can trace it as if it were a raw pointer. If a Value
  // is not zero-tagged, we have to interpret it as a Value to ensure that the
  // tag bits are masked off to recover the actual pointer.
  uintptr_t word = reinterpret_cast<uintptr_t>(ptr);
  if (word >> JSVAL_TAG_SHIFT) {
    // This relocation is a Value with a non-zero tag.
    Value v = Value::fromRawBits(word);
    TraceManuallyBarrieredEdge(trc, &v, "jit-masm-value");
    ptr = (void*)v.bitsAsPunboxPointer();
  } else {
    // This relocation is a raw pointer or a Value with a zero tag.
    // No barrier needed since these are constants.
    TraceManuallyBarrieredGenericPointerEdge(
        trc, reinterpret_cast<gc::Cell**>(&ptr), "jit-masm-ptr");
  }

  if (ptr != prior) {
    if (awjc.isNothing()) {
      awjc.emplace(code);
    }
    Assembler::UpdateLoad64Value(inst, uint64_t(ptr));
  }
}

/* static */
void Assembler::TraceDataRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  mozilla::Maybe<AutoWritableJitCode> awjc;
  while (reader.more()) {
    size_t offset = reader.readUnsigned();
    Instruction* inst = (Instruction*)(code->raw() + offset);
    TraceOneDataRelocation(trc, awjc, code, inst);
  }
}

void Assembler::Bind(uint8_t* rawCode, const CodeLabel& label) {
  if (label.patchAt().bound()) {
    auto mode = label.linkMode();
    intptr_t offset = label.patchAt().offset();
    intptr_t target = label.target().offset();

    if (mode == CodeLabel::RawPointer) {
      *reinterpret_cast<const void**>(rawCode + offset) = rawCode + target;
    } else {
      MOZ_ASSERT(mode == CodeLabel::MoveImmediate ||
                 mode == CodeLabel::JumpImmediate);
      Instruction* inst = (Instruction*)(rawCode + offset);
      Assembler::UpdateLoad64Value(inst, (uint64_t)(rawCode + target));
    }
  }
}

void Assembler::bind(InstImm* inst, uintptr_t branch, uintptr_t target) {
  int64_t offset = target - branch;
  InstImm inst_bgezal = InstImm(op_regimm, zero, rt_bgezal, BOffImm16(0));
  InstImm inst_beq = InstImm(op_beq, zero, zero, BOffImm16(0));

  // If encoded offset is 4, then the jump must be short
  if (BOffImm16(inst[0]).decode() == 4) {
    MOZ_ASSERT(BOffImm16::IsInRange(offset));
    inst[0].setBOffImm16(BOffImm16(offset));
    inst[1].makeNop();
    return;
  }

  // Generate the long jump for calls because return address has to be the
  // address after the reserved block.
  if (inst[0].encode() == inst_bgezal.encode()) {
    addLongJump(BufferOffset(branch), BufferOffset(target));
    Assembler::WriteLoad64Instructions(inst, ScratchRegister,
                                       LabelBase::INVALID_OFFSET);
    inst[4] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr).encode();
    // There is 1 nop after this.
    return;
  }

  if (BOffImm16::IsInRange(offset)) {
    // Don't skip trailing nops can improve performance
    // on Loongson3 platform.
    bool skipNops =
        !isLoongson() && (inst[0].encode() != inst_bgezal.encode() &&
                          inst[0].encode() != inst_beq.encode());

    inst[0].setBOffImm16(BOffImm16(offset));
    inst[1].makeNop();

    if (skipNops) {
      inst[2] =
          InstImm(op_regimm, zero, rt_bgez, BOffImm16(5 * sizeof(uint32_t)))
              .encode();
      // There are 4 nops after this
    }
    return;
  }

  if (inst[0].encode() == inst_beq.encode()) {
    // Handle long unconditional jump.
    addLongJump(BufferOffset(branch), BufferOffset(target));
    Assembler::WriteLoad64Instructions(inst, ScratchRegister,
                                       LabelBase::INVALID_OFFSET);
#ifdef MIPSR6
    inst[4] =
        InstReg(op_special, ScratchRegister, zero, zero, ff_jalr).encode();
#else
    inst[4] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
#endif
    // There is 1 nop after this.
  } else {
    // Handle long conditional jump.
    inst[0] = invertBranch(inst[0], BOffImm16(7 * sizeof(uint32_t)));
    // No need for a "nop" here because we can clobber scratch.
    addLongJump(BufferOffset(branch + sizeof(uint32_t)), BufferOffset(target));
    Assembler::WriteLoad64Instructions(&inst[1], ScratchRegister,
                                       LabelBase::INVALID_OFFSET);
#ifdef MIPSR6
    inst[5] =
        InstReg(op_special, ScratchRegister, zero, zero, ff_jalr).encode();
#else
    inst[5] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
#endif
    // There is 1 nop after this.
  }
}

void Assembler::processCodeLabels(uint8_t* rawCode) {
  for (const CodeLabel& label : codeLabels_) {
    Bind(rawCode, label);
  }
}

uint32_t Assembler::PatchWrite_NearCallSize() {
  // Load an address needs 4 instructions, and a jump with a delay slot.
  return (4 + 2) * sizeof(uint32_t);
}

void Assembler::PatchWrite_NearCall(CodeLocationLabel start,
                                    CodeLocationLabel toCall) {
  Instruction* inst = (Instruction*)start.raw();
  uint8_t* dest = toCall.raw();

  // Overwrite whatever instruction used to be here with a call.
  // Always use long jump for two reasons:
  // - Jump has to be the same size because of PatchWrite_NearCallSize.
  // - Return address has to be at the end of replaced block.
  // Short jump wouldn't be more efficient.
  Assembler::WriteLoad64Instructions(inst, ScratchRegister, (uint64_t)dest);
  inst[4] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
  inst[5] = InstNOP();
}

uint64_t Assembler::ExtractLoad64Value(Instruction* inst0) {
  InstImm* i0 = (InstImm*)inst0;
  InstImm* i1 = (InstImm*)i0->next();
  InstReg* i2 = (InstReg*)i1->next();
  InstImm* i3 = (InstImm*)i2->next();
  InstImm* i5 = (InstImm*)i3->next()->next();

  MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
  MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));
  MOZ_ASSERT(i3->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

  if ((i2->extractOpcode() == ((uint32_t)op_special >> OpcodeShift)) &&
      (i2->extractFunctionField() == ff_dsrl32)) {
    uint64_t value = (uint64_t(i0->extractImm16Value()) << 32) |
                     (uint64_t(i1->extractImm16Value()) << 16) |
                     uint64_t(i3->extractImm16Value());
    return uint64_t((int64_t(value) << 16) >> 16);
  }

  MOZ_ASSERT(i5->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));
  uint64_t value = (uint64_t(i0->extractImm16Value()) << 48) |
                   (uint64_t(i1->extractImm16Value()) << 32) |
                   (uint64_t(i3->extractImm16Value()) << 16) |
                   uint64_t(i5->extractImm16Value());
  return value;
}

void Assembler::UpdateLoad64Value(Instruction* inst0, uint64_t value) {
  InstImm* i0 = (InstImm*)inst0;
  InstImm* i1 = (InstImm*)i0->next();
  InstReg* i2 = (InstReg*)i1->next();
  InstImm* i3 = (InstImm*)i2->next();
  InstImm* i5 = (InstImm*)i3->next()->next();

  MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
  MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));
  MOZ_ASSERT(i3->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

  if ((i2->extractOpcode() == ((uint32_t)op_special >> OpcodeShift)) &&
      (i2->extractFunctionField() == ff_dsrl32)) {
    i0->setImm16(Imm16::Lower(Imm32(value >> 32)));
    i1->setImm16(Imm16::Upper(Imm32(value)));
    i3->setImm16(Imm16::Lower(Imm32(value)));
    return;
  }

  MOZ_ASSERT(i5->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

  i0->setImm16(Imm16::Upper(Imm32(value >> 32)));
  i1->setImm16(Imm16::Lower(Imm32(value >> 32)));
  i3->setImm16(Imm16::Upper(Imm32(value)));
  i5->setImm16(Imm16::Lower(Imm32(value)));
}

void Assembler::WriteLoad64Instructions(Instruction* inst0, Register reg,
                                        uint64_t value) {
  Instruction* inst1 = inst0->next();
  Instruction* inst2 = inst1->next();
  Instruction* inst3 = inst2->next();

  *inst0 = InstImm(op_lui, zero, reg, Imm16::Lower(Imm32(value >> 32)));
  *inst1 = InstImm(op_ori, reg, reg, Imm16::Upper(Imm32(value)));
  *inst2 = InstReg(op_special, rs_one, reg, reg, 48 - 32, ff_dsrl32);
  *inst3 = InstImm(op_ori, reg, reg, Imm16::Lower(Imm32(value)));
}

void Assembler::PatchDataWithValueCheck(CodeLocationLabel label,
                                        ImmPtr newValue, ImmPtr expectedValue) {
  PatchDataWithValueCheck(label, PatchedImmPtr(newValue.value),
                          PatchedImmPtr(expectedValue.value));
}

void Assembler::PatchDataWithValueCheck(CodeLocationLabel label,
                                        PatchedImmPtr newValue,
                                        PatchedImmPtr expectedValue) {
  Instruction* inst = (Instruction*)label.raw();

  // Extract old Value
  DebugOnly<uint64_t> value = Assembler::ExtractLoad64Value(inst);
  MOZ_ASSERT(value == uint64_t(expectedValue.value));

  // Replace with new value
  Assembler::UpdateLoad64Value(inst, uint64_t(newValue.value));
}

uint64_t Assembler::ExtractInstructionImmediate(uint8_t* code) {
  InstImm* inst = (InstImm*)code;
  return Assembler::ExtractLoad64Value(inst);
}

void Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled) {
  Instruction* inst = (Instruction*)inst_.raw();
  InstImm* i0 = (InstImm*)inst;
  InstImm* i1 = (InstImm*)i0->next();
  InstImm* i3 = (InstImm*)i1->next()->next();
  Instruction* i4 = (Instruction*)i3->next();

  MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
  MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));
  MOZ_ASSERT(i3->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

  if (enabled) {
    MOZ_ASSERT(i4->extractOpcode() != ((uint32_t)op_lui >> OpcodeShift));
    InstReg jalr = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
    *i4 = jalr;
  } else {
    InstNOP nop;
    *i4 = nop;
  }
}
