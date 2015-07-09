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
CodeGeneratorX86::visitCompareV(LCompareV* lir)
{
    MCompare* mir = lir->mir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareV::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareV::RhsInput);
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
CodeGeneratorX86::visitCompareVAndBranch(LCompareVAndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareVAndBranch::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareVAndBranch::RhsInput);

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

template<typename T>
void
CodeGeneratorX86::load(Scalar::Type vt, const T& srcAddr, const LDefinition* out)
{
    switch (vt) {
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

template<typename T>
void
CodeGeneratorX86::loadAndNoteViewTypeElement(Scalar::Type vt, const T& srcAddr,
                                             const LDefinition* out)
{
    uint32_t before = masm.size();
    load(vt, srcAddr, out);
    uint32_t after = masm.size();
    masm.append(AsmJSHeapAccess(before, after, vt, ToAnyRegister(out)));
}

void
CodeGeneratorX86::visitLoadTypedArrayElementStatic(LLoadTypedArrayElementStatic* ins)
{
    const MLoadTypedArrayElementStatic* mir = ins->mir();
    Scalar::Type vt = mir->accessType();
    MOZ_ASSERT_IF(vt == Scalar::Float32, mir->type() == MIRType_Float32);

    Register ptr = ToRegister(ins->ptr());
    const LDefinition* out = ins->output();
    OutOfLineLoadTypedArrayOutOfBounds* ool = nullptr;
    uint32_t offset = mir->offset();

    if (mir->needsBoundsCheck()) {
        MOZ_ASSERT(offset == 0);
        if (!mir->fallible()) {
            ool = new(alloc()) OutOfLineLoadTypedArrayOutOfBounds(ToAnyRegister(out), vt);
            addOutOfLineCode(ool, ins->mir());
        }

        masm.cmpPtr(ptr, ImmWord(mir->length()));
        if (ool)
            masm.j(Assembler::AboveOrEqual, ool->entry());
        else
            bailoutIf(Assembler::AboveOrEqual, ins->snapshot());
    }

    Address srcAddr(ptr, int32_t(mir->base()) + int32_t(offset));
    load(vt, srcAddr, out);
    if (vt == Scalar::Float64)
        masm.canonicalizeDouble(ToFloatRegister(out));
    if (vt == Scalar::Float32)
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

template<typename T>
void
CodeGeneratorX86::loadSimd(Scalar::Type type, unsigned numElems, T srcAddr, FloatRegister out)
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
          // See comment above, which also applies to movsd.
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
CodeGeneratorX86::emitSimdLoad(Scalar::Type type, unsigned numElems, const LAllocation* ptr,
                               FloatRegister out, bool needsBoundsCheck /* = false */,
                               Label* oobLabel /* = nullptr */)
{
    if (ptr->isConstant()) {
        MOZ_ASSERT(!needsBoundsCheck);

        if (numElems == 3) {
            MOZ_ASSERT(type == Scalar::Int32x4 || type == Scalar::Float32x4);

            // Load XY
            emitSimdLoad(type, 2, ptr, out);

            // Load Z (W is zeroed)
            // This add won't overflow, as we've checked that we have at least
            // room for loading 4 elements during asm.js validation.
            PatchedAbsoluteAddress srcAddr((void*) (ptr->toConstant()->toInt32() + 2 * sizeof(float)));
            uint32_t before = masm.size();
            loadSimd(type, 1, srcAddr, ScratchSimdReg);
            uint32_t after = masm.size();
            masm.append(AsmJSHeapAccess(before, after, 1, type));

            // Move ZW atop XY
            masm.vmovlhps(ScratchSimdReg, out, out);
            return;
        }

        PatchedAbsoluteAddress srcAddr((void*) ptr->toConstant()->toInt32());
        uint32_t before = masm.size();
        loadSimd(type, numElems, srcAddr, out);
        uint32_t after = masm.size();
        masm.append(AsmJSHeapAccess(before, after, numElems, type));
        return;
    }

    Register ptrReg = ToRegister(ptr);
    uint32_t maybeCmpOffset = AsmJSHeapAccess::NoLengthCheck;
    if (needsBoundsCheck) {
        maybeCmpOffset = masm.cmp32WithPatch(ptrReg, Imm32(0)).offset();
        masm.j(Assembler::AboveOrEqual, oobLabel); // Throws RangeError
    }

    uint32_t before = masm.size();
    if (numElems == 3) {
        MOZ_ASSERT(type == Scalar::Int32x4 || type == Scalar::Float32x4);

        // Load XY
        Address addr(ptrReg, 0);
        before = masm.size();
        loadSimd(type, 2, addr, out);
        uint32_t after = masm.size();
        masm.append(AsmJSHeapAccess(before, after, 3, type, maybeCmpOffset));

        // Load Z (W is zeroed)
        // This is still in bounds, as we've checked with a manual bounds check
        // or we had enough space for sure when removing the bounds check.
        Address shiftedAddr(ptrReg, 2 * sizeof(float));
        before = after;
        loadSimd(type, 1, shiftedAddr, ScratchSimdReg);
        after = masm.size();
        masm.append(AsmJSHeapAccess(before, after, 1, type));

        // Move ZW atop XY
        masm.vmovlhps(ScratchSimdReg, out, out);
        return;
    }

    Address addr(ptrReg, 0);
    loadSimd(type, numElems, addr, out);
    uint32_t after = masm.size();
    masm.append(AsmJSHeapAccess(before, after, numElems, type, maybeCmpOffset));
}

void
CodeGeneratorX86::visitAsmJSLoadHeap(LAsmJSLoadHeap* ins)
{
    const MAsmJSLoadHeap* mir = ins->mir();
    Scalar::Type accessType = mir->accessType();
    const LAllocation* ptr = ins->ptr();
    const LDefinition* out = ins->output();

    if (Scalar::isSimdType(accessType)) {
        return emitSimdLoad(accessType, mir->numSimdElems(), ptr, ToFloatRegister(out),
                            mir->needsBoundsCheck(), mir->outOfBoundsLabel());
    }

    memoryBarrier(ins->mir()->barrierBefore());

    if (ptr->isConstant()) {
        // The constant displacement still needs to be added to the as-yet-unknown
        // base address of the heap. For now, embed the displacement as an
        // immediate in the instruction. This displacement will fixed up when the
        // base address is known during dynamic linking (AsmJSModule::initHeap).
        PatchedAbsoluteAddress srcAddr((void*) ptr->toConstant()->toInt32());
        loadAndNoteViewTypeElement(accessType, srcAddr, out);
        memoryBarrier(ins->mir()->barrierAfter());
        return;
    }

    Register ptrReg = ToRegister(ptr);
    Address srcAddr(ptrReg, 0);

    if (!mir->needsBoundsCheck()) {
        loadAndNoteViewTypeElement(accessType, srcAddr, out);
        memoryBarrier(ins->mir()->barrierAfter());
        return;
    }

    OutOfLineLoadTypedArrayOutOfBounds* ool =
        new(alloc()) OutOfLineLoadTypedArrayOutOfBounds(ToAnyRegister(out), accessType);
    CodeOffsetLabel cmp = masm.cmp32WithPatch(ptrReg, Imm32(0));
    addOutOfLineCode(ool, mir);
    masm.j(Assembler::AboveOrEqual, ool->entry());

    uint32_t before = masm.size();
    load(accessType, srcAddr, out);
    uint32_t after = masm.size();
    if (ool)
        masm.bind(ool->rejoin());
    memoryBarrier(ins->mir()->barrierAfter());
    masm.append(AsmJSHeapAccess(before, after, accessType, ToAnyRegister(out), cmp.offset()));
}

template<typename T>
void
CodeGeneratorX86::store(Scalar::Type vt, const LAllocation* value, const T& dstAddr)
{
    switch (vt) {
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

template<typename T>
void
CodeGeneratorX86::storeAndNoteViewTypeElement(Scalar::Type vt, const LAllocation* value,
                                              const T& dstAddr)
{
    uint32_t before = masm.size();
    store(vt, value, dstAddr);
    uint32_t after = masm.size();
    masm.append(AsmJSHeapAccess(before, after, vt));
}

void
CodeGeneratorX86::visitStoreTypedArrayElementStatic(LStoreTypedArrayElementStatic* ins)
{
    MStoreTypedArrayElementStatic* mir = ins->mir();
    Scalar::Type vt = mir->accessType();
    Register ptr = ToRegister(ins->ptr());
    const LAllocation* value = ins->value();
    uint32_t offset = mir->offset();

    if (!mir->needsBoundsCheck()) {
        Address dstAddr(ptr, int32_t(mir->base()) + int32_t(offset));
        store(vt, value, dstAddr);
        return;
    }

    MOZ_ASSERT(offset == 0);
    masm.cmpPtr(ptr, ImmWord(mir->length()));
    Label rejoin;
    masm.j(Assembler::AboveOrEqual, &rejoin);

    Address dstAddr(ptr, (int32_t) mir->base());
    store(vt, value, dstAddr);
    masm.bind(&rejoin);
}

template<typename T>
void
CodeGeneratorX86::storeSimd(Scalar::Type type, unsigned numElems, FloatRegister in, T destAddr)
{
    switch (type) {
      case Scalar::Float32x4: {
        switch (numElems) {
          // In memory-to-register mode, movss zeroes out the high lanes.
          case 1: masm.vmovssWithPatch(in, destAddr); break;
          // See comment above, which also applies to movsd.
          case 2: masm.vmovsdWithPatch(in, destAddr); break;
          case 4: masm.vmovupsWithPatch(in, destAddr); break;
          default: MOZ_CRASH("unexpected size for partial load");
        }
        break;
      }
      case Scalar::Int32x4: {
        switch (numElems) {
          // In memory-to-register mode, movd zeroes destAddr the high lanes.
          case 1: masm.vmovdWithPatch(in, destAddr); break;
          // See comment above, which also applies to movsd.
          case 2: masm.vmovqWithPatch(in, destAddr); break;
          case 4: masm.vmovdquWithPatch(in, destAddr); break;
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
CodeGeneratorX86::emitSimdStore(Scalar::Type type, unsigned numElems, FloatRegister in,
                                const LAllocation* ptr, bool needsBoundsCheck /* = false */,
                                Label* oobLabel /* = nullptr */)
{
    if (ptr->isConstant()) {
        MOZ_ASSERT(!needsBoundsCheck);

        if (numElems == 3) {
            MOZ_ASSERT(type == Scalar::Int32x4 || type == Scalar::Float32x4);

            // Store XY
            emitSimdStore(type, 2, in, ptr);

            masm.vmovhlps(in, ScratchSimdReg, ScratchSimdReg);

            // Store Z
            // This add won't overflow, as we've checked that we have at least
            // room for loading 4 elements during asm.js validation.
            PatchedAbsoluteAddress dstAddr((void*) (ptr->toConstant()->toInt32() + 2 * sizeof(float)));
            uint32_t before = masm.size();
            storeSimd(type, 1, ScratchSimdReg, dstAddr);
            uint32_t after = masm.size();
            masm.append(AsmJSHeapAccess(before, after, 1, type));
            return;
        }

        PatchedAbsoluteAddress dstAddr((void*) ptr->toConstant()->toInt32());
        uint32_t before = masm.size();
        storeSimd(type, numElems, in, dstAddr);
        uint32_t after = masm.size();
        masm.append(AsmJSHeapAccess(before, after, 3, type));
        return;
    }

    Register ptrReg = ToRegister(ptr);
    uint32_t maybeCmpOffset = AsmJSHeapAccess::NoLengthCheck;
    if (needsBoundsCheck) {
        maybeCmpOffset = masm.cmp32WithPatch(ptrReg, Imm32(0)).offset();
        masm.j(Assembler::AboveOrEqual, oobLabel); // Throws RangeError
    }

    uint32_t before = masm.size();
    if (numElems == 3) {
        MOZ_ASSERT(type == Scalar::Int32x4 || type == Scalar::Float32x4);

        // Store XY
        Address addr(ptrReg, 0);
        before = masm.size();
        storeSimd(type, 2, in, addr);
        uint32_t after = masm.size();
        masm.append(AsmJSHeapAccess(before, after, 3, type, maybeCmpOffset));

        masm.vmovhlps(in, ScratchSimdReg, ScratchSimdReg);

        // Store Z (W is zeroed)
        // This is still in bounds, as we've checked with a manual bounds check
        // or we had enough space for sure when removing the bounds check.
        Address shiftedAddr(ptrReg, 2 * sizeof(float));
        before = masm.size();
        storeSimd(type, 1, ScratchSimdReg, shiftedAddr);
        after = masm.size();
        masm.append(AsmJSHeapAccess(before, after, 1, type));
        return;
    }

    Address addr(ptrReg, 0);
    storeSimd(type, numElems, in, addr);
    uint32_t after = masm.size();
    masm.append(AsmJSHeapAccess(before, after, numElems, type, maybeCmpOffset));
}

void
CodeGeneratorX86::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins)
{
    MAsmJSStoreHeap* mir = ins->mir();
    Scalar::Type vt = mir->accessType();
    const LAllocation* value = ins->value();
    const LAllocation* ptr = ins->ptr();

    if (Scalar::isSimdType(vt)) {
        return emitSimdStore(vt, mir->numSimdElems(), ToFloatRegister(value), ptr,
                             mir->needsBoundsCheck(), mir->outOfBoundsLabel());
    }

    memoryBarrier(ins->mir()->barrierBefore());

    if (ptr->isConstant()) {
        // The constant displacement still needs to be added to the as-yet-unknown
        // base address of the heap. For now, embed the displacement as an
        // immediate in the instruction. This displacement will fixed up when the
        // base address is known during dynamic linking (AsmJSModule::initHeap).
        PatchedAbsoluteAddress dstAddr((void*) ptr->toConstant()->toInt32());
        storeAndNoteViewTypeElement(vt, value, dstAddr);
        memoryBarrier(ins->mir()->barrierAfter());
        return;
    }

    Register ptrReg = ToRegister(ptr);
    Address dstAddr(ptrReg, 0);

    if (!mir->needsBoundsCheck()) {
        storeAndNoteViewTypeElement(vt, value, dstAddr);
        memoryBarrier(ins->mir()->barrierAfter());
        return;
    }

    CodeOffsetLabel cmp = masm.cmp32WithPatch(ptrReg, Imm32(0));
    Label rejoin;
    masm.j(Assembler::AboveOrEqual, &rejoin);

    uint32_t before = masm.size();
    store(vt, value, dstAddr);
    uint32_t after = masm.size();
    masm.bind(&rejoin);
    memoryBarrier(ins->mir()->barrierAfter());
    masm.append(AsmJSHeapAccess(before, after, vt, cmp.offset()));
}

void
CodeGeneratorX86::visitAsmJSCompareExchangeHeap(LAsmJSCompareExchangeHeap* ins)
{
    MAsmJSCompareExchangeHeap* mir = ins->mir();
    Scalar::Type vt = mir->accessType();
    const LAllocation* ptr = ins->ptr();
    Register oldval = ToRegister(ins->oldValue());
    Register newval = ToRegister(ins->newValue());

    MOZ_ASSERT(ptr->isRegister());
    // Set up the offset within the heap in the pointer reg.
    Register ptrReg = ToRegister(ptr);

    Label rejoin;
    uint32_t maybeCmpOffset = AsmJSHeapAccess::NoLengthCheck;

    if (mir->needsBoundsCheck()) {
        maybeCmpOffset = masm.cmp32WithPatch(ptrReg, Imm32(0)).offset();
        Label goahead;
        masm.j(Assembler::Below, &goahead);
        memoryBarrier(MembarFull);
        Register out = ToRegister(ins->output());
        masm.xorl(out,out);
        masm.jmp(&rejoin);
        masm.bind(&goahead);
    }

    // Add in the actual heap pointer explicitly, to avoid opening up
    // the abstraction that is compareExchangeToTypedIntArray at this time.
    uint32_t before = masm.size();
    masm.addlWithPatch(Imm32(0), ptrReg);
    uint32_t after = masm.size();
    masm.append(AsmJSHeapAccess(before, after, mir->accessType(), maybeCmpOffset));

    Address memAddr(ToRegister(ptr), 0);
    masm.compareExchangeToTypedIntArray(vt == Scalar::Uint32 ? Scalar::Int32 : vt,
                                        memAddr,
                                        oldval,
                                        newval,
                                        InvalidReg,
                                        ToAnyRegister(ins->output()));
    if (rejoin.used())
        masm.bind(&rejoin);
}

void
CodeGeneratorX86::visitAsmJSAtomicBinopHeap(LAsmJSAtomicBinopHeap* ins)
{
    MAsmJSAtomicBinopHeap* mir = ins->mir();
    Scalar::Type vt = mir->accessType();
    const LAllocation* ptr = ins->ptr();
    Register temp = ins->temp()->isBogusTemp() ? InvalidReg : ToRegister(ins->temp());
    const LAllocation* value = ins->value();
    AtomicOp op = mir->operation();

    MOZ_ASSERT(ptr->isRegister());
    // Set up the offset within the heap in the pointer reg.
    Register ptrReg = ToRegister(ptr);

    Label rejoin;
    uint32_t maybeCmpOffset = AsmJSHeapAccess::NoLengthCheck;

    if (mir->needsBoundsCheck()) {
        maybeCmpOffset = masm.cmp32WithPatch(ptrReg, Imm32(0)).offset();
        Label goahead;
        masm.j(Assembler::Below, &goahead);
        memoryBarrier(MembarFull);
        Register out = ToRegister(ins->output());
        masm.xorl(out,out);
        masm.jmp(&rejoin);
        masm.bind(&goahead);
    }

    // Add in the actual heap pointer explicitly, to avoid opening up
    // the abstraction that is atomicBinopToTypedIntArray at this time.
    uint32_t before = masm.size();
    masm.addlWithPatch(Imm32(0), ptrReg);
    uint32_t after = masm.size();
    masm.append(AsmJSHeapAccess(before, after, mir->accessType(), maybeCmpOffset));

    Address memAddr(ptrReg, 0);
    if (value->isConstant()) {
        masm.atomicBinopToTypedIntArray(op, vt == Scalar::Uint32 ? Scalar::Int32 : vt,
                                        Imm32(ToInt32(value)),
                                        memAddr,
                                        temp,
                                        InvalidReg,
                                        ToAnyRegister(ins->output()));
    } else {
        masm.atomicBinopToTypedIntArray(op, vt == Scalar::Uint32 ? Scalar::Int32 : vt,
                                        ToRegister(value),
                                        memAddr,
                                        temp,
                                        InvalidReg,
                                        ToAnyRegister(ins->output()));
    }
    if (rejoin.used())
        masm.bind(&rejoin);
}

void
CodeGeneratorX86::visitAsmJSLoadGlobalVar(LAsmJSLoadGlobalVar* ins)
{
    MAsmJSLoadGlobalVar* mir = ins->mir();
    MIRType type = mir->type();
    MOZ_ASSERT(IsNumberType(type) || IsSimdType(type));

    CodeOffsetLabel label;
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

    CodeOffsetLabel label;
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
    CodeOffsetLabel label = masm.movlWithPatch(PatchedAbsoluteAddress(), index, TimesFour, out);
    masm.append(AsmJSGlobalAccess(label, mir->globalDataOffset()));
}

void
CodeGeneratorX86::visitAsmJSLoadFFIFunc(LAsmJSLoadFFIFunc* ins)
{
    MAsmJSLoadFFIFunc* mir = ins->mir();

    Register out = ToRegister(ins->output());
    CodeOffsetLabel label = masm.movlWithPatch(PatchedAbsoluteAddress(), out);
    masm.append(AsmJSGlobalAccess(label, mir->globalDataOffset()));
}

void
DispatchIonCache::initializeAddCacheState(LInstruction* ins, AddCacheState* addState)
{
    // On x86, where there is no general purpose scratch register available,
    // child cache classes must manually specify a dispatch scratch register.
    MOZ_CRASH("x86 needs manual assignment of dispatchScratch");
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

        masm.setupUnalignedABICall(1, output);
        masm.passABIArg(input, MoveOp::DOUBLE);
        if (gen->compilingAsmJS())
            masm.callWithABI(AsmJSImm_ToInt32);
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
        masm.setupUnalignedABICall(1, output);
        masm.vcvtss2sd(input, input, input);
        masm.passABIArg(input, MoveOp::DOUBLE);

        if (gen->compilingAsmJS())
            masm.callWithABI(AsmJSImm_ToInt32);
        else
            masm.callWithABI(BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32));

        masm.storeCallResult(output);
        masm.pop(input);

        restoreVolatile(output);
    }

    masm.jump(ool->rejoin());
}
