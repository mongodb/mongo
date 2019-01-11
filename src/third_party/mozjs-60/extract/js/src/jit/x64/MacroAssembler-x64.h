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

// ScratchTagScope and ScratchTagScopeRelease are used to manage the tag
// register for splitTagForTest(), which has different register management on
// different platforms.  On 64-bit platforms it requires a scratch register that
// does not interfere with other operations; on 32-bit platforms it uses a
// register that is already part of the Value.
//
// The ScratchTagScope RAII type acquires the appropriate register; a reference
// to a variable of this type is then passed to splitTagForTest().
//
// On 64-bit platforms ScratchTagScopeRelease makes the owned scratch register
// available in a dynamic scope during compilation.  However it is important to
// remember that that does not preserve the register value in any way, so this
// RAII type should only be used along paths that eventually branch past further
// uses of the extracted tag value.
//
// On 32-bit platforms ScratchTagScopeRelease has no effect, since it does not
// manage a register, it only aliases a register in the ValueOperand.

class ScratchTagScope : public ScratchRegisterScope
{
  public:
    ScratchTagScope(MacroAssembler& masm, const ValueOperand&)
      : ScratchRegisterScope(masm)
    {}
};

class ScratchTagScopeRelease
{
    ScratchTagScope* ts_;
  public:
    explicit ScratchTagScopeRelease(ScratchTagScope* ts) : ts_(ts) {
        ts_->release();
    }
    ~ScratchTagScopeRelease() {
        ts_->reacquire();
    }
};

class MacroAssemblerX64 : public MacroAssemblerX86Shared
{
  private:
    // Perform a downcast. Should be removed by Bug 996602.
    MacroAssembler& asMasm();
    const MacroAssembler& asMasm() const;

    void bindOffsets(const MacroAssemblerX86Shared::UsesVector&);

  public:
    using MacroAssemblerX86Shared::load32;
    using MacroAssemblerX86Shared::store32;
    using MacroAssemblerX86Shared::store16;

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
        if (val.isGCThing()) {
            gc::Cell* cell = val.toGCThing();
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
        if (val.isGCThing()) {
            movWithPatch(ImmWord(val.asRawBits()), scratch);
            writeDataRelocation(val);
        } else {
            mov(ImmWord(val.asRawBits()), scratch);
        }
        movq(scratch, Operand(dest));
    }
    void storeValue(ValueOperand val, BaseIndex dest) {
        storeValue(val, Operand(dest));
    }
    void storeValue(const Address& src, const Address& dest, Register temp) {
        loadPtr(src, temp);
        storePtr(temp, dest);
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
        if (val.isGCThing()) {
            ScratchRegisterScope scratch(asMasm());
            movWithPatch(ImmWord(val.asRawBits()), scratch);
            writeDataRelocation(val);
            push(scratch);
        } else {
            push(ImmWord(val.asRawBits()));
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

    void boxValue(JSValueType type, Register src, Register dest);

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
        cmp32(ToUpper32(src), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_UNDEFINED))));
        return cond;
    }
    Condition testInt32(Condition cond, const Address& src) {
        cmp32(ToUpper32(src), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_INT32))));
        return cond;
    }
    Condition testBoolean(Condition cond, const Address& src) {
        cmp32(ToUpper32(src), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_BOOLEAN))));
        return cond;
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
        cmp32(ToUpper32(src), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_NULL))));
        return cond;
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

    /////////////////////////////////////////////////////////////////
    // Common interface.
    /////////////////////////////////////////////////////////////////

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
    void load64(const Address& address, Register dest) {
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
    void store64(Register src, const Address& address) {
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
    void store16(Register src, AbsoluteAddress address) {
        if (X86Encoding::IsAddressImmediate(address.addr)) {
            movw(src, Operand(address));
        } else {
            ScratchRegisterScope scratch(asMasm());
            mov(ImmPtr(address.addr), scratch);
            store16(src, Address(scratch, 0x0));
        }
    }
    void store64(Register64 src, Address address) {
        storePtr(src.reg, address);
    }
    void store64(Imm64 imm, Address address) {
        storePtr(ImmWord(imm.value), address);
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

    // Extracts the tag of a value and places it in tag.
    void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag) {
        splitTag(value, tag);
    }
    void cmpTag(const ValueOperand& operand, ImmTag tag) {
        ScratchTagScope reg(asMasm(), operand);
        splitTagForTest(operand, reg);
        cmp32(reg, tag);
    }

    Condition testMagic(Condition cond, const ValueOperand& src) {
        ScratchTagScope scratch(asMasm(), src);
        splitTagForTest(src, scratch);
        return testMagic(cond, scratch);
    }
    Condition testError(Condition cond, const ValueOperand& src) {
        return testMagic(cond, src);
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

    void boxDouble(FloatRegister src, const ValueOperand& dest, FloatRegister) {
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

    void unboxNonDouble(const ValueOperand& src, Register dest, JSValueType type) {
        MOZ_ASSERT(type != JSVAL_TYPE_DOUBLE);
        if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
            movl(src.valueReg(), dest);
            return;
        }
        if (src.valueReg() == dest) {
            ScratchRegisterScope scratch(asMasm());
            mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), scratch);
            xorq(scratch, dest);
        } else {
            mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), dest);
            xorq(src.valueReg(), dest);
        }
    }
    void unboxNonDouble(const Operand& src, Register dest, JSValueType type) {
        MOZ_ASSERT(type != JSVAL_TYPE_DOUBLE);
        if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
            movl(src, dest);
            return;
        }
        // Explicitly permits |dest| to be used in |src|.
        ScratchRegisterScope scratch(asMasm());
        MOZ_ASSERT(dest != scratch);
        if (src.containsReg(dest)) {
            mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), scratch);
            // If src is already a register, then src and dest are the same
            // thing and we don't need to move anything into dest.
            if (src.kind() != Operand::REG)
                movq(src, dest);
            xorq(scratch, dest);
        } else {
            mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), dest);
            xorq(src, dest);
        }
    }
    void unboxNonDouble(const Address& src, Register dest, JSValueType type) {
        unboxNonDouble(Operand(src), dest, type);
    }
    void unboxNonDouble(const BaseIndex& src, Register dest, JSValueType type) {
        unboxNonDouble(Operand(src), dest, type);
    }

    void unboxString(const ValueOperand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
    }
    void unboxString(const Operand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
    }
    void unboxString(const Address& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
    }

    void unboxSymbol(const ValueOperand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
    }
    void unboxSymbol(const Operand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
    }

    void unboxObject(const ValueOperand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    }
    void unboxObject(const Operand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    }
    void unboxObject(const Address& src, Register dest) {
        unboxNonDouble(Operand(src), dest, JSVAL_TYPE_OBJECT);
    }
    void unboxObject(const BaseIndex& src, Register dest) {
        unboxNonDouble(Operand(src), dest, JSVAL_TYPE_OBJECT);
    }

    template <typename T>
    void unboxObjectOrNull(const T& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
        ScratchRegisterScope scratch(asMasm());
        mov(ImmWord(~JSVAL_OBJECT_OR_NULL_BIT), scratch);
        andq(scratch, dest);
    }

    // This should only be used for the pre-barrier trampoline, to unbox a
    // string/symbol/object Value. It's fine there because we don't depend on
    // the actual Value type. In almost all other cases, this would be
    // Spectre-unsafe - use unboxNonDouble and friends instead.
    void unboxGCThingForPreBarrierTrampoline(const Address& src, Register dest) {
        movq(ImmWord(JSVAL_PAYLOAD_MASK_GCTHING), dest);
        andq(Operand(src), dest);
    }

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
    Register extractString(const ValueOperand& value, Register scratch) {
        MOZ_ASSERT(scratch != ScratchReg);
        unboxString(value, scratch);
        return scratch;
    }
    Register extractSymbol(const ValueOperand& value, Register scratch) {
        MOZ_ASSERT(scratch != ScratchReg);
        unboxSymbol(value, scratch);
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

    inline void unboxValue(const ValueOperand& src, AnyRegister dest, JSValueType type);

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

    void loadConstantSimd128Int(const SimdConstant& v, FloatRegister dest);
    void loadConstantSimd128Float(const SimdConstant& v, FloatRegister dest);

    void loadWasmGlobalPtr(uint32_t globalDataOffset, Register dest) {
        loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, globalArea) + globalDataOffset), dest);
    }
    void loadWasmPinnedRegsFromTls() {
        loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, memoryBase)), HeapReg);
    }

  public:
    Condition testInt32Truthy(bool truthy, const ValueOperand& operand) {
        test32(operand.valueReg(), operand.valueReg());
        return truthy ? NonZero : Zero;
    }
    Condition testStringTruthy(bool truthy, const ValueOperand& value) {
        ScratchRegisterScope scratch(asMasm());
        unboxString(value, scratch);
        cmp32(Operand(scratch, JSString::offsetOfLength()), Imm32(0));
        return truthy ? Assembler::NotEqual : Assembler::Equal;
    }

    template <typename T>
    inline void loadInt32OrDouble(const T& src, FloatRegister dest);

    template <typename T>
    void loadUnboxedValue(const T& src, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(src, dest.fpu());
        else if (type == MIRType::ObjectOrNull)
            unboxObjectOrNull(src, dest.gpr());
        else
            unboxNonDouble(Operand(src), dest.gpr(), ValueTypeFromMIRType(type));
    }

    template <typename T>
    void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes, JSValueType type) {
        switch (nbytes) {
          case 8: {
            ScratchRegisterScope scratch(asMasm());
            unboxNonDouble(value, scratch, type);
            storePtr(scratch, address);
            if (type == JSVAL_TYPE_OBJECT) {
                // Ideally we would call unboxObjectOrNull, but we need an extra
                // scratch register for that. So unbox as object, then clear the
                // object-or-null bit.
                mov(ImmWord(~JSVAL_OBJECT_OR_NULL_BIT), scratch);
                andq(scratch, Operand(address));
            }
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
        // Zero the output register to break dependencies, see convertInt32ToDouble.
        zeroDouble(dest);

        vcvtsq2sd(src, dest, dest);
    }

    void convertUInt32ToFloat32(Register src, FloatRegister dest) {
        // Zero the output register to break dependencies, see convertInt32ToDouble.
        zeroDouble(dest);

        vcvtsq2ss(src, dest, dest);
    }

    inline void incrementInt32Value(const Address& addr);

    inline void ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure);

  public:
    void handleFailureWithHandlerTail(void* handler, Label* profilerExitTail);

    // Instrumentation for entering and leaving the profiler.
    void profilerEnterFrame(Register framePtr, Register scratch);
    void profilerExitFrame();
};

typedef MacroAssemblerX64 MacroAssemblerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x64_MacroAssembler_x64_h */
