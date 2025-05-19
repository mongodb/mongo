/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ShuffleAnalysis.h"
#include "mozilla/MathAlgorithms.h"
#include "jit/MIR.h"
#include "wasm/WasmFeatures.h"

using namespace js;
using namespace jit;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

#ifdef ENABLE_WASM_SIMD

// Specialization analysis for SIMD operations.  This is still x86-centric but
// generalizes fairly easily to other architectures.

// Optimization of v8x16.shuffle.  The general byte shuffle+blend is very
// expensive (equivalent to at least a dozen instructions), and we want to avoid
// that if we can.  So look for special cases - there are many.
//
// The strategy is to sort the operation into one of three buckets depending
// on the shuffle pattern and inputs:
//
//  - single operand; shuffles on these values are rotations, reversals,
//    transpositions, and general permutations
//  - single-operand-with-interesting-constant (especially zero); shuffles on
//    these values are often byte shift or scatter operations
//  - dual operand; shuffles on these operations are blends, catenated
//    shifts, and (in the worst case) general shuffle+blends
//
// We're not trying to solve the general problem, only to lower reasonably
// expressed patterns that express common operations.  Producers that produce
// dense and convoluted patterns will end up with the general byte shuffle.
// Producers that produce simpler patterns that easily map to hardware will
// get faster code.
//
// In particular, these matchers do not try to combine transformations, so a
// shuffle that optimally is lowered to rotate + permute32x4 + rotate, say, is
// usually going to end up as a general byte shuffle.

// Reduce a 0..31 byte mask to a 0..15 word mask if possible and if so return
// true, updating *control.
static bool ByteMaskToWordMask(SimdConstant* control) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();
  int16_t controlWords[8];
  for (int i = 0; i < 16; i += 2) {
    if (!((lanes[i] & 1) == 0 && lanes[i + 1] == lanes[i] + 1)) {
      return false;
    }
    controlWords[i / 2] = int16_t(lanes[i] / 2);
  }
  *control = SimdConstant::CreateX8(controlWords);
  return true;
}

// Reduce a 0..31 byte mask to a 0..7 dword mask if possible and if so return
// true, updating *control.
static bool ByteMaskToDWordMask(SimdConstant* control) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();
  int32_t controlDWords[4];
  for (int i = 0; i < 16; i += 4) {
    if (!((lanes[i] & 3) == 0 && lanes[i + 1] == lanes[i] + 1 &&
          lanes[i + 2] == lanes[i] + 2 && lanes[i + 3] == lanes[i] + 3)) {
      return false;
    }
    controlDWords[i / 4] = lanes[i] / 4;
  }
  *control = SimdConstant::CreateX4(controlDWords);
  return true;
}

// Reduce a 0..31 byte mask to a 0..3 qword mask if possible and if so return
// true, updating *control.
static bool ByteMaskToQWordMask(SimdConstant* control) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();
  int64_t controlQWords[2];
  for (int i = 0; i < 16; i += 8) {
    if (!((lanes[i] & 7) == 0 && lanes[i + 1] == lanes[i] + 1 &&
          lanes[i + 2] == lanes[i] + 2 && lanes[i + 3] == lanes[i] + 3 &&
          lanes[i + 4] == lanes[i] + 4 && lanes[i + 5] == lanes[i] + 5 &&
          lanes[i + 6] == lanes[i] + 6 && lanes[i + 7] == lanes[i] + 7)) {
      return false;
    }
    controlQWords[i / 8] = lanes[i] / 8;
  }
  *control = SimdConstant::CreateX2(controlQWords);
  return true;
}

// Skip across consecutive values in lanes starting at i, returning the index
// after the last element.  Lane values must be <= len-1 ("masked").
//
// Since every element is a 1-element run, the return value is never the same as
// the starting i.
template <typename T>
static int ScanIncreasingMasked(const T* lanes, int i) {
  int len = int(16 / sizeof(T));
  MOZ_ASSERT(i < len);
  MOZ_ASSERT(lanes[i] <= len - 1);
  i++;
  while (i < len && lanes[i] == lanes[i - 1] + 1) {
    MOZ_ASSERT(lanes[i] <= len - 1);
    i++;
  }
  return i;
}

// Skip across consecutive values in lanes starting at i, returning the index
// after the last element.  Lane values must be <= len*2-1 ("unmasked"); the
// values len-1 and len are not considered consecutive.
//
// Since every element is a 1-element run, the return value is never the same as
// the starting i.
template <typename T>
static int ScanIncreasingUnmasked(const T* lanes, int i) {
  int len = int(16 / sizeof(T));
  MOZ_ASSERT(i < len);
  if (lanes[i] < len) {
    i++;
    while (i < len && lanes[i] < len && lanes[i - 1] == lanes[i] - 1) {
      i++;
    }
  } else {
    i++;
    while (i < len && lanes[i] >= len && lanes[i - 1] == lanes[i] - 1) {
      i++;
    }
  }
  return i;
}

// Skip lanes that equal v starting at i, returning the index just beyond the
// last of those.  There is no requirement that the initial lanes[i] == v.
template <typename T>
static int ScanConstant(const T* lanes, int v, int i) {
  int len = int(16 / sizeof(T));
  MOZ_ASSERT(i <= len);
  while (i < len && lanes[i] == v) {
    i++;
  }
  return i;
}

// Mask lane values denoting rhs elements into lhs elements.
template <typename T>
static void MaskLanes(T* result, const T* input) {
  int len = int(16 / sizeof(T));
  for (int i = 0; i < len; i++) {
    result[i] = input[i] & (len - 1);
  }
}

// Apply a transformation to each lane value.
template <typename T>
static void MapLanes(T* result, const T* input, int (*f)(int)) {
  // Hazard analysis trips on "IndirectCall: f" error.
  // Suppress the check -- `f` is expected to be trivial here.
  JS::AutoSuppressGCAnalysis nogc;

  int len = int(16 / sizeof(T));
  for (int i = 0; i < len; i++) {
    result[i] = f(input[i]);
  }
}

// Recognize an identity permutation, assuming lanes is masked.
template <typename T>
static bool IsIdentity(const T* lanes) {
  return ScanIncreasingMasked(lanes, 0) == int(16 / sizeof(T));
}

// Recognize part of an identity permutation starting at start, with
// the first value of the permutation expected to be bias.
template <typename T>
static bool IsIdentity(const T* lanes, int start, int len, int bias) {
  if (lanes[start] != bias) {
    return false;
  }
  for (int i = start + 1; i < start + len; i++) {
    if (lanes[i] != lanes[i - 1] + 1) {
      return false;
    }
  }
  return true;
}

// We can permute by dwords if the mask is reducible to a dword mask, and in
// this case a single PSHUFD is enough.
static bool TryPermute32x4(SimdConstant* control) {
  SimdConstant tmp = *control;
  if (!ByteMaskToDWordMask(&tmp)) {
    return false;
  }
  *control = tmp;
  return true;
}

// Can we perform a byte rotate right?  We can use PALIGNR.  The shift count is
// just lanes[0], and *control is unchanged.
static bool TryRotateRight8x16(SimdConstant* control) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();
  // Look for the end of the first run of consecutive bytes.
  int i = ScanIncreasingMasked(lanes, 0);

  // First run must start at a value s.t. we have a rotate if all remaining
  // bytes are a run.
  if (lanes[0] != 16 - i) {
    return false;
  }

  // If we reached the end of the vector, we're done.
  if (i == 16) {
    return true;
  }

  // Second run must start at source lane zero.
  if (lanes[i] != 0) {
    return false;
  }

  // Second run must end at the end of the lane vector.
  return ScanIncreasingMasked(lanes, i) == 16;
}

// We can permute by words if the mask is reducible to a word mask.
static bool TryPermute16x8(SimdConstant* control) {
  SimdConstant tmp = *control;
  if (!ByteMaskToWordMask(&tmp)) {
    return false;
  }
  *control = tmp;
  return true;
}

// A single word lane is copied into all the other lanes: PSHUF*W + PSHUFD.
static bool TryBroadcast16x8(SimdConstant* control) {
  SimdConstant tmp = *control;
  if (!ByteMaskToWordMask(&tmp)) {
    return false;
  }
  const SimdConstant::I16x8& lanes = tmp.asInt16x8();
  if (ScanConstant(lanes, lanes[0], 0) < 8) {
    return false;
  }
  *control = tmp;
  return true;
}

// A single byte lane is copied int all the other lanes: PUNPCK*BW + PSHUF*W +
// PSHUFD.
static bool TryBroadcast8x16(SimdConstant* control) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();
  return ScanConstant(lanes, lanes[0], 0) >= 16;
}

template <int N>
static bool TryReverse(SimdConstant* control) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();
  for (int i = 0; i < 16; i++) {
    if (lanes[i] != (i ^ (N - 1))) {
      return false;
    }
  }
  return true;
}

// Look for permutations of a single operand.
static SimdPermuteOp AnalyzePermute(SimdConstant* control) {
  // Lane indices are input-agnostic for single-operand permutations.
  SimdConstant::I8x16 controlBytes;
  MaskLanes(controlBytes, control->asInt8x16());

  // Get rid of no-ops immediately, so nobody else needs to check.
  if (IsIdentity(controlBytes)) {
    return SimdPermuteOp::MOVE;
  }

  // Default control is the masked bytes.
  *control = SimdConstant::CreateX16(controlBytes);

  // Analysis order matters here and is architecture-dependent or even
  // microarchitecture-dependent: ideally the cheapest implementation first.
  // The Intel manual says that the cost of a PSHUFB is about five other
  // operations, so make that our cutoff.
  //
  // Word, dword, and qword reversals are handled optimally by general permutes.
  //
  // Byte reversals are probably best left to PSHUFB, no alternative rendition
  // seems to reliably go below five instructions.  (Discuss.)
  //
  // Word swaps within doublewords and dword swaps within quadwords are handled
  // optimally by general permutes.
  //
  // Dword and qword broadcasts are handled by dword permute.

  if (TryPermute32x4(control)) {
    return SimdPermuteOp::PERMUTE_32x4;
  }
  if (TryRotateRight8x16(control)) {
    return SimdPermuteOp::ROTATE_RIGHT_8x16;
  }
  if (TryBroadcast16x8(control)) {
    return SimdPermuteOp::BROADCAST_16x8;
  }
  if (TryPermute16x8(control)) {
    return SimdPermuteOp::PERMUTE_16x8;
  }
  if (TryBroadcast8x16(control)) {
    return SimdPermuteOp::BROADCAST_8x16;
  }
  if (TryReverse<2>(control)) {
    return SimdPermuteOp::REVERSE_16x8;
  }
  if (TryReverse<4>(control)) {
    return SimdPermuteOp::REVERSE_32x4;
  }
  if (TryReverse<8>(control)) {
    return SimdPermuteOp::REVERSE_64x2;
  }

  // TODO: (From v8) Unzip and transpose generally have renditions that slightly
  // beat a general permute (three or four instructions)
  //
  // TODO: (From MacroAssemblerX86Shared::ShuffleX4): MOVLHPS and MOVHLPS can be
  // used when merging two values.

  // The default operation is to permute bytes with the default control.
  return SimdPermuteOp::PERMUTE_8x16;
}

// Can we shift the bytes left or right by a constant?  A shift is a run of
// lanes from the rhs (which is zero) on one end and a run of values from the
// lhs on the other end.
static Maybe<SimdPermuteOp> TryShift8x16(SimdConstant* control) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();

  // Represent all zero lanes by 16
  SimdConstant::I8x16 zeroesMasked;
  MapLanes(zeroesMasked, lanes, [](int x) -> int { return x >= 16 ? 16 : x; });

  int i = ScanConstant(zeroesMasked, 16, 0);
  int shiftLeft = i;
  if (shiftLeft > 0 && lanes[shiftLeft] != 0) {
    return Nothing();
  }

  i = ScanIncreasingUnmasked(zeroesMasked, i);
  int shiftRight = 16 - i;
  if (shiftRight > 0 && lanes[i - 1] != 15) {
    return Nothing();
  }

  i = ScanConstant(zeroesMasked, 16, i);
  if (i < 16 || (shiftRight > 0 && shiftLeft > 0) ||
      (shiftRight == 0 && shiftLeft == 0)) {
    return Nothing();
  }

  if (shiftRight) {
    *control = SimdConstant::SplatX16((int8_t)shiftRight);
    return Some(SimdPermuteOp::SHIFT_RIGHT_8x16);
  }
  *control = SimdConstant::SplatX16((int8_t)shiftLeft);
  return Some(SimdPermuteOp::SHIFT_LEFT_8x16);
}

// Check if it is unsigned integer extend operation.
static Maybe<SimdPermuteOp> TryZeroExtend(SimdConstant* control) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();

  // Find fragment of sequantial lanes indices that starts from 0.
  uint32_t i = 0;
  for (; i <= 4 && lanes[i] == int8_t(i); i++) {
  }
  // The length of the fragment has to be a power of 2, and next item is zero.
  if (!mozilla::IsPowerOfTwo(i) || lanes[i] < 16) {
    return Nothing();
  }
  MOZ_ASSERT(i > 0 && i <= 4);
  uint32_t fromLen = i;
  // Skip items that will be zero'ed.
  for (; i <= 8 && lanes[i] >= 16; i++) {
  }
  // The length of the entire fragment of zero and non-zero items
  // needs to be power of 2.
  if (!mozilla::IsPowerOfTwo(i)) {
    return Nothing();
  }
  MOZ_ASSERT(i > fromLen && i <= 8);
  uint32_t toLen = i;

  // The sequence will repeat every toLen elements: in which first
  // fromLen items are sequential lane indices, and the rest are zeros.
  int8_t current = int8_t(fromLen);
  for (; i < 16; i++) {
    if ((i % toLen) >= fromLen) {
      // Expect the item be a zero.
      if (lanes[i] < 16) {
        return Nothing();
      }
    } else {
      // Check the item is in ascending sequence.
      if (lanes[i] != current) {
        return Nothing();
      }
      current++;
    }
  }

  switch (fromLen) {
    case 1:
      switch (toLen) {
        case 2:
          return Some(SimdPermuteOp::ZERO_EXTEND_8x16_TO_16x8);
        case 4:
          return Some(SimdPermuteOp::ZERO_EXTEND_8x16_TO_32x4);
        case 8:
          return Some(SimdPermuteOp::ZERO_EXTEND_8x16_TO_64x2);
      }
      break;
    case 2:
      switch (toLen) {
        case 4:
          return Some(SimdPermuteOp::ZERO_EXTEND_16x8_TO_32x4);
        case 8:
          return Some(SimdPermuteOp::ZERO_EXTEND_16x8_TO_64x2);
      }
      break;
    case 4:
      switch (toLen) {
        case 8:
          return Some(SimdPermuteOp::ZERO_EXTEND_32x4_TO_64x2);
      }
      break;
  }
  MOZ_CRASH("Invalid TryZeroExtend match");
}

static Maybe<SimdPermuteOp> AnalyzeShuffleWithZero(SimdConstant* control) {
  Maybe<SimdPermuteOp> op;
  op = TryShift8x16(control);
  if (op) {
    return op;
  }

  op = TryZeroExtend(control);
  if (op) {
    return op;
  }

  // TODO: Optimization opportunity? A byte-blend-with-zero is just a CONST;
  // PAND.  This may beat the general byte blend code below.
  return Nothing();
}

// Concat: if the result is the suffix (high bytes) of the rhs in front of a
// prefix (low bytes) of the lhs then this is PALIGNR; ditto if the operands are
// swapped.
static Maybe<SimdShuffleOp> TryConcatRightShift8x16(SimdConstant* control,
                                                    bool* swapOperands) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();
  int i = ScanIncreasingUnmasked(lanes, 0);
  MOZ_ASSERT(i < 16, "Single-operand run should have been handled elswhere");
  // First run must end with 15 % 16
  if ((lanes[i - 1] & 15) != 15) {
    return Nothing();
  }
  // Second run must start with 0 % 16
  if ((lanes[i] & 15) != 0) {
    return Nothing();
  }
  // The two runs must come from different inputs
  if ((lanes[i] & 16) == (lanes[i - 1] & 16)) {
    return Nothing();
  }
  int suffixLength = i;

  i = ScanIncreasingUnmasked(lanes, i);
  // Must end at the left end
  if (i != 16) {
    return Nothing();
  }

  // If the suffix is from the lhs then swap the operands
  if (lanes[0] < 16) {
    *swapOperands = !*swapOperands;
  }
  *control = SimdConstant::SplatX16((int8_t)suffixLength);
  return Some(SimdShuffleOp::CONCAT_RIGHT_SHIFT_8x16);
}

// Blend words: if we pick words from both operands without a pattern but all
// the input words stay in their position then this is PBLENDW (immediate mask);
// this also handles all larger sizes on x64.
static Maybe<SimdShuffleOp> TryBlendInt16x8(SimdConstant* control) {
  SimdConstant tmp(*control);
  if (!ByteMaskToWordMask(&tmp)) {
    return Nothing();
  }
  SimdConstant::I16x8 masked;
  MaskLanes(masked, tmp.asInt16x8());
  if (!IsIdentity(masked)) {
    return Nothing();
  }
  SimdConstant::I16x8 mapped;
  MapLanes(mapped, tmp.asInt16x8(),
           [](int x) -> int { return x < 8 ? 0 : -1; });
  *control = SimdConstant::CreateX8(mapped);
  return Some(SimdShuffleOp::BLEND_16x8);
}

// Blend bytes: if we pick bytes ditto then this is a byte blend, which can be
// handled with a CONST, PAND, PANDNOT, and POR.
//
// TODO: Optimization opportunity? If we pick all but one lanes from one with at
// most one from the other then it could be a MOV + PEXRB + PINSRB (also if this
// element is not in its source location).
static Maybe<SimdShuffleOp> TryBlendInt8x16(SimdConstant* control) {
  SimdConstant::I8x16 masked;
  MaskLanes(masked, control->asInt8x16());
  if (!IsIdentity(masked)) {
    return Nothing();
  }
  SimdConstant::I8x16 mapped;
  MapLanes(mapped, control->asInt8x16(),
           [](int x) -> int { return x < 16 ? 0 : -1; });
  *control = SimdConstant::CreateX16(mapped);
  return Some(SimdShuffleOp::BLEND_8x16);
}

template <typename T>
static bool MatchInterleave(const T* lanes, int lhs, int rhs, int len) {
  for (int i = 0; i < len; i++) {
    if (lanes[i * 2] != lhs + i || lanes[i * 2 + 1] != rhs + i) {
      return false;
    }
  }
  return true;
}

// Unpack/interleave:
//  - if we interleave the low (bytes/words/doublewords) of the inputs into
//    the output then this is UNPCKL*W (possibly with a swap of operands).
//  - if we interleave the high ditto then it is UNPCKH*W (ditto)
template <typename T>
static Maybe<SimdShuffleOp> TryInterleave(const T* lanes, int lhs, int rhs,
                                          bool* swapOperands,
                                          SimdShuffleOp lowOp,
                                          SimdShuffleOp highOp) {
  int len = int(32 / (sizeof(T) * 4));
  if (MatchInterleave(lanes, lhs, rhs, len)) {
    return Some(lowOp);
  }
  if (MatchInterleave(lanes, rhs, lhs, len)) {
    *swapOperands = !*swapOperands;
    return Some(lowOp);
  }
  if (MatchInterleave(lanes, lhs + len, rhs + len, len)) {
    return Some(highOp);
  }
  if (MatchInterleave(lanes, rhs + len, lhs + len, len)) {
    *swapOperands = !*swapOperands;
    return Some(highOp);
  }
  return Nothing();
}

static Maybe<SimdShuffleOp> TryInterleave64x2(SimdConstant* control,
                                              bool* swapOperands) {
  SimdConstant tmp = *control;
  if (!ByteMaskToQWordMask(&tmp)) {
    return Nothing();
  }
  const SimdConstant::I64x2& lanes = tmp.asInt64x2();
  return TryInterleave(lanes, 0, 2, swapOperands,
                       SimdShuffleOp::INTERLEAVE_LOW_64x2,
                       SimdShuffleOp::INTERLEAVE_HIGH_64x2);
}

static Maybe<SimdShuffleOp> TryInterleave32x4(SimdConstant* control,
                                              bool* swapOperands) {
  SimdConstant tmp = *control;
  if (!ByteMaskToDWordMask(&tmp)) {
    return Nothing();
  }
  const SimdConstant::I32x4& lanes = tmp.asInt32x4();
  return TryInterleave(lanes, 0, 4, swapOperands,
                       SimdShuffleOp::INTERLEAVE_LOW_32x4,
                       SimdShuffleOp::INTERLEAVE_HIGH_32x4);
}

static Maybe<SimdShuffleOp> TryInterleave16x8(SimdConstant* control,
                                              bool* swapOperands) {
  SimdConstant tmp = *control;
  if (!ByteMaskToWordMask(&tmp)) {
    return Nothing();
  }
  const SimdConstant::I16x8& lanes = tmp.asInt16x8();
  return TryInterleave(lanes, 0, 8, swapOperands,
                       SimdShuffleOp::INTERLEAVE_LOW_16x8,
                       SimdShuffleOp::INTERLEAVE_HIGH_16x8);
}

static Maybe<SimdShuffleOp> TryInterleave8x16(SimdConstant* control,
                                              bool* swapOperands) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();
  return TryInterleave(lanes, 0, 16, swapOperands,
                       SimdShuffleOp::INTERLEAVE_LOW_8x16,
                       SimdShuffleOp::INTERLEAVE_HIGH_8x16);
}

static SimdShuffleOp AnalyzeTwoArgShuffle(SimdConstant* control,
                                          bool* swapOperands) {
  Maybe<SimdShuffleOp> op;
  op = TryConcatRightShift8x16(control, swapOperands);
  if (!op) {
    op = TryBlendInt16x8(control);
  }
  if (!op) {
    op = TryBlendInt8x16(control);
  }
  if (!op) {
    op = TryInterleave64x2(control, swapOperands);
  }
  if (!op) {
    op = TryInterleave32x4(control, swapOperands);
  }
  if (!op) {
    op = TryInterleave16x8(control, swapOperands);
  }
  if (!op) {
    op = TryInterleave8x16(control, swapOperands);
  }
  if (!op) {
    op = Some(SimdShuffleOp::SHUFFLE_BLEND_8x16);
  }
  return *op;
}

// Reorder the operands if that seems useful, notably, move a constant to the
// right hand side.  Rewrites the control to account for any move.
static bool MaybeReorderShuffleOperands(MDefinition** lhs, MDefinition** rhs,
                                        SimdConstant* control) {
  if ((*lhs)->isWasmFloatConstant()) {
    MDefinition* tmp = *lhs;
    *lhs = *rhs;
    *rhs = tmp;

    int8_t controlBytes[16];
    const SimdConstant::I8x16& lanes = control->asInt8x16();
    for (unsigned i = 0; i < 16; i++) {
      controlBytes[i] = int8_t(lanes[i] ^ 16);
    }
    *control = SimdConstant::CreateX16(controlBytes);

    return true;
  }
  return false;
}

#  ifdef DEBUG
static const SimdShuffle& ReportShuffleSpecialization(const SimdShuffle& s) {
  switch (s.opd) {
    case SimdShuffle::Operand::BOTH:
    case SimdShuffle::Operand::BOTH_SWAPPED:
      switch (*s.shuffleOp) {
        case SimdShuffleOp::SHUFFLE_BLEND_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> shuffle+blend 8x16");
          break;
        case SimdShuffleOp::BLEND_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> blend 8x16");
          break;
        case SimdShuffleOp::BLEND_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> blend 16x8");
          break;
        case SimdShuffleOp::CONCAT_RIGHT_SHIFT_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> concat+shift-right 8x16");
          break;
        case SimdShuffleOp::INTERLEAVE_HIGH_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-high 8x16");
          break;
        case SimdShuffleOp::INTERLEAVE_HIGH_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-high 16x8");
          break;
        case SimdShuffleOp::INTERLEAVE_HIGH_32x4:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-high 32x4");
          break;
        case SimdShuffleOp::INTERLEAVE_HIGH_64x2:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-high 64x2");
          break;
        case SimdShuffleOp::INTERLEAVE_LOW_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-low 8x16");
          break;
        case SimdShuffleOp::INTERLEAVE_LOW_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-low 16x8");
          break;
        case SimdShuffleOp::INTERLEAVE_LOW_32x4:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-low 32x4");
          break;
        case SimdShuffleOp::INTERLEAVE_LOW_64x2:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-low 64x2");
          break;
        default:
          MOZ_CRASH("Unexpected shuffle op");
      }
      break;
    case SimdShuffle::Operand::LEFT:
    case SimdShuffle::Operand::RIGHT:
      switch (*s.permuteOp) {
        case SimdPermuteOp::BROADCAST_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> broadcast 8x16");
          break;
        case SimdPermuteOp::BROADCAST_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> broadcast 16x8");
          break;
        case SimdPermuteOp::MOVE:
          js::wasm::ReportSimdAnalysis("shuffle -> move");
          break;
        case SimdPermuteOp::REVERSE_16x8:
          js::wasm::ReportSimdAnalysis(
              "shuffle -> reverse bytes in 16-bit lanes");
          break;
        case SimdPermuteOp::REVERSE_32x4:
          js::wasm::ReportSimdAnalysis(
              "shuffle -> reverse bytes in 32-bit lanes");
          break;
        case SimdPermuteOp::REVERSE_64x2:
          js::wasm::ReportSimdAnalysis(
              "shuffle -> reverse bytes in 64-bit lanes");
          break;
        case SimdPermuteOp::PERMUTE_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> permute 8x16");
          break;
        case SimdPermuteOp::PERMUTE_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> permute 16x8");
          break;
        case SimdPermuteOp::PERMUTE_32x4:
          js::wasm::ReportSimdAnalysis("shuffle -> permute 32x4");
          break;
        case SimdPermuteOp::ROTATE_RIGHT_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> rotate-right 8x16");
          break;
        case SimdPermuteOp::SHIFT_LEFT_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> shift-left 8x16");
          break;
        case SimdPermuteOp::SHIFT_RIGHT_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> shift-right 8x16");
          break;
        case SimdPermuteOp::ZERO_EXTEND_8x16_TO_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> zero-extend 8x16 to 16x8");
          break;
        case SimdPermuteOp::ZERO_EXTEND_8x16_TO_32x4:
          js::wasm::ReportSimdAnalysis("shuffle -> zero-extend 8x16 to 32x4");
          break;
        case SimdPermuteOp::ZERO_EXTEND_8x16_TO_64x2:
          js::wasm::ReportSimdAnalysis("shuffle -> zero-extend 8x16 to 64x2");
          break;
        case SimdPermuteOp::ZERO_EXTEND_16x8_TO_32x4:
          js::wasm::ReportSimdAnalysis("shuffle -> zero-extend 16x8 to 32x4");
          break;
        case SimdPermuteOp::ZERO_EXTEND_16x8_TO_64x2:
          js::wasm::ReportSimdAnalysis("shuffle -> zero-extend 16x8 to 64x2");
          break;
        case SimdPermuteOp::ZERO_EXTEND_32x4_TO_64x2:
          js::wasm::ReportSimdAnalysis("shuffle -> zero-extend 32x4 to 64x2");
          break;
        default:
          MOZ_CRASH("Unexpected permute op");
      }
      break;
  }
  return s;
}
#  endif  // DEBUG

SimdShuffle jit::AnalyzeSimdShuffle(SimdConstant control, MDefinition* lhs,
                                    MDefinition* rhs) {
#  ifdef DEBUG
#    define R(s) ReportShuffleSpecialization(s)
#  else
#    define R(s) (s)
#  endif

  // If only one of the inputs is used, determine which.
  bool useLeft = true;
  bool useRight = true;
  if (lhs == rhs) {
    useRight = false;
  } else {
    bool allAbove = true;
    bool allBelow = true;
    const SimdConstant::I8x16& lanes = control.asInt8x16();
    for (int8_t i : lanes) {
      allAbove = allAbove && i >= 16;
      allBelow = allBelow && i < 16;
    }
    if (allAbove) {
      useLeft = false;
    } else if (allBelow) {
      useRight = false;
    }
  }

  // Deal with one-ignored-input.
  if (!(useLeft && useRight)) {
    SimdPermuteOp op = AnalyzePermute(&control);
    return R(SimdShuffle::permute(
        useLeft ? SimdShuffle::Operand::LEFT : SimdShuffle::Operand::RIGHT,
        control, op));
  }

  // Move constants to rhs.
  bool swapOperands = MaybeReorderShuffleOperands(&lhs, &rhs, &control);

  // Deal with constant rhs.
  if (rhs->isWasmFloatConstant()) {
    SimdConstant rhsConstant = rhs->toWasmFloatConstant()->toSimd128();
    if (rhsConstant.isZeroBits()) {
      Maybe<SimdPermuteOp> op = AnalyzeShuffleWithZero(&control);
      if (op) {
        return R(SimdShuffle::permute(swapOperands ? SimdShuffle::Operand::RIGHT
                                                   : SimdShuffle::Operand::LEFT,
                                      control, *op));
      }
    }
  }

  // Two operands both of which are used.  If there's one constant operand it is
  // now on the rhs.
  SimdShuffleOp op = AnalyzeTwoArgShuffle(&control, &swapOperands);
  return R(SimdShuffle::shuffle(swapOperands
                                    ? SimdShuffle::Operand::BOTH_SWAPPED
                                    : SimdShuffle::Operand::BOTH,
                                control, op));
#  undef R
}

#endif  // ENABLE_WASM_SIMD
