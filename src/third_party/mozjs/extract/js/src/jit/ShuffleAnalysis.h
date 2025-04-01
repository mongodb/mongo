/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ShuffleAnalysis_h
#define jit_ShuffleAnalysis_h

#include "jit/IonTypes.h"

namespace js {
namespace jit {

class MDefinition;

// Permutation operations.  NOTE: these may still be x86-centric, but the set
// can accomodate operations from other architectures.
//
// The "low-order" byte is in lane 0 of an 8x16 datum, the "high-order" byte
// in lane 15.  The low-order byte is also the "rightmost".  In wasm, the
// constant (v128.const i8x16 0 1 2 ... 15) has 0 in the low-order byte and 15
// in the high-order byte.
enum class SimdPermuteOp {
  // A single byte lane is copied into all the other byte lanes.  control_[0]
  // has the source lane.
  BROADCAST_8x16,

  // A single word lane is copied into all the other word lanes.  control_[0]
  // has the source lane.
  BROADCAST_16x8,

  // Copy input to output.
  MOVE,

  // control_ has bytes in range 0..15 s.t. control_[i] holds the source lane
  // for output lane i.
  PERMUTE_8x16,

  // control_ has int16s in range 0..7, as for 8x16.  In addition, the high
  // byte of control_[0] has flags detailing the operation, values taken
  // from the Perm16x8Action enum below.
  PERMUTE_16x8,

  // control_ has int32s in range 0..3, as for 8x16.
  PERMUTE_32x4,

  // control_[0] has the number of places to rotate by.
  ROTATE_RIGHT_8x16,

  // Zeroes are shifted into high-order bytes and low-order bytes are lost.
  // control_[0] has the number of places to shift by.
  SHIFT_RIGHT_8x16,

  // Zeroes are shifted into low-order bytes and high-order bytes are lost.
  // control_[0] has the number of places to shift by.
  SHIFT_LEFT_8x16,

  // Reverse bytes of 16-bit lanes.
  REVERSE_16x8,

  // Reverse bytes of 32-bit lanes.
  REVERSE_32x4,

  // Reverse bytes of 64-bit lanes.
  REVERSE_64x2,

  // Zero extends.
  ZERO_EXTEND_8x16_TO_16x8,
  ZERO_EXTEND_8x16_TO_32x4,
  ZERO_EXTEND_8x16_TO_64x2,
  ZERO_EXTEND_16x8_TO_32x4,
  ZERO_EXTEND_16x8_TO_64x2,
  ZERO_EXTEND_32x4_TO_64x2,
};

// Shuffle operations.  NOTE: these may still be x86-centric, but the set can
// accomodate operations from other architectures.
enum class SimdShuffleOp {
  // Blend bytes.  control_ has the blend mask as an I8x16: 0 to select from
  // the lhs, -1 to select from the rhs.
  BLEND_8x16,

  // Blend words.  control_ has the blend mask as an I16x8: 0 to select from
  // the lhs, -1 to select from the rhs.
  BLEND_16x8,

  // Concat the lhs in front of the rhs and shift right by bytes, extracting
  // the low 16 bytes; control_[0] has the shift count.
  CONCAT_RIGHT_SHIFT_8x16,

  // Interleave qwords/dwords/words/bytes from high/low halves of operands.
  // The low-order item in the result comes from the lhs, then the next from
  // the rhs, and so on.  control_ is ignored.
  INTERLEAVE_HIGH_8x16,
  INTERLEAVE_HIGH_16x8,
  INTERLEAVE_HIGH_32x4,
  INTERLEAVE_HIGH_64x2,
  INTERLEAVE_LOW_8x16,
  INTERLEAVE_LOW_16x8,
  INTERLEAVE_LOW_32x4,
  INTERLEAVE_LOW_64x2,

  // Fully general shuffle+blend.  control_ has the shuffle mask.
  SHUFFLE_BLEND_8x16,
};

// Representation of the result of the shuffle analysis.
struct SimdShuffle {
  enum class Operand {
    // Both inputs, in the original lhs-rhs order
    BOTH,
    // Both inputs, but in rhs-lhs order
    BOTH_SWAPPED,
    // Only the lhs input
    LEFT,
    // Only the rhs input
    RIGHT,
  };

  Operand opd;
  SimdConstant control;
  mozilla::Maybe<SimdPermuteOp> permuteOp;  // Single operands
  mozilla::Maybe<SimdShuffleOp> shuffleOp;  // Double operands

  static SimdShuffle permute(Operand opd, SimdConstant control,
                             SimdPermuteOp op) {
    MOZ_ASSERT(opd == Operand::LEFT || opd == Operand::RIGHT);
    SimdShuffle s{opd, control, mozilla::Some(op), mozilla::Nothing()};
    return s;
  }

  static SimdShuffle shuffle(Operand opd, SimdConstant control,
                             SimdShuffleOp op) {
    MOZ_ASSERT(opd == Operand::BOTH || opd == Operand::BOTH_SWAPPED);
    SimdShuffle s{opd, control, mozilla::Nothing(), mozilla::Some(op)};
    return s;
  }

  bool equals(const SimdShuffle* other) const {
    return permuteOp == other->permuteOp && shuffleOp == other->shuffleOp &&
           opd == other->opd && control.bitwiseEqual(other->control);
  }
};

#ifdef ENABLE_WASM_SIMD

SimdShuffle AnalyzeSimdShuffle(SimdConstant control, MDefinition* lhs,
                               MDefinition* rhs);

#endif

}  // namespace jit
}  // namespace js

#endif  // jit_ShuffleAnalysis_h
