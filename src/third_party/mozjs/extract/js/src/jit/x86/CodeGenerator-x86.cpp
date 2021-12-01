/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/CodeGenerator-x86.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"

#include "jsnum.h"

#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "js/Conversions.h"
#include "vm/Shape.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"
#include "vm/JSScript-inl.h"

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
    for (uint32_t i = 0; i < mozilla::ArrayLength(FrameSizes); i++) {
        if (frameDepth < FrameSizes[i])
            return FrameSizeClass(i);
    }

    return FrameSizeClass::None();
}

FrameSizeClass
FrameSizeClass::ClassLimit()
{
    return FrameSizeClass(mozilla::ArrayLength(FrameSizes));
}

uint32_t
FrameSizeClass::frameSize() const
{
    MOZ_ASSERT(class_ != NO_FRAME_SIZE_CLASS_ID);
    MOZ_ASSERT(class_ < mozilla::ArrayLength(FrameSizes));

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
    const AnyRegister in = ToAnyRegister(box->getOperand(0));
    const ValueOperand out = ToOutValue(box);

    masm.moveValue(TypedOrValueRegister(box->type(), in), out);
}

void
CodeGeneratorX86::visitUnbox(LUnbox* unbox)
{
    // Note that for unbox, the type and payload indexes are switched on the
    // inputs.
    Operand type = ToOperand(unbox->type());
    Operand payload = ToOperand(unbox->payload());
    Register output = ToRegister(unbox->output());
    MUnbox* mir = unbox->mir();

    JSValueTag tag = MIRTypeToTag(mir->type());
    if (mir->fallible()) {
        masm.cmp32(type, Imm32(tag));
        bailoutIf(Assembler::NotEqual, unbox->snapshot());
    } else {
#ifdef DEBUG
        Label ok;
        masm.branch32(Assembler::Equal, type, Imm32(tag), &ok);
        masm.assumeUnreachable("Infallible unbox type mismatch");
        masm.bind(&ok);
#endif
    }

    // Note: If spectreValueMasking is disabled, then this instruction will
    // default to a no-op as long as the lowering allocate the same register for
    // the output and the payload.
    masm.unboxNonDouble(type, payload, output, ValueTypeFromMIRType(mir->type()));
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
CodeGeneratorX86::visitWasmUint32ToDouble(LWasmUint32ToDouble* lir)
{
    Register input = ToRegister(lir->input());
    Register temp = ToRegister(lir->temp());

    if (input != temp)
        masm.mov(input, temp);

    // Beware: convertUInt32ToDouble clobbers input.
    masm.convertUInt32ToDouble(temp, ToFloatRegister(lir->output()));
}

void
CodeGeneratorX86::visitWasmUint32ToFloat32(LWasmUint32ToFloat32* lir)
{
    Register input = ToRegister(lir->input());
    Register temp = ToRegister(lir->temp());
    FloatRegister output = ToFloatRegister(lir->output());

    if (input != temp)
        masm.mov(input, temp);

    // Beware: convertUInt32ToFloat32 clobbers input.
    masm.convertUInt32ToFloat32(temp, output);
}

template <typename T>
void
CodeGeneratorX86::emitWasmLoad(T* ins)
{
    const MWasmLoad* mir = ins->mir();

    uint32_t offset = mir->access().offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);

    const LAllocation* ptr = ins->ptr();
    const LAllocation* memoryBase = ins->memoryBase();

    // Lowering has set things up so that we can use a BaseIndex form if the
    // pointer is constant and the offset is zero, or if the pointer is zero.

    Operand srcAddr = ptr->isBogus()
                      ? Operand(ToRegister(memoryBase), offset ? offset : mir->base()->toConstant()->toInt32())
                      : Operand(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

    if (mir->type() == MIRType::Int64) {
        MOZ_ASSERT_IF(mir->access().isAtomic(), mir->access().type() != Scalar::Int64);
        masm.wasmLoadI64(mir->access(), srcAddr, ToOutRegister64(ins));
    } else {
        masm.wasmLoad(mir->access(), srcAddr, ToAnyRegister(ins->output()));
    }
}

void
CodeGeneratorX86::visitWasmLoad(LWasmLoad* ins)
{
    emitWasmLoad(ins);
}

void
CodeGeneratorX86::visitWasmLoadI64(LWasmLoadI64* ins)
{
    emitWasmLoad(ins);
}

template <typename T>
void
CodeGeneratorX86::emitWasmStore(T* ins)
{
    const MWasmStore* mir = ins->mir();

    uint32_t offset = mir->access().offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);

    const LAllocation* ptr = ins->ptr();
    const LAllocation* memoryBase = ins->memoryBase();

    // Lowering has set things up so that we can use a BaseIndex form if the
    // pointer is constant and the offset is zero, or if the pointer is zero.

    Operand dstAddr = ptr->isBogus()
                      ? Operand(ToRegister(memoryBase), offset ? offset : mir->base()->toConstant()->toInt32())
                      : Operand(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

    if (mir->access().type() == Scalar::Int64) {
        Register64 value = ToRegister64(ins->getInt64Operand(LWasmStoreI64::ValueIndex));
        masm.wasmStoreI64(mir->access(), value, dstAddr);
    } else {
        AnyRegister value = ToAnyRegister(ins->getOperand(LWasmStore::ValueIndex));
        masm.wasmStore(mir->access(), value, dstAddr);
    }
}

void
CodeGeneratorX86::visitWasmStore(LWasmStore* ins)
{
    emitWasmStore(ins);
}

void
CodeGeneratorX86::visitWasmStoreI64(LWasmStoreI64* ins)
{
    emitWasmStore(ins);
}

void
CodeGeneratorX86::visitAsmJSLoadHeap(LAsmJSLoadHeap* ins)
{
    const MAsmJSLoadHeap* mir = ins->mir();
    MOZ_ASSERT(mir->access().offset() == 0);

    const LAllocation* ptr = ins->ptr();
    const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();
    const LAllocation* memoryBase = ins->memoryBase();
    AnyRegister out = ToAnyRegister(ins->output());

    Scalar::Type accessType = mir->accessType();
    MOZ_ASSERT(!Scalar::isSimdType(accessType));

    OutOfLineLoadTypedArrayOutOfBounds* ool = nullptr;
    if (mir->needsBoundsCheck()) {
        ool = new(alloc()) OutOfLineLoadTypedArrayOutOfBounds(out, accessType);
        addOutOfLineCode(ool, mir);

        masm.wasmBoundsCheck(Assembler::AboveOrEqual, ToRegister(ptr), ToRegister(boundsCheckLimit),
                             ool->entry());
    }

    Operand srcAddr = ptr->isBogus()
                      ? Operand(ToRegister(memoryBase), 0)
                      : Operand(ToRegister(memoryBase), ToRegister(ptr), TimesOne);

    masm.wasmLoad(mir->access(), srcAddr, out);

    if (ool)
        masm.bind(ool->rejoin());
}

void
CodeGeneratorX86::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins)
{
    const MAsmJSStoreHeap* mir = ins->mir();
    MOZ_ASSERT(mir->offset() == 0);

    const LAllocation* ptr = ins->ptr();
    const LAllocation* value = ins->value();
    const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();
    const LAllocation* memoryBase = ins->memoryBase();

    Scalar::Type accessType = mir->accessType();
    MOZ_ASSERT(!Scalar::isSimdType(accessType));
    canonicalizeIfDeterministic(accessType, value);

    Operand dstAddr = ptr->isBogus()
                      ? Operand(ToRegister(memoryBase), 0)
                      : Operand(ToRegister(memoryBase), ToRegister(ptr), TimesOne);

    Label rejoin;
    if (mir->needsBoundsCheck()) {
        masm.wasmBoundsCheck(Assembler::AboveOrEqual, ToRegister(ptr), ToRegister(boundsCheckLimit),
                             &rejoin);
    }

    masm.wasmStore(mir->access(), ToAnyRegister(value), dstAddr);

    if (rejoin.used())
        masm.bind(&rejoin);
}

void
CodeGeneratorX86::visitWasmCompareExchangeHeap(LWasmCompareExchangeHeap* ins)
{
    MWasmCompareExchangeHeap* mir = ins->mir();

    Scalar::Type accessType = mir->access().type();
    Register ptrReg = ToRegister(ins->ptr());
    Register oldval = ToRegister(ins->oldValue());
    Register newval = ToRegister(ins->newValue());
    Register addrTemp = ToRegister(ins->addrTemp());
    Register memoryBase = ToRegister(ins->memoryBase());
    Register output = ToRegister(ins->output());

    masm.leal(Operand(memoryBase, ptrReg, TimesOne, mir->access().offset()), addrTemp);

    Address memAddr(addrTemp, 0);
    masm.compareExchange(accessType, Synchronization::Full(), memAddr, oldval, newval, output);
}

void
CodeGeneratorX86::visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins)
{
    MWasmAtomicExchangeHeap* mir = ins->mir();

    Scalar::Type accessType = mir->access().type();
    Register ptrReg = ToRegister(ins->ptr());
    Register value = ToRegister(ins->value());
    Register addrTemp = ToRegister(ins->addrTemp());
    Register memoryBase = ToRegister(ins->memoryBase());
    Register output = ToRegister(ins->output());

    masm.leal(Operand(memoryBase, ptrReg, TimesOne, mir->access().offset()), addrTemp);

    Address memAddr(addrTemp, 0);
    masm.atomicExchange(accessType, Synchronization::Full(), memAddr, value, output);
}

void
CodeGeneratorX86::visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins)
{
    MWasmAtomicBinopHeap* mir = ins->mir();

    Scalar::Type accessType = mir->access().type();
    Register ptrReg = ToRegister(ins->ptr());
    Register temp = ins->temp()->isBogusTemp() ? InvalidReg : ToRegister(ins->temp());
    Register addrTemp = ToRegister(ins->addrTemp());
    Register out = ToRegister(ins->output());
    const LAllocation* value = ins->value();
    AtomicOp op = mir->operation();
    Register memoryBase = ToRegister(ins->memoryBase());

    masm.leal(Operand(memoryBase, ptrReg, TimesOne, mir->access().offset()), addrTemp);

    Address memAddr(addrTemp, 0);
    if (value->isConstant()) {
        masm.atomicFetchOp(accessType, Synchronization::Full(), op, Imm32(ToInt32(value)),
                           memAddr, temp, out);
    } else {
        masm.atomicFetchOp(accessType, Synchronization::Full(), op, ToRegister(value),
                           memAddr, temp, out);
    }
}

void
CodeGeneratorX86::visitWasmAtomicBinopHeapForEffect(LWasmAtomicBinopHeapForEffect* ins)
{
    MWasmAtomicBinopHeap* mir = ins->mir();
    MOZ_ASSERT(!mir->hasUses());

    Scalar::Type accessType = mir->access().type();
    Register ptrReg = ToRegister(ins->ptr());
    Register addrTemp = ToRegister(ins->addrTemp());
    const LAllocation* value = ins->value();
    AtomicOp op = mir->operation();
    Register memoryBase = ToRegister(ins->memoryBase());

    masm.leal(Operand(memoryBase, ptrReg, TimesOne, mir->access().offset()), addrTemp);

    Address memAddr(addrTemp, 0);
    if (value->isConstant()) {
        masm.atomicEffectOp(accessType, Synchronization::Full(), op, Imm32(ToInt32(value)), memAddr,
                            InvalidReg);
    } else {
        masm.atomicEffectOp(accessType, Synchronization::Full(), op, ToRegister(value), memAddr,
                            InvalidReg);
    }
}

void
CodeGeneratorX86::visitWasmAtomicLoadI64(LWasmAtomicLoadI64* ins)
{
    uint32_t offset = ins->mir()->access().offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);

    const LAllocation* memoryBase = ins->memoryBase();
    const LAllocation* ptr = ins->ptr();
    Operand srcAddr(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

    MOZ_ASSERT(ToRegister(ins->t1()) == ecx);
    MOZ_ASSERT(ToRegister(ins->t2()) == ebx);
    MOZ_ASSERT(ToOutRegister64(ins).high == edx);
    MOZ_ASSERT(ToOutRegister64(ins).low == eax);

    // We must have the same values in ecx:ebx and edx:eax, but we don't care
    // what they are.  It's safe to clobber ecx:ebx for sure because they are
    // fixed temp registers.
    masm.movl(eax, ebx);
    masm.movl(edx, ecx);
    masm.lock_cmpxchg8b(edx, eax, ecx, ebx, srcAddr);
}

void
CodeGeneratorX86::visitWasmCompareExchangeI64(LWasmCompareExchangeI64* ins)
{
    uint32_t offset = ins->mir()->access().offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);

    const LAllocation* memoryBase = ins->memoryBase();
    const LAllocation* ptr = ins->ptr();
    Operand srcAddr(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

    MOZ_ASSERT(ToRegister64(ins->expected()).low == eax);
    MOZ_ASSERT(ToRegister64(ins->expected()).high == edx);
    MOZ_ASSERT(ToRegister64(ins->replacement()).low == ebx);
    MOZ_ASSERT(ToRegister64(ins->replacement()).high == ecx);
    MOZ_ASSERT(ToOutRegister64(ins).low == eax);
    MOZ_ASSERT(ToOutRegister64(ins).high == edx);

    masm.lock_cmpxchg8b(edx, eax, ecx, ebx, srcAddr);
}

template<typename T>
void
CodeGeneratorX86::emitWasmStoreOrExchangeAtomicI64(T* ins, uint32_t offset)
{
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);

    const LAllocation* memoryBase = ins->memoryBase();
    const LAllocation* ptr = ins->ptr();
    Operand srcAddr(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

    DebugOnly<const LInt64Allocation> value = ins->value();
    MOZ_ASSERT(ToRegister64(value).low == ebx);
    MOZ_ASSERT(ToRegister64(value).high == ecx);

    // eax and ebx will be overwritten every time through the loop but
    // memoryBase and ptr must remain live for a possible second iteration.

    MOZ_ASSERT(ToRegister(memoryBase) != edx && ToRegister(memoryBase) != eax);
    MOZ_ASSERT(ToRegister(ptr) != edx && ToRegister(ptr) != eax);

    Label again;
    masm.bind(&again);
    masm.lock_cmpxchg8b(edx, eax, ecx, ebx, srcAddr);
    masm.j(Assembler::Condition::NonZero, &again);
}

void
CodeGeneratorX86::visitWasmAtomicStoreI64(LWasmAtomicStoreI64* ins)
{
    MOZ_ASSERT(ToRegister(ins->t1()) == edx);
    MOZ_ASSERT(ToRegister(ins->t2()) == eax);

    emitWasmStoreOrExchangeAtomicI64(ins, ins->mir()->access().offset());
}

void
CodeGeneratorX86::visitWasmAtomicExchangeI64(LWasmAtomicExchangeI64* ins)
{
    MOZ_ASSERT(ToOutRegister64(ins).high == edx);
    MOZ_ASSERT(ToOutRegister64(ins).low == eax);

    emitWasmStoreOrExchangeAtomicI64(ins, ins->access().offset());
}

void
CodeGeneratorX86::visitWasmAtomicBinopI64(LWasmAtomicBinopI64* ins)
{
    uint32_t offset = ins->access().offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);

    const LAllocation* memoryBase = ins->memoryBase();
    const LAllocation* ptr = ins->ptr();

    BaseIndex srcAddr(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

    MOZ_ASSERT(ToRegister(memoryBase) == esi || ToRegister(memoryBase) == edi);
    MOZ_ASSERT(ToRegister(ptr) == esi || ToRegister(ptr) == edi);

    Register64 value = ToRegister64(ins->value());

    MOZ_ASSERT(value.low == ebx);
    MOZ_ASSERT(value.high == ecx);

    Register64 output = ToOutRegister64(ins);

    MOZ_ASSERT(output.low == eax);
    MOZ_ASSERT(output.high == edx);

    masm.Push(ecx);
    masm.Push(ebx);

    Address valueAddr(esp, 0);

    // Here the `value` register acts as a temp, we'll restore it below.
    masm.atomicFetchOp64(Synchronization::Full(), ins->operation(), valueAddr, srcAddr, value, output);

    masm.Pop(ebx);
    masm.Pop(ecx);
}

namespace js {
namespace jit {

class OutOfLineTruncate : public OutOfLineCodeBase<CodeGeneratorX86>
{
    LTruncateDToInt32* ins_;

  public:
    explicit OutOfLineTruncate(LTruncateDToInt32* ins)
      : ins_(ins)
    { }

    void accept(CodeGeneratorX86* codegen) override {
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
    explicit OutOfLineTruncateFloat32(LTruncateFToInt32* ins)
      : ins_(ins)
    { }

    void accept(CodeGeneratorX86* codegen) override {
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

    masm.branchTruncateDoubleMaybeModUint32(input, output, ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGeneratorX86::visitTruncateFToInt32(LTruncateFToInt32* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    OutOfLineTruncateFloat32* ool = new(alloc()) OutOfLineTruncateFloat32(ins);
    addOutOfLineCode(ool, ins->mir());

    masm.branchTruncateFloat32MaybeModUint32(input, output, ool->entry());
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
        Label failPopDouble;
        // Push double.
        masm.subl(Imm32(sizeof(double)), esp);
        masm.storeDouble(input, Operand(esp, 0));

        // Check exponent to avoid fp exceptions.
        masm.branchDoubleNotInInt64Range(Address(esp, 0), output, &failPopDouble);

        // Load double, perform 64-bit truncation.
        masm.truncateDoubleToInt64(Address(esp, 0), Address(esp, 0), output);

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

        if (gen->compilingWasm()) {
            masm.setupWasmABICall();
            masm.passABIArg(input, MoveOp::DOUBLE);
            masm.callWithABI(ins->mir()->bytecodeOffset(), wasm::SymbolicAddress::ToInt32);
        } else {
            masm.setupUnalignedABICall(output);
            masm.passABIArg(input, MoveOp::DOUBLE);
            masm.callWithABI(BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32), MoveOp::GENERAL,
                             CheckUnsafeCallWithABI::DontCheckOther);
        }
        masm.storeCallInt32Result(output);

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
        Label failPopFloat;

        // Push float32, but subtracts 64 bits so that the value popped by fisttp fits
        masm.subl(Imm32(sizeof(uint64_t)), esp);
        masm.storeFloat32(input, Operand(esp, 0));

        // Check exponent to avoid fp exceptions.
        masm.branchDoubleNotInInt64Range(Address(esp, 0), output, &failPopFloat);

        // Load float, perform 32-bit truncation.
        masm.truncateFloat32ToInt64(Address(esp, 0), Address(esp, 0), output);

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

        masm.Push(input);

        if (gen->compilingWasm())
            masm.setupWasmABICall();
        else
            masm.setupUnalignedABICall(output);

        masm.vcvtss2sd(input, input, input);
        masm.passABIArg(input.asDouble(), MoveOp::DOUBLE);

        if (gen->compilingWasm()) {
            masm.callWithABI(ins->mir()->bytecodeOffset(), wasm::SymbolicAddress::ToInt32);
        } else {
            masm.callWithABI(BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32), MoveOp::GENERAL,
                             CheckUnsafeCallWithABI::DontCheckOther);
        }

        masm.storeCallInt32Result(output);
        masm.Pop(input);

        restoreVolatile(output);
    }

    masm.jump(ool->rejoin());
}

void
CodeGeneratorX86::visitCompareI64(LCompareI64* lir)
{
    MCompare* mir = lir->mir();
    MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
               mir->compareType() == MCompare::Compare_UInt64);

    const LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
    const LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
    Register64 lhsRegs = ToRegister64(lhs);
    Register output = ToRegister(lir->output());

    bool isSigned = mir->compareType() == MCompare::Compare_Int64;
    Assembler::Condition condition = JSOpToCondition(lir->jsop(), isSigned);
    Label done;

    masm.move32(Imm32(1), output);

    if (IsConstant(rhs)) {
        Imm64 imm = Imm64(ToInt64(rhs));
        masm.branch64(condition, lhsRegs, imm, &done);
    } else {
        Register64 rhsRegs = ToRegister64(rhs);
        masm.branch64(condition, lhsRegs, rhsRegs, &done);
    }

    masm.xorl(output, output);
    masm.bind(&done);
}

void
CodeGeneratorX86::visitCompareI64AndBranch(LCompareI64AndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
               mir->compareType() == MCompare::Compare_UInt64);

    const LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
    const LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
    Register64 lhsRegs = ToRegister64(lhs);

    bool isSigned = mir->compareType() == MCompare::Compare_Int64;
    Assembler::Condition condition = JSOpToCondition(lir->jsop(), isSigned);

    Label* trueLabel = getJumpLabelForBranch(lir->ifTrue());
    Label* falseLabel = getJumpLabelForBranch(lir->ifFalse());

    if (isNextBlock(lir->ifFalse()->lir())) {
        falseLabel = nullptr;
    } else if (isNextBlock(lir->ifTrue()->lir())) {
        condition = Assembler::InvertCondition(condition);
        trueLabel = falseLabel;
        falseLabel = nullptr;
    }

    if (IsConstant(rhs)) {
        Imm64 imm = Imm64(ToInt64(rhs));
        masm.branch64(condition, lhsRegs, imm, trueLabel, falseLabel);
    } else {
        Register64 rhsRegs = ToRegister64(rhs);
        masm.branch64(condition, lhsRegs, rhsRegs, trueLabel, falseLabel);
    }
}

void
CodeGeneratorX86::visitDivOrModI64(LDivOrModI64* lir)
{
    Register64 lhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Lhs));
    Register64 rhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Rhs));
    Register temp = ToRegister(lir->temp());
    Register64 output = ToOutRegister64(lir);

    MOZ_ASSERT(output == ReturnReg64);

    Label done;

    // Handle divide by zero.
    if (lir->canBeDivideByZero()) {
        Label nonZero;
        masm.branchTest64(Assembler::NonZero, rhs, rhs, temp, &nonZero);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
        masm.bind(&nonZero);
    }

    MDefinition* mir = lir->mir();

    // Handle an integer overflow exception from INT64_MIN / -1.
    if (lir->canBeNegativeOverflow()) {
        Label notOverflow;
        masm.branch64(Assembler::NotEqual, lhs, Imm64(INT64_MIN), &notOverflow);
        masm.branch64(Assembler::NotEqual, rhs, Imm64(-1), &notOverflow);
        if (mir->isMod())
            masm.xor64(output, output);
        else
            masm.wasmTrap(wasm::Trap::IntegerOverflow, lir->bytecodeOffset());
        masm.jump(&done);
        masm.bind(&notOverflow);
    }

    masm.setupWasmABICall();
    masm.passABIArg(lhs.high);
    masm.passABIArg(lhs.low);
    masm.passABIArg(rhs.high);
    masm.passABIArg(rhs.low);

    MOZ_ASSERT(gen->compilingWasm());
    if (mir->isMod())
        masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::ModI64);
    else
        masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::DivI64);

    // output in edx:eax, move to output register.
    masm.movl(edx, output.high);
    MOZ_ASSERT(eax == output.low);

    masm.bind(&done);
}

void
CodeGeneratorX86::visitUDivOrModI64(LUDivOrModI64* lir)
{
    Register64 lhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Lhs));
    Register64 rhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Rhs));
    Register temp = ToRegister(lir->temp());
    Register64 output = ToOutRegister64(lir);

    MOZ_ASSERT(output == ReturnReg64);

    // Prevent divide by zero.
    if (lir->canBeDivideByZero()) {
        Label nonZero;
        masm.branchTest64(Assembler::NonZero, rhs, rhs, temp, &nonZero);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
        masm.bind(&nonZero);
    }

    masm.setupWasmABICall();
    masm.passABIArg(lhs.high);
    masm.passABIArg(lhs.low);
    masm.passABIArg(rhs.high);
    masm.passABIArg(rhs.low);

    MOZ_ASSERT(gen->compilingWasm());
    MDefinition* mir = lir->mir();
    if (mir->isMod())
        masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::UModI64);
    else
        masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::UDivI64);

    // output in edx:eax, move to output register.
    masm.movl(edx, output.high);
    MOZ_ASSERT(eax == output.low);
}

void
CodeGeneratorX86::visitWasmSelectI64(LWasmSelectI64* lir)
{
    MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);

    Register cond = ToRegister(lir->condExpr());
    Register64 falseExpr = ToRegister64(lir->falseExpr());
    Register64 out = ToOutRegister64(lir);

    MOZ_ASSERT(ToRegister64(lir->trueExpr()) == out, "true expr is reused for input");

    Label done;
    masm.branchTest32(Assembler::NonZero, cond, cond, &done);
    masm.movl(falseExpr.low, out.low);
    masm.movl(falseExpr.high, out.high);
    masm.bind(&done);
}

void
CodeGeneratorX86::visitWasmReinterpretFromI64(LWasmReinterpretFromI64* lir)
{
    MOZ_ASSERT(lir->mir()->type() == MIRType::Double);
    MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Int64);
    Register64 input = ToRegister64(lir->getInt64Operand(0));

    masm.Push(input.high);
    masm.Push(input.low);
    masm.vmovq(Operand(esp, 0), ToFloatRegister(lir->output()));
    masm.freeStack(sizeof(uint64_t));
}

void
CodeGeneratorX86::visitWasmReinterpretToI64(LWasmReinterpretToI64* lir)
{
    MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);
    MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Double);
    Register64 output = ToOutRegister64(lir);

    masm.reserveStack(sizeof(uint64_t));
    masm.vmovq(ToFloatRegister(lir->input()), Operand(esp, 0));
    masm.Pop(output.low);
    masm.Pop(output.high);
}

void
CodeGeneratorX86::visitExtendInt32ToInt64(LExtendInt32ToInt64* lir)
{
    Register64 output = ToOutRegister64(lir);
    Register input = ToRegister(lir->input());

    if (lir->mir()->isUnsigned()) {
        if (output.low != input)
            masm.movl(input, output.low);
        masm.xorl(output.high, output.high);
    } else {
        MOZ_ASSERT(output.low == input);
        MOZ_ASSERT(output.low == eax);
        MOZ_ASSERT(output.high == edx);
        masm.cdq();
    }
}

void
CodeGeneratorX86::visitSignExtendInt64(LSignExtendInt64* lir)
{
#ifdef DEBUG
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    Register64 output = ToOutRegister64(lir);
    MOZ_ASSERT(input.low == eax);
    MOZ_ASSERT(output.low == eax);
    MOZ_ASSERT(input.high == edx);
    MOZ_ASSERT(output.high == edx);
#endif
    switch (lir->mode()) {
      case MSignExtendInt64::Byte:
        masm.move8SignExtend(eax, eax);
        break;
      case MSignExtendInt64::Half:
        masm.move16SignExtend(eax, eax);
        break;
      case MSignExtendInt64::Word:
        break;
    }
    masm.cdq();
}

void
CodeGeneratorX86::visitWrapInt64ToInt32(LWrapInt64ToInt32* lir)
{
    const LInt64Allocation& input = lir->getInt64Operand(0);
    Register output = ToRegister(lir->output());

    if (lir->mir()->bottomHalf())
        masm.movl(ToRegister(input.low()), output);
    else
        masm.movl(ToRegister(input.high()), output);
}

void
CodeGeneratorX86::visitClzI64(LClzI64* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    Register64 output = ToOutRegister64(lir);

    masm.clz64(input, output.low);
    masm.xorl(output.high, output.high);
}

void
CodeGeneratorX86::visitCtzI64(LCtzI64* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    Register64 output = ToOutRegister64(lir);

    masm.ctz64(input, output.low);
    masm.xorl(output.high, output.high);
}

void
CodeGeneratorX86::visitNotI64(LNotI64* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    Register output = ToRegister(lir->output());

    if (input.high == output) {
        masm.orl(input.low, output);
    } else if (input.low == output) {
        masm.orl(input.high, output);
    } else {
        masm.movl(input.high, output);
        masm.orl(input.low, output);
    }

    masm.cmpl(Imm32(0), output);
    masm.emitSet(Assembler::Equal, output);
}

void
CodeGeneratorX86::visitWasmTruncateToInt64(LWasmTruncateToInt64* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register64 output = ToOutRegister64(lir);

    MWasmTruncateToInt64* mir = lir->mir();
    FloatRegister floatTemp = ToFloatRegister(lir->temp());

    Label fail, convert;

    MOZ_ASSERT (mir->input()->type() == MIRType::Double || mir->input()->type() == MIRType::Float32);

    auto* ool = new(alloc()) OutOfLineWasmTruncateCheck(mir, input, output);
    addOutOfLineCode(ool, mir);

    bool isSaturating = mir->isSaturating();
    if (mir->input()->type() == MIRType::Float32) {
        if (mir->isUnsigned())
            masm.wasmTruncateFloat32ToUInt64(input, output, isSaturating,
                                             ool->entry(), ool->rejoin(), floatTemp);
        else
            masm.wasmTruncateFloat32ToInt64(input, output, isSaturating,
                                            ool->entry(), ool->rejoin(), floatTemp);
    } else {
        if (mir->isUnsigned())
            masm.wasmTruncateDoubleToUInt64(input, output, isSaturating,
                                            ool->entry(), ool->rejoin(), floatTemp);
        else
            masm.wasmTruncateDoubleToInt64(input, output, isSaturating,
                                           ool->entry(), ool->rejoin(), floatTemp);
    }
}

void
CodeGeneratorX86::visitInt64ToFloatingPoint(LInt64ToFloatingPoint* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    FloatRegister output = ToFloatRegister(lir->output());
    Register temp = lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());

    MIRType outputType = lir->mir()->type();
    MOZ_ASSERT(outputType == MIRType::Double || outputType == MIRType::Float32);

    if (outputType == MIRType::Double) {
        if (lir->mir()->isUnsigned())
            masm.convertUInt64ToDouble(input, output, temp);
        else
            masm.convertInt64ToDouble(input, output);
    } else {
        if (lir->mir()->isUnsigned())
            masm.convertUInt64ToFloat32(input, output, temp);
        else
            masm.convertInt64ToFloat32(input, output);
    }
}

void
CodeGeneratorX86::visitTestI64AndBranch(LTestI64AndBranch* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));

    masm.testl(input.high, input.high);
    jumpToBlock(lir->ifTrue(), Assembler::NonZero);
    masm.testl(input.low, input.low);
    emitBranch(Assembler::NonZero, lir->ifTrue(), lir->ifFalse());
}
