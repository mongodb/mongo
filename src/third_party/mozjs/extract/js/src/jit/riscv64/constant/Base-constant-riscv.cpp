// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "jit/riscv64/constant/Base-constant-riscv.h"

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

#include <stdio.h>

#include "jit/riscv64/constant/Constant-riscv-c.h"
#include "jit/riscv64/constant/Constant-riscv-d.h"
#include "jit/riscv64/constant/Constant-riscv-f.h"
#include "jit/riscv64/constant/Constant-riscv-i.h"
#include "jit/riscv64/constant/Constant-riscv-m.h"
#include "jit/riscv64/constant/Constant-riscv-v.h"
#include "jit/riscv64/constant/Constant-riscv-zicsr.h"
#include "jit/riscv64/constant/Constant-riscv-zifencei.h"
#include "jit/riscv64/Simulator-riscv64.h"
namespace js {
namespace jit {

int32_t ImmBranchMaxForwardOffset(OffsetSize bits) {
  return (1 << (bits - 1)) - 1;
}

bool InstructionBase::IsShortInstruction() const {
  uint8_t FirstByte = *reinterpret_cast<const uint8_t*>(this);
  return (FirstByte & 0x03) <= C2;
}

template <class T>
int InstructionGetters<T>::RvcRdValue() const {
  MOZ_ASSERT(this->IsShortInstruction());
  return this->Bits(kRvcRdShift + kRvcRdBits - 1, kRvcRdShift);
}

template <class T>
int InstructionGetters<T>::RvcRs2Value() const {
  MOZ_ASSERT(this->IsShortInstruction());
  return this->Bits(kRvcRs2Shift + kRvcRs2Bits - 1, kRvcRs2Shift);
}

template <class T>
int InstructionGetters<T>::RvcRs1sValue() const {
  MOZ_ASSERT(this->IsShortInstruction());
  return 0b1000 + this->Bits(kRvcRs1sShift + kRvcRs1sBits - 1, kRvcRs1sShift);
}

template <class T>
int InstructionGetters<T>::RvcRs2sValue() const {
  MOZ_ASSERT(this->IsShortInstruction());
  return 0b1000 + this->Bits(kRvcRs2sShift + kRvcRs2sBits - 1, kRvcRs2sShift);
}

template <class T>
inline int InstructionGetters<T>::RvcFunct6Value() const {
  MOZ_ASSERT(this->IsShortInstruction());
  return this->Bits(kRvcFunct6Shift + kRvcFunct6Bits - 1, kRvcFunct6Shift);
}

template <class T>
inline int InstructionGetters<T>::RvcFunct4Value() const {
  MOZ_ASSERT(this->IsShortInstruction());
  return this->Bits(kRvcFunct4Shift + kRvcFunct4Bits - 1, kRvcFunct4Shift);
}

template <class T>
inline int InstructionGetters<T>::RvcFunct3Value() const {
  MOZ_ASSERT(this->IsShortInstruction());
  return this->Bits(kRvcFunct3Shift + kRvcFunct3Bits - 1, kRvcFunct3Shift);
}

template <class T>
inline int InstructionGetters<T>::RvcFunct2Value() const {
  MOZ_ASSERT(this->IsShortInstruction());
  return this->Bits(kRvcFunct2Shift + kRvcFunct2Bits - 1, kRvcFunct2Shift);
}

template <class T>
inline int InstructionGetters<T>::RvcFunct2BValue() const {
  MOZ_ASSERT(this->IsShortInstruction());
  return this->Bits(kRvcFunct2BShift + kRvcFunct2Bits - 1, kRvcFunct2BShift);
}

template <class T>
uint32_t InstructionGetters<T>::Rvvzimm() const {
  if ((this->InstructionBits() &
       (kBaseOpcodeMask | kFunct3Mask | 0x80000000)) == RO_V_VSETVLI) {
    uint32_t Bits = this->InstructionBits();
    uint32_t zimm = Bits & kRvvZimmMask;
    return zimm >> kRvvZimmShift;
  } else {
    MOZ_ASSERT((this->InstructionBits() &
                (kBaseOpcodeMask | kFunct3Mask | 0xC0000000)) == RO_V_VSETIVLI);
    uint32_t Bits = this->InstructionBits();
    uint32_t zimm = Bits & kRvvZimmMask;
    return (zimm >> kRvvZimmShift) & 0x3FF;
  }
}

template <class T>
uint32_t InstructionGetters<T>::Rvvuimm() const {
  MOZ_ASSERT((this->InstructionBits() &
              (kBaseOpcodeMask | kFunct3Mask | 0xC0000000)) == RO_V_VSETIVLI);
  uint32_t Bits = this->InstructionBits();
  uint32_t uimm = Bits & kRvvUimmMask;
  return uimm >> kRvvUimmShift;
}

template class InstructionGetters<InstructionBase>;
#ifdef JS_SIMULATOR_RISCV64
template class InstructionGetters<SimInstructionBase>;
#endif

OffsetSize InstructionBase::GetOffsetSize() const {
  if (IsIllegalInstruction()) {
    MOZ_CRASH("IllegalInstruction");
  }
  if (IsShortInstruction()) {
    switch (InstructionBits() & kRvcOpcodeMask) {
      case RO_C_J:
        return kOffset11;
      case RO_C_BEQZ:
      case RO_C_BNEZ:
        return kOffset9;
      default:
        MOZ_CRASH("IllegalInstruction");
    }
  } else {
    switch (InstructionBits() & kBaseOpcodeMask) {
      case BRANCH:
        return kOffset13;
      case JAL:
        return kOffset21;
      default:
        MOZ_CRASH("IllegalInstruction");
    }
  }
}

InstructionBase::Type InstructionBase::InstructionType() const {
  if (IsIllegalInstruction()) {
    return kUnsupported;
  }
  // RV64C Instruction
  if (IsShortInstruction()) {
    switch (InstructionBits() & kRvcOpcodeMask) {
      case RO_C_ADDI4SPN:
        return kCIWType;
      case RO_C_FLD:
      case RO_C_LW:
#ifdef JS_CODEGEN_RISCV64
      case RO_C_LD:
#endif
        return kCLType;
      case RO_C_FSD:
      case RO_C_SW:
#ifdef JS_CODEGEN_RISCV64
      case RO_C_SD:
#endif
        return kCSType;
      case RO_C_NOP_ADDI:
      case RO_C_LI:
#ifdef JS_CODEGEN_RISCV64
      case RO_C_ADDIW:
#endif
      case RO_C_LUI_ADD:
        return kCIType;
      case RO_C_MISC_ALU:
        if (Bits(11, 10) != 0b11)
          return kCBType;
        else
          return kCAType;
      case RO_C_J:
        return kCJType;
      case RO_C_BEQZ:
      case RO_C_BNEZ:
        return kCBType;
      case RO_C_SLLI:
      case RO_C_FLDSP:
      case RO_C_LWSP:
#ifdef JS_CODEGEN_RISCV64
      case RO_C_LDSP:
#endif
        return kCIType;
      case RO_C_JR_MV_ADD:
        return kCRType;
      case RO_C_FSDSP:
      case RO_C_SWSP:
#ifdef JS_CODEGEN_RISCV64
      case RO_C_SDSP:
#endif
        return kCSSType;
      default:
        break;
    }
  } else {
    // RISCV routine
    switch (InstructionBits() & kBaseOpcodeMask) {
      case LOAD:
        return kIType;
      case LOAD_FP:
        return kIType;
      case MISC_MEM:
        return kIType;
      case OP_IMM:
        return kIType;
      case AUIPC:
        return kUType;
      case OP_IMM_32:
        return kIType;
      case STORE:
        return kSType;
      case STORE_FP:
        return kSType;
      case AMO:
        return kRType;
      case OP:
        return kRType;
      case LUI:
        return kUType;
      case OP_32:
        return kRType;
      case MADD:
      case MSUB:
      case NMSUB:
      case NMADD:
        return kR4Type;
      case OP_FP:
        return kRType;
      case BRANCH:
        return kBType;
      case JALR:
        return kIType;
      case JAL:
        return kJType;
      case SYSTEM:
        return kIType;
      case OP_V:
        return kVType;
    }
  }
  return kUnsupported;
}
}  // namespace jit
}  // namespace js
