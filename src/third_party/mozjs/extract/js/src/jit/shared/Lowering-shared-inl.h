/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_Lowering_shared_inl_h
#define jit_shared_Lowering_shared_inl_h

#include "jit/shared/Lowering-shared.h"

#include "jit/MIR.h"
#include "jit/MIRGenerator.h"

namespace js {
namespace jit {

void LIRGeneratorShared::emitAtUses(MInstruction* mir) {
  MOZ_ASSERT(mir->canEmitAtUses());
  mir->setEmittedAtUses();
  mir->setVirtualRegister(0);
}

LUse LIRGeneratorShared::use(MDefinition* mir, LUse policy) {
  // It is illegal to call use() on an instruction with two defs.
#if BOX_PIECES > 1
  MOZ_ASSERT(mir->type() != MIRType::Value);
#endif
#if INT64_PIECES > 1
  MOZ_ASSERT(mir->type() != MIRType::Int64);
#endif
  ensureDefined(mir);
  policy.setVirtualRegister(mir->virtualRegister());
  return policy;
}

template <size_t X>
void LIRGeneratorShared::define(
    details::LInstructionFixedDefsTempsHelper<1, X>* lir, MDefinition* mir,
    LDefinition::Policy policy) {
  LDefinition::Type type = LDefinition::TypeFrom(mir->type());
  define(lir, mir, LDefinition(type, policy));
}

template <size_t X>
void LIRGeneratorShared::define(
    details::LInstructionFixedDefsTempsHelper<1, X>* lir, MDefinition* mir,
    const LDefinition& def) {
  // Call instructions should use defineReturn.
  MOZ_ASSERT(!lir->isCall());

  uint32_t vreg = getVirtualRegister();

  // Assign the definition and a virtual register. Then, propagate this
  // virtual register to the MIR, so we can map MIR to LIR during lowering.
  lir->setDef(0, def);
  lir->getDef(0)->setVirtualRegister(vreg);
  lir->setMir(mir);
  mir->setVirtualRegister(vreg);
  add(lir);
}

template <size_t X, size_t Y>
void LIRGeneratorShared::defineFixed(LInstructionHelper<1, X, Y>* lir,
                                     MDefinition* mir,
                                     const LAllocation& output) {
  LDefinition::Type type = LDefinition::TypeFrom(mir->type());

  LDefinition def(type, LDefinition::FIXED);
  def.setOutput(output);

  define(lir, mir, def);
}

template <size_t Ops, size_t Temps>
void LIRGeneratorShared::defineInt64Fixed(
    LInstructionHelper<INT64_PIECES, Ops, Temps>* lir, MDefinition* mir,
    const LInt64Allocation& output) {
  uint32_t vreg = getVirtualRegister();

#if JS_BITS_PER_WORD == 64
  LDefinition def(LDefinition::GENERAL, LDefinition::FIXED);
  def.setOutput(output.value());
  lir->setDef(0, def);
  lir->getDef(0)->setVirtualRegister(vreg);
#else
  LDefinition def0(LDefinition::GENERAL, LDefinition::FIXED);
  def0.setOutput(output.low());
  lir->setDef(0, def0);
  lir->getDef(0)->setVirtualRegister(vreg);

  getVirtualRegister();
  LDefinition def1(LDefinition::GENERAL, LDefinition::FIXED);
  def1.setOutput(output.high());
  lir->setDef(1, def1);
  lir->getDef(1)->setVirtualRegister(vreg + 1);
#endif

  lir->setMir(mir);
  mir->setVirtualRegister(vreg);
  add(lir);
}

template <size_t Ops, size_t Temps>
void LIRGeneratorShared::defineReuseInput(
    LInstructionHelper<1, Ops, Temps>* lir, MDefinition* mir,
    uint32_t operand) {
  // Note: Any other operand that is not the same as this operand should be
  // marked as not being "atStart". The regalloc cannot handle those and can
  // overwrite the inputs!

  // The input should be used at the start of the instruction, to avoid moves.
  MOZ_ASSERT(lir->getOperand(operand)->toUse()->usedAtStart());

  LDefinition::Type type = LDefinition::TypeFrom(mir->type());

  LDefinition def(type, LDefinition::MUST_REUSE_INPUT);
  def.setReusedInput(operand);

  define(lir, mir, def);
}

template <size_t Ops, size_t Temps>
void LIRGeneratorShared::defineInt64ReuseInput(
    LInstructionHelper<INT64_PIECES, Ops, Temps>* lir, MDefinition* mir,
    uint32_t operand) {
  // Note: Any other operand that is not the same as this operand should be
  // marked as not being "atStart". The regalloc cannot handle those and can
  // overwrite the inputs!

  // The input should be used at the start of the instruction, to avoid moves.
  MOZ_ASSERT(lir->getOperand(operand)->toUse()->usedAtStart());
#if JS_BITS_PER_WORD == 32
  MOZ_ASSERT(lir->getOperand(operand + 1)->toUse()->usedAtStart());
#endif
  MOZ_ASSERT(!lir->isCall());

  uint32_t vreg = getVirtualRegister();

  LDefinition def1(LDefinition::GENERAL, LDefinition::MUST_REUSE_INPUT);
  def1.setReusedInput(operand);
  lir->setDef(0, def1);
  lir->getDef(0)->setVirtualRegister(vreg);

#if JS_BITS_PER_WORD == 32
  getVirtualRegister();
  LDefinition def2(LDefinition::GENERAL, LDefinition::MUST_REUSE_INPUT);
  def2.setReusedInput(operand + 1);
  lir->setDef(1, def2);
  lir->getDef(1)->setVirtualRegister(vreg + 1);
#endif

  lir->setMir(mir);
  mir->setVirtualRegister(vreg);
  add(lir);
}

template <size_t Ops, size_t Temps>
void LIRGeneratorShared::defineBoxReuseInput(
    LInstructionHelper<BOX_PIECES, Ops, Temps>* lir, MDefinition* mir,
    uint32_t operand) {
  // The input should be used at the start of the instruction, to avoid moves.
  MOZ_ASSERT(lir->getOperand(operand)->toUse()->usedAtStart());
#ifdef JS_NUNBOX32
  MOZ_ASSERT(lir->getOperand(operand + 1)->toUse()->usedAtStart());
#endif
  MOZ_ASSERT(!lir->isCall());
  MOZ_ASSERT(mir->type() == MIRType::Value);

  uint32_t vreg = getVirtualRegister();

#ifdef JS_NUNBOX32
  static_assert(VREG_TYPE_OFFSET == 0,
                "Code below assumes VREG_TYPE_OFFSET == 0");
  static_assert(VREG_DATA_OFFSET == 1,
                "Code below assumes VREG_DATA_OFFSET == 1");

  LDefinition def1(LDefinition::TYPE, LDefinition::MUST_REUSE_INPUT);
  def1.setReusedInput(operand);
  def1.setVirtualRegister(vreg);
  lir->setDef(0, def1);

  getVirtualRegister();
  LDefinition def2(LDefinition::PAYLOAD, LDefinition::MUST_REUSE_INPUT);
  def2.setReusedInput(operand + 1);
  def2.setVirtualRegister(vreg + 1);
  lir->setDef(1, def2);
#else
  LDefinition def(LDefinition::BOX, LDefinition::MUST_REUSE_INPUT);
  def.setReusedInput(operand);
  def.setVirtualRegister(vreg);
  lir->setDef(0, def);
#endif

  lir->setMir(mir);
  mir->setVirtualRegister(vreg);
  add(lir);
}

template <size_t Temps>
void LIRGeneratorShared::defineBox(
    details::LInstructionFixedDefsTempsHelper<BOX_PIECES, Temps>* lir,
    MDefinition* mir, LDefinition::Policy policy) {
  // Call instructions should use defineReturn.
  MOZ_ASSERT(!lir->isCall());
  MOZ_ASSERT(mir->type() == MIRType::Value);

  uint32_t vreg = getVirtualRegister();

#if defined(JS_NUNBOX32)
  lir->setDef(0,
              LDefinition(vreg + VREG_TYPE_OFFSET, LDefinition::TYPE, policy));
  lir->setDef(
      1, LDefinition(vreg + VREG_DATA_OFFSET, LDefinition::PAYLOAD, policy));
  getVirtualRegister();
#elif defined(JS_PUNBOX64)
  lir->setDef(0, LDefinition(vreg, LDefinition::BOX, policy));
#endif
  lir->setMir(mir);

  mir->setVirtualRegister(vreg);
  add(lir);
}

template <size_t Ops, size_t Temps>
void LIRGeneratorShared::defineInt64(
    LInstructionHelper<INT64_PIECES, Ops, Temps>* lir, MDefinition* mir,
    LDefinition::Policy policy) {
  // Call instructions should use defineReturn.
  MOZ_ASSERT(!lir->isCall());

#ifdef JS_64BIT
  MOZ_ASSERT(mir->type() == MIRType::Int64 || mir->type() == MIRType::IntPtr);
#else
  MOZ_ASSERT(mir->type() == MIRType::Int64);
#endif

  uint32_t vreg = getVirtualRegister();

#if JS_BITS_PER_WORD == 32
  lir->setDef(0,
              LDefinition(vreg + INT64LOW_INDEX, LDefinition::GENERAL, policy));
  lir->setDef(
      1, LDefinition(vreg + INT64HIGH_INDEX, LDefinition::GENERAL, policy));
  getVirtualRegister();
#else
  lir->setDef(0, LDefinition(vreg, LDefinition::GENERAL, policy));
#endif
  lir->setMir(mir);

  mir->setVirtualRegister(vreg);
  add(lir);
}

void LIRGeneratorShared::defineReturn(LInstruction* lir, MDefinition* mir) {
  lir->setMir(mir);

  MOZ_ASSERT(lir->isCall());

  uint32_t vreg = getVirtualRegister();

  switch (mir->type()) {
    case MIRType::Value:
#if defined(JS_NUNBOX32)
      lir->setDef(TYPE_INDEX,
                  LDefinition(vreg + VREG_TYPE_OFFSET, LDefinition::TYPE,
                              LGeneralReg(JSReturnReg_Type)));
      lir->setDef(PAYLOAD_INDEX,
                  LDefinition(vreg + VREG_DATA_OFFSET, LDefinition::PAYLOAD,
                              LGeneralReg(JSReturnReg_Data)));
      getVirtualRegister();
#elif defined(JS_PUNBOX64)
      lir->setDef(
          0, LDefinition(vreg, LDefinition::BOX, LGeneralReg(JSReturnReg)));
#endif
      break;
    case MIRType::Int64:
#if defined(JS_NUNBOX32)
      lir->setDef(INT64LOW_INDEX,
                  LDefinition(vreg + INT64LOW_INDEX, LDefinition::GENERAL,
                              LGeneralReg(ReturnReg64.low)));
      lir->setDef(INT64HIGH_INDEX,
                  LDefinition(vreg + INT64HIGH_INDEX, LDefinition::GENERAL,
                              LGeneralReg(ReturnReg64.high)));
      getVirtualRegister();
#elif defined(JS_PUNBOX64)
      lir->setDef(
          0, LDefinition(vreg, LDefinition::GENERAL, LGeneralReg(ReturnReg)));
#endif
      break;
    case MIRType::Float32:
      lir->setDef(0, LDefinition(vreg, LDefinition::FLOAT32,
                                 LFloatReg(ReturnFloat32Reg)));
      break;
    case MIRType::Double:
      lir->setDef(0, LDefinition(vreg, LDefinition::DOUBLE,
                                 LFloatReg(ReturnDoubleReg)));
      break;
    case MIRType::Simd128:
#ifdef ENABLE_WASM_SIMD
      lir->setDef(0, LDefinition(vreg, LDefinition::SIMD128,
                                 LFloatReg(ReturnSimd128Reg)));
      break;
#else
      MOZ_CRASH("No SIMD support");
#endif
    default:
      LDefinition::Type type = LDefinition::TypeFrom(mir->type());
      switch (type) {
        case LDefinition::GENERAL:
        case LDefinition::INT32:
        case LDefinition::OBJECT:
        case LDefinition::SLOTS:
        case LDefinition::STACKRESULTS:
          lir->setDef(0, LDefinition(vreg, type, LGeneralReg(ReturnReg)));
          break;
        case LDefinition::DOUBLE:
        case LDefinition::FLOAT32:
        case LDefinition::SIMD128:
          MOZ_CRASH("Float cases must have been handled earlier");
        default:
          MOZ_CRASH("Unexpected type");
      }
      break;
  }

  mir->setVirtualRegister(vreg);
  add(lir);
}

#ifdef DEBUG
// This function checks that when making redefinitions, we don't accidentally
// coerce two incompatible types.
static inline bool IsCompatibleLIRCoercion(MIRType to, MIRType from) {
  if (to == from) {
    return true;
  }
  // In LIR, we treat boolean and int32 as the same low-level type (INTEGER).
  // When snapshotting, we recover the actual JS type from MIR.
  if ((to == MIRType::Int32 || to == MIRType::Boolean) &&
      (from == MIRType::Int32 || from == MIRType::Boolean)) {
    return true;
  }
  // On 32-bit platforms Int32 can be redefined as IntPtr and vice versa.
  // On 64-bit platforms we can redefine non-negative Int32 values as IntPtr.
  if (from == MIRType::Int32 && to == MIRType::IntPtr) {
    return true;
  }
#  ifndef JS_64BIT
  if (from == MIRType::IntPtr && to == MIRType::Int32) {
    return true;
  }
#  endif
  return false;
}
#endif

void LIRGeneratorShared::redefine(MDefinition* def, MDefinition* as) {
  MOZ_ASSERT(IsCompatibleLIRCoercion(def->type(), as->type()));

  // Try to emit MIR marked as emitted-at-uses at, well, uses. For
  // snapshotting reasons we delay the MIRTypes match, or when we are
  // coercing between bool and int32 constants.
  if (as->isEmittedAtUses() &&
      (def->type() == as->type() ||
       (as->isConstant() &&
        (def->type() == MIRType::Int32 || def->type() == MIRType::Boolean) &&
        (as->type() == MIRType::Int32 || as->type() == MIRType::Boolean)))) {
    MInstruction* replacement;
    if (def->type() != as->type()) {
      if (as->type() == MIRType::Int32) {
        replacement =
            MConstant::New(alloc(), BooleanValue(as->toConstant()->toInt32()));
      } else {
        replacement =
            MConstant::New(alloc(), Int32Value(as->toConstant()->toBoolean()));
      }
      def->block()->insertBefore(def->toInstruction(), replacement);
      emitAtUses(replacement->toInstruction());
    } else {
      replacement = as->toInstruction();
    }
    def->replaceAllUsesWith(replacement);
  } else {
    ensureDefined(as);
    def->setVirtualRegister(as->virtualRegister());
  }
}

void LIRGeneratorShared::ensureDefined(MDefinition* mir) {
  if (mir->isEmittedAtUses()) {
    visitEmittedAtUses(mir->toInstruction());
    MOZ_ASSERT(mir->isLowered());
  }
}

bool LIRGeneratorShared::willHaveDifferentLIRNodes(MDefinition* mir1,
                                                   MDefinition* mir2) {
  if (mir1 != mir2) {
    return true;
  }
  if (mir1->isEmittedAtUses()) {
    return true;
  }
  return false;
}

template <typename LClass, typename... Args>
LClass* LIRGeneratorShared::allocateVariadic(uint32_t numOperands,
                                             Args&&... args) {
  size_t numBytes = sizeof(LClass) + numOperands * sizeof(LAllocation);
  void* buf = alloc().allocate(numBytes);
  if (!buf) {
    return nullptr;
  }

  LClass* ins = static_cast<LClass*>(buf);
  new (ins) LClass(numOperands, std::forward<Args>(args)...);

  ins->initOperandsOffset(sizeof(LClass));

  for (uint32_t i = 0; i < numOperands; i++) {
    ins->setOperand(i, LAllocation());
  }

  return ins;
}

LUse LIRGeneratorShared::useRegister(MDefinition* mir) {
  return use(mir, LUse(LUse::REGISTER));
}

LUse LIRGeneratorShared::useRegisterAtStart(MDefinition* mir) {
  return use(mir, LUse(LUse::REGISTER, true));
}

LUse LIRGeneratorShared::use(MDefinition* mir) {
  return use(mir, LUse(LUse::ANY));
}

LUse LIRGeneratorShared::useAtStart(MDefinition* mir) {
  return use(mir, LUse(LUse::ANY, true));
}

LAllocation LIRGeneratorShared::useOrConstant(MDefinition* mir) {
  if (mir->isConstant()) {
    return LAllocation(mir->toConstant());
  }
  return use(mir);
}

LAllocation LIRGeneratorShared::useOrConstantAtStart(MDefinition* mir) {
  if (mir->isConstant()) {
    return LAllocation(mir->toConstant());
  }
  return useAtStart(mir);
}

LAllocation LIRGeneratorShared::useRegisterOrConstant(MDefinition* mir) {
  if (mir->isConstant()) {
    return LAllocation(mir->toConstant());
  }
  return useRegister(mir);
}

LAllocation LIRGeneratorShared::useRegisterOrConstantAtStart(MDefinition* mir) {
  if (mir->isConstant()) {
    return LAllocation(mir->toConstant());
  }
  return useRegisterAtStart(mir);
}

inline bool CanUseInt32Constant(MDefinition* mir) {
  if (!mir->isConstant()) {
    return false;
  }
  MConstant* cst = mir->toConstant();
  if (cst->type() == MIRType::IntPtr) {
    return INT32_MIN <= cst->toIntPtr() && cst->toIntPtr() <= INT32_MAX;
  }
  MOZ_ASSERT(cst->type() == MIRType::Int32);
  return true;
}

LAllocation LIRGeneratorShared::useRegisterOrInt32Constant(MDefinition* mir) {
  if (CanUseInt32Constant(mir)) {
    return LAllocation(mir->toConstant());
  }
  return useRegister(mir);
}

LAllocation LIRGeneratorShared::useAnyOrInt32Constant(MDefinition* mir) {
  if (CanUseInt32Constant(mir)) {
    return LAllocation(mir->toConstant());
  }
  return useAny(mir);
}

LAllocation LIRGeneratorShared::useRegisterOrZero(MDefinition* mir) {
  if (mir->isConstant() &&
      (mir->toConstant()->isInt32(0) || mir->toConstant()->isInt64(0))) {
    return LAllocation();
  }
  return useRegister(mir);
}

LAllocation LIRGeneratorShared::useRegisterOrZeroAtStart(MDefinition* mir) {
  if (mir->isConstant() &&
      (mir->toConstant()->isInt32(0) || mir->toConstant()->isInt64(0))) {
    return LAllocation();
  }
  return useRegisterAtStart(mir);
}

LAllocation LIRGeneratorShared::useRegisterOrNonDoubleConstant(
    MDefinition* mir) {
  if (mir->isConstant() && mir->type() != MIRType::Double &&
      mir->type() != MIRType::Float32) {
    return LAllocation(mir->toConstant());
  }
  return useRegister(mir);
}

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) ||      \
    defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_RISCV64)
LAllocation LIRGeneratorShared::useAnyOrConstant(MDefinition* mir) {
  return useRegisterOrConstant(mir);
}
LAllocation LIRGeneratorShared::useStorable(MDefinition* mir) {
  return useRegister(mir);
}
LAllocation LIRGeneratorShared::useStorableAtStart(MDefinition* mir) {
  return useRegisterAtStart(mir);
}

LAllocation LIRGeneratorShared::useAny(MDefinition* mir) {
  return useRegister(mir);
}
LAllocation LIRGeneratorShared::useAnyAtStart(MDefinition* mir) {
  return useRegisterAtStart(mir);
}
#else
LAllocation LIRGeneratorShared::useAnyOrConstant(MDefinition* mir) {
  return useOrConstant(mir);
}

LAllocation LIRGeneratorShared::useAny(MDefinition* mir) { return use(mir); }
LAllocation LIRGeneratorShared::useAnyAtStart(MDefinition* mir) {
  return useAtStart(mir);
}
LAllocation LIRGeneratorShared::useStorable(MDefinition* mir) {
  return useRegisterOrConstant(mir);
}
LAllocation LIRGeneratorShared::useStorableAtStart(MDefinition* mir) {
  return useRegisterOrConstantAtStart(mir);
}

#endif

LAllocation LIRGeneratorShared::useKeepalive(MDefinition* mir) {
  return use(mir, LUse(LUse::KEEPALIVE));
}

LAllocation LIRGeneratorShared::useKeepaliveOrConstant(MDefinition* mir) {
  if (mir->isConstant()) {
    return LAllocation(mir->toConstant());
  }
  return useKeepalive(mir);
}

LUse LIRGeneratorShared::useFixed(MDefinition* mir, Register reg) {
  return use(mir, LUse(reg));
}

LUse LIRGeneratorShared::useFixedAtStart(MDefinition* mir, Register reg) {
  return use(mir, LUse(reg, true));
}

LUse LIRGeneratorShared::useFixed(MDefinition* mir, FloatRegister reg) {
  return use(mir, LUse(reg));
}

LUse LIRGeneratorShared::useFixed(MDefinition* mir, AnyRegister reg) {
  return reg.isFloat() ? use(mir, LUse(reg.fpu())) : use(mir, LUse(reg.gpr()));
}

LUse LIRGeneratorShared::useFixedAtStart(MDefinition* mir, AnyRegister reg) {
  return reg.isFloat() ? use(mir, LUse(reg.fpu(), true))
                       : use(mir, LUse(reg.gpr(), true));
}

LDefinition LIRGeneratorShared::temp(LDefinition::Type type,
                                     LDefinition::Policy policy) {
  return LDefinition(getVirtualRegister(), type, policy);
}

LInt64Definition LIRGeneratorShared::tempInt64(LDefinition::Policy policy) {
#if JS_BITS_PER_WORD == 32
  LDefinition high = temp(LDefinition::GENERAL, policy);
  LDefinition low = temp(LDefinition::GENERAL, policy);
  return LInt64Definition(high, low);
#else
  return LInt64Definition(temp(LDefinition::GENERAL, policy));
#endif
}

LDefinition LIRGeneratorShared::tempFixed(Register reg) {
  LDefinition t = temp(LDefinition::GENERAL);
  t.setOutput(LGeneralReg(reg));
  return t;
}

LInt64Definition LIRGeneratorShared::tempInt64Fixed(Register64 reg) {
#if JS_BITS_PER_WORD == 32
  LDefinition high = temp(LDefinition::GENERAL);
  LDefinition low = temp(LDefinition::GENERAL);
  high.setOutput(LGeneralReg(reg.high));
  low.setOutput(LGeneralReg(reg.low));
  return LInt64Definition(high, low);
#else
  LDefinition t = temp(LDefinition::GENERAL);
  t.setOutput(LGeneralReg(reg.reg));
  return LInt64Definition(t);
#endif
}

LDefinition LIRGeneratorShared::tempFixed(FloatRegister reg) {
  LDefinition t = temp(LDefinition::DOUBLE);
  t.setOutput(LFloatReg(reg));
  return t;
}

LDefinition LIRGeneratorShared::tempFloat32() {
  return temp(LDefinition::FLOAT32);
}

LDefinition LIRGeneratorShared::tempDouble() {
  return temp(LDefinition::DOUBLE);
}

#ifdef ENABLE_WASM_SIMD
LDefinition LIRGeneratorShared::tempSimd128() {
  return temp(LDefinition::SIMD128);
}
#endif

LDefinition LIRGeneratorShared::tempCopy(MDefinition* input,
                                         uint32_t reusedInput) {
  MOZ_ASSERT(input->virtualRegister());
  LDefinition t =
      temp(LDefinition::TypeFrom(input->type()), LDefinition::MUST_REUSE_INPUT);
  t.setReusedInput(reusedInput);
  return t;
}

template <typename T>
void LIRGeneratorShared::annotate(T* ins) {
  ins->setId(lirGraph_.getInstructionId());
}

template <typename T>
void LIRGeneratorShared::add(T* ins, MInstruction* mir) {
  MOZ_ASSERT(!ins->isPhi());
  current->add(ins);
  if (mir) {
    MOZ_ASSERT(current == mir->block()->lir());
    ins->setMir(mir);
  }
  annotate(ins);
  if (ins->isCall()) {
    gen->setNeedsOverrecursedCheck();
    gen->setNeedsStaticStackAlignment();
  }
}

#ifdef JS_NUNBOX32
// Returns the virtual register of a js::Value-defining instruction. This is
// abstracted because MBox is a special value-returning instruction that
// redefines its input payload if its input is not constant. Therefore, it is
// illegal to request a box's payload by adding VREG_DATA_OFFSET to its raw id.
static inline uint32_t VirtualRegisterOfPayload(MDefinition* mir) {
  if (mir->isBox()) {
    MDefinition* inner = mir->toBox()->getOperand(0);
    if (!inner->isConstant() && inner->type() != MIRType::Double &&
        inner->type() != MIRType::Float32) {
      return inner->virtualRegister();
    }
  }
  return mir->virtualRegister() + VREG_DATA_OFFSET;
}

// Note: always call ensureDefined before calling useType/usePayload,
// so that emitted-at-use operands are handled correctly.
LUse LIRGeneratorShared::useType(MDefinition* mir, LUse::Policy policy) {
  MOZ_ASSERT(mir->type() == MIRType::Value);

  return LUse(mir->virtualRegister() + VREG_TYPE_OFFSET, policy);
}

LUse LIRGeneratorShared::usePayload(MDefinition* mir, LUse::Policy policy) {
  MOZ_ASSERT(mir->type() == MIRType::Value);

  return LUse(VirtualRegisterOfPayload(mir), policy);
}

LUse LIRGeneratorShared::usePayloadAtStart(MDefinition* mir,
                                           LUse::Policy policy) {
  MOZ_ASSERT(mir->type() == MIRType::Value);

  return LUse(VirtualRegisterOfPayload(mir), policy, true);
}

LUse LIRGeneratorShared::usePayloadInRegisterAtStart(MDefinition* mir) {
  return usePayloadAtStart(mir, LUse::REGISTER);
}

void LIRGeneratorShared::fillBoxUses(LInstruction* lir, size_t n,
                                     MDefinition* mir) {
  ensureDefined(mir);
  lir->getOperand(n)->toUse()->setVirtualRegister(mir->virtualRegister() +
                                                  VREG_TYPE_OFFSET);
  lir->getOperand(n + 1)->toUse()->setVirtualRegister(
      VirtualRegisterOfPayload(mir));
}
#endif

LUse LIRGeneratorShared::useRegisterForTypedLoad(MDefinition* mir,
                                                 MIRType type) {
  MOZ_ASSERT(type != MIRType::Value && type != MIRType::None);
  MOZ_ASSERT(mir->type() == MIRType::Object || mir->type() == MIRType::Slots);

#ifdef JS_PUNBOX64
  // On x64, masm.loadUnboxedValue emits slightly less efficient code when
  // the input and output use the same register and we're not loading an
  // int32/bool/double, so we just call useRegister in this case.
  if (type != MIRType::Int32 && type != MIRType::Boolean &&
      type != MIRType::Double) {
    return useRegister(mir);
  }
#endif

  return useRegisterAtStart(mir);
}

LBoxAllocation LIRGeneratorShared::useBox(MDefinition* mir, LUse::Policy policy,
                                          bool useAtStart) {
  MOZ_ASSERT(mir->type() == MIRType::Value);

  ensureDefined(mir);

#if defined(JS_NUNBOX32)
  return LBoxAllocation(
      LUse(mir->virtualRegister(), policy, useAtStart),
      LUse(VirtualRegisterOfPayload(mir), policy, useAtStart));
#else
  return LBoxAllocation(LUse(mir->virtualRegister(), policy, useAtStart));
#endif
}

LBoxAllocation LIRGeneratorShared::useBoxOrTyped(MDefinition* mir,
                                                 bool useAtStart) {
  if (mir->type() == MIRType::Value) {
    return useBox(mir, LUse::REGISTER, useAtStart);
  }

#if defined(JS_NUNBOX32)
  return LBoxAllocation(useAtStart ? useRegisterAtStart(mir) : useRegister(mir),
                        LAllocation());
#else
  return LBoxAllocation(useAtStart ? useRegisterAtStart(mir)
                                   : useRegister(mir));
#endif
}

LBoxAllocation LIRGeneratorShared::useBoxOrTypedOrConstant(MDefinition* mir,
                                                           bool useConstant,
                                                           bool useAtStart) {
  if (useConstant && mir->isConstant()) {
#if defined(JS_NUNBOX32)
    return LBoxAllocation(LAllocation(mir->toConstant()), LAllocation());
#else
    return LBoxAllocation(LAllocation(mir->toConstant()));
#endif
  }

  return useBoxOrTyped(mir, useAtStart);
}

LInt64Allocation LIRGeneratorShared::useInt64(MDefinition* mir,
                                              LUse::Policy policy,
                                              bool useAtStart) {
  MOZ_ASSERT(mir->type() == MIRType::Int64);

  ensureDefined(mir);

  uint32_t vreg = mir->virtualRegister();
#if JS_BITS_PER_WORD == 32
  return LInt64Allocation(LUse(vreg + INT64HIGH_INDEX, policy, useAtStart),
                          LUse(vreg + INT64LOW_INDEX, policy, useAtStart));
#else
  return LInt64Allocation(LUse(vreg, policy, useAtStart));
#endif
}

LInt64Allocation LIRGeneratorShared::useInt64Fixed(MDefinition* mir,
                                                   Register64 regs,
                                                   bool useAtStart) {
  MOZ_ASSERT(mir->type() == MIRType::Int64);

  ensureDefined(mir);

  uint32_t vreg = mir->virtualRegister();
#if JS_BITS_PER_WORD == 32
  return LInt64Allocation(LUse(regs.high, vreg + INT64HIGH_INDEX, useAtStart),
                          LUse(regs.low, vreg + INT64LOW_INDEX, useAtStart));
#else
  return LInt64Allocation(LUse(regs.reg, vreg, useAtStart));
#endif
}

LInt64Allocation LIRGeneratorShared::useInt64FixedAtStart(MDefinition* mir,
                                                          Register64 regs) {
  return useInt64Fixed(mir, regs, true);
}

LInt64Allocation LIRGeneratorShared::useInt64(MDefinition* mir,
                                              bool useAtStart) {
  // On 32-bit platforms, always load the value in registers.
#if JS_BITS_PER_WORD == 32
  return useInt64(mir, LUse::REGISTER, useAtStart);
#else
  return useInt64(mir, LUse::ANY, useAtStart);
#endif
}

LInt64Allocation LIRGeneratorShared::useInt64AtStart(MDefinition* mir) {
  return useInt64(mir, /* useAtStart = */ true);
}

LInt64Allocation LIRGeneratorShared::useInt64Register(MDefinition* mir,
                                                      bool useAtStart) {
  return useInt64(mir, LUse::REGISTER, useAtStart);
}

LInt64Allocation LIRGeneratorShared::useInt64OrConstant(MDefinition* mir,
                                                        bool useAtStart) {
  if (mir->isConstant()) {
#if defined(JS_NUNBOX32)
    return LInt64Allocation(LAllocation(mir->toConstant()), LAllocation());
#else
    return LInt64Allocation(LAllocation(mir->toConstant()));
#endif
  }
  return useInt64(mir, useAtStart);
}

LInt64Allocation LIRGeneratorShared::useInt64RegisterOrConstant(
    MDefinition* mir, bool useAtStart) {
  if (mir->isConstant()) {
#if defined(JS_NUNBOX32)
    return LInt64Allocation(LAllocation(mir->toConstant()), LAllocation());
#else
    return LInt64Allocation(LAllocation(mir->toConstant()));
#endif
  }
  return useInt64Register(mir, useAtStart);
}

LInt64Allocation LIRGeneratorShared::useInt64RegisterAtStart(MDefinition* mir) {
  return useInt64Register(mir, /* useAtStart = */ true);
}

LInt64Allocation LIRGeneratorShared::useInt64RegisterOrConstantAtStart(
    MDefinition* mir) {
  return useInt64RegisterOrConstant(mir, /* useAtStart = */ true);
}

LInt64Allocation LIRGeneratorShared::useInt64OrConstantAtStart(
    MDefinition* mir) {
  return useInt64OrConstant(mir, /* useAtStart = */ true);
}

void LIRGeneratorShared::lowerConstantDouble(double d, MInstruction* mir) {
  define(new (alloc()) LDouble(d), mir);
}
void LIRGeneratorShared::lowerConstantFloat32(float f, MInstruction* mir) {
  define(new (alloc()) LFloat32(f), mir);
}

}  // namespace jit
}  // namespace js

#endif /* jit_shared_Lowering_shared_inl_h */
