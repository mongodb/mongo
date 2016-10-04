/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_MacroAssembler_x64_h
#define jit_x64_MacroAssembler_x64_h

#include "jit/JitFrames.h"
#include "jit/MoveResolver.h"
#include "jit/x86-shared/MacroAssembler-x86-shared.h"

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
  private:
    // Perform a downcast. Should be removed by Bug 996602.
    MacroAssembler& asMasm();
    const MacroAssembler& asMasm() const;

    void bindOffsets(const MacroAssemblerX86Shared::UsesVector&);

  public:
    using MacroAssemblerX86Shared::branch32;
    using MacroAssemblerX86Shared::branchTest32;
    using MacroAssemblerX86Shared::load32;
    using MacroAssemblerX86Shared::store32;

    MacroAssemblerX64()
    {
    }

    // The buffer is about to be linked, make sure any constant pools or excess
    // bookkeeping has been flushed to the instruction stream.
    void finish();

    /////////////////////////////////////////////////////////////////
    // X64 helpers.
    /////////////////////////////////////////////////////////////////
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
            ScratchRegisterScope scratch(asMasm());
            boxValue(type, reg, scratch);
            movq(scratch, Operand(dest));
        }
    }
    template <typename T>
    void storeValue(const Value& val, const T& dest) {
        ScratchRegisterScope scratch(asMasm());
        jsval_layout jv = JSVAL_TO_IMPL(val);
        if (val.isMarkable()) {
            movWithPatch(ImmWord(jv.asBits), scratch);
            writeDataRelocation(val);
        } else {
            mov(ImmWord(jv.asBits), scratch);
        }
        movq(scratch, Operand(dest));
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
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(dest.valueReg() != scratch);
        if (payload != dest.valueReg())
            movq(payload, dest.valueReg());
        mov(ImmShiftedTag(type), scratch);
        orq(scratch, dest.valueReg());
    }
    void pushValue(ValueOperand val) {
        push(val.valueReg());
    }
    void popValue(ValueOperand val) {
        pop(val.valueReg());
    }
    void pushValue(const Value& val) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        if (val.isMarkable()) {
            ScratchRegisterScope scratch(asMasm());
            movWithPatch(ImmWord(jv.asBits), scratch);
            writeDataRelocation(val);
            push(scratch);
        } else {
            push(ImmWord(jv.asBits));
        }
    }
    void pushValue(JSValueType type, Register reg) {
        ScratchRegisterScope scratch(asMasm());
        boxValue(type, reg, scratch);
        push(scratch);
    }
    void pushValue(const Address& addr) {
        push(Operand(addr));
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
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testUndefined(cond, scratch);
    }
    Condition testInt32(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testInt32(cond, scratch);
    }
    Condition testBoolean(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testBoolean(cond, scratch);
    }
    Condition testDouble(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testDouble(cond, scratch);
    }
    Condition testNumber(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testNumber(cond, scratch);
    }
    Condition testNull(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testNull(cond, scratch);
    }
    Condition testString(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testString(cond, scratch);
    }
    Condition testSymbol(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testSymbol(cond, scratch);
    }
    Condition testObject(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testObject(cond, scratch);
    }
    Condition testGCThing(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testGCThing(cond, scratch);
    }
    Condition testPrimitive(Condition cond, const ValueOperand& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testPrimitive(cond, scratch);
    }


    Condition testUndefined(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testUndefined(cond, scratch);
    }
    Condition testInt32(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testInt32(cond, scratch);
    }
    Condition testBoolean(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testBoolean(cond, scratch);
    }
    Condition testDouble(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testDouble(cond, scratch);
    }
    Condition testNumber(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testNumber(cond, scratch);
    }
    Condition testNull(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testNull(cond, scratch);
    }
    Condition testString(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testString(cond, scratch);
    }
    Condition testSymbol(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testSymbol(cond, scratch);
    }
    Condition testObject(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testObject(cond, scratch);
    }
    Condition testPrimitive(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testPrimitive(cond, scratch);
    }
    Condition testGCThing(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testGCThing(cond, scratch);
    }
    Condition testMagic(Condition cond, const Address& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testMagic(cond, scratch);
    }


    Condition testUndefined(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testUndefined(cond, scratch);
    }
    Condition testNull(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testNull(cond, scratch);
    }
    Condition testBoolean(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testBoolean(cond, scratch);
    }
    Condition testString(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testString(cond, scratch);
    }
    Condition testSymbol(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testSymbol(cond, scratch);
    }
    Condition testInt32(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testInt32(cond, scratch);
    }
    Condition testObject(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testObject(cond, scratch);
    }
    Condition testDouble(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testDouble(cond, scratch);
    }
    Condition testMagic(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testMagic(cond, scratch);
    }
    Condition testGCThing(Condition cond, const BaseIndex& src) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testGCThing(cond, scratch);
    }

    Condition isMagic(Condition cond, const ValueOperand& src, JSWhyMagic why) {
        uint64_t magic = MagicValue(why).asRawBits();
        cmpPtr(src.valueReg(), ImmWord(magic));
        return cond;
    }

    void cmpPtr(Register lhs, const ImmWord rhs) {
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(lhs != scratch);
        if (intptr_t(rhs.value) <= INT32_MAX && intptr_t(rhs.value) >= INT32_MIN) {
            cmpPtr(lhs, Imm32(int32_t(rhs.value)));
        } else {
            movePtr(rhs, scratch);
            cmpPtr(lhs, scratch);
        }
    }
    void cmpPtr(Register lhs, const ImmPtr rhs) {
        cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
    }
    void cmpPtr(Register lhs, const ImmGCPtr rhs) {
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(lhs != scratch);
        movePtr(rhs, scratch);
        cmpPtr(lhs, scratch);
    }
    void cmpPtr(Register lhs, const Imm32 rhs) {
        cmpq(rhs, lhs);
    }
    void cmpPtr(const Operand& lhs, const ImmGCPtr rhs) {
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(!lhs.containsReg(scratch));
        movePtr(rhs, scratch);
        cmpPtr(lhs, scratch);
    }
    void cmpPtr(const Operand& lhs, const ImmWord rhs) {
        if ((intptr_t)rhs.value <= INT32_MAX && (intptr_t)rhs.value >= INT32_MIN) {
            cmpPtr(lhs, Imm32((int32_t)rhs.value));
        } else {
            ScratchRegisterScope scratch(asMasm());
            movePtr(rhs, scratch);
            cmpPtr(lhs, scratch);
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
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(dest != scratch);
        if ((intptr_t)imm.value <= INT32_MAX && (intptr_t)imm.value >= INT32_MIN) {
            addq(Imm32((int32_t)imm.value), dest);
        } else {
            mov(imm, scratch);
            addq(scratch, dest);
        }
    }
    void addPtr(ImmPtr imm, Register dest) {
        addPtr(ImmWord(uintptr_t(imm.value)), dest);
    }
    void addPtr(const Address& src, Register dest) {
        addq(Operand(src), dest);
    }
    void add64(Imm32 imm, Register64 dest) {
        addq(imm, dest.reg);
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
    void mul64(Imm64 imm, const Register64& dest) {
        movq(ImmWord(uintptr_t(imm.value)), ScratchReg);
        imulq(ScratchReg, dest.reg);
    }

    void branch32(Condition cond, AbsoluteAddress lhs, Imm32 rhs, Label* label) {
        if (X86Encoding::IsAddressImmediate(lhs.addr)) {
            branch32(cond, Operand(lhs), rhs, label);
        } else {
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(lhs.addr), scratch);
            branch32(cond, Address(scratch, 0), rhs, label);
        }
    }
    void branch32(Condition cond, wasm::SymbolicAddress lhs, Imm32 rhs, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        mov(lhs, scratch);
        branch32(cond, Address(scratch, 0), rhs, label);
    }
    void branch32(Condition cond, AbsoluteAddress lhs, Register rhs, Label* label) {
        if (X86Encoding::IsAddressImmediate(lhs.addr)) {
            branch32(cond, Operand(lhs), rhs, label);
        } else {
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(lhs.addr), scratch);
            branch32(cond, Address(scratch, 0), rhs, label);
        }
    }
    void branchTest32(Condition cond, AbsoluteAddress address, Imm32 imm, Label* label) {
        if (X86Encoding::IsAddressImmediate(address.addr)) {
            test32(Operand(address), imm);
        } else {
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(address.addr), scratch);
            test32(Operand(scratch, 0), imm);
        }
        j(cond, label);
    }

    // Specialization for AbsoluteAddress.
    void branchPtr(Condition cond, AbsoluteAddress addr, Register ptr, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(ptr != scratch);
        if (X86Encoding::IsAddressImmediate(addr.addr)) {
            branchPtr(cond, Operand(addr), ptr, label);
        } else {
            mov(ImmPtr(addr.addr), scratch);
            branchPtr(cond, Operand(scratch, 0x0), ptr, label);
        }
    }
    void branchPtr(Condition cond, AbsoluteAddress addr, ImmWord ptr, Label* label) {
        if (X86Encoding::IsAddressImmediate(addr.addr)) {
            branchPtr(cond, Operand(addr), ptr, label);
        } else {
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(addr.addr), scratch);
            branchPtr(cond, Operand(scratch, 0x0), ptr, label);
        }
    }
    void branchPtr(Condition cond, wasm::SymbolicAddress addr, Register ptr, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(ptr != scratch);
        mov(addr, scratch);
        branchPtr(cond, Operand(scratch, 0x0), ptr, label);
    }

    void branchPrivatePtr(Condition cond, Address lhs, ImmPtr ptr, Label* label) {
        branchPtr(cond, lhs, ImmWord(uintptr_t(ptr.value) >> 1), label);
    }

    void branchPrivatePtr(Condition cond, Address lhs, Register ptr, Label* label);
    template <typename T, typename S>
    void branchPtr(Condition cond, const T& lhs, const S& ptr, Label* label) {
        cmpPtr(Operand(lhs), ptr);
        j(cond, label);
    }

    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Label* documentation = nullptr) {
        JmpSrc src = jmpSrc(label);
        return CodeOffsetJump(size(), addPatchableJump(src, Relocation::HARDCODED));
    }

    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Condition cond,
                                 Label* documentation = nullptr)
    {
        JmpSrc src = jSrc(cond, label);
        return CodeOffsetJump(size(), addPatchableJump(src, Relocation::HARDCODED));
    }

    CodeOffsetJump backedgeJump(RepatchLabel* label, Label* documentation = nullptr) {
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

    void branchTest64(Condition cond, Register64 lhs, Register64 rhs, Register temp, Label* label) {
        branchTestPtr(cond, lhs.reg, rhs.reg, label);
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
    void movePtr(wasm::SymbolicAddress imm, Register dest) {
        mov(imm, dest);
    }
    void movePtr(ImmGCPtr imm, Register dest) {
        movq(imm, dest);
    }
    void move64(Register64 src, Register64 dest) {
        movq(src.reg, dest.reg);
    }
    void loadPtr(AbsoluteAddress address, Register dest) {
        if (X86Encoding::IsAddressImmediate(address.addr)) {
            movq(Operand(address), dest);
        } else {
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(address.addr), scratch);
            loadPtr(Address(scratch, 0x0), dest);
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
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(address.addr), scratch);
            load32(Address(scratch, 0x0), dest);
        }
    }
    void load64(const Address& address, Register64 dest) {
        movq(Operand(address), dest.reg);
    }
    template <typename T>
    void storePtr(ImmWord imm, T address) {
        if ((intptr_t)imm.value <= INT32_MAX && (intptr_t)imm.value >= INT32_MIN) {
            movq(Imm32((int32_t)imm.value), Operand(address));
        } else {
            ScratchRegisterScope scratch(asMasm());
            mov(imm, scratch);
            movq(scratch, Operand(address));
        }
    }
    template <typename T>
    void storePtr(ImmPtr imm, T address) {
        storePtr(ImmWord(uintptr_t(imm.value)), address);
    }
    template <typename T>
    void storePtr(ImmGCPtr imm, T address) {
        ScratchRegisterScope scratch(asMasm());
        movq(imm, scratch);
        movq(scratch, Operand(address));
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
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(address.addr), scratch);
            storePtr(src, Address(scratch, 0x0));
        }
    }
    void store32(Register src, AbsoluteAddress address) {
        if (X86Encoding::IsAddressImmediate(address.addr)) {
            movl(src, Operand(address));
        } else {
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(address.addr), scratch);
            store32(src, Address(scratch, 0x0));
        }
    }
    void store64(Register64 src, Address address) {
        movq(src.reg, Operand(address));
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
        ScratchRegisterScope scratch(asMasm());
        splitTag(operand, scratch);
        branchTestDouble(cond, scratch, label);
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
    void branchTestBoolean(Condition cond, const Address& address, Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        branchTestBoolean(cond, Operand(address), label);
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
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        branchTestInt32(cond, scratch, label);
    }
    void branchTestBoolean(Condition cond, const ValueOperand& src, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        branchTestBoolean(cond, scratch, label);
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
        ScratchRegisterScope scratch(asMasm());
        splitTag(address, scratch);
        branchTestInt32(cond, scratch, label);
    }
    void branchTestBoolean(Condition cond, const BaseIndex& address, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        splitTag(address, scratch);
        branchTestBoolean(cond, scratch, label);
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
        ScratchRegisterScope scratch(asMasm());
        splitTag(src, scratch);
        return testMagic(cond, scratch);
    }
    Condition testError(Condition cond, const ValueOperand& src) {
        return testMagic(cond, src);
    }
    void branchTestValue(Condition cond, const ValueOperand& value, const Value& v, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(value.valueReg() != scratch);
        moveValue(v, scratch);
        cmpPtr(value.valueReg(), scratch);
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
            ScratchRegisterScope scratch(asMasm());
            mov(ImmWord(JSVAL_PAYLOAD_MASK), scratch);
            andq(scratch, dest);
        } else {
            mov(ImmWord(JSVAL_PAYLOAD_MASK), dest);
            andq(src.valueReg(), dest);
        }
    }
    void unboxNonDouble(const Operand& src, Register dest) {
        // Explicitly permits |dest| to be used in |src|.
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(dest != scratch);
        if (src.containsReg(dest)) {
            mov(ImmWord(JSVAL_PAYLOAD_MASK), scratch);
            // If src is already a register, then src and dest are the same
            // thing and we don't need to move anything into dest.
            if (src.kind() != Operand::REG)
                movq(src, dest);
            andq(scratch, dest);
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
        ScratchRegisterScope scratch(asMasm());
        unboxString(value, scratch);
        cmp32(Operand(scratch, JSString::offsetOfLength()), Imm32(0));
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
          case 8: {
            ScratchRegisterScope scratch(asMasm());
            unboxNonDouble(value, scratch);
            storePtr(scratch, address);
            return;
          }
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

    void convertUInt64ToDouble(Register64 src, Register temp, FloatRegister dest) {
        vcvtsi2sdq(src.reg, dest);
    }

    void mulDoublePtr(ImmPtr imm, Register temp, FloatRegister dest) {
        movq(imm, ScratchReg);
        vmulsd(Operand(ScratchReg, 0), dest, dest);
    }

    void inc64(AbsoluteAddress dest) {
        if (X86Encoding::IsAddressImmediate(dest.addr)) {
            addPtr(Imm32(1), Operand(dest));
        } else {
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(dest.addr), scratch);
            addPtr(Imm32(1), Address(scratch, 0));
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

        ScratchRegisterScope scratch(asMasm());
        unboxInt32(source, scratch);
        convertInt32ToDouble(scratch, dest);
        jump(&done);

        bind(&isDouble);
        unboxDouble(source, dest);

        bind(&done);
    }

  public:
    void handleFailureWithHandlerTail(void* handler);

    // See CodeGeneratorX64 calls to noteAsmJSGlobalAccess.
    void patchAsmJSGlobalAccess(CodeOffset patchAt, uint8_t* code, uint8_t* globalData,
                                unsigned globalDataOffset)
    {
        uint8_t* nextInsn = code + patchAt.offset();
        MOZ_ASSERT(nextInsn <= globalData);
        uint8_t* target = globalData + globalDataOffset;
        ((int32_t*)nextInsn)[-1] = target - nextInsn;
    }
    void memIntToValue(Address Source, Address Dest) {
        ScratchRegisterScope scratch(asMasm());
        load32(Source, scratch);
        storeValue(JSVAL_TYPE_INT32, scratch, Dest);
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
