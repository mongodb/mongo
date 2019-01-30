/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm64/CodeGenerator-arm64.h"

#include "mozilla/MathAlgorithms.h"

#include "jsnum.h"

#include "jit/CodeGenerator.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"
#include "vm/Shape.h"
#include "vm/TraceLogging.h"

#include "jit/shared/CodeGenerator-shared-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::FloorLog2;
using mozilla::NegativeInfinity;
using JS::GenericNaN;

// shared
CodeGeneratorARM64::CodeGeneratorARM64(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
  : CodeGeneratorShared(gen, graph, masm)
{
}

bool
CodeGeneratorARM64::generateOutOfLineCode()
{
    MOZ_CRASH("generateOutOfLineCode");
}

void
CodeGeneratorARM64::emitBranch(Assembler::Condition cond, MBasicBlock* mirTrue, MBasicBlock* mirFalse)
{
    MOZ_CRASH("emitBranch");
}

void
OutOfLineBailout::accept(CodeGeneratorARM64* codegen)
{
    MOZ_CRASH("accept");
}

void
CodeGeneratorARM64::visitTestIAndBranch(LTestIAndBranch* test)
{
    MOZ_CRASH("visitTestIAndBranch");
}

void
CodeGeneratorARM64::visitCompare(LCompare* comp)
{
    MOZ_CRASH("visitCompare");
}

void
CodeGeneratorARM64::visitCompareAndBranch(LCompareAndBranch* comp)
{
    MOZ_CRASH("visitCompareAndBranch");
}

void
CodeGeneratorARM64::bailoutIf(Assembler::Condition condition, LSnapshot* snapshot)
{
    MOZ_CRASH("bailoutIf");
}

void
CodeGeneratorARM64::bailoutFrom(Label* label, LSnapshot* snapshot)
{
    MOZ_CRASH("bailoutFrom");
}

void
CodeGeneratorARM64::bailout(LSnapshot* snapshot)
{
    MOZ_CRASH("bailout");
}

void
CodeGeneratorARM64::visitOutOfLineBailout(OutOfLineBailout* ool)
{
    MOZ_CRASH("visitOutOfLineBailout");
}

void
CodeGeneratorARM64::visitMinMaxD(LMinMaxD* ins)
{
    MOZ_CRASH("visitMinMaxD");
}

void
CodeGeneratorARM64::visitMinMaxF(LMinMaxF* ins)
{
    MOZ_CRASH("visitMinMaxF");
}

void
CodeGeneratorARM64::visitAbsD(LAbsD* ins)
{
    MOZ_CRASH("visitAbsD");
}

void
CodeGeneratorARM64::visitAbsF(LAbsF* ins)
{
    MOZ_CRASH("visitAbsF");
}

void
CodeGeneratorARM64::visitSqrtD(LSqrtD* ins)
{
    MOZ_CRASH("visitSqrtD");
}

void
CodeGeneratorARM64::visitSqrtF(LSqrtF* ins)
{
    MOZ_CRASH("visitSqrtF");
}

// FIXME: Uh, is this a static function? It looks like it is...
template <typename T>
ARMRegister
toWRegister(const T* a)
{
    return ARMRegister(ToRegister(a), 32);
}

// FIXME: Uh, is this a static function? It looks like it is...
template <typename T>
ARMRegister
toXRegister(const T* a)
{
    return ARMRegister(ToRegister(a), 64);
}

js::jit::Operand
toWOperand(const LAllocation* a)
{
    MOZ_CRASH("toWOperand");
}

vixl::CPURegister
ToCPURegister(const LAllocation* a, Scalar::Type type)
{
    MOZ_CRASH("ToCPURegister");
}

vixl::CPURegister
ToCPURegister(const LDefinition* d, Scalar::Type type)
{
    return ToCPURegister(d->output(), type);
}

void
CodeGeneratorARM64::visitAddI(LAddI* ins)
{
    MOZ_CRASH("visitAddI");
}

void
CodeGeneratorARM64::visitSubI(LSubI* ins)
{
    MOZ_CRASH("visitSubI");
}

void
CodeGeneratorARM64::visitMulI(LMulI* ins)
{
    MOZ_CRASH("visitMulI");
}


void
CodeGeneratorARM64::visitDivI(LDivI* ins)
{
    MOZ_CRASH("visitDivI");
}

void
CodeGeneratorARM64::visitDivPowTwoI(LDivPowTwoI* ins)
{
    MOZ_CRASH("CodeGeneratorARM64::visitDivPowTwoI");
}

void
CodeGeneratorARM64::modICommon(MMod* mir, Register lhs, Register rhs, Register output,
                               LSnapshot* snapshot, Label& done)
{
    MOZ_CRASH("CodeGeneratorARM64::modICommon");
}

void
CodeGeneratorARM64::visitModI(LModI* ins)
{
    MOZ_CRASH("visitModI");
}

void
CodeGeneratorARM64::visitModPowTwoI(LModPowTwoI* ins)
{
    MOZ_CRASH("visitModPowTwoI");
}

void
CodeGeneratorARM64::visitModMaskI(LModMaskI* ins)
{
    MOZ_CRASH("CodeGeneratorARM64::visitModMaskI");
}

void
CodeGeneratorARM64::visitBitNotI(LBitNotI* ins)
{
    MOZ_CRASH("visitBitNotI");
}

void
CodeGeneratorARM64::visitBitOpI(LBitOpI* ins)
{
    MOZ_CRASH("visitBitOpI");
}

void
CodeGeneratorARM64::visitShiftI(LShiftI* ins)
{
    MOZ_CRASH("visitShiftI");
}

void
CodeGeneratorARM64::visitUrshD(LUrshD* ins)
{
    MOZ_CRASH("visitUrshD");
}

void
CodeGeneratorARM64::visitPowHalfD(LPowHalfD* ins)
{
    MOZ_CRASH("visitPowHalfD");
}

MoveOperand
CodeGeneratorARM64::toMoveOperand(const LAllocation a) const
{
    MOZ_CRASH("toMoveOperand");
}

class js::jit::OutOfLineTableSwitch : public OutOfLineCodeBase<CodeGeneratorARM64>
{
    MTableSwitch* mir_;
    Vector<CodeLabel, 8, JitAllocPolicy> codeLabels_;

    void accept(CodeGeneratorARM64* codegen) override {
        codegen->visitOutOfLineTableSwitch(this);
    }

  public:
    OutOfLineTableSwitch(TempAllocator& alloc, MTableSwitch* mir)
      : mir_(mir),
        codeLabels_(alloc)
    { }

    MTableSwitch* mir() const {
        return mir_;
    }

    bool addCodeLabel(CodeLabel label) {
        return codeLabels_.append(label);
    }
    CodeLabel codeLabel(unsigned i) {
        return codeLabels_[i];
    }
};

void
CodeGeneratorARM64::visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool)
{
    MOZ_CRASH("visitOutOfLineTableSwitch");
}

void
CodeGeneratorARM64::emitTableSwitchDispatch(MTableSwitch* mir, Register index_, Register base_)
{
    MOZ_CRASH("emitTableSwitchDispatch");
}

void
CodeGeneratorARM64::visitMathD(LMathD* math)
{
    MOZ_CRASH("visitMathD");
}

void
CodeGeneratorARM64::visitMathF(LMathF* math)
{
    MOZ_CRASH("visitMathF");
}

void
CodeGeneratorARM64::visitFloor(LFloor* lir)
{
    MOZ_CRASH("visitFloor");
}

void
CodeGeneratorARM64::visitFloorF(LFloorF* lir)
{
    MOZ_CRASH("visitFloorF");
}

void
CodeGeneratorARM64::visitCeil(LCeil* lir)
{
    MOZ_CRASH("visitCeil");
}

void
CodeGeneratorARM64::visitCeilF(LCeilF* lir)
{
    MOZ_CRASH("visitCeilF");
}

void
CodeGeneratorARM64::visitRound(LRound* lir)
{
    MOZ_CRASH("visitRound");
}

void
CodeGeneratorARM64::visitRoundF(LRoundF* lir)
{
    MOZ_CRASH("visitRoundF");
}

void
CodeGeneratorARM64::visitClzI(LClzI* lir)
{
    MOZ_CRASH("visitClzI");
}

void
CodeGeneratorARM64::visitCtzI(LCtzI* lir)
{
    MOZ_CRASH("visitCtzI");
}

void
CodeGeneratorARM64::emitRoundDouble(FloatRegister src, Register dest, Label* fail)
{
    MOZ_CRASH("CodeGeneratorARM64::emitRoundDouble");
}

void
CodeGeneratorARM64::visitTruncateDToInt32(LTruncateDToInt32* ins)
{
    MOZ_CRASH("visitTruncateDToInt32");
}

void
CodeGeneratorARM64::visitTruncateFToInt32(LTruncateFToInt32* ins)
{
    MOZ_CRASH("visitTruncateFToInt32");
}

static const uint32_t FrameSizes[] = { 128, 256, 512, 1024 };

FrameSizeClass
FrameSizeClass::FromDepth(uint32_t frameDepth)
{
    return FrameSizeClass::None();
}

FrameSizeClass
FrameSizeClass::ClassLimit()
{
    return FrameSizeClass(0);
}

uint32_t
FrameSizeClass::frameSize() const
{
    MOZ_CRASH("arm64 does not use frame size classes");
}

ValueOperand
CodeGeneratorARM64::ToValue(LInstruction* ins, size_t pos)
{
    return ValueOperand(ToRegister(ins->getOperand(pos)));
}

ValueOperand
CodeGeneratorARM64::ToTempValue(LInstruction* ins, size_t pos)
{
    MOZ_CRASH("CodeGeneratorARM64::ToTempValue");
}

void
CodeGeneratorARM64::visitValue(LValue* value)
{
    MOZ_CRASH("visitValue");
}

void
CodeGeneratorARM64::visitBox(LBox* box)
{
    MOZ_CRASH("visitBox");
}

void
CodeGeneratorARM64::visitUnbox(LUnbox* unbox)
{
    MOZ_CRASH("visitUnbox");
}

void
CodeGeneratorARM64::visitDouble(LDouble* ins)
{
    MOZ_CRASH("visitDouble");
}

void
CodeGeneratorARM64::visitFloat32(LFloat32* ins)
{
    MOZ_CRASH("visitFloat32");
}

void
CodeGeneratorARM64::splitTagForTest(const ValueOperand& value, ScratchTagScope& tag)
{
    MOZ_CRASH("splitTagForTest");
}

void
CodeGeneratorARM64::visitTestDAndBranch(LTestDAndBranch* test)
{
    MOZ_CRASH("visitTestDAndBranch");
}

void
CodeGeneratorARM64::visitTestFAndBranch(LTestFAndBranch* test)
{
    MOZ_CRASH("visitTestFAndBranch");
}

void
CodeGeneratorARM64::visitCompareD(LCompareD* comp)
{
    MOZ_CRASH("visitCompareD");
}

void
CodeGeneratorARM64::visitCompareF(LCompareF* comp)
{
    MOZ_CRASH("visitCompareF");
}

void
CodeGeneratorARM64::visitCompareDAndBranch(LCompareDAndBranch* comp)
{
    MOZ_CRASH("visitCompareDAndBranch");
}

void
CodeGeneratorARM64::visitCompareFAndBranch(LCompareFAndBranch* comp)
{
    MOZ_CRASH("visitCompareFAndBranch");
}

void
CodeGeneratorARM64::visitCompareB(LCompareB* lir)
{
    MOZ_CRASH("visitCompareB");
}

void
CodeGeneratorARM64::visitCompareBAndBranch(LCompareBAndBranch* lir)
{
    MOZ_CRASH("visitCompareBAndBranch");
}

void
CodeGeneratorARM64::visitCompareBitwise(LCompareBitwise* lir)
{
    MOZ_CRASH("visitCompareBitwise");
}

void
CodeGeneratorARM64::visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir)
{
    MOZ_CRASH("visitCompareBitwiseAndBranch");
}

void
CodeGeneratorARM64::visitBitAndAndBranch(LBitAndAndBranch* baab)
{
    MOZ_CRASH("visitBitAndAndBranch");
}

void
CodeGeneratorARM64::visitWasmUint32ToDouble(LWasmUint32ToDouble* lir)
{
    MOZ_CRASH("visitWasmUint32ToDouble");
}

void
CodeGeneratorARM64::visitWasmUint32ToFloat32(LWasmUint32ToFloat32* lir)
{
    MOZ_CRASH("visitWasmUint32ToFloat32");
}

void
CodeGeneratorARM64::visitNotI(LNotI* ins)
{
    MOZ_CRASH("visitNotI");
}

//        NZCV
// NAN -> 0011
// ==  -> 0110
// <   -> 1000
// >   -> 0010
void
CodeGeneratorARM64::visitNotD(LNotD* ins)
{
    MOZ_CRASH("visitNotD");
}

void
CodeGeneratorARM64::visitNotF(LNotF* ins)
{
    MOZ_CRASH("visitNotF");
}

void
CodeGeneratorARM64::visitLoadSlotV(LLoadSlotV* load)
{
    MOZ_CRASH("CodeGeneratorARM64::visitLoadSlotV");
}

void
CodeGeneratorARM64::visitLoadSlotT(LLoadSlotT* load)
{
    MOZ_CRASH("CodeGeneratorARM64::visitLoadSlotT");
}

void
CodeGeneratorARM64::visitStoreSlotT(LStoreSlotT* store)
{
    MOZ_CRASH("CodeGeneratorARM64::visitStoreSlotT");
}

void
CodeGeneratorARM64::visitLoadElementT(LLoadElementT* load)
{
    MOZ_CRASH("CodeGeneratorARM64::visitLoadElementT");
}

void
CodeGeneratorARM64::storeElementTyped(const LAllocation* value, MIRType valueType,
                                      MIRType elementType, Register elements,
                                      const LAllocation* index)
{
    MOZ_CRASH("CodeGeneratorARM64::storeElementTyped");
}

void
CodeGeneratorARM64::visitInterruptCheck(LInterruptCheck* lir)
{
    MOZ_CRASH("CodeGeneratorARM64::visitInterruptCheck");
}

void
CodeGeneratorARM64::generateInvalidateEpilogue()
{
    MOZ_CRASH("generateInvalidateEpilogue");
}

template <class U>
Register
getBase(U* mir)
{
    switch (mir->base()) {
      case U::Heap: return HeapReg;
    }
    return InvalidReg;
}

void
CodeGeneratorARM64::visitAsmJSLoadHeap(LAsmJSLoadHeap* ins)
{
    MOZ_CRASH("visitAsmJSLoadHeap");
}

void
CodeGeneratorARM64::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins)
{
    MOZ_CRASH("visitAsmJSStoreHeap");
}

void
CodeGeneratorARM64::visitWasmCompareExchangeHeap(LWasmCompareExchangeHeap* ins)
{
    MOZ_CRASH("visitWasmCompareExchangeHeap");
}

void
CodeGeneratorARM64::visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins)
{
    MOZ_CRASH("visitWasmAtomicBinopHeap");
}

void
CodeGeneratorARM64::visitWasmStackArg(LWasmStackArg* ins)
{
    MOZ_CRASH("visitWasmStackArg");
}

void
CodeGeneratorARM64::visitUDiv(LUDiv* ins)
{
    MOZ_CRASH("visitUDiv");
}

void
CodeGeneratorARM64::visitUMod(LUMod* ins)
{
    MOZ_CRASH("visitUMod");
}

void
CodeGeneratorARM64::visitEffectiveAddress(LEffectiveAddress* ins)
{
    MOZ_CRASH("visitEffectiveAddress");
}

void
CodeGeneratorARM64::visitNegI(LNegI* ins)
{
    MOZ_CRASH("visitNegI");
}

void
CodeGeneratorARM64::visitNegD(LNegD* ins)
{
    MOZ_CRASH("visitNegD");
}

void
CodeGeneratorARM64::visitNegF(LNegF* ins)
{
    MOZ_CRASH("visitNegF");
}

void
CodeGeneratorARM64::setReturnDoubleRegs(LiveRegisterSet* regs)
{
    MOZ_ASSERT(ReturnFloat32Reg.code_ == FloatRegisters::s0);
    MOZ_ASSERT(ReturnDoubleReg.code_ == FloatRegisters::d0);
    FloatRegister s1 = {FloatRegisters::s1, FloatRegisters::Single};
    regs->add(ReturnFloat32Reg);
    regs->add(s1);
    regs->add(ReturnDoubleReg);
}

void
CodeGeneratorARM64::visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* lir)
{
    Register elements = ToRegister(lir->elements());
    AnyRegister output = ToAnyRegister(lir->output());
    Register temp = lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());

    Register oldval = ToRegister(lir->oldval());
    Register newval = ToRegister(lir->newval());

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    if (lir->index()->isConstant()) {
        Address dest(elements, ToInt32(lir->index()) * width);
        masm.compareExchangeJS(arrayType, Synchronization::Full(), dest, oldval, newval, temp, output);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        masm.compareExchangeJS(arrayType, Synchronization::Full(), dest, oldval, newval, temp, output);
    }
}

void
CodeGeneratorARM64::visitAtomicExchangeTypedArrayElement(LAtomicExchangeTypedArrayElement* lir)
{
    Register elements = ToRegister(lir->elements());
    AnyRegister output = ToAnyRegister(lir->output());
    Register temp = lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());

    Register value = ToRegister(lir->value());

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    if (lir->index()->isConstant()) {
        Address dest(elements, ToInt32(lir->index()) * width);
        masm.atomicExchangeJS(arrayType, Synchronization::Full(), dest, value, temp, output);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        masm.atomicExchangeJS(arrayType, Synchronization::Full(), dest, value, temp, output);
    }
}
