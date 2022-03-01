/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/Lowering-x86.h"

#include "jit/Lowering.h"
#include "jit/MIR.h"
#include "jit/x86/Assembler-x86.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

LBoxAllocation LIRGeneratorX86::useBoxFixed(MDefinition* mir, Register reg1,
                                            Register reg2, bool useAtStart) {
  MOZ_ASSERT(mir->type() == MIRType::Value);
  MOZ_ASSERT(reg1 != reg2);

  ensureDefined(mir);
  return LBoxAllocation(LUse(reg1, mir->virtualRegister(), useAtStart),
                        LUse(reg2, VirtualRegisterOfPayload(mir), useAtStart));
}

LAllocation LIRGeneratorX86::useByteOpRegister(MDefinition* mir) {
  return useFixed(mir, eax);
}

LAllocation LIRGeneratorX86::useByteOpRegisterAtStart(MDefinition* mir) {
  return useFixedAtStart(mir, eax);
}

LAllocation LIRGeneratorX86::useByteOpRegisterOrNonDoubleConstant(
    MDefinition* mir) {
  return useFixed(mir, eax);
}

LDefinition LIRGeneratorX86::tempByteOpRegister() { return tempFixed(eax); }

void LIRGenerator::visitBox(MBox* box) {
  MDefinition* inner = box->getOperand(0);

  // If the box wrapped a double, it needs a new register.
  if (IsFloatingPointType(inner->type())) {
    LDefinition spectreTemp =
        JitOptions.spectreValueMasking ? temp() : LDefinition::BogusTemp();
    defineBox(new (alloc()) LBoxFloatingPoint(useRegisterAtStart(inner),
                                              tempCopy(inner, 0), spectreTemp,
                                              inner->type()),
              box);
    return;
  }

  if (box->canEmitAtUses()) {
    emitAtUses(box);
    return;
  }

  if (inner->isConstant()) {
    defineBox(new (alloc()) LValue(inner->toConstant()->toJSValue()), box);
    return;
  }

  LBox* lir = new (alloc()) LBox(use(inner), inner->type());

  // Otherwise, we should not define a new register for the payload portion
  // of the output, so bypass defineBox().
  uint32_t vreg = getVirtualRegister();

  // Note that because we're using BogusTemp(), we do not change the type of
  // the definition. We also do not define the first output as "TYPE",
  // because it has no corresponding payload at (vreg + 1). Also note that
  // although we copy the input's original type for the payload half of the
  // definition, this is only for clarity. BogusTemp() definitions are
  // ignored.
  lir->setDef(0, LDefinition(vreg, LDefinition::GENERAL));
  lir->setDef(1, LDefinition::BogusTemp());
  box->setVirtualRegister(vreg);
  add(lir);
}

void LIRGenerator::visitUnbox(MUnbox* unbox) {
  MDefinition* inner = unbox->getOperand(0);

  // An unbox on x86 reads in a type tag (either in memory or a register) and
  // a payload. Unlike most instructions consuming a box, we ask for the type
  // second, so that the result can re-use the first input.
  MOZ_ASSERT(inner->type() == MIRType::Value);

  ensureDefined(inner);

  if (IsFloatingPointType(unbox->type())) {
    LUnboxFloatingPoint* lir =
        new (alloc()) LUnboxFloatingPoint(useBox(inner), unbox->type());
    if (unbox->fallible()) {
      assignSnapshot(lir, unbox->bailoutKind());
    }
    define(lir, unbox);
    return;
  }

  // Swap the order we use the box pieces so we can re-use the payload register.
  LUnbox* lir = new (alloc()) LUnbox;
  bool reusePayloadReg = !JitOptions.spectreValueMasking ||
                         unbox->type() == MIRType::Int32 ||
                         unbox->type() == MIRType::Boolean;
  if (reusePayloadReg) {
    lir->setOperand(0, usePayloadInRegisterAtStart(inner));
    lir->setOperand(1, useType(inner, LUse::ANY));
  } else {
    lir->setOperand(0, usePayload(inner, LUse::REGISTER));
    lir->setOperand(1, useType(inner, LUse::ANY));
  }

  if (unbox->fallible()) {
    assignSnapshot(lir, unbox->bailoutKind());
  }

  // Types and payloads form two separate intervals. If the type becomes dead
  // before the payload, it could be used as a Value without the type being
  // recoverable. Unbox's purpose is to eagerly kill the definition of a type
  // tag, so keeping both alive (for the purpose of gcmaps) is unappealing.
  // Instead, we create a new virtual register.
  if (reusePayloadReg) {
    defineReuseInput(lir, unbox, 0);
  } else {
    define(lir, unbox);
  }
}

void LIRGenerator::visitReturnImpl(MDefinition* opd, bool isGenerator) {
  MOZ_ASSERT(opd->type() == MIRType::Value);

  LReturn* ins = new (alloc()) LReturn(isGenerator);
  ins->setOperand(0, LUse(JSReturnReg_Type));
  ins->setOperand(1, LUse(JSReturnReg_Data));
  fillBoxUses(ins, 0, opd);
  add(ins);
}

void LIRGeneratorX86::lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition,
                                           LBlock* block, size_t lirIndex) {
  MDefinition* operand = phi->getOperand(inputPosition);
  LPhi* type = block->getPhi(lirIndex + VREG_TYPE_OFFSET);
  LPhi* payload = block->getPhi(lirIndex + VREG_DATA_OFFSET);
  type->setOperand(
      inputPosition,
      LUse(operand->virtualRegister() + VREG_TYPE_OFFSET, LUse::ANY));
  payload->setOperand(inputPosition,
                      LUse(VirtualRegisterOfPayload(operand), LUse::ANY));
}

void LIRGeneratorX86::defineInt64Phi(MPhi* phi, size_t lirIndex) {
  LPhi* low = current->getPhi(lirIndex + INT64LOW_INDEX);
  LPhi* high = current->getPhi(lirIndex + INT64HIGH_INDEX);

  uint32_t lowVreg = getVirtualRegister();

  phi->setVirtualRegister(lowVreg);

  uint32_t highVreg = getVirtualRegister();
  MOZ_ASSERT(lowVreg + INT64HIGH_INDEX == highVreg + INT64LOW_INDEX);

  low->setDef(0, LDefinition(lowVreg, LDefinition::INT32));
  high->setDef(0, LDefinition(highVreg, LDefinition::INT32));
  annotate(high);
  annotate(low);
}

void LIRGeneratorX86::lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition,
                                         LBlock* block, size_t lirIndex) {
  MDefinition* operand = phi->getOperand(inputPosition);
  LPhi* low = block->getPhi(lirIndex + INT64LOW_INDEX);
  LPhi* high = block->getPhi(lirIndex + INT64HIGH_INDEX);
  low->setOperand(inputPosition,
                  LUse(operand->virtualRegister() + INT64LOW_INDEX, LUse::ANY));
  high->setOperand(
      inputPosition,
      LUse(operand->virtualRegister() + INT64HIGH_INDEX, LUse::ANY));
}

void LIRGeneratorX86::lowerForALUInt64(
    LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs) {
  ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));
  ins->setInt64Operand(INT64_PIECES, useInt64OrConstant(rhs));
  defineInt64ReuseInput(ins, mir, 0);
}

void LIRGeneratorX86::lowerForMulInt64(LMulI64* ins, MMul* mir,
                                       MDefinition* lhs, MDefinition* rhs) {
  bool needsTemp = true;

  if (rhs->isConstant()) {
    int64_t constant = rhs->toConstant()->toInt64();
    int32_t shift = mozilla::FloorLog2(constant);
    // See special cases in CodeGeneratorX86Shared::visitMulI64.
    if (constant >= -1 && constant <= 2) {
      needsTemp = false;
    }
    if (constant > 0 && int64_t(1) << shift == constant) {
      needsTemp = false;
    }
  }

  // MulI64 on x86 needs output to be in edx, eax;
  ins->setInt64Operand(
      0, useInt64Fixed(lhs, Register64(edx, eax), /*useAtStart = */ true));
  ins->setInt64Operand(INT64_PIECES, useInt64OrConstant(rhs));
  if (needsTemp) {
    ins->setTemp(0, temp());
  }

  defineInt64Fixed(ins, mir,
                   LInt64Allocation(LAllocation(AnyRegister(edx)),
                                    LAllocation(AnyRegister(eax))));
}

void LIRGenerator::visitCompareExchangeTypedArrayElement(
    MCompareExchangeTypedArrayElement* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  if (Scalar::isBigIntType(ins->arrayType())) {
    LUse elements = useFixed(ins->elements(), esi);
    LAllocation index =
        useRegisterOrIndexConstant(ins->index(), ins->arrayType());
    LUse oldval = useFixed(ins->oldval(), eax);
    LUse newval = useFixed(ins->newval(), edx);
    LDefinition temp = tempFixed(ebx);

    auto* lir = new (alloc()) LCompareExchangeTypedArrayElement64(
        elements, index, oldval, newval, temp);
    defineFixed(lir, ins, LAllocation(AnyRegister(ecx)));
    assignSafepoint(lir, ins);
    return;
  }

  lowerCompareExchangeTypedArrayElement(ins, /* useI386ByteRegisters = */ true);
}

void LIRGenerator::visitAtomicExchangeTypedArrayElement(
    MAtomicExchangeTypedArrayElement* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  if (Scalar::isBigIntType(ins->arrayType())) {
    LUse elements = useRegister(ins->elements());
    LAllocation index =
        useRegisterOrIndexConstant(ins->index(), ins->arrayType());
    LAllocation value = useFixed(ins->value(), edx);
    LInt64Definition temp = tempInt64Fixed(Register64(ecx, ebx));

    auto* lir = new (alloc())
        LAtomicExchangeTypedArrayElement64(elements, index, value, temp);
    defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
    assignSafepoint(lir, ins);
    return;
  }

  lowerAtomicExchangeTypedArrayElement(ins, /*useI386ByteRegisters=*/true);
}

void LIRGenerator::visitAtomicTypedArrayElementBinop(
    MAtomicTypedArrayElementBinop* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  if (Scalar::isBigIntType(ins->arrayType())) {
    LUse elements = useRegister(ins->elements());
    LAllocation index =
        useRegisterOrIndexConstant(ins->index(), ins->arrayType());
    LAllocation value = useFixed(ins->value(), edx);
    LInt64Definition temp = tempInt64Fixed(Register64(ecx, ebx));

    // Case 1: the result of the operation is not used.
    //
    // We can omit allocating the result BigInt.

    if (ins->isForEffect()) {
      LDefinition tempLow = tempFixed(eax);

      auto* lir = new (alloc()) LAtomicTypedArrayElementBinopForEffect64(
          elements, index, value, temp, tempLow);
      add(lir, ins);
      return;
    }

    // Case 2: the result of the operation is used.

    auto* lir = new (alloc())
        LAtomicTypedArrayElementBinop64(elements, index, value, temp);
    defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
    assignSafepoint(lir, ins);
    return;
  }

  lowerAtomicTypedArrayElementBinop(ins, /* useI386ByteRegisters = */ true);
}

void LIRGeneratorX86::lowerAtomicLoad64(MLoadUnboxedScalar* ins) {
  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->storageType());

  auto* lir = new (alloc()) LAtomicLoad64(elements, index, tempFixed(ebx),
                                          tempInt64Fixed(Register64(edx, eax)));
  defineFixed(lir, ins, LAllocation(AnyRegister(ecx)));
  assignSafepoint(lir, ins);
}

void LIRGeneratorX86::lowerAtomicStore64(MStoreUnboxedScalar* ins) {
  LUse elements = useRegister(ins->elements());
  LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->writeType());
  LAllocation value = useFixed(ins->value(), edx);
  LInt64Definition temp1 = tempInt64Fixed(Register64(ecx, ebx));
  LDefinition temp2 = tempFixed(eax);

  add(new (alloc()) LAtomicStore64(elements, index, value, temp1, temp2), ins);
}

void LIRGenerator::visitWasmUnsignedToDouble(MWasmUnsignedToDouble* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::Int32);
  LWasmUint32ToDouble* lir = new (alloc())
      LWasmUint32ToDouble(useRegisterAtStart(ins->input()), temp());
  define(lir, ins);
}

void LIRGenerator::visitWasmUnsignedToFloat32(MWasmUnsignedToFloat32* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::Int32);
  LWasmUint32ToFloat32* lir = new (alloc())
      LWasmUint32ToFloat32(useRegisterAtStart(ins->input()), temp());
  define(lir, ins);
}

// If the base is a constant, and it is zero or its offset is zero, then
// code generation will fold the values into the access.  Allocate the
// pointer to a register only if that can't happen.

static bool OptimizableConstantAccess(MDefinition* base,
                                      const wasm::MemoryAccessDesc& access) {
  MOZ_ASSERT(base->isConstant());
  MOZ_ASSERT(base->type() == MIRType::Int32);

  if (!(base->toConstant()->isInt32(0) || access.offset() == 0)) {
    return false;
  }
  if (access.type() == Scalar::Int64) {
    // For int64 accesses on 32-bit systems we will need to add another offset
    // of 4 to access the high part of the value; make sure this does not
    // overflow the value.
    int32_t v;
    if (base->toConstant()->isInt32(0)) {
      v = access.offset();
    } else {
      v = base->toConstant()->toInt32();
    }
    return v <= int32_t(INT32_MAX - INT64HIGH_OFFSET);
  }
  return true;
}

void LIRGenerator::visitWasmHeapBase(MWasmHeapBase* ins) {
  auto* lir = new (alloc()) LWasmHeapBase(useRegisterAtStart(ins->tlsPtr()));
  define(lir, ins);
}

void LIRGenerator::visitWasmLoad(MWasmLoad* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  MDefinition* memoryBase = ins->memoryBase();
  MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

  if (ins->access().type() == Scalar::Int64 && ins->access().isAtomic()) {
    auto* lir = new (alloc())
        LWasmAtomicLoadI64(useRegister(memoryBase), useRegister(base),
                           tempFixed(ecx), tempFixed(ebx));
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(edx)),
                                      LAllocation(AnyRegister(eax))));
    return;
  }

  LAllocation baseAlloc;
  if (!base->isConstant() || !OptimizableConstantAccess(base, ins->access())) {
    baseAlloc = ins->type() == MIRType::Int64 ? useRegister(base)
                                              : useRegisterAtStart(base);
  }

  if (ins->type() != MIRType::Int64) {
    auto* lir =
        new (alloc()) LWasmLoad(baseAlloc, useRegisterAtStart(memoryBase));
    define(lir, ins);
    return;
  }

  // "AtStart" register usage does not work for the 64-bit case because we
  // clobber two registers for the result and may need two registers for a
  // scaled address; we can't guarantee non-interference.

  auto* lir = new (alloc()) LWasmLoadI64(baseAlloc, useRegister(memoryBase));

  Scalar::Type accessType = ins->access().type();
  if (accessType == Scalar::Int8 || accessType == Scalar::Int16 ||
      accessType == Scalar::Int32) {
    // We use cdq to sign-extend the result and cdq demands these registers.
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(edx)),
                                      LAllocation(AnyRegister(eax))));
    return;
  }

  defineInt64(lir, ins);
}

void LIRGenerator::visitWasmStore(MWasmStore* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  MDefinition* memoryBase = ins->memoryBase();
  MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

  if (ins->access().type() == Scalar::Int64 && ins->access().isAtomic()) {
    auto* lir = new (alloc())
        LWasmAtomicStoreI64(useRegister(memoryBase), useRegister(base),
                            useInt64Fixed(ins->value(), Register64(ecx, ebx)),
                            tempFixed(edx), tempFixed(eax));
    add(lir, ins);
    return;
  }

  LAllocation baseAlloc;
  if (!base->isConstant() || !OptimizableConstantAccess(base, ins->access())) {
    baseAlloc = useRegisterAtStart(base);
  }

  LAllocation valueAlloc;
  switch (ins->access().type()) {
    case Scalar::Int8:
    case Scalar::Uint8:
      // See comment for LIRGeneratorX86::useByteOpRegister.
      valueAlloc = useFixed(ins->value(), eax);
      break;
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
    case Scalar::Float64:
      // For now, don't allow constant values. The immediate operand affects
      // instruction layout which affects patching.
      valueAlloc = useRegisterAtStart(ins->value());
      break;
    case Scalar::Simd128:
#ifdef ENABLE_WASM_SIMD
      valueAlloc = useRegisterAtStart(ins->value());
      break;
#else
      MOZ_CRASH("unexpected array type");
#endif
    case Scalar::Int64: {
      LInt64Allocation valueAlloc = useInt64RegisterAtStart(ins->value());
      auto* lir = new (alloc())
          LWasmStoreI64(baseAlloc, valueAlloc, useRegisterAtStart(memoryBase));
      add(lir, ins);
      return;
    }
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected array type");
  }

  auto* lir = new (alloc())
      LWasmStore(baseAlloc, valueAlloc, useRegisterAtStart(memoryBase));
  add(lir, ins);
}

void LIRGenerator::visitWasmCompareExchangeHeap(MWasmCompareExchangeHeap* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  MDefinition* memoryBase = ins->memoryBase();
  MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

  if (ins->access().type() == Scalar::Int64) {
    auto* lir = new (alloc()) LWasmCompareExchangeI64(
        useRegisterAtStart(memoryBase), useRegisterAtStart(base),
        useInt64FixedAtStart(ins->oldValue(), Register64(edx, eax)),
        useInt64FixedAtStart(ins->newValue(), Register64(ecx, ebx)));
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(edx)),
                                      LAllocation(AnyRegister(eax))));
    return;
  }

  MOZ_ASSERT(ins->access().type() < Scalar::Float32);

  bool byteArray = byteSize(ins->access().type()) == 1;

  // Register allocation:
  //
  // The output may not be used, but eax will be clobbered regardless
  // so pin the output to eax.
  //
  // oldval must be in a register.
  //
  // newval must be in a register.  If the source is a byte array
  // then newval must be a register that has a byte size: this must
  // be ebx, ecx, or edx (eax is taken).
  //
  // Bug #1077036 describes some optimization opportunities.

  const LAllocation oldval = useRegister(ins->oldValue());
  const LAllocation newval =
      byteArray ? useFixed(ins->newValue(), ebx) : useRegister(ins->newValue());

  LWasmCompareExchangeHeap* lir = new (alloc()) LWasmCompareExchangeHeap(
      useRegister(base), oldval, newval, useRegister(memoryBase));

  lir->setAddrTemp(temp());
  defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
}

void LIRGenerator::visitWasmAtomicExchangeHeap(MWasmAtomicExchangeHeap* ins) {
  MDefinition* memoryBase = ins->memoryBase();
  MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

  if (ins->access().type() == Scalar::Int64) {
    MDefinition* base = ins->base();
    auto* lir = new (alloc()) LWasmAtomicExchangeI64(
        useRegister(memoryBase), useRegister(base),
        useInt64Fixed(ins->value(), Register64(ecx, ebx)), ins->access());
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(edx)),
                                      LAllocation(AnyRegister(eax))));
    return;
  }

  const LAllocation base = useRegister(ins->base());
  const LAllocation value = useRegister(ins->value());

  LWasmAtomicExchangeHeap* lir = new (alloc())
      LWasmAtomicExchangeHeap(base, value, useRegister(memoryBase));

  lir->setAddrTemp(temp());
  if (byteSize(ins->access().type()) == 1) {
    defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
  } else {
    define(lir, ins);
  }
}

void LIRGenerator::visitWasmAtomicBinopHeap(MWasmAtomicBinopHeap* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  MDefinition* memoryBase = ins->memoryBase();
  MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

  if (ins->access().type() == Scalar::Int64) {
    auto* lir = new (alloc())
        LWasmAtomicBinopI64(useRegister(memoryBase), useRegister(base),
                            useInt64Fixed(ins->value(), Register64(ecx, ebx)),
                            ins->access(), ins->operation());
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(edx)),
                                      LAllocation(AnyRegister(eax))));
    return;
  }

  MOZ_ASSERT(ins->access().type() < Scalar::Float32);

  bool byteArray = byteSize(ins->access().type()) == 1;

  // Case 1: the result of the operation is not used.
  //
  // We'll emit a single instruction: LOCK ADD, LOCK SUB, LOCK AND,
  // LOCK OR, or LOCK XOR.  These can all take an immediate.

  if (!ins->hasUses()) {
    LAllocation value;
    if (byteArray && !ins->value()->isConstant()) {
      value = useFixed(ins->value(), ebx);
    } else {
      value = useRegisterOrConstant(ins->value());
    }
    LWasmAtomicBinopHeapForEffect* lir =
        new (alloc()) LWasmAtomicBinopHeapForEffect(useRegister(base), value,
                                                    LDefinition::BogusTemp(),
                                                    useRegister(memoryBase));
    lir->setAddrTemp(temp());
    add(lir, ins);
    return;
  }

  // Case 2: the result of the operation is used.
  //
  // For ADD and SUB we'll use XADD:
  //
  //    movl       value, output
  //    lock xaddl output, mem
  //
  // For the 8-bit variants XADD needs a byte register for the
  // output only, we can still set up with movl; just pin the output
  // to eax (or ebx / ecx / edx).
  //
  // For AND/OR/XOR we need to use a CMPXCHG loop:
  //
  //    movl          *mem, eax
  // L: mov           eax, temp
  //    andl          value, temp
  //    lock cmpxchg  temp, mem  ; reads eax also
  //    jnz           L
  //    ; result in eax
  //
  // Note the placement of L, cmpxchg will update eax with *mem if
  // *mem does not have the expected value, so reloading it at the
  // top of the loop would be redundant.
  //
  // We want to fix eax as the output.  We also need a temp for
  // the intermediate value.
  //
  // For the 8-bit variants the temp must have a byte register.
  //
  // There are optimization opportunities:
  //  - better 8-bit register allocation and instruction selection, Bug
  //  #1077036.

  bool bitOp = !(ins->operation() == AtomicFetchAddOp ||
                 ins->operation() == AtomicFetchSubOp);
  LDefinition tempDef = LDefinition::BogusTemp();
  LAllocation value;

  if (byteArray) {
    value = useFixed(ins->value(), ebx);
    if (bitOp) {
      tempDef = tempFixed(ecx);
    }
  } else if (bitOp || ins->value()->isConstant()) {
    value = useRegisterOrConstant(ins->value());
    if (bitOp) {
      tempDef = temp();
    }
  } else {
    value = useRegisterAtStart(ins->value());
  }

  LWasmAtomicBinopHeap* lir = new (alloc())
      LWasmAtomicBinopHeap(useRegister(base), value, tempDef,
                           LDefinition::BogusTemp(), useRegister(memoryBase));

  lir->setAddrTemp(temp());
  if (byteArray || bitOp) {
    defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
  } else if (ins->value()->isConstant()) {
    define(lir, ins);
  } else {
    defineReuseInput(lir, ins, LWasmAtomicBinopHeap::valueOp);
  }
}

void LIRGeneratorX86::lowerDivI64(MDiv* div) {
  MOZ_CRASH("We use MWasmBuiltinModI64 instead.");
}

void LIRGeneratorX86::lowerWasmBuiltinDivI64(MWasmBuiltinDivI64* div) {
  MOZ_ASSERT(div->lhs()->type() == div->rhs()->type());
  MOZ_ASSERT(IsNumberType(div->type()));

  MOZ_ASSERT(div->type() == MIRType::Int64);

  if (div->isUnsigned()) {
    LUDivOrModI64* lir = new (alloc())
        LUDivOrModI64(useInt64FixedAtStart(div->lhs(), Register64(eax, ebx)),
                      useInt64FixedAtStart(div->rhs(), Register64(ecx, edx)),
                      useFixedAtStart(div->tls(), WasmTlsReg));
    defineReturn(lir, div);
    return;
  }

  LDivOrModI64* lir = new (alloc())
      LDivOrModI64(useInt64FixedAtStart(div->lhs(), Register64(eax, ebx)),
                   useInt64FixedAtStart(div->rhs(), Register64(ecx, edx)),
                   useFixedAtStart(div->tls(), WasmTlsReg));
  defineReturn(lir, div);
}

void LIRGeneratorX86::lowerModI64(MMod* mod) {
  MOZ_CRASH("We use MWasmBuiltinModI64 instead.");
}

void LIRGeneratorX86::lowerWasmBuiltinModI64(MWasmBuiltinModI64* mod) {
  MDefinition* lhs = mod->lhs();
  MDefinition* rhs = mod->rhs();
  MOZ_ASSERT(lhs->type() == rhs->type());
  MOZ_ASSERT(IsNumberType(mod->type()));

  MOZ_ASSERT(mod->type() == MIRType::Int64);
  MOZ_ASSERT(mod->type() == MIRType::Int64);

  if (mod->isUnsigned()) {
    LUDivOrModI64* lir = new (alloc())
        LUDivOrModI64(useInt64FixedAtStart(lhs, Register64(eax, ebx)),
                      useInt64FixedAtStart(rhs, Register64(ecx, edx)),
                      useFixedAtStart(mod->tls(), WasmTlsReg));
    defineReturn(lir, mod);
    return;
  }

  LDivOrModI64* lir = new (alloc())
      LDivOrModI64(useInt64FixedAtStart(lhs, Register64(eax, ebx)),
                   useInt64FixedAtStart(rhs, Register64(ecx, edx)),
                   useFixedAtStart(mod->tls(), WasmTlsReg));
  defineReturn(lir, mod);
}

void LIRGeneratorX86::lowerUDivI64(MDiv* div) {
  MOZ_CRASH("We use MWasmBuiltinDivI64 instead.");
}

void LIRGeneratorX86::lowerUModI64(MMod* mod) {
  MOZ_CRASH("We use MWasmBuiltinModI64 instead.");
}

void LIRGeneratorX86::lowerBigIntDiv(MBigIntDiv* ins) {
  auto* lir = new (alloc()) LBigIntDiv(
      useRegister(ins->lhs()), useRegister(ins->rhs()), tempFixed(eax), temp());
  defineFixed(lir, ins, LAllocation(AnyRegister(edx)));
  assignSafepoint(lir, ins);
}

void LIRGeneratorX86::lowerBigIntMod(MBigIntMod* ins) {
  auto* lir = new (alloc()) LBigIntMod(
      useRegister(ins->lhs()), useRegister(ins->rhs()), tempFixed(eax), temp());
  defineFixed(lir, ins, LAllocation(AnyRegister(edx)));
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitSubstr(MSubstr* ins) {
  // Due to lack of registers on x86, we reuse the string register as
  // temporary. As a result we only need two temporary registers and take a
  // bugos temporary as fifth argument.
  LSubstr* lir = new (alloc())
      LSubstr(useRegister(ins->string()), useRegister(ins->begin()),
              useRegister(ins->length()), temp(), LDefinition::BogusTemp(),
              tempByteOpRegister());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitWasmTruncateToInt64(MWasmTruncateToInt64* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

  LDefinition temp = tempDouble();
  defineInt64(new (alloc()) LWasmTruncateToInt64(useRegister(opd), temp), ins);
}

void LIRGeneratorX86::lowerWasmBuiltinTruncateToInt64(
    MWasmBuiltinTruncateToInt64* ins) {
  MOZ_CRASH("We don't use it for this architecture");
}

void LIRGenerator::visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Int64);
  MOZ_ASSERT(IsFloatingPointType(ins->type()));

  LDefinition maybeTemp =
      (ins->isUnsigned() &&
       ((ins->type() == MIRType::Double && AssemblerX86Shared::HasSSE3()) ||
        ins->type() == MIRType::Float32))
          ? temp()
          : LDefinition::BogusTemp();

  define(new (alloc()) LInt64ToFloatingPoint(useInt64Register(opd), maybeTemp),
         ins);
}

void LIRGeneratorX86::lowerBuiltinInt64ToFloatingPoint(
    MBuiltinInt64ToFloatingPoint* ins) {
  MOZ_CRASH("We don't use it for this architecture");
}

void LIRGenerator::visitExtendInt32ToInt64(MExtendInt32ToInt64* ins) {
  if (ins->isUnsigned()) {
    defineInt64(new (alloc())
                    LExtendInt32ToInt64(useRegisterAtStart(ins->input())),
                ins);
  } else {
    LExtendInt32ToInt64* lir =
        new (alloc()) LExtendInt32ToInt64(useFixedAtStart(ins->input(), eax));
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(edx)),
                                      LAllocation(AnyRegister(eax))));
  }
}

void LIRGenerator::visitSignExtendInt64(MSignExtendInt64* ins) {
  // Here we'll end up using cdq which requires input and output in (edx,eax).
  LSignExtendInt64* lir = new (alloc()) LSignExtendInt64(
      useInt64FixedAtStart(ins->input(), Register64(edx, eax)));
  defineInt64Fixed(lir, ins,
                   LInt64Allocation(LAllocation(AnyRegister(edx)),
                                    LAllocation(AnyRegister(eax))));
}
