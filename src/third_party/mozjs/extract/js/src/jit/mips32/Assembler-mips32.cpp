/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips32/Assembler-mips32.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include "jit/AutoWritableJitCode.h"

using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

ABIArgGenerator::ABIArgGenerator()
    : usedArgSlots_(0),
      firstArgFloatSize_(0),
      useGPRForFloats_(false),
      current_() {}

ABIArg ABIArgGenerator::next(MIRType type) {
  Register destReg;
  switch (type) {
    case MIRType::Int32:
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
    case MIRType::StackResults:
      if (GetIntArgReg(usedArgSlots_, &destReg)) {
        current_ = ABIArg(destReg);
      } else {
        current_ = ABIArg(usedArgSlots_ * sizeof(intptr_t));
      }
      usedArgSlots_++;
      break;
    case MIRType::Int64:
      if (!usedArgSlots_) {
        current_ = ABIArg(a0, a1);
        usedArgSlots_ = 2;
      } else if (usedArgSlots_ <= 2) {
        current_ = ABIArg(a2, a3);
        usedArgSlots_ = 4;
      } else {
        if (usedArgSlots_ < NumIntArgRegs) {
          usedArgSlots_ = NumIntArgRegs;
        }
        usedArgSlots_ += usedArgSlots_ % 2;
        current_ = ABIArg(usedArgSlots_ * sizeof(intptr_t));
        usedArgSlots_ += 2;
      }
      break;
    case MIRType::Float32:
      if (!usedArgSlots_) {
        current_ = ABIArg(f12.asSingle());
        firstArgFloatSize_ = 1;
      } else if (usedArgSlots_ == firstArgFloatSize_) {
        current_ = ABIArg(f14.asSingle());
      } else if (useGPRForFloats_ && GetIntArgReg(usedArgSlots_, &destReg)) {
        current_ = ABIArg(destReg);
      } else {
        if (usedArgSlots_ < NumIntArgRegs) {
          usedArgSlots_ = NumIntArgRegs;
        }
        current_ = ABIArg(usedArgSlots_ * sizeof(intptr_t));
      }
      usedArgSlots_++;
      break;
    case MIRType::Double:
      if (!usedArgSlots_) {
        current_ = ABIArg(f12);
        usedArgSlots_ = 2;
        firstArgFloatSize_ = 2;
      } else if (usedArgSlots_ == firstArgFloatSize_) {
        current_ = ABIArg(f14);
        usedArgSlots_ = 4;
      } else if (useGPRForFloats_ && usedArgSlots_ <= 2) {
        current_ = ABIArg(a2, a3);
        usedArgSlots_ = 4;
      } else {
        if (usedArgSlots_ < NumIntArgRegs) {
          usedArgSlots_ = NumIntArgRegs;
        }
        usedArgSlots_ += usedArgSlots_ % 2;
        current_ = ABIArg(usedArgSlots_ * sizeof(intptr_t));
        usedArgSlots_ += 2;
      }
      break;
    default:
      MOZ_CRASH("Unexpected argument type");
  }
  return current_;
}

uint32_t js::jit::RT(FloatRegister r) {
  MOZ_ASSERT(r.id() < FloatRegisters::RegisterIdLimit);
  return r.id() << RTShift;
}

uint32_t js::jit::RD(FloatRegister r) {
  MOZ_ASSERT(r.id() < FloatRegisters::RegisterIdLimit);
  return r.id() << RDShift;
}

uint32_t js::jit::RZ(FloatRegister r) {
  MOZ_ASSERT(r.id() < FloatRegisters::RegisterIdLimit);
  return r.id() << RZShift;
}

uint32_t js::jit::SA(FloatRegister r) {
  MOZ_ASSERT(r.id() < FloatRegisters::RegisterIdLimit);
  return r.id() << SAShift;
}

void Assembler::executableCopy(uint8_t* buffer) {
  MOZ_ASSERT(isFinished);
  m_buffer.executableCopy(buffer);
}

uintptr_t Assembler::GetPointer(uint8_t* instPtr) {
  Instruction* inst = (Instruction*)instPtr;
  return Assembler::ExtractLuiOriValue(inst, inst->next());
}

static JitCode* CodeFromJump(Instruction* jump) {
  uint8_t* target = (uint8_t*)Assembler::ExtractLuiOriValue(jump, jump->next());
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
  void* ptr = (void*)Assembler::ExtractLuiOriValue(inst, inst->next());
  void* prior = ptr;

  // No barrier needed since these are constants.
  TraceManuallyBarrieredGenericPointerEdge(
      trc, reinterpret_cast<gc::Cell**>(&ptr), "jit-masm-ptr");
  if (ptr != prior) {
    if (awjc.isNothing()) {
      awjc.emplace(code);
    }
    AssemblerMIPSShared::UpdateLuiOriValue(inst, inst->next(), uint32_t(ptr));
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

Assembler::Condition Assembler::UnsignedCondition(Condition cond) {
  switch (cond) {
    case Zero:
    case NonZero:
      return cond;
    case LessThan:
    case Below:
      return Below;
    case LessThanOrEqual:
    case BelowOrEqual:
      return BelowOrEqual;
    case GreaterThan:
    case Above:
      return Above;
    case AboveOrEqual:
    case GreaterThanOrEqual:
      return AboveOrEqual;
    default:
      MOZ_CRASH("unexpected condition");
  }
}

Assembler::Condition Assembler::ConditionWithoutEqual(Condition cond) {
  switch (cond) {
    case LessThan:
    case LessThanOrEqual:
      return LessThan;
    case Below:
    case BelowOrEqual:
      return Below;
    case GreaterThan:
    case GreaterThanOrEqual:
      return GreaterThan;
    case Above:
    case AboveOrEqual:
      return Above;
    default:
      MOZ_CRASH("unexpected condition");
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
      AssemblerMIPSShared::UpdateLuiOriValue(inst, inst->next(),
                                             (uint32_t)(rawCode + target));
    }
  }
}

void Assembler::bind(InstImm* inst, uintptr_t branch, uintptr_t target) {
  int32_t offset = target - branch;
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
    Assembler::WriteLuiOriInstructions(inst, &inst[1], ScratchRegister,
                                       LabelBase::INVALID_OFFSET);
    inst[2] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr).encode();
    // There is 1 nop after this.
    return;
  }

  if (BOffImm16::IsInRange(offset)) {
    bool conditional = (inst[0].encode() != inst_bgezal.encode() &&
                        inst[0].encode() != inst_beq.encode());

    inst[0].setBOffImm16(BOffImm16(offset));
    inst[1].makeNop();

    // Skip the trailing nops in conditional branches.
    if (conditional) {
      inst[2] = InstImm(op_regimm, zero, rt_bgez, BOffImm16(3 * sizeof(void*)))
                    .encode();
      // There are 2 nops after this
    }
    return;
  }

  if (inst[0].encode() == inst_beq.encode()) {
    // Handle long unconditional jump.
    addLongJump(BufferOffset(branch), BufferOffset(target));
    Assembler::WriteLuiOriInstructions(inst, &inst[1], ScratchRegister,
                                       LabelBase::INVALID_OFFSET);
    inst[2] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
    // There is 1 nop after this.
  } else {
    // Handle long conditional jump.
    inst[0] = invertBranch(inst[0], BOffImm16(5 * sizeof(void*)));
    // No need for a "nop" here because we can clobber scratch.
    addLongJump(BufferOffset(branch + sizeof(void*)), BufferOffset(target));
    Assembler::WriteLuiOriInstructions(&inst[1], &inst[2], ScratchRegister,
                                       LabelBase::INVALID_OFFSET);
    inst[3] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
    // There is 1 nop after this.
  }
}

void Assembler::processCodeLabels(uint8_t* rawCode) {
  for (const CodeLabel& label : codeLabels_) {
    Bind(rawCode, label);
  }
}

uint32_t Assembler::PatchWrite_NearCallSize() { return 4 * sizeof(uint32_t); }

void Assembler::PatchWrite_NearCall(CodeLocationLabel start,
                                    CodeLocationLabel toCall) {
  Instruction* inst = (Instruction*)start.raw();
  uint8_t* dest = toCall.raw();

  // Overwrite whatever instruction used to be here with a call.
  // Always use long jump for two reasons:
  // - Jump has to be the same size because of PatchWrite_NearCallSize.
  // - Return address has to be at the end of replaced block.
  // Short jump wouldn't be more efficient.
  Assembler::WriteLuiOriInstructions(inst, &inst[1], ScratchRegister,
                                     (uint32_t)dest);
  inst[2] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
  inst[3] = InstNOP();
}

uint32_t Assembler::ExtractLuiOriValue(Instruction* inst0, Instruction* inst1) {
  InstImm* i0 = (InstImm*)inst0;
  InstImm* i1 = (InstImm*)inst1;
  MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
  MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

  uint32_t value = i0->extractImm16Value() << 16;
  value = value | i1->extractImm16Value();
  return value;
}

void Assembler::WriteLuiOriInstructions(Instruction* inst0, Instruction* inst1,
                                        Register reg, uint32_t value) {
  *inst0 = InstImm(op_lui, zero, reg, Imm16::Upper(Imm32(value)));
  *inst1 = InstImm(op_ori, reg, reg, Imm16::Lower(Imm32(value)));
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
  DebugOnly<uint32_t> value = Assembler::ExtractLuiOriValue(&inst[0], &inst[1]);
  MOZ_ASSERT(value == uint32_t(expectedValue.value));

  // Replace with new value
  AssemblerMIPSShared::UpdateLuiOriValue(inst, inst->next(),
                                         uint32_t(newValue.value));
}

uint32_t Assembler::ExtractInstructionImmediate(uint8_t* code) {
  InstImm* inst = (InstImm*)code;
  return Assembler::ExtractLuiOriValue(inst, inst->next());
}

void Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled) {
  Instruction* inst = (Instruction*)inst_.raw();
  InstImm* i0 = (InstImm*)inst;
  InstImm* i1 = (InstImm*)i0->next();
  Instruction* i2 = (Instruction*)i1->next();

  MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
  MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

  if (enabled) {
    InstReg jalr = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
    *i2 = jalr;
  } else {
    InstNOP nop;
    *i2 = nop;
  }
}
