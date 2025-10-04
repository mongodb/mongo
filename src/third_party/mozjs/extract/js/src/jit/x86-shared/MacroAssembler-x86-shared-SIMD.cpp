/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MacroAssembler.h"
#include "jit/x86-shared/MacroAssembler-x86-shared.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;
using mozilla::FloatingPoint;
using mozilla::Maybe;
using mozilla::SpecificNaN;

void MacroAssemblerX86Shared::splatX16(Register input, FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());

  vmovd(input, output);
  if (HasAVX2()) {
    vbroadcastb(Operand(output), output);
    return;
  }
  vpxor(scratch, scratch, scratch);
  vpshufb(scratch, output, output);
}

void MacroAssemblerX86Shared::splatX8(Register input, FloatRegister output) {
  vmovd(input, output);
  if (HasAVX2()) {
    vbroadcastw(Operand(output), output);
    return;
  }
  vpshuflw(0, output, output);
  vpshufd(0, output, output);
}

void MacroAssemblerX86Shared::splatX4(Register input, FloatRegister output) {
  vmovd(input, output);
  if (HasAVX2()) {
    vbroadcastd(Operand(output), output);
    return;
  }
  vpshufd(0, output, output);
}

void MacroAssemblerX86Shared::splatX4(FloatRegister input,
                                      FloatRegister output) {
  MOZ_ASSERT(input.isSingle() && output.isSimd128());
  if (HasAVX2()) {
    vbroadcastss(Operand(input), output);
    return;
  }
  input = asMasm().moveSimd128FloatIfNotAVX(input.asSimd128(), output);
  vshufps(0, input, input, output);
}

void MacroAssemblerX86Shared::splatX2(FloatRegister input,
                                      FloatRegister output) {
  MOZ_ASSERT(input.isDouble() && output.isSimd128());
  vmovddup(Operand(input.asSimd128()), output);
}

void MacroAssemblerX86Shared::extractLaneInt32x4(FloatRegister input,
                                                 Register output,
                                                 unsigned lane) {
  if (lane == 0) {
    // The value we want to extract is in the low double-word
    moveLowInt32(input, output);
  } else {
    vpextrd(lane, input, output);
  }
}

void MacroAssemblerX86Shared::extractLaneFloat32x4(FloatRegister input,
                                                   FloatRegister output,
                                                   unsigned lane) {
  MOZ_ASSERT(input.isSimd128() && output.isSingle());
  if (lane == 0) {
    // The value we want to extract is in the low double-word
    if (input.asSingle() != output) {
      moveFloat32(input, output);
    }
  } else if (lane == 2) {
    moveHighPairToLowPairFloat32(input, output);
  } else {
    uint32_t mask = MacroAssembler::ComputeShuffleMask(lane);
    FloatRegister dest = output.asSimd128();
    input = moveSimd128FloatIfNotAVX(input, dest);
    vshufps(mask, input, input, dest);
  }
}

void MacroAssemblerX86Shared::extractLaneFloat64x2(FloatRegister input,
                                                   FloatRegister output,
                                                   unsigned lane) {
  MOZ_ASSERT(input.isSimd128() && output.isDouble());
  if (lane == 0) {
    // The value we want to extract is in the low quadword
    if (input.asDouble() != output) {
      moveDouble(input, output);
    }
  } else {
    vpalignr(Operand(input), output, output, 8);
  }
}

void MacroAssemblerX86Shared::extractLaneInt16x8(FloatRegister input,
                                                 Register output, unsigned lane,
                                                 SimdSign sign) {
  vpextrw(lane, input, Operand(output));
  if (sign == SimdSign::Signed) {
    movswl(output, output);
  }
}

void MacroAssemblerX86Shared::extractLaneInt8x16(FloatRegister input,
                                                 Register output, unsigned lane,
                                                 SimdSign sign) {
  vpextrb(lane, input, Operand(output));
  if (sign == SimdSign::Signed) {
    if (!AllocatableGeneralRegisterSet(Registers::SingleByteRegs).has(output)) {
      xchgl(eax, output);
      movsbl(eax, eax);
      xchgl(eax, output);
    } else {
      movsbl(output, output);
    }
  }
}

void MacroAssemblerX86Shared::replaceLaneFloat32x4(unsigned lane,
                                                   FloatRegister lhs,
                                                   FloatRegister rhs,
                                                   FloatRegister dest) {
  MOZ_ASSERT(lhs.isSimd128() && rhs.isSingle());

  if (lane == 0) {
    if (rhs.asSimd128() == lhs) {
      // no-op, although this should not normally happen for type checking
      // reasons higher up in the stack.
      moveSimd128Float(lhs, dest);
    } else {
      // move low dword of value into low dword of output
      vmovss(rhs, lhs, dest);
    }
  } else {
    vinsertps(vinsertpsMask(0, lane), rhs, lhs, dest);
  }
}

void MacroAssemblerX86Shared::replaceLaneFloat64x2(unsigned lane,
                                                   FloatRegister lhs,
                                                   FloatRegister rhs,
                                                   FloatRegister dest) {
  MOZ_ASSERT(lhs.isSimd128() && rhs.isDouble());

  if (lane == 0) {
    if (rhs.asSimd128() == lhs) {
      // no-op, although this should not normally happen for type checking
      // reasons higher up in the stack.
      moveSimd128Float(lhs, dest);
    } else {
      // move low qword of value into low qword of output
      vmovsd(rhs, lhs, dest);
    }
  } else {
    // move low qword of value into high qword of output
    vshufpd(0, rhs, lhs, dest);
  }
}

void MacroAssemblerX86Shared::blendInt8x16(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister output,
                                           FloatRegister temp,
                                           const uint8_t lanes[16]) {
  asMasm().loadConstantSimd128Int(
      SimdConstant::CreateX16(reinterpret_cast<const int8_t*>(lanes)), temp);
  vpblendvb(temp, rhs, lhs, output);
}

void MacroAssemblerX86Shared::blendInt16x8(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister output,
                                           const uint16_t lanes[8]) {
  uint32_t mask = 0;
  for (unsigned i = 0; i < 8; i++) {
    if (lanes[i]) {
      mask |= (1 << i);
    }
  }
  vpblendw(mask, rhs, lhs, output);
}

void MacroAssemblerX86Shared::laneSelectSimd128(FloatRegister mask,
                                                FloatRegister lhs,
                                                FloatRegister rhs,
                                                FloatRegister output) {
  vpblendvb(mask, lhs, rhs, output);
}

void MacroAssemblerX86Shared::shuffleInt8x16(FloatRegister lhs,
                                             FloatRegister rhs,
                                             FloatRegister output,
                                             const uint8_t lanes[16]) {
  ScratchSimd128Scope scratch(asMasm());

  // Use pshufb instructions to gather the lanes from each source vector.
  // A negative index creates a zero lane, so the two vectors can be combined.

  // Set scratch = lanes from rhs.
  int8_t idx[16];
  for (unsigned i = 0; i < 16; i++) {
    idx[i] = lanes[i] >= 16 ? lanes[i] - 16 : -1;
  }
  rhs = moveSimd128IntIfNotAVX(rhs, scratch);
  asMasm().vpshufbSimd128(SimdConstant::CreateX16(idx), rhs, scratch);

  // Set output = lanes from lhs.
  for (unsigned i = 0; i < 16; i++) {
    idx[i] = lanes[i] < 16 ? lanes[i] : -1;
  }
  lhs = moveSimd128IntIfNotAVX(lhs, output);
  asMasm().vpshufbSimd128(SimdConstant::CreateX16(idx), lhs, output);

  // Combine.
  vpor(scratch, output, output);
}

static inline FloatRegister ToSimdFloatRegister(const Operand& op) {
  return FloatRegister(op.fpu(), FloatRegister::Codes::ContentType::Simd128);
}

void MacroAssemblerX86Shared::compareInt8x16(FloatRegister lhs, Operand rhs,
                                             Assembler::Condition cond,
                                             FloatRegister output) {
  switch (cond) {
    case Assembler::Condition::GreaterThan:
      vpcmpgtb(rhs, lhs, output);
      break;
    case Assembler::Condition::Equal:
      vpcmpeqb(rhs, lhs, output);
      break;
    case Assembler::Condition::LessThan: {
      ScratchSimd128Scope scratch(asMasm());
      if (lhs == output) {
        moveSimd128Int(lhs, scratch);
        lhs = scratch;
      }
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), output);
      } else {
        loadAlignedSimd128Int(rhs, output);
      }
      vpcmpgtb(Operand(lhs), output, output);
      break;
    }
    case Assembler::Condition::NotEqual:
      vpcmpeqb(rhs, lhs, output);
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual: {
      ScratchSimd128Scope scratch(asMasm());
      if (lhs == output) {
        moveSimd128Int(lhs, scratch);
        lhs = scratch;
      }
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), output);
      } else {
        loadAlignedSimd128Int(rhs, output);
      }
      vpcmpgtb(Operand(lhs), output, output);
    }
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::Condition::LessThanOrEqual:
      // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
      vpcmpgtb(rhs, lhs, output);
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::Above:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpminub(rhs, lhs, output);
        vpcmpeqb(Operand(lhs), output, output);
      } else {
        vpmaxub(rhs, lhs, output);
        vpcmpeqb(rhs, output, output);
      }
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::BelowOrEqual:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpminub(rhs, lhs, output);
        vpcmpeqb(Operand(lhs), output, output);
      } else {
        vpmaxub(rhs, lhs, output);
        vpcmpeqb(rhs, output, output);
      }
      break;
    case Assembler::Below:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpmaxub(rhs, lhs, output);
        vpcmpeqb(Operand(lhs), output, output);
      } else {
        vpminub(rhs, lhs, output);
        vpcmpeqb(rhs, output, output);
      }
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::AboveOrEqual:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpmaxub(rhs, lhs, output);
        vpcmpeqb(Operand(lhs), output, output);
      } else {
        vpminub(rhs, lhs, output);
        vpcmpeqb(rhs, output, output);
      }
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareInt8x16(Assembler::Condition cond,
                                             FloatRegister lhs,
                                             const SimdConstant& rhs,
                                             FloatRegister dest) {
  bool complement = false;
  switch (cond) {
    case Assembler::Condition::NotEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::Equal:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vpcmpeqb,
                    &MacroAssembler::vpcmpeqbSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::GreaterThan:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vpcmpgtb,
                    &MacroAssembler::vpcmpgtbSimd128);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
  if (complement) {
    asMasm().bitwiseXorSimd128(dest, SimdConstant::SplatX16(-1), dest);
  }
}

void MacroAssemblerX86Shared::compareInt16x8(FloatRegister lhs, Operand rhs,
                                             Assembler::Condition cond,
                                             FloatRegister output) {
  switch (cond) {
    case Assembler::Condition::GreaterThan:
      vpcmpgtw(rhs, lhs, output);
      break;
    case Assembler::Condition::Equal:
      vpcmpeqw(rhs, lhs, output);
      break;
    case Assembler::Condition::LessThan: {
      ScratchSimd128Scope scratch(asMasm());
      if (lhs == output) {
        moveSimd128Int(lhs, scratch);
        lhs = scratch;
      }
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), output);
      } else {
        loadAlignedSimd128Int(rhs, output);
      }
      vpcmpgtw(Operand(lhs), output, output);
      break;
    }
    case Assembler::Condition::NotEqual:
      vpcmpeqw(rhs, lhs, output);
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual: {
      ScratchSimd128Scope scratch(asMasm());
      if (lhs == output) {
        moveSimd128Int(lhs, scratch);
        lhs = scratch;
      }
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), output);
      } else {
        loadAlignedSimd128Int(rhs, output);
      }
      vpcmpgtw(Operand(lhs), output, output);
    }
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::Condition::LessThanOrEqual:
      // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
      vpcmpgtw(rhs, lhs, output);
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::Above:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpminuw(rhs, lhs, output);
        vpcmpeqw(Operand(lhs), output, output);
      } else {
        vpmaxuw(rhs, lhs, output);
        vpcmpeqw(rhs, output, output);
      }
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::BelowOrEqual:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpminuw(rhs, lhs, output);
        vpcmpeqw(Operand(lhs), output, output);
      } else {
        vpmaxuw(rhs, lhs, output);
        vpcmpeqw(rhs, output, output);
      }
      break;
    case Assembler::Below:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpmaxuw(rhs, lhs, output);
        vpcmpeqw(Operand(lhs), output, output);
      } else {
        vpminuw(rhs, lhs, output);
        vpcmpeqw(rhs, output, output);
      }
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::AboveOrEqual:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpmaxuw(rhs, lhs, output);
        vpcmpeqw(Operand(lhs), output, output);
      } else {
        vpminuw(rhs, lhs, output);
        vpcmpeqw(rhs, output, output);
      }
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareInt16x8(Assembler::Condition cond,
                                             FloatRegister lhs,
                                             const SimdConstant& rhs,
                                             FloatRegister dest) {
  bool complement = false;
  switch (cond) {
    case Assembler::Condition::NotEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::Equal:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vpcmpeqw,
                    &MacroAssembler::vpcmpeqwSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::GreaterThan:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vpcmpgtw,
                    &MacroAssembler::vpcmpgtwSimd128);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
  if (complement) {
    asMasm().bitwiseXorSimd128(dest, SimdConstant::SplatX16(-1), dest);
  }
}

void MacroAssemblerX86Shared::compareInt32x4(FloatRegister lhs, Operand rhs,
                                             Assembler::Condition cond,
                                             FloatRegister output) {
  switch (cond) {
    case Assembler::Condition::GreaterThan:
      vpcmpgtd(rhs, lhs, output);
      break;
    case Assembler::Condition::Equal:
      vpcmpeqd(rhs, lhs, output);
      break;
    case Assembler::Condition::LessThan: {
      ScratchSimd128Scope scratch(asMasm());
      if (lhs == output) {
        moveSimd128Int(lhs, scratch);
        lhs = scratch;
      }
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), output);
      } else {
        loadAlignedSimd128Int(rhs, output);
      }
      vpcmpgtd(Operand(lhs), output, output);
      break;
    }
    case Assembler::Condition::NotEqual:
      vpcmpeqd(rhs, lhs, output);
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual: {
      ScratchSimd128Scope scratch(asMasm());
      if (lhs == output) {
        moveSimd128Int(lhs, scratch);
        lhs = scratch;
      }
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), output);
      } else {
        loadAlignedSimd128Int(rhs, output);
      }
      vpcmpgtd(Operand(lhs), output, output);
    }
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::Condition::LessThanOrEqual:
      // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
      vpcmpgtd(rhs, lhs, output);
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::Above:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpminud(rhs, lhs, output);
        vpcmpeqd(Operand(lhs), output, output);
      } else {
        vpmaxud(rhs, lhs, output);
        vpcmpeqd(rhs, output, output);
      }
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::BelowOrEqual:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpminud(rhs, lhs, output);
        vpcmpeqd(Operand(lhs), output, output);
      } else {
        vpmaxud(rhs, lhs, output);
        vpcmpeqd(rhs, output, output);
      }
      break;
    case Assembler::Below:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpmaxud(rhs, lhs, output);
        vpcmpeqd(Operand(lhs), output, output);
      } else {
        vpminud(rhs, lhs, output);
        vpcmpeqd(rhs, output, output);
      }
      asMasm().bitwiseNotSimd128(output, output);
      break;
    case Assembler::AboveOrEqual:
      if (rhs.kind() == Operand::FPREG && ToSimdFloatRegister(rhs) == output) {
        vpmaxud(rhs, lhs, output);
        vpcmpeqd(Operand(lhs), output, output);
      } else {
        vpminud(rhs, lhs, output);
        vpcmpeqd(rhs, output, output);
      }
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareInt32x4(Assembler::Condition cond,
                                             FloatRegister lhs,
                                             const SimdConstant& rhs,
                                             FloatRegister dest) {
  bool complement = false;
  switch (cond) {
    case Assembler::Condition::NotEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::Equal:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vpcmpeqd,
                    &MacroAssembler::vpcmpeqdSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::GreaterThan:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vpcmpgtd,
                    &MacroAssembler::vpcmpgtdSimd128);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
  if (complement) {
    asMasm().bitwiseXorSimd128(dest, SimdConstant::SplatX16(-1), dest);
  }
}

void MacroAssemblerX86Shared::compareForEqualityInt64x2(
    FloatRegister lhs, Operand rhs, Assembler::Condition cond,
    FloatRegister output) {
  static const SimdConstant allOnes = SimdConstant::SplatX4(-1);
  switch (cond) {
    case Assembler::Condition::Equal:
      vpcmpeqq(rhs, lhs, output);
      break;
    case Assembler::Condition::NotEqual:
      vpcmpeqq(rhs, lhs, output);
      asMasm().bitwiseXorSimd128(output, allOnes, output);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareForOrderingInt64x2(
    FloatRegister lhs, Operand rhs, Assembler::Condition cond,
    FloatRegister temp1, FloatRegister temp2, FloatRegister output) {
  static const SimdConstant allOnes = SimdConstant::SplatX4(-1);
  // The pseudo code is for (e.g. > comparison):
  //  __m128i pcmpgtq_sse2 (__m128i a, __m128i b) {
  //    __m128i r = _mm_and_si128(_mm_cmpeq_epi32(a, b), _mm_sub_epi64(b, a));
  //    r = _mm_or_si128(r, _mm_cmpgt_epi32(a, b));
  //    return _mm_shuffle_epi32(r, _MM_SHUFFLE(3,3,1,1));
  //  }
  // Credits to https://stackoverflow.com/a/65175746
  switch (cond) {
    case Assembler::Condition::GreaterThan:
      vmovdqa(rhs, temp1);
      vmovdqa(Operand(lhs), temp2);
      vpsubq(Operand(lhs), temp1, temp1);
      vpcmpeqd(rhs, temp2, temp2);
      vandpd(temp2, temp1, temp1);
      lhs = asMasm().moveSimd128IntIfNotAVX(lhs, output);
      vpcmpgtd(rhs, lhs, output);
      vpor(Operand(temp1), output, output);
      vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), output, output);
      break;
    case Assembler::Condition::LessThan:
      vmovdqa(rhs, temp1);
      vmovdqa(Operand(lhs), temp2);
      vpcmpgtd(Operand(lhs), temp1, temp1);
      vpcmpeqd(Operand(rhs), temp2, temp2);
      lhs = asMasm().moveSimd128IntIfNotAVX(lhs, output);
      vpsubq(rhs, lhs, output);
      vandpd(temp2, output, output);
      vpor(Operand(temp1), output, output);
      vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), output, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual:
      vmovdqa(rhs, temp1);
      vmovdqa(Operand(lhs), temp2);
      vpcmpgtd(Operand(lhs), temp1, temp1);
      vpcmpeqd(Operand(rhs), temp2, temp2);
      lhs = asMasm().moveSimd128IntIfNotAVX(lhs, output);
      vpsubq(rhs, lhs, output);
      vandpd(temp2, output, output);
      vpor(Operand(temp1), output, output);
      vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), output, output);
      asMasm().bitwiseXorSimd128(output, allOnes, output);
      break;
    case Assembler::Condition::LessThanOrEqual:
      vmovdqa(rhs, temp1);
      vmovdqa(Operand(lhs), temp2);
      vpsubq(Operand(lhs), temp1, temp1);
      vpcmpeqd(rhs, temp2, temp2);
      vandpd(temp2, temp1, temp1);
      lhs = asMasm().moveSimd128IntIfNotAVX(lhs, output);
      vpcmpgtd(rhs, lhs, output);
      vpor(Operand(temp1), output, output);
      vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), output, output);
      asMasm().bitwiseXorSimd128(output, allOnes, output);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareForOrderingInt64x2AVX(
    FloatRegister lhs, FloatRegister rhs, Assembler::Condition cond,
    FloatRegister output) {
  MOZ_ASSERT(HasSSE42());
  static const SimdConstant allOnes = SimdConstant::SplatX4(-1);
  switch (cond) {
    case Assembler::Condition::GreaterThan:
      vpcmpgtq(Operand(rhs), lhs, output);
      break;
    case Assembler::Condition::LessThan:
      vpcmpgtq(Operand(lhs), rhs, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual:
      vpcmpgtq(Operand(lhs), rhs, output);
      asMasm().bitwiseXorSimd128(output, allOnes, output);
      break;
    case Assembler::Condition::LessThanOrEqual:
      vpcmpgtq(Operand(rhs), lhs, output);
      asMasm().bitwiseXorSimd128(output, allOnes, output);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareFloat32x4(FloatRegister lhs, Operand rhs,
                                               Assembler::Condition cond,
                                               FloatRegister output) {
  // TODO Can do better here with three-address compares

  // Move lhs to output if lhs!=output; move rhs out of the way if rhs==output.
  // This is bad, but Ion does not need this fixup.
  ScratchSimd128Scope scratch(asMasm());
  if (!HasAVX() && !lhs.aliases(output)) {
    if (rhs.kind() == Operand::FPREG &&
        output.aliases(FloatRegister::FromCode(rhs.fpu()))) {
      vmovaps(rhs, scratch);
      rhs = Operand(scratch);
    }
    vmovaps(lhs, output);
    lhs = output;
  }

  switch (cond) {
    case Assembler::Condition::Equal:
      vcmpeqps(rhs, lhs, output);
      break;
    case Assembler::Condition::LessThan:
      vcmpltps(rhs, lhs, output);
      break;
    case Assembler::Condition::LessThanOrEqual:
      vcmpleps(rhs, lhs, output);
      break;
    case Assembler::Condition::NotEqual:
      vcmpneqps(rhs, lhs, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual:
    case Assembler::Condition::GreaterThan:
      // We reverse these operations in the -inl.h file so that we don't have to
      // copy into and out of temporaries after codegen.
      MOZ_CRASH("should have reversed this");
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareFloat32x4(Assembler::Condition cond,
                                               FloatRegister lhs,
                                               const SimdConstant& rhs,
                                               FloatRegister dest) {
  switch (cond) {
    case Assembler::Condition::Equal:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vcmpeqps,
                    &MacroAssembler::vcmpeqpsSimd128);
      break;
    case Assembler::Condition::LessThan:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vcmpltps,
                    &MacroAssembler::vcmpltpsSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vcmpleps,
                    &MacroAssembler::vcmplepsSimd128);
      break;
    case Assembler::Condition::NotEqual:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vcmpneqps,
                    &MacroAssembler::vcmpneqpsSimd128);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareFloat64x2(FloatRegister lhs, Operand rhs,
                                               Assembler::Condition cond,
                                               FloatRegister output) {
  // TODO Can do better here with three-address compares

  // Move lhs to output if lhs!=output; move rhs out of the way if rhs==output.
  // This is bad, but Ion does not need this fixup.
  ScratchSimd128Scope scratch(asMasm());
  if (!HasAVX() && !lhs.aliases(output)) {
    if (rhs.kind() == Operand::FPREG &&
        output.aliases(FloatRegister::FromCode(rhs.fpu()))) {
      vmovapd(rhs, scratch);
      rhs = Operand(scratch);
    }
    vmovapd(lhs, output);
    lhs = output;
  }

  switch (cond) {
    case Assembler::Condition::Equal:
      vcmpeqpd(rhs, lhs, output);
      break;
    case Assembler::Condition::LessThan:
      vcmpltpd(rhs, lhs, output);
      break;
    case Assembler::Condition::LessThanOrEqual:
      vcmplepd(rhs, lhs, output);
      break;
    case Assembler::Condition::NotEqual:
      vcmpneqpd(rhs, lhs, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual:
    case Assembler::Condition::GreaterThan:
      // We reverse these operations in the -inl.h file so that we don't have to
      // copy into and out of temporaries after codegen.
      MOZ_CRASH("should have reversed this");
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareFloat64x2(Assembler::Condition cond,
                                               FloatRegister lhs,
                                               const SimdConstant& rhs,
                                               FloatRegister dest) {
  switch (cond) {
    case Assembler::Condition::Equal:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vcmpeqpd,
                    &MacroAssembler::vcmpeqpdSimd128);
      break;
    case Assembler::Condition::LessThan:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vcmpltpd,
                    &MacroAssembler::vcmpltpdSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vcmplepd,
                    &MacroAssembler::vcmplepdSimd128);
      break;
    case Assembler::Condition::NotEqual:
      binarySimd128(lhs, rhs, dest, &MacroAssembler::vcmpneqpd,
                    &MacroAssembler::vcmpneqpdSimd128);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

// Semantics of wasm max and min.
//
//  * -0 < 0
//  * If one input is NaN then that NaN is the output
//  * If both inputs are NaN then the output is selected nondeterministically
//  * Any returned NaN is always made quiet
//  * The MVP spec 2.2.3 says "No distinction is made between signalling and
//    quiet NaNs", suggesting SNaN inputs are allowed and should not fault
//
// Semantics of maxps/minps/maxpd/minpd:
//
//  * If the values are both +/-0 the rhs is returned
//  * If the rhs is SNaN then the rhs is returned
//  * If either value is NaN then the rhs is returned
//  * An SNaN operand does not appear to give rise to an exception, at least
//    not in the JS shell on Linux, though the Intel spec lists Invalid
//    as one of the possible exceptions

// Various unaddressed considerations:
//
// It's pretty insane for this to take an Operand rhs - it really needs to be
// a register, given the number of times we access it.
//
// Constant load can be folded into the ANDPS.  Do we care?  It won't save us
// any registers, since output/temp1/temp2/scratch are all live at the same time
// after the first instruction of the slow path.
//
// Can we use blend for the NaN extraction/insertion?  We'd need xmm0 for the
// mask, which is no fun.  But it would be lhs UNORD lhs -> mask, blend;
// rhs UNORD rhs -> mask; blend.  Better than the mess we have below.  But
// we'd still need to setup the QNaN bits, unless we can blend those too
// with the lhs UNORD rhs mask?
//
// If we could determine that both input lanes are NaN then the result of the
// fast path should be fine modulo the QNaN bits, but it's not obvious this is
// much of an advantage.

void MacroAssemblerX86Shared::minMaxFloat32x4(bool isMin, FloatRegister lhs,
                                              Operand rhs, FloatRegister temp1,
                                              FloatRegister temp2,
                                              FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  Label l;
  SimdConstant quietBits(SimdConstant::SplatX4(int32_t(0x00400000)));

  /* clang-format off */ /* leave my comments alone */
  lhs = moveSimd128FloatIfNotAVXOrOther(lhs, scratch, output);
  if (isMin) {
    vmovaps(lhs, output);                    // compute
    vminps(rhs, output, output);             //   min lhs, rhs
    vmovaps(rhs, temp1);                     // compute
    vminps(Operand(lhs), temp1, temp1);      //   min rhs, lhs
    vorps(temp1, output, output);            // fix min(-0, 0) with OR
  } else {
    vmovaps(lhs, output);                    // compute
    vmaxps(rhs, output, output);             //   max lhs, rhs
    vmovaps(rhs, temp1);                     // compute
    vmaxps(Operand(lhs), temp1, temp1);      //   max rhs, lhs
    vandps(temp1, output, output);           // fix max(-0, 0) with AND
  }
  vmovaps(lhs, temp1);                       // compute
  vcmpunordps(rhs, temp1, temp1);            //   lhs UNORD rhs
  vptest(temp1, temp1);                      // check if any unordered
  j(Assembler::Equal, &l);                   //   and exit if not

  // Slow path.
  // output has result for non-NaN lanes, garbage in NaN lanes.
  // temp1 has lhs UNORD rhs.
  // temp2 is dead.

  vmovaps(temp1, temp2);                     // clear NaN lanes of result
  vpandn(output, temp2, temp2);              //   result now in temp2
  asMasm().vpandSimd128(quietBits, temp1, temp1);   // setup QNaN bits in NaN lanes
  vorps(temp1, temp2, temp2);                //   and OR into result
  vmovaps(lhs, temp1);                       // find NaN lanes
  vcmpunordps(Operand(temp1), temp1, temp1); //   in lhs
  vmovaps(temp1, output);                    //     (and save them for later)
  vandps(lhs, temp1, temp1);                 //       and extract the NaNs
  vorps(temp1, temp2, temp2);                //         and add to the result
  vmovaps(rhs, temp1);                       // find NaN lanes
  vcmpunordps(Operand(temp1), temp1, temp1); //   in rhs
  vpandn(temp1, output, output);             //     except if they were in lhs
  vandps(rhs, output, output);               //       and extract the NaNs
  vorps(temp2, output, output);              //         and add to the result

  bind(&l);
  /* clang-format on */
}

void MacroAssemblerX86Shared::minMaxFloat32x4AVX(bool isMin, FloatRegister lhs,
                                                 FloatRegister rhs,
                                                 FloatRegister temp1,
                                                 FloatRegister temp2,
                                                 FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  Label l;
  SimdConstant quietBits(SimdConstant::SplatX4(int32_t(0x00400000)));

  /* clang-format off */ /* leave my comments alone */
  FloatRegister lhsCopy = moveSimd128FloatIfEqual(lhs, scratch, output);
  // Allow rhs be assigned to scratch when rhs == lhs and == output --
  // don't make a special case since the semantics require setup QNaN bits.
  FloatRegister rhsCopy = moveSimd128FloatIfEqual(rhs, scratch, output);
  if (isMin) {
    vminps(Operand(rhs), lhs, temp2);             // min lhs, rhs
    vminps(Operand(lhs), rhs, temp1);             // min rhs, lhs
    vorps(temp1, temp2, output);                  // fix min(-0, 0) with OR
  } else {
    vmaxps(Operand(rhs), lhs, temp2);             // max lhs, rhs
    vmaxps(Operand(lhs), rhs, temp1);             // max rhs, lhs
    vandps(temp1, temp2, output);                 // fix max(-0, 0) with AND
  }
  vcmpunordps(Operand(rhsCopy), lhsCopy, temp1);  // lhs UNORD rhs
  vptest(temp1, temp1);                           // check if any unordered
  j(Assembler::Equal, &l);                        //   and exit if not

  // Slow path.
  // output has result for non-NaN lanes, garbage in NaN lanes.
  // temp1 has lhs UNORD rhs.
  // temp2 is dead.
  vcmpunordps(Operand(lhsCopy), lhsCopy, temp2);  // find NaN lanes in lhs
  vblendvps(temp2, lhsCopy, rhsCopy, temp2);      //   add other lines from rhs
  asMasm().vporSimd128(quietBits, temp2, temp2);  // setup QNaN bits in NaN lanes
  vblendvps(temp1, temp2, output, output);        // replace NaN lines from temp2

  bind(&l);
  /* clang-format on */
}

// Exactly as above.
void MacroAssemblerX86Shared::minMaxFloat64x2(bool isMin, FloatRegister lhs,
                                              Operand rhs, FloatRegister temp1,
                                              FloatRegister temp2,
                                              FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  Label l;
  SimdConstant quietBits(SimdConstant::SplatX2(int64_t(0x0008000000000000ull)));

  /* clang-format off */ /* leave my comments alone */
  lhs = moveSimd128FloatIfNotAVXOrOther(lhs, scratch, output);
  if (isMin) {
    vmovapd(lhs, output);                    // compute
    vminpd(rhs, output, output);             //   min lhs, rhs
    vmovapd(rhs, temp1);                     // compute
    vminpd(Operand(lhs), temp1, temp1);      //   min rhs, lhs
    vorpd(temp1, output, output);            // fix min(-0, 0) with OR
  } else {
    vmovapd(lhs, output);                    // compute
    vmaxpd(rhs, output, output);             //   max lhs, rhs
    vmovapd(rhs, temp1);                     // compute
    vmaxpd(Operand(lhs), temp1, temp1);      //   max rhs, lhs
    vandpd(temp1, output, output);           // fix max(-0, 0) with AND
  }
  vmovapd(lhs, temp1);                       // compute
  vcmpunordpd(rhs, temp1, temp1);                   //   lhs UNORD rhs
  vptest(temp1, temp1);                      // check if any unordered
  j(Assembler::Equal, &l);                   //   and exit if not

  // Slow path.
  // output has result for non-NaN lanes, garbage in NaN lanes.
  // temp1 has lhs UNORD rhs.
  // temp2 is dead.

  vmovapd(temp1, temp2);                     // clear NaN lanes of result
  vpandn(output, temp2, temp2);              //   result now in temp2
  asMasm().vpandSimd128(quietBits, temp1, temp1);   // setup QNaN bits in NaN lanes
  vorpd(temp1, temp2, temp2);                //   and OR into result
  vmovapd(lhs, temp1);                       // find NaN lanes
  vcmpunordpd(Operand(temp1), temp1, temp1);        //   in lhs
  vmovapd(temp1, output);                    //     (and save them for later)
  vandpd(lhs, temp1, temp1);                 //       and extract the NaNs
  vorpd(temp1, temp2, temp2);                //         and add to the result
  vmovapd(rhs, temp1);                       // find NaN lanes
  vcmpunordpd(Operand(temp1), temp1, temp1);        //   in rhs
  vpandn(temp1, output, output);             //     except if they were in lhs
  vandpd(rhs, output, output);               //       and extract the NaNs
  vorpd(temp2, output, output);              //         and add to the result

  bind(&l);
  /* clang-format on */
}

void MacroAssemblerX86Shared::minMaxFloat64x2AVX(bool isMin, FloatRegister lhs,
                                                 FloatRegister rhs,
                                                 FloatRegister temp1,
                                                 FloatRegister temp2,
                                                 FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  Label l;
  SimdConstant quietBits(SimdConstant::SplatX2(int64_t(0x0008000000000000ull)));

  /* clang-format off */ /* leave my comments alone */
  FloatRegister lhsCopy = moveSimd128FloatIfEqual(lhs, scratch, output);
  // Allow rhs be assigned to scratch when rhs == lhs and == output --
  // don't make a special case since the semantics require setup QNaN bits.
  FloatRegister rhsCopy = moveSimd128FloatIfEqual(rhs, scratch, output);
  if (isMin) {
    vminpd(Operand(rhs), lhs, temp2);             // min lhs, rhs
    vminpd(Operand(lhs), rhs, temp1);             // min rhs, lhs
    vorpd(temp1, temp2, output);                  // fix min(-0, 0) with OR
  } else {
    vmaxpd(Operand(rhs), lhs, temp2);             // max lhs, rhs
    vmaxpd(Operand(lhs), rhs, temp1);             // max rhs, lhs
    vandpd(temp1, temp2, output);                 // fix max(-0, 0) with AND
  }
  vcmpunordpd(Operand(rhsCopy), lhsCopy, temp1);  // lhs UNORD rhs
  vptest(temp1, temp1);                           // check if any unordered
  j(Assembler::Equal, &l);                        //   and exit if not

  // Slow path.
  // output has result for non-NaN lanes, garbage in NaN lanes.
  // temp1 has lhs UNORD rhs.
  // temp2 is dead.
  vcmpunordpd(Operand(lhsCopy), lhsCopy, temp2);  // find NaN lanes in lhs
  vblendvpd(temp2, lhsCopy, rhsCopy, temp2);      //   add other lines from rhs
  asMasm().vporSimd128(quietBits, temp2, temp2);  // setup QNaN bits in NaN lanes
  vblendvpd(temp1, temp2, output, output);        // replace NaN lines from temp2

  bind(&l);
  /* clang-format on */
}

void MacroAssemblerX86Shared::minFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister temp1,
                                           FloatRegister temp2,
                                           FloatRegister output) {
  if (HasAVX()) {
    minMaxFloat32x4AVX(/*isMin=*/true, lhs, rhs, temp1, temp2, output);
    return;
  }
  minMaxFloat32x4(/*isMin=*/true, lhs, Operand(rhs), temp1, temp2, output);
}

void MacroAssemblerX86Shared::maxFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister temp1,
                                           FloatRegister temp2,
                                           FloatRegister output) {
  if (HasAVX()) {
    minMaxFloat32x4AVX(/*isMin=*/false, lhs, rhs, temp1, temp2, output);
    return;
  }
  minMaxFloat32x4(/*isMin=*/false, lhs, Operand(rhs), temp1, temp2, output);
}

void MacroAssemblerX86Shared::minFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister temp1,
                                           FloatRegister temp2,
                                           FloatRegister output) {
  if (HasAVX()) {
    minMaxFloat64x2AVX(/*isMin=*/true, lhs, rhs, temp1, temp2, output);
    return;
  }
  minMaxFloat64x2(/*isMin=*/true, lhs, Operand(rhs), temp1, temp2, output);
}

void MacroAssemblerX86Shared::maxFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister temp1,
                                           FloatRegister temp2,
                                           FloatRegister output) {
  if (HasAVX()) {
    minMaxFloat64x2AVX(/*isMin=*/false, lhs, rhs, temp1, temp2, output);
    return;
  }
  minMaxFloat64x2(/*isMin=*/false, lhs, Operand(rhs), temp1, temp2, output);
}

void MacroAssemblerX86Shared::packedShiftByScalarInt8x16(
    FloatRegister in, Register count, FloatRegister xtmp, FloatRegister dest,
    void (MacroAssemblerX86Shared::*shift)(FloatRegister, FloatRegister,
                                           FloatRegister),
    void (MacroAssemblerX86Shared::*extend)(const Operand&, FloatRegister)) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, scratch);

  // High bytes
  vpalignr(Operand(in), xtmp, xtmp, 8);
  (this->*extend)(Operand(xtmp), xtmp);
  (this->*shift)(scratch, xtmp, xtmp);

  // Low bytes
  (this->*extend)(Operand(dest), dest);
  (this->*shift)(scratch, dest, dest);

  // Mask off garbage to avoid saturation during packing
  asMasm().loadConstantSimd128Int(SimdConstant::SplatX4(int32_t(0x00FF00FF)),
                                  scratch);
  vpand(Operand(scratch), xtmp, xtmp);
  vpand(Operand(scratch), dest, dest);

  vpackuswb(Operand(xtmp), dest, dest);
}

void MacroAssemblerX86Shared::packedLeftShiftByScalarInt8x16(
    FloatRegister in, Register count, FloatRegister xtmp, FloatRegister dest) {
  packedShiftByScalarInt8x16(in, count, xtmp, dest,
                             &MacroAssemblerX86Shared::vpsllw,
                             &MacroAssemblerX86Shared::vpmovzxbw);
}

void MacroAssemblerX86Shared::packedLeftShiftByScalarInt8x16(
    Imm32 count, FloatRegister src, FloatRegister dest) {
  MOZ_ASSERT(count.value <= 7);
  if (MOZ_UNLIKELY(count.value == 0)) {
    moveSimd128Int(src, dest);
    return;
  }
  src = asMasm().moveSimd128IntIfNotAVX(src, dest);
  // Use the doubling trick for low shift counts, otherwise mask off the bits
  // that are shifted out of the low byte of each word and use word shifts.  The
  // optimal cutoff remains to be explored.
  if (count.value <= 3) {
    vpaddb(Operand(src), src, dest);
    for (int32_t shift = count.value - 1; shift > 0; --shift) {
      vpaddb(Operand(dest), dest, dest);
    }
  } else {
    asMasm().bitwiseAndSimd128(src, SimdConstant::SplatX16(0xFF >> count.value),
                               dest);
    vpsllw(count, dest, dest);
  }
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt8x16(
    FloatRegister in, Register count, FloatRegister xtmp, FloatRegister dest) {
  packedShiftByScalarInt8x16(in, count, xtmp, dest,
                             &MacroAssemblerX86Shared::vpsraw,
                             &MacroAssemblerX86Shared::vpmovsxbw);
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt8x16(
    Imm32 count, FloatRegister src, FloatRegister dest) {
  MOZ_ASSERT(count.value <= 7);
  ScratchSimd128Scope scratch(asMasm());

  vpunpckhbw(src, scratch, scratch);
  vpunpcklbw(src, dest, dest);
  vpsraw(Imm32(count.value + 8), scratch, scratch);
  vpsraw(Imm32(count.value + 8), dest, dest);
  vpacksswb(Operand(scratch), dest, dest);
}

void MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt8x16(
    FloatRegister in, Register count, FloatRegister xtmp, FloatRegister dest) {
  packedShiftByScalarInt8x16(in, count, xtmp, dest,
                             &MacroAssemblerX86Shared::vpsrlw,
                             &MacroAssemblerX86Shared::vpmovzxbw);
}

void MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt8x16(
    Imm32 count, FloatRegister src, FloatRegister dest) {
  MOZ_ASSERT(count.value <= 7);
  src = asMasm().moveSimd128IntIfNotAVX(src, dest);
  asMasm().bitwiseAndSimd128(
      src, SimdConstant::SplatX16((0xFF << count.value) & 0xFF), dest);
  vpsrlw(count, dest, dest);
}

void MacroAssemblerX86Shared::packedLeftShiftByScalarInt16x8(
    FloatRegister in, Register count, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, scratch);
  vpsllw(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt16x8(
    FloatRegister in, Register count, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, scratch);
  vpsraw(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt16x8(
    FloatRegister in, Register count, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, scratch);
  vpsrlw(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedLeftShiftByScalarInt32x4(
    FloatRegister in, Register count, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, scratch);
  vpslld(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt32x4(
    FloatRegister in, Register count, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, scratch);
  vpsrad(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt32x4(
    FloatRegister in, Register count, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, scratch);
  vpsrld(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedLeftShiftByScalarInt64x2(
    FloatRegister in, Register count, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, scratch);
  vpsllq(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt64x2(
    FloatRegister in, Register count, FloatRegister temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, temp);
  asMasm().signReplicationInt64x2(in, scratch);
  in = asMasm().moveSimd128FloatIfNotAVX(in, dest);
  // Invert if negative, shift all, invert back if negative.
  vpxor(Operand(scratch), in, dest);
  vpsrlq(temp, dest, dest);
  vpxor(Operand(scratch), dest, dest);
}

void MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt64x2(
    FloatRegister in, Register count, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovd(count, scratch);
  vpsrlq(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt64x2(
    Imm32 count, FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  asMasm().signReplicationInt64x2(src, scratch);
  // Invert if negative, shift all, invert back if negative.
  src = asMasm().moveSimd128FloatIfNotAVX(src, dest);
  vpxor(Operand(scratch), src, dest);
  vpsrlq(Imm32(count.value & 63), dest, dest);
  vpxor(Operand(scratch), dest, dest);
}

void MacroAssemblerX86Shared::selectSimd128(FloatRegister mask,
                                            FloatRegister onTrue,
                                            FloatRegister onFalse,
                                            FloatRegister temp,
                                            FloatRegister output) {
  // Normally the codegen will attempt to enforce these register assignments so
  // that the moves are avoided.

  onTrue = asMasm().moveSimd128IntIfNotAVX(onTrue, output);
  if (MOZ_UNLIKELY(mask == onTrue)) {
    vpor(Operand(onFalse), onTrue, output);
    return;
  }

  mask = asMasm().moveSimd128IntIfNotAVX(mask, temp);

  vpand(Operand(mask), onTrue, output);
  vpandn(Operand(onFalse), mask, temp);
  vpor(Operand(temp), output, output);
}

// Code sequences for int32x4<->float32x4 culled from v8; commentary added.

void MacroAssemblerX86Shared::unsignedConvertInt32x4ToFloat32x4(
    FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  src = asMasm().moveSimd128IntIfNotAVX(src, dest);
  vpxor(Operand(scratch), scratch, scratch);  // extract low bits
  vpblendw(0x55, src, scratch, scratch);      //   into scratch
  vpsubd(Operand(scratch), src, dest);        //     and high bits into dest
  vcvtdq2ps(scratch, scratch);                // convert low bits
  vpsrld(Imm32(1), dest, dest);               // get high into unsigned range
  vcvtdq2ps(dest, dest);                      //   convert
  vaddps(Operand(dest), dest, dest);          //     and back into signed
  vaddps(Operand(scratch), dest, dest);       // combine high+low: may round
}

void MacroAssemblerX86Shared::truncSatFloat32x4ToInt32x4(FloatRegister src,
                                                         FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());

  // The cvttps2dq instruction is the workhorse but does not handle NaN or out
  // of range values as we need it to.  We want to saturate too-large positive
  // values to 7FFFFFFFh and too-large negative values to 80000000h.  NaN and -0
  // become 0.

  // Convert NaN to 0 by masking away values that compare unordered to itself.
  if (HasAVX()) {
    vcmpeqps(Operand(src), src, scratch);
    vpand(Operand(scratch), src, dest);
  } else {
    vmovaps(src, scratch);
    vcmpeqps(Operand(scratch), scratch, scratch);
    moveSimd128Float(src, dest);
    vpand(Operand(scratch), dest, dest);
  }

  // Make lanes in scratch == 0xFFFFFFFFh, if dest overflows during cvttps2dq,
  // otherwise 0.
  static const SimdConstant minOverflowedInt =
      SimdConstant::SplatX4(2147483648.f);
  if (HasAVX()) {
    asMasm().vcmpgepsSimd128(minOverflowedInt, dest, scratch);
  } else {
    asMasm().loadConstantSimd128Float(minOverflowedInt, scratch);
    vcmpleps(Operand(dest), scratch, scratch);
  }

  // Convert.  This will make the output 80000000h if the input is out of range.
  vcvttps2dq(dest, dest);

  // Convert overflow lanes to 0x7FFFFFFF.
  vpxor(Operand(scratch), dest, dest);
}

void MacroAssemblerX86Shared::unsignedTruncSatFloat32x4ToInt32x4(
    FloatRegister src, FloatRegister temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  src = asMasm().moveSimd128FloatIfNotAVX(src, dest);

  // The cvttps2dq instruction is the workhorse but does not handle NaN or out
  // of range values as we need it to.  We want to saturate too-large positive
  // values to FFFFFFFFh and negative values to zero.  NaN and -0 become 0.

  // Convert NaN and negative values to zeroes in dest.
  vxorps(Operand(scratch), scratch, scratch);
  vmaxps(Operand(scratch), src, dest);

  // Place the largest positive signed integer in all lanes in scratch.
  // We use it to bias the conversion to handle edge cases.
  asMasm().loadConstantSimd128Float(SimdConstant::SplatX4(2147483647.f),
                                    scratch);

  // temp = dest - 7FFFFFFFh (as floating), this brings integers in the unsigned
  // range but above the signed range into the signed range; 0 => -7FFFFFFFh.
  vmovaps(dest, temp);
  vsubps(Operand(scratch), temp, temp);

  // scratch = mask of biased values that are greater than 7FFFFFFFh.
  vcmpleps(Operand(temp), scratch, scratch);

  // Convert the biased values to integer.  Positive values above 7FFFFFFFh will
  // have been converted to 80000000h, all others become the expected integer.
  vcvttps2dq(temp, temp);

  // As lanes of scratch are ~0 where the result overflows, this computes
  // 7FFFFFFF in lanes of temp that are 80000000h, and leaves other lanes
  // untouched as the biased integer.
  vpxor(Operand(scratch), temp, temp);

  // Convert negative biased lanes in temp to zero.  After this, temp will be
  // zero where the result should be zero or is less than 80000000h, 7FFFFFFF
  // where the result overflows, and will have the converted biased result in
  // other lanes (for input values >= 80000000h).
  vpxor(Operand(scratch), scratch, scratch);
  vpmaxsd(Operand(scratch), temp, temp);

  // Convert. Overflow lanes above 7FFFFFFFh will be 80000000h, other lanes will
  // be what they should be.
  vcvttps2dq(dest, dest);

  // Add temp to the result.  Overflow lanes with 80000000h becomes FFFFFFFFh,
  // biased high-value unsigned lanes become unbiased, everything else is left
  // unchanged.
  vpaddd(Operand(temp), dest, dest);
}

void MacroAssemblerX86Shared::unsignedTruncFloat32x4ToInt32x4Relaxed(
    FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  src = asMasm().moveSimd128FloatIfNotAVX(src, dest);

  // Place lanes below 80000000h into dest, otherwise into scratch.
  // Keep dest or scratch 0 as default.
  asMasm().loadConstantSimd128Float(SimdConstant::SplatX4(0x4f000000), scratch);
  vcmpltps(Operand(src), scratch, scratch);
  vpand(Operand(src), scratch, scratch);
  vpxor(Operand(scratch), src, dest);

  // Convert lanes below 80000000h into unsigned int without issues.
  vcvttps2dq(dest, dest);
  // Knowing IEEE-754 number representation: convert lanes above 7FFFFFFFh,
  // mutiply by 2 (to add 1 in exponent) and shift to the left by 8 bits.
  vaddps(Operand(scratch), scratch, scratch);
  vpslld(Imm32(8), scratch, scratch);

  // Combine the results.
  vpaddd(Operand(scratch), dest, dest);
}

void MacroAssemblerX86Shared::unsignedConvertInt32x4ToFloat64x2(
    FloatRegister src, FloatRegister dest) {
  src = asMasm().moveSimd128FloatIfNotAVX(src, dest);
  asMasm().vunpcklpsSimd128(SimdConstant::SplatX4(0x43300000), src, dest);
  asMasm().vsubpdSimd128(SimdConstant::SplatX2(4503599627370496.0), dest, dest);
}

void MacroAssemblerX86Shared::truncSatFloat64x2ToInt32x4(FloatRegister src,
                                                         FloatRegister temp,
                                                         FloatRegister dest) {
  FloatRegister srcForTemp = asMasm().moveSimd128FloatIfNotAVX(src, temp);
  vcmpeqpd(Operand(srcForTemp), srcForTemp, temp);

  src = asMasm().moveSimd128FloatIfNotAVX(src, dest);
  asMasm().vandpdSimd128(SimdConstant::SplatX2(2147483647.0), temp, temp);
  vminpd(Operand(temp), src, dest);
  vcvttpd2dq(dest, dest);
}

void MacroAssemblerX86Shared::unsignedTruncSatFloat64x2ToInt32x4(
    FloatRegister src, FloatRegister temp, FloatRegister dest) {
  src = asMasm().moveSimd128FloatIfNotAVX(src, dest);

  vxorpd(temp, temp, temp);
  vmaxpd(Operand(temp), src, dest);

  asMasm().vminpdSimd128(SimdConstant::SplatX2(4294967295.0), dest, dest);
  vroundpd(SSERoundingMode::Trunc, Operand(dest), dest);
  asMasm().vaddpdSimd128(SimdConstant::SplatX2(4503599627370496.0), dest, dest);

  // temp == 0
  vshufps(0x88, temp, dest, dest);
}

void MacroAssemblerX86Shared::unsignedTruncFloat64x2ToInt32x4Relaxed(
    FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());

  // The same as unsignedConvertInt32x4ToFloat64x2, but without NaN
  // and out-of-bounds checks.
  vroundpd(SSERoundingMode::Trunc, Operand(src), dest);
  asMasm().loadConstantSimd128Float(SimdConstant::SplatX2(4503599627370496.0),
                                    scratch);
  vaddpd(Operand(scratch), dest, dest);
  // The scratch has zeros in f32x4 lanes with index 0 and 2. The in-memory
  // representantation of the splatted double contantant contains zero in its
  // low bits.
  vshufps(0x88, scratch, dest, dest);
}

void MacroAssemblerX86Shared::popcntInt8x16(FloatRegister src,
                                            FloatRegister temp,
                                            FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  asMasm().loadConstantSimd128Int(SimdConstant::SplatX16(0x0f), scratch);
  FloatRegister srcForTemp = asMasm().moveSimd128IntIfNotAVX(src, temp);
  vpand(scratch, srcForTemp, temp);
  vpandn(src, scratch, scratch);
  int8_t counts[] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
  asMasm().loadConstantSimd128(SimdConstant::CreateX16(counts), output);
  vpsrlw(Imm32(4), scratch, scratch);
  vpshufb(temp, output, output);
  asMasm().loadConstantSimd128(SimdConstant::CreateX16(counts), temp);
  vpshufb(scratch, temp, temp);
  vpaddb(Operand(temp), output, output);
}
