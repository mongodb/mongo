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

void
LIRGeneratorX86::useBoxFixed(LInstruction* lir, size_t n, MDefinition* mir, Register reg1,
                             Register reg2, bool useAtStart)
{
    MOZ_ASSERT(mir->type() == MIRType_Value);
    MOZ_ASSERT(reg1 != reg2);

    ensureDefined(mir);
    lir->setOperand(n, LUse(reg1, mir->virtualRegister(), useAtStart));
    lir->setOperand(n + 1, LUse(reg2, VirtualRegisterOfPayload(mir), useAtStart));
}

LAllocation
LIRGeneratorX86::useByteOpRegister(MDefinition* mir)
{
    return useFixed(mir, eax);
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
        defineBox(new(alloc()) LValue(inner->toConstant()->value()), box);
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

    if (inner->type() == MIRType_ObjectOrNull) {
        LUnboxObjectOrNull* lir = new(alloc()) LUnboxObjectOrNull(useRegisterAtStart(inner));
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        defineReuseInput(lir, unbox, 0);
        return;
    }

    // An unbox on x86 reads in a type tag (either in memory or a register) and
    // a payload. Unlike most instructions consuming a box, we ask for the type
    // second, so that the result can re-use the first input.
    MOZ_ASSERT(inner->type() == MIRType_Value);

    ensureDefined(inner);

    if (IsFloatingPointType(unbox->type())) {
        LUnboxFloatingPoint* lir = new(alloc()) LUnboxFloatingPoint(unbox->type());
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        useBox(lir, LUnboxFloatingPoint::Input, inner);
        define(lir, unbox);
        return;
    }

    // Swap the order we use the box pieces so we can re-use the payload register.
    LUnbox* lir = new(alloc()) LUnbox;
    lir->setOperand(0, usePayloadInRegisterAtStart(inner));
    lir->setOperand(1, useType(inner, LUse::ANY));

    if (unbox->fallible())
        assignSnapshot(lir, unbox->bailoutKind());

    // Types and payloads form two separate intervals. If the type becomes dead
    // before the payload, it could be used as a Value without the type being
    // recoverable. Unbox's purpose is to eagerly kill the definition of a type
    // tag, so keeping both alive (for the purpose of gcmaps) is unappealing.
    // Instead, we create a new virtual register.
    defineReuseInput(lir, unbox, 0);
}

void
LIRGeneratorX86::visitReturn(MReturn* ret)
{
    MDefinition* opd = ret->getOperand(0);
    MOZ_ASSERT(opd->type() == MIRType_Value);

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
LIRGeneratorX86::visitAsmJSUnsignedToDouble(MAsmJSUnsignedToDouble* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Int32);
    LAsmJSUInt32ToDouble* lir = new(alloc()) LAsmJSUInt32ToDouble(useRegisterAtStart(ins->input()), temp());
    define(lir, ins);
}

void
LIRGeneratorX86::visitAsmJSUnsignedToFloat32(MAsmJSUnsignedToFloat32* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Int32);
    LAsmJSUInt32ToFloat32* lir = new(alloc()) LAsmJSUInt32ToFloat32(useRegisterAtStart(ins->input()), temp());
    define(lir, ins);
}

void
LIRGeneratorX86::visitAsmJSLoadHeap(MAsmJSLoadHeap* ins)
{
    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);

    // For simplicity, require a register if we're going to emit a bounds-check
    // branch, so that we don't have special cases for constants.
    LAllocation ptrAlloc = gen->needsAsmJSBoundsCheckBranch(ins)
                           ? useRegisterAtStart(ptr)
                           : useRegisterOrZeroAtStart(ptr);

    define(new(alloc()) LAsmJSLoadHeap(ptrAlloc), ins);
}

void
LIRGeneratorX86::visitAsmJSStoreHeap(MAsmJSStoreHeap* ins)
{
    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);

    // For simplicity, require a register if we're going to emit a bounds-check
    // branch, so that we don't have special cases for constants.
    LAllocation ptrAlloc = gen->needsAsmJSBoundsCheckBranch(ins)
                           ? useRegisterAtStart(ptr)
                           : useRegisterOrZeroAtStart(ptr);

    LAsmJSStoreHeap* lir = nullptr;
    switch (ins->accessType()) {
      case Scalar::Int8: case Scalar::Uint8:
        // See comment for LIRGeneratorX86::useByteOpRegister.
        lir = new(alloc()) LAsmJSStoreHeap(ptrAlloc, useFixed(ins->value(), eax));
        break;
      case Scalar::Int16: case Scalar::Uint16:
      case Scalar::Int32: case Scalar::Uint32:
      case Scalar::Float32: case Scalar::Float64:
      case Scalar::Float32x4: case Scalar::Int32x4:
        // For now, don't allow constant values. The immediate operand
        // affects instruction layout which affects patching.
        lir = new(alloc()) LAsmJSStoreHeap(ptrAlloc, useRegisterAtStart(ins->value()));
        break;
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected array type");
    }
    add(lir, ins);
}

void
LIRGeneratorX86::visitStoreTypedArrayElementStatic(MStoreTypedArrayElementStatic* ins)
{
    // The code generated for StoreTypedArrayElementStatic is identical to that
    // for AsmJSStoreHeap, and the same concerns apply.
    LStoreTypedArrayElementStatic* lir;
    switch (ins->accessType()) {
      case Scalar::Int8: case Scalar::Uint8:
      case Scalar::Uint8Clamped:
        lir = new(alloc()) LStoreTypedArrayElementStatic(useRegister(ins->ptr()),
                                                         useFixed(ins->value(), eax));
        break;
      case Scalar::Int16: case Scalar::Uint16:
      case Scalar::Int32: case Scalar::Uint32:
      case Scalar::Float32: case Scalar::Float64:
        lir = new(alloc()) LStoreTypedArrayElementStatic(useRegisterAtStart(ins->ptr()),
                                                         useRegisterAtStart(ins->value()));
        break;
      default: MOZ_CRASH("unexpected array type");
    }

    add(lir, ins);
}

void
LIRGeneratorX86::visitAsmJSCompareExchangeHeap(MAsmJSCompareExchangeHeap* ins)
{
    MOZ_ASSERT(ins->accessType() < Scalar::Float32);

    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);

    bool byteArray = byteSize(ins->accessType()) == 1;

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

    LAsmJSCompareExchangeHeap* lir =
        new(alloc()) LAsmJSCompareExchangeHeap(useRegister(ptr), oldval, newval);

    lir->setAddrTemp(temp());
    defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
}

void
LIRGeneratorX86::visitAsmJSAtomicExchangeHeap(MAsmJSAtomicExchangeHeap* ins)
{
    MOZ_ASSERT(ins->ptr()->type() == MIRType_Int32);

    const LAllocation ptr = useRegister(ins->ptr());
    const LAllocation value = useRegister(ins->value());

    LAsmJSAtomicExchangeHeap* lir =
        new(alloc()) LAsmJSAtomicExchangeHeap(ptr, value);

    lir->setAddrTemp(temp());
    if (byteSize(ins->accessType()) == 1)
        defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
    else
        define(lir, ins);
}

void
LIRGeneratorX86::visitAsmJSAtomicBinopHeap(MAsmJSAtomicBinopHeap* ins)
{
    MOZ_ASSERT(ins->accessType() < Scalar::Float32);

    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);

    bool byteArray = byteSize(ins->accessType()) == 1;

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
        LAsmJSAtomicBinopHeapForEffect* lir =
            new(alloc()) LAsmJSAtomicBinopHeapForEffect(useRegister(ptr), value);
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

    LAsmJSAtomicBinopHeap* lir =
        new(alloc()) LAsmJSAtomicBinopHeap(useRegister(ptr), value, tempDef);

    lir->setAddrTemp(temp());
    if (byteArray || bitOp)
        defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
    else if (ins->value()->isConstant())
        define(lir, ins);
    else
        defineReuseInput(lir, ins, LAsmJSAtomicBinopHeap::valueOp);
}

void
LIRGeneratorX86::visitAsmJSLoadFuncPtr(MAsmJSLoadFuncPtr* ins)
{
    define(new(alloc()) LAsmJSLoadFuncPtr(useRegisterAtStart(ins->index())), ins);
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
