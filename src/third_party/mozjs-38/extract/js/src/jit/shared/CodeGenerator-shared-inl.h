/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_CodeGenerator_shared_inl_h
#define jit_shared_CodeGenerator_shared_inl_h

#include "jit/shared/CodeGenerator-shared.h"
#include "jit/Disassembler.h"

namespace js {
namespace jit {

static inline int32_t
ToInt32(const LAllocation* a)
{
    if (a->isConstantValue())
        return a->toConstant()->toInt32();
    if (a->isConstantIndex())
        return a->toConstantIndex()->index();
    MOZ_CRASH("this is not a constant!");
}

static inline double
ToDouble(const LAllocation* a)
{
    return a->toConstant()->toNumber();
}

static inline Register
ToRegister(const LAllocation& a)
{
    MOZ_ASSERT(a.isGeneralReg());
    return a.toGeneralReg()->reg();
}

static inline Register
ToRegister(const LAllocation* a)
{
    return ToRegister(*a);
}

static inline Register
ToRegister(const LDefinition* def)
{
    return ToRegister(*def->output());
}

static inline Register
ToTempRegisterOrInvalid(const LDefinition* def)
{
    if (def->isBogusTemp())
        return InvalidReg;
    return ToRegister(def);
}

static inline Register
ToTempUnboxRegister(const LDefinition* def)
{
    return ToTempRegisterOrInvalid(def);
}

static inline Register
ToRegisterOrInvalid(const LDefinition* a)
{
    return a ? ToRegister(a) : InvalidReg;
}

static inline FloatRegister
ToFloatRegister(const LAllocation& a)
{
    MOZ_ASSERT(a.isFloatReg());
    return a.toFloatReg()->reg();
}

static inline FloatRegister
ToFloatRegister(const LAllocation* a)
{
    return ToFloatRegister(*a);
}

static inline FloatRegister
ToFloatRegister(const LDefinition* def)
{
    return ToFloatRegister(*def->output());
}

static inline AnyRegister
ToAnyRegister(const LAllocation& a)
{
    MOZ_ASSERT(a.isGeneralReg() || a.isFloatReg());
    if (a.isGeneralReg())
        return AnyRegister(ToRegister(a));
    return AnyRegister(ToFloatRegister(a));
}

static inline AnyRegister
ToAnyRegister(const LAllocation* a)
{
    return ToAnyRegister(*a);
}

static inline AnyRegister
ToAnyRegister(const LDefinition* def)
{
    return ToAnyRegister(def->output());
}

static inline Int32Key
ToInt32Key(const LAllocation* a)
{
    if (a->isConstant())
        return Int32Key(ToInt32(a));
    return Int32Key(ToRegister(a));
}

static inline ValueOperand
GetValueOutput(LInstruction* ins)
{
#if defined(JS_NUNBOX32)
    return ValueOperand(ToRegister(ins->getDef(TYPE_INDEX)),
                        ToRegister(ins->getDef(PAYLOAD_INDEX)));
#elif defined(JS_PUNBOX64)
    return ValueOperand(ToRegister(ins->getDef(0)));
#else
#error "Unknown"
#endif
}

static inline ValueOperand
GetTempValue(Register type, Register payload)
{
#if defined(JS_NUNBOX32)
    return ValueOperand(type, payload);
#elif defined(JS_PUNBOX64)
    (void)type;
    return ValueOperand(payload);
#else
#error "Unknown"
#endif
}

void
CodeGeneratorShared::saveLive(LInstruction* ins)
{
    MOZ_ASSERT(!ins->isCall());
    LSafepoint* safepoint = ins->safepoint();
    masm.PushRegsInMask(safepoint->liveRegs());
}

void
CodeGeneratorShared::restoreLive(LInstruction* ins)
{
    MOZ_ASSERT(!ins->isCall());
    LSafepoint* safepoint = ins->safepoint();
    masm.PopRegsInMask(safepoint->liveRegs());
}

void
CodeGeneratorShared::restoreLiveIgnore(LInstruction* ins, RegisterSet ignore)
{
    MOZ_ASSERT(!ins->isCall());
    LSafepoint* safepoint = ins->safepoint();
    masm.PopRegsInMaskIgnore(safepoint->liveRegs(), ignore);
}

void
CodeGeneratorShared::saveLiveVolatile(LInstruction* ins)
{
    MOZ_ASSERT(!ins->isCall());
    LSafepoint* safepoint = ins->safepoint();
    RegisterSet regs = RegisterSet::Intersect(safepoint->liveRegs(), RegisterSet::Volatile());
    masm.PushRegsInMask(regs);
}

void
CodeGeneratorShared::restoreLiveVolatile(LInstruction* ins)
{
    MOZ_ASSERT(!ins->isCall());
    LSafepoint* safepoint = ins->safepoint();
    RegisterSet regs = RegisterSet::Intersect(safepoint->liveRegs(), RegisterSet::Volatile());
    masm.PopRegsInMask(regs);
}

void
CodeGeneratorShared::verifyHeapAccessDisassembly(uint32_t begin, uint32_t end, bool isLoad,
                                                 Scalar::Type type, const Operand& mem,
                                                 LAllocation alloc)
{
#ifdef DEBUG
    using namespace Disassembler;

    OtherOperand op;
    Disassembler::HeapAccess::Kind kind = isLoad ? HeapAccess::Load : HeapAccess::Store;
    switch (type) {
      case Scalar::Int8:
      case Scalar::Int16:
        if (kind == HeapAccess::Load)
            kind = HeapAccess::LoadSext32;
        // FALL THROUGH
      case Scalar::Uint8:
      case Scalar::Uint16:
      case Scalar::Int32:
      case Scalar::Uint32:
        if (!alloc.isConstant()) {
            op = OtherOperand(ToRegister(alloc).code());
        } else {
            int32_t i = ToInt32(&alloc);

            // Sign-extend the immediate value out to 32 bits. We do this even
            // for unsigned element types so that we match what the disassembly
            // code does, as it doesn't know about signedness of stores.
            unsigned shift = 32 - TypedArrayElemSize(type) * 8;
            i = i << shift >> shift;

            op = OtherOperand(i);
        }
        break;
      case Scalar::Float32:
      case Scalar::Float64:
      case Scalar::Float32x4:
      case Scalar::Int32x4:
        op = OtherOperand(ToFloatRegister(alloc).code());
        break;
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("Unexpected array type");
    }

    masm.verifyHeapAccessDisassembly(begin, end,
                                     HeapAccess(kind, TypedArrayElemSize(type),
                                     ComplexAddress(mem), op));
#endif
}

} // ion
} // js

#endif /* jit_shared_CodeGenerator_shared_inl_h */
