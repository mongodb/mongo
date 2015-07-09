/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_MacroAssembler_x64_h
#define jit_x64_MacroAssembler_x64_h

#include "jit/JitFrames.h"
#include "jit/MoveResolver.h"
#include "jit/shared/MacroAssembler-x86-shared.h"

namespace js {
namespace jit {

struct ImmShiftedTag : public ImmWord
{
    explicit ImmShiftedTag(JSValueShiftedTag shtag)
      : ImmWord((uintptr_t)shtag)
    { }

    explicit ImmShiftedTag(JSValueType type)
      : ImmWord(uintptr_t(JSValueShiftedTag(JSVAL_TYPE_TO_SHIFTED_TAG(type))))
    { }
};

struct ImmTag : public Imm32
{
    explicit ImmTag(JSValueTag tag)
      : Imm32(tag)
    { }
};

class MacroAssemblerX64 : public MacroAssemblerX86Shared
{
    // Number of bytes the stack is adjusted inside a call to C. Calls to C may
    // not be nested.
    bool inCall_;
    uint32_t args_;
    uint32_t passedIntArgs_;
    uint32_t passedFloatArgs_;
    uint32_t stackForCall_;
    bool dynamicAlignment_;

    // These use SystemAllocPolicy since asm.js releases memory after each
    // function is compiled, and these need to live until after all functions
    // are compiled.
    struct Double {
        double value;
        NonAssertingLabel uses;
        explicit Double(double value) : value(value) {}
    };
    Vector<Double, 0, SystemAllocPolicy> doubles_;

    typedef HashMap<double, size_t, DefaultHasher<double>, SystemAllocPolicy> DoubleMap;
    DoubleMap doubleMap_;

    struct Float {
        float value;
        NonAssertingLabel uses;
        explicit Float(float value) : value(value) {}
    };
    Vector<Float, 0, SystemAllocPolicy> floats_;

    typedef HashMap<float, size_t, DefaultHasher<float>, SystemAllocPolicy> FloatMap;
    FloatMap floatMap_;

    struct SimdData {
        SimdConstant value;
        NonAssertingLabel uses;

        explicit SimdData(const SimdConstant& v) : value(v) {}
        SimdConstant::Type type() { return value.type(); }
    };
    Vector<SimdData, 0, SystemAllocPolicy> simds_;
    typedef HashMap<SimdConstant, size_t, SimdConstant, SystemAllocPolicy> SimdMap;
    SimdMap simdMap_;

    void setupABICall(uint32_t arg);

  protected:
    MoveResolver moveResolver_;

  public:
    using MacroAssemblerX86Shared::call;
    using MacroAssemblerX86Shared::Push;
    using MacroAssemblerX86Shared::Pop;
    using MacroAssemblerX86Shared::callWithExitFrame;
    using MacroAssemblerX86Shared::branch32;
    using MacroAssemblerX86Shared::branchTest32;
    using MacroAssemblerX86Shared::load32;
    using MacroAssemblerX86Shared::store32;

    MacroAssemblerX64()
      : inCall_(false)
    {
    }

    // The buffer is about to be linked, make sure any constant pools or excess
    // bookkeeping has been flushed to the instruction stream.
    void finish();

    /////////////////////////////////////////////////////////////////
    // X64 helpers.
    /////////////////////////////////////////////////////////////////
    void call(ImmWord target) {
        mov(target, rax);
        call(rax);
    }
    void call(ImmPtr target) {
        call(ImmWord(uintptr_t(target.value)));
    }
    void writeDataRelocation(const Value& val) {
        if (val.isMarkable()) {
            gc::Cell* cell = reinterpret_cast<gc::Cell*>(val.toGCThing());
            if (cell && gc::IsInsideNursery(cell))
                embedsNurseryPointers_ = true;
            dataRelocations_.writeUnsigned(masm.currentOffset());
        }
    }

    // Refers to the upper 32 bits of a 64-bit Value operand.
    // On x86_64, the upper 32 bits do not necessarily only contain the type.
    Operand ToUpper32(Operand base) {
        switch (base.kind()) {
          case Operand::MEM_REG_DISP:
            return Operand(Register::FromCode(base.base()), base.disp() + 4);

          case Operand::MEM_SCALE:
            return Operand(Register::FromCode(base.base()), Register::FromCode(base.index()),
                           base.scale(), base.disp() + 4);

          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    static inline Operand ToUpper32(const Address& address) {
        return Operand(address.base, address.offset + 4);
    }
    static inline Operand ToUpper32(const BaseIndex& address) {
        return Operand(address.base, address.index, address.scale, address.offset + 4);
    }

    uint32_t Upper32Of(JSValueShiftedTag tag) {
        union { // Implemented in this way to appease MSVC++.
            uint64_t tag;
            struct {
                uint32_t lo32;
                uint32_t hi32;
            } s;
        } e;
        e.tag = tag;
        return e.s.hi32;
    }

    JSValueShiftedTag GetShiftedTag(JSValueType type) {
        return (JSValueShiftedTag)JSVAL_TYPE_TO_SHIFTED_TAG(type);
    }

    /////////////////////////////////////////////////////////////////
    // X86/X64-common interface.
    /////////////////////////////////////////////////////////////////
    Address ToPayload(Address value) {
        return value;
    }

    void storeValue(ValueOperand val, Operand dest) {
        movq(val.valueReg(), dest);
    }
    void storeValue(ValueOperand val, const Address& dest) {
        storeValue(val, Operand(dest));
    }
    template <typename T>
    void storeValue(JSValueType type, Register reg, const T& dest) {
        // Value types with 32-bit payloads can be emitted as two 32-bit moves.
        if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
            movl(reg, Operand(dest));
            movl(Imm32(Upper32Of(GetShiftedTag(type))), ToUpper32(Operand(dest)));
        } else {
            boxValue(type, reg, ScratchReg);
            movq(ScratchReg, Operand(dest));
        }
    }
    template <typename T>
    void storeValue(const Value& val, const T& dest) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        if (val.isMarkable()) {
            movWithPatch(ImmWord(jv.asBits), ScratchReg);
            writeDataRelocation(val);
        } else {
            mov(ImmWord(jv.asBits), ScratchReg);
        }
        movq(ScratchReg, Operand(dest));
    }
    void storeValue(ValueOperand val, BaseIndex dest) {
        storeValue(val, Operand(dest));
    }
    void loadValue(Operand src, ValueOperand val) {
        movq(src, val.valueReg());
    }
    void loadValue(Address src, ValueOperand val) {
        loadValue(Operand(src), val);
    }
    void loadValue(const BaseIndex& src, ValueOperand val) {
        loadValue(Operand(src), val);
    }
    void tagValue(JSValueType type, Register payload, ValueOperand dest) {
        MOZ_ASSERT(dest.valueReg() != ScratchReg);
        if (payload != dest.valueReg())
            movq(payload, dest.valueReg());
        mov(ImmShiftedTag(type), ScratchReg);
        orq(ScratchReg, dest.valueReg());
    }
    void pushValue(ValueOperand val) {
        push(val.valueReg());
    }
    void Push(const ValueOperand& val) {
        pushValue(val);
        framePushed_ += sizeof(Value);
    }
    void popValue(ValueOperand val) {
        pop(val.valueReg());
    }
    void pushValue(const Value& val) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        if (val.isMarkable()) {
            movWithPatch(ImmWord(jv.asBits), ScratchReg);
            writeDataRelocation(val);
            push(ScratchReg);
        } else {
            push(ImmWord(jv.asBits));
        }
    }
    void pushValue(JSValueType type, Register reg) {
        boxValue(type, reg, ScratchReg);
        push(ScratchReg);
    }
    void pushValue(const Address& addr) {
        push(Operand(addr));
    }
    void Pop(const ValueOperand& val) {
        popValue(val);
        framePushed_ -= sizeof(Value);
    }

    void moveValue(const Value& val, Register dest) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        movWithPatch(ImmWord(jv.asBits), dest);
        writeDataRelocation(val);
    }
    void moveValue(const Value& src, const ValueOperand& dest) {
        moveValue(src, dest.valueReg());
    }
    void moveValue(const ValueOperand& src, const ValueOperand& dest) {
        if (src.valueReg() != dest.valueReg())
            movq(src.valueReg(), dest.valueReg());
    }
    void boxValue(JSValueType type, Register src, Register dest) {
        MOZ_ASSERT(src != dest);

        JSValueShiftedTag tag = (JSValueShiftedTag)JSVAL_TYPE_TO_SHIFTED_TAG(type);
#ifdef DEBUG
        if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
            Label upper32BitsZeroed;
            movePtr(ImmWord(UINT32_MAX), dest);
            branchPtr(Assembler::BelowOrEqual, src, dest, &upper32BitsZeroed);
            breakpoint();
            bind(&upper32BitsZeroed);
        }
#endif
        mov(ImmShiftedTag(tag), dest);
        orq(src, dest);
    }

    Condition testUndefined(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_UNDEFINED));
        return cond;
    }
    Condition testInt32(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_INT32));
        return cond;
    }
    Condition testBoolean(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_BOOLEAN));
        return cond;
    }
    Condition testNull(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_NULL));
        return cond;
    }
    Condition testString(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_STRING));
        return cond;
    }
    Condition testSymbol(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_SYMBOL));
        return cond;
    }
    Condition testObject(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_OBJECT));
        return cond;
    }
    Condition testDouble(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, Imm32(JSVAL_TAG_MAX_DOUBLE));
        return cond == Equal ? BelowOrEqual : Above;
    }
    Condition testNumber(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, Imm32(JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET));
        return cond == Equal ? BelowOrEqual : Above;
    }
    Condition testGCThing(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, Imm32(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET));
        return cond == Equal ? AboveOrEqual : Below;
    }

    Condition testMagic(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_MAGIC));
        return cond;
    }
    Condition testError(Condition cond, Register tag) {
        return testMagic(cond, tag);
    }
    Condition testPrimitive(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET));
        return cond == Equal ? Below : AboveOrEqual;
    }

    Condition testUndefined(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testUndefined(cond, ScratchReg);
    }
    Condition testInt32(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testInt32(cond, ScratchReg);
    }
    Condition testBoolean(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testBoolean(cond, ScratchReg);
    }
    Condition testDouble(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testDouble(cond, ScratchReg);
    }
    Condition testNumber(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testNumber(cond, ScratchReg);
    }
    Condition testNull(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testNull(cond, ScratchReg);
    }
    Condition testString(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testString(cond, ScratchReg);
    }
    Condition testSymbol(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testSymbol(cond, ScratchReg);
    }
    Condition testObject(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testObject(cond, ScratchReg);
    }
    Condition testGCThing(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testGCThing(cond, ScratchReg);
    }
    Condition testPrimitive(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testPrimitive(cond, ScratchReg);
    }


    Condition testUndefined(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testUndefined(cond, ScratchReg);
    }
    Condition testInt32(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testInt32(cond, ScratchReg);
    }
    Condition testBoolean(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testBoolean(cond, ScratchReg);
    }
    Condition testDouble(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testDouble(cond, ScratchReg);
    }
    Condition testNumber(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testNumber(cond, ScratchReg);
    }
    Condition testNull(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testNull(cond, ScratchReg);
    }
    Condition testString(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testString(cond, ScratchReg);
    }
    Condition testSymbol(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testSymbol(cond, ScratchReg);
    }
    Condition testObject(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testObject(cond, ScratchReg);
    }
    Condition testPrimitive(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testPrimitive(cond, ScratchReg);
    }
    Condition testGCThing(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testGCThing(cond, ScratchReg);
    }
    Condition testMagic(Condition cond, const Address& src) {
        splitTag(src, ScratchReg);
        return testMagic(cond, ScratchReg);
    }


    Condition testUndefined(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testUndefined(cond, ScratchReg);
    }
    Condition testNull(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testNull(cond, ScratchReg);
    }
    Condition testBoolean(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testBoolean(cond, ScratchReg);
    }
    Condition testString(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testString(cond, ScratchReg);
    }
    Condition testSymbol(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testSymbol(cond, ScratchReg);
    }
    Condition testInt32(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testInt32(cond, ScratchReg);
    }
    Condition testObject(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testObject(cond, ScratchReg);
    }
    Condition testDouble(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testDouble(cond, ScratchReg);
    }
    Condition testMagic(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testMagic(cond, ScratchReg);
    }
    Condition testGCThing(Condition cond, const BaseIndex& src) {
        splitTag(src, ScratchReg);
        return testGCThing(cond, ScratchReg);
    }

    Condition isMagic(Condition cond, const ValueOperand& src, JSWhyMagic why) {
        uint64_t magic = MagicValue(why).asRawBits();
        cmpPtr(src.valueReg(), ImmWord(magic));
        return cond;
    }

    void cmpPtr(Register lhs, const ImmWord rhs) {
        MOZ_ASSERT(lhs != ScratchReg);
        if (intptr_t(rhs.value) <= INT32_MAX && intptr_t(rhs.value) >= INT32_MIN) {
            cmpPtr(lhs, Imm32(int32_t(rhs.value)));
        } else {
            movePtr(rhs, ScratchReg);
            cmpPtr(lhs, ScratchReg);
        }
    }
    void cmpPtr(Register lhs, const ImmPtr rhs) {
        cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
    }
    void cmpPtr(Register lhs, const ImmGCPtr rhs) {
        MOZ_ASSERT(lhs != ScratchReg);
        movePtr(rhs, ScratchReg);
        cmpPtr(lhs, ScratchReg);
    }
    void cmpPtr(Register lhs, const Imm32 rhs) {
        cmpq(rhs, lhs);
    }
    void cmpPtr(const Operand& lhs, const ImmGCPtr rhs) {
        MOZ_ASSERT(!lhs.containsReg(ScratchReg));
        movePtr(rhs, ScratchReg);
        cmpPtr(lhs, ScratchReg);
    }
    void cmpPtr(const Operand& lhs, const ImmMaybeNurseryPtr rhs) {
        cmpPtr(lhs, noteMaybeNurseryPtr(rhs));
    }
    void cmpPtr(const Operand& lhs, const ImmWord rhs) {
        if ((intptr_t)rhs.value <= INT32_MAX && (intptr_t)rhs.value >= INT32_MIN) {
            cmpPtr(lhs, Imm32((int32_t)rhs.value));
        } else {
            movePtr(rhs, ScratchReg);
            cmpPtr(lhs, ScratchReg);
        }
    }
    void cmpPtr(const Operand& lhs, const ImmPtr rhs) {
        cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
    }
    void cmpPtr(const Address& lhs, const ImmGCPtr rhs) {
        cmpPtr(Operand(lhs), rhs);
    }
    void cmpPtr(const Address& lhs, const ImmWord rhs) {
        cmpPtr(Operand(lhs), rhs);
    }
    void cmpPtr(const Address& lhs, const ImmPtr rhs) {
        cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
    }
    void cmpPtr(const Operand& lhs, Register rhs) {
        cmpq(rhs, lhs);
    }
    void cmpPtr(Register lhs, const Operand& rhs) {
        cmpq(rhs, lhs);
    }
    void cmpPtr(const Operand& lhs, const Imm32 rhs) {
        cmpq(rhs, lhs);
    }
    void cmpPtr(const Address& lhs, Register rhs) {
        cmpPtr(Operand(lhs), rhs);
    }
    void cmpPtr(Register lhs, Register rhs) {
        cmpq(rhs, lhs);
    }
    void testPtr(Register lhs, Register rhs) {
        testq(rhs, lhs);
    }
    void testPtr(Register lhs, Imm32 rhs) {
        testq(rhs, lhs);
    }
    void testPtr(const Operand& lhs, Imm32 rhs) {
        testq(rhs, lhs);
    }

    template <typename T1, typename T2>
    void cmpPtrSet(Assembler::Condition cond, T1 lhs, T2 rhs, Register dest)
    {
        cmpPtr(lhs, rhs);
        emitSet(cond, dest);
    }

    /////////////////////////////////////////////////////////////////
    // Common interface.
    /////////////////////////////////////////////////////////////////
    void reserveStack(uint32_t amount) {
        if (amount) {
            // On windows, we cannot skip very far down the stack without touching the
            // memory pages in-between.  This is a corner-case code for situations where the
            // Ion frame data for a piece of code is very large.  To handle this special case,
            // for frames over 1k in size we allocate memory on the stack incrementally, touching
            // it as we go.
            uint32_t amountLeft = amount;
            while (amountLeft > 4096) {
                subq(Imm32(4096), StackPointer);
                store32(Imm32(0), Address(StackPointer, 0));
                amountLeft -= 4096;
            }
            subq(Imm32(amountLeft), StackPointer);
        }
        framePushed_ += amount;
    }
    void freeStack(uint32_t amount) {
        MOZ_ASSERT(amount <= framePushed_);
        if (amount)
            addq(Imm32(amount), StackPointer);
        framePushed_ -= amount;
    }
    void freeStack(Register amount) {
        addq(amount, StackPointer);
    }

    void addPtr(Register src, Register dest) {
        addq(src, dest);
    }
    void addPtr(Imm32 imm, Register dest) {
        addq(imm, dest);
    }
    void addPtr(Imm32 imm, const Address& dest) {
        addq(imm, Operand(dest));
    }
    void addPtr(Imm32 imm, const Operand& dest) {
        addq(imm, dest);
    }
    void addPtr(ImmWord imm, Register dest) {
        MOZ_ASSERT(dest != ScratchReg);
        if ((intptr_t)imm.value <= INT32_MAX && (intptr_t)imm.value >= INT32_MIN) {
            addq(Imm32((int32_t)imm.value), dest);
        } else {
            mov(imm, ScratchReg);
            addq(ScratchReg, dest);
        }
    }
    void addPtr(ImmPtr imm, Register dest) {
        addPtr(ImmWord(uintptr_t(imm.value)), dest);
    }
    void addPtr(const Address& src, Register dest) {
        addq(Operand(src), dest);
    }
    void subPtr(Imm32 imm, Register dest) {
        subq(imm, dest);
    }
    void subPtr(Register src, Register dest) {
        subq(src, dest);
    }
    void subPtr(const Address& addr, Register dest) {
        subq(Operand(addr), dest);
    }
    void subPtr(Register src, const Address& dest) {
        subq(src, Operand(dest));
    }
    void mulBy3(const Register& src, const Register& dest) {
        lea(Operand(src, src, TimesTwo), dest);
    }

    void branch32(Condition cond, AbsoluteAddress lhs, Imm32 rhs, Label* label) {
        if (X86Encoding::IsAddressImmediate(lhs.addr)) {
            branch32(cond, Operand(lhs), rhs, label);
        } else {
            mov(ImmPtr(lhs.addr), ScratchReg);
            branch32(cond, Address(ScratchReg, 0), rhs, label);
        }
    }
    void branch32(Condition cond, AsmJSAbsoluteAddress lhs, Imm32 rhs, Label* label) {
        mov(AsmJSImmPtr(lhs.kind()), ScratchReg);
        branch32(cond, Address(ScratchReg, 0), rhs, label);
    }
    void branch32(Condition cond, AbsoluteAddress lhs, Register rhs, Label* label) {
        if (X86Encoding::IsAddressImmediate(lhs.addr)) {
            branch32(cond, Operand(lhs), rhs, label);
        } else {
            mov(ImmPtr(lhs.addr), ScratchReg);
            branch32(cond, Address(ScratchReg, 0), rhs, label);
        }
    }
    void branchTest32(Condition cond, AbsoluteAddress address, Imm32 imm, Label* label) {
        if (X86Encoding::IsAddressImmediate(address.addr)) {
            test32(Operand(address), imm);
        } else {
            mov(ImmPtr(address.addr), ScratchReg);
            test32(Operand(ScratchReg, 0), imm);
        }
        j(cond, label);
    }

    // Specialization for AbsoluteAddress.
    void branchPtr(Condition cond, AbsoluteAddress addr, Register ptr, Label* label) {
        MOZ_ASSERT(ptr != ScratchReg);
        if (X86Encoding::IsAddressImmediate(addr.addr)) {
            branchPtr(cond, Operand(addr), ptr, label);
        } else {
            mov(ImmPtr(addr.addr), ScratchReg);
            branchPtr(cond, Operand(ScratchReg, 0x0), ptr, label);
        }
    }
    void branchPtr(Condition cond, AbsoluteAddress addr, ImmWord ptr, Label* label) {
        if (X86Encoding::IsAddressImmediate(addr.addr)) {
            branchPtr(cond, Operand(addr), ptr, label);
        } else {
            mov(ImmPtr(addr.addr), ScratchReg);
            branchPtr(cond, Operand(ScratchReg, 0x0), ptr, label);
        }
    }
    void branchPtr(Condition cond, AsmJSAbsoluteAddress addr, Register ptr, Label* label) {
        MOZ_ASSERT(ptr != ScratchReg);
        mov(AsmJSImmPtr(addr.kind()), ScratchReg);
        branchPtr(cond, Operand(ScratchReg, 0x0), ptr, label);
    }

    void branchPrivatePtr(Condition cond, Address lhs, ImmPtr ptr, Label* label) {
        branchPtr(cond, lhs, ImmWord(uintptr_t(ptr.value) >> 1), label);
    }

    void branchPrivatePtr(Condition cond, Address lhs, Register ptr, Label* label) {
        if (ptr != ScratchReg)
            movePtr(ptr, ScratchReg);
        rshiftPtr(Imm32(1), ScratchReg);
        branchPtr(cond, lhs, ScratchReg, label);
    }

    template <typename T, typename S>
    void branchPtr(Condition cond, T lhs, S ptr, Label* label) {
        cmpPtr(Operand(lhs), ptr);
        j(cond, label);
    }

    CodeOffsetJump jumpWithPatch(RepatchLabel* label) {
        JmpSrc src = jmpSrc(label);
        return CodeOffsetJump(size(), addPatchableJump(src, Relocation::HARDCODED));
    }

    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Condition cond) {
        JmpSrc src = jSrc(cond, label);
        return CodeOffsetJump(size(), addPatchableJump(src, Relocation::HARDCODED));
    }

    CodeOffsetJump backedgeJump(RepatchLabel* label) {
        return jumpWithPatch(label);
    }

    template <typename S, typename T>
    CodeOffsetJump branchPtrWithPatch(Condition cond, S lhs, T ptr, RepatchLabel* label) {
        cmpPtr(lhs, ptr);
        return jumpWithPatch(label, cond);
    }
    void branchPtr(Condition cond, Register lhs, Register rhs, Label* label) {
        cmpPtr(lhs, rhs);
        j(cond, label);
    }
    void branchTestPtr(Condition cond, Register lhs, Register rhs, Label* label) {
        testPtr(lhs, rhs);
        j(cond, label);
    }
    void branchTestPtr(Condition cond, Register lhs, Imm32 imm, Label* label) {
        testPtr(lhs, imm);
        j(cond, label);
    }
    void branchTestPtr(Condition cond, const Address& lhs, Imm32 imm, Label* label) {
        testPtr(Operand(lhs), imm);
        j(cond, label);
    }
    void decBranchPtr(Condition cond, Register lhs, Imm32 imm, Label* label) {
        subPtr(imm, lhs);
        j(cond, label);
    }

    void movePtr(Register src, Register dest) {
        movq(src, dest);
    }
    void movePtr(Register src, const Operand& dest) {
        movq(src, dest);
    }
    void movePtr(ImmWord imm, Register dest) {
        mov(imm, dest);
    }
    void movePtr(ImmPtr imm, Register dest) {
        mov(imm, dest);
    }
    void movePtr(AsmJSImmPtr imm, Register dest) {
        mov(imm, dest);
    }
    void movePtr(ImmGCPtr imm, Register dest) {
        movq(imm, dest);
    }
    void movePtr(ImmMaybeNurseryPtr imm, Register dest) {
        movePtr(noteMaybeNurseryPtr(imm), dest);
    }
    void loadPtr(AbsoluteAddress address, Register dest) {
        if (X86Encoding::IsAddressImmediate(address.addr)) {
            movq(Operand(address), dest);
        } else {
            mov(ImmPtr(address.addr), ScratchReg);
            loadPtr(Address(ScratchReg, 0x0), dest);
        }
    }
    void loadPtr(const Address& address, Register dest) {
        movq(Operand(address), dest);
    }
    void loadPtr(const Operand& src, Register dest) {
        movq(src, dest);
    }
    void loadPtr(const BaseIndex& src, Register dest) {
        movq(Operand(src), dest);
    }
    void loadPrivate(const Address& src, Register dest) {
        loadPtr(src, dest);
        shlq(Imm32(1), dest);
    }
    void load32(AbsoluteAddress address, Register dest) {
        if (X86Encoding::IsAddressImmediate(address.addr)) {
            movl(Operand(address), dest);
        } else {
            mov(ImmPtr(address.addr), ScratchReg);
            load32(Address(ScratchReg, 0x0), dest);
        }
    }
    template <typename T>
    void storePtr(ImmWord imm, T address) {
        if ((intptr_t)imm.value <= INT32_MAX && (intptr_t)imm.value >= INT32_MIN) {
            movq(Imm32((int32_t)imm.value), Operand(address));
        } else {
            mov(imm, ScratchReg);
            movq(ScratchReg, Operand(address));
        }
    }
    template <typename T>
    void storePtr(ImmPtr imm, T address) {
        storePtr(ImmWord(uintptr_t(imm.value)), address);
    }
    template <typename T>
    void storePtr(ImmGCPtr imm, T address) {
        movq(imm, ScratchReg);
        movq(ScratchReg, Operand(address));
    }
    void storePtr(Register src, const Address& address) {
        movq(src, Operand(address));
    }
    void storePtr(Register src, const BaseIndex& address) {
        movq(src, Operand(address));
    }
    void storePtr(Register src, const Operand& dest) {
        movq(src, dest);
    }
    void storePtr(Register src, AbsoluteAddress address) {
        if (X86Encoding::IsAddressImmediate(address.addr)) {
            movq(src, Operand(address));
        } else {
            mov(ImmPtr(address.addr), ScratchReg);
            storePtr(src, Address(ScratchReg, 0x0));
        }
    }
    void store32(Register src, AbsoluteAddress address) {
        if (X86Encoding::IsAddressImmediate(address.addr)) {
            movl(src, Operand(address));
        } else {
            mov(ImmPtr(address.addr), ScratchReg);
            store32(src, Address(ScratchReg, 0x0));
        }
    }
    void rshiftPtr(Imm32 imm, Register dest) {
        shrq(imm, dest);
    }
    void rshiftPtrArithmetic(Imm32 imm, Register dest) {
        sarq(imm, dest);
    }
    void lshiftPtr(Imm32 imm, Register dest) {
        shlq(imm, dest);
    }
    void xorPtr(Imm32 imm, Register dest) {
        xorq(imm, dest);
    }
    void xorPtr(Register src, Register dest) {
        xorq(src, dest);
    }
    void orPtr(Imm32 imm, Register dest) {
        orq(imm, dest);
    }
    void orPtr(Register src, Register dest) {
        orq(src, dest);
    }
    void andPtr(Imm32 imm, Register dest) {
        andq(imm, dest);
    }
    void andPtr(Register src, Register dest) {
        andq(src, dest);
    }

    void splitTag(Register src, Register dest) {
        if (src != dest)
            movq(src, dest);
        shrq(Imm32(JSVAL_TAG_SHIFT), dest);
    }
    void splitTag(const ValueOperand& operand, Register dest) {
        splitTag(operand.valueReg(), dest);
    }
    void splitTag(const Operand& operand, Register dest) {
        movq(operand, dest);
        shrq(Imm32(JSVAL_TAG_SHIFT), dest);
    }
    void splitTag(const Address& operand, Register dest) {
        splitTag(Operand(operand), dest);
    }
    void splitTag(const BaseIndex& operand, Register dest) {
        splitTag(Operand(operand), dest);
    }

    // Extracts the tag of a value and places it in ScratchReg.
    Register splitTagForTest(const ValueOperand& value) {
        splitTag(value, ScratchReg);
        return ScratchReg;
    }
    void cmpTag(const ValueOperand& operand, ImmTag tag) {
        Register reg = splitTagForTest(operand);
        cmp32(reg, tag);
    }

    void branchTestUndefined(Condition cond, Register tag, Label* label) {
        cond = testUndefined(cond, tag);
        j(cond, label);
    }
    void branchTestInt32(Condition cond, Register tag, Label* label) {
        cond = testInt32(cond, tag);
        j(cond, label);
    }
    void branchTestDouble(Condition cond, Register tag, Label* label) {
        cond = testDouble(cond, tag);
        j(cond, label);
    }
    void branchTestBoolean(Condition cond, Register tag, Label* label) {
        cond = testBoolean(cond, tag);
        j(cond, label);
    }
    void branchTestNull(Condition cond, Register tag, Label* label) {
        cond = testNull(cond, tag);
        j(cond, label);
    }
    void branchTestString(Condition cond, Register tag, Label* label) {
        cond = testString(cond, tag);
        j(cond, label);
    }
    void branchTestSymbol(Condition cond, Register tag, Label* label) {
        cond = testSymbol(cond, tag);
        j(cond, label);
    }
    void branchTestObject(Condition cond, Register tag, Label* label) {
        cond = testObject(cond, tag);
        j(cond, label);
    }
    void branchTestNumber(Condition cond, Register tag, Label* label) {
        cond = testNumber(cond, tag);
        j(cond, label);
    }

    // x64 can test for certain types directly from memory when the payload
    // of the type is limited to 32 bits. This avoids loading into a register,
    // accesses half as much memory, and removes a right-shift.
    void branchTestUndefined(Condition cond, const Operand& operand, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(ToUpper32(operand), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_UNDEFINED))));
        j(cond, label);
    }
    void branchTestUndefined(Condition cond, const Address& address, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        branchTestUndefined(cond, Operand(address), label);
    }
    void branchTestInt32(Condition cond, const Operand& operand, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(ToUpper32(operand), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_INT32))));
        j(cond, label);
    }
    void branchTestInt32(Condition cond, const Address& address, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        branchTestInt32(cond, Operand(address), label);
    }
    void branchTestDouble(Condition cond, const Operand& operand, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        splitTag(operand, ScratchReg);
        branchTestDouble(cond, ScratchReg, label);
    }
    void branchTestDouble(Condition cond, const Address& address, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        branchTestDouble(cond, Operand(address), label);
    }
    void branchTestBoolean(Condition cond, const Operand& operand, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(ToUpper32(operand), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_BOOLEAN))));
        j(cond, label);
    }
    void branchTestNull(Condition cond, const Operand& operand, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(ToUpper32(operand), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_NULL))));
        j(cond, label);
    }
    void branchTestNull(Condition cond, const Address& address, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        branchTestNull(cond, Operand(address), label);
    }

    // This one, though, clobbers the ScratchReg.
    void branchTestObject(Condition cond, const Address& src, Label* label) {
        cond = testObject(cond, src);
        j(cond, label);
    }

    // Perform a type-test on a full Value loaded into a register.
    // Clobbers the ScratchReg.
    void branchTestUndefined(Condition cond, const ValueOperand& src, Label* label) {
        cond = testUndefined(cond, src);
        j(cond, label);
    }
    void branchTestInt32(Condition cond, const ValueOperand& src, Label* label) {
        splitTag(src, ScratchReg);
        branchTestInt32(cond, ScratchReg, label);
    }
    void branchTestBoolean(Condition cond, const ValueOperand& src, Label* label) {
        splitTag(src, ScratchReg);
        branchTestBoolean(cond, ScratchReg, label);
    }
    void branchTestDouble(Condition cond, const ValueOperand& src, Label* label) {
        cond = testDouble(cond, src);
        j(cond, label);
    }
    void branchTestNull(Condition cond, const ValueOperand& src, Label* label) {
        cond = testNull(cond, src);
        j(cond, label);
    }
    void branchTestString(Condition cond, const ValueOperand& src, Label* label) {
        cond = testString(cond, src);
        j(cond, label);
    }
    void branchTestSymbol(Condition cond, const ValueOperand& src, Label* label) {
        cond = testSymbol(cond, src);
        j(cond, label);
    }
    void branchTestObject(Condition cond, const ValueOperand& src, Label* label) {
        cond = testObject(cond, src);
        j(cond, label);
    }
    void branchTestNumber(Condition cond, const ValueOperand& src, Label* label) {
        cond = testNumber(cond, src);
        j(cond, label);
    }

    // Perform a type-test on a Value addressed by BaseIndex.
    // Clobbers the ScratchReg.
    void branchTestUndefined(Condition cond, const BaseIndex& address, Label* label) {
        cond = testUndefined(cond, address);
        j(cond, label);
    }
    void branchTestInt32(Condition cond, const BaseIndex& address, Label* label) {
        splitTag(address, ScratchReg);
        branchTestInt32(cond, ScratchReg, label);
    }
    void branchTestBoolean(Condition cond, const BaseIndex& address, Label* label) {
        splitTag(address, ScratchReg);
        branchTestBoolean(cond, ScratchReg, label);
    }
    void branchTestDouble(Condition cond, const BaseIndex& address, Label* label) {
        cond = testDouble(cond, address);
        j(cond, label);
    }
    void branchTestNull(Condition cond, const BaseIndex& address, Label* label) {
        cond = testNull(cond, address);
        j(cond, label);
    }
    void branchTestString(Condition cond, const BaseIndex& address, Label* label) {
        cond = testString(cond, address);
        j(cond, label);
    }
    void branchTestSymbol(Condition cond, const BaseIndex& address, Label* label) {
        cond = testSymbol(cond, address);
        j(cond, label);
    }
    void branchTestObject(Condition cond, const BaseIndex& address, Label* label) {
        cond = testObject(cond, address);
        j(cond, label);
    }

    template <typename T>
    void branchTestGCThing(Condition cond, const T& src, Label* label) {
        cond = testGCThing(cond, src);
        j(cond, label);
    }
    template <typename T>
    void branchTestPrimitive(Condition cond, const T& t, Label* label) {
        cond = testPrimitive(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestMagic(Condition cond, const T& t, Label* label) {
        cond = testMagic(cond, t);
        j(cond, label);
    }
    void branchTestMagicValue(Condition cond, const ValueOperand& val, JSWhyMagic why,
                              Label* label)
    {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        branchTestValue(cond, val, MagicValue(why), label);
    }
    Condition testMagic(Condition cond, const ValueOperand& src) {
        splitTag(src, ScratchReg);
        return testMagic(cond, ScratchReg);
    }
    Condition testError(Condition cond, const ValueOperand& src) {
        return testMagic(cond, src);
    }
    void branchTestValue(Condition cond, const ValueOperand& value, const Value& v, Label* label) {
        MOZ_ASSERT(value.valueReg() != ScratchReg);
        moveValue(v, ScratchReg);
        cmpPtr(value.valueReg(), ScratchReg);
        j(cond, label);
    }
    void branchTestValue(Condition cond, const Address& valaddr, const ValueOperand& value,
                         Label* label)
    {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        branchPtr(cond, valaddr, value.valueReg(), label);
    }

    void testNullSet(Condition cond, const ValueOperand& value, Register dest) {
        cond = testNull(cond, value);
        emitSet(cond, dest);
    }

    void testObjectSet(Condition cond, const ValueOperand& value, Register dest) {
        cond = testObject(cond, value);
        emitSet(cond, dest);
    }

    void testUndefinedSet(Condition cond, const ValueOperand& value, Register dest) {
        cond = testUndefined(cond, value);
        emitSet(cond, dest);
    }

    void boxDouble(FloatRegister src, const ValueOperand& dest) {
        vmovq(src, dest.valueReg());
    }
    void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest) {
        MOZ_ASSERT(src != dest.valueReg());
        boxValue(type, src, dest.valueReg());
    }

    // Note that the |dest| register here may be ScratchReg, so we shouldn't
    // use it.
    void unboxInt32(const ValueOperand& src, Register dest) {
        movl(src.valueReg(), dest);
    }
    void unboxInt32(const Operand& src, Register dest) {
        movl(src, dest);
    }
    void unboxInt32(const Address& src, Register dest) {
        unboxInt32(Operand(src), dest);
    }
    void unboxDouble(const Address& src, FloatRegister dest) {
        loadDouble(Operand(src), dest);
    }

    void unboxArgObjMagic(const ValueOperand& src, Register dest) {
        unboxArgObjMagic(Operand(src.valueReg()), dest);
    }
    void unboxArgObjMagic(const Operand& src, Register dest) {
        mov(ImmWord(0), dest);
    }
    void unboxArgObjMagic(const Address& src, Register dest) {
        unboxArgObjMagic(Operand(src), dest);
    }

    void unboxBoolean(const ValueOperand& src, Register dest) {
        movl(src.valueReg(), dest);
    }
    void unboxBoolean(const Operand& src, Register dest) {
        movl(src, dest);
    }
    void unboxBoolean(const Address& src, Register dest) {
        unboxBoolean(Operand(src), dest);
    }

    void unboxMagic(const ValueOperand& src, Register dest) {
        movl(src.valueReg(), dest);
    }

    void unboxDouble(const ValueOperand& src, FloatRegister dest) {
        vmovq(src.valueReg(), dest);
    }
    void unboxPrivate(const ValueOperand& src, const Register dest) {
        movq(src.valueReg(), dest);
        shlq(Imm32(1), dest);
    }

    void notBoolean(const ValueOperand& val) {
        xorq(Imm32(1), val.valueReg());
    }

    // Unbox any non-double value into dest. Prefer unboxInt32 or unboxBoolean
    // instead if the source type is known.
    void unboxNonDouble(const ValueOperand& src, Register dest) {
        if (src.valueReg() == dest) {
            mov(ImmWord(JSVAL_PAYLOAD_MASK), ScratchReg);
            andq(ScratchReg, dest);
        } else {
            mov(ImmWord(JSVAL_PAYLOAD_MASK), dest);
            andq(src.valueReg(), dest);
        }
    }
    void unboxNonDouble(const Operand& src, Register dest) {
        // Explicitly permits |dest| to be used in |src|.
        MOZ_ASSERT(dest != ScratchReg);
        if (src.containsReg(dest)) {
            mov(ImmWord(JSVAL_PAYLOAD_MASK), ScratchReg);
            // If src is already a register, then src and dest are the same
            // thing and we don't need to move anything into dest.
            if (src.kind() != Operand::REG)
                movq(src, dest);
            andq(ScratchReg, dest);
        } else {
            mov(ImmWord(JSVAL_PAYLOAD_MASK), dest);
            andq(src, dest);
        }
    }

    void unboxString(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxString(const Operand& src, Register dest) { unboxNonDouble(src, dest); }

    void unboxSymbol(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxSymbol(const Operand& src, Register dest) { unboxNonDouble(src, dest); }

    void unboxObject(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxObject(const Operand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxObject(const Address& src, Register dest) { unboxNonDouble(Operand(src), dest); }
    void unboxObject(const BaseIndex& src, Register dest) { unboxNonDouble(Operand(src), dest); }

    // Extended unboxing API. If the payload is already in a register, returns
    // that register. Otherwise, provides a move to the given scratch register,
    // and returns that.
    Register extractObject(const Address& address, Register scratch) {
        MOZ_ASSERT(scratch != ScratchReg);
        unboxObject(address, scratch);
        return scratch;
    }
    Register extractObject(const ValueOperand& value, Register scratch) {
        MOZ_ASSERT(scratch != ScratchReg);
        unboxObject(value, scratch);
        return scratch;
    }
    Register extractInt32(const ValueOperand& value, Register scratch) {
        MOZ_ASSERT(scratch != ScratchReg);
        unboxInt32(value, scratch);
        return scratch;
    }
    Register extractBoolean(const ValueOperand& value, Register scratch) {
        MOZ_ASSERT(scratch != ScratchReg);
        unboxBoolean(value, scratch);
        return scratch;
    }
    Register extractTag(const Address& address, Register scratch) {
        MOZ_ASSERT(scratch != ScratchReg);
        loadPtr(address, scratch);
        splitTag(scratch, scratch);
        return scratch;
    }
    Register extractTag(const ValueOperand& value, Register scratch) {
        MOZ_ASSERT(scratch != ScratchReg);
        splitTag(value, scratch);
        return scratch;
    }

    void unboxValue(const ValueOperand& src, AnyRegister dest) {
        if (dest.isFloat()) {
            Label notInt32, end;
            branchTestInt32(Assembler::NotEqual, src, &notInt32);
            convertInt32ToDouble(src.valueReg(), dest.fpu());
            jump(&end);
            bind(&notInt32);
            unboxDouble(src, dest.fpu());
            bind(&end);
        } else {
            unboxNonDouble(src, dest.gpr());
        }
    }

    // These two functions use the low 32-bits of the full value register.
    void boolValueToDouble(const ValueOperand& operand, FloatRegister dest) {
        convertInt32ToDouble(operand.valueReg(), dest);
    }
    void int32ValueToDouble(const ValueOperand& operand, FloatRegister dest) {
        convertInt32ToDouble(operand.valueReg(), dest);
    }

    void boolValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
        convertInt32ToFloat32(operand.valueReg(), dest);
    }
    void int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
        convertInt32ToFloat32(operand.valueReg(), dest);
    }

    void loadConstantDouble(double d, FloatRegister dest);
    void loadConstantFloat32(float f, FloatRegister dest);
  private:
    SimdData* getSimdData(const SimdConstant& v);
  public:
    void loadConstantInt32x4(const SimdConstant& v, FloatRegister dest);
    void loadConstantFloat32x4(const SimdConstant& v, FloatRegister dest);

    void branchTruncateDouble(FloatRegister src, Register dest, Label* fail) {
        vcvttsd2sq(src, dest);

        // vcvttsd2sq returns 0x8000000000000000 on failure. Test for it by
        // subtracting 1 and testing overflow (this avoids the need to
        // materialize that value in a register).
        cmpPtr(dest, Imm32(1));
        j(Assembler::Overflow, fail);

        movl(dest, dest); // Zero upper 32-bits.
    }
    void branchTruncateFloat32(FloatRegister src, Register dest, Label* fail) {
        vcvttss2sq(src, dest);

        // Same trick as for Doubles
        cmpPtr(dest, Imm32(1));
        j(Assembler::Overflow, fail);

        movl(dest, dest); // Zero upper 32-bits.
    }

    Condition testInt32Truthy(bool truthy, const ValueOperand& operand) {
        test32(operand.valueReg(), operand.valueReg());
        return truthy ? NonZero : Zero;
    }
    void branchTestInt32Truthy(bool truthy, const ValueOperand& operand, Label* label) {
        Condition cond = testInt32Truthy(truthy, operand);
        j(cond, label);
    }
    void branchTestBooleanTruthy(bool truthy, const ValueOperand& operand, Label* label) {
        test32(operand.valueReg(), operand.valueReg());
        j(truthy ? NonZero : Zero, label);
    }
    Condition testStringTruthy(bool truthy, const ValueOperand& value) {
        unboxString(value, ScratchReg);
        cmp32(Operand(ScratchReg, JSString::offsetOfLength()), Imm32(0));
        return truthy ? Assembler::NotEqual : Assembler::Equal;
    }
    void branchTestStringTruthy(bool truthy, const ValueOperand& value, Label* label) {
        Condition cond = testStringTruthy(truthy, value);
        j(cond, label);
    }

    void loadInt32OrDouble(const Operand& operand, FloatRegister dest) {
        Label notInt32, end;
        branchTestInt32(Assembler::NotEqual, operand, &notInt32);
        convertInt32ToDouble(operand, dest);
        jump(&end);
        bind(&notInt32);
        loadDouble(operand, dest);
        bind(&end);
    }

    template <typename T>
    void loadUnboxedValue(const T& src, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(Operand(src), dest.fpu());
        else if (type == MIRType_Int32 || type == MIRType_Boolean)
            movl(Operand(src), dest.gpr());
        else
            unboxNonDouble(Operand(src), dest.gpr());
    }

    template <typename T>
    void storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const T& dest, MIRType slotType);

    template <typename T>
    void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes) {
        switch (nbytes) {
          case 8:
            unboxNonDouble(value, ScratchReg);
            storePtr(ScratchReg, address);
            return;
          case 4:
            store32(value.valueReg(), address);
            return;
          case 1:
            store8(value.valueReg(), address);
            return;
          default: MOZ_CRASH("Bad payload width");
        }
    }

    void loadInstructionPointerAfterCall(Register dest) {
        loadPtr(Address(StackPointer, 0x0), dest);
    }

    void convertUInt32ToDouble(Register src, FloatRegister dest) {
        vcvtsq2sd(src, dest, dest);
    }

    void convertUInt32ToFloat32(Register src, FloatRegister dest) {
        vcvtsq2ss(src, dest, dest);
    }

    void inc64(AbsoluteAddress dest) {
        if (X86Encoding::IsAddressImmediate(dest.addr)) {
            addPtr(Imm32(1), Operand(dest));
        } else {
            mov(ImmPtr(dest.addr), ScratchReg);
            addPtr(Imm32(1), Address(ScratchReg, 0));
        }
    }

    void incrementInt32Value(const Address& addr) {
        addPtr(Imm32(1), addr);
    }

    // If source is a double, load it into dest. If source is int32,
    // convert it to double. Else, branch to failure.
    void ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure) {
        Label isDouble, done;
        Register tag = splitTagForTest(source);
        branchTestDouble(Assembler::Equal, tag, &isDouble);
        branchTestInt32(Assembler::NotEqual, tag, failure);

        unboxInt32(source, ScratchReg);
        convertInt32ToDouble(ScratchReg, dest);
        jump(&done);

        bind(&isDouble);
        unboxDouble(source, dest);

        bind(&done);
    }

    // Setup a call to C/C++ code, given the number of general arguments it
    // takes. Note that this only supports cdecl.
    //
    // In order for alignment to work correctly, the MacroAssembler must have a
    // consistent view of the stack displacement. It is okay to call "push"
    // manually, however, if the stack alignment were to change, the macro
    // assembler should be notified before starting a call.
    void setupAlignedABICall(uint32_t args);

    // Sets up an ABI call for when the alignment is not known. This may need a
    // scratch register.
    void setupUnalignedABICall(uint32_t args, Register scratch);

    // Arguments must be assigned to a C/C++ call in order. They are moved
    // in parallel immediately before performing the call. This process may
    // temporarily use more stack, in which case esp-relative addresses will be
    // automatically adjusted. It is extremely important that esp-relative
    // addresses are computed *after* setupABICall(). Furthermore, no
    // operations should be emitted while setting arguments.
    void passABIArg(const MoveOperand& from, MoveOp::Type type);
    void passABIArg(Register reg);
    void passABIArg(FloatRegister reg, MoveOp::Type type);

  private:
    void callWithABIPre(uint32_t* stackAdjust);
    void callWithABIPost(uint32_t stackAdjust, MoveOp::Type result);

  public:
    // Emits a call to a C/C++ function, resolving all argument moves.
    void callWithABI(void* fun, MoveOp::Type result = MoveOp::GENERAL);
    void callWithABI(AsmJSImmPtr imm, MoveOp::Type result = MoveOp::GENERAL);
    void callWithABI(Address fun, MoveOp::Type result = MoveOp::GENERAL);
    void callWithABI(Register fun, MoveOp::Type result = MoveOp::GENERAL);

    void handleFailureWithHandlerTail(void* handler);

    void makeFrameDescriptor(Register frameSizeReg, FrameType type) {
        shlq(Imm32(FRAMESIZE_SHIFT), frameSizeReg);
        orq(Imm32(type), frameSizeReg);
    }

    void callWithExitFrame(JitCode* target, Register dynStack) {
        addPtr(Imm32(framePushed()), dynStack);
        makeFrameDescriptor(dynStack, JitFrame_IonJS);
        Push(dynStack);
        call(target);
    }

    // See CodeGeneratorX64 calls to noteAsmJSGlobalAccess.
    void patchAsmJSGlobalAccess(CodeOffsetLabel patchAt, uint8_t* code, uint8_t* globalData,
                                unsigned globalDataOffset)
    {
        uint8_t* nextInsn = code + patchAt.offset();
        MOZ_ASSERT(nextInsn <= globalData);
        uint8_t* target = globalData + globalDataOffset;
        ((int32_t*)nextInsn)[-1] = target - nextInsn;
    }
    void memIntToValue(Address Source, Address Dest) {
        load32(Source, ScratchReg);
        storeValue(JSVAL_TYPE_INT32, ScratchReg, Dest);
    }

    void branchPtrInNurseryRange(Condition cond, Register ptr, Register temp, Label* label);
    void branchValueIsNurseryObject(Condition cond, ValueOperand value, Register temp, Label* label);

    // Instrumentation for entering and leaving the profiler.
    void profilerEnterFrame(Register framePtr, Register scratch);
    void profilerExitFrame();
};

typedef MacroAssemblerX64 MacroAssemblerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x64_MacroAssembler_x64_h */
