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

// The following routines are from the old asm.js implementation but are UNUSED
// in the wasm implementation currently.  They are preserved here because it's
// sad to throw out working code.  They are defined in the header file.
//
// Before using these, they should minimally be moved to
// MacroAssembler-x86-shared-SIMD.cpp, and it would be a wrong move to assume
// that they are correct according to the wasm spec.

void MacroAssemblerX86Shared::checkedConvertFloat32x4ToInt32x4(
    FloatRegister src, FloatRegister dest, Register temp, Label* oolEntry,
    Label* rejoin) {
  // Does the conversion and jumps to the OOL entry if the result value
  // is the undefined integer pattern.
  static const SimdConstant InvalidResult =
      SimdConstant::SplatX4(int32_t(-2147483648));
  convertFloat32x4ToInt32x4(src, dest);

  ScratchSimd128Scope scratch(asMasm());
  asMasm().loadConstantSimd128Int(InvalidResult, scratch);
  vpcmpeqd(Operand(dest), scratch, scratch);
  // TODO (bug 1156228): If we have SSE4.1, we can use PTEST here instead of
  // the two following instructions.
  vmovmskps(scratch, temp);
  cmp32(temp, Imm32(0));
  j(Assembler::NotEqual, oolEntry);
  bind(rejoin);
}

void MacroAssemblerX86Shared::oolConvertFloat32x4ToInt32x4(
    FloatRegister src, Register temp, Label* rejoin, Label* onConversionError) {
  static const SimdConstant Int32MaxX4 = SimdConstant::SplatX4(2147483647.f);
  static const SimdConstant Int32MinX4 = SimdConstant::SplatX4(-2147483648.f);

  ScratchSimd128Scope scratch(asMasm());
  asMasm().loadConstantSimd128Float(Int32MinX4, scratch);
  vcmpleps(Operand(src), scratch);
  vmovmskps(scratch, temp);
  cmp32(temp, Imm32(15));
  j(Assembler::NotEqual, onConversionError);

  asMasm().loadConstantSimd128Float(Int32MaxX4, scratch);
  vcmpleps(Operand(src), scratch);
  vmovmskps(scratch, temp);
  cmp32(temp, Imm32(0));
  j(Assembler::NotEqual, onConversionError);

  jump(rejoin);
}

void MacroAssemblerX86Shared::checkedConvertFloat32x4ToUint32x4(
    FloatRegister in, FloatRegister out, Register temp, FloatRegister tempF,
    Label* failed) {
  // Classify lane values into 4 disjoint classes:
  //
  //   N-lanes:             in <= -1.0
  //   A-lanes:      -1.0 < in <= 0x0.ffffffp31
  //   B-lanes: 0x1.0p31 <= in <= 0x0.ffffffp32
  //   V-lanes: 0x1.0p32 <= in, or isnan(in)
  //
  // We need to bail out to throw a RangeError if we see any N-lanes or
  // V-lanes.
  //
  // For A-lanes and B-lanes, we make two float -> int32 conversions:
  //
  //   A = cvttps2dq(in)
  //   B = cvttps2dq(in - 0x1.0p31f)
  //
  // Note that the subtraction for the B computation is exact for B-lanes.
  // There is no rounding, so B is the low 31 bits of the correctly converted
  // result.
  //
  // The cvttps2dq instruction produces 0x80000000 when the input is NaN or
  // out of range for a signed int32_t. This conveniently provides the missing
  // high bit for B, so the desired result is A for A-lanes and A|B for
  // B-lanes.

  ScratchSimd128Scope scratch(asMasm());

  // TODO: If the majority of lanes are A-lanes, it could be faster to compute
  // A first, use vmovmskps to check for any non-A-lanes and handle them in
  // ool code. OTOH, we we're wrong about the lane distribution, that would be
  // slower.

  // Compute B in |scratch|.
  static const float Adjust = 0x80000000;  // 0x1.0p31f for the benefit of MSVC.
  static const SimdConstant Bias = SimdConstant::SplatX4(-Adjust);
  asMasm().loadConstantSimd128Float(Bias, scratch);
  packedAddFloat32(Operand(in), scratch);
  convertFloat32x4ToInt32x4(scratch, scratch);

  // Compute A in |out|. This is the last time we use |in| and the first time
  // we use |out|, so we can tolerate if they are the same register.
  convertFloat32x4ToInt32x4(in, out);

  // We can identify A-lanes by the sign bits in A: Any A-lanes will be
  // positive in A, and N, B, and V-lanes will be 0x80000000 in A. Compute a
  // mask of non-A-lanes into |tempF|.
  zeroSimd128Float(tempF);
  vpcmpgtd(Operand(out), tempF, tempF);

  // Clear the A-lanes in B.
  bitwiseAndSimdInt(scratch, Operand(tempF), scratch);

  // Compute the final result: A for A-lanes, A|B for B-lanes.
  bitwiseOrSimdInt(out, Operand(scratch), out);

  // We still need to filter out the V-lanes. They would show up as 0x80000000
  // in both A and B. Since we cleared the valid A-lanes in B, the V-lanes are
  // the remaining negative lanes in B.
  vmovmskps(scratch, temp);
  cmp32(temp, Imm32(0));
  j(Assembler::NotEqual, failed);
}

void MacroAssemblerX86Shared::createInt32x4(Register lane0, Register lane1,
                                            Register lane2, Register lane3,
                                            FloatRegister dest) {
  if (AssemblerX86Shared::HasSSE41()) {
    vmovd(lane0, dest);
    vpinsrd(1, lane1, dest, dest);
    vpinsrd(2, lane2, dest, dest);
    vpinsrd(3, lane3, dest, dest);
    return;
  }

  asMasm().reserveStack(Simd128DataSize);
  store32(lane0, Address(StackPointer, 0 * sizeof(int32_t)));
  store32(lane1, Address(StackPointer, 1 * sizeof(int32_t)));
  store32(lane2, Address(StackPointer, 2 * sizeof(int32_t)));
  store32(lane3, Address(StackPointer, 3 * sizeof(int32_t)));
  loadAlignedSimd128Int(Address(StackPointer, 0), dest);
  asMasm().freeStack(Simd128DataSize);
}

void MacroAssemblerX86Shared::createFloat32x4(
    FloatRegister lane0, FloatRegister lane1, FloatRegister lane2,
    FloatRegister lane3, FloatRegister temp, FloatRegister output) {
  FloatRegister lane0Copy = reusedInputSimd128Float(lane0, output);
  FloatRegister lane1Copy = reusedInputSimd128Float(lane1, temp);
  vunpcklps(lane3, lane1Copy, temp);
  vunpcklps(lane2, lane0Copy, output);
  vunpcklps(temp, output, output);
}

void MacroAssemblerX86Shared::reinterpretSimd(bool isIntegerLaneType,
                                              FloatRegister input,
                                              FloatRegister output) {
  if (input.aliases(output)) {
    return;
  }
  if (isIntegerLaneType) {
    vmovdqa(input, output);
  } else {
    vmovaps(input, output);
  }
}

void MacroAssemblerX86Shared::extractLaneSimdBool(FloatRegister input,
                                                  Register output,
                                                  unsigned numLanes,
                                                  unsigned lane) {
  switch (numLanes) {
    case 4:
      extractLaneInt32x4(input, output, lane);
      break;
    case 8:
      // Get a lane, don't bother fixing the high bits since we'll mask below.
      extractLaneInt16x8(input, output, lane, SimdSign::NotApplicable);
      break;
    case 16:
      extractLaneInt8x16(input, output, lane, SimdSign::NotApplicable);
      break;
    default:
      MOZ_CRASH("Unhandled SIMD number of lanes");
  }
  // We need to generate a 0/1 value. We have 0/-1 and possibly dirty high bits.
  asMasm().and32(Imm32(1), output);
}

void MacroAssemblerX86Shared::allTrueSimdBool(FloatRegister input,
                                              Register output) {
  // We know that the input lanes are boolean, so they are either 0 or -1.
  // The all-true vector has all 128 bits set, no matter the lane geometry.
  vpmovmskb(input, output);
  cmp32(output, Imm32(0xffff));
  emitSet(Assembler::Zero, output);
}

void MacroAssemblerX86Shared::anyTrueSimdBool(FloatRegister input,
                                              Register output) {
  vpmovmskb(input, output);
  cmp32(output, Imm32(0x0));
  emitSet(Assembler::NonZero, output);
}

void MacroAssemblerX86Shared::swizzleInt32x4(FloatRegister input,
                                             FloatRegister output,
                                             unsigned lanes[4]) {
  uint32_t mask = MacroAssembler::ComputeShuffleMask(lanes[0], lanes[1],
                                                     lanes[2], lanes[3]);
  shuffleInt32(mask, input, output);
}

// For SIMD.js
void MacroAssemblerX86Shared::oldSwizzleInt8x16(FloatRegister input,
                                                FloatRegister output,
                                                const Maybe<Register>& temp,
                                                int8_t lanes[16]) {
  if (AssemblerX86Shared::HasSSSE3()) {
    ScratchSimd128Scope scratch(asMasm());
    asMasm().loadConstantSimd128Int(SimdConstant::CreateX16(lanes), scratch);
    FloatRegister inputCopy = reusedInputInt32x4(input, output);
    vpshufb(scratch, inputCopy, output);
    return;
  }

  // Worst-case fallback for pre-SSSE3 machines. Bounce through memory.
  MOZ_ASSERT(!!temp, "needs a temp for the memory fallback");
  asMasm().reserveStack(2 * Simd128DataSize);
  storeAlignedSimd128Int(input, Address(StackPointer, Simd128DataSize));
  for (unsigned i = 0; i < 16; i++) {
    load8ZeroExtend(Address(StackPointer, Simd128DataSize + lanes[i]), *temp);
    store8(*temp, Address(StackPointer, i));
  }
  loadAlignedSimd128Int(Address(StackPointer, 0), output);
  asMasm().freeStack(2 * Simd128DataSize);
}

static inline bool LanesMatch(unsigned lanes[4], unsigned x, unsigned y,
                              unsigned z, unsigned w) {
  return lanes[0] == x && lanes[1] == y && lanes[2] == z && lanes[3] == w;
}

void MacroAssemblerX86Shared::swizzleFloat32x4(FloatRegister input,
                                               FloatRegister output,
                                               unsigned lanes[4]) {
  if (AssemblerX86Shared::HasSSE3()) {
    if (LanesMatch(lanes, 0, 0, 2, 2)) {
      vmovsldup(input, output);
      return;
    }
    if (LanesMatch(lanes, 1, 1, 3, 3)) {
      vmovshdup(input, output);
      return;
    }
  }

  // TODO Here and below, arch specific lowering could identify this pattern
  // and use defineReuseInput to avoid this move (bug 1084404)
  if (LanesMatch(lanes, 2, 3, 2, 3)) {
    FloatRegister inputCopy = reusedInputSimd128Float(input, output);
    vmovhlps(input, inputCopy, output);
    return;
  }

  if (LanesMatch(lanes, 0, 1, 0, 1)) {
    if (AssemblerX86Shared::HasSSE3() && !AssemblerX86Shared::HasAVX()) {
      vmovddup(Operand(input), output);
      return;
    }
    FloatRegister inputCopy = reusedInputSimd128Float(input, output);
    vmovlhps(input, inputCopy, output);
    return;
  }

  if (LanesMatch(lanes, 0, 0, 1, 1)) {
    FloatRegister inputCopy = reusedInputSimd128Float(input, output);
    vunpcklps(input, inputCopy, output);
    return;
  }

  if (LanesMatch(lanes, 2, 2, 3, 3)) {
    FloatRegister inputCopy = reusedInputSimd128Float(input, output);
    vunpckhps(input, inputCopy, output);
    return;
  }

  uint32_t x = lanes[0];
  uint32_t y = lanes[1];
  uint32_t z = lanes[2];
  uint32_t w = lanes[3];

  uint32_t mask = MacroAssembler::ComputeShuffleMask(x, y, z, w);
  shuffleFloat32(mask, input, output);
}

void MacroAssemblerX86Shared::shuffleX4(FloatRegister lhs, Operand rhs,
                                        FloatRegister out,
                                        const Maybe<FloatRegister>& maybeTemp,
                                        unsigned lanes[4]) {
  uint32_t x = lanes[0];
  uint32_t y = lanes[1];
  uint32_t z = lanes[2];
  uint32_t w = lanes[3];

  // Check that lanes come from LHS in majority:
  unsigned numLanesFromLHS = (x < 4) + (y < 4) + (z < 4) + (w < 4);
  MOZ_ASSERT(numLanesFromLHS >= 2);

  // When reading this method, remember that vshufps takes the two first
  // inputs of the destination operand (right operand) and the two last
  // inputs of the source operand (left operand).
  //
  // Legend for explanations:
  // - L: LHS
  // - R: RHS
  // - T: temporary

  uint32_t mask;

  // If all lanes came from a single vector, we should use swizzle instead.
  MOZ_ASSERT(numLanesFromLHS < 4);

  // If all values stay in their lane, this is a blend.
  if (AssemblerX86Shared::HasSSE41()) {
    if (x % 4 == 0 && y % 4 == 1 && z % 4 == 2 && w % 4 == 3) {
      vblendps(blendpsMask(x >= 4, y >= 4, z >= 4, w >= 4), rhs, lhs, out);
      return;
    }
  }

  // One element of the second, all other elements of the first
  if (numLanesFromLHS == 3) {
    unsigned firstMask = -1, secondMask = -1;

    // register-register vmovss preserves the high lanes.
    if (LanesMatch(lanes, 4, 1, 2, 3) && rhs.kind() == Operand::FPREG) {
      vmovss(FloatRegister::FromCode(rhs.fpu()), lhs, out);
      return;
    }

    // SSE4.1 vinsertps can handle any single element.
    unsigned numLanesUnchanged = (x == 0) + (y == 1) + (z == 2) + (w == 3);
    if (AssemblerX86Shared::HasSSE41() && numLanesUnchanged == 3) {
      unsigned srcLane;
      unsigned dstLane;
      if (x >= 4) {
        srcLane = x - 4;
        dstLane = 0;
      } else if (y >= 4) {
        srcLane = y - 4;
        dstLane = 1;
      } else if (z >= 4) {
        srcLane = z - 4;
        dstLane = 2;
      } else {
        MOZ_ASSERT(w >= 4);
        srcLane = w - 4;
        dstLane = 3;
      }
      vinsertps(vinsertpsMask(srcLane, dstLane), rhs, lhs, out);
      return;
    }

    MOZ_ASSERT(!!maybeTemp);
    FloatRegister rhsCopy = *maybeTemp;
    loadAlignedSimd128Float(rhs, rhsCopy);

    if (x < 4 && y < 4) {
      if (w >= 4) {
        w %= 4;
        // T = (Rw Rw Lz Lz) = vshufps(firstMask, lhs, rhs, rhsCopy)
        firstMask = MacroAssembler::ComputeShuffleMask(w, w, z, z);
        // (Lx Ly Lz Rw) = (Lx Ly Tz Tx) = vshufps(secondMask, T, lhs, out)
        secondMask = MacroAssembler::ComputeShuffleMask(x, y, 2, 0);
      } else {
        MOZ_ASSERT(z >= 4);
        z %= 4;
        // T = (Rz Rz Lw Lw) = vshufps(firstMask, lhs, rhs, rhsCopy)
        firstMask = MacroAssembler::ComputeShuffleMask(z, z, w, w);
        // (Lx Ly Rz Lw) = (Lx Ly Tx Tz) = vshufps(secondMask, T, lhs, out)
        secondMask = MacroAssembler::ComputeShuffleMask(x, y, 0, 2);
      }

      vshufps(firstMask, lhs, rhsCopy, rhsCopy);
      vshufps(secondMask, rhsCopy, lhs, out);
      return;
    }

    MOZ_ASSERT(z < 4 && w < 4);

    if (y >= 4) {
      y %= 4;
      // T = (Ry Ry Lx Lx) = vshufps(firstMask, lhs, rhs, rhsCopy)
      firstMask = MacroAssembler::ComputeShuffleMask(y, y, x, x);
      // (Lx Ry Lz Lw) = (Tz Tx Lz Lw) = vshufps(secondMask, lhs, T, out)
      secondMask = MacroAssembler::ComputeShuffleMask(2, 0, z, w);
    } else {
      MOZ_ASSERT(x >= 4);
      x %= 4;
      // T = (Rx Rx Ly Ly) = vshufps(firstMask, lhs, rhs, rhsCopy)
      firstMask = MacroAssembler::ComputeShuffleMask(x, x, y, y);
      // (Rx Ly Lz Lw) = (Tx Tz Lz Lw) = vshufps(secondMask, lhs, T, out)
      secondMask = MacroAssembler::ComputeShuffleMask(0, 2, z, w);
    }

    vshufps(firstMask, lhs, rhsCopy, rhsCopy);
    if (AssemblerX86Shared::HasAVX()) {
      vshufps(secondMask, lhs, rhsCopy, out);
    } else {
      vshufps(secondMask, lhs, rhsCopy, rhsCopy);
      moveSimd128Float(rhsCopy, out);
    }
    return;
  }

  // Two elements from one vector, two other elements from the other
  MOZ_ASSERT(numLanesFromLHS == 2);

  // TODO Here and below, symmetric case would be more handy to avoid a move,
  // but can't be reached because operands would get swapped (bug 1084404).
  if (LanesMatch(lanes, 2, 3, 6, 7)) {
    ScratchSimd128Scope scratch(asMasm());
    if (AssemblerX86Shared::HasAVX()) {
      FloatRegister rhsCopy = reusedInputAlignedSimd128Float(rhs, scratch);
      vmovhlps(lhs, rhsCopy, out);
    } else {
      loadAlignedSimd128Float(rhs, scratch);
      vmovhlps(lhs, scratch, scratch);
      moveSimd128Float(scratch, out);
    }
    return;
  }

  if (LanesMatch(lanes, 0, 1, 4, 5)) {
    FloatRegister rhsCopy;
    ScratchSimd128Scope scratch(asMasm());
    if (rhs.kind() == Operand::FPREG) {
      // No need to make an actual copy, since the operand is already
      // in a register, and it won't be clobbered by the vmovlhps.
      rhsCopy = FloatRegister::FromCode(rhs.fpu());
    } else {
      loadAlignedSimd128Float(rhs, scratch);
      rhsCopy = scratch;
    }
    vmovlhps(rhsCopy, lhs, out);
    return;
  }

  if (LanesMatch(lanes, 0, 4, 1, 5)) {
    vunpcklps(rhs, lhs, out);
    return;
  }

  // TODO swapped case would be better (bug 1084404)
  if (LanesMatch(lanes, 4, 0, 5, 1)) {
    ScratchSimd128Scope scratch(asMasm());
    if (AssemblerX86Shared::HasAVX()) {
      FloatRegister rhsCopy = reusedInputAlignedSimd128Float(rhs, scratch);
      vunpcklps(lhs, rhsCopy, out);
    } else {
      loadAlignedSimd128Float(rhs, scratch);
      vunpcklps(lhs, scratch, scratch);
      moveSimd128Float(scratch, out);
    }
    return;
  }

  if (LanesMatch(lanes, 2, 6, 3, 7)) {
    vunpckhps(rhs, lhs, out);
    return;
  }

  // TODO swapped case would be better (bug 1084404)
  if (LanesMatch(lanes, 6, 2, 7, 3)) {
    ScratchSimd128Scope scratch(asMasm());
    if (AssemblerX86Shared::HasAVX()) {
      FloatRegister rhsCopy = reusedInputAlignedSimd128Float(rhs, scratch);
      vunpckhps(lhs, rhsCopy, out);
    } else {
      loadAlignedSimd128Float(rhs, scratch);
      vunpckhps(lhs, scratch, scratch);
      moveSimd128Float(scratch, out);
    }
    return;
  }

  // In one vshufps
  if (x < 4 && y < 4) {
    mask = MacroAssembler::ComputeShuffleMask(x, y, z % 4, w % 4);
    vshufps(mask, rhs, lhs, out);
    return;
  }

  // At creation, we should have explicitly swapped in this case.
  MOZ_ASSERT(!(z >= 4 && w >= 4));

  // In two vshufps, for the most generic case:
  uint32_t firstMask[4], secondMask[4];
  unsigned i = 0, j = 2, k = 0;

#define COMPUTE_MASK(lane)   \
  if (lane >= 4) {           \
    firstMask[j] = lane % 4; \
    secondMask[k++] = j++;   \
  } else {                   \
    firstMask[i] = lane;     \
    secondMask[k++] = i++;   \
  }

  COMPUTE_MASK(x)
  COMPUTE_MASK(y)
  COMPUTE_MASK(z)
  COMPUTE_MASK(w)
#undef COMPUTE_MASK

  MOZ_ASSERT(i == 2 && j == 4 && k == 4);

  mask = MacroAssembler::ComputeShuffleMask(firstMask[0], firstMask[1],
                                            firstMask[2], firstMask[3]);
  vshufps(mask, rhs, lhs, lhs);

  mask = MacroAssembler::ComputeShuffleMask(secondMask[0], secondMask[1],
                                            secondMask[2], secondMask[3]);
  vshufps(mask, lhs, lhs, lhs);
}

void MacroAssemblerX86Shared::minNumFloat32x4(FloatRegister lhs, Operand rhs,
                                              FloatRegister temp,
                                              FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  asMasm().loadConstantSimd128Int(SimdConstant::SplatX4(int32_t(0x80000000)),
                                  temp);

  FloatRegister mask = scratch;
  FloatRegister tmpCopy = reusedInputSimd128Float(temp, scratch);
  vpcmpeqd(Operand(lhs), tmpCopy, mask);
  vandps(temp, mask, mask);

  FloatRegister lhsCopy = reusedInputSimd128Float(lhs, temp);
  vminps(rhs, lhsCopy, temp);
  vorps(mask, temp, temp);

  if (AssemblerX86Shared::HasAVX()) {
    MOZ_CRASH("Can do better by avoiding the movaps");
  } else {
    vmovaps(rhs, mask);
    vcmpneqps(rhs, mask);
  }

  if (AssemblerX86Shared::HasAVX()) {
    vblendvps(mask, lhs, temp, output);
  } else {
    // Emulate vblendvps.
    // With SSE.4.1 we could use blendvps, however it's awkward since
    // it requires the mask to be in xmm0.
    if (lhs != output) {
      moveSimd128Float(lhs, output);
    }
    vandps(Operand(mask), output, output);
    vandnps(Operand(temp), mask, mask);
    vorps(Operand(mask), output, output);
  }
}

void MacroAssemblerX86Shared::maxNumFloat32x4(FloatRegister lhs, Operand rhs,
                                              FloatRegister temp,
                                              FloatRegister output) {
  ScratchSimd128Scope scratch(asMasm());
  FloatRegister mask = scratch;

  asMasm().loadConstantSimd128Int(SimdConstant::SplatX4(0), mask);
  vpcmpeqd(Operand(lhs), mask, mask);

  asMasm().loadConstantSimd128Int(SimdConstant::SplatX4(int32_t(0x80000000)),
                                  temp);
  vandps(temp, mask, mask);

  FloatRegister lhsCopy = reusedInputSimd128Float(lhs, temp);
  vmaxps(rhs, lhsCopy, temp);
  vandnps(Operand(temp), mask, mask);

  // Ensure temp always contains the temporary result
  mask = temp;
  temp = scratch;

  if (AssemblerX86Shared::HasAVX()) {
    MOZ_CRASH("Can do better by avoiding the movaps");
  } else {
    vmovaps(rhs, mask);
    vcmpneqps(rhs, mask);
  }

  if (AssemblerX86Shared::HasAVX()) {
    vblendvps(mask, lhs, temp, output);
  } else {
    // Emulate vblendvps.
    // With SSE.4.1 we could use blendvps, however it's awkward since
    // it requires the mask to be in xmm0.
    if (lhs != output) {
      moveSimd128Float(lhs, output);
    }
    vandps(Operand(mask), output, output);
    vandnps(Operand(temp), mask, mask);
    vorps(Operand(mask), output, output);
  }
}

void MacroAssemblerX86Shared::selectX4(FloatRegister mask, FloatRegister onTrue,
                                       FloatRegister onFalse,
                                       FloatRegister temp,
                                       FloatRegister output) {
  if (AssemblerX86Shared::HasAVX()) {
    vblendvps(mask, onTrue, onFalse, output);
  } else {
    selectSimd128(mask, onTrue, onFalse, temp, output);
  }
}
