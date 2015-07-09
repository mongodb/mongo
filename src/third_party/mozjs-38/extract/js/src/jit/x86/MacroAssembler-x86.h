/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_MacroAssembler_x86_h
#define jit_x86_MacroAssembler_x86_h

#include "jscompartment.h"

#include "jit/JitFrames.h"
#include "jit/MoveResolver.h"
#include "jit/shared/MacroAssembler-x86-shared.h"

namespace js {
namespace jit {

class MacroAssemblerX86 : public MacroAssemblerX86Shared
{
    // Number of bytes the stack is adjusted inside a call to C. Calls to C may
    // not be nested.
    bool inCall_;
    uint32_t args_;
    uint32_t passedArgs_;
    uint32_t stackForCall_;
    bool dynamicAlignment_;

    struct Double {
        double value;
        AbsoluteLabel uses;
        Double(double value) : value(value) {}
    };
    Vector<Double, 0, SystemAllocPolicy> doubles_;
    struct Float {
        float value;
        AbsoluteLabel uses;
        Float(float value) : value(value) {}
    };
    Vector<Float, 0, SystemAllocPolicy> floats_;
    struct SimdData {
        SimdConstant value;
        AbsoluteLabel uses;
        SimdData(const SimdConstant& v) : value(v) {}
        SimdConstant::Type type() { return value.type(); }
    };
    Vector<SimdData, 0, SystemAllocPolicy> simds_;

    typedef HashMap<double, size_t, DefaultHasher<double>, SystemAllocPolicy> DoubleMap;
    DoubleMap doubleMap_;
    typedef HashMap<float, size_t, DefaultHasher<float>, SystemAllocPolicy> FloatMap;
    FloatMap floatMap_;
    typedef HashMap<SimdConstant, size_t, SimdConstant, SystemAllocPolicy> SimdMap;
    SimdMap simdMap_;

    Double* getDouble(double d);
    Float* getFloat(float f);
    SimdData* getSimdData(const SimdConstant& v);

  protected:
    MoveResolver moveResolver_;

  private:
    Operand payloadOf(const Address& address) {
        return Operand(address.base, address.offset);
    }
    Operand payloadOf(const BaseIndex& address) {
        return Operand(address.base, address.index, address.scale, address.offset);
    }
    Operand tagOf(const Address& address) {
        return Operand(address.base, address.offset + 4);
    }
    Operand tagOf(const BaseIndex& address) {
        return Operand(address.base, address.index, address.scale, address.offset + 4);
    }

    void setupABICall(uint32_t args);

  public:
    using MacroAssemblerX86Shared::Push;
    using MacroAssemblerX86Shared::Pop;
    using MacroAssemblerX86Shared::callWithExitFrame;
    using MacroAssemblerX86Shared::branch32;
    using MacroAssemblerX86Shared::branchTest32;
    using MacroAssemblerX86Shared::load32;
    using MacroAssemblerX86Shared::store32;
    using MacroAssemblerX86Shared::call;

    MacroAssemblerX86()
      : inCall_(false)
    {
    }

    // The buffer is about to be linked, make sure any constant pools or excess
    // bookkeeping has been flushed to the instruction stream.
    void finish();

    /////////////////////////////////////////////////////////////////
    // X86-specific interface.
    /////////////////////////////////////////////////////////////////

    Operand ToPayload(Operand base) {
        return base;
    }
    Address ToPayload(Address base) {
        return base;
    }
    Operand ToType(Operand base) {
        switch (base.kind()) {
          case Operand::MEM_REG_DISP:
            return Operand(Register::FromCode(base.base()), base.disp() + sizeof(void*));

          case Operand::MEM_SCALE:
            return Operand(Register::FromCode(base.base()), Register::FromCode(base.index()),
                           base.scale(), base.disp() + sizeof(void*));

          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    Address ToType(Address base) {
        return ToType(Operand(base)).toAddress();
    }
    void moveValue(const Value& val, Register type, Register data) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        movl(Imm32(jv.s.tag), type);
        if (val.isMarkable())
            movl(ImmGCPtr(reinterpret_cast<gc::Cell*>(val.toGCThing())), data);
        else
            movl(Imm32(jv.s.payload.i32), data);
    }
    void moveValue(const Value& val, const ValueOperand& dest) {
        moveValue(val, dest.typeReg(), dest.payloadReg());
    }
    void moveValue(const ValueOperand& src, const ValueOperand& dest) {
        Register s0 = src.typeReg(), d0 = dest.typeReg(),
                 s1 = src.payloadReg(), d1 = dest.payloadReg();

        // Either one or both of the source registers could be the same as a
        // destination register.
        if (s1 == d0) {
            if (s0 == d1) {
                // If both are, this is just a swap of two registers.
                xchgl(d0, d1);
                return;
            }
            // If only one is, copy that source first.
            mozilla::Swap(s0, s1);
            mozilla::Swap(d0, d1);
        }

        if (s0 != d0)
            movl(s0, d0);
        if (s1 != d1)
            movl(s1, d1);
    }

    /////////////////////////////////////////////////////////////////
    // X86/X64-common interface.
    /////////////////////////////////////////////////////////////////
    void storeValue(ValueOperand val, Operand dest) {
        movl(val.payloadReg(), ToPayload(dest));
        movl(val.typeReg(), ToType(dest));
    }
    void storeValue(ValueOperand val, const Address& dest) {
        storeValue(val, Operand(dest));
    }
    template <typename T>
    void storeValue(JSValueType type, Register reg, const T& dest) {
        storeTypeTag(ImmTag(JSVAL_TYPE_TO_TAG(type)), Operand(dest));
        storePayload(reg, Operand(dest));
    }
    template <typename T>
    void storeValue(const Value& val, const T& dest) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        storeTypeTag(ImmTag(jv.s.tag), Operand(dest));
        storePayload(val, Operand(dest));
    }
    void storeValue(ValueOperand val, BaseIndex dest) {
        storeValue(val, Operand(dest));
    }
    void loadValue(Operand src, ValueOperand val) {
        Operand payload = ToPayload(src);
        Operand type = ToType(src);

        // Ensure that loading the payload does not erase the pointer to the
        // Value in memory or the index.
        Register baseReg = Register::FromCode(src.base());
        Register indexReg = (src.kind() == Operand::MEM_SCALE) ? Register::FromCode(src.index()) : InvalidReg;

        if (baseReg == val.payloadReg() || indexReg == val.payloadReg()) {
            MOZ_ASSERT(baseReg != val.typeReg());
            MOZ_ASSERT(indexReg != val.typeReg());

            movl(type, val.typeReg());
            movl(payload, val.payloadReg());
        } else {
            MOZ_ASSERT(baseReg != val.payloadReg());
            MOZ_ASSERT(indexReg != val.payloadReg());

            movl(payload, val.payloadReg());
            movl(type, val.typeReg());
        }
    }
    void loadValue(Address src, ValueOperand val) {
        loadValue(Operand(src), val);
    }
    void loadValue(const BaseIndex& src, ValueOperand val) {
        loadValue(Operand(src), val);
    }
    void tagValue(JSValueType type, Register payload, ValueOperand dest) {
        MOZ_ASSERT(dest.typeReg() != dest.payloadReg());
        if (payload != dest.payloadReg())
            movl(payload, dest.payloadReg());
        movl(ImmType(type), dest.typeReg());
    }
    void pushValue(ValueOperand val) {
        push(val.typeReg());
        push(val.payloadReg());
    }
    void popValue(ValueOperand val) {
        pop(val.payloadReg());
        pop(val.typeReg());
    }
    void pushValue(const Value& val) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        push(Imm32(jv.s.tag));
        if (val.isMarkable())
            push(ImmMaybeNurseryPtr(reinterpret_cast<gc::Cell*>(val.toGCThing())));
        else
            push(Imm32(jv.s.payload.i32));
    }
    void pushValue(JSValueType type, Register reg) {
        push(ImmTag(JSVAL_TYPE_TO_TAG(type)));
        push(reg);
    }
    void pushValue(const Address& addr) {
        push(tagOf(addr));
        push(payloadOf(addr));
    }
    void Push(const ValueOperand& val) {
        pushValue(val);
        framePushed_ += sizeof(Value);
    }
    void Pop(const ValueOperand& val) {
        popValue(val);
        framePushed_ -= sizeof(Value);
    }
    void storePayload(const Value& val, Operand dest) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        if (val.isMarkable())
            movl(ImmGCPtr((gc::Cell*)jv.s.payload.ptr), ToPayload(dest));
        else
            movl(Imm32(jv.s.payload.i32), ToPayload(dest));
    }
    void storePayload(Register src, Operand dest) {
        movl(src, ToPayload(dest));
    }
    void storeTypeTag(ImmTag tag, Operand dest) {
        movl(tag, ToType(dest));
    }

    void movePtr(Register src, Register dest) {
        movl(src, dest);
    }
    void movePtr(Register src, const Operand& dest) {
        movl(src, dest);
    }

    // Returns the register containing the type tag.
    Register splitTagForTest(const ValueOperand& value) {
        return value.typeReg();
    }

    Condition testUndefined(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_UNDEFINED));
        return cond;
    }
    Condition testBoolean(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_BOOLEAN));
        return cond;
    }
    Condition testInt32(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_INT32));
        return cond;
    }
    Condition testDouble(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
        Condition actual = (cond == Equal) ? Below : AboveOrEqual;
        cmp32(tag, ImmTag(JSVAL_TAG_CLEAR));
        return actual;
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
    Condition testNumber(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET));
        return cond == Equal ? BelowOrEqual : Above;
    }
    Condition testGCThing(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET));
        return cond == Equal ? AboveOrEqual : Below;
    }
    Condition testGCThing(Condition cond, const Address& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET));
        return cond == Equal ? AboveOrEqual : Below;
    }
    Condition testMagic(Condition cond, const Address& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_MAGIC));
        return cond;
    }
    Condition testMagic(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_TAG_MAGIC));
        return cond;
    }
    Condition testMagic(Condition cond, const Operand& operand) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(ToType(operand), ImmTag(JSVAL_TAG_MAGIC));
        return cond;
    }
    Condition testPrimitive(Condition cond, Register tag) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tag, ImmTag(JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET));
        return cond == Equal ? Below : AboveOrEqual;
    }
    Condition testError(Condition cond, Register tag) {
        return testMagic(cond, tag);
    }
    Condition testInt32(Condition cond, const Operand& operand) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(ToType(operand), ImmTag(JSVAL_TAG_INT32));
        return cond;
    }
    Condition testInt32(Condition cond, const Address& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        return testInt32(cond, Operand(address));
    }
    Condition testObject(Condition cond, const Operand& operand) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(ToType(operand), ImmTag(JSVAL_TAG_OBJECT));
        return cond;
    }
    Condition testObject(Condition cond, const Address& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        return testObject(cond, Operand(address));
    }
    Condition testDouble(Condition cond, const Operand& operand) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        Condition actual = (cond == Equal) ? Below : AboveOrEqual;
        cmp32(ToType(operand), ImmTag(JSVAL_TAG_CLEAR));
        return actual;
    }
    Condition testDouble(Condition cond, const Address& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        return testDouble(cond, Operand(address));
    }


    Condition testUndefined(Condition cond, const Operand& operand) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(ToType(operand), ImmTag(JSVAL_TAG_UNDEFINED));
        return cond;
    }
    Condition testUndefined(Condition cond, const Address& addr) {
        return testUndefined(cond, Operand(addr));
    }
    Condition testNull(Condition cond, const Operand& operand) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(ToType(operand), ImmTag(JSVAL_TAG_NULL));
        return cond;
    }
    Condition testNull(Condition cond, const Address& addr) {
        return testNull(cond, Operand(addr));
    }

    Condition testUndefined(Condition cond, const ValueOperand& value) {
        return testUndefined(cond, value.typeReg());
    }
    Condition testBoolean(Condition cond, const ValueOperand& value) {
        return testBoolean(cond, value.typeReg());
    }
    Condition testInt32(Condition cond, const ValueOperand& value) {
        return testInt32(cond, value.typeReg());
    }
    Condition testDouble(Condition cond, const ValueOperand& value) {
        return testDouble(cond, value.typeReg());
    }
    Condition testNull(Condition cond, const ValueOperand& value) {
        return testNull(cond, value.typeReg());
    }
    Condition testString(Condition cond, const ValueOperand& value) {
        return testString(cond, value.typeReg());
    }
    Condition testSymbol(Condition cond, const ValueOperand& value) {
        return testSymbol(cond, value.typeReg());
    }
    Condition testObject(Condition cond, const ValueOperand& value) {
        return testObject(cond, value.typeReg());
    }
    Condition testMagic(Condition cond, const ValueOperand& value) {
        return testMagic(cond, value.typeReg());
    }
    Condition testError(Condition cond, const ValueOperand& value) {
        return testMagic(cond, value);
    }
    Condition testNumber(Condition cond, const ValueOperand& value) {
        return testNumber(cond, value.typeReg());
    }
    Condition testGCThing(Condition cond, const ValueOperand& value) {
        return testGCThing(cond, value.typeReg());
    }
    Condition testPrimitive(Condition cond, const ValueOperand& value) {
        return testPrimitive(cond, value.typeReg());
    }


    Condition testUndefined(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_UNDEFINED));
        return cond;
    }
    Condition testNull(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_NULL));
        return cond;
    }
    Condition testBoolean(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_BOOLEAN));
        return cond;
    }
    Condition testString(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_STRING));
        return cond;
    }
    Condition testSymbol(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_SYMBOL));
        return cond;
    }
    Condition testInt32(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_INT32));
        return cond;
    }
    Condition testObject(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_OBJECT));
        return cond;
    }
    Condition testDouble(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        Condition actual = (cond == Equal) ? Below : AboveOrEqual;
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_CLEAR));
        return actual;
    }
    Condition testMagic(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_TAG_MAGIC));
        return cond;
    }
    Condition testGCThing(Condition cond, const BaseIndex& address) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        cmp32(tagOf(address), ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET));
        return cond == Equal ? AboveOrEqual : Below;
    }



    void branchTestValue(Condition cond, const ValueOperand& value, const Value& v, Label* label);
    void branchTestValue(Condition cond, const Address& valaddr, const ValueOperand& value,
                         Label* label)
    {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        // Check payload before tag, since payload is more likely to differ.
        if (cond == NotEqual) {
            branchPtr(NotEqual, payloadOf(valaddr), value.payloadReg(), label);
            branchPtr(NotEqual, tagOf(valaddr), value.typeReg(), label);

        } else {
            Label fallthrough;
            branchPtr(NotEqual, payloadOf(valaddr), value.payloadReg(), &fallthrough);
            branchPtr(Equal, tagOf(valaddr), value.typeReg(), label);
            bind(&fallthrough);
        }
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

    void cmpPtr(Register lhs, const ImmWord rhs) {
        cmpl(Imm32(rhs.value), lhs);
    }
    void cmpPtr(Register lhs, const ImmPtr imm) {
        cmpPtr(lhs, ImmWord(uintptr_t(imm.value)));
    }
    void cmpPtr(Register lhs, const ImmGCPtr rhs) {
        cmpl(rhs, lhs);
    }
    void cmpPtr(const Operand& lhs, Imm32 rhs) {
        cmp32(lhs, rhs);
    }
    void cmpPtr(const Operand& lhs, const ImmWord rhs) {
        cmp32(lhs, Imm32(rhs.value));
    }
    void cmpPtr(const Operand& lhs, const ImmPtr imm) {
        cmpPtr(lhs, ImmWord(uintptr_t(imm.value)));
    }
    void cmpPtr(const Operand& lhs, const ImmGCPtr rhs) {
        cmpl(rhs, lhs);
    }
    void cmpPtr(const Address& lhs, Register rhs) {
        cmpPtr(Operand(lhs), rhs);
    }
    void cmpPtr(const Operand& lhs, Register rhs) {
        cmp32(lhs, rhs);
    }
    void cmpPtr(const Address& lhs, const ImmWord rhs) {
        cmpPtr(Operand(lhs), rhs);
    }
    void cmpPtr(const Address& lhs, const ImmPtr rhs) {
        cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
    }
    void cmpPtr(const Address& lhs, const ImmGCPtr rhs) {
        cmpPtr(Operand(lhs), rhs);
    }
    void cmpPtr(Register lhs, Register rhs) {
        cmp32(lhs, rhs);
    }
    void cmpPtr(const Operand& lhs, ImmMaybeNurseryPtr rhs) {
        cmpl(rhs, lhs);
    }
    void testPtr(Register lhs, Register rhs) {
        test32(lhs, rhs);
    }
    void testPtr(Register lhs, Imm32 rhs) {
        test32(lhs, rhs);
    }
    void testPtr(Register lhs, ImmWord rhs) {
        test32(lhs, Imm32(rhs.value));
    }
    void testPtr(const Operand& lhs, Imm32 rhs) {
        test32(lhs, rhs);
    }
    void testPtr(const Operand& lhs, ImmWord rhs) {
        test32(lhs, Imm32(rhs.value));
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
                subl(Imm32(4096), StackPointer);
                store32(Imm32(0), Address(StackPointer, 0));
                amountLeft -= 4096;
            }
            subl(Imm32(amountLeft), StackPointer);
        }
        framePushed_ += amount;
    }
    void freeStack(uint32_t amount) {
        MOZ_ASSERT(amount <= framePushed_);
        if (amount)
            addl(Imm32(amount), StackPointer);
        framePushed_ -= amount;
    }
    void freeStack(Register amount) {
        addl(amount, StackPointer);
    }

    void addPtr(Register src, Register dest) {
        add32(src, dest);
    }
    void addPtr(Imm32 imm, Register dest) {
        add32(imm, dest);
    }
    void addPtr(ImmWord imm, Register dest) {
        add32(Imm32(imm.value), dest);
    }
    void addPtr(ImmPtr imm, Register dest) {
        addPtr(ImmWord(uintptr_t(imm.value)), dest);
    }
    void addPtr(Imm32 imm, const Address& dest) {
        add32(imm, Operand(dest));
    }
    void addPtr(Imm32 imm, const Operand& dest) {
        add32(imm, dest);
    }
    void addPtr(const Address& src, Register dest) {
        addl(Operand(src), dest);
    }
    void subPtr(Imm32 imm, Register dest) {
        sub32(imm, dest);
    }
    void subPtr(Register src, Register dest) {
        sub32(src, dest);
    }
    void subPtr(const Address& addr, Register dest) {
        sub32(Operand(addr), dest);
    }
    void subPtr(Register src, const Address& dest) {
        sub32(src, Operand(dest));
    }
    void mulBy3(const Register& src, const Register& dest) {
        lea(Operand(src, src, TimesTwo), dest);
    }

    void branch32(Condition cond, AbsoluteAddress lhs, Imm32 rhs, Label* label) {
        cmp32(Operand(lhs), rhs);
        j(cond, label);
    }
    void branch32(Condition cond, AsmJSAbsoluteAddress lhs, Imm32 rhs, Label* label) {
        cmpl(rhs, lhs);
        j(cond, label);
    }
    void branch32(Condition cond, AbsoluteAddress lhs, Register rhs, Label* label) {
        cmp32(Operand(lhs), rhs);
        j(cond, label);
    }
    void branchTest32(Condition cond, AbsoluteAddress address, Imm32 imm, Label* label) {
        test32(Operand(address), imm);
        j(cond, label);
    }

    // Specialization for AsmJSAbsoluteAddress.
    void branchPtr(Condition cond, AsmJSAbsoluteAddress lhs, Register ptr, Label* label) {
        cmpl(ptr, lhs);
        j(cond, label);
    }

    template <typename T, typename S>
    void branchPtr(Condition cond, T lhs, S ptr, Label* label) {
        cmpPtr(Operand(lhs), ptr);
        j(cond, label);
    }

    void branchPrivatePtr(Condition cond, const Address& lhs, ImmPtr ptr, Label* label) {
        branchPtr(cond, lhs, ptr, label);
    }

    void branchPrivatePtr(Condition cond, const Address& lhs, Register ptr, Label* label) {
        branchPtr(cond, lhs, ptr, label);
    }

    template <typename T, typename S>
    void branchPtr(Condition cond, T lhs, S ptr, RepatchLabel* label) {
        cmpPtr(Operand(lhs), ptr);
        j(cond, label);
    }

    CodeOffsetJump jumpWithPatch(RepatchLabel* label) {
        jump(label);
        return CodeOffsetJump(size());
    }

    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Assembler::Condition cond) {
        j(cond, label);
        return CodeOffsetJump(size());
    }

    CodeOffsetJump backedgeJump(RepatchLabel* label) {
        return jumpWithPatch(label);
    }

    template <typename S, typename T>
    CodeOffsetJump branchPtrWithPatch(Condition cond, S lhs, T ptr, RepatchLabel* label) {
        branchPtr(cond, lhs, ptr, label);
        return CodeOffsetJump(size());
    }
    void branchPtr(Condition cond, Register lhs, Register rhs, RepatchLabel* label) {
        cmpPtr(lhs, rhs);
        j(cond, label);
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

    void movePtr(ImmWord imm, Register dest) {
        movl(Imm32(imm.value), dest);
    }
    void movePtr(ImmPtr imm, Register dest) {
        movl(imm, dest);
    }
    void movePtr(AsmJSImmPtr imm, Register dest) {
        mov(imm, dest);
    }
    void movePtr(ImmGCPtr imm, Register dest) {
        movl(imm, dest);
    }
    void movePtr(ImmMaybeNurseryPtr imm, Register dest) {
        movePtr(noteMaybeNurseryPtr(imm), dest);
    }
    void loadPtr(const Address& address, Register dest) {
        movl(Operand(address), dest);
    }
    void loadPtr(const Operand& src, Register dest) {
        movl(src, dest);
    }
    void loadPtr(const BaseIndex& src, Register dest) {
        movl(Operand(src), dest);
    }
    void loadPtr(AbsoluteAddress address, Register dest) {
        movl(Operand(address), dest);
    }
    void loadPrivate(const Address& src, Register dest) {
        movl(payloadOf(src), dest);
    }
    void load32(AbsoluteAddress address, Register dest) {
        movl(Operand(address), dest);
    }
    template <typename T>
    void storePtr(ImmWord imm, T address) {
        movl(Imm32(imm.value), Operand(address));
    }
    template <typename T>
    void storePtr(ImmPtr imm, T address) {
        storePtr(ImmWord(uintptr_t(imm.value)), address);
    }
    template <typename T>
    void storePtr(ImmGCPtr imm, T address) {
        movl(imm, Operand(address));
    }
    void storePtr(Register src, const Address& address) {
        movl(src, Operand(address));
    }
    void storePtr(Register src, const BaseIndex& address) {
        movl(src, Operand(address));
    }
    void storePtr(Register src, const Operand& dest) {
        movl(src, dest);
    }
    void storePtr(Register src, AbsoluteAddress address) {
        movl(src, Operand(address));
    }
    void store32(Register src, AbsoluteAddress address) {
        movl(src, Operand(address));
    }

    void setStackArg(Register reg, uint32_t arg) {
        movl(reg, Operand(esp, arg * sizeof(intptr_t)));
    }

    // Type testing instructions can take a tag in a register or a
    // ValueOperand.
    template <typename T>
    void branchTestUndefined(Condition cond, const T& t, Label* label) {
        cond = testUndefined(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestInt32(Condition cond, const T& t, Label* label) {
        cond = testInt32(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestBoolean(Condition cond, const T& t, Label* label) {
        cond = testBoolean(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestDouble(Condition cond, const T& t, Label* label) {
        cond = testDouble(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestNull(Condition cond, const T& t, Label* label) {
        cond = testNull(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestString(Condition cond, const T& t, Label* label) {
        cond = testString(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestSymbol(Condition cond, const T& t, Label* label) {
        cond = testSymbol(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestObject(Condition cond, const T& t, Label* label) {
        cond = testObject(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestNumber(Condition cond, const T& t, Label* label) {
        cond = testNumber(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestGCThing(Condition cond, const T& t, Label* label) {
        cond = testGCThing(cond, t);
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

    // Note: this function clobbers the source register.
    void boxDouble(FloatRegister src, const ValueOperand& dest) {
        if (Assembler::HasSSE41()) {
            vmovd(src, dest.payloadReg());
            vpextrd(1, src, dest.typeReg());
        } else {
            vmovd(src, dest.payloadReg());
            vpsrldq(Imm32(4), src, src);
            vmovd(src, dest.typeReg());
        }
    }
    void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest) {
        if (src != dest.payloadReg())
            movl(src, dest.payloadReg());
        movl(ImmType(type), dest.typeReg());
    }

    void unboxNonDouble(const ValueOperand& src, Register dest) {
        if (src.payloadReg() != dest)
            movl(src.payloadReg(), dest);
    }
    void unboxNonDouble(const Address& src, Register dest) {
        movl(payloadOf(src), dest);
    }
    void unboxNonDouble(const BaseIndex& src, Register dest) {
        movl(payloadOf(src), dest);
    }
    void unboxInt32(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxInt32(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxBoolean(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxBoolean(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxString(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxString(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxSymbol(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxSymbol(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxObject(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxObject(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxObject(const BaseIndex& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxDouble(const Address& src, FloatRegister dest) {
        loadDouble(Operand(src), dest);
    }
    void unboxDouble(const ValueOperand& src, FloatRegister dest) {
        MOZ_ASSERT(dest != ScratchDoubleReg);
        if (Assembler::HasSSE41()) {
            vmovd(src.payloadReg(), dest);
            vpinsrd(1, src.typeReg(), dest, dest);
        } else {
            vmovd(src.payloadReg(), dest);
            vmovd(src.typeReg(), ScratchDoubleReg);
            vunpcklps(ScratchDoubleReg, dest, dest);
        }
    }
    void unboxDouble(const Operand& payload, const Operand& type,
                     Register scratch, FloatRegister dest) {
        MOZ_ASSERT(dest != ScratchDoubleReg);
        if (Assembler::HasSSE41()) {
            movl(payload, scratch);
            vmovd(scratch, dest);
            movl(type, scratch);
            vpinsrd(1, scratch, dest, dest);
        } else {
            movl(payload, scratch);
            vmovd(scratch, dest);
            movl(type, scratch);
            vmovd(scratch, ScratchDoubleReg);
            vunpcklps(ScratchDoubleReg, dest, dest);
        }
    }
    void unboxValue(const ValueOperand& src, AnyRegister dest) {
        if (dest.isFloat()) {
            Label notInt32, end;
            branchTestInt32(Assembler::NotEqual, src, &notInt32);
            convertInt32ToDouble(src.payloadReg(), dest.fpu());
            jump(&end);
            bind(&notInt32);
            unboxDouble(src, dest.fpu());
            bind(&end);
        } else {
            if (src.payloadReg() != dest.gpr())
                movl(src.payloadReg(), dest.gpr());
        }
    }
    void unboxPrivate(const ValueOperand& src, Register dest) {
        if (src.payloadReg() != dest)
            movl(src.payloadReg(), dest);
    }

    void notBoolean(const ValueOperand& val) {
        xorl(Imm32(1), val.payloadReg());
    }

    // Extended unboxing API. If the payload is already in a register, returns
    // that register. Otherwise, provides a move to the given scratch register,
    // and returns that.
    Register extractObject(const Address& address, Register scratch) {
        movl(payloadOf(address), scratch);
        return scratch;
    }
    Register extractObject(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractInt32(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractBoolean(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractTag(const Address& address, Register scratch) {
        movl(tagOf(address), scratch);
        return scratch;
    }
    Register extractTag(const ValueOperand& value, Register scratch) {
        return value.typeReg();
    }

    void boolValueToDouble(const ValueOperand& operand, FloatRegister dest) {
        convertInt32ToDouble(operand.payloadReg(), dest);
    }
    void boolValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
        convertInt32ToFloat32(operand.payloadReg(), dest);
    }
    void int32ValueToDouble(const ValueOperand& operand, FloatRegister dest) {
        convertInt32ToDouble(operand.payloadReg(), dest);
    }
    void int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
        convertInt32ToFloat32(operand.payloadReg(), dest);
    }

    void loadConstantDouble(double d, FloatRegister dest);
    void addConstantDouble(double d, FloatRegister dest);
    void loadConstantFloat32(float f, FloatRegister dest);
    void addConstantFloat32(float f, FloatRegister dest);
    void loadConstantInt32x4(const SimdConstant& v, FloatRegister dest);
    void loadConstantFloat32x4(const SimdConstant& v, FloatRegister dest);

    void branchTruncateDouble(FloatRegister src, Register dest, Label* fail) {
        vcvttsd2si(src, dest);

        // vcvttsd2si returns 0x80000000 on failure. Test for it by
        // subtracting 1 and testing overflow (this permits the use of a
        // smaller immediate field).
        cmp32(dest, Imm32(1));
        j(Assembler::Overflow, fail);
    }
    void branchTruncateFloat32(FloatRegister src, Register dest, Label* fail) {
        vcvttss2si(src, dest);

        // vcvttss2si returns 0x80000000 on failure. Test for it by
        // subtracting 1 and testing overflow (this permits the use of a
        // smaller immediate field).
        cmp32(dest, Imm32(1));
        j(Assembler::Overflow, fail);
    }

    Condition testInt32Truthy(bool truthy, const ValueOperand& operand) {
        test32(operand.payloadReg(), operand.payloadReg());
        return truthy ? NonZero : Zero;
    }
    void branchTestInt32Truthy(bool truthy, const ValueOperand& operand, Label* label) {
        Condition cond = testInt32Truthy(truthy, operand);
        j(cond, label);
    }
    void branchTestBooleanTruthy(bool truthy, const ValueOperand& operand, Label* label) {
        test32(operand.payloadReg(), operand.payloadReg());
        j(truthy ? NonZero : Zero, label);
    }
    Condition testStringTruthy(bool truthy, const ValueOperand& value) {
        Register string = value.payloadReg();
        cmp32(Operand(string, JSString::offsetOfLength()), Imm32(0));
        return truthy ? Assembler::NotEqual : Assembler::Equal;
    }
    void branchTestStringTruthy(bool truthy, const ValueOperand& value, Label* label) {
        Condition cond = testStringTruthy(truthy, value);
        j(cond, label);
    }

    void loadInt32OrDouble(const Operand& operand, FloatRegister dest) {
        Label notInt32, end;
        branchTestInt32(Assembler::NotEqual, operand, &notInt32);
        convertInt32ToDouble(ToPayload(operand), dest);
        jump(&end);
        bind(&notInt32);
        loadDouble(operand, dest);
        bind(&end);
    }

    template <typename T>
    void loadUnboxedValue(const T& src, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(Operand(src), dest.fpu());
        else
            movl(Operand(src), dest.gpr());
    }

    template <typename T>
    void storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const T& dest,
                           MIRType slotType);

    template <typename T>
    void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes) {
        switch (nbytes) {
          case 4:
            storePtr(value.payloadReg(), address);
            return;
          case 1:
            store8(value.payloadReg(), address);
            return;
          default: MOZ_CRASH("Bad payload width");
        }
    }

    void rshiftPtr(Imm32 imm, Register dest) {
        shrl(imm, dest);
    }
    void rshiftPtrArithmetic(Imm32 imm, Register dest) {
        sarl(imm, dest);
    }
    void lshiftPtr(Imm32 imm, Register dest) {
        shll(imm, dest);
    }
    void xorPtr(Imm32 imm, Register dest) {
        xorl(imm, dest);
    }
    void xorPtr(Register src, Register dest) {
        xorl(src, dest);
    }
    void orPtr(Imm32 imm, Register dest) {
        orl(imm, dest);
    }
    void orPtr(Register src, Register dest) {
        orl(src, dest);
    }
    void andPtr(Imm32 imm, Register dest) {
        andl(imm, dest);
    }
    void andPtr(Register src, Register dest) {
        andl(src, dest);
    }

    void loadInstructionPointerAfterCall(Register dest) {
        movl(Operand(StackPointer, 0x0), dest);
    }

    // Note: this function clobbers the source register.
    void convertUInt32ToDouble(Register src, FloatRegister dest) {
        // src is [0, 2^32-1]
        subl(Imm32(0x80000000), src);

        // Now src is [-2^31, 2^31-1] - int range, but not the same value.
        convertInt32ToDouble(src, dest);

        // dest is now a double with the int range.
        // correct the double value by adding 0x80000000.
        addConstantDouble(2147483648.0, dest);
    }

    // Note: this function clobbers the source register.
    void convertUInt32ToFloat32(Register src, FloatRegister dest) {
        convertUInt32ToDouble(src, dest);
        convertDoubleToFloat32(dest, dest);
    }

    void inc64(AbsoluteAddress dest) {
        addl(Imm32(1), Operand(dest));
        Label noOverflow;
        j(NonZero, &noOverflow);
        addl(Imm32(1), Operand(dest.offset(4)));
        bind(&noOverflow);
    }

    void incrementInt32Value(const Address& addr) {
        addl(Imm32(1), payloadOf(addr));
    }

    // If source is a double, load it into dest. If source is int32,
    // convert it to double. Else, branch to failure.
    void ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure) {
        Label isDouble, done;
        branchTestDouble(Assembler::Equal, source.typeReg(), &isDouble);
        branchTestInt32(Assembler::NotEqual, source.typeReg(), failure);

        convertInt32ToDouble(source.payloadReg(), dest);
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
    void callWithABI(AsmJSImmPtr fun, MoveOp::Type result = MoveOp::GENERAL);
    void callWithABI(const Address& fun, MoveOp::Type result = MoveOp::GENERAL);
    void callWithABI(Register fun, MoveOp::Type result = MoveOp::GENERAL);

    // Used from within an Exit frame to handle a pending exception.
    void handleFailureWithHandlerTail(void* handler);

    void makeFrameDescriptor(Register frameSizeReg, FrameType type) {
        shll(Imm32(FRAMESIZE_SHIFT), frameSizeReg);
        orl(Imm32(type), frameSizeReg);
    }

    void callWithExitFrame(JitCode* target, Register dynStack) {
        addPtr(ImmWord(framePushed()), dynStack);
        makeFrameDescriptor(dynStack, JitFrame_IonJS);
        Push(dynStack);
        call(target);
    }

    void branchPtrInNurseryRange(Condition cond, Register ptr, Register temp, Label* label);
    void branchValueIsNurseryObject(Condition cond, ValueOperand value, Register temp, Label* label);

    // Instrumentation for entering and leaving the profiler.
    void profilerEnterFrame(Register framePtr, Register scratch);
    void profilerExitFrame();
};

typedef MacroAssemblerX86 MacroAssemblerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x86_MacroAssembler_x86_h */
