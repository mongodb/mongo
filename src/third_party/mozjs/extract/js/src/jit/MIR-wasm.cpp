/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MIR-wasm.h"

#include "mozilla/ScopeExit.h"

#include "jit/MIRGraph.h"
#include "js/Conversions.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmFeatures.h"  // for wasm::ReportSimdAnalysis

#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::jit;

using JS::ToInt32;

using mozilla::CheckedInt;
using mozilla::IsFloat32Representable;

HashNumber MWasmFloatConstant::valueHash() const {
#ifdef ENABLE_WASM_SIMD
  return ConstantValueHash(type(), u.bits_[0] ^ u.bits_[1]);
#else
  return ConstantValueHash(type(), u.bits_[0]);
#endif
}

bool MWasmFloatConstant::congruentTo(const MDefinition* ins) const {
  return ins->isWasmFloatConstant() && type() == ins->type() &&
#ifdef ENABLE_WASM_SIMD
         u.bits_[1] == ins->toWasmFloatConstant()->u.bits_[1] &&
#endif
         u.bits_[0] == ins->toWasmFloatConstant()->u.bits_[0];
}

HashNumber MWasmNullConstant::valueHash() const {
  return ConstantValueHash(MIRType::WasmAnyRef, 0);
}

MDefinition* MWasmTruncateToInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (input->type() == MIRType::Int32) {
    return input;
  }

  if (input->type() == MIRType::Double && input->isConstant()) {
    double d = input->toConstant()->toDouble();
    if (std::isnan(d)) {
      return this;
    }

    if (!isUnsigned() && d <= double(INT32_MAX) && d >= double(INT32_MIN)) {
      return MConstant::New(alloc, Int32Value(ToInt32(d)));
    }

    if (isUnsigned() && d <= double(UINT32_MAX) && d >= 0) {
      return MConstant::New(alloc, Int32Value(ToInt32(d)));
    }
  }

  if (input->type() == MIRType::Float32 && input->isConstant()) {
    double f = double(input->toConstant()->toFloat32());
    if (std::isnan(f)) {
      return this;
    }

    if (!isUnsigned() && f <= double(INT32_MAX) && f >= double(INT32_MIN)) {
      return MConstant::New(alloc, Int32Value(ToInt32(f)));
    }

    if (isUnsigned() && f <= double(UINT32_MAX) && f >= 0) {
      return MConstant::New(alloc, Int32Value(ToInt32(f)));
    }
  }

  return this;
}

MDefinition* MWasmExtendU32Index::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    return MConstant::NewInt64(
        alloc, int64_t(uint32_t(input->toConstant()->toInt32())));
  }

  return this;
}

MDefinition* MWasmWrapU32Index::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    return MConstant::New(
        alloc, Int32Value(int32_t(uint32_t(input->toConstant()->toInt64()))));
  }

  return this;
}

// Some helpers for folding wasm and/or/xor on int32/64 values.  Rather than
// duplicating these for 32 and 64-bit values, all folding is done on 64-bit
// values and masked for the 32-bit case.

const uint64_t Low32Mask = uint64_t(0xFFFFFFFFULL);

// Routines to check and disassemble values.

static bool IsIntegralConstant(const MDefinition* def) {
  return def->isConstant() &&
         (def->type() == MIRType::Int32 || def->type() == MIRType::Int64);
}

static uint64_t GetIntegralConstant(const MDefinition* def) {
  if (def->type() == MIRType::Int32) {
    return uint64_t(def->toConstant()->toInt32()) & Low32Mask;
  }
  return uint64_t(def->toConstant()->toInt64());
}

static bool IsIntegralConstantZero(const MDefinition* def) {
  return IsIntegralConstant(def) && GetIntegralConstant(def) == 0;
}

static bool IsIntegralConstantOnes(const MDefinition* def) {
  uint64_t ones = def->type() == MIRType::Int32 ? Low32Mask : ~uint64_t(0);
  return IsIntegralConstant(def) && GetIntegralConstant(def) == ones;
}

// Routines to create values.
static MDefinition* ToIntegralConstant(TempAllocator& alloc, MIRType ty,
                                       uint64_t val) {
  switch (ty) {
    case MIRType::Int32:
      return MConstant::New(alloc,
                            Int32Value(int32_t(uint32_t(val & Low32Mask))));
    case MIRType::Int64:
      return MConstant::NewInt64(alloc, int64_t(val));
    default:
      MOZ_CRASH();
  }
}

static MDefinition* ZeroOfType(TempAllocator& alloc, MIRType ty) {
  return ToIntegralConstant(alloc, ty, 0);
}

static MDefinition* OnesOfType(TempAllocator& alloc, MIRType ty) {
  return ToIntegralConstant(alloc, ty, ~uint64_t(0));
}

MDefinition* MWasmBinaryBitwise::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(op() == Opcode::WasmBinaryBitwise);
  MOZ_ASSERT(type() == MIRType::Int32 || type() == MIRType::Int64);

  MDefinition* argL = getOperand(0);
  MDefinition* argR = getOperand(1);
  MOZ_ASSERT(argL->type() == type() && argR->type() == type());

  // The args are the same (SSA name)
  if (argL == argR) {
    switch (subOpcode()) {
      case SubOpcode::And:
      case SubOpcode::Or:
        return argL;
      case SubOpcode::Xor:
        return ZeroOfType(alloc, type());
      default:
        MOZ_CRASH();
    }
  }

  // Both args constant
  if (IsIntegralConstant(argL) && IsIntegralConstant(argR)) {
    uint64_t valL = GetIntegralConstant(argL);
    uint64_t valR = GetIntegralConstant(argR);
    uint64_t val = valL;
    switch (subOpcode()) {
      case SubOpcode::And:
        val &= valR;
        break;
      case SubOpcode::Or:
        val |= valR;
        break;
      case SubOpcode::Xor:
        val ^= valR;
        break;
      default:
        MOZ_CRASH();
    }
    return ToIntegralConstant(alloc, type(), val);
  }

  // Left arg is zero
  if (IsIntegralConstantZero(argL)) {
    switch (subOpcode()) {
      case SubOpcode::And:
        return ZeroOfType(alloc, type());
      case SubOpcode::Or:
      case SubOpcode::Xor:
        return argR;
      default:
        MOZ_CRASH();
    }
  }

  // Right arg is zero
  if (IsIntegralConstantZero(argR)) {
    switch (subOpcode()) {
      case SubOpcode::And:
        return ZeroOfType(alloc, type());
      case SubOpcode::Or:
      case SubOpcode::Xor:
        return argL;
      default:
        MOZ_CRASH();
    }
  }

  // Left arg is ones
  if (IsIntegralConstantOnes(argL)) {
    switch (subOpcode()) {
      case SubOpcode::And:
        return argR;
      case SubOpcode::Or:
        return OnesOfType(alloc, type());
      case SubOpcode::Xor:
        return MBitNot::New(alloc, argR, type());
      default:
        MOZ_CRASH();
    }
  }

  // Right arg is ones
  if (IsIntegralConstantOnes(argR)) {
    switch (subOpcode()) {
      case SubOpcode::And:
        return argL;
      case SubOpcode::Or:
        return OnesOfType(alloc, type());
      case SubOpcode::Xor:
        return MBitNot::New(alloc, argL, type());
      default:
        MOZ_CRASH();
    }
  }

  return this;
}

MDefinition* MWasmAddOffset::foldsTo(TempAllocator& alloc) {
  MDefinition* baseArg = base();
  if (!baseArg->isConstant()) {
    return this;
  }

  if (baseArg->type() == MIRType::Int32) {
    CheckedInt<uint32_t> ptr = baseArg->toConstant()->toInt32();
    ptr += offset();
    if (!ptr.isValid()) {
      return this;
    }
    return MConstant::New(alloc, Int32Value(ptr.value()));
  }

  MOZ_ASSERT(baseArg->type() == MIRType::Int64);
  CheckedInt<uint64_t> ptr = baseArg->toConstant()->toInt64();
  ptr += offset();
  if (!ptr.isValid()) {
    return this;
  }
  return MConstant::NewInt64(alloc, ptr.value());
}

bool MWasmAlignmentCheck::congruentTo(const MDefinition* ins) const {
  if (!ins->isWasmAlignmentCheck()) {
    return false;
  }
  const MWasmAlignmentCheck* check = ins->toWasmAlignmentCheck();
  return byteSize_ == check->byteSize() && congruentIfOperandsEqual(check);
}

MDefinition::AliasType MAsmJSLoadHeap::mightAlias(
    const MDefinition* def) const {
  if (def->isAsmJSStoreHeap()) {
    const MAsmJSStoreHeap* store = def->toAsmJSStoreHeap();
    if (store->accessType() != accessType()) {
      return AliasType::MayAlias;
    }
    if (!base()->isConstant() || !store->base()->isConstant()) {
      return AliasType::MayAlias;
    }
    const MConstant* otherBase = store->base()->toConstant();
    if (base()->toConstant()->equals(otherBase)) {
      return AliasType::MayAlias;
    }
    return AliasType::NoAlias;
  }
  return AliasType::MayAlias;
}

bool MAsmJSLoadHeap::congruentTo(const MDefinition* ins) const {
  if (!ins->isAsmJSLoadHeap()) {
    return false;
  }
  const MAsmJSLoadHeap* load = ins->toAsmJSLoadHeap();
  return load->accessType() == accessType() && congruentIfOperandsEqual(load);
}

MDefinition::AliasType MWasmLoadInstanceDataField::mightAlias(
    const MDefinition* def) const {
  if (def->isWasmStoreInstanceDataField()) {
    const MWasmStoreInstanceDataField* store =
        def->toWasmStoreInstanceDataField();
    return store->instanceDataOffset() == instanceDataOffset_
               ? AliasType::MayAlias
               : AliasType::NoAlias;
  }

  return AliasType::MayAlias;
}

MDefinition::AliasType MWasmLoadGlobalCell::mightAlias(
    const MDefinition* def) const {
  if (def->isWasmStoreGlobalCell()) {
    // No globals of different type can alias.  See bug 1467415 comment 3.
    if (type() != def->toWasmStoreGlobalCell()->value()->type()) {
      return AliasType::NoAlias;
    }

    // We could do better here.  We're dealing with two indirect globals.
    // If at at least one of them is created in this module, then they
    // can't alias -- in other words they can only alias if they are both
    // imported.  That would require having a flag on globals to indicate
    // which are imported.  See bug 1467415 comment 3, 4th rule.
  }

  return AliasType::MayAlias;
}

HashNumber MWasmLoadInstanceDataField::valueHash() const {
  // Same comment as in MWasmLoadInstanceDataField::congruentTo() applies here.
  HashNumber hash = MDefinition::valueHash();
  hash = addU32ToHash(hash, instanceDataOffset_);
  return hash;
}

bool MWasmLoadInstanceDataField::congruentTo(const MDefinition* ins) const {
  if (!ins->isWasmLoadInstanceDataField()) {
    return false;
  }

  const MWasmLoadInstanceDataField* other = ins->toWasmLoadInstanceDataField();

  // We don't need to consider the isConstant_ markings here, because
  // equivalence of offsets implies equivalence of constness.
  bool sameOffsets = instanceDataOffset_ == other->instanceDataOffset_;
  MOZ_ASSERT_IF(sameOffsets, isConstant_ == other->isConstant_);

  // We omit checking congruence of the operands.  There is only one
  // operand, the instance pointer, and it only ever has one value within the
  // domain of optimization.  If that should ever change then operand
  // congruence checking should be reinstated.
  return sameOffsets /* && congruentIfOperandsEqual(other) */;
}

MDefinition* MWasmLoadInstanceDataField::foldsTo(TempAllocator& alloc) {
  if (!dependency() || !dependency()->isWasmStoreInstanceDataField()) {
    return this;
  }

  MWasmStoreInstanceDataField* store =
      dependency()->toWasmStoreInstanceDataField();
  if (!store->block()->dominates(block())) {
    return this;
  }

  if (store->instanceDataOffset() != instanceDataOffset()) {
    return this;
  }

  if (store->value()->type() != type()) {
    return this;
  }

  return store->value();
}

MDefinition* MWasmSelect::foldsTo(TempAllocator& alloc) {
  if (condExpr()->isConstant()) {
    return condExpr()->toConstant()->toInt32() != 0 ? trueExpr() : falseExpr();
  }
  return this;
}

bool MWasmLoadGlobalCell::congruentTo(const MDefinition* ins) const {
  if (!ins->isWasmLoadGlobalCell()) {
    return false;
  }
  const MWasmLoadGlobalCell* other = ins->toWasmLoadGlobalCell();
  return congruentIfOperandsEqual(other);
}

#ifdef ENABLE_WASM_SIMD
MDefinition* MWasmTernarySimd128::foldsTo(TempAllocator& alloc) {
  if (simdOp() == wasm::SimdOp::V128Bitselect) {
    if (v2()->op() == MDefinition::Opcode::WasmFloatConstant) {
      int8_t shuffle[16];
      if (specializeBitselectConstantMaskAsShuffle(shuffle)) {
        return BuildWasmShuffleSimd128(alloc, shuffle, v0(), v1());
      }
    } else if (canRelaxBitselect()) {
      return MWasmTernarySimd128::New(alloc, v0(), v1(), v2(),
                                      wasm::SimdOp::I8x16RelaxedLaneSelect);
    }
  }
  return this;
}

inline static bool MatchSpecificShift(MDefinition* instr,
                                      wasm::SimdOp simdShiftOp,
                                      int shiftValue) {
  return instr->isWasmShiftSimd128() &&
         instr->toWasmShiftSimd128()->simdOp() == simdShiftOp &&
         instr->toWasmShiftSimd128()->rhs()->isConstant() &&
         instr->toWasmShiftSimd128()->rhs()->toConstant()->toInt32() ==
             shiftValue;
}

// Matches MIR subtree that represents PMADDUBSW instruction generated by
// emscripten. The a and b parameters return subtrees that correspond
// operands of the instruction, if match is found.
static bool MatchPmaddubswSequence(MWasmBinarySimd128* lhs,
                                   MWasmBinarySimd128* rhs, MDefinition** a,
                                   MDefinition** b) {
  MOZ_ASSERT(lhs->simdOp() == wasm::SimdOp::I16x8Mul &&
             rhs->simdOp() == wasm::SimdOp::I16x8Mul);
  // The emscripten/LLVM produced the following sequence for _mm_maddubs_epi16:
  //
  //  return _mm_adds_epi16(
  //    _mm_mullo_epi16(
  //      _mm_and_si128(__a, _mm_set1_epi16(0x00FF)),
  //      _mm_srai_epi16(_mm_slli_epi16(__b, 8), 8)),
  //    _mm_mullo_epi16(_mm_srli_epi16(__a, 8), _mm_srai_epi16(__b, 8)));
  //
  //  This will roughly correspond the following MIR:
  //    MWasmBinarySimd128[I16x8AddSatS]
  //      |-- lhs: MWasmBinarySimd128[I16x8Mul]                      (lhs)
  //      |     |-- lhs: MWasmBinarySimd128WithConstant[V128And]     (op0)
  //      |     |     |-- lhs: a
  //      |     |      -- rhs: SimdConstant::SplatX8(0x00FF)
  //      |      -- rhs: MWasmShiftSimd128[I16x8ShrS]                (op1)
  //      |           |-- lhs: MWasmShiftSimd128[I16x8Shl]
  //      |           |     |-- lhs: b
  //      |           |      -- rhs: MConstant[8]
  //      |            -- rhs: MConstant[8]
  //       -- rhs: MWasmBinarySimd128[I16x8Mul]                      (rhs)
  //            |-- lhs: MWasmShiftSimd128[I16x8ShrU]                (op2)
  //            |     |-- lhs: a
  //            |     |-- rhs: MConstant[8]
  //             -- rhs: MWasmShiftSimd128[I16x8ShrS]                (op3)
  //                  |-- lhs: b
  //                   -- rhs: MConstant[8]

  // The I16x8AddSatS and I16x8Mul are commutative, so their operands
  // may be swapped. Rearrange op0, op1, op2, op3 to be in the order
  // noted above.
  MDefinition *op0 = lhs->lhs(), *op1 = lhs->rhs(), *op2 = rhs->lhs(),
              *op3 = rhs->rhs();
  if (op1->isWasmBinarySimd128WithConstant()) {
    // Move MWasmBinarySimd128WithConstant[V128And] as first operand in lhs.
    std::swap(op0, op1);
  } else if (op3->isWasmBinarySimd128WithConstant()) {
    // Move MWasmBinarySimd128WithConstant[V128And] as first operand in rhs.
    std::swap(op2, op3);
  }
  if (op2->isWasmBinarySimd128WithConstant()) {
    // The lhs and rhs are swapped.
    // Make MWasmBinarySimd128WithConstant[V128And] to be op0.
    std::swap(op0, op2);
    std::swap(op1, op3);
  }
  if (op2->isWasmShiftSimd128() &&
      op2->toWasmShiftSimd128()->simdOp() == wasm::SimdOp::I16x8ShrS) {
    // The op2 and op3 appears to be in wrong order, swap.
    std::swap(op2, op3);
  }

  // Check all instructions SIMD code and constant values for assigned
  // names op0, op1, op2, op3 (see diagram above).
  const uint16_t const00FF[8] = {255, 255, 255, 255, 255, 255, 255, 255};
  if (!op0->isWasmBinarySimd128WithConstant() ||
      op0->toWasmBinarySimd128WithConstant()->simdOp() !=
          wasm::SimdOp::V128And ||
      memcmp(op0->toWasmBinarySimd128WithConstant()->rhs().bytes(), const00FF,
             16) != 0 ||
      !MatchSpecificShift(op1, wasm::SimdOp::I16x8ShrS, 8) ||
      !MatchSpecificShift(op2, wasm::SimdOp::I16x8ShrU, 8) ||
      !MatchSpecificShift(op3, wasm::SimdOp::I16x8ShrS, 8) ||
      !MatchSpecificShift(op1->toWasmShiftSimd128()->lhs(),
                          wasm::SimdOp::I16x8Shl, 8)) {
    return false;
  }

  // Check if the instructions arguments that are subtrees match the
  // a and b assignments. May depend on GVN behavior.
  MDefinition* maybeA = op0->toWasmBinarySimd128WithConstant()->lhs();
  MDefinition* maybeB = op3->toWasmShiftSimd128()->lhs();
  if (maybeA != op2->toWasmShiftSimd128()->lhs() ||
      maybeB != op1->toWasmShiftSimd128()->lhs()->toWasmShiftSimd128()->lhs()) {
    return false;
  }

  *a = maybeA;
  *b = maybeB;
  return true;
}

MDefinition* MWasmBinarySimd128::foldsTo(TempAllocator& alloc) {
  if (simdOp() == wasm::SimdOp::I8x16Swizzle && rhs()->isWasmFloatConstant()) {
    // Specialize swizzle(v, constant) as shuffle(mask, v, zero) to trigger all
    // our shuffle optimizations.  We don't report this rewriting as the report
    // will be overwritten by the subsequent shuffle analysis.
    int8_t shuffleMask[16];
    memcpy(shuffleMask, rhs()->toWasmFloatConstant()->toSimd128().bytes(), 16);
    for (int i = 0; i < 16; i++) {
      // Out-of-bounds lanes reference the zero vector; in many cases, the zero
      // vector is removed by subsequent optimizations.
      if (shuffleMask[i] < 0 || shuffleMask[i] > 15) {
        shuffleMask[i] = 16;
      }
    }
    MWasmFloatConstant* zero =
        MWasmFloatConstant::NewSimd128(alloc, SimdConstant::SplatX4(0));
    if (!zero) {
      return nullptr;
    }
    block()->insertBefore(this, zero);
    return BuildWasmShuffleSimd128(alloc, shuffleMask, lhs(), zero);
  }

  // Specialize var OP const / const OP var when possible.
  //
  // As the LIR layer can't directly handle v128 constants as part of its normal
  // machinery we specialize some nodes here if they have single-use v128
  // constant arguments.  The purpose is to generate code that inlines the
  // constant in the instruction stream, using either a rip-relative load+op or
  // quickly-synthesized constant in a scratch on x64.  There is a general
  // assumption here that that is better than generating the constant into an
  // allocatable register, since that register value could not be reused. (This
  // ignores the possibility that the constant load could be hoisted).

  if (lhs()->isWasmFloatConstant() != rhs()->isWasmFloatConstant() &&
      specializeForConstantRhs()) {
    if (isCommutative() && lhs()->isWasmFloatConstant() && lhs()->hasOneUse()) {
      return MWasmBinarySimd128WithConstant::New(
          alloc, rhs(), lhs()->toWasmFloatConstant()->toSimd128(), simdOp());
    }

    if (rhs()->isWasmFloatConstant() && rhs()->hasOneUse()) {
      return MWasmBinarySimd128WithConstant::New(
          alloc, lhs(), rhs()->toWasmFloatConstant()->toSimd128(), simdOp());
    }
  }

  // Check special encoding for PMADDUBSW.
  if (canPmaddubsw() && simdOp() == wasm::SimdOp::I16x8AddSatS &&
      lhs()->isWasmBinarySimd128() && rhs()->isWasmBinarySimd128() &&
      lhs()->toWasmBinarySimd128()->simdOp() == wasm::SimdOp::I16x8Mul &&
      rhs()->toWasmBinarySimd128()->simdOp() == wasm::SimdOp::I16x8Mul) {
    MDefinition *a, *b;
    if (MatchPmaddubswSequence(lhs()->toWasmBinarySimd128(),
                               rhs()->toWasmBinarySimd128(), &a, &b)) {
      return MWasmBinarySimd128::New(alloc, a, b, /* commutative = */ false,
                                     wasm::SimdOp::MozPMADDUBSW);
    }
  }

  return this;
}

MDefinition* MWasmScalarToSimd128::foldsTo(TempAllocator& alloc) {
#  ifdef DEBUG
  auto logging = mozilla::MakeScopeExit([&] {
    js::wasm::ReportSimdAnalysis("scalar-to-simd128 -> constant folded");
  });
#  endif
  if (input()->isConstant()) {
    MConstant* c = input()->toConstant();
    switch (simdOp()) {
      case wasm::SimdOp::I8x16Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX16(c->toInt32()));
      case wasm::SimdOp::I16x8Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX8(c->toInt32()));
      case wasm::SimdOp::I32x4Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX4(c->toInt32()));
      case wasm::SimdOp::I64x2Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX2(c->toInt64()));
      default:
#  ifdef DEBUG
        logging.release();
#  endif
        return this;
    }
  }
  if (input()->isWasmFloatConstant()) {
    MWasmFloatConstant* c = input()->toWasmFloatConstant();
    switch (simdOp()) {
      case wasm::SimdOp::F32x4Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX4(c->toFloat32()));
      case wasm::SimdOp::F64x2Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX2(c->toDouble()));
      default:
#  ifdef DEBUG
        logging.release();
#  endif
        return this;
    }
  }
#  ifdef DEBUG
  logging.release();
#  endif
  return this;
}

template <typename T>
static bool AllTrue(const T& v) {
  constexpr size_t count = sizeof(T) / sizeof(*v);
  static_assert(count == 16 || count == 8 || count == 4 || count == 2);
  bool result = true;
  for (unsigned i = 0; i < count; i++) {
    result = result && v[i] != 0;
  }
  return result;
}

template <typename T>
static int32_t Bitmask(const T& v) {
  constexpr size_t count = sizeof(T) / sizeof(*v);
  constexpr size_t shift = 8 * sizeof(*v) - 1;
  static_assert(shift == 7 || shift == 15 || shift == 31 || shift == 63);
  int32_t result = 0;
  for (unsigned i = 0; i < count; i++) {
    result = result | int32_t(((v[i] >> shift) & 1) << i);
  }
  return result;
}

MDefinition* MWasmReduceSimd128::foldsTo(TempAllocator& alloc) {
#  ifdef DEBUG
  auto logging = mozilla::MakeScopeExit([&] {
    js::wasm::ReportSimdAnalysis("simd128-to-scalar -> constant folded");
  });
#  endif
  if (input()->isWasmFloatConstant()) {
    SimdConstant c = input()->toWasmFloatConstant()->toSimd128();
    int32_t i32Result = 0;
    switch (simdOp()) {
      case wasm::SimdOp::V128AnyTrue:
        i32Result = !c.isZeroBits();
        break;
      case wasm::SimdOp::I8x16AllTrue:
        i32Result = AllTrue(
            SimdConstant::CreateSimd128((int8_t*)c.bytes()).asInt8x16());
        break;
      case wasm::SimdOp::I8x16Bitmask:
        i32Result = Bitmask(
            SimdConstant::CreateSimd128((int8_t*)c.bytes()).asInt8x16());
        break;
      case wasm::SimdOp::I16x8AllTrue:
        i32Result = AllTrue(
            SimdConstant::CreateSimd128((int16_t*)c.bytes()).asInt16x8());
        break;
      case wasm::SimdOp::I16x8Bitmask:
        i32Result = Bitmask(
            SimdConstant::CreateSimd128((int16_t*)c.bytes()).asInt16x8());
        break;
      case wasm::SimdOp::I32x4AllTrue:
        i32Result = AllTrue(
            SimdConstant::CreateSimd128((int32_t*)c.bytes()).asInt32x4());
        break;
      case wasm::SimdOp::I32x4Bitmask:
        i32Result = Bitmask(
            SimdConstant::CreateSimd128((int32_t*)c.bytes()).asInt32x4());
        break;
      case wasm::SimdOp::I64x2AllTrue:
        i32Result = AllTrue(
            SimdConstant::CreateSimd128((int64_t*)c.bytes()).asInt64x2());
        break;
      case wasm::SimdOp::I64x2Bitmask:
        i32Result = Bitmask(
            SimdConstant::CreateSimd128((int64_t*)c.bytes()).asInt64x2());
        break;
      case wasm::SimdOp::I8x16ExtractLaneS:
        i32Result =
            SimdConstant::CreateSimd128((int8_t*)c.bytes()).asInt8x16()[imm()];
        break;
      case wasm::SimdOp::I8x16ExtractLaneU:
        i32Result = int32_t(SimdConstant::CreateSimd128((int8_t*)c.bytes())
                                .asInt8x16()[imm()]) &
                    0xFF;
        break;
      case wasm::SimdOp::I16x8ExtractLaneS:
        i32Result =
            SimdConstant::CreateSimd128((int16_t*)c.bytes()).asInt16x8()[imm()];
        break;
      case wasm::SimdOp::I16x8ExtractLaneU:
        i32Result = int32_t(SimdConstant::CreateSimd128((int16_t*)c.bytes())
                                .asInt16x8()[imm()]) &
                    0xFFFF;
        break;
      case wasm::SimdOp::I32x4ExtractLane:
        i32Result =
            SimdConstant::CreateSimd128((int32_t*)c.bytes()).asInt32x4()[imm()];
        break;
      case wasm::SimdOp::I64x2ExtractLane:
        return MConstant::NewInt64(
            alloc, SimdConstant::CreateSimd128((int64_t*)c.bytes())
                       .asInt64x2()[imm()]);
      case wasm::SimdOp::F32x4ExtractLane:
        return MWasmFloatConstant::NewFloat32(
            alloc, SimdConstant::CreateSimd128((float*)c.bytes())
                       .asFloat32x4()[imm()]);
      case wasm::SimdOp::F64x2ExtractLane:
        return MWasmFloatConstant::NewDouble(
            alloc, SimdConstant::CreateSimd128((double*)c.bytes())
                       .asFloat64x2()[imm()]);
      default:
#  ifdef DEBUG
        logging.release();
#  endif
        return this;
    }
    return MConstant::New(alloc, Int32Value(i32Result), MIRType::Int32);
  }
#  ifdef DEBUG
  logging.release();
#  endif
  return this;
}
#endif  // ENABLE_WASM_SIMD

MDefinition* MWasmUnsignedToDouble::foldsTo(TempAllocator& alloc) {
  if (input()->isConstant()) {
    return MConstant::New(
        alloc, DoubleValue(uint32_t(input()->toConstant()->toInt32())));
  }

  return this;
}

MDefinition* MWasmUnsignedToFloat32::foldsTo(TempAllocator& alloc) {
  if (input()->isConstant()) {
    double dval = double(uint32_t(input()->toConstant()->toInt32()));
    if (IsFloat32Representable(dval)) {
      return MConstant::NewFloat32(alloc, float(dval));
    }
  }

  return this;
}

MWasmCallCatchable* MWasmCallCatchable::New(
    TempAllocator& alloc, const wasm::CallSiteDesc& desc,
    const wasm::CalleeDesc& callee, const Args& args,
    uint32_t stackArgAreaSizeUnaligned, uint32_t tryNoteIndex,
    MBasicBlock* fallthroughBlock, MBasicBlock* prePadBlock,
    MDefinition* tableAddressOrRef) {
  MWasmCallCatchable* call = new (alloc)
      MWasmCallCatchable(desc, callee, stackArgAreaSizeUnaligned, tryNoteIndex);

  call->setSuccessor(FallthroughBranchIndex, fallthroughBlock);
  call->setSuccessor(PrePadBranchIndex, prePadBlock);

  MOZ_ASSERT_IF(callee.isTable() || callee.isFuncRef(), tableAddressOrRef);
  if (!call->initWithArgs(alloc, call, args, tableAddressOrRef)) {
    return nullptr;
  }

  return call;
}

MWasmCallCatchable* MWasmCallCatchable::NewBuiltinInstanceMethodCall(
    TempAllocator& alloc, const wasm::CallSiteDesc& desc,
    const wasm::SymbolicAddress builtin, wasm::FailureMode failureMode,
    const ABIArg& instanceArg, const Args& args,
    uint32_t stackArgAreaSizeUnaligned, uint32_t tryNoteIndex,
    MBasicBlock* fallthroughBlock, MBasicBlock* prePadBlock) {
  auto callee = wasm::CalleeDesc::builtinInstanceMethod(builtin);
  MWasmCallCatchable* call = MWasmCallCatchable::New(
      alloc, desc, callee, args, stackArgAreaSizeUnaligned, tryNoteIndex,
      fallthroughBlock, prePadBlock, nullptr);
  if (!call) {
    return nullptr;
  }

  MOZ_ASSERT(instanceArg != ABIArg());
  call->instanceArg_ = instanceArg;
  call->builtinMethodFailureMode_ = failureMode;
  return call;
}

MWasmCallUncatchable* MWasmCallUncatchable::New(
    TempAllocator& alloc, const wasm::CallSiteDesc& desc,
    const wasm::CalleeDesc& callee, const Args& args,
    uint32_t stackArgAreaSizeUnaligned, MDefinition* tableAddressOrRef) {
  MWasmCallUncatchable* call =
      new (alloc) MWasmCallUncatchable(desc, callee, stackArgAreaSizeUnaligned);

  MOZ_ASSERT_IF(callee.isTable() || callee.isFuncRef(), tableAddressOrRef);
  if (!call->initWithArgs(alloc, call, args, tableAddressOrRef)) {
    return nullptr;
  }

  return call;
}

MWasmCallUncatchable* MWasmCallUncatchable::NewBuiltinInstanceMethodCall(
    TempAllocator& alloc, const wasm::CallSiteDesc& desc,
    const wasm::SymbolicAddress builtin, wasm::FailureMode failureMode,
    const ABIArg& instanceArg, const Args& args,
    uint32_t stackArgAreaSizeUnaligned) {
  auto callee = wasm::CalleeDesc::builtinInstanceMethod(builtin);
  MWasmCallUncatchable* call = MWasmCallUncatchable::New(
      alloc, desc, callee, args, stackArgAreaSizeUnaligned, nullptr);
  if (!call) {
    return nullptr;
  }

  MOZ_ASSERT(instanceArg != ABIArg());
  call->instanceArg_ = instanceArg;
  call->builtinMethodFailureMode_ = failureMode;
  return call;
}

MWasmReturnCall* MWasmReturnCall::New(TempAllocator& alloc,
                                      const wasm::CallSiteDesc& desc,
                                      const wasm::CalleeDesc& callee,
                                      const Args& args,
                                      uint32_t stackArgAreaSizeUnaligned,
                                      MDefinition* tableAddressOrRef) {
  MWasmReturnCall* call =
      new (alloc) MWasmReturnCall(desc, callee, stackArgAreaSizeUnaligned);

  MOZ_ASSERT_IF(callee.isTable() || callee.isFuncRef(), tableAddressOrRef);
  if (!call->initWithArgs(alloc, call, args, tableAddressOrRef)) {
    return nullptr;
  }

  return call;
}

MIonToWasmCall* MIonToWasmCall::New(TempAllocator& alloc,
                                    WasmInstanceObject* instanceObj,
                                    const wasm::FuncExport& funcExport) {
  const wasm::FuncType& funcType =
      instanceObj->instance().codeMeta().getFuncType(funcExport.funcIndex());
  const wasm::ValTypeVector& results = funcType.results();
  MIRType resultType = MIRType::Value;
  // At the JS boundary some wasm types must be represented as a Value, and in
  // addition a void return requires an Undefined value.
  if (results.length() > 0 && !results[0].isEncodedAsJSValueOnEscape()) {
    MOZ_ASSERT(results.length() == 1,
               "multiple returns not implemented for inlined Wasm calls");
    resultType = results[0].toMIRType();
  }

  auto* ins = new (alloc) MIonToWasmCall(instanceObj, resultType, funcExport);
  if (!ins->init(alloc, funcType.args().length())) {
    return nullptr;
  }
  return ins;
}

#ifdef DEBUG
bool MIonToWasmCall::isConsistentFloat32Use(MUse* use) const {
  const wasm::FuncType& funcType =
      instance()->codeMeta().getFuncType(funcExport_.funcIndex());
  return funcType.args()[use->index()].kind() == wasm::ValType::F32;
}
#endif

bool MWasmShiftSimd128::congruentTo(const MDefinition* ins) const {
  if (!ins->isWasmShiftSimd128()) {
    return false;
  }
  return ins->toWasmShiftSimd128()->simdOp() == simdOp_ &&
         congruentIfOperandsEqual(ins);
}

bool MWasmShuffleSimd128::congruentTo(const MDefinition* ins) const {
  if (!ins->isWasmShuffleSimd128()) {
    return false;
  }
  return ins->toWasmShuffleSimd128()->shuffle().equals(&shuffle_) &&
         congruentIfOperandsEqual(ins);
}

bool MWasmUnarySimd128::congruentTo(const MDefinition* ins) const {
  if (!ins->isWasmUnarySimd128()) {
    return false;
  }
  return ins->toWasmUnarySimd128()->simdOp() == simdOp_ &&
         congruentIfOperandsEqual(ins);
}

#ifdef ENABLE_WASM_SIMD
MWasmShuffleSimd128* jit::BuildWasmShuffleSimd128(TempAllocator& alloc,
                                                  const int8_t* control,
                                                  MDefinition* lhs,
                                                  MDefinition* rhs) {
  SimdShuffle s =
      AnalyzeSimdShuffle(SimdConstant::CreateX16(control), lhs, rhs);
  switch (s.opd) {
    case SimdShuffle::Operand::LEFT:
      // When SimdShuffle::Operand is LEFT the right operand is not used,
      // lose reference to rhs.
      rhs = lhs;
      break;
    case SimdShuffle::Operand::RIGHT:
      // When SimdShuffle::Operand is RIGHT the left operand is not used,
      // lose reference to lhs.
      lhs = rhs;
      break;
    default:
      break;
  }
  return MWasmShuffleSimd128::New(alloc, lhs, rhs, s);
}
#endif  // ENABLE_WASM_SIMD

static MDefinition* FoldTrivialWasmTests(TempAllocator& alloc,
                                         wasm::RefType sourceType,
                                         wasm::RefType destType) {
  // Upcasts are trivially valid.
  if (wasm::RefType::isSubTypeOf(sourceType, destType)) {
    return MConstant::New(alloc, Int32Value(1), MIRType::Int32);
  }

  // If two types are completely disjoint, then all casts between them are
  // impossible.
  if (!wasm::RefType::castPossible(destType, sourceType)) {
    return MConstant::New(alloc, Int32Value(0), MIRType::Int32);
  }

  return nullptr;
}

static MDefinition* FoldTrivialWasmCasts(MDefinition* ref,
                                         wasm::RefType sourceType,
                                         wasm::RefType destType) {
  // Upcasts are trivially valid.
  if (wasm::RefType::isSubTypeOf(sourceType, destType)) {
    return ref;
  }

  // We can't fold invalid casts to a trap instruction, because that will
  // confuse GVN which assumes the folded to instruction has the same type
  // as the original instruction.

  return nullptr;
}

MDefinition* MWasmRefTestAbstract::foldsTo(TempAllocator& alloc) {
  if (ref()->wasmRefType().isNothing()) {
    return this;
  }

  MDefinition* folded =
      FoldTrivialWasmTests(alloc, ref()->wasmRefType().value(), destType());
  if (folded) {
    return folded;
  }
  return this;
}

MDefinition* MWasmRefTestConcrete::foldsTo(TempAllocator& alloc) {
  if (ref()->wasmRefType().isNothing()) {
    return this;
  }

  MDefinition* folded =
      FoldTrivialWasmTests(alloc, ref()->wasmRefType().value(), destType());
  if (folded) {
    return folded;
  }
  return this;
}

MDefinition* MWasmRefCastAbstract::foldsTo(TempAllocator& alloc) {
  if (ref()->wasmRefType().isNothing()) {
    return this;
  }

  MDefinition* folded =
      FoldTrivialWasmCasts(ref(), ref()->wasmRefType().value(), destType());
  if (folded) {
    return folded;
  }
  return this;
}

MDefinition* MWasmRefCastConcrete::foldsTo(TempAllocator& alloc) {
  if (ref()->wasmRefType().isNothing()) {
    return this;
  }

  MDefinition* folded =
      FoldTrivialWasmCasts(ref(), ref()->wasmRefType().value(), destType());
  if (folded) {
    return folded;
  }
  return this;
}

MDefinition* MWasmRefAsNonNull::foldsTo(TempAllocator& alloc) {
  wasm::MaybeRefType inputType = ref()->wasmRefType();
  if (inputType.isSome() && !inputType.value().isNullable()) {
    return ref();
  }
  return this;
}

bool MWasmStructState::init() {
  // Reserve the size for the number of fields.
  return fields_.resize(
      wasmStruct_->toWasmNewStructObject()->structType().fields_.length());
}

MWasmStructState* MWasmStructState::New(TempAllocator& alloc,
                                        MDefinition* structObject) {
  MWasmStructState* state = new (alloc) MWasmStructState(alloc, structObject);
  if (!state->init()) {
    return nullptr;
  }
  return state;
}

MWasmStructState* MWasmStructState::Copy(TempAllocator& alloc,
                                         MWasmStructState* state) {
  MDefinition* newWasmStruct = state->wasmStruct();
  MWasmStructState* res = new (alloc) MWasmStructState(alloc, newWasmStruct);
  if (!res || !res->init()) {
    return nullptr;
  }
  for (size_t i = 0; i < state->numFields(); i++) {
    res->setField(i, state->getField(i));
  }
  return res;
}
