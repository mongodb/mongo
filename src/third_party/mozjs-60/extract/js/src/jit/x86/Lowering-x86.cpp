/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/Lowering-x86.h"

#include "jit/MIR.h"
#include "jit/x86/Assembler-x86.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

LBoxAllocation
LIRGeneratorX86::useBoxFixed(MDefinition* mir, Register reg1, Register reg2, bool useAtStart)
{
    MOZ_ASSERT(mir->type() == MIRType::Value);
    MOZ_ASSERT(reg1 != reg2);

    ensureDefined(mir);
    return LBoxAllocation(LUse(reg1, mir->virtualRegister(), useAtStart),
                          LUse(reg2, VirtualRegisterOfPayload(mir), useAtStart));
}

LAllocation
LIRGeneratorX86::useByteOpRegister(MDefinition* mir)
{
    return useFixed(mir, eax);
}

LAllocation
LIRGeneratorX86::useByteOpRegisterAtStart(MDefinition* mir)
{
    return useFixedAtStart(mir, eax);
}

LAllocation
LIRGeneratorX86::useByteOpRegisterOrNonDoubleConstant(MDefinition* mir)
{
    return useFixed(mir, eax);
}

LDefinition
LIRGeneratorX86::tempByteOpRegister()
{
    return tempFixed(eax);
}

void
LIRGeneratorX86::visitBox(MBox* box)
{
    MDefinition* inner = box->getOperand(0);

    // If the box wrapped a double, it needs a new register.
    if (IsFloatingPointType(inner->type())) {
        defineBox(new(alloc()) LBoxFloatingPoint(useRegisterAtStart(inner), tempCopy(inner, 0),
                                                 inner->type()), box);
        return;
    }

    if (box->canEmitAtUses()) {
        emitAtUses(box);
        return;
    }

    if (inner->isConstant()) {
        defineBox(new(alloc()) LValue(inner->toConstant()->toJSValue()), box);
        return;
    }

    LBox* lir = new(alloc()) LBox(use(inner), inner->type());

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

void
LIRGeneratorX86::visitUnbox(MUnbox* unbox)
{
    MDefinition* inner = unbox->getOperand(0);

    if (inner->type() == MIRType::ObjectOrNull) {
        LUnboxObjectOrNull* lir = new(alloc()) LUnboxObjectOrNull(useRegisterAtStart(inner));
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        defineReuseInput(lir, unbox, 0);
        return;
    }

    // An unbox on x86 reads in a type tag (either in memory or a register) and
    // a payload. Unlike most instructions consuming a box, we ask for the type
    // second, so that the result can re-use the first input.
    MOZ_ASSERT(inner->type() == MIRType::Value);

    ensureDefined(inner);

    if (IsFloatingPointType(unbox->type())) {
        LUnboxFloatingPoint* lir = new(alloc()) LUnboxFloatingPoint(useBox(inner), unbox->type());
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        define(lir, unbox);
        return;
    }

    // Swap the order we use the box pieces so we can re-use the payload register.
    LUnbox* lir = new(alloc()) LUnbox;
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

    if (unbox->fallible())
        assignSnapshot(lir, unbox->bailoutKind());

    // Types and payloads form two separate intervals. If the type becomes dead
    // before the payload, it could be used as a Value without the type being
    // recoverable. Unbox's purpose is to eagerly kill the definition of a type
    // tag, so keeping both alive (for the purpose of gcmaps) is unappealing.
    // Instead, we create a new virtual register.
    if (reusePayloadReg)
        defineReuseInput(lir, unbox, 0);
    else
        define(lir, unbox);
}

void
LIRGeneratorX86::visitReturn(MReturn* ret)
{
    MDefinition* opd = ret->getOperand(0);
    MOZ_ASSERT(opd->type() == MIRType::Value);

    LReturn* ins = new(alloc()) LReturn;
    ins->setOperand(0, LUse(JSReturnReg_Type));
    ins->setOperand(1, LUse(JSReturnReg_Data));
    fillBoxUses(ins, 0, opd);
    add(ins);
}

void
LIRGeneratorX86::defineUntypedPhi(MPhi* phi, size_t lirIndex)
{
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

void
LIRGeneratorX86::lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex)
{
    MDefinition* operand = phi->getOperand(inputPosition);
    LPhi* type = block->getPhi(lirIndex + VREG_TYPE_OFFSET);
    LPhi* payload = block->getPhi(lirIndex + VREG_DATA_OFFSET);
    type->setOperand(inputPosition, LUse(operand->virtualRegister() + VREG_TYPE_OFFSET, LUse::ANY));
    payload->setOperand(inputPosition, LUse(VirtualRegisterOfPayload(operand), LUse::ANY));
}

void
LIRGeneratorX86::defineInt64Phi(MPhi* phi, size_t lirIndex)
{
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

void
LIRGeneratorX86::lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex)
{
    MDefinition* operand = phi->getOperand(inputPosition);
    LPhi* low = block->getPhi(lirIndex + INT64LOW_INDEX);
    LPhi* high = block->getPhi(lirIndex + INT64HIGH_INDEX);
    low->setOperand(inputPosition, LUse(operand->virtualRegister() + INT64LOW_INDEX, LUse::ANY));
    high->setOperand(inputPosition, LUse(operand->virtualRegister() + INT64HIGH_INDEX, LUse::ANY));
}

void
LIRGeneratorX86::lowerForALUInt64(LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
                                  MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));
    ins->setInt64Operand(INT64_PIECES, useInt64OrConstant(rhs));
    defineInt64ReuseInput(ins, mir, 0);
}

void
LIRGeneratorX86::lowerForMulInt64(LMulI64* ins, MMul* mir, MDefinition* lhs, MDefinition* rhs)
{
    bool needsTemp = true;

    if (rhs->isConstant()) {
        int64_t constant = rhs->toConstant()->toInt64();
        int32_t shift = mozilla::FloorLog2(constant);
        // See special cases in CodeGeneratorX86Shared::visitMulI64.
        if (constant >= -1 && constant <= 2)
            needsTemp = false;
        if (int64_t(1) << shift == constant)
            needsTemp = false;
    }

    // MulI64 on x86 needs output to be in edx, eax;
    ins->setInt64Operand(0, useInt64Fixed(lhs, Register64(edx, eax), /*useAtStart = */ true));
    ins->setInt64Operand(INT64_PIECES, useInt64OrConstant(rhs));
    if (needsTemp)
        ins->setTemp(0, temp());

    defineInt64Fixed(ins, mir, LInt64Allocation(LAllocation(AnyRegister(edx)),
                                                LAllocation(AnyRegister(eax))));
}

void
LIRGeneratorX86::visitCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins)
{
    lowerCompareExchangeTypedArrayElement(ins, /* useI386ByteRegisters = */ true);
}

void
LIRGeneratorX86::visitAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins)
{
    lowerAtomicExchangeTypedArrayElement(ins, /*useI386ByteRegisters=*/ true);
}

void
LIRGeneratorX86::visitAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins)
{
    lowerAtomicTypedArrayElementBinop(ins, /* useI386ByteRegisters = */ true);
}

void
LIRGeneratorX86::visitWasmUnsignedToDouble(MWasmUnsignedToDouble* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType::Int32);
    LWasmUint32ToDouble* lir = new(alloc()) LWasmUint32ToDouble(useRegisterAtStart(ins->input()), temp());
    define(lir, ins);
}

void
LIRGeneratorX86::visitWasmUnsignedToFloat32(MWasmUnsignedToFloat32* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType::Int32);
    LWasmUint32ToFloat32* lir = new(alloc()) LWasmUint32ToFloat32(useRegisterAtStart(ins->input()), temp());
    define(lir, ins);
}

void
LIRGeneratorX86::visitWasmLoad(MWasmLoad* ins)
{
    MDefinition* base = ins->base();
    MOZ_ASSERT(base->type() == MIRType::Int32);

    MDefinition* memoryBase = ins->memoryBase();
    MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

    if (ins->access().type() == Scalar::Int64 && ins->access().isAtomic()) {
        auto* lir = new(alloc()) LWasmAtomicLoadI64(useRegister(memoryBase),
                                                    useRegister(base),
                                                    tempFixed(ecx),
                                                    tempFixed(ebx));
        defineInt64Fixed(lir, ins, LInt64Allocation(LAllocation(AnyRegister(edx)),
                                                    LAllocation(AnyRegister(eax))));
        return;
    }

    // If the base is a constant, and it is zero or its offset is zero, then
    // code generation will fold the values into the access.  Allocate the
    // pointer to a register only if that can't happen.

    LAllocation baseAlloc;
    if (!base->isConstant() || !(base->toConstant()->isInt32(0) || ins->access().offset() == 0))
        baseAlloc = ins->type() == MIRType::Int64 ? useRegister(base) : useRegisterAtStart(base);

    if (ins->type() != MIRType::Int64) {
        auto* lir = new(alloc()) LWasmLoad(baseAlloc, useRegisterAtStart(memoryBase));
        define(lir, ins);
        return;
    }

    // "AtStart" register usage does not work for the 64-bit case because we
    // clobber two registers for the result and may need two registers for a
    // scaled address; we can't guarantee non-interference.

    auto* lir = new(alloc()) LWasmLoadI64(baseAlloc, useRegister(memoryBase));

    Scalar::Type accessType = ins->access().type();
    if (accessType == Scalar::Int8 || accessType == Scalar::Int16 || accessType == Scalar::Int32) {
        // We use cdq to sign-extend the result and cdq demands these registers.
        defineInt64Fixed(lir, ins, LInt64Allocation(LAllocation(AnyRegister(edx)),
                                                    LAllocation(AnyRegister(eax))));
        return;
    }

    defineInt64(lir, ins);
}

void
LIRGeneratorX86::visitWasmStore(MWasmStore* ins)
{
    MDefinition* base = ins->base();
    MOZ_ASSERT(base->type() == MIRType::Int32);

    MDefinition* memoryBase = ins->memoryBase();
    MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

    if (ins->access().type() == Scalar::Int64 && ins->access().isAtomic()) {
        auto* lir = new(alloc()) LWasmAtomicStoreI64(useRegister(memoryBase),
                                                     useRegister(base),
                                                     useInt64Fixed(ins->value(),
                                                                   Register64(ecx, ebx)),
                                                     tempFixed(edx),
                                                     tempFixed(eax));
        add(lir, ins);
        return;
    }

    // If the base is a constant, and it is zero or its offset is zero, then
    // code generation will fold the values into the access.  Allocate the
    // pointer to a register only if that can't happen.

    LAllocation baseAlloc;
    if (!base->isConstant() || !(base->toConstant()->isInt32(0) || ins->access().offset() == 0))
        baseAlloc = useRegisterAtStart(base);

    LAllocation valueAlloc;
    switch (ins->access().type()) {
      case Scalar::Int8: case Scalar::Uint8:
        // See comment for LIRGeneratorX86::useByteOpRegister.
        valueAlloc = useFixed(ins->value(), eax);
        break;
      case Scalar::Int16: case Scalar::Uint16:
      case Scalar::Int32: case Scalar::Uint32:
      case Scalar::Float32: case Scalar::Float64:
      case Scalar::Float32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
      case Scalar::Int32x4:
        // For now, don't allow constant values. The immediate operand affects
        // instruction layout which affects patching.
        valueAlloc = useRegisterAtStart(ins->value());
        break;
      case Scalar::Int64: {
        LInt64Allocation valueAlloc = useInt64RegisterAtStart(ins->value());
        auto* lir = new(alloc()) LWasmStoreI64(baseAlloc, valueAlloc,
                                               useRegisterAtStart(memoryBase));
        add(lir, ins);
        return;
      }
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected array type");
    }

    auto* lir = new(alloc()) LWasmStore(baseAlloc, valueAlloc, useRegisterAtStart(memoryBase));
    add(lir, ins);
}

void
LIRGeneratorX86::visitAsmJSLoadHeap(MAsmJSLoadHeap* ins)
{
    MDefinition* base = ins->base();
    MOZ_ASSERT(base->type() == MIRType::Int32);

    MDefinition* boundsCheckLimit = ins->boundsCheckLimit();
    MOZ_ASSERT_IF(ins->needsBoundsCheck(), boundsCheckLimit->type() == MIRType::Int32);

    MDefinition* memoryBase = ins->memoryBase();
    MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

    // For simplicity, require a register if we're going to emit a bounds-check
    // branch, so that we don't have special cases for constants.
    LAllocation baseAlloc = ins->needsBoundsCheck()
                            ? useRegisterAtStart(base)
                            : useRegisterOrZeroAtStart(base);
    LAllocation limitAlloc = ins->needsBoundsCheck()
                           ? useRegisterAtStart(boundsCheckLimit)
                           : LAllocation();

    auto* lir = new(alloc()) LAsmJSLoadHeap(baseAlloc, limitAlloc, useRegisterAtStart(memoryBase));
    define(lir, ins);
}

void
LIRGeneratorX86::visitAsmJSStoreHeap(MAsmJSStoreHeap* ins)
{
    MDefinition* base = ins->base();
    MOZ_ASSERT(base->type() == MIRType::Int32);

    MDefinition* boundsCheckLimit = ins->boundsCheckLimit();
    MOZ_ASSERT_IF(ins->needsBoundsCheck(), boundsCheckLimit->type() == MIRType::Int32);

    MDefinition* memoryBase = ins->memoryBase();
    MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

    // For simplicity, require a register if we're going to emit a bounds-check
    // branch, so that we don't have special cases for constants.
    LAllocation baseAlloc = ins->needsBoundsCheck()
                            ? useRegisterAtStart(base)
                            : useRegisterOrZeroAtStart(base);
    LAllocation limitAlloc = ins->needsBoundsCheck()
                           ? useRegisterAtStart(boundsCheckLimit)
                           : LAllocation();

    LAsmJSStoreHeap* lir = nullptr;
    switch (ins->access().type()) {
      case Scalar::Int8: case Scalar::Uint8:
        // See comment for LIRGeneratorX86::useByteOpRegister.
        lir = new(alloc()) LAsmJSStoreHeap(baseAlloc, useFixed(ins->value(), eax),
                                           limitAlloc, useRegisterAtStart(memoryBase));
        break;
      case Scalar::Int16: case Scalar::Uint16:
      case Scalar::Int32: case Scalar::Uint32:
      case Scalar::Float32: case Scalar::Float64:
      case Scalar::Float32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
      case Scalar::Int32x4:
        // For now, don't allow constant values. The immediate operand affects
        // instruction layout which affects patching.
        lir = new (alloc()) LAsmJSStoreHeap(baseAlloc, useRegisterAtStart(ins->value()),
                                            limitAlloc, useRegisterAtStart(memoryBase));
        break;
      case Scalar::Int64:
        MOZ_CRASH("NYI");
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected array type");
    }
    add(lir, ins);
}

void
LIRGeneratorX86::visitWasmCompareExchangeHeap(MWasmCompareExchangeHeap* ins)
{
    MDefinition* base = ins->base();
    MOZ_ASSERT(base->type() == MIRType::Int32);

    MDefinition* memoryBase = ins->memoryBase();
    MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

    if (ins->access().type() == Scalar::Int64) {
        auto* lir = new(alloc()) LWasmCompareExchangeI64(useRegister(memoryBase),
                                                         useRegister(base),
                                                         useInt64Fixed(ins->oldValue(),
                                                                       Register64(edx, eax)),
                                                         useInt64Fixed(ins->newValue(),
                                                                       Register64(ecx, ebx)));
        defineInt64Fixed(lir, ins, LInt64Allocation(LAllocation(AnyRegister(edx)),
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
    const LAllocation newval = byteArray ? useFixed(ins->newValue(), ebx) : useRegister(ins->newValue());

    LWasmCompareExchangeHeap* lir =
        new(alloc()) LWasmCompareExchangeHeap(useRegister(base), oldval, newval, useRegister(memoryBase));

    lir->setAddrTemp(temp());
    defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
}

void
LIRGeneratorX86::visitWasmAtomicExchangeHeap(MWasmAtomicExchangeHeap* ins)
{
    MDefinition* memoryBase = ins->memoryBase();
    MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

    if (ins->access().type() == Scalar::Int64) {
        MDefinition* base = ins->base();
        auto* lir = new(alloc()) LWasmAtomicExchangeI64(useRegister(memoryBase),
                                                        useRegister(base),
                                                        useInt64Fixed(ins->value(),
                                                                      Register64(ecx, ebx)),
                                                        ins->access());
        defineInt64Fixed(lir, ins, LInt64Allocation(LAllocation(AnyRegister(edx)),
                                                    LAllocation(AnyRegister(eax))));
        return;
    }

    const LAllocation base = useRegister(ins->base());
    const LAllocation value = useRegister(ins->value());

    LWasmAtomicExchangeHeap* lir =
        new(alloc()) LWasmAtomicExchangeHeap(base, value, useRegister(memoryBase));

    lir->setAddrTemp(temp());
    if (byteSize(ins->access().type()) == 1)
        defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
    else
        define(lir, ins);
}

void
LIRGeneratorX86::visitWasmAtomicBinopHeap(MWasmAtomicBinopHeap* ins)
{
    MDefinition* base = ins->base();
    MOZ_ASSERT(base->type() == MIRType::Int32);

    MDefinition* memoryBase = ins->memoryBase();
    MOZ_ASSERT(memoryBase->type() == MIRType::Pointer);

    if (ins->access().type() == Scalar::Int64) {
        auto* lir = new(alloc()) LWasmAtomicBinopI64(useRegister(memoryBase),
                                                     useRegister(base),
                                                     useInt64Fixed(ins->value(),
                                                                   Register64(ecx, ebx)),
                                                     ins->access(),
                                                     ins->operation());
        defineInt64Fixed(lir, ins, LInt64Allocation(LAllocation(AnyRegister(edx)),
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
        if (byteArray && !ins->value()->isConstant())
            value = useFixed(ins->value(), ebx);
        else
            value = useRegisterOrConstant(ins->value());
        LWasmAtomicBinopHeapForEffect* lir =
            new(alloc()) LWasmAtomicBinopHeapForEffect(useRegister(base), value, LDefinition::BogusTemp(),
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
    //  - better 8-bit register allocation and instruction selection, Bug #1077036.

    bool bitOp = !(ins->operation() == AtomicFetchAddOp || ins->operation() == AtomicFetchSubOp);
    LDefinition tempDef = LDefinition::BogusTemp();
    LAllocation value;

    if (byteArray) {
        value = useFixed(ins->value(), ebx);
        if (bitOp)
            tempDef = tempFixed(ecx);
    } else if (bitOp || ins->value()->isConstant()) {
        value = useRegisterOrConstant(ins->value());
        if (bitOp)
            tempDef = temp();
    } else {
        value = useRegisterAtStart(ins->value());
    }

    LWasmAtomicBinopHeap* lir =
        new(alloc()) LWasmAtomicBinopHeap(useRegister(base), value, tempDef, LDefinition::BogusTemp(),
                                          useRegister(memoryBase));

    lir->setAddrTemp(temp());
    if (byteArray || bitOp)
        defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
    else if (ins->value()->isConstant())
        define(lir, ins);
    else
        defineReuseInput(lir, ins, LWasmAtomicBinopHeap::valueOp);
}

void
LIRGeneratorX86::lowerDivI64(MDiv* div)
{
    if (div->isUnsigned()) {
        lowerUDivI64(div);
        return;
    }

    LDivOrModI64* lir = new(alloc()) LDivOrModI64(useInt64FixedAtStart(div->lhs(), Register64(eax, ebx)),
                                                  useInt64FixedAtStart(div->rhs(), Register64(ecx, edx)),
                                                  tempFixed(esi));
    defineReturn(lir, div);
}

void
LIRGeneratorX86::lowerModI64(MMod* mod)
{
    if (mod->isUnsigned()) {
        lowerUModI64(mod);
        return;
    }

    LDivOrModI64* lir = new(alloc()) LDivOrModI64(useInt64FixedAtStart(mod->lhs(), Register64(eax, ebx)),
                                                  useInt64FixedAtStart(mod->rhs(), Register64(ecx, edx)),
                                                  tempFixed(esi));
    defineReturn(lir, mod);
}

void
LIRGeneratorX86::lowerUDivI64(MDiv* div)
{
    LUDivOrModI64* lir = new(alloc()) LUDivOrModI64(useInt64FixedAtStart(div->lhs(), Register64(eax, ebx)),
                                                    useInt64FixedAtStart(div->rhs(), Register64(ecx, edx)),
                                                    tempFixed(esi));
    defineReturn(lir, div);
}

void
LIRGeneratorX86::lowerUModI64(MMod* mod)
{
    LUDivOrModI64* lir = new(alloc()) LUDivOrModI64(useInt64FixedAtStart(mod->lhs(), Register64(eax, ebx)),
                                                    useInt64FixedAtStart(mod->rhs(), Register64(ecx, edx)),
                                                    tempFixed(esi));
    defineReturn(lir, mod);
}

void
LIRGeneratorX86::visitSubstr(MSubstr* ins)
{
    // Due to lack of registers on x86, we reuse the string register as
    // temporary. As a result we only need two temporary registers and take a
    // bugos temporary as fifth argument.
    LSubstr* lir = new (alloc()) LSubstr(useRegister(ins->string()),
                                         useRegister(ins->begin()),
                                         useRegister(ins->length()),
                                         temp(),
                                         LDefinition::BogusTemp(),
                                         tempByteOpRegister());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGeneratorX86::visitRandom(MRandom* ins)
{
    LRandom *lir = new(alloc()) LRandom(temp(),
                                        temp(),
                                        temp(),
                                        temp(),
                                        temp());
    defineFixed(lir, ins, LFloatReg(ReturnDoubleReg));
}

void
LIRGeneratorX86::visitWasmTruncateToInt64(MWasmTruncateToInt64* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

    LDefinition temp = tempDouble();
    defineInt64(new(alloc()) LWasmTruncateToInt64(useRegister(opd), temp), ins);
}

void
LIRGeneratorX86::visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType::Int64);
    MOZ_ASSERT(IsFloatingPointType(ins->type()));

    LDefinition maybeTemp = (ins->isUnsigned() &&
                             ((ins->type() == MIRType::Double && AssemblerX86Shared::HasSSE3()) ||
                              ins->type() == MIRType::Float32))
                            ? temp()
                            : LDefinition::BogusTemp();

    define(new(alloc()) LInt64ToFloatingPoint(useInt64Register(opd), maybeTemp), ins);
}

void
LIRGeneratorX86::visitExtendInt32ToInt64(MExtendInt32ToInt64* ins)
{
    if (ins->isUnsigned()) {
        defineInt64(new(alloc()) LExtendInt32ToInt64(useRegisterAtStart(ins->input())), ins);
    } else {
        LExtendInt32ToInt64* lir =
            new(alloc()) LExtendInt32ToInt64(useFixedAtStart(ins->input(), eax));
        defineInt64Fixed(lir, ins, LInt64Allocation(LAllocation(AnyRegister(edx)),
                                                    LAllocation(AnyRegister(eax))));
    }
}

void
LIRGeneratorX86::visitSignExtendInt64(MSignExtendInt64* ins)
{
    // Here we'll end up using cdq which requires input and output in (edx,eax).
    LSignExtendInt64* lir =
        new(alloc()) LSignExtendInt64(useInt64FixedAtStart(ins->input(), Register64(edx, eax)));
    defineInt64Fixed(lir, ins, LInt64Allocation(LAllocation(AnyRegister(edx)),
                                                LAllocation(AnyRegister(eax))));
}
