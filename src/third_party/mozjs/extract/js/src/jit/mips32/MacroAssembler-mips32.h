/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_MacroAssembler_mips32_h
#define jit_mips32_MacroAssembler_mips32_h

#include "mozilla/EndianUtils.h"

#include "jit/JitFrames.h"
#include "jit/mips-shared/MacroAssembler-mips-shared.h"
#include "jit/MoveResolver.h"
#include "vm/BytecodeUtil.h"

namespace js {
namespace jit {

struct ImmTag : public Imm32
{
    ImmTag(JSValueTag mask)
      : Imm32(int32_t(mask))
    { }
};

struct ImmType : public ImmTag
{
    ImmType(JSValueType type)
      : ImmTag(JSVAL_TYPE_TO_TAG(type))
    { }
};

static constexpr ValueOperand JSReturnOperand{JSReturnReg_Type, JSReturnReg_Data};
static const ValueOperand softfpReturnOperand = ValueOperand(v1, v0);

static const int defaultShift = 3;
static_assert(1 << defaultShift == sizeof(JS::Value), "The defaultShift is wrong");

static const uint32_t LOW_32_MASK = (1LL << 32) - 1;
#if MOZ_LITTLE_ENDIAN
static const int32_t LOW_32_OFFSET = 0;
static const int32_t HIGH_32_OFFSET = 4;
#else
static const int32_t LOW_32_OFFSET = 4;
static const int32_t HIGH_32_OFFSET = 0;
#endif

// See documentation for ScratchTagScope and ScratchTagScopeRelease in
// MacroAssembler-x64.h.

class ScratchTagScope
{
    const ValueOperand& v_;
  public:
    ScratchTagScope(MacroAssembler&, const ValueOperand& v) : v_(v) {}
    operator Register() { return v_.typeReg(); }
    void release() {}
    void reacquire() {}
};

class ScratchTagScopeRelease
{
  public:
    explicit ScratchTagScopeRelease(ScratchTagScope*) {}
};

class MacroAssemblerMIPS : public MacroAssemblerMIPSShared
{
  public:
    using MacroAssemblerMIPSShared::ma_b;
    using MacroAssemblerMIPSShared::ma_li;
    using MacroAssemblerMIPSShared::ma_ss;
    using MacroAssemblerMIPSShared::ma_sd;
    using MacroAssemblerMIPSShared::ma_ls;
    using MacroAssemblerMIPSShared::ma_ld;
    using MacroAssemblerMIPSShared::ma_load;
    using MacroAssemblerMIPSShared::ma_store;
    using MacroAssemblerMIPSShared::ma_cmp_set;
    using MacroAssemblerMIPSShared::ma_subTestOverflow;
    using MacroAssemblerMIPSShared::ma_liPatchable;

    void ma_li(Register dest, CodeLabel* label);

    void ma_li(Register dest, ImmWord imm);
    void ma_liPatchable(Register dest, ImmPtr imm);
    void ma_liPatchable(Register dest, ImmWord imm);

    // load
    void ma_load(Register dest, Address address, LoadStoreSize size = SizeWord,
                 LoadStoreExtension extension = SignExtend);

    // store
    void ma_store(Register data, Address address, LoadStoreSize size = SizeWord,
                  LoadStoreExtension extension = SignExtend);

    // arithmetic based ops
    // add
    template <typename L>
    void ma_addTestOverflow(Register rd, Register rs, Register rt, L overflow);
    template <typename L>
    void ma_addTestOverflow(Register rd, Register rs, Imm32 imm, L overflow);

    // subtract
    void ma_subTestOverflow(Register rd, Register rs, Register rt, Label* overflow);

    // memory
    // shortcut for when we know we're transferring 32 bits of data
    void ma_lw(Register data, Address address);

    void ma_sw(Register data, Address address);
    void ma_sw(Imm32 imm, Address address);
    void ma_sw(Register data, BaseIndex& address);

    void ma_pop(Register r);
    void ma_push(Register r);

    void branchWithCode(InstImm code, Label* label, JumpKind jumpKind);
    // branches when done from within mips-specific code
    void ma_b(Register lhs, ImmWord imm, Label* l, Condition c, JumpKind jumpKind = LongJump)
    {
        ma_b(lhs, Imm32(uint32_t(imm.value)), l, c, jumpKind);
    }
    void ma_b(Address addr, ImmWord imm, Label* l, Condition c, JumpKind jumpKind = LongJump)
    {
        ma_b(addr, Imm32(uint32_t(imm.value)), l, c, jumpKind);
    }

    void ma_b(Register lhs, Address addr, Label* l, Condition c, JumpKind jumpKind = LongJump);
    void ma_b(Address addr, Imm32 imm, Label* l, Condition c, JumpKind jumpKind = LongJump);
    void ma_b(Address addr, ImmGCPtr imm, Label* l, Condition c, JumpKind jumpKind = LongJump);
    void ma_b(Address addr, Register rhs, Label* l, Condition c, JumpKind jumpKind = LongJump) {
        MOZ_ASSERT(rhs != ScratchRegister);
        ma_lw(ScratchRegister, addr);
        ma_b(ScratchRegister, rhs, l, c, jumpKind);
    }

    void ma_bal(Label* l, DelaySlotFill delaySlotFill = FillDelaySlot);

    // fp instructions
    void ma_lid(FloatRegister dest, double value);

    void ma_mv(FloatRegister src, ValueOperand dest);
    void ma_mv(ValueOperand src, FloatRegister dest);

    void ma_ls(FloatRegister ft, Address address);
    void ma_ld(FloatRegister ft, Address address);
    void ma_sd(FloatRegister ft, Address address);
    void ma_ss(FloatRegister ft, Address address);

    void ma_ldc1WordAligned(FloatRegister ft, Register base, int32_t off);
    void ma_sdc1WordAligned(FloatRegister ft, Register base, int32_t off);

    void ma_pop(FloatRegister f);
    void ma_push(FloatRegister f);

    void ma_cmp_set(Register dst, Register lhs, ImmPtr imm, Condition c) {
        ma_cmp_set(dst, lhs, Imm32(uint32_t(imm.value)), c);
    }
    void ma_cmp_set(Register dst, Register lhs, Address addr, Condition c) {
        MOZ_ASSERT(lhs != ScratchRegister);
        ma_lw(ScratchRegister, addr);
        ma_cmp_set(dst, lhs, ScratchRegister, c);
    }
    void ma_cmp_set(Register dst, Address lhs, Register rhs, Condition c) {
        MOZ_ASSERT(rhs != ScratchRegister);
        ma_lw(ScratchRegister, lhs);
        ma_cmp_set(dst, ScratchRegister, rhs, c);
    }
    void ma_cmp_set(Register dst, Address lhs, ImmPtr imm, Condition c) {
        ma_lw(SecondScratchReg, lhs);
        ma_cmp_set(dst, SecondScratchReg, imm, c);
    }

    // These fuctions abstract the access to high part of the double precision
    // float register. It is intended to work on both 32 bit and 64 bit
    // floating point coprocessor.
    // :TODO: (Bug 985881) Modify this for N32 ABI to use mthc1 and mfhc1
    void moveToDoubleHi(Register src, FloatRegister dest) {
        as_mtc1(src, getOddPair(dest));
    }
    void moveFromDoubleHi(FloatRegister src, Register dest) {
        as_mfc1(dest, getOddPair(src));
    }
};

class MacroAssembler;

class MacroAssemblerMIPSCompat : public MacroAssemblerMIPS
{
  public:
    using MacroAssemblerMIPS::call;

    MacroAssemblerMIPSCompat()
    { }

    void convertBoolToInt32(Register source, Register dest);
    void convertInt32ToDouble(Register src, FloatRegister dest);
    void convertInt32ToDouble(const Address& src, FloatRegister dest);
    void convertInt32ToDouble(const BaseIndex& src, FloatRegister dest);
    void convertUInt32ToDouble(Register src, FloatRegister dest);
    void convertUInt32ToFloat32(Register src, FloatRegister dest);
    void convertDoubleToFloat32(FloatRegister src, FloatRegister dest);
    void convertDoubleToInt32(FloatRegister src, Register dest, Label* fail,
                              bool negativeZeroCheck = true);
    void convertFloat32ToInt32(FloatRegister src, Register dest, Label* fail,
                               bool negativeZeroCheck = true);

    void convertFloat32ToDouble(FloatRegister src, FloatRegister dest);
    void convertInt32ToFloat32(Register src, FloatRegister dest);
    void convertInt32ToFloat32(const Address& src, FloatRegister dest);

    void computeScaledAddress(const BaseIndex& address, Register dest);

    void computeEffectiveAddress(const Address& address, Register dest) {
        ma_addu(dest, address.base, Imm32(address.offset));
    }

    inline void computeEffectiveAddress(const BaseIndex& address, Register dest);

    void j(Label* dest) {
        ma_b(dest);
    }

    void mov(Register src, Register dest) {
        as_ori(dest, src, 0);
    }
    void mov(ImmWord imm, Register dest) {
        ma_li(dest, imm);
    }
    void mov(ImmPtr imm, Register dest) {
        mov(ImmWord(uintptr_t(imm.value)), dest);
    }
    void mov(CodeLabel* label, Register dest) {
        ma_li(dest, label);
    }
    void mov(Register src, Address dest) {
        MOZ_CRASH("NYI-IC");
    }
    void mov(Address src, Register dest) {
        MOZ_CRASH("NYI-IC");
    }

    void branch(JitCode* c) {
        BufferOffset bo = m_buffer.nextOffset();
        addPendingJump(bo, ImmPtr(c->raw()), Relocation::JITCODE);
        ma_liPatchable(ScratchRegister, ImmPtr(c->raw()));
        as_jr(ScratchRegister);
        as_nop();
    }
    void branch(const Register reg) {
        as_jr(reg);
        as_nop();
    }
    void nop() {
        as_nop();
    }
    void ret() {
        ma_pop(ra);
        as_jr(ra);
        as_nop();
    }
    inline void retn(Imm32 n);
    void push(Imm32 imm) {
        ma_li(ScratchRegister, imm);
        ma_push(ScratchRegister);
    }
    void push(ImmWord imm) {
        ma_li(ScratchRegister, imm);
        ma_push(ScratchRegister);
    }
    void push(ImmGCPtr imm) {
        ma_li(ScratchRegister, imm);
        ma_push(ScratchRegister);
    }
    void push(const Address& address) {
        loadPtr(address, ScratchRegister);
        ma_push(ScratchRegister);
    }
    void push(Register reg) {
        ma_push(reg);
    }
    void push(FloatRegister reg) {
        ma_push(reg);
    }
    void pop(Register reg) {
        ma_pop(reg);
    }
    void pop(FloatRegister reg) {
        ma_pop(reg);
    }

    // Emit a branch that can be toggled to a non-operation. On MIPS we use
    // "andi" instruction to toggle the branch.
    // See ToggleToJmp(), ToggleToCmp().
    CodeOffset toggledJump(Label* label);

    // Emit a "jalr" or "nop" instruction. ToggleCall can be used to patch
    // this instruction.
    CodeOffset toggledCall(JitCode* target, bool enabled);

    static size_t ToggledCallSize(uint8_t* code) {
        // Four instructions used in: MacroAssemblerMIPSCompat::toggledCall
        return 4 * sizeof(uint32_t);
    }

    CodeOffset pushWithPatch(ImmWord imm) {
        CodeOffset label = movWithPatch(imm, ScratchRegister);
        ma_push(ScratchRegister);
        return label;
    }

    CodeOffset movWithPatch(ImmWord imm, Register dest) {
        CodeOffset label = CodeOffset(currentOffset());
        ma_liPatchable(dest, imm);
        return label;
    }
    CodeOffset movWithPatch(ImmPtr imm, Register dest) {
        return movWithPatch(ImmWord(uintptr_t(imm.value)), dest);
    }

    void writeCodePointer(CodeLabel* label) {
        BufferOffset off = writeInst(-1);
        label->patchAt()->bind(off.getOffset());
        label->setLinkMode(CodeLabel::RawPointer);
    }

    void jump(Label* label) {
        ma_b(label);
    }
    void jump(Register reg) {
        as_jr(reg);
        as_nop();
    }
    void jump(const Address& address) {
        loadPtr(address, ScratchRegister);
        as_jr(ScratchRegister);
        as_nop();
    }

    void jump(JitCode* code) {
        branch(code);
    }

    void jump(wasm::OldTrapDesc target) {
        ma_b(target);
    }

    void jump(TrampolinePtr code)
    {
        auto target = ImmPtr(code.value);
        BufferOffset bo = m_buffer.nextOffset();
        addPendingJump(bo, target, Relocation::HARDCODED);
        ma_jump(target);
    }

    void negl(Register reg) {
        ma_negu(reg, reg);
    }

    void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag) {
        MOZ_ASSERT(value.typeReg() == tag);
    }

    // unboxing code
    void unboxNonDouble(const ValueOperand& operand, Register dest, JSValueType);
    void unboxNonDouble(const Address& src, Register dest, JSValueType);
    void unboxNonDouble(const BaseIndex& src, Register dest, JSValueType);
    void unboxInt32(const ValueOperand& operand, Register dest);
    void unboxInt32(const Address& src, Register dest);
    void unboxBoolean(const ValueOperand& operand, Register dest);
    void unboxBoolean(const Address& src, Register dest);
    void unboxDouble(const ValueOperand& operand, FloatRegister dest);
    void unboxDouble(const Address& src, FloatRegister dest);
    void unboxString(const ValueOperand& operand, Register dest);
    void unboxString(const Address& src, Register dest);
    void unboxObject(const ValueOperand& src, Register dest);
    void unboxObject(const Address& src, Register dest);
    void unboxObject(const BaseIndex& src, Register dest)
    {
        unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    }
    void unboxValue(const ValueOperand& src, AnyRegister dest, JSValueType);
    void unboxPrivate(const ValueOperand& src, Register dest);

    void unboxGCThingForPreBarrierTrampoline(const Address& src, Register dest)
    {
        unboxObject(src, dest);
    }

    void notBoolean(const ValueOperand& val) {
        as_xori(val.payloadReg(), val.payloadReg(), 1);
    }

    // boxing code
    void boxDouble(FloatRegister src, const ValueOperand& dest, FloatRegister);
    void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest);

    // Extended unboxing API. If the payload is already in a register, returns
    // that register. Otherwise, provides a move to the given scratch register,
    // and returns that.
    Register extractObject(const Address& address, Register scratch);
    Register extractObject(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractString(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractSymbol(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractInt32(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractBoolean(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractTag(const Address& address, Register scratch);
    Register extractTag(const BaseIndex& address, Register scratch);
    Register extractTag(const ValueOperand& value, Register scratch) {
        return value.typeReg();
    }

    void boolValueToDouble(const ValueOperand& operand, FloatRegister dest);
    void int32ValueToDouble(const ValueOperand& operand, FloatRegister dest);
    void loadInt32OrDouble(const Address& address, FloatRegister dest);
    void loadInt32OrDouble(Register base, Register index,
                           FloatRegister dest, int32_t shift = defaultShift);
    void loadConstantDouble(double dp, FloatRegister dest);

    void boolValueToFloat32(const ValueOperand& operand, FloatRegister dest);
    void int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest);
    void loadConstantFloat32(float f, FloatRegister dest);

    void testNullSet(Condition cond, const ValueOperand& value, Register dest);

    void testObjectSet(Condition cond, const ValueOperand& value, Register dest);

    void testUndefinedSet(Condition cond, const ValueOperand& value, Register dest);

    // higher level tag testing code
    Operand ToPayload(Operand base);
    Address ToPayload(Address base) {
        return ToPayload(Operand(base)).toAddress();
    }

    BaseIndex ToPayload(BaseIndex base) {
        return BaseIndex(base.base, base.index, base.scale, base.offset + NUNBOX32_PAYLOAD_OFFSET);
    }

  protected:
    Operand ToType(Operand base);
    Address ToType(Address base) {
        return ToType(Operand(base)).toAddress();
    }

    uint32_t getType(const Value& val);
    void moveData(const Value& val, Register data);

  public:
    void moveValue(const Value& val, Register type, Register data);

    CodeOffsetJump backedgeJump(RepatchLabel* label, Label* documentation = nullptr);
    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Label* documentation = nullptr);

    void loadUnboxedValue(Address address, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(address, dest.fpu());
        else
            ma_lw(dest.gpr(), ToPayload(address));
    }

    void loadUnboxedValue(BaseIndex address, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(address.base, address.index, dest.fpu(), address.scale);
        else
            load32(ToPayload(address), dest.gpr());
    }

    template <typename T>
    void storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const T& dest,
                           MIRType slotType);

    template <typename T>
    void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes, JSValueType) {
        switch (nbytes) {
          case 4:
            store32(value.payloadReg(), address);
            return;
          case 1:
            store8(value.payloadReg(), address);
            return;
          default: MOZ_CRASH("Bad payload width");
        }
    }

    void moveValue(const Value& val, const ValueOperand& dest);

    void moveValue(const ValueOperand& src, const ValueOperand& dest) {
        Register s0 = src.typeReg(), d0 = dest.typeReg(),
                 s1 = src.payloadReg(), d1 = dest.payloadReg();

        // Either one or both of the source registers could be the same as a
        // destination register.
        if (s1 == d0) {
            if (s0 == d1) {
                // If both are, this is just a swap of two registers.
                MOZ_ASSERT(d1 != ScratchRegister);
                MOZ_ASSERT(d0 != ScratchRegister);
                move32(d1, ScratchRegister);
                move32(d0, d1);
                move32(ScratchRegister, d0);
                return;
            }
            // If only one is, copy that source first.
            mozilla::Swap(s0, s1);
            mozilla::Swap(d0, d1);
        }

        if (s0 != d0)
            move32(s0, d0);
        if (s1 != d1)
            move32(s1, d1);
    }

    void storeValue(ValueOperand val, Operand dst);
    void storeValue(ValueOperand val, const BaseIndex& dest);
    void storeValue(JSValueType type, Register reg, BaseIndex dest);
    void storeValue(ValueOperand val, const Address& dest);
    void storeValue(JSValueType type, Register reg, Address dest);
    void storeValue(const Value& val, Address dest);
    void storeValue(const Value& val, BaseIndex dest);
    void storeValue(const Address& src, const Address& dest, Register temp) {
        load32(ToType(src), temp);
        store32(temp, ToType(dest));

        load32(ToPayload(src), temp);
        store32(temp, ToPayload(dest));
    }

    void loadValue(Address src, ValueOperand val);
    void loadValue(Operand dest, ValueOperand val) {
        loadValue(dest.toAddress(), val);
    }
    void loadValue(const BaseIndex& addr, ValueOperand val);
    void tagValue(JSValueType type, Register payload, ValueOperand dest);

    void pushValue(ValueOperand val);
    void popValue(ValueOperand val);
#if MOZ_LITTLE_ENDIAN
    void pushValue(const Value& val) {
        push(Imm32(val.toNunboxTag()));
        if (val.isGCThing())
            push(ImmGCPtr(val.toGCThing()));
        else
            push(Imm32(val.toNunboxPayload()));
    }
    void pushValue(JSValueType type, Register reg) {
        push(ImmTag(JSVAL_TYPE_TO_TAG(type)));
        ma_push(reg);
    }
#else
    void pushValue(const Value& val) {
        if (val.isGCThing())
            push(ImmGCPtr(val.toGCThing()));
        else
            push(Imm32(val.toNunboxPayload()));
        push(Imm32(val.toNunboxTag()));
    }
    void pushValue(JSValueType type, Register reg) {
        ma_push(reg);
        push(ImmTag(JSVAL_TYPE_TO_TAG(type)));
    }
#endif
    void pushValue(const Address& addr);

    void storePayload(const Value& val, Address dest);
    void storePayload(Register src, Address dest);
    void storePayload(const Value& val, const BaseIndex& dest);
    void storePayload(Register src, const BaseIndex& dest);
    void storeTypeTag(ImmTag tag, Address dest);
    void storeTypeTag(ImmTag tag, const BaseIndex& dest);

    void handleFailureWithHandlerTail(void* handler, Label* profilerExitTail);

    template <typename T>
    void atomicStore64(const T& mem, Register temp, Register64 value);


    /////////////////////////////////////////////////////////////////
    // Common interface.
    /////////////////////////////////////////////////////////////////
  public:
    // The following functions are exposed for use in platform-shared code.

    inline void incrementInt32Value(const Address& addr);

    void move32(Imm32 imm, Register dest);
    void move32(Register src, Register dest);

    void movePtr(Register src, Register dest);
    void movePtr(ImmWord imm, Register dest);
    void movePtr(ImmPtr imm, Register dest);
    void movePtr(wasm::SymbolicAddress imm, Register dest);
    void movePtr(ImmGCPtr imm, Register dest);

    void load8SignExtend(const Address& address, Register dest);
    void load8SignExtend(const BaseIndex& src, Register dest);

    void load8ZeroExtend(const Address& address, Register dest);
    void load8ZeroExtend(const BaseIndex& src, Register dest);

    void load16SignExtend(const Address& address, Register dest);
    void load16SignExtend(const BaseIndex& src, Register dest);

    void load16ZeroExtend(const Address& address, Register dest);
    void load16ZeroExtend(const BaseIndex& src, Register dest);

    void load32(const Address& address, Register dest);
    void load32(const BaseIndex& address, Register dest);
    void load32(AbsoluteAddress address, Register dest);
    void load32(wasm::SymbolicAddress address, Register dest);
    void load64(const Address& address, Register64 dest) {
        load32(LowWord(address), dest.low);
        load32(HighWord(address), dest.high);
    }

    void loadPtr(const Address& address, Register dest);
    void loadPtr(const BaseIndex& src, Register dest);
    void loadPtr(AbsoluteAddress address, Register dest);
    void loadPtr(wasm::SymbolicAddress address, Register dest);

    void loadPrivate(const Address& address, Register dest);

    void loadInt32x1(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x1(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x2(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x2(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x3(const Address& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x3(const BaseIndex& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x4(const Address& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeInt32x1(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x1(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x2(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x2(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x3(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x3(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x4(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void loadAlignedSimd128Int(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeAlignedSimd128Int(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void loadUnalignedSimd128Int(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadUnalignedSimd128Int(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeUnalignedSimd128Int(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void storeUnalignedSimd128Int(FloatRegister src, BaseIndex addr) { MOZ_CRASH("NYI"); }

    void loadFloat32x3(const Address& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadFloat32x3(const BaseIndex& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadFloat32x4(const Address& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeFloat32x4(FloatRegister src, const Address& addr) { MOZ_CRASH("NYI"); }

    void loadAlignedSimd128Float(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeAlignedSimd128Float(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void loadUnalignedSimd128Float(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadUnalignedSimd128Float(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeUnalignedSimd128Float(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void storeUnalignedSimd128Float(FloatRegister src, BaseIndex addr) { MOZ_CRASH("NYI"); }

    void loadUnalignedDouble(const wasm::MemoryAccessDesc& access, const BaseIndex& src,
                             Register temp, FloatRegister dest);

    void loadUnalignedFloat32(const wasm::MemoryAccessDesc& access, const BaseIndex& src,
                              Register temp, FloatRegister dest);

    void store8(Register src, const Address& address);
    void store8(Imm32 imm, const Address& address);
    void store8(Register src, const BaseIndex& address);
    void store8(Imm32 imm, const BaseIndex& address);

    void store16(Register src, const Address& address);
    void store16(Imm32 imm, const Address& address);
    void store16(Register src, const BaseIndex& address);
    void store16(Imm32 imm, const BaseIndex& address);

    void store32(Register src, AbsoluteAddress address);
    void store32(Register src, const Address& address);
    void store32(Register src, const BaseIndex& address);
    void store32(Imm32 src, const Address& address);
    void store32(Imm32 src, const BaseIndex& address);

    // NOTE: This will use second scratch on MIPS. Only ARM needs the
    // implementation without second scratch.
    void store32_NoSecondScratch(Imm32 src, const Address& address) {
        store32(src, address);
    }

    void store64(Register64 src, Address address) {
        store32(src.low, Address(address.base, address.offset + LOW_32_OFFSET));
        store32(src.high, Address(address.base, address.offset + HIGH_32_OFFSET));
    }

    void store64(Imm64 imm, Address address) {
        store32(imm.low(), Address(address.base, address.offset + LOW_32_OFFSET));
        store32(imm.hi(), Address(address.base, address.offset + HIGH_32_OFFSET));
    }

    template <typename T> void storePtr(ImmWord imm, T address);
    template <typename T> void storePtr(ImmPtr imm, T address);
    template <typename T> void storePtr(ImmGCPtr imm, T address);
    void storePtr(Register src, const Address& address);
    void storePtr(Register src, const BaseIndex& address);
    void storePtr(Register src, AbsoluteAddress dest);

    void storeUnalignedFloat32(const wasm::MemoryAccessDesc& access, FloatRegister src,
                               Register temp, const BaseIndex& dest);
    void storeUnalignedDouble(const wasm::MemoryAccessDesc& access, FloatRegister src,
                              Register temp, const BaseIndex& dest);

    void moveDouble(FloatRegister src, FloatRegister dest) {
        as_movd(dest, src);
    }

    void zeroDouble(FloatRegister reg) {
        moveToDoubleLo(zero, reg);
        moveToDoubleHi(zero, reg);
    }

    void breakpoint();

    void checkStackAlignment();

    void alignStackPointer();
    void restoreStackPointer();
    static void calculateAlignedStackPointer(void** stackPointer);

    // If source is a double, load it into dest. If source is int32,
    // convert it to double. Else, branch to failure.
    void ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure);

    void cmp64Set(Condition cond, Register64 lhs, Register64 rhs, Register dest);
    void cmp64Set(Condition cond, Register64 lhs, Imm64 val, Register dest);

  protected:
    bool buildOOLFakeExitFrame(void* fakeReturnAddr);

    void enterAtomic64Region(Register addr, Register spinlock, Register tmp);
    void exitAtomic64Region(Register spinlock);
    void wasmLoadI64Impl(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                         Register ptrScratch, Register64 output, Register tmp);
    void wasmStoreI64Impl(const wasm::MemoryAccessDesc& access, Register64 value, Register memoryBase,
                          Register ptr, Register ptrScratch, Register tmp);
    Condition ma_cmp64(Condition cond, Register64 lhs, Register64 rhs, Register dest);
    Condition ma_cmp64(Condition cond, Register64 lhs, Imm64 val, Register dest);

  public:
    CodeOffset labelForPatch() {
        return CodeOffset(nextOffset().getOffset());
    }

    void lea(Operand addr, Register dest) {
        ma_addu(dest, addr.baseReg(), Imm32(addr.disp()));
    }

    void abiret() {
        as_jr(ra);
        as_nop();
    }

    void ma_storeImm(Imm32 imm, const Address& addr) {
        ma_sw(imm, addr);
    }

    void moveFloat32(FloatRegister src, FloatRegister dest) {
        as_movs(dest, src);
    }
    void loadWasmGlobalPtr(uint32_t globalDataOffset, Register dest) {
        loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, globalArea) + globalDataOffset), dest);
    }
    void loadWasmPinnedRegsFromTls() {
        loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, memoryBase)), HeapReg);
    }

    // Instrumentation for entering and leaving the profiler.
    void profilerEnterFrame(Register framePtr, Register scratch);
    void profilerExitFrame();
};

typedef MacroAssemblerMIPSCompat MacroAssemblerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_mips32_MacroAssembler_mips32_h */
