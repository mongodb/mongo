/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/CodeGenerator-x86.h"

#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"

#include "jsnum.h"

#include "jit/IonCaches.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "js/Conversions.h"
#include "vm/Shape.h"

#include "jsscriptinlines.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::BitwiseCast;
using mozilla::DebugOnly;
using mozilla::FloatingPoint;
using JS::GenericNaN;

CodeGeneratorX86::CodeGeneratorX86(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
  : CodeGeneratorX86Shared(gen, graph, masm)
{
}

static const uint32_t FrameSizes[] = { 128, 256, 512, 1024 };

FrameSizeClass
FrameSizeClass::FromDepth(uint32_t frameDepth)
{
    for (uint32_t i = 0; i < JS_ARRAY_LENGTH(FrameSizes); i++) {
        if (frameDepth < FrameSizes[i])
            return FrameSizeClass(i);
    }

    return FrameSizeClass::None();
}

FrameSizeClass
FrameSizeClass::ClassLimit()
{
    return FrameSizeClass(JS_ARRAY_LENGTH(FrameSizes));
}

uint32_t
FrameSizeClass::frameSize() const
{
    MOZ_ASSERT(class_ != NO_FRAME_SIZE_CLASS_ID);
    MOZ_ASSERT(class_ < JS_ARRAY_LENGTH(FrameSizes));

    return FrameSizes[class_];
}

ValueOperand
CodeGeneratorX86::ToValue(LInstruction* ins, size_t pos)
{
    Register typeReg = ToRegister(ins->getOperand(pos + TYPE_INDEX));
    Register payloadReg = ToRegister(ins->getOperand(pos + PAYLOAD_INDEX));
    return ValueOperand(typeReg, payloadReg);
}

ValueOperand
CodeGeneratorX86::ToOutValue(LInstruction* ins)
{
    Register typeReg = ToRegister(ins->getDef(TYPE_INDEX));
    Register payloadReg = ToRegister(ins->getDef(PAYLOAD_INDEX));
    return ValueOperand(typeReg, payloadReg);
}

ValueOperand
CodeGeneratorX86::ToTempValue(LInstruction* ins, size_t pos)
{
    Register typeReg = ToRegister(ins->getTemp(pos + TYPE_INDEX));
    Register payloadReg = ToRegister(ins->getTemp(pos + PAYLOAD_INDEX));
    return ValueOperand(typeReg, payloadReg);
}

void
CodeGeneratorX86::visitValue(LValue* value)
{
    const ValueOperand out = ToOutValue(value);
    masm.moveValue(value->value(), out);
}

void
CodeGeneratorX86::visitBox(LBox* box)
{
    const LDefinition* type = box->getDef(TYPE_INDEX);

    DebugOnly<const LAllocation*> a = box->getOperand(0);
    MOZ_ASSERT(!a->isConstant());

    // On x86, the input operand and the output payload have the same
    // virtual register. All that needs to be written is the type tag for
    // the type definition.
    masm.mov(ImmWord(MIRTypeToTag(box->type())), ToRegister(type));
}

void
CodeGeneratorX86::visitBoxFloatingPoint(LBoxFloatingPoint* box)
{
    const LAllocation* in = box->getOperand(0);
    const ValueOperand out = ToOutValue(box);

    FloatRegister reg = ToFloatRegister(in);
    if (box->type() == MIRType_Float32) {
        masm.convertFloat32ToDouble(reg, ScratchFloat32Reg);
        reg = ScratchFloat32Reg;
    }
    masm.boxDouble(reg, out);
}

void
CodeGeneratorX86::visitUnbox(LUnbox* unbox)
{
    // Note that for unbox, the type and payload indexes are switched on the
    // inputs.
    MUnbox* mir = unbox->mir();

    if (mir->fallible()) {
        masm.cmp32(ToOperand(unbox->type()), Imm32(MIRTypeToTag(mir->type())));
        bailoutIf(Assembler::NotEqual, unbox->snapshot());
    }
}

void
CodeGeneratorX86::visitCompareB(LCompareB* lir)
{
    MCompare* mir = lir->mir();

    const ValueOperand lhs = ToValue(lir, LCompareB::Lhs);
    const LAllocation* rhs = lir->rhs();
    const Register output = ToRegister(lir->output());

    MOZ_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);

    Label notBoolean, done;
    masm.branchTestBoolean(Assembler::NotEqual, lhs, &notBoolean);
    {
        if (rhs->isConstant())
            masm.cmp32(lhs.payloadReg(), Imm32(rhs->toConstant()->toBoolean()));
        else
            masm.cmp32(lhs.payloadReg(), ToRegister(rhs));
        masm.emitSet(JSOpToCondition(mir->compareType(), mir->jsop()), output);
        masm.jump(&done);
    }
    masm.bind(&notBoolean);
    {
        masm.move32(Imm32(mir->jsop() == JSOP_STRICTNE), output);
    }

    masm.bind(&done);
}

void
CodeGeneratorX86::visitCompareBAndBranch(LCompareBAndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    const ValueOperand lhs = ToValue(lir, LCompareBAndBranch::Lhs);
    const LAllocation* rhs = lir->rhs();

    MOZ_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);

    Assembler::Condition cond = masm.testBoolean(Assembler::NotEqual, lhs);
    jumpToBlock((mir->jsop() == JSOP_STRICTEQ) ? lir->ifFalse() : lir->ifTrue(), cond);

    if (rhs->isConstant())
        masm.cmp32(lhs.payloadReg(), Imm32(rhs->toConstant()->toBoolean()));
    else
        masm.cmp32(lhs.payloadReg(), ToRegister(rhs));
    emitBranch(JSOpToCondition(mir->compareType(), mir->jsop()), lir->ifTrue(), lir->ifFalse());
}

void
CodeGeneratorX86::visitCompareBitwise(LCompareBitwise* lir)
{
    MCompare* mir = lir->mir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareBitwise::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareBitwise::RhsInput);
    const Register output = ToRegister(lir->output());

    MOZ_ASSERT(IsEqualityOp(mir->jsop()));

    Label notEqual, done;
    masm.cmp32(lhs.typeReg(), rhs.typeReg());
    masm.j(Assembler::NotEqual, &notEqual);
    {
        masm.cmp32(lhs.payloadReg(), rhs.payloadReg());
        masm.emitSet(cond, output);
        masm.jump(&done);
    }
    masm.bind(&notEqual);
    {
        masm.move32(Imm32(cond == Assembler::NotEqual), output);
    }

    masm.bind(&done);
}

void
CodeGeneratorX86::visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareBitwiseAndBranch::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareBitwiseAndBranch::RhsInput);

    MOZ_ASSERT(mir->jsop() == JSOP_EQ || mir->jsop() == JSOP_STRICTEQ ||
               mir->jsop() == JSOP_NE || mir->jsop() == JSOP_STRICTNE);

    MBasicBlock* notEqual = (cond == Assembler::Equal) ? lir->ifFalse() : lir->ifTrue();

    masm.cmp32(lhs.typeReg(), rhs.typeReg());
    jumpToBlock(notEqual, Assembler::NotEqual);
    masm.cmp32(lhs.payloadReg(), rhs.payloadReg());
    emitBranch(cond, lir->ifTrue(), lir->ifFalse());
}

void
CodeGeneratorX86::visitAsmJSUInt32ToDouble(LAsmJSUInt32ToDouble* lir)
{
    Register input = ToRegister(lir->input());
    Register temp = ToRegister(lir->temp());

    if (input != temp)
        masm.mov(input, temp);

    // Beware: convertUInt32ToDouble clobbers input.
    masm.convertUInt32ToDouble(temp, ToFloatRegister(lir->output()));
}

void
CodeGeneratorX86::visitAsmJSUInt32ToFloat32(LAsmJSUInt32ToFloat32* lir)
{
    Register input = ToRegister(lir->input());
    Register temp = ToRegister(lir->temp());
    FloatRegister output = ToFloatRegister(lir->output());

    if (input != temp)
        masm.mov(input, temp);

    // Beware: convertUInt32ToFloat32 clobbers input.
    masm.convertUInt32ToFloat32(temp, output);
}

void
CodeGeneratorX86::load(Scalar::Type accessType, const Operand& srcAddr, const LDefinition* out)
{
    switch (accessType) {
      case Scalar::Int8:         masm.movsblWithPatch(srcAddr, ToRegister(out)); break;
      case Scalar::Uint8Clamped:
      case Scalar::Uint8:        masm.movzblWithPatch(srcAddr, ToRegister(out)); break;
      case Scalar::Int16:        masm.movswlWithPatch(srcAddr, ToRegister(out)); break;
      case Scalar::Uint16:       masm.movzwlWithPatch(srcAddr, ToRegister(out)); break;
      case Scalar::Int32:
      case Scalar::Uint32:       masm.movlWithPatch(srcAddr, ToRegister(out)); break;
      case Scalar::Float32:      masm.vmovssWithPatch(srcAddr, ToFloatRegister(out)); break;
      case Scalar::Float64:      masm.vmovsdWithPatch(srcAddr, ToFloatRegister(out)); break;
      case Scalar::Float32x4:
      case Scalar::Int32x4:      MOZ_CRASH("SIMD load should be handled in their own function");
      case Scalar::MaxTypedArrayViewType: MOZ_CRASH("unexpected type");
    }
}

void
CodeGeneratorX86::visitLoadTypedArrayElementStatic(LLoadTypedArrayElementStatic* ins)
{
    const MLoadTypedArrayElementStatic* mir = ins->mir();
    Scalar::Type accessType = mir->accessType();
    MOZ_ASSERT_IF(accessType == Scalar::Float32, mir->type() == MIRType_Float32);

    Register ptr = ToRegister(ins->ptr());
    const LDefinition* out = ins->output();
    OutOfLineLoadTypedArrayOutOfBounds* ool = nullptr;
    uint32_t offset = mir->offset();

    if (mir->needsBoundsCheck()) {
        MOZ_ASSERT(offset == 0);
        if (!mir->fallible()) {
            ool = new(alloc()) OutOfLineLoadTypedArrayOutOfBounds(ToAnyRegister(out), accessType);
            addOutOfLineCode(ool, ins->mir());
        }

        masm.cmpPtr(ptr, ImmWord(mir->length()));
        if (ool)
            masm.j(Assembler::AboveOrEqual, ool->entry());
        else
            bailoutIf(Assembler::AboveOrEqual, ins->snapshot());
    }

    Operand srcAddr(ptr, int32_t(mir->base().asValue()) + int32_t(offset));
    load(accessType, srcAddr, out);
    if (accessType == Scalar::Float64)
        masm.canonicalizeDouble(ToFloatRegister(out));
    if (accessType == Scalar::Float32)
        masm.canonicalizeFloat(ToFloatRegister(out));
    if (ool)
        masm.bind(ool->rejoin());
}

void
CodeGeneratorX86::visitAsmJSCall(LAsmJSCall* ins)
{
    MAsmJSCall* mir = ins->mir();

    emitAsmJSCall(ins);

    if (IsFloatingPointType(mir->type()) && mir->callee().which() == MAsmJSCall::Callee::Builtin) {
        if (mir->type() == MIRType_Float32) {
            masm.reserveStack(sizeof(float));
            Operand op(esp, 0);
            masm.fstp32(op);
            masm.loadFloat32(op, ReturnFloat32Reg);
            masm.freeStack(sizeof(float));
        } else {
            MOZ_ASSERT(mir->type() == MIRType_Double);
            masm.reserveStack(sizeof(double));
            Operand op(esp, 0);
            masm.fstp(op);
            masm.loadDouble(op, ReturnDoubleReg);
            masm.freeStack(sizeof(double));
        }
    }
}

void
CodeGeneratorX86::memoryBarrier(MemoryBarrierBits barrier)
{
    if (barrier & MembarStoreLoad)
        masm.storeLoadFence();
}

void
CodeGeneratorX86::loadSimd(Scalar::Type type, unsigned numElems, const Operand& srcAddr,
                           FloatRegister out)
{
    switch (type) {
      case Scalar::Float32x4: {
        switch (numElems) {
          // In memory-to-register mode, movss zeroes out the high lanes.
          case 1: masm.vmovssWithPatch(srcAddr, out); break;
          // See comment above, which also applies to movsd.
          case 2: masm.vmovsdWithPatch(srcAddr, out); break;
          case 4: masm.vmovupsWithPatch(srcAddr, out); break;
          default: MOZ_CRASH("unexpected size for partial load");
        }
        break;
      }
      case Scalar::Int32x4: {
        switch (numElems) {
          // In memory-to-register mode, movd zeroes out the high lanes.
          case 1: masm.vmovdWithPatch(srcAddr, out); break;
          // See comment above, which also applies to movq.
          case 2: masm.vmovqWithPatch(srcAddr, out); break;
          case 4: masm.vmovdquWithPatch(srcAddr, out); break;
          default: MOZ_CRASH("unexpected size for partial load");
        }
        break;
      }
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Int16:
      case Scalar::Uint16:
      case Scalar::Int32:
      case Scalar::Uint32:
      case Scalar::Float32:
      case Scalar::Float64:
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("should only handle SIMD types");
    }
}

void
CodeGeneratorX86::emitSimdLoad(LAsmJSLoadHeap* ins)
{
    const MAsmJSLoadHeap* mir = ins->mir();
    Scalar::Type type = mir->accessType();
    FloatRegister out = ToFloatRegister(ins->output());
    const LAllocation* ptr = ins->ptr();
    Operand srcAddr = ptr->isBogus()
                      ? Operand(PatchedAbsoluteAddress(mir->offset()))
                      : Operand(ToRegister(ptr), mir->offset());

    uint32_t maybeCmpOffset = wasm::HeapAccess::NoLengthCheck;
    if (gen->needsAsmJSBoundsCheckBranch(mir))
        maybeCmpOffset = emitAsmJSBoundsCheckBranch(mir, mir, ToRegister(ptr),
                                                    masm.asmOnOutOfBoundsLabel());

    unsigned numElems = mir->numSimdElems();
    if (numElems == 3) {
        MOZ_ASSERT(type == Scalar::Int32x4 || type == Scalar::Float32x4);

        Operand srcAddrZ =
            ptr->isBogus()
            ? Operand(PatchedAbsoluteAddress(2 * sizeof(float) + mir->offset()))
            : Operand(ToRegister(ptr), 2 * sizeof(float) + mir->offset());

        // Load XY
        uint32_t before = masm.size();
        loadSimd(type, 2, srcAddr, out);
        uint32_t after = masm.size();
        masm.append(wasm::HeapAccess(before, after, maybeCmpOffset));

        // Load Z (W is zeroed)
        // This is still in bounds, as we've checked with a manual bounds check
        // or we had enough space for sure when removing the bounds check.
        before = after;
        loadSimd(type, 1, srcAddrZ, ScratchSimd128Reg);
        after = masm.size();
        masm.append(wasm::HeapAccess(before, after));

        // Move ZW atop XY
        masm.vmovlhps(ScratchSimd128Reg, out, out);
    } else {
        uint32_t before = masm.size();
        loadSimd(type, numElems, srcAddr, out);
        uint32_t after = masm.size();
        masm.append(wasm::HeapAccess(before, after, maybeCmpOffset));
    }

    if (maybeCmpOffset != wasm::HeapAccess::NoLengthCheck)
        cleanupAfterAsmJSBoundsCheckBranch(mir, ToRegister(ptr));
}

void
CodeGeneratorX86::visitAsmJSLoadHeap(LAsmJSLoadHeap* ins)
{
    const MAsmJSLoadHeap* mir = ins->mir();
    Scalar::Type accessType = mir->accessType();

    if (Scalar::isSimdType(accessType))
        return emitSimdLoad(ins);

    const LAllocation* ptr = ins->ptr();
    const LDefinition* out = ins->output();
    Operand srcAddr = ptr->isBogus()
                      ? Operand(PatchedAbsoluteAddress(mir->offset()))
                      : Operand(ToRegister(ptr), mir->offset());

    memoryBarrier(mir->barrierBefore());
    OutOfLineLoadTypedArrayOutOfBounds* ool = nullptr;
    uint32_t maybeCmpOffset = wasm::HeapAccess::NoLengthCheck;
    if (gen->needsAsmJSBoundsCheckBranch(mir)) {
        Label* jumpTo = nullptr;
        if (mir->isAtomicAccess()) {
            jumpTo = masm.asmOnOutOfBoundsLabel();
        } else {
            ool = new(alloc()) OutOfLineLoadTypedArrayOutOfBounds(ToAnyRegister(out), accessType);
            addOutOfLineCode(ool, mir);
            jumpTo = ool->entry();
        }
        maybeCmpOffset = emitAsmJSBoundsCheckBranch(mir, mir, ToRegister(ptr), jumpTo);
    }

    uint32_t before = masm.size();
    load(accessType, srcAddr, out);
    uint32_t after = masm.size();
    if (ool) {
        cleanupAfterAsmJSBoundsCheckBranch(mir, ToRegister(ptr));
        masm.bind(ool->rejoin());
    }
    memoryBarrier(mir->barrierAfter());
    masm.append(wasm::HeapAccess(before, after, maybeCmpOffset));
}

void
CodeGeneratorX86::store(Scalar::Type accessType, const LAllocation* value, const Operand& dstAddr)
{
    switch (accessType) {
      case Scalar::Int8:
      case Scalar::Uint8Clamped:
      case Scalar::Uint8:        masm.movbWithPatch(ToRegister(value), dstAddr); break;
      case Scalar::Int16:
      case Scalar::Uint16:       masm.movwWithPatch(ToRegister(value), dstAddr); break;
      case Scalar::Int32:
      case Scalar::Uint32:       masm.movlWithPatch(ToRegister(value), dstAddr); break;
      case Scalar::Float32:      masm.vmovssWithPatch(ToFloatRegister(value), dstAddr); break;
      case Scalar::Float64:      masm.vmovsdWithPatch(ToFloatRegister(value), dstAddr); break;
      case Scalar::Float32x4:
      case Scalar::Int32x4:      MOZ_CRASH("SIMD stores should be handled in emitSimdStore");
      case Scalar::MaxTypedArrayViewType: MOZ_CRASH("unexpected type");
    }
}

void
CodeGeneratorX86::visitStoreTypedArrayElementStatic(LStoreTypedArrayElementStatic* ins)
{
    MStoreTypedArrayElementStatic* mir = ins->mir();
    Scalar::Type accessType = mir->accessType();
    Register ptr = ToRegister(ins->ptr());
    const LAllocation* value = ins->value();
    uint32_t offset = mir->offset();

    if (!mir->needsBoundsCheck()) {
        Operand dstAddr(ptr, int32_t(mir->base().asValue()) + int32_t(offset));
        store(accessType, value, dstAddr);
        return;
    }

    MOZ_ASSERT(offset == 0);
    masm.cmpPtr(ptr, ImmWord(mir->length()));
    Label rejoin;
    masm.j(Assembler::AboveOrEqual, &rejoin);

    Operand dstAddr(ptr, int32_t(mir->base().asValue()));
    store(accessType, value, dstAddr);
    masm.bind(&rejoin);
}

void
CodeGeneratorX86::storeSimd(Scalar::Type type, unsigned numElems, FloatRegister in,
                            const Operand& dstAddr)
{
    switch (type) {
      case Scalar::Float32x4: {
        switch (numElems) {
          // In memory-to-register mode, movss zeroes out the high lanes.
          case 1: masm.vmovssWithPatch(in, dstAddr); break;
          // See comment above, which also applies to movsd.
          case 2: masm.vmovsdWithPatch(in, dstAddr); break;
          case 4: masm.vmovupsWithPatch(in, dstAddr); break;
          default: MOZ_CRASH("unexpected size for partial load");
        }
        break;
      }
      case Scalar::Int32x4: {
        switch (numElems) {
          // In memory-to-register mode, movd zeroes out the high lanes.
          case 1: masm.vmovdWithPatch(in, dstAddr); break;
          // See comment above, which also applies to movsd.
          case 2: masm.vmovqWithPatch(in, dstAddr); break;
          case 4: masm.vmovdquWithPatch(in, dstAddr); break;
          default: MOZ_CRASH("unexpected size for partial load");
        }
        break;
      }
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Int16:
      case Scalar::Uint16:
      case Scalar::Int32:
      case Scalar::Uint32:
      case Scalar::Float32:
      case Scalar::Float64:
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("should only handle SIMD types");
    }
}

void
CodeGeneratorX86::emitSimdStore(LAsmJSStoreHeap* ins)
{
    const MAsmJSStoreHeap* mir = ins->mir();
    Scalar::Type type = mir->accessType();
    FloatRegister in = ToFloatRegister(ins->value());
    const LAllocation* ptr = ins->ptr();
    Operand dstAddr = ptr->isBogus()
                      ? Operand(PatchedAbsoluteAddress(mir->offset()))
                      : Operand(ToRegister(ptr), mir->offset());

    uint32_t maybeCmpOffset = wasm::HeapAccess::NoLengthCheck;
    if (gen->needsAsmJSBoundsCheckBranch(mir))
        maybeCmpOffset = emitAsmJSBoundsCheckBranch(mir, mir, ToRegister(ptr),
                                                    masm.asmOnOutOfBoundsLabel());

    unsigned numElems = mir->numSimdElems();
    if (numElems == 3) {
        MOZ_ASSERT(type == Scalar::Int32x4 || type == Scalar::Float32x4);

        Operand dstAddrZ =
            ptr->isBogus()
            ? Operand(PatchedAbsoluteAddress(2 * sizeof(float) + mir->offset()))
            : Operand(ToRegister(ptr), 2 * sizeof(float) + mir->offset());

        // Store XY
        uint32_t before = masm.size();
        storeSimd(type, 2, in, dstAddr);
        uint32_t after = masm.size();
        masm.append(wasm::HeapAccess(before, after, maybeCmpOffset));

        masm.vmovhlps(in, ScratchSimd128Reg, ScratchSimd128Reg);

        // Store Z (W is zeroed)
        // This is still in bounds, as we've checked with a manual bounds check
        // or we had enough space for sure when removing the bounds check.
        before = masm.size();
        storeSimd(type, 1, ScratchSimd128Reg, dstAddrZ);
        after = masm.size();
        masm.append(wasm::HeapAccess(before, after));
    } else {
        uint32_t before = masm.size();
        storeSimd(type, numElems, in, dstAddr);
        uint32_t after = masm.size();
        masm.append(wasm::HeapAccess(before, after, maybeCmpOffset));
    }

    if (maybeCmpOffset != wasm::HeapAccess::NoLengthCheck)
        cleanupAfterAsmJSBoundsCheckBranch(mir, ToRegister(ptr));
}

void
CodeGeneratorX86::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins)
{
    const MAsmJSStoreHeap* mir = ins->mir();
    Scalar::Type accessType = mir->accessType();

    if (Scalar::isSimdType(accessType))
        return emitSimdStore(ins);

    const LAllocation* value = ins->value();
    const LAllocation* ptr = ins->ptr();
    Operand dstAddr = ptr->isBogus()
                      ? Operand(PatchedAbsoluteAddress(mir->offset()))
                      : Operand(ToRegister(ptr), mir->offset());

    memoryBarrier(mir->barrierBefore());
    Label* rejoin = nullptr;
    uint32_t maybeCmpOffset = wasm::HeapAccess::NoLengthCheck;
    if (gen->needsAsmJSBoundsCheckBranch(mir)) {
        Label* jumpTo = nullptr;
        if (mir->isAtomicAccess())
            jumpTo = masm.asmOnOutOfBoundsLabel();
        else
            rejoin = jumpTo = alloc().lifoAlloc()->newInfallible<Label>();
        maybeCmpOffset = emitAsmJSBoundsCheckBranch(mir, mir, ToRegister(ptr), jumpTo);
    }

    uint32_t before = masm.size();
    store(accessType, value, dstAddr);
    uint32_t after = masm.size();
    if (rejoin) {
        cleanupAfterAsmJSBoundsCheckBranch(mir, ToRegister(ptr));
        masm.bind(rejoin);
    }
    memoryBarrier(mir->barrierAfter());
    masm.append(wasm::HeapAccess(before, after, maybeCmpOffset));
}

void
CodeGeneratorX86::visitAsmJSCompareExchangeHeap(LAsmJSCompareExchangeHeap* ins)
{
    MAsmJSCompareExchangeHeap* mir = ins->mir();
    Scalar::Type accessType = mir->accessType();
    Register ptrReg = ToRegister(ins->ptr());
    Register oldval = ToRegister(ins->oldValue());
    Register newval = ToRegister(ins->newValue());
    Register addrTemp = ToRegister(ins->addrTemp());

    asmJSAtomicComputeAddress(addrTemp, ptrReg, mir->needsBoundsCheck(), mir->offset(),
                              mir->endOffset());

    Address memAddr(addrTemp, mir->offset());
    masm.compareExchangeToTypedIntArray(accessType == Scalar::Uint32 ? Scalar::Int32 : accessType,
                                        memAddr,
                                        oldval,
                                        newval,
                                        InvalidReg,
                                        ToAnyRegister(ins->output()));
}

// Perform bounds checking on the access if necessary; if it fails,
// jump to out-of-line code that throws.  If the bounds check passes,
// set up the heap address in addrTemp.

void
CodeGeneratorX86::asmJSAtomicComputeAddress(Register addrTemp, Register ptrReg, bool boundsCheck,
                                            int32_t offset, int32_t endOffset)
{
    uint32_t maybeCmpOffset = wasm::HeapAccess::NoLengthCheck;

    if (boundsCheck) {
        maybeCmpOffset = masm.cmp32WithPatch(ptrReg, Imm32(-endOffset)).offset();
        masm.j(Assembler::Above, masm.asmOnOutOfBoundsLabel());
    }

    // Add in the actual heap pointer explicitly, to avoid opening up
    // the abstraction that is atomicBinopToTypedIntArray at this time.
    masm.movl(ptrReg, addrTemp);
    uint32_t before = masm.size();
    masm.addlWithPatch(Imm32(offset), addrTemp);
    uint32_t after = masm.size();
    masm.append(wasm::HeapAccess(before, after, maybeCmpOffset));
}

void
CodeGeneratorX86::visitAsmJSAtomicExchangeHeap(LAsmJSAtomicExchangeHeap* ins)
{
    MAsmJSAtomicExchangeHeap* mir = ins->mir();
    Scalar::Type accessType = mir->accessType();
    Register ptrReg = ToRegister(ins->ptr());
    Register value = ToRegister(ins->value());
    Register addrTemp = ToRegister(ins->addrTemp());

    asmJSAtomicComputeAddress(addrTemp, ptrReg, mir->needsBoundsCheck(), mir->offset(),
                              mir->endOffset());

    Address memAddr(addrTemp, mir->offset());
    masm.atomicExchangeToTypedIntArray(accessType == Scalar::Uint32 ? Scalar::Int32 : accessType,
                                       memAddr,
                                       value,
                                       InvalidReg,
                                       ToAnyRegister(ins->output()));
}

void
CodeGeneratorX86::visitAsmJSAtomicBinopHeap(LAsmJSAtomicBinopHeap* ins)
{
    MAsmJSAtomicBinopHeap* mir = ins->mir();
    Scalar::Type accessType = mir->accessType();
    Register ptrReg = ToRegister(ins->ptr());
    Register temp = ins->temp()->isBogusTemp() ? InvalidReg : ToRegister(ins->temp());
    Register addrTemp = ToRegister(ins->addrTemp());
    const LAllocation* value = ins->value();
    AtomicOp op = mir->operation();

    asmJSAtomicComputeAddress(addrTemp, ptrReg, mir->needsBoundsCheck(), mir->offset(),
                              mir->endOffset());

    Address memAddr(addrTemp, mir->offset());
    if (value->isConstant()) {
        atomicBinopToTypedIntArray(op, accessType == Scalar::Uint32 ? Scalar::Int32 : accessType,
                                   Imm32(ToInt32(value)),
                                   memAddr,
                                   temp,
                                   InvalidReg,
                                   ToAnyRegister(ins->output()));
    } else {
        atomicBinopToTypedIntArray(op, accessType == Scalar::Uint32 ? Scalar::Int32 : accessType,
                                   ToRegister(value),
                                   memAddr,
                                   temp,
                                   InvalidReg,
                                   ToAnyRegister(ins->output()));
    }
}

void
CodeGeneratorX86::visitAsmJSAtomicBinopHeapForEffect(LAsmJSAtomicBinopHeapForEffect* ins)
{
    MAsmJSAtomicBinopHeap* mir = ins->mir();
    Scalar::Type accessType = mir->accessType();
    Register ptrReg = ToRegister(ins->ptr());
    Register addrTemp = ToRegister(ins->addrTemp());
    const LAllocation* value = ins->value();
    AtomicOp op = mir->operation();

    MOZ_ASSERT(!mir->hasUses());

    asmJSAtomicComputeAddress(addrTemp, ptrReg, mir->needsBoundsCheck(), mir->offset(),
                              mir->endOffset());

    Address memAddr(addrTemp, mir->offset());
    if (value->isConstant())
        atomicBinopToTypedIntArray(op, accessType, Imm32(ToInt32(value)), memAddr);
    else
        atomicBinopToTypedIntArray(op, accessType, ToRegister(value), memAddr);
}

void
CodeGeneratorX86::visitAsmJSLoadGlobalVar(LAsmJSLoadGlobalVar* ins)
{
    MAsmJSLoadGlobalVar* mir = ins->mir();
    MIRType type = mir->type();
    MOZ_ASSERT(IsNumberType(type) || IsSimdType(type));

    CodeOffset label;
    switch (type) {
      case MIRType_Int32:
        label = masm.movlWithPatch(PatchedAbsoluteAddress(), ToRegister(ins->output()));
        break;
      case MIRType_Float32:
        label = masm.vmovssWithPatch(PatchedAbsoluteAddress(), ToFloatRegister(ins->output()));
        break;
      case MIRType_Double:
        label = masm.vmovsdWithPatch(PatchedAbsoluteAddress(), ToFloatRegister(ins->output()));
        break;
      // Aligned access: code is aligned on PageSize + there is padding
      // before the global data section.
      case MIRType_Int32x4:
        label = masm.vmovdqaWithPatch(PatchedAbsoluteAddress(), ToFloatRegister(ins->output()));
        break;
      case MIRType_Float32x4:
        label = masm.vmovapsWithPatch(PatchedAbsoluteAddress(), ToFloatRegister(ins->output()));
        break;
      default:
        MOZ_CRASH("unexpected type in visitAsmJSLoadGlobalVar");
    }
    masm.append(AsmJSGlobalAccess(label, mir->globalDataOffset()));
}

void
CodeGeneratorX86::visitAsmJSStoreGlobalVar(LAsmJSStoreGlobalVar* ins)
{
    MAsmJSStoreGlobalVar* mir = ins->mir();

    MIRType type = mir->value()->type();
    MOZ_ASSERT(IsNumberType(type) || IsSimdType(type));

    CodeOffset label;
    switch (type) {
      case MIRType_Int32:
        label = masm.movlWithPatch(ToRegister(ins->value()), PatchedAbsoluteAddress());
        break;
      case MIRType_Float32:
        label = masm.vmovssWithPatch(ToFloatRegister(ins->value()), PatchedAbsoluteAddress());
        break;
      case MIRType_Double:
        label = masm.vmovsdWithPatch(ToFloatRegister(ins->value()), PatchedAbsoluteAddress());
        break;
      // Aligned access: code is aligned on PageSize + there is padding
      // before the global data section.
      case MIRType_Int32x4:
        label = masm.vmovdqaWithPatch(ToFloatRegister(ins->value()), PatchedAbsoluteAddress());
        break;
      case MIRType_Float32x4:
        label = masm.vmovapsWithPatch(ToFloatRegister(ins->value()), PatchedAbsoluteAddress());
        break;
      default:
        MOZ_CRASH("unexpected type in visitAsmJSStoreGlobalVar");
    }
    masm.append(AsmJSGlobalAccess(label, mir->globalDataOffset()));
}

void
CodeGeneratorX86::visitAsmJSLoadFuncPtr(LAsmJSLoadFuncPtr* ins)
{
    MAsmJSLoadFuncPtr* mir = ins->mir();

    Register index = ToRegister(ins->index());
    Register out = ToRegister(ins->output());
    CodeOffset label = masm.movlWithPatch(PatchedAbsoluteAddress(), index, TimesFour, out);
    masm.append(AsmJSGlobalAccess(label, mir->globalDataOffset()));
}

void
CodeGeneratorX86::visitAsmJSLoadFFIFunc(LAsmJSLoadFFIFunc* ins)
{
    MAsmJSLoadFFIFunc* mir = ins->mir();

    Register out = ToRegister(ins->output());
    CodeOffset label = masm.movlWithPatch(PatchedAbsoluteAddress(), out);
    masm.append(AsmJSGlobalAccess(label, mir->globalDataOffset()));
}

namespace js {
namespace jit {

class OutOfLineTruncate : public OutOfLineCodeBase<CodeGeneratorX86>
{
    LTruncateDToInt32* ins_;

  public:
    OutOfLineTruncate(LTruncateDToInt32* ins)
      : ins_(ins)
    { }

    void accept(CodeGeneratorX86* codegen) {
        codegen->visitOutOfLineTruncate(this);
    }
    LTruncateDToInt32* ins() const {
        return ins_;
    }
};

class OutOfLineTruncateFloat32 : public OutOfLineCodeBase<CodeGeneratorX86>
{
    LTruncateFToInt32* ins_;

  public:
    OutOfLineTruncateFloat32(LTruncateFToInt32* ins)
      : ins_(ins)
    { }

    void accept(CodeGeneratorX86* codegen) {
        codegen->visitOutOfLineTruncateFloat32(this);
    }
    LTruncateFToInt32* ins() const {
        return ins_;
    }
};

} // namespace jit
} // namespace js

void
CodeGeneratorX86::visitTruncateDToInt32(LTruncateDToInt32* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    OutOfLineTruncate* ool = new(alloc()) OutOfLineTruncate(ins);
    addOutOfLineCode(ool, ins->mir());

    masm.branchTruncateDouble(input, output, ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGeneratorX86::visitTruncateFToInt32(LTruncateFToInt32* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    OutOfLineTruncateFloat32* ool = new(alloc()) OutOfLineTruncateFloat32(ins);
    addOutOfLineCode(ool, ins->mir());

    masm.branchTruncateFloat32(input, output, ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGeneratorX86::visitOutOfLineTruncate(OutOfLineTruncate* ool)
{
    LTruncateDToInt32* ins = ool->ins();
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    Label fail;

    if (Assembler::HasSSE3()) {
        // Push double.
        masm.subl(Imm32(sizeof(double)), esp);
        masm.storeDouble(input, Operand(esp, 0));

        static const uint32_t EXPONENT_MASK = 0x7ff00000;
        static const uint32_t EXPONENT_SHIFT = FloatingPoint<double>::kExponentShift - 32;
        static const uint32_t TOO_BIG_EXPONENT = (FloatingPoint<double>::kExponentBias + 63)
                                                 << EXPONENT_SHIFT;

        // Check exponent to avoid fp exceptions.
        Label failPopDouble;
        masm.load32(Address(esp, 4), output);
        masm.and32(Imm32(EXPONENT_MASK), output);
        masm.branch32(Assembler::GreaterThanOrEqual, output, Imm32(TOO_BIG_EXPONENT), &failPopDouble);

        // Load double, perform 64-bit truncation.
        masm.fld(Operand(esp, 0));
        masm.fisttp(Operand(esp, 0));

        // Load low word, pop double and jump back.
        masm.load32(Address(esp, 0), output);
        masm.addl(Imm32(sizeof(double)), esp);
        masm.jump(ool->rejoin());

        masm.bind(&failPopDouble);
        masm.addl(Imm32(sizeof(double)), esp);
        masm.jump(&fail);
    } else {
        FloatRegister temp = ToFloatRegister(ins->tempFloat());

        // Try to convert doubles representing integers within 2^32 of a signed
        // integer, by adding/subtracting 2^32 and then trying to convert to int32.
        // This has to be an exact conversion, as otherwise the truncation works
        // incorrectly on the modified value.
        masm.zeroDouble(ScratchDoubleReg);
        masm.vucomisd(ScratchDoubleReg, input);
        masm.j(Assembler::Parity, &fail);

        {
            Label positive;
            masm.j(Assembler::Above, &positive);

            masm.loadConstantDouble(4294967296.0, temp);
            Label skip;
            masm.jmp(&skip);

            masm.bind(&positive);
            masm.loadConstantDouble(-4294967296.0, temp);
            masm.bind(&skip);
        }

        masm.addDouble(input, temp);
        masm.vcvttsd2si(temp, output);
        masm.vcvtsi2sd(output, ScratchDoubleReg, ScratchDoubleReg);

        masm.vucomisd(ScratchDoubleReg, temp);
        masm.j(Assembler::Parity, &fail);
        masm.j(Assembler::Equal, ool->rejoin());
    }

    masm.bind(&fail);
    {
        saveVolatile(output);

        masm.setupUnalignedABICall(output);
        masm.passABIArg(input, MoveOp::DOUBLE);
        if (gen->compilingAsmJS())
            masm.callWithABI(wasm::SymbolicAddress::ToInt32);
        else
            masm.callWithABI(BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32));
        masm.storeCallResult(output);

        restoreVolatile(output);
    }

    masm.jump(ool->rejoin());
}

void
CodeGeneratorX86::visitOutOfLineTruncateFloat32(OutOfLineTruncateFloat32* ool)
{
    LTruncateFToInt32* ins = ool->ins();
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    Label fail;

    if (Assembler::HasSSE3()) {
        // Push float32, but subtracts 64 bits so that the value popped by fisttp fits
        masm.subl(Imm32(sizeof(uint64_t)), esp);
        masm.storeFloat32(input, Operand(esp, 0));

        static const uint32_t EXPONENT_MASK = FloatingPoint<float>::kExponentBits;
        static const uint32_t EXPONENT_SHIFT = FloatingPoint<float>::kExponentShift;
        // Integers are still 64 bits long, so we can still test for an exponent > 63.
        static const uint32_t TOO_BIG_EXPONENT = (FloatingPoint<float>::kExponentBias + 63)
                                                 << EXPONENT_SHIFT;

        // Check exponent to avoid fp exceptions.
        Label failPopFloat;
        masm.movl(Operand(esp, 0), output);
        masm.and32(Imm32(EXPONENT_MASK), output);
        masm.branch32(Assembler::GreaterThanOrEqual, output, Imm32(TOO_BIG_EXPONENT), &failPopFloat);

        // Load float, perform 32-bit truncation.
        masm.fld32(Operand(esp, 0));
        masm.fisttp(Operand(esp, 0));

        // Load low word, pop 64bits and jump back.
        masm.load32(Address(esp, 0), output);
        masm.addl(Imm32(sizeof(uint64_t)), esp);
        masm.jump(ool->rejoin());

        masm.bind(&failPopFloat);
        masm.addl(Imm32(sizeof(uint64_t)), esp);
        masm.jump(&fail);
    } else {
        FloatRegister temp = ToFloatRegister(ins->tempFloat());

        // Try to convert float32 representing integers within 2^32 of a signed
        // integer, by adding/subtracting 2^32 and then trying to convert to int32.
        // This has to be an exact conversion, as otherwise the truncation works
        // incorrectly on the modified value.
        masm.zeroFloat32(ScratchFloat32Reg);
        masm.vucomiss(ScratchFloat32Reg, input);
        masm.j(Assembler::Parity, &fail);

        {
            Label positive;
            masm.j(Assembler::Above, &positive);

            masm.loadConstantFloat32(4294967296.f, temp);
            Label skip;
            masm.jmp(&skip);

            masm.bind(&positive);
            masm.loadConstantFloat32(-4294967296.f, temp);
            masm.bind(&skip);
        }

        masm.addFloat32(input, temp);
        masm.vcvttss2si(temp, output);
        masm.vcvtsi2ss(output, ScratchFloat32Reg, ScratchFloat32Reg);

        masm.vucomiss(ScratchFloat32Reg, temp);
        masm.j(Assembler::Parity, &fail);
        masm.j(Assembler::Equal, ool->rejoin());
    }

    masm.bind(&fail);
    {
        saveVolatile(output);

        masm.push(input);
        masm.setupUnalignedABICall(output);
        masm.vcvtss2sd(input, input, input);
        masm.passABIArg(input.asDouble(), MoveOp::DOUBLE);

        if (gen->compilingAsmJS())
            masm.callWithABI(wasm::SymbolicAddress::ToInt32);
        else
            masm.callWithABI(BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32));

        masm.storeCallResult(output);
        masm.pop(input);

        restoreVolatile(output);
    }

    masm.jump(ool->rejoin());
}
