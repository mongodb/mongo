/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/shared/Lowering-shared-inl.h"

#include "jit/LIR.h"
#include "jit/Lowering.h"
#include "jit/MIR.h"
#include "jit/ScalarTypeUtils.h"

#include "vm/SymbolType.h"

using namespace js;
using namespace jit;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

bool LIRGeneratorShared::ShouldReorderCommutative(MDefinition* lhs,
                                                  MDefinition* rhs,
                                                  MInstruction* ins) {
  // lhs and rhs are used by the commutative operator.
  MOZ_ASSERT(lhs->hasDefUses());
  MOZ_ASSERT(rhs->hasDefUses());

  // Ensure that if there is a constant, then it is in rhs.
  if (rhs->isConstant()) {
    return false;
  }
  if (lhs->isConstant()) {
    return true;
  }

  // Since clobbering binary operations clobber the left operand, prefer a
  // non-constant lhs operand with no further uses. To be fully precise, we
  // should check whether this is the *last* use, but checking hasOneDefUse()
  // is a decent approximation which doesn't require any extra analysis.
  bool rhsSingleUse = rhs->hasOneDefUse();
  bool lhsSingleUse = lhs->hasOneDefUse();
  if (rhsSingleUse) {
    if (!lhsSingleUse) {
      return true;
    }
  } else {
    if (lhsSingleUse) {
      return false;
    }
  }

  // If this is a reduction-style computation, such as
  //
  //   sum = 0;
  //   for (...)
  //      sum += ...;
  //
  // put the phi on the left to promote coalescing. This is fairly specific.
  if (rhsSingleUse && rhs->isPhi() && rhs->block()->isLoopHeader() &&
      ins == rhs->toPhi()->getLoopBackedgeOperand()) {
    return true;
  }

  return false;
}

void LIRGeneratorShared::ReorderCommutative(MDefinition** lhsp,
                                            MDefinition** rhsp,
                                            MInstruction* ins) {
  MDefinition* lhs = *lhsp;
  MDefinition* rhs = *rhsp;

  if (ShouldReorderCommutative(lhs, rhs, ins)) {
    *rhsp = lhs;
    *lhsp = rhs;
  }
}

void LIRGeneratorShared::definePhiOneRegister(MPhi* phi, size_t lirIndex) {
  LPhi* lir = current->getPhi(lirIndex);

  uint32_t vreg = getVirtualRegister();

  phi->setVirtualRegister(vreg);
  lir->setDef(0, LDefinition(vreg, LDefinition::TypeFrom(phi->type())));
  annotate(lir);
}

#ifdef JS_NUNBOX32
void LIRGeneratorShared::definePhiTwoRegisters(MPhi* phi, size_t lirIndex) {
  LPhi* type = current->getPhi(lirIndex + VREG_TYPE_OFFSET);
  LPhi* payload = current->getPhi(lirIndex + VREG_DATA_OFFSET);

  uint32_t typeVreg = getVirtualRegister();
  phi->setVirtualRegister(typeVreg);

  uint32_t payloadVreg = getVirtualRegister();
  MOZ_ASSERT(typeVreg + 1 == payloadVreg);

  type->setDef(0, LDefinition(typeVreg, LDefinition::TYPE));
  payload->setDef(0, LDefinition(payloadVreg, LDefinition::PAYLOAD));
  annotate(type);
  annotate(payload);
}
#endif

void LIRGeneratorShared::lowerTypedPhiInput(MPhi* phi, uint32_t inputPosition,
                                            LBlock* block, size_t lirIndex) {
  MDefinition* operand = phi->getOperand(inputPosition);
  LPhi* lir = block->getPhi(lirIndex);
  lir->setOperand(inputPosition, LUse(operand->virtualRegister(), LUse::ANY));
}

LRecoverInfo* LIRGeneratorShared::getRecoverInfo(MResumePoint* rp) {
  if (cachedRecoverInfo_ && cachedRecoverInfo_->mir() == rp) {
    return cachedRecoverInfo_;
  }

  LRecoverInfo* recoverInfo = LRecoverInfo::New(gen, rp);
  if (!recoverInfo) {
    return nullptr;
  }

  cachedRecoverInfo_ = recoverInfo;
  return recoverInfo;
}

#ifdef DEBUG
bool LRecoverInfo::OperandIter::canOptimizeOutIfUnused() {
  MDefinition* ins = **this;

  // We check ins->type() in addition to ins->isUnused() because
  // EliminateDeadResumePointOperands may replace nodes with the constant
  // MagicValue(JS_OPTIMIZED_OUT).
  if ((ins->isUnused() || ins->type() == MIRType::MagicOptimizedOut) &&
      (*it_)->isResumePoint()) {
    return !(*it_)->toResumePoint()->isObservableOperand(op_);
  }

  return true;
}
#endif

LAllocation LIRGeneratorShared::useRegisterOrIndexConstant(
    MDefinition* mir, Scalar::Type type, int32_t offsetAdjustment) {
  if (CanUseInt32Constant(mir)) {
    MConstant* cst = mir->toConstant();
    int32_t val =
        cst->type() == MIRType::Int32 ? cst->toInt32() : cst->toIntPtr();
    int32_t offset;
    if (ArrayOffsetFitsInInt32(val, type, offsetAdjustment, &offset)) {
      return LAllocation(mir->toConstant());
    }
  }
  return useRegister(mir);
}

#ifdef JS_NUNBOX32
LSnapshot* LIRGeneratorShared::buildSnapshot(MResumePoint* rp,
                                             BailoutKind kind) {
  LRecoverInfo* recoverInfo = getRecoverInfo(rp);
  if (!recoverInfo) {
    return nullptr;
  }

  LSnapshot* snapshot = LSnapshot::New(gen, recoverInfo, kind);
  if (!snapshot) {
    return nullptr;
  }

  size_t index = 0;
  for (LRecoverInfo::OperandIter it(recoverInfo); !it; ++it) {
    // Check that optimized out operands are in eliminable slots.
    MOZ_ASSERT(it.canOptimizeOutIfUnused());

    MDefinition* ins = *it;

    if (ins->isRecoveredOnBailout()) {
      continue;
    }

    LAllocation* type = snapshot->typeOfSlot(index);
    LAllocation* payload = snapshot->payloadOfSlot(index);
    ++index;

    if (ins->isBox()) {
      ins = ins->toBox()->getOperand(0);
    }

    // Guards should never be eliminated.
    MOZ_ASSERT_IF(ins->isUnused(), !ins->isGuard());

    // Snapshot operands other than constants should never be
    // emitted-at-uses. Try-catch support depends on there being no
    // code between an instruction and the LOsiPoint that follows it.
    MOZ_ASSERT_IF(!ins->isConstant(), !ins->isEmittedAtUses());

    // The register allocation will fill these fields in with actual
    // register/stack assignments. During code generation, we can restore
    // interpreter state with the given information. Note that for
    // constants, including known types, we record a dummy placeholder,
    // since we can recover the same information, much cleaner, from MIR.
    if (ins->isConstant() || ins->isUnused()) {
      *type = LAllocation();
      *payload = LAllocation();
    } else if (ins->type() != MIRType::Value) {
      *type = LAllocation();
      *payload = use(ins, LUse(LUse::KEEPALIVE));
    } else {
      *type = useType(ins, LUse::KEEPALIVE);
      *payload = usePayload(ins, LUse::KEEPALIVE);
    }
  }

  return snapshot;
}

#elif JS_PUNBOX64

LSnapshot* LIRGeneratorShared::buildSnapshot(MResumePoint* rp,
                                             BailoutKind kind) {
  LRecoverInfo* recoverInfo = getRecoverInfo(rp);
  if (!recoverInfo) {
    return nullptr;
  }

  LSnapshot* snapshot = LSnapshot::New(gen, recoverInfo, kind);
  if (!snapshot) {
    return nullptr;
  }

  size_t index = 0;
  for (LRecoverInfo::OperandIter it(recoverInfo); !it; ++it) {
    // Check that optimized out operands are in eliminable slots.
    MOZ_ASSERT(it.canOptimizeOutIfUnused());

    MDefinition* def = *it;

    if (def->isRecoveredOnBailout()) {
      continue;
    }

    if (def->isBox()) {
      def = def->toBox()->getOperand(0);
    }

    // Guards should never be eliminated.
    MOZ_ASSERT_IF(def->isUnused(), !def->isGuard());

    // Snapshot operands other than constants should never be
    // emitted-at-uses. Try-catch support depends on there being no
    // code between an instruction and the LOsiPoint that follows it.
    MOZ_ASSERT_IF(!def->isConstant(), !def->isEmittedAtUses());

    LAllocation* a = snapshot->getEntry(index++);

    if (def->isUnused()) {
      *a = LAllocation();
      continue;
    }

    *a = useKeepaliveOrConstant(def);
  }

  return snapshot;
}
#endif

void LIRGeneratorShared::assignSnapshot(LInstruction* ins, BailoutKind kind) {
  // assignSnapshot must be called before define/add, since
  // it may add new instructions for emitted-at-use operands.
  MOZ_ASSERT(ins->id() == 0);
  MOZ_ASSERT(kind != BailoutKind::Unknown);

  LSnapshot* snapshot = buildSnapshot(lastResumePoint_, kind);
  if (!snapshot) {
    abort(AbortReason::Alloc, "buildSnapshot failed");
    return;
  }

  ins->assignSnapshot(snapshot);
}

void LIRGeneratorShared::assignSafepoint(LInstruction* ins, MInstruction* mir,
                                         BailoutKind kind) {
  MOZ_ASSERT(!osiPoint_);
  MOZ_ASSERT(!ins->safepoint());

  ins->initSafepoint(alloc());

  MResumePoint* mrp =
      mir->resumePoint() ? mir->resumePoint() : lastResumePoint_;
  LSnapshot* postSnapshot = buildSnapshot(mrp, kind);
  if (!postSnapshot) {
    abort(AbortReason::Alloc, "buildSnapshot failed");
    return;
  }

  osiPoint_ = new (alloc()) LOsiPoint(ins->safepoint(), postSnapshot);

  if (!lirGraph_.noteNeedsSafepoint(ins)) {
    abort(AbortReason::Alloc, "noteNeedsSafepoint failed");
    return;
  }
}

void LIRGeneratorShared::assignWasmSafepoint(LInstruction* ins,
                                             MInstruction* mir) {
  MOZ_ASSERT(!osiPoint_);
  MOZ_ASSERT(!ins->safepoint());

  ins->initSafepoint(alloc());

  if (!lirGraph_.noteNeedsSafepoint(ins)) {
    abort(AbortReason::Alloc, "noteNeedsSafepoint failed");
    return;
  }
}

// Simple shared compare-and-select for all platforms that don't specialize
// further.  See emitWasmCompareAndSelect in CodeGenerator.cpp.
bool LIRGeneratorShared::canSpecializeWasmCompareAndSelect(
    MCompare::CompareType compTy, MIRType insTy) {
  return insTy == MIRType::Int32 && (compTy == MCompare::Compare_Int32 ||
                                     compTy == MCompare::Compare_UInt32);
}

void LIRGeneratorShared::lowerWasmCompareAndSelect(MWasmSelect* ins,
                                                   MDefinition* lhs,
                                                   MDefinition* rhs,
                                                   MCompare::CompareType compTy,
                                                   JSOp jsop) {
  MOZ_ASSERT(canSpecializeWasmCompareAndSelect(compTy, ins->type()));
  auto* lir = new (alloc()) LWasmCompareAndSelect(
      useRegister(lhs), useAny(rhs), compTy, jsop,
      useRegisterAtStart(ins->trueExpr()), useAny(ins->falseExpr()));
  defineReuseInput(lir, ins, LWasmCompareAndSelect::IfTrueExprIndex);
}

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
    controlWords[i / 2] = lanes[i] / 2;
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
  if (ScanConstant(lanes, lanes[0], 0) < 16) {
    return false;
  }
  return true;
}

// Look for permutations of a single operand.
static LWasmPermuteSimd128::Op AnalyzePermute(SimdConstant* control) {
  // Lane indices are input-agnostic for single-operand permutations.
  SimdConstant::I8x16 controlBytes;
  MaskLanes(controlBytes, control->asInt8x16());

  // Get rid of no-ops immediately, so nobody else needs to check.
  if (IsIdentity(controlBytes)) {
    return LWasmPermuteSimd128::MOVE;
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
    return LWasmPermuteSimd128::PERMUTE_32x4;
  }
  if (TryRotateRight8x16(control)) {
    return LWasmPermuteSimd128::ROTATE_RIGHT_8x16;
  }
  if (TryBroadcast16x8(control)) {
    return LWasmPermuteSimd128::BROADCAST_16x8;
  }
  if (TryPermute16x8(control)) {
    return LWasmPermuteSimd128::PERMUTE_16x8;
  }
  if (TryBroadcast8x16(control)) {
    return LWasmPermuteSimd128::BROADCAST_8x16;
  }

  // TODO: (From v8) Unzip and transpose generally have renditions that slightly
  // beat a general permute (three or four instructions)
  //
  // TODO: (From MacroAssemblerX86Shared::ShuffleX4): MOVLHPS and MOVHLPS can be
  // used when merging two values.
  //
  // TODO: Byteswap is MOV + PSLLW + PSRLW + POR, a small win over PSHUFB.

  // The default operation is to permute bytes with the default control.
  return LWasmPermuteSimd128::PERMUTE_8x16;
}

// Can we shift the bytes left or right by a constant?  A shift is a run of
// lanes from the rhs (which is zero) on one end and a run of values from the
// lhs on the other end.
static Maybe<LWasmPermuteSimd128::Op> TryShift8x16(SimdConstant* control) {
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
    *control = SimdConstant::SplatX16(shiftRight);
    return Some(LWasmPermuteSimd128::SHIFT_RIGHT_8x16);
  }
  *control = SimdConstant::SplatX16(shiftLeft);
  return Some(LWasmPermuteSimd128::SHIFT_LEFT_8x16);
}

static Maybe<LWasmPermuteSimd128::Op> AnalyzeShuffleWithZero(
    SimdConstant* control) {
  Maybe<LWasmPermuteSimd128::Op> op;
  op = TryShift8x16(control);
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
static Maybe<LWasmShuffleSimd128::Op> TryConcatRightShift8x16(
    SimdConstant* control, bool* swapOperands) {
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
  *control = SimdConstant::SplatX16(suffixLength);
  return Some(LWasmShuffleSimd128::CONCAT_RIGHT_SHIFT_8x16);
}

// Blend words: if we pick words from both operands without a pattern but all
// the input words stay in their position then this is PBLENDW (immediate mask);
// this also handles all larger sizes on x64.
static Maybe<LWasmShuffleSimd128::Op> TryBlendInt16x8(SimdConstant* control) {
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
  return Some(LWasmShuffleSimd128::BLEND_16x8);
}

// Blend bytes: if we pick bytes ditto then this is a byte blend, which can be
// handled with a CONST, PAND, PANDNOT, and POR.
//
// TODO: Optimization opportunity? If we pick all but one lanes from one with at
// most one from the other then it could be a MOV + PEXRB + PINSRB (also if this
// element is not in its source location).
static Maybe<LWasmShuffleSimd128::Op> TryBlendInt8x16(SimdConstant* control) {
  SimdConstant::I8x16 masked;
  MaskLanes(masked, control->asInt8x16());
  if (!IsIdentity(masked)) {
    return Nothing();
  }
  SimdConstant::I8x16 mapped;
  MapLanes(mapped, control->asInt8x16(),
           [](int x) -> int { return x < 16 ? 0 : -1; });
  *control = SimdConstant::CreateX16(mapped);
  return Some(LWasmShuffleSimd128::BLEND_8x16);
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
static Maybe<LWasmShuffleSimd128::Op> TryInterleave(
    const T* lanes, int lhs, int rhs, bool* swapOperands,
    LWasmShuffleSimd128::Op lowOp, LWasmShuffleSimd128::Op highOp) {
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

static Maybe<LWasmShuffleSimd128::Op> TryInterleave64x2(SimdConstant* control,
                                                        bool* swapOperands) {
  SimdConstant tmp = *control;
  if (!ByteMaskToQWordMask(&tmp)) {
    return Nothing();
  }
  const SimdConstant::I64x2& lanes = tmp.asInt64x2();
  return TryInterleave(lanes, 0, 2, swapOperands,
                       LWasmShuffleSimd128::INTERLEAVE_LOW_64x2,
                       LWasmShuffleSimd128::INTERLEAVE_HIGH_64x2);
}

static Maybe<LWasmShuffleSimd128::Op> TryInterleave32x4(SimdConstant* control,
                                                        bool* swapOperands) {
  SimdConstant tmp = *control;
  if (!ByteMaskToDWordMask(&tmp)) {
    return Nothing();
  }
  const SimdConstant::I32x4& lanes = tmp.asInt32x4();
  return TryInterleave(lanes, 0, 4, swapOperands,
                       LWasmShuffleSimd128::INTERLEAVE_LOW_32x4,
                       LWasmShuffleSimd128::INTERLEAVE_HIGH_32x4);
}

static Maybe<LWasmShuffleSimd128::Op> TryInterleave16x8(SimdConstant* control,
                                                        bool* swapOperands) {
  SimdConstant tmp = *control;
  if (!ByteMaskToWordMask(&tmp)) {
    return Nothing();
  }
  const SimdConstant::I16x8& lanes = tmp.asInt16x8();
  return TryInterleave(lanes, 0, 8, swapOperands,
                       LWasmShuffleSimd128::INTERLEAVE_LOW_16x8,
                       LWasmShuffleSimd128::INTERLEAVE_HIGH_16x8);
}

static Maybe<LWasmShuffleSimd128::Op> TryInterleave8x16(SimdConstant* control,
                                                        bool* swapOperands) {
  const SimdConstant::I8x16& lanes = control->asInt8x16();
  return TryInterleave(lanes, 0, 16, swapOperands,
                       LWasmShuffleSimd128::INTERLEAVE_LOW_8x16,
                       LWasmShuffleSimd128::INTERLEAVE_HIGH_8x16);
}

static LWasmShuffleSimd128::Op AnalyzeTwoArgShuffle(SimdConstant* control,
                                                    bool* swapOperands) {
  Maybe<LWasmShuffleSimd128::Op> op;
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
    op = Some(LWasmShuffleSimd128::SHUFFLE_BLEND_8x16);
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
      controlBytes[i] = lanes[i] ^ 16;
    }
    *control = SimdConstant::CreateX16(controlBytes);

    return true;
  }
  return false;
}

Shuffle LIRGeneratorShared::AnalyzeShuffle(MWasmShuffleSimd128* ins) {
  // Control may be updated, but only once we commit to an operation or when we
  // swap operands.
  SimdConstant control = ins->control();
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  // If only one of the inputs is used, determine which.
  bool useLeft = true;
  bool useRight = true;
  if (lhs == rhs) {
    useRight = false;
  } else {
    bool allAbove = true;
    bool allBelow = true;
    const SimdConstant::I8x16& lanes = control.asInt8x16();
    for (unsigned i = 0; i < 16; i++) {
      allAbove = allAbove && lanes[i] >= 16;
      allBelow = allBelow && lanes[i] < 16;
    }
    if (allAbove) {
      useLeft = false;
    } else if (allBelow) {
      useRight = false;
    }
  }

  // Deal with one-ignored-input.
  if (!(useLeft && useRight)) {
    LWasmPermuteSimd128::Op op = AnalyzePermute(&control);
    return Shuffle::permute(
        useLeft ? Shuffle::Operand::LEFT : Shuffle::Operand::RIGHT, control,
        op);
  }

  // Move constants to rhs.
  bool swapOperands = MaybeReorderShuffleOperands(&lhs, &rhs, &control);

  // Deal with constant rhs.
  if (rhs->isWasmFloatConstant()) {
    SimdConstant rhsConstant = rhs->toWasmFloatConstant()->toSimd128();
    if (rhsConstant.isZeroBits()) {
      Maybe<LWasmPermuteSimd128::Op> op = AnalyzeShuffleWithZero(&control);
      if (op) {
        return Shuffle::permute(
            swapOperands ? Shuffle::Operand::RIGHT : Shuffle::Operand::LEFT,
            control, *op);
      }
    }
  }

  // Two operands both of which are used.  If there's one constant operand it is
  // now on the rhs.
  LWasmShuffleSimd128::Op op = AnalyzeTwoArgShuffle(&control, &swapOperands);
  return Shuffle::shuffle(
      swapOperands ? Shuffle::Operand::BOTH_SWAPPED : Shuffle::Operand::BOTH,
      control, op);
}

#  ifdef DEBUG
void LIRGeneratorShared::ReportShuffleSpecialization(const Shuffle& s) {
  switch (s.opd) {
    case Shuffle::Operand::BOTH:
    case Shuffle::Operand::BOTH_SWAPPED:
      switch (*s.shuffleOp) {
        case LWasmShuffleSimd128::SHUFFLE_BLEND_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> shuffle+blend 8x16");
          break;
        case LWasmShuffleSimd128::BLEND_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> blend 8x16");
          break;
        case LWasmShuffleSimd128::BLEND_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> blend 16x8");
          break;
        case LWasmShuffleSimd128::CONCAT_RIGHT_SHIFT_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> concat+shift-right 8x16");
          break;
        case LWasmShuffleSimd128::INTERLEAVE_HIGH_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-high 8x16");
          break;
        case LWasmShuffleSimd128::INTERLEAVE_HIGH_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-high 16x8");
          break;
        case LWasmShuffleSimd128::INTERLEAVE_HIGH_32x4:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-high 32x4");
          break;
        case LWasmShuffleSimd128::INTERLEAVE_HIGH_64x2:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-high 64x2");
          break;
        case LWasmShuffleSimd128::INTERLEAVE_LOW_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-low 8x16");
          break;
        case LWasmShuffleSimd128::INTERLEAVE_LOW_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-low 16x8");
          break;
        case LWasmShuffleSimd128::INTERLEAVE_LOW_32x4:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-low 32x4");
          break;
        case LWasmShuffleSimd128::INTERLEAVE_LOW_64x2:
          js::wasm::ReportSimdAnalysis("shuffle -> interleave-low 64x2");
          break;
        default:
          MOZ_CRASH("Unexpected shuffle op");
      }
      break;
    case Shuffle::Operand::LEFT:
    case Shuffle::Operand::RIGHT:
      switch (*s.permuteOp) {
        case LWasmPermuteSimd128::BROADCAST_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> broadcast 8x16");
          break;
        case LWasmPermuteSimd128::BROADCAST_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> broadcast 16x8");
          break;
        case LWasmPermuteSimd128::MOVE:
          js::wasm::ReportSimdAnalysis("shuffle -> move");
          break;
        case LWasmPermuteSimd128::PERMUTE_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> permute 8x16");
          break;
        case LWasmPermuteSimd128::PERMUTE_16x8:
          js::wasm::ReportSimdAnalysis("shuffle -> permute 16x8");
          break;
        case LWasmPermuteSimd128::PERMUTE_32x4:
          js::wasm::ReportSimdAnalysis("shuffle -> permute 32x4");
          break;
        case LWasmPermuteSimd128::ROTATE_RIGHT_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> rotate-right 8x16");
          break;
        case LWasmPermuteSimd128::SHIFT_LEFT_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> shift-left 8x16");
          break;
        case LWasmPermuteSimd128::SHIFT_RIGHT_8x16:
          js::wasm::ReportSimdAnalysis("shuffle -> shift-right 8x16");
          break;
        default:
          MOZ_CRASH("Unexpected permute op");
      }
      break;
  }
}
#  endif  // DEBUG

#endif  // ENABLE_WASM_SIMD
