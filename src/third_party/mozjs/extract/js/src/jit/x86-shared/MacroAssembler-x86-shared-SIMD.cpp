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
  zeroSimd128Int(scratch);
  vpshufb(scratch, output, output);
}

void MacroAssemblerX86Shared::splatX8(Register input, FloatRegister output) {
  vmovd(input, output);
  vpshuflw(0, output, output);
  vpshufd(0, output, output);
}

void MacroAssemblerX86Shared::splatX4(Register input, FloatRegister output) {
  vmovd(input, output);
  vpshufd(0, output, output);
}

void MacroAssemblerX86Shared::splatX4(FloatRegister input,
                                      FloatRegister output) {
  MOZ_ASSERT(input.isSingle() && output.isSimd128());
  asMasm().moveSimd128Float(input.asSimd128(), output);
  vshufps(0, output, output, output);
}

void MacroAssemblerX86Shared::splatX2(FloatRegister input,
                                      FloatRegister output) {
  MOZ_ASSERT(input.isDouble() && output.isSimd128());
  asMasm().moveSimd128Float(input.asSimd128(), output);
  vshufpd(0, output, output, output);
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
    shuffleFloat32(mask, input, output.asSimd128());
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
    vpalignr(Operand(input), output, 8);
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

void MacroAssemblerX86Shared::replaceLaneFloat32x4(FloatRegister rhs,
                                                   FloatRegister lhsDest,
                                                   unsigned lane) {
  MOZ_ASSERT(lhsDest.isSimd128() && rhs.isSingle());

  if (lane == 0) {
    if (rhs.asSimd128() == lhsDest) {
      // no-op, although this should not normally happen for type checking
      // reasons higher up in the stack.
    } else {
      // move low dword of value into low dword of output
      vmovss(rhs, lhsDest, lhsDest);
    }
  } else {
    vinsertps(vinsertpsMask(0, lane), rhs, lhsDest, lhsDest);
  }
}

void MacroAssemblerX86Shared::replaceLaneFloat64x2(FloatRegister rhs,
                                                   FloatRegister lhsDest,
                                                   unsigned lane) {
  MOZ_ASSERT(lhsDest.isSimd128() && rhs.isDouble());

  if (lane == 0) {
    if (rhs.asSimd128() == lhsDest) {
      // no-op, although this should not normally happen for type checking
      // reasons higher up in the stack.
    } else {
      // move low qword of value into low qword of output
      vmovsd(rhs, lhsDest, lhsDest);
    }
  } else {
    // move low qword of value into high qword of output
    vshufpd(0, rhs, lhsDest, lhsDest);
  }
}

void MacroAssemblerX86Shared::blendInt8x16(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister output,
                                           FloatRegister temp,
                                           const uint8_t lanes[16]) {
  MOZ_ASSERT(lhs == output);
  MOZ_ASSERT(temp.encoding() == X86Encoding::xmm0, "pblendvb needs xmm0");

  asMasm().loadConstantSimd128Int(
      SimdConstant::CreateX16(reinterpret_cast<const int8_t*>(lanes)), temp);
  vpblendvb(temp, rhs, lhs, output);
}

void MacroAssemblerX86Shared::blendInt16x8(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister output,
                                           const uint16_t lanes[8]) {
  MOZ_ASSERT(lhs == output);

  uint32_t mask = 0;
  for (unsigned i = 0; i < 8; i++) {
    if (lanes[i]) {
      mask |= (1 << i);
    }
  }
  vpblendw(mask, rhs, lhs, lhs);
}

void MacroAssemblerX86Shared::shuffleInt8x16(FloatRegister lhs,
                                             FloatRegister rhs,
                                             FloatRegister output,
                                             const uint8_t lanes[16]) {
  ScratchSimd128Scope scratch(asMasm());

  // Use pshufb instructions to gather the lanes from each source vector.
  // A negative index creates a zero lane, so the two vectors can be combined.

  // Register preference: lhs == output.

  // Set scratch = lanes from rhs.
  int8_t idx[16];
  for (unsigned i = 0; i < 16; i++) {
    idx[i] = lanes[i] >= 16 ? lanes[i] - 16 : -1;
  }
  moveSimd128Int(rhs, scratch);
  asMasm().vpshufbSimd128(SimdConstant::CreateX16(idx), scratch);

  // Set output = lanes from lhs.
  for (unsigned i = 0; i < 16; i++) {
    idx[i] = lanes[i] < 16 ? lanes[i] : -1;
  }
  moveSimd128Int(lhs, output);
  asMasm().vpshufbSimd128(SimdConstant::CreateX16(idx), output);

  // Combine.
  vpor(scratch, output, output);
}

static inline FloatRegister ToSimdFloatRegister(const Operand& op) {
  return FloatRegister(op.fpu(), FloatRegister::Codes::ContentType::Simd128);
}

void MacroAssemblerX86Shared::compareInt8x16(FloatRegister lhs, Operand rhs,
                                             Assembler::Condition cond,
                                             FloatRegister output) {
  static const SimdConstant allOnes = SimdConstant::SplatX16(-1);
  switch (cond) {
    case Assembler::Condition::GreaterThan:
      vpcmpgtb(rhs, lhs, output);
      break;
    case Assembler::Condition::Equal:
      vpcmpeqb(rhs, lhs, output);
      break;
    case Assembler::Condition::LessThan: {
      ScratchSimd128Scope scratch(asMasm());
      // This is bad, but Ion does not use it.
      // src := rhs
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), scratch);
      } else {
        loadAlignedSimd128Int(rhs, scratch);
      }
      // src := src > lhs (i.e. lhs < rhs)
      vpcmpgtb(Operand(lhs), scratch, scratch);
      moveSimd128Int(scratch, output);
      break;
    }
    case Assembler::Condition::NotEqual:
      vpcmpeqb(rhs, lhs, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual: {
      ScratchSimd128Scope scratch(asMasm());
      // This is bad, but Ion does not use it.
      // src := rhs
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), scratch);
      } else {
        loadAlignedSimd128Int(rhs, scratch);
      }
      vpcmpgtb(Operand(lhs), scratch, scratch);
      asMasm().loadConstantSimd128Int(allOnes, output);
      vpxor(Operand(scratch), output, output);
      break;
    }
    case Assembler::Condition::LessThanOrEqual:
      // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
      vpcmpgtb(rhs, lhs, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::Above:
      vpmaxub(rhs, lhs, output);
      vpcmpeqb(rhs, output, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::BelowOrEqual:
      vpmaxub(rhs, lhs, output);
      vpcmpeqb(rhs, output, output);
      break;
    case Assembler::Below:
      vpminub(rhs, lhs, output);
      vpcmpeqb(rhs, output, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::AboveOrEqual:
      vpminub(rhs, lhs, output);
      vpcmpeqb(rhs, output, output);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareInt8x16(Assembler::Condition cond,
                                             const SimdConstant& rhs,
                                             FloatRegister lhsDest) {
  bool complement = false;
  switch (cond) {
    case Assembler::Condition::NotEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::Equal:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vpcmpeqb,
                    &MacroAssembler::vpcmpeqbSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::GreaterThan:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vpcmpgtb,
                    &MacroAssembler::vpcmpgtbSimd128);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
  if (complement) {
    asMasm().bitwiseXorSimd128(SimdConstant::SplatX16(-1), lhsDest);
  }
}

void MacroAssemblerX86Shared::compareInt16x8(FloatRegister lhs, Operand rhs,
                                             Assembler::Condition cond,
                                             FloatRegister output) {
  static const SimdConstant allOnes = SimdConstant::SplatX8(-1);

  switch (cond) {
    case Assembler::Condition::GreaterThan:
      vpcmpgtw(rhs, lhs, output);
      break;
    case Assembler::Condition::Equal:
      vpcmpeqw(rhs, lhs, output);
      break;
    case Assembler::Condition::LessThan: {
      ScratchSimd128Scope scratch(asMasm());
      // This is bad, but Ion does not use it.
      // src := rhs
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), scratch);
      } else {
        loadAlignedSimd128Int(rhs, scratch);
      }
      // src := src > lhs (i.e. lhs < rhs)
      vpcmpgtw(Operand(lhs), scratch, scratch);
      moveSimd128Int(scratch, output);
      break;
    }
    case Assembler::Condition::NotEqual:
      vpcmpeqw(rhs, lhs, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual: {
      ScratchSimd128Scope scratch(asMasm());
      // This is bad, but Ion does not use it.
      // src := rhs
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), scratch);
      } else {
        loadAlignedSimd128Int(rhs, scratch);
      }
      vpcmpgtw(Operand(lhs), scratch, scratch);
      asMasm().loadConstantSimd128Int(allOnes, output);
      vpxor(Operand(scratch), output, output);
      break;
    }
    case Assembler::Condition::LessThanOrEqual:
      // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
      vpcmpgtw(rhs, lhs, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::Above:
      vpmaxuw(rhs, lhs, output);
      vpcmpeqw(rhs, output, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::BelowOrEqual:
      vpmaxuw(rhs, lhs, output);
      vpcmpeqw(rhs, output, output);
      break;
    case Assembler::Below:
      vpminuw(rhs, lhs, output);
      vpcmpeqw(rhs, output, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::AboveOrEqual:
      vpminuw(rhs, lhs, lhs);
      vpcmpeqw(rhs, lhs, lhs);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareInt16x8(Assembler::Condition cond,
                                             const SimdConstant& rhs,
                                             FloatRegister lhsDest) {
  bool complement = false;
  switch (cond) {
    case Assembler::Condition::NotEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::Equal:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vpcmpeqw,
                    &MacroAssembler::vpcmpeqwSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::GreaterThan:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vpcmpgtw,
                    &MacroAssembler::vpcmpgtwSimd128);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
  if (complement) {
    asMasm().bitwiseXorSimd128(SimdConstant::SplatX16(-1), lhsDest);
  }
}

void MacroAssemblerX86Shared::compareInt32x4(FloatRegister lhs, Operand rhs,
                                             Assembler::Condition cond,
                                             FloatRegister output) {
  static const SimdConstant allOnes = SimdConstant::SplatX4(-1);
  switch (cond) {
    case Assembler::Condition::GreaterThan:
      vpcmpgtd(rhs, lhs, lhs);
      break;
    case Assembler::Condition::Equal:
      vpcmpeqd(rhs, lhs, lhs);
      break;
    case Assembler::Condition::LessThan: {
      ScratchSimd128Scope scratch(asMasm());
      // This is bad, but Ion does not use it.
      // src := rhs
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), scratch);
      } else {
        loadAlignedSimd128Int(rhs, scratch);
      }
      // src := src > lhs (i.e. lhs < rhs)
      vpcmpgtd(Operand(lhs), scratch, scratch);
      moveSimd128Int(scratch, lhs);
      break;
    }
    case Assembler::Condition::NotEqual:
      vpcmpeqd(rhs, lhs, lhs);
      asMasm().bitwiseXorSimd128(allOnes, lhs);
      break;
    case Assembler::Condition::GreaterThanOrEqual: {
      ScratchSimd128Scope scratch(asMasm());
      // This is bad, but Ion does not use it.
      // src := rhs
      if (rhs.kind() == Operand::FPREG) {
        moveSimd128Int(ToSimdFloatRegister(rhs), scratch);
      } else {
        loadAlignedSimd128Int(rhs, scratch);
      }
      vpcmpgtd(Operand(lhs), scratch, scratch);
      asMasm().loadConstantSimd128Int(allOnes, lhs);
      vpxor(Operand(scratch), lhs, lhs);
      break;
    }
    case Assembler::Condition::LessThanOrEqual:
      // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
      vpcmpgtd(rhs, lhs, lhs);
      asMasm().bitwiseXorSimd128(allOnes, lhs);
      break;
    case Assembler::Above:
      vpmaxud(rhs, lhs, output);
      vpcmpeqd(rhs, output, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::BelowOrEqual:
      vpmaxud(rhs, lhs, output);
      vpcmpeqd(rhs, output, output);
      break;
    case Assembler::Below:
      vpminud(rhs, lhs, output);
      vpcmpeqd(rhs, output, output);
      asMasm().bitwiseXorSimd128(allOnes, output);
      break;
    case Assembler::AboveOrEqual:
      vpminud(rhs, lhs, output);
      vpcmpeqd(rhs, output, output);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareInt32x4(Assembler::Condition cond,
                                             const SimdConstant& rhs,
                                             FloatRegister lhsDest) {
  bool complement = false;
  switch (cond) {
    case Assembler::Condition::NotEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::Equal:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vpcmpeqd,
                    &MacroAssembler::vpcmpeqdSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      complement = true;
      [[fallthrough]];
    case Assembler::Condition::GreaterThan:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vpcmpgtd,
                    &MacroAssembler::vpcmpgtdSimd128);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
  if (complement) {
    asMasm().bitwiseXorSimd128(SimdConstant::SplatX16(-1), lhsDest);
  }
}

void MacroAssemblerX86Shared::compareForEqualityInt64x2(
    FloatRegister lhs, Operand rhs, Assembler::Condition cond,
    FloatRegister output) {
  static const SimdConstant allOnes = SimdConstant::SplatX4(-1);
  switch (cond) {
    case Assembler::Condition::Equal:
      vpcmpeqq(rhs, lhs, lhs);
      break;
    case Assembler::Condition::NotEqual:
      vpcmpeqq(rhs, lhs, lhs);
      asMasm().bitwiseXorSimd128(allOnes, lhs);
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
  //      __m128i r = _mm_and_si128(_mm_cmpeq_epi32(a, b), _mm_sub_epi64(b,
  //      a)); r = _mm_or_si128(r, _mm_cmpgt_epi32(a, b)); return
  //      _mm_shuffle_epi32(r, _MM_SHUFFLE(3,3,1,1));
  //  }
  // Credits to https://stackoverflow.com/a/65175746
  switch (cond) {
    case Assembler::Condition::GreaterThan:
      vmovdqa(rhs, temp1);
      vmovdqa(Operand(lhs), temp2);
      vpsubq(Operand(lhs), temp1, temp1);
      vpcmpeqd(rhs, temp2, temp2);
      vandpd(temp2, temp1, temp1);
      asMasm().moveSimd128(lhs, output);
      vpcmpgtd(rhs, output, output);
      vpor(Operand(temp1), output, output);
      vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), output, output);
      break;
    case Assembler::Condition::LessThan:
      vmovdqa(rhs, temp1);
      vmovdqa(Operand(lhs), temp2);
      vpcmpgtd(Operand(lhs), temp1, temp1);
      vpcmpeqd(Operand(rhs), temp2, temp2);
      asMasm().moveSimd128(lhs, output);
      vpsubq(rhs, output, output);
      vandpd(temp2, output, output);
      vpor(Operand(temp1), output, output);
      vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), output, output);
      break;
    case Assembler::Condition::GreaterThanOrEqual:
      vmovdqa(rhs, temp1);
      vmovdqa(Operand(lhs), temp2);
      vpcmpgtd(Operand(lhs), temp1, temp1);
      vpcmpeqd(Operand(rhs), temp2, temp2);
      asMasm().moveSimd128(lhs, output);
      vpsubq(rhs, output, output);
      vandpd(temp2, output, output);
      vpor(Operand(temp1), output, output);
      vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), output, output);
      asMasm().bitwiseXorSimd128(allOnes, lhs);
      break;
    case Assembler::Condition::LessThanOrEqual:
      vmovdqa(rhs, temp1);
      vmovdqa(Operand(lhs), temp2);
      vpsubq(Operand(lhs), temp1, temp1);
      vpcmpeqd(rhs, temp2, temp2);
      vandpd(temp2, temp1, temp1);
      asMasm().moveSimd128(lhs, output);
      vpcmpgtd(rhs, output, output);
      vpor(Operand(temp1), output, output);
      vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), output, output);
      asMasm().bitwiseXorSimd128(allOnes, lhs);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareFloat32x4(FloatRegister lhs, Operand rhs,
                                               Assembler::Condition cond,
                                               FloatRegister output) {
  if (HasAVX()) {
    MOZ_CRASH("Can do better here with three-address compares");
  }

  // Move lhs to output if lhs!=output; move rhs out of the way if rhs==output.
  // This is bad, but Ion does not need this fixup.
  ScratchSimd128Scope scratch(asMasm());
  if (!lhs.aliases(output)) {
    if (rhs.kind() == Operand::FPREG &&
        output.aliases(FloatRegister::FromCode(rhs.fpu()))) {
      vmovaps(rhs, scratch);
      rhs = Operand(scratch);
    }
    vmovaps(lhs, output);
  }

  switch (cond) {
    case Assembler::Condition::Equal:
      vcmpeqps(rhs, output);
      break;
    case Assembler::Condition::LessThan:
      vcmpltps(rhs, output);
      break;
    case Assembler::Condition::LessThanOrEqual:
      vcmpleps(rhs, output);
      break;
    case Assembler::Condition::NotEqual:
      vcmpneqps(rhs, output);
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
                                               const SimdConstant& rhs,
                                               FloatRegister lhsDest) {
  switch (cond) {
    case Assembler::Condition::Equal:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vcmpeqps,
                    &MacroAssembler::vcmpeqpsSimd128);
      break;
    case Assembler::Condition::LessThan:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vcmpltps,
                    &MacroAssembler::vcmpltpsSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vcmpleps,
                    &MacroAssembler::vcmplepsSimd128);
      break;
    case Assembler::Condition::NotEqual:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vcmpneqps,
                    &MacroAssembler::vcmpneqpsSimd128);
      break;
    default:
      MOZ_CRASH("unexpected condition op");
  }
}

void MacroAssemblerX86Shared::compareFloat64x2(FloatRegister lhs, Operand rhs,
                                               Assembler::Condition cond,
                                               FloatRegister output) {
  if (HasAVX()) {
    MOZ_CRASH("Can do better here with three-address compares");
  }

  // Move lhs to output if lhs!=output; move rhs out of the way if rhs==output.
  // This is bad, but Ion does not need this fixup.
  ScratchSimd128Scope scratch(asMasm());
  if (!lhs.aliases(output)) {
    if (rhs.kind() == Operand::FPREG &&
        output.aliases(FloatRegister::FromCode(rhs.fpu()))) {
      vmovapd(rhs, scratch);
      rhs = Operand(scratch);
    }
    vmovapd(lhs, output);
  }

  switch (cond) {
    case Assembler::Condition::Equal:
      vcmpeqpd(rhs, output);
      break;
    case Assembler::Condition::LessThan:
      vcmpltpd(rhs, output);
      break;
    case Assembler::Condition::LessThanOrEqual:
      vcmplepd(rhs, output);
      break;
    case Assembler::Condition::NotEqual:
      vcmpneqpd(rhs, output);
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
                                               const SimdConstant& rhs,
                                               FloatRegister lhsDest) {
  switch (cond) {
    case Assembler::Condition::Equal:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vcmpeqpd,
                    &MacroAssembler::vcmpeqpdSimd128);
      break;
    case Assembler::Condition::LessThan:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vcmpltpd,
                    &MacroAssembler::vcmpltpdSimd128);
      break;
    case Assembler::Condition::LessThanOrEqual:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vcmplepd,
                    &MacroAssembler::vcmplepdSimd128);
      break;
    case Assembler::Condition::NotEqual:
      binarySimd128(rhs, lhsDest, &MacroAssembler::vcmpneqpd,
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

void MacroAssemblerX86Shared::minMaxFloat32x4(bool isMin, FloatRegister lhs_,
                                              Operand rhs, FloatRegister temp1,
                                              FloatRegister temp2,
                                              FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  Label l;
  SimdConstant quietBits(SimdConstant::SplatX4(int32_t(0x00400000)));

  /* clang-format off */ /* leave my comments alone */
  FloatRegister lhs = reusedInputSimd128Float(lhs_, scratch);
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
  vcmpunordps(rhs, temp1);                   //   lhs UNORD rhs
  vptest(temp1, temp1);                      // check if any unordered
  j(Assembler::Equal, &l);                   //   and exit if not

  // Slow path.
  // output has result for non-NaN lanes, garbage in NaN lanes.
  // temp1 has lhs UNORD rhs.
  // temp2 is dead.

  vmovaps(temp1, temp2);                     // clear NaN lanes of result
  vpandn(output, temp2, temp2);              //   result now in temp2
  asMasm().vpandSimd128(quietBits, temp1);            // setup QNaN bits in NaN lanes
  vorps(temp1, temp2, temp2);                //   and OR into result
  vmovaps(lhs, temp1);                       // find NaN lanes
  vcmpunordps(Operand(temp1), temp1);        //   in lhs
  vmovaps(temp1, output);                    //     (and save them for later)
  vandps(lhs, temp1, temp1);                 //       and extract the NaNs
  vorps(temp1, temp2, temp2);                //         and add to the result
  vmovaps(rhs, temp1);                       // find NaN lanes
  vcmpunordps(Operand(temp1), temp1);        //   in rhs
  vpandn(temp1, output, output);             //     except if they were in lhs
  vandps(rhs, output, output);               //       and extract the NaNs
  vorps(temp2, output, output);              //         and add to the result

  bind(&l);
  /* clang-format on */
}

// Exactly as above.
void MacroAssemblerX86Shared::minMaxFloat64x2(bool isMin, FloatRegister lhs_,
                                              Operand rhs, FloatRegister temp1,
                                              FloatRegister temp2,
                                              FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  Label l;
  SimdConstant quietBits(SimdConstant::SplatX2(int64_t(0x0008000000000000ull)));

  /* clang-format off */ /* leave my comments alone */
  FloatRegister lhs = reusedInputSimd128Float(lhs_, scratch);
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
  vcmpunordpd(rhs, temp1);                   //   lhs UNORD rhs
  vptest(temp1, temp1);                      // check if any unordered
  j(Assembler::Equal, &l);                   //   and exit if not

  // Slow path.
  // output has result for non-NaN lanes, garbage in NaN lanes.
  // temp1 has lhs UNORD rhs.
  // temp2 is dead.

  vmovapd(temp1, temp2);                     // clear NaN lanes of result
  vpandn(output, temp2, temp2);              //   result now in temp2
  asMasm().vpandSimd128(quietBits, temp1);   // setup QNaN bits in NaN lanes
  vorpd(temp1, temp2, temp2);                //   and OR into result
  vmovapd(lhs, temp1);                       // find NaN lanes
  vcmpunordpd(Operand(temp1), temp1);        //   in lhs
  vmovapd(temp1, output);                    //     (and save them for later)
  vandpd(lhs, temp1, temp1);                 //       and extract the NaNs
  vorpd(temp1, temp2, temp2);                //         and add to the result
  vmovapd(rhs, temp1);                       // find NaN lanes
  vcmpunordpd(Operand(temp1), temp1);        //   in rhs
  vpandn(temp1, output, output);             //     except if they were in lhs
  vandpd(rhs, output, output);               //       and extract the NaNs
  vorpd(temp2, output, output);              //         and add to the result

  bind(&l);
  /* clang-format on */
}

void MacroAssemblerX86Shared::minFloat32x4(FloatRegister lhs, Operand rhs,
                                           FloatRegister temp1,
                                           FloatRegister temp2,
                                           FloatRegister output) {
  minMaxFloat32x4(/*isMin=*/true, lhs, rhs, temp1, temp2, output);
}

void MacroAssemblerX86Shared::maxFloat32x4(FloatRegister lhs, Operand rhs,
                                           FloatRegister temp1,
                                           FloatRegister temp2,
                                           FloatRegister output) {
  minMaxFloat32x4(/*isMin=*/false, lhs, rhs, temp1, temp2, output);
}

void MacroAssemblerX86Shared::minFloat64x2(FloatRegister lhs, Operand rhs,
                                           FloatRegister temp1,
                                           FloatRegister temp2,
                                           FloatRegister output) {
  minMaxFloat64x2(/*isMin=*/true, lhs, rhs, temp1, temp2, output);
}

void MacroAssemblerX86Shared::maxFloat64x2(FloatRegister lhs, Operand rhs,
                                           FloatRegister temp1,
                                           FloatRegister temp2,
                                           FloatRegister output) {
  minMaxFloat64x2(/*isMin=*/false, lhs, rhs, temp1, temp2, output);
}

static inline void MaskSimdShiftCount(MacroAssembler& masm, unsigned shiftmask,
                                      Register count, Register temp,
                                      FloatRegister dest) {
  masm.mov(count, temp);
  masm.andl(Imm32(shiftmask), temp);
  masm.vmovd(temp, dest);
}

void MacroAssemblerX86Shared::packedShiftByScalarInt8x16(
    FloatRegister in, Register count, Register temp, FloatRegister xtmp,
    FloatRegister dest,
    void (MacroAssemblerX86Shared::*shift)(FloatRegister, FloatRegister,
                                           FloatRegister),
    void (MacroAssemblerX86Shared::*extend)(const Operand&, FloatRegister)) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 7, count, temp, scratch);

  // High bytes
  vpalignr(Operand(in), xtmp, 8);
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
    FloatRegister in, Register count, Register temp, FloatRegister xtmp,
    FloatRegister dest) {
  packedShiftByScalarInt8x16(in, count, temp, xtmp, dest,
                             &MacroAssemblerX86Shared::vpsllw,
                             &MacroAssemblerX86Shared::vpmovzxbw);
}

void MacroAssemblerX86Shared::packedLeftShiftByScalarInt8x16(
    Imm32 count, FloatRegister src, FloatRegister dest) {
  MOZ_ASSERT(count.value <= 7);
  asMasm().moveSimd128(src, dest);
  // Use the doubling trick for low shift counts, otherwise mask off the bits
  // that are shifted out of the low byte of each word and use word shifts.  The
  // optimal cutoff remains to be explored.
  if (count.value <= 3) {
    for (int32_t shift = count.value; shift > 0; --shift) {
      asMasm().addInt8x16(dest, dest);
    }
  } else {
    asMasm().bitwiseAndSimd128(SimdConstant::SplatX16(0xFF >> count.value),
                               dest);
    vpsllw(count, dest, dest);
  }
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt8x16(
    FloatRegister in, Register count, Register temp, FloatRegister xtmp,
    FloatRegister dest) {
  packedShiftByScalarInt8x16(in, count, temp, xtmp, dest,
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
    FloatRegister in, Register count, Register temp, FloatRegister xtmp,
    FloatRegister dest) {
  packedShiftByScalarInt8x16(in, count, temp, xtmp, dest,
                             &MacroAssemblerX86Shared::vpsrlw,
                             &MacroAssemblerX86Shared::vpmovzxbw);
}

void MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt8x16(
    Imm32 count, FloatRegister src, FloatRegister dest) {
  MOZ_ASSERT(count.value <= 7);
  asMasm().moveSimd128(src, dest);
  asMasm().bitwiseAndSimd128(
      SimdConstant::SplatX16((0xFF << count.value) & 0xFF), dest);
  vpsrlw(count, dest, dest);
}

void MacroAssemblerX86Shared::packedLeftShiftByScalarInt16x8(
    FloatRegister in, Register count, Register temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 15, count, temp, scratch);
  vpsllw(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt16x8(
    FloatRegister in, Register count, Register temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 15, count, temp, scratch);
  vpsraw(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt16x8(
    FloatRegister in, Register count, Register temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 15, count, temp, scratch);
  vpsrlw(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedLeftShiftByScalarInt32x4(
    FloatRegister in, Register count, Register temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 31, count, temp, scratch);
  vpslld(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt32x4(
    FloatRegister in, Register count, Register temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 31, count, temp, scratch);
  vpsrad(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt32x4(
    FloatRegister in, Register count, Register temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 31, count, temp, scratch);
  vpsrld(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedLeftShiftByScalarInt64x2(
    FloatRegister in, Register count, Register temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 63, count, temp, scratch);
  vpsllq(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt64x2(
    FloatRegister in, Register count, Register temp1, FloatRegister temp2,
    FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 63, count, temp1, temp2);
  asMasm().moveSimd128(in, dest);
  asMasm().signReplicationInt64x2(in, scratch);
  // Invert if negative, shift all, invert back if negative.
  vpxor(Operand(scratch), dest, dest);
  vpsrlq(temp2, dest, dest);
  vpxor(Operand(scratch), dest, dest);
}

void MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt64x2(
    FloatRegister in, Register count, Register temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  MaskSimdShiftCount(asMasm(), 63, count, temp, scratch);
  vpsrlq(scratch, in, dest);
}

void MacroAssemblerX86Shared::packedRightShiftByScalarInt64x2(
    Imm32 count, FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  asMasm().moveSimd128(src, dest);
  asMasm().signReplicationInt64x2(src, scratch);
  // Invert if negative, shift all, invert back if negative.
  vpxor(Operand(scratch), dest, dest);
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

  asMasm().moveSimd128Int(onTrue, output);
  asMasm().moveSimd128Int(mask, temp);

  vpand(Operand(temp), output, output);
  vpandn(Operand(onFalse), temp, temp);
  vpor(Operand(temp), output, output);
}

// Code sequences for int32x4<->float32x4 culled from v8; commentary added.

void MacroAssemblerX86Shared::unsignedConvertInt32x4ToFloat32x4(
    FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  asMasm().moveSimd128Int(src, dest);
  vpxor(Operand(scratch), scratch, scratch);  // extract low bits
  vpblendw(0x55, dest, scratch, scratch);     //   into scratch
  vpsubd(Operand(scratch), dest, dest);       //     and high bits into dest
  vcvtdq2ps(scratch, scratch);                // convert low bits
  vpsrld(Imm32(1), dest, dest);               // get high into unsigned range
  vcvtdq2ps(dest, dest);                      //   convert
  vaddps(Operand(dest), dest, dest);          //     and back into signed
  vaddps(Operand(scratch), dest, dest);       // combine high+low: may round
}

void MacroAssemblerX86Shared::truncSatFloat32x4ToInt32x4(FloatRegister src,
                                                         FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  asMasm().moveSimd128Float(src, dest);

  // The cvttps2dq instruction is the workhorse but does not handle NaN or out
  // of range values as we need it to.  We want to saturate too-large positive
  // values to 7FFFFFFFh and too-large negative values to 80000000h.  NaN and -0
  // become 0.

  // Convert NaN to 0 by masking away values that compare unordered to itself.
  vmovaps(dest, scratch);
  vcmpeqps(Operand(scratch), scratch);
  vpand(Operand(scratch), dest, dest);

  // Compute the complement of each non-NaN lane's sign bit, we'll need this to
  // correct the result of cvttps2dq.  All other output bits are garbage.
  vpxor(Operand(dest), scratch, scratch);

  // Convert.  This will make the output 80000000h if the input is out of range.
  vcvttps2dq(dest, dest);

  // Preserve the computed complemented sign bit if the output was 80000000h.
  // The sign bit will be 1 precisely for nonnegative values that overflowed.
  vpand(Operand(dest), scratch, scratch);

  // Create a mask with that sign bit.  Now a lane is either FFFFFFFFh if there
  // was a positive overflow, otherwise zero.
  vpsrad(Imm32(31), scratch, scratch);

  // Convert overflow lanes to 0x7FFFFFFF.
  vpxor(Operand(scratch), dest, dest);
}

void MacroAssemblerX86Shared::unsignedTruncSatFloat32x4ToInt32x4(
    FloatRegister src, FloatRegister temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  asMasm().moveSimd128Float(src, dest);

  // The cvttps2dq instruction is the workhorse but does not handle NaN or out
  // of range values as we need it to.  We want to saturate too-large positive
  // values to FFFFFFFFh and negative values to zero.  NaN and -0 become 0.

  // Convert NaN and negative values to zeroes in dest.
  vpxor(Operand(scratch), scratch, scratch);
  vmaxps(Operand(scratch), dest, dest);

  // Place the largest positive signed integer in all lanes in scratch.
  // We use it to bias the conversion to handle edge cases.
  asMasm().loadConstantSimd128Float(SimdConstant::SplatX4(2147483647.f),
                                    scratch);

  // temp = dest - 7FFFFFFFh (as floating), this brings integers in the unsigned
  // range but above the signed range into the signed range; 0 => -7FFFFFFFh.
  vmovaps(dest, temp);
  vsubps(Operand(scratch), temp, temp);

  // scratch = mask of biased values that are greater than 7FFFFFFFh.
  vcmpleps(Operand(temp), scratch);

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

void MacroAssemblerX86Shared::unsignedConvertInt32x4ToFloat64x2(
    FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  vmovaps(src, dest);

  asMasm().loadConstantSimd128Float(SimdConstant::SplatX4(0x43300000), scratch);
  vunpcklps(scratch, dest, dest);

  asMasm().loadConstantSimd128Float(SimdConstant::SplatX2(4503599627370496.0),
                                    scratch);
  vsubpd(Operand(scratch), dest, dest);
}

void MacroAssemblerX86Shared::truncSatFloat64x2ToInt32x4(FloatRegister src,
                                                         FloatRegister temp,
                                                         FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());

  vmovapd(src, scratch);
  vcmpeqpd(Operand(scratch), scratch);
  asMasm().moveSimd128Float(src, dest);
  asMasm().loadConstantSimd128Float(SimdConstant::SplatX2(2147483647.0), temp);
  vandpd(Operand(temp), scratch, scratch);
  vminpd(Operand(scratch), dest, dest);
  vcvttpd2dq(dest, dest);
}

void MacroAssemblerX86Shared::unsignedTruncSatFloat64x2ToInt32x4(
    FloatRegister src, FloatRegister temp, FloatRegister dest) {
  ScratchSimd128Scope scratch(asMasm());
  asMasm().moveSimd128Float(src, dest);

  vxorpd(scratch, scratch, scratch);
  vmaxpd(Operand(scratch), dest, dest);

  asMasm().loadConstantSimd128Float(SimdConstant::SplatX2(4294967295.0), temp);
  vminpd(Operand(temp), dest, dest);
  vroundpd(SSERoundingMode::Trunc, Operand(dest), dest);
  asMasm().loadConstantSimd128Float(SimdConstant::SplatX2(4503599627370496.0),
                                    temp);
  vaddpd(Operand(temp), dest, dest);
  vshufps(0x88, scratch, dest, dest);
}

void MacroAssemblerX86Shared::popcntInt8x16(FloatRegister src,
                                            FloatRegister temp,
                                            FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  asMasm().loadConstantSimd128Float(SimdConstant::SplatX16(0x0f), scratch);
  asMasm().moveSimd128Int(src, temp);
  vpand(scratch, temp, temp);
  vpandn(src, scratch, scratch);
  int8_t counts[] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
  asMasm().loadConstantSimd128(SimdConstant::CreateX16(counts), output);
  vpsrlw(Imm32(4), scratch, scratch);
  vpshufb(temp, output, output);
  asMasm().loadConstantSimd128(SimdConstant::CreateX16(counts), temp);
  vpshufb(scratch, temp, temp);
  vpaddb(Operand(temp), output, output);
}
