/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_MacroAssembler_x86_shared_h
#define jit_x86_shared_MacroAssembler_x86_shared_h

#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"

#if defined(JS_CODEGEN_X86)
# include "jit/x86/Assembler-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/Assembler-x64.h"
#endif

#ifdef DEBUG
  #define CHECK_BYTEREG(reg)                                               \
      JS_BEGIN_MACRO                                                       \
        AllocatableGeneralRegisterSet byteRegs(Registers::SingleByteRegs); \
        MOZ_ASSERT(byteRegs.has(reg));                                     \
      JS_END_MACRO
  #define CHECK_BYTEREGS(r1, r2)                                           \
      JS_BEGIN_MACRO                                                       \
        AllocatableGeneralRegisterSet byteRegs(Registers::SingleByteRegs); \
        MOZ_ASSERT(byteRegs.has(r1));                                      \
        MOZ_ASSERT(byteRegs.has(r2));                                      \
      JS_END_MACRO
#else
  #define CHECK_BYTEREG(reg) (void)0
  #define CHECK_BYTEREGS(r1, r2) (void)0
#endif

namespace js {
namespace jit {

class MacroAssembler;

class MacroAssemblerX86Shared : public Assembler
{
  private:
    // Perform a downcast. Should be removed by Bug 996602.
    MacroAssembler& asMasm();
    const MacroAssembler& asMasm() const;

  public:
    typedef Vector<CodeOffset, 0, SystemAllocPolicy> UsesVector;

  protected:

    // For Double, Float and SimdData, make the move ctors explicit so that MSVC
    // knows what to use instead of copying these data structures.
    template<class T>
    struct Constant {
        typedef T Pod;

        T value;
        UsesVector uses;

        explicit Constant(const T& value) : value(value) {}
        Constant(Constant<T>&& other) : value(other.value), uses(mozilla::Move(other.uses)) {}
        explicit Constant(const Constant<T>&) = delete;
    };

    // Containers use SystemAllocPolicy since asm.js releases memory after each
    // function is compiled, and these need to live until after all functions
    // are compiled.
    using Double = Constant<double>;
    Vector<Double, 0, SystemAllocPolicy> doubles_;
    typedef HashMap<double, size_t, DefaultHasher<double>, SystemAllocPolicy> DoubleMap;
    DoubleMap doubleMap_;

    using Float = Constant<float>;
    Vector<Float, 0, SystemAllocPolicy> floats_;
    typedef HashMap<float, size_t, DefaultHasher<float>, SystemAllocPolicy> FloatMap;
    FloatMap floatMap_;

    struct SimdData : public Constant<SimdConstant> {
        explicit SimdData(SimdConstant d) : Constant<SimdConstant>(d) {}
        SimdData(SimdData&& d) : Constant<SimdConstant>(mozilla::Move(d)) {}
        explicit SimdData(const SimdData&) = delete;
        SimdConstant::Type type() const { return value.type(); }
    };

    Vector<SimdData, 0, SystemAllocPolicy> simds_;
    typedef HashMap<SimdConstant, size_t, SimdConstant, SystemAllocPolicy> SimdMap;
    SimdMap simdMap_;

    template<class T, class Map>
    T* getConstant(const typename T::Pod& value, Map& map, Vector<T, 0, SystemAllocPolicy>& vec);

    Float* getFloat(float f);
    Double* getDouble(double d);
    SimdData* getSimdData(const SimdConstant& v);

    bool asmMergeWith(const MacroAssemblerX86Shared& other);

  public:
    using Assembler::call;

    MacroAssemblerX86Shared()
    { }

    void compareDouble(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs) {
        if (cond & DoubleConditionBitInvert)
            vucomisd(lhs, rhs);
        else
            vucomisd(rhs, lhs);
    }
    void branchDouble(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs, Label* label)
    {
        compareDouble(cond, lhs, rhs);

        if (cond == DoubleEqual) {
            Label unordered;
            j(Parity, &unordered);
            j(Equal, label);
            bind(&unordered);
            return;
        }
        if (cond == DoubleNotEqualOrUnordered) {
            j(NotEqual, label);
            j(Parity, label);
            return;
        }

        MOZ_ASSERT(!(cond & DoubleConditionBitSpecial));
        j(ConditionFromDoubleCondition(cond), label);
    }

    void compareFloat(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs) {
        if (cond & DoubleConditionBitInvert)
            vucomiss(lhs, rhs);
        else
            vucomiss(rhs, lhs);
    }
    void branchFloat(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs, Label* label)
    {
        compareFloat(cond, lhs, rhs);

        if (cond == DoubleEqual) {
            Label unordered;
            j(Parity, &unordered);
            j(Equal, label);
            bind(&unordered);
            return;
        }
        if (cond == DoubleNotEqualOrUnordered) {
            j(NotEqual, label);
            j(Parity, label);
            return;
        }

        MOZ_ASSERT(!(cond & DoubleConditionBitSpecial));
        j(ConditionFromDoubleCondition(cond), label);
    }

    void branchNegativeZero(FloatRegister reg, Register scratch, Label* label, bool  maybeNonZero = true);
    void branchNegativeZeroFloat32(FloatRegister reg, Register scratch, Label* label);

    void move32(Imm32 imm, Register dest) {
        // Use the ImmWord version of mov to register, which has special
        // optimizations. Casting to uint32_t here ensures that the value
        // is zero-extended.
        mov(ImmWord(uint32_t(imm.value)), dest);
    }
    void move32(Imm32 imm, const Operand& dest) {
        movl(imm, dest);
    }
    void move32(Register src, Register dest) {
        movl(src, dest);
    }
    void move32(Register src, const Operand& dest) {
        movl(src, dest);
    }
    void neg32(Register reg) {
        negl(reg);
    }
    void test32(Register lhs, Register rhs) {
        testl(rhs, lhs);
    }
    void test32(const Address& addr, Imm32 imm) {
        testl(imm, Operand(addr));
    }
    void test32(const Operand lhs, Imm32 imm) {
        testl(imm, lhs);
    }
    void test32(Register lhs, Imm32 rhs) {
        testl(rhs, lhs);
    }
    void cmp32(Register lhs, Imm32 rhs) {
        cmpl(rhs, lhs);
    }
    void cmp32(Register lhs, Register rhs) {
        cmpl(rhs, lhs);
    }
    void cmp32(const Operand& lhs, Imm32 rhs) {
        cmpl(rhs, lhs);
    }
    void cmp32(const Operand& lhs, Register rhs) {
        cmpl(rhs, lhs);
    }
    void cmp32(Register lhs, const Operand& rhs) {
        cmpl(rhs, lhs);
    }
    CodeOffset cmp32WithPatch(Register lhs, Imm32 rhs) {
        return cmplWithPatch(rhs, lhs);
    }
    void add32(Register src, Register dest) {
        addl(src, dest);
    }
    void add32(Imm32 imm, Register dest) {
        addl(imm, dest);
    }
    void add32(Imm32 imm, const Operand& dest) {
        addl(imm, dest);
    }
    void add32(Imm32 imm, const Address& dest) {
        addl(imm, Operand(dest));
    }
    template <typename T>
    void branchAdd32(Condition cond, T src, Register dest, Label* label) {
        add32(src, dest);
        j(cond, label);
    }
    template <typename T>
    void branchSub32(Condition cond, T src, Register dest, Label* label) {
        subl(src, dest);
        j(cond, label);
    }
    void atomic_inc32(const Operand& addr) {
        lock_incl(addr);
    }
    void atomic_dec32(const Operand& addr) {
        lock_decl(addr);
    }

    template <typename T>
    void atomicFetchAdd8SignExtend(Register src, const T& mem, Register temp, Register output) {
        CHECK_BYTEREGS(src, output);
        if (src != output)
            movl(src, output);
        lock_xaddb(output, Operand(mem));
        movsbl(output, output);
    }

    template <typename T>
    void atomicFetchAdd8ZeroExtend(Register src, const T& mem, Register temp, Register output) {
        CHECK_BYTEREGS(src, output);
        MOZ_ASSERT(temp == InvalidReg);
        if (src != output)
            movl(src, output);
        lock_xaddb(output, Operand(mem));
        movzbl(output, output);
    }

    template <typename T>
    void atomicFetchAdd8SignExtend(Imm32 src, const T& mem, Register temp, Register output) {
        CHECK_BYTEREG(output);
        MOZ_ASSERT(temp == InvalidReg);
        movb(src, output);
        lock_xaddb(output, Operand(mem));
        movsbl(output, output);
    }

    template <typename T>
    void atomicFetchAdd8ZeroExtend(Imm32 src, const T& mem, Register temp, Register output) {
        CHECK_BYTEREG(output);
        MOZ_ASSERT(temp == InvalidReg);
        movb(src, output);
        lock_xaddb(output, Operand(mem));
        movzbl(output, output);
    }

    template <typename T>
    void atomicFetchAdd16SignExtend(Register src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        if (src != output)
            movl(src, output);
        lock_xaddw(output, Operand(mem));
        movswl(output, output);
    }

    template <typename T>
    void atomicFetchAdd16ZeroExtend(Register src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        if (src != output)
            movl(src, output);
        lock_xaddw(output, Operand(mem));
        movzwl(output, output);
    }

    template <typename T>
    void atomicFetchAdd16SignExtend(Imm32 src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        movl(src, output);
        lock_xaddw(output, Operand(mem));
        movswl(output, output);
    }

    template <typename T>
    void atomicFetchAdd16ZeroExtend(Imm32 src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        movl(src, output);
        lock_xaddw(output, Operand(mem));
        movzwl(output, output);
    }

    template <typename T>
    void atomicFetchAdd32(Register src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        if (src != output)
            movl(src, output);
        lock_xaddl(output, Operand(mem));
    }

    template <typename T>
    void atomicFetchAdd32(Imm32 src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        movl(src, output);
        lock_xaddl(output, Operand(mem));
    }

    template <typename T>
    void atomicFetchSub8SignExtend(Register src, const T& mem, Register temp, Register output) {
        CHECK_BYTEREGS(src, output);
        MOZ_ASSERT(temp == InvalidReg);
        if (src != output)
            movl(src, output);
        negl(output);
        lock_xaddb(output, Operand(mem));
        movsbl(output, output);
    }

    template <typename T>
    void atomicFetchSub8ZeroExtend(Register src, const T& mem, Register temp, Register output) {
        CHECK_BYTEREGS(src, output);
        MOZ_ASSERT(temp == InvalidReg);
        if (src != output)
            movl(src, output);
        negl(output);
        lock_xaddb(output, Operand(mem));
        movzbl(output, output);
    }

    template <typename T>
    void atomicFetchSub8SignExtend(Imm32 src, const T& mem, Register temp, Register output) {
        CHECK_BYTEREG(output);
        MOZ_ASSERT(temp == InvalidReg);
        movb(Imm32(-src.value), output);
        lock_xaddb(output, Operand(mem));
        movsbl(output, output);
    }

    template <typename T>
    void atomicFetchSub8ZeroExtend(Imm32 src, const T& mem, Register temp, Register output) {
        CHECK_BYTEREG(output);
        MOZ_ASSERT(temp == InvalidReg);
        movb(Imm32(-src.value), output);
        lock_xaddb(output, Operand(mem));
        movzbl(output, output);
    }

    template <typename T>
    void atomicFetchSub16SignExtend(Register src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        if (src != output)
            movl(src, output);
        negl(output);
        lock_xaddw(output, Operand(mem));
        movswl(output, output);
    }

    template <typename T>
    void atomicFetchSub16ZeroExtend(Register src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        if (src != output)
            movl(src, output);
        negl(output);
        lock_xaddw(output, Operand(mem));
        movzwl(output, output);
    }

    template <typename T>
    void atomicFetchSub16SignExtend(Imm32 src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        movl(Imm32(-src.value), output);
        lock_xaddw(output, Operand(mem));
        movswl(output, output);
    }

    template <typename T>
    void atomicFetchSub16ZeroExtend(Imm32 src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        movl(Imm32(-src.value), output);
        lock_xaddw(output, Operand(mem));
        movzwl(output, output);
    }

    template <typename T>
    void atomicFetchSub32(Register src, const T& mem, Register temp, Register output) {
        MOZ_ASSERT(temp == InvalidReg);
        if (src != output)
            movl(src, output);
        negl(output);
        lock_xaddl(output, Operand(mem));
    }

    template <typename T>
    void atomicFetchSub32(Imm32 src, const T& mem, Register temp, Register output) {
        movl(Imm32(-src.value), output);
        lock_xaddl(output, Operand(mem));
    }

    // requires output == eax
#define ATOMIC_BITOP_BODY(LOAD, OP, LOCK_CMPXCHG) \
        MOZ_ASSERT(output == eax);                \
        LOAD(Operand(mem), eax);                  \
        Label again;                              \
        bind(&again);                             \
        movl(eax, temp);                          \
        OP(src, temp);                            \
        LOCK_CMPXCHG(temp, Operand(mem));         \
        j(NonZero, &again);

    template <typename S, typename T>
    void atomicFetchAnd8SignExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movb, andl, lock_cmpxchgb)
        CHECK_BYTEREG(temp);
        movsbl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchAnd8ZeroExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movb, andl, lock_cmpxchgb)
        CHECK_BYTEREG(temp);
        movzbl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchAnd16SignExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movw, andl, lock_cmpxchgw)
        movswl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchAnd16ZeroExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movw, andl, lock_cmpxchgw)
        movzwl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchAnd32(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movl, andl, lock_cmpxchgl)
    }

    template <typename S, typename T>
    void atomicFetchOr8SignExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movb, orl, lock_cmpxchgb)
        CHECK_BYTEREG(temp);
        movsbl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchOr8ZeroExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movb, orl, lock_cmpxchgb)
        CHECK_BYTEREG(temp);
        movzbl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchOr16SignExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movw, orl, lock_cmpxchgw)
        movswl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchOr16ZeroExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movw, orl, lock_cmpxchgw)
        movzwl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchOr32(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movl, orl, lock_cmpxchgl)
    }

    template <typename S, typename T>
    void atomicFetchXor8SignExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movb, xorl, lock_cmpxchgb)
        CHECK_BYTEREG(temp);
        movsbl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchXor8ZeroExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movb, xorl, lock_cmpxchgb)
        CHECK_BYTEREG(temp);
        movzbl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchXor16SignExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movw, xorl, lock_cmpxchgw)
        movswl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchXor16ZeroExtend(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movw, xorl, lock_cmpxchgw)
        movzwl(eax, eax);
    }
    template <typename S, typename T>
    void atomicFetchXor32(const S& src, const T& mem, Register temp, Register output) {
        ATOMIC_BITOP_BODY(movl, xorl, lock_cmpxchgl)
    }

#undef ATOMIC_BITOP_BODY

    // S is Register or Imm32; T is Address or BaseIndex.

    template <typename S, typename T>
    void atomicAdd8(const S& src, const T& mem) {
        lock_addb(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicAdd16(const S& src, const T& mem) {
        lock_addw(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicAdd32(const S& src, const T& mem) {
        lock_addl(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicSub8(const S& src, const T& mem) {
        lock_subb(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicSub16(const S& src, const T& mem) {
        lock_subw(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicSub32(const S& src, const T& mem) {
        lock_subl(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicAnd8(const S& src, const T& mem) {
        lock_andb(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicAnd16(const S& src, const T& mem) {
        lock_andw(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicAnd32(const S& src, const T& mem) {
        lock_andl(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicOr8(const S& src, const T& mem) {
        lock_orb(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicOr16(const S& src, const T& mem) {
        lock_orw(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicOr32(const S& src, const T& mem) {
        lock_orl(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicXor8(const S& src, const T& mem) {
        lock_xorb(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicXor16(const S& src, const T& mem) {
        lock_xorw(src, Operand(mem));
    }
    template <typename S, typename T>
    void atomicXor32(const S& src, const T& mem) {
        lock_xorl(src, Operand(mem));
    }

    void storeLoadFence() {
        // This implementation follows Linux.
        if (HasSSE2())
            masm.mfence();
        else
            lock_addl(Imm32(0), Operand(Address(esp, 0)));
    }

    void branch16(Condition cond, Register lhs, Register rhs, Label* label) {
        cmpw(rhs, lhs);
        j(cond, label);
    }
    void branch32(Condition cond, const Operand& lhs, Register rhs, Label* label) {
        cmp32(lhs, rhs);
        j(cond, label);
    }
    void branch32(Condition cond, const Operand& lhs, Imm32 rhs, Label* label) {
        cmp32(lhs, rhs);
        j(cond, label);
    }
    void branch32(Condition cond, const Address& lhs, Register rhs, Label* label) {
        cmp32(Operand(lhs), rhs);
        j(cond, label);
    }
    void branch32(Condition cond, const Address& lhs, Imm32 imm, Label* label) {
        cmp32(Operand(lhs), imm);
        j(cond, label);
    }
    void branch32(Condition cond, const BaseIndex& lhs, Register rhs, Label* label) {
        cmp32(Operand(lhs), rhs);
        j(cond, label);
    }
    void branch32(Condition cond, const BaseIndex& lhs, Imm32 imm, Label* label) {
        cmp32(Operand(lhs), imm);
        j(cond, label);
    }
    void branch32(Condition cond, Register lhs, Imm32 imm, Label* label) {
        cmp32(lhs, imm);
        j(cond, label);
    }
    void branch32(Condition cond, Register lhs, Register rhs, Label* label) {
        cmp32(lhs, rhs);
        j(cond, label);
    }
    void branchTest16(Condition cond, Register lhs, Register rhs, Label* label) {
        testw(rhs, lhs);
        j(cond, label);
    }
    void branchTest32(Condition cond, Register lhs, Register rhs, Label* label) {
        MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
        test32(lhs, rhs);
        j(cond, label);
    }
    void branchTest32(Condition cond, Register lhs, Imm32 imm, Label* label) {
        MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
        test32(lhs, imm);
        j(cond, label);
    }
    void branchTest32(Condition cond, const Address& address, Imm32 imm, Label* label) {
        MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
        test32(Operand(address), imm);
        j(cond, label);
    }
    void branchTest32(Condition cond, const Operand& lhs, Imm32 imm, Label* label) {
        MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
        test32(lhs, imm);
        j(cond, label);
    }

    void jump(Label* label) {
        jmp(label);
    }
    void jump(JitCode* code) {
        jmp(code);
    }
    void jump(RepatchLabel* label) {
        jmp(label);
    }
    void jump(Register reg) {
        jmp(Operand(reg));
    }
    void jump(const Address& addr) {
        jmp(Operand(addr));
    }

    void convertInt32ToDouble(Register src, FloatRegister dest) {
        // vcvtsi2sd and friends write only part of their output register, which
        // causes slowdowns on out-of-order processors. Explicitly break
        // dependencies with vxorpd (and vxorps elsewhere), which are handled
        // specially in modern CPUs, for this purpose. See sections 8.14, 9.8,
        // 10.8, 12.9, 13.16, 14.14, and 15.8 of Agner's Microarchitecture
        // document.
        zeroDouble(dest);
        vcvtsi2sd(src, dest, dest);
    }
    void convertInt32ToDouble(const Address& src, FloatRegister dest) {
        convertInt32ToDouble(Operand(src), dest);
    }
    void convertInt32ToDouble(const BaseIndex& src, FloatRegister dest) {
        convertInt32ToDouble(Operand(src), dest);
    }
    void convertInt32ToDouble(const Operand& src, FloatRegister dest) {
        // Clear the output register first to break dependencies; see above;
        zeroDouble(dest);
        vcvtsi2sd(Operand(src), dest, dest);
    }
    void convertInt32ToFloat32(Register src, FloatRegister dest) {
        // Clear the output register first to break dependencies; see above;
        zeroFloat32(dest);
        vcvtsi2ss(src, dest, dest);
    }
    void convertInt32ToFloat32(const Address& src, FloatRegister dest) {
        convertInt32ToFloat32(Operand(src), dest);
    }
    void convertInt32ToFloat32(const Operand& src, FloatRegister dest) {
        // Clear the output register first to break dependencies; see above;
        zeroFloat32(dest);
        vcvtsi2ss(src, dest, dest);
    }
    Condition testDoubleTruthy(bool truthy, FloatRegister reg) {
        ScratchDoubleScope scratch(asMasm());
        zeroDouble(scratch);
        vucomisd(reg, scratch);
        return truthy ? NonZero : Zero;
    }
    void branchTestDoubleTruthy(bool truthy, FloatRegister reg, Label* label) {
        Condition cond = testDoubleTruthy(truthy, reg);
        j(cond, label);
    }

    // Class which ensures that registers used in byte ops are compatible with
    // such instructions, even if the original register passed in wasn't. This
    // only applies to x86, as on x64 all registers are valid single byte regs.
    // This doesn't lead to great code but helps to simplify code generation.
    //
    // Note that this can currently only be used in cases where the register is
    // read from by the guarded instruction, not written to.
    class AutoEnsureByteRegister {
        MacroAssemblerX86Shared* masm;
        Register original_;
        Register substitute_;

      public:
        template <typename T>
        AutoEnsureByteRegister(MacroAssemblerX86Shared* masm, T address, Register reg)
          : masm(masm), original_(reg)
        {
            AllocatableGeneralRegisterSet singleByteRegs(Registers::SingleByteRegs);
            if (singleByteRegs.has(reg)) {
                substitute_ = reg;
            } else {
                MOZ_ASSERT(address.base != StackPointer);
                do {
                    substitute_ = singleByteRegs.takeAny();
                } while (Operand(address).containsReg(substitute_));

                masm->push(substitute_);
                masm->mov(reg, substitute_);
            }
        }

        ~AutoEnsureByteRegister() {
            if (original_ != substitute_)
                masm->pop(substitute_);
        }

        Register reg() {
            return substitute_;
        }
    };

    void load8ZeroExtend(const Address& src, Register dest) {
        movzbl(Operand(src), dest);
    }
    void load8ZeroExtend(const BaseIndex& src, Register dest) {
        movzbl(Operand(src), dest);
    }
    void load8SignExtend(const Address& src, Register dest) {
        movsbl(Operand(src), dest);
    }
    void load8SignExtend(const BaseIndex& src, Register dest) {
        movsbl(Operand(src), dest);
    }
    template <typename T>
    void store8(Imm32 src, const T& dest) {
        movb(src, Operand(dest));
    }
    template <typename T>
    void store8(Register src, const T& dest) {
        AutoEnsureByteRegister ensure(this, dest, src);
        movb(ensure.reg(), Operand(dest));
    }
    template <typename T>
    void compareExchange8ZeroExtend(const T& mem, Register oldval, Register newval, Register output) {
        MOZ_ASSERT(output == eax);
        CHECK_BYTEREG(newval);
        if (oldval != output)
            movl(oldval, output);
        lock_cmpxchgb(newval, Operand(mem));
        movzbl(output, output);
    }
    template <typename T>
    void compareExchange8SignExtend(const T& mem, Register oldval, Register newval, Register output) {
        MOZ_ASSERT(output == eax);
        CHECK_BYTEREG(newval);
        if (oldval != output)
            movl(oldval, output);
        lock_cmpxchgb(newval, Operand(mem));
        movsbl(output, output);
    }
    template <typename T>
    void atomicExchange8ZeroExtend(const T& mem, Register value, Register output) {
        if (value != output)
            movl(value, output);
        xchgb(output, Operand(mem));
        movzbl(output, output);
    }
    template <typename T>
    void atomicExchange8SignExtend(const T& mem, Register value, Register output) {
        if (value != output)
            movl(value, output);
        xchgb(output, Operand(mem));
        movsbl(output, output);
    }
    void load16ZeroExtend(const Address& src, Register dest) {
        movzwl(Operand(src), dest);
    }
    void load16ZeroExtend(const BaseIndex& src, Register dest) {
        movzwl(Operand(src), dest);
    }
    template <typename S, typename T>
    void store16(const S& src, const T& dest) {
        movw(src, Operand(dest));
    }
    template <typename T>
    void compareExchange16ZeroExtend(const T& mem, Register oldval, Register newval, Register output) {
        MOZ_ASSERT(output == eax);
        if (oldval != output)
            movl(oldval, output);
        lock_cmpxchgw(newval, Operand(mem));
        movzwl(output, output);
    }
    template <typename T>
    void compareExchange16SignExtend(const T& mem, Register oldval, Register newval, Register output) {
        MOZ_ASSERT(output == eax);
        if (oldval != output)
            movl(oldval, output);
        lock_cmpxchgw(newval, Operand(mem));
        movswl(output, output);
    }
    template <typename T>
    void atomicExchange16ZeroExtend(const T& mem, Register value, Register output) {
        if (value != output)
            movl(value, output);
        xchgw(output, Operand(mem));
        movzwl(output, output);
    }
    template <typename T>
    void atomicExchange16SignExtend(const T& mem, Register value, Register output) {
        if (value != output)
            movl(value, output);
        xchgw(output, Operand(mem));
        movswl(output, output);
    }
    void load16SignExtend(const Address& src, Register dest) {
        movswl(Operand(src), dest);
    }
    void load16SignExtend(const BaseIndex& src, Register dest) {
        movswl(Operand(src), dest);
    }
    void load32(const Address& address, Register dest) {
        movl(Operand(address), dest);
    }
    void load32(const BaseIndex& src, Register dest) {
        movl(Operand(src), dest);
    }
    void load32(const Operand& src, Register dest) {
        movl(src, dest);
    }
    template <typename S, typename T>
    void store32(const S& src, const T& dest) {
        movl(src, Operand(dest));
    }
    template <typename T>
    void compareExchange32(const T& mem, Register oldval, Register newval, Register output) {
        MOZ_ASSERT(output == eax);
        if (oldval != output)
            movl(oldval, output);
        lock_cmpxchgl(newval, Operand(mem));
    }
    template <typename T>
    void atomicExchange32(const T& mem, Register value, Register output) {
        if (value != output)
            movl(value, output);
        xchgl(output, Operand(mem));
    }
    template <typename S, typename T>
    void store32_NoSecondScratch(const S& src, const T& dest) {
        store32(src, dest);
    }
    void loadDouble(const Address& src, FloatRegister dest) {
        vmovsd(src, dest);
    }
    void loadDouble(const BaseIndex& src, FloatRegister dest) {
        vmovsd(src, dest);
    }
    void loadDouble(const Operand& src, FloatRegister dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            loadDouble(src.toAddress(), dest);
            break;
          case Operand::MEM_SCALE:
            loadDouble(src.toBaseIndex(), dest);
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void storeDouble(FloatRegister src, const Address& dest) {
        vmovsd(src, dest);
    }
    void storeDouble(FloatRegister src, const BaseIndex& dest) {
        vmovsd(src, dest);
    }
    void storeDouble(FloatRegister src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            storeDouble(src, dest.toAddress());
            break;
          case Operand::MEM_SCALE:
            storeDouble(src, dest.toBaseIndex());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void moveDouble(FloatRegister src, FloatRegister dest) {
        // Use vmovapd instead of vmovsd to avoid dependencies.
        vmovapd(src, dest);
    }
    void zeroDouble(FloatRegister reg) {
        vxorpd(reg, reg, reg);
    }
    void zeroFloat32(FloatRegister reg) {
        vxorps(reg, reg, reg);
    }
    void negateDouble(FloatRegister reg) {
        // From MacroAssemblerX86Shared::maybeInlineDouble
        ScratchDoubleScope scratch(asMasm());
        vpcmpeqw(scratch, scratch, scratch);
        vpsllq(Imm32(63), scratch, scratch);

        // XOR the float in a float register with -0.0.
        vxorpd(scratch, reg, reg); // s ^ 0x80000000000000
    }
    void negateFloat(FloatRegister reg) {
        ScratchFloat32Scope scratch(asMasm());
        vpcmpeqw(scratch, scratch, scratch);
        vpsllq(Imm32(31), scratch, scratch);

        // XOR the float in a float register with -0.0.
        vxorps(scratch, reg, reg); // s ^ 0x80000000
    }
    void addDouble(FloatRegister src, FloatRegister dest) {
        vaddsd(src, dest, dest);
    }
    void subDouble(FloatRegister src, FloatRegister dest) {
        vsubsd(src, dest, dest);
    }
    void mulDouble(FloatRegister src, FloatRegister dest) {
        vmulsd(src, dest, dest);
    }
    void divDouble(FloatRegister src, FloatRegister dest) {
        vdivsd(src, dest, dest);
    }
    void addFloat32(FloatRegister src, FloatRegister dest) {
        vaddss(src, dest, dest);
    }
    void convertFloat32ToDouble(FloatRegister src, FloatRegister dest) {
        vcvtss2sd(src, dest, dest);
    }
    void convertDoubleToFloat32(FloatRegister src, FloatRegister dest) {
        vcvtsd2ss(src, dest, dest);
    }

    void convertFloat32x4ToInt32x4(FloatRegister src, FloatRegister dest) {
        // Note that if the conversion failed (because the converted
        // result is larger than the maximum signed int32, or less than the
        // least signed int32, or NaN), this will return the undefined integer
        // value (0x8000000).
        vcvttps2dq(src, dest);
    }
    void convertInt32x4ToFloat32x4(FloatRegister src, FloatRegister dest) {
        vcvtdq2ps(src, dest);
    }

    void bitwiseAndX4(const Operand& src, FloatRegister dest) {
        // TODO Using the "ps" variant for all types incurs a domain crossing
        // penalty for integer types and double.
        vandps(src, dest, dest);
    }
    void bitwiseAndNotX4(const Operand& src, FloatRegister dest) {
        vandnps(src, dest, dest);
    }
    void bitwiseOrX4(const Operand& src, FloatRegister dest) {
        vorps(src, dest, dest);
    }
    void bitwiseXorX4(const Operand& src, FloatRegister dest) {
        vxorps(src, dest, dest);
    }
    void zeroFloat32x4(FloatRegister dest) {
        vxorps(dest, dest, dest);
    }
    void zeroInt32x4(FloatRegister dest) {
        vpxor(dest, dest, dest);
    }

    template <class T, class Reg> inline void loadScalar(const Operand& src, Reg dest);
    template <class T, class Reg> inline void storeScalar(Reg src, const Address& dest);
    template <class T> inline void loadAlignedVector(const Address& src, FloatRegister dest);
    template <class T> inline void storeAlignedVector(FloatRegister src, const Address& dest);

    void loadInt32x1(const Address& src, FloatRegister dest) {
        vmovd(Operand(src), dest);
    }
    void loadInt32x1(const BaseIndex& src, FloatRegister dest) {
        vmovd(Operand(src), dest);
    }
    void loadInt32x2(const Address& src, FloatRegister dest) {
        vmovq(Operand(src), dest);
    }
    void loadInt32x2(const BaseIndex& src, FloatRegister dest) {
        vmovq(Operand(src), dest);
    }
    void loadInt32x3(const BaseIndex& src, FloatRegister dest) {
        BaseIndex srcZ(src);
        srcZ.offset += 2 * sizeof(int32_t);

        ScratchSimd128Scope scratch(asMasm());
        vmovq(Operand(src), dest);
        vmovd(Operand(srcZ), scratch);
        vmovlhps(scratch, dest, dest);
    }
    void loadInt32x3(const Address& src, FloatRegister dest) {
        Address srcZ(src);
        srcZ.offset += 2 * sizeof(int32_t);

        ScratchSimd128Scope scratch(asMasm());
        vmovq(Operand(src), dest);
        vmovd(Operand(srcZ), scratch);
        vmovlhps(scratch, dest, dest);
    }

    void loadAlignedInt32x4(const Address& src, FloatRegister dest) {
        vmovdqa(Operand(src), dest);
    }
    void loadAlignedInt32x4(const Operand& src, FloatRegister dest) {
        vmovdqa(src, dest);
    }
    void storeAlignedInt32x4(FloatRegister src, const Address& dest) {
        vmovdqa(src, Operand(dest));
    }
    void moveInt32x4(FloatRegister src, FloatRegister dest) {
        vmovdqa(src, dest);
    }
    FloatRegister reusedInputInt32x4(FloatRegister src, FloatRegister dest) {
        if (HasAVX())
            return src;
        moveInt32x4(src, dest);
        return dest;
    }
    FloatRegister reusedInputAlignedInt32x4(const Operand& src, FloatRegister dest) {
        if (HasAVX() && src.kind() == Operand::FPREG)
            return FloatRegister::FromCode(src.fpu());
        loadAlignedInt32x4(src, dest);
        return dest;
    }
    void loadUnalignedInt32x4(const Address& src, FloatRegister dest) {
        vmovdqu(Operand(src), dest);
    }
    void loadUnalignedInt32x4(const BaseIndex& src, FloatRegister dest) {
        vmovdqu(Operand(src), dest);
    }
    void loadUnalignedInt32x4(const Operand& src, FloatRegister dest) {
        vmovdqu(src, dest);
    }

    void storeInt32x1(FloatRegister src, const Address& dest) {
        vmovd(src, Operand(dest));
    }
    void storeInt32x1(FloatRegister src, const BaseIndex& dest) {
        vmovd(src, Operand(dest));
    }
    void storeInt32x2(FloatRegister src, const Address& dest) {
        vmovq(src, Operand(dest));
    }
    void storeInt32x2(FloatRegister src, const BaseIndex& dest) {
        vmovq(src, Operand(dest));
    }
    void storeInt32x3(FloatRegister src, const Address& dest) {
        Address destZ(dest);
        destZ.offset += 2 * sizeof(int32_t);
        vmovq(src, Operand(dest));
        ScratchSimd128Scope scratch(asMasm());
        vmovhlps(src, scratch, scratch);
        vmovd(scratch, Operand(destZ));
    }
    void storeInt32x3(FloatRegister src, const BaseIndex& dest) {
        BaseIndex destZ(dest);
        destZ.offset += 2 * sizeof(int32_t);
        vmovq(src, Operand(dest));
        ScratchSimd128Scope scratch(asMasm());
        vmovhlps(src, scratch, scratch);
        vmovd(scratch, Operand(destZ));
    }

    void storeUnalignedInt32x4(FloatRegister src, const Address& dest) {
        vmovdqu(src, Operand(dest));
    }
    void storeUnalignedInt32x4(FloatRegister src, const BaseIndex& dest) {
        vmovdqu(src, Operand(dest));
    }
    void storeUnalignedInt32x4(FloatRegister src, const Operand& dest) {
        vmovdqu(src, dest);
    }
    void packedEqualInt32x4(const Operand& src, FloatRegister dest) {
        vpcmpeqd(src, dest, dest);
    }
    void packedGreaterThanInt32x4(const Operand& src, FloatRegister dest) {
        vpcmpgtd(src, dest, dest);
    }
    void packedAddInt32(const Operand& src, FloatRegister dest) {
        vpaddd(src, dest, dest);
    }
    void packedSubInt32(const Operand& src, FloatRegister dest) {
        vpsubd(src, dest, dest);
    }
    void packedRcpApproximationFloat32x4(const Operand& src, FloatRegister dest) {
        // This function is an approximation of the result, this might need
        // fix up if the spec requires a given precision for this operation.
        // TODO See also bug 1068028.
        vrcpps(src, dest);
    }
    void packedRcpSqrtApproximationFloat32x4(const Operand& src, FloatRegister dest) {
        // TODO See comment above. See also bug 1068028.
        vrsqrtps(src, dest);
    }
    void packedSqrtFloat32x4(const Operand& src, FloatRegister dest) {
        vsqrtps(src, dest);
    }

    void packedLeftShiftByScalar(FloatRegister src, FloatRegister dest) {
        vpslld(src, dest, dest);
    }
    void packedLeftShiftByScalar(Imm32 count, FloatRegister dest) {
        vpslld(count, dest, dest);
    }
    void packedRightShiftByScalar(FloatRegister src, FloatRegister dest) {
        vpsrad(src, dest, dest);
    }
    void packedRightShiftByScalar(Imm32 count, FloatRegister dest) {
        vpsrad(count, dest, dest);
    }
    void packedUnsignedRightShiftByScalar(FloatRegister src, FloatRegister dest) {
        vpsrld(src, dest, dest);
    }
    void packedUnsignedRightShiftByScalar(Imm32 count, FloatRegister dest) {
        vpsrld(count, dest, dest);
    }

    void loadFloat32x3(const Address& src, FloatRegister dest) {
        Address srcZ(src);
        srcZ.offset += 2 * sizeof(float);
        vmovsd(src, dest);
        ScratchSimd128Scope scratch(asMasm());
        vmovss(srcZ, scratch);
        vmovlhps(scratch, dest, dest);
    }
    void loadFloat32x3(const BaseIndex& src, FloatRegister dest) {
        BaseIndex srcZ(src);
        srcZ.offset += 2 * sizeof(float);
        vmovsd(src, dest);
        ScratchSimd128Scope scratch(asMasm());
        vmovss(srcZ, scratch);
        vmovlhps(scratch, dest, dest);
    }

    void loadAlignedFloat32x4(const Address& src, FloatRegister dest) {
        vmovaps(Operand(src), dest);
    }
    void loadAlignedFloat32x4(const Operand& src, FloatRegister dest) {
        vmovaps(src, dest);
    }

    void storeFloat32x3(FloatRegister src, const Address& dest) {
        Address destZ(dest);
        destZ.offset += 2 * sizeof(int32_t);
        storeDouble(src, dest);
        ScratchSimd128Scope scratch(asMasm());
        vmovhlps(src, scratch, scratch);
        storeFloat32(scratch, destZ);
    }
    void storeFloat32x3(FloatRegister src, const BaseIndex& dest) {
        BaseIndex destZ(dest);
        destZ.offset += 2 * sizeof(int32_t);
        storeDouble(src, dest);
        ScratchSimd128Scope scratch(asMasm());
        vmovhlps(src, scratch, scratch);
        storeFloat32(scratch, destZ);
    }
    void storeAlignedFloat32x4(FloatRegister src, const Address& dest) {
        vmovaps(src, Operand(dest));
    }
    void moveFloat32x4(FloatRegister src, FloatRegister dest) {
        vmovaps(src, dest);
    }
    FloatRegister reusedInputFloat32x4(FloatRegister src, FloatRegister dest) {
        if (HasAVX())
            return src;
        moveFloat32x4(src, dest);
        return dest;
    }
    FloatRegister reusedInputAlignedFloat32x4(const Operand& src, FloatRegister dest) {
        if (HasAVX() && src.kind() == Operand::FPREG)
            return FloatRegister::FromCode(src.fpu());
        loadAlignedFloat32x4(src, dest);
        return dest;
    }
    void loadUnalignedFloat32x4(const Address& src, FloatRegister dest) {
        vmovups(Operand(src), dest);
    }
    void loadUnalignedFloat32x4(const BaseIndex& src, FloatRegister dest) {
        vmovdqu(Operand(src), dest);
    }
    void loadUnalignedFloat32x4(const Operand& src, FloatRegister dest) {
        vmovups(src, dest);
    }
    void storeUnalignedFloat32x4(FloatRegister src, const Address& dest) {
        vmovups(src, Operand(dest));
    }
    void storeUnalignedFloat32x4(FloatRegister src, const BaseIndex& dest) {
        vmovups(src, Operand(dest));
    }
    void storeUnalignedFloat32x4(FloatRegister src, const Operand& dest) {
        vmovups(src, dest);
    }
    void packedAddFloat32(const Operand& src, FloatRegister dest) {
        vaddps(src, dest, dest);
    }
    void packedSubFloat32(const Operand& src, FloatRegister dest) {
        vsubps(src, dest, dest);
    }
    void packedMulFloat32(const Operand& src, FloatRegister dest) {
        vmulps(src, dest, dest);
    }
    void packedDivFloat32(const Operand& src, FloatRegister dest) {
        vdivps(src, dest, dest);
    }

    static uint32_t ComputeShuffleMask(uint32_t x = LaneX, uint32_t y = LaneY,
                                       uint32_t z = LaneZ, uint32_t w = LaneW)
    {
        MOZ_ASSERT(x < 4 && y < 4 && z < 4 && w < 4);
        uint32_t r = (w << 6) | (z << 4) | (y << 2) | (x << 0);
        MOZ_ASSERT(r < 256);
        return r;
    }

    void shuffleInt32(uint32_t mask, FloatRegister src, FloatRegister dest) {
        vpshufd(mask, src, dest);
    }
    void moveLowInt32(FloatRegister src, Register dest) {
        vmovd(src, dest);
    }

    void moveHighPairToLowPairFloat32(FloatRegister src, FloatRegister dest) {
        vmovhlps(src, dest, dest);
    }
    void shuffleFloat32(uint32_t mask, FloatRegister src, FloatRegister dest) {
        // The shuffle instruction on x86 is such that it moves 2 words from
        // the dest and 2 words from the src operands. To simplify things, just
        // clobber the output with the input and apply the instruction
        // afterwards.
        // Note: this is useAtStart-safe because src isn't read afterwards.
        FloatRegister srcCopy = reusedInputFloat32x4(src, dest);
        vshufps(mask, srcCopy, srcCopy, dest);
    }
    void shuffleMix(uint32_t mask, const Operand& src, FloatRegister dest) {
        // Note this uses vshufps, which is a cross-domain penalty on CPU where it
        // applies, but that's the way clang and gcc do it.
        vshufps(mask, src, dest, dest);
    }

    void moveFloatAsDouble(Register src, FloatRegister dest) {
        vmovd(src, dest);
        vcvtss2sd(dest, dest, dest);
    }
    void loadFloatAsDouble(const Address& src, FloatRegister dest) {
        vmovss(src, dest);
        vcvtss2sd(dest, dest, dest);
    }
    void loadFloatAsDouble(const BaseIndex& src, FloatRegister dest) {
        vmovss(src, dest);
        vcvtss2sd(dest, dest, dest);
    }
    void loadFloatAsDouble(const Operand& src, FloatRegister dest) {
        loadFloat32(src, dest);
        vcvtss2sd(dest, dest, dest);
    }
    void loadFloat32(const Address& src, FloatRegister dest) {
        vmovss(src, dest);
    }
    void loadFloat32(const BaseIndex& src, FloatRegister dest) {
        vmovss(src, dest);
    }
    void loadFloat32(const Operand& src, FloatRegister dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            loadFloat32(src.toAddress(), dest);
            break;
          case Operand::MEM_SCALE:
            loadFloat32(src.toBaseIndex(), dest);
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void storeFloat32(FloatRegister src, const Address& dest) {
        vmovss(src, dest);
    }
    void storeFloat32(FloatRegister src, const BaseIndex& dest) {
        vmovss(src, dest);
    }
    void storeFloat32(FloatRegister src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            storeFloat32(src, dest.toAddress());
            break;
          case Operand::MEM_SCALE:
            storeFloat32(src, dest.toBaseIndex());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void moveFloat32(FloatRegister src, FloatRegister dest) {
        // Use vmovaps instead of vmovss to avoid dependencies.
        vmovaps(src, dest);
    }

    // Checks whether a double is representable as a 32-bit integer. If so, the
    // integer is written to the output register. Otherwise, a bailout is taken to
    // the given snapshot. This function overwrites the scratch float register.
    void convertDoubleToInt32(FloatRegister src, Register dest, Label* fail,
                              bool negativeZeroCheck = true)
    {
        // Check for -0.0
        if (negativeZeroCheck)
            branchNegativeZero(src, dest, fail);

        ScratchDoubleScope scratch(asMasm());
        vcvttsd2si(src, dest);
        convertInt32ToDouble(dest, scratch);
        vucomisd(scratch, src);
        j(Assembler::Parity, fail);
        j(Assembler::NotEqual, fail);
    }

    // Checks whether a float32 is representable as a 32-bit integer. If so, the
    // integer is written to the output register. Otherwise, a bailout is taken to
    // the given snapshot. This function overwrites the scratch float register.
    void convertFloat32ToInt32(FloatRegister src, Register dest, Label* fail,
                               bool negativeZeroCheck = true)
    {
        // Check for -0.0
        if (negativeZeroCheck)
            branchNegativeZeroFloat32(src, dest, fail);

        ScratchFloat32Scope scratch(asMasm());
        vcvttss2si(src, dest);
        convertInt32ToFloat32(dest, scratch);
        vucomiss(scratch, src);
        j(Assembler::Parity, fail);
        j(Assembler::NotEqual, fail);
    }

    void clampIntToUint8(Register reg) {
        Label inRange;
        branchTest32(Assembler::Zero, reg, Imm32(0xffffff00), &inRange);
        {
            sarl(Imm32(31), reg);
            notl(reg);
            andl(Imm32(255), reg);
        }
        bind(&inRange);
    }

    bool maybeInlineDouble(double d, FloatRegister dest) {
        uint64_t u = mozilla::BitwiseCast<uint64_t>(d);

        // Loading zero with xor is specially optimized in hardware.
        if (u == 0) {
            zeroDouble(dest);
            return true;
        }

        // It is also possible to load several common constants using vpcmpeqw
        // to get all ones and then vpsllq and vpsrlq to get zeros at the ends,
        // as described in "13.4 Generating constants" of
        // "2. Optimizing subroutines in assembly language" by Agner Fog, and as
        // previously implemented here. However, with x86 and x64 both using
        // constant pool loads for double constants, this is probably only
        // worthwhile in cases where a load is likely to be delayed.

        return false;
    }

    bool maybeInlineFloat(float f, FloatRegister dest) {
        uint32_t u = mozilla::BitwiseCast<uint32_t>(f);

        // See comment above
        if (u == 0) {
            zeroFloat32(dest);
            return true;
        }
        return false;
    }

    bool maybeInlineInt32x4(const SimdConstant& v, const FloatRegister& dest) {
        static const SimdConstant zero = SimdConstant::CreateX4(0, 0, 0, 0);
        static const SimdConstant minusOne = SimdConstant::CreateX4(-1, -1, -1, -1);
        if (v == zero) {
            zeroInt32x4(dest);
            return true;
        }
        if (v == minusOne) {
            vpcmpeqw(dest, dest, dest);
            return true;
        }
        return false;
    }
    bool maybeInlineFloat32x4(const SimdConstant& v, const FloatRegister& dest) {
        static const SimdConstant zero = SimdConstant::CreateX4(0.f, 0.f, 0.f, 0.f);
        if (v == zero) {
            // This won't get inlined if the SimdConstant v contains -0 in any
            // lane, as operator== here does a memcmp.
            zeroFloat32x4(dest);
            return true;
        }
        return false;
    }

    void convertBoolToInt32(Register source, Register dest) {
        // Note that C++ bool is only 1 byte, so zero extend it to clear the
        // higher-order bits.
        movzbl(source, dest);
    }

    void emitSet(Assembler::Condition cond, Register dest,
                 Assembler::NaNCond ifNaN = Assembler::NaN_HandledByCond) {
        if (AllocatableGeneralRegisterSet(Registers::SingleByteRegs).has(dest)) {
            // If the register we're defining is a single byte register,
            // take advantage of the setCC instruction
            setCC(cond, dest);
            movzbl(dest, dest);

            if (ifNaN != Assembler::NaN_HandledByCond) {
                Label noNaN;
                j(Assembler::NoParity, &noNaN);
                mov(ImmWord(ifNaN == Assembler::NaN_IsTrue), dest);
                bind(&noNaN);
            }
        } else {
            Label end;
            Label ifFalse;

            if (ifNaN == Assembler::NaN_IsFalse)
                j(Assembler::Parity, &ifFalse);
            // Note a subtlety here: FLAGS is live at this point, and the
            // mov interface doesn't guarantee to preserve FLAGS. Use
            // movl instead of mov, because the movl instruction
            // preserves FLAGS.
            movl(Imm32(1), dest);
            j(cond, &end);
            if (ifNaN == Assembler::NaN_IsTrue)
                j(Assembler::Parity, &end);
            bind(&ifFalse);
            mov(ImmWord(0), dest);

            bind(&end);
        }
    }

    template <typename T1, typename T2>
    void cmp32Set(Assembler::Condition cond, T1 lhs, T2 rhs, Register dest)
    {
        cmp32(lhs, rhs);
        emitSet(cond, dest);
    }

    // Emit a JMP that can be toggled to a CMP. See ToggleToJmp(), ToggleToCmp().
    CodeOffset toggledJump(Label* label) {
        CodeOffset offset(size());
        jump(label);
        return offset;
    }

    template <typename T>
    void computeEffectiveAddress(const T& address, Register dest) {
        lea(Operand(address), dest);
    }

    void checkStackAlignment() {
        // Exists for ARM compatibility.
    }

    CodeOffset labelForPatch() {
        return CodeOffset(size());
    }

    void abiret() {
        ret();
    }

    template<typename T>
    void compareExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem, Register oldval, Register newval,
                                        Register temp, AnyRegister output);

    template<typename T>
    void atomicExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem, Register value,
                                       Register temp, AnyRegister output);

  protected:
    bool buildOOLFakeExitFrame(void* fakeReturnAddr);
};

template <> inline void
MacroAssemblerX86Shared::loadAlignedVector<int32_t>(const Address& src, FloatRegister dest) {
    loadAlignedInt32x4(src, dest);
}
template <> inline void
MacroAssemblerX86Shared::loadAlignedVector<float>(const Address& src, FloatRegister dest) {
    loadAlignedFloat32x4(src, dest);
}

template <> inline void
MacroAssemblerX86Shared::storeAlignedVector<int32_t>(FloatRegister src, const Address& dest) {
    storeAlignedInt32x4(src, dest);
}
template <> inline void
MacroAssemblerX86Shared::storeAlignedVector<float>(FloatRegister src, const Address& dest) {
    storeAlignedFloat32x4(src, dest);
}

template <> inline void
MacroAssemblerX86Shared::loadScalar<int32_t>(const Operand& src, Register dest) {
    load32(src, dest);
}
template <> inline void
MacroAssemblerX86Shared::loadScalar<float>(const Operand& src, FloatRegister dest) {
    loadFloat32(src, dest);
}

template <> inline void
MacroAssemblerX86Shared::storeScalar<int32_t>(Register src, const Address& dest) {
    store32(src, dest);
}
template <> inline void
MacroAssemblerX86Shared::storeScalar<float>(FloatRegister src, const Address& dest) {
    storeFloat32(src, dest);
}

} // namespace jit
} // namespace js

#undef CHECK_BYTEREG
#undef CHECK_BYTEREGS

#endif /* jit_x86_shared_MacroAssembler_x86_shared_h */
