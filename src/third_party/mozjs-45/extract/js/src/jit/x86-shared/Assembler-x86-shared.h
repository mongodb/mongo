/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_Assembler_x86_shared_h
#define jit_x86_shared_Assembler_x86_shared_h

#include <cstddef>

#include "jit/shared/Assembler-shared.h"

#if defined(JS_CODEGEN_X86)
# include "jit/x86/BaseAssembler-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/BaseAssembler-x64.h"
#else
# error "Unknown architecture!"
#endif

namespace js {
namespace jit {

struct ScratchFloat32Scope : public AutoFloatRegisterScope
{
    explicit ScratchFloat32Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchFloat32Reg)
    { }
};

struct ScratchDoubleScope : public AutoFloatRegisterScope
{
    explicit ScratchDoubleScope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchDoubleReg)
    { }
};

struct ScratchSimd128Scope : public AutoFloatRegisterScope
{
    explicit ScratchSimd128Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchSimd128Reg)
    { }
};

class Operand
{
  public:
    enum Kind {
        REG,
        MEM_REG_DISP,
        FPREG,
        MEM_SCALE,
        MEM_ADDRESS32
    };

  private:
    Kind kind_ : 4;
    // Used as a Register::Encoding and a FloatRegister::Encoding.
    uint32_t base_ : 5;
    Scale scale_ : 3;
    Register::Encoding index_ : 5;
    int32_t disp_;

  public:
    explicit Operand(Register reg)
      : kind_(REG),
        base_(reg.encoding()),
        scale_(TimesOne),
        index_(Registers::Invalid),
        disp_(0)
    { }
    explicit Operand(FloatRegister reg)
      : kind_(FPREG),
        base_(reg.encoding()),
        scale_(TimesOne),
        index_(Registers::Invalid),
        disp_(0)
    { }
    explicit Operand(const Address& address)
      : kind_(MEM_REG_DISP),
        base_(address.base.encoding()),
        scale_(TimesOne),
        index_(Registers::Invalid),
        disp_(address.offset)
    { }
    explicit Operand(const BaseIndex& address)
      : kind_(MEM_SCALE),
        base_(address.base.encoding()),
        scale_(address.scale),
        index_(address.index.encoding()),
        disp_(address.offset)
    { }
    Operand(Register base, Register index, Scale scale, int32_t disp = 0)
      : kind_(MEM_SCALE),
        base_(base.encoding()),
        scale_(scale),
        index_(index.encoding()),
        disp_(disp)
    { }
    Operand(Register reg, int32_t disp)
      : kind_(MEM_REG_DISP),
        base_(reg.encoding()),
        scale_(TimesOne),
        index_(Registers::Invalid),
        disp_(disp)
    { }
    explicit Operand(AbsoluteAddress address)
      : kind_(MEM_ADDRESS32),
        base_(Registers::Invalid),
        scale_(TimesOne),
        index_(Registers::Invalid),
        disp_(X86Encoding::AddressImmediate(address.addr))
    { }
    explicit Operand(PatchedAbsoluteAddress address)
      : kind_(MEM_ADDRESS32),
        base_(Registers::Invalid),
        scale_(TimesOne),
        index_(Registers::Invalid),
        disp_(X86Encoding::AddressImmediate(address.addr))
    { }

    Address toAddress() const {
        MOZ_ASSERT(kind() == MEM_REG_DISP);
        return Address(Register::FromCode(base()), disp());
    }

    BaseIndex toBaseIndex() const {
        MOZ_ASSERT(kind() == MEM_SCALE);
        return BaseIndex(Register::FromCode(base()), Register::FromCode(index()), scale(), disp());
    }

    Kind kind() const {
        return kind_;
    }
    Register::Encoding reg() const {
        MOZ_ASSERT(kind() == REG);
        return Register::Encoding(base_);
    }
    Register::Encoding base() const {
        MOZ_ASSERT(kind() == MEM_REG_DISP || kind() == MEM_SCALE);
        return Register::Encoding(base_);
    }
    Register::Encoding index() const {
        MOZ_ASSERT(kind() == MEM_SCALE);
        return index_;
    }
    Scale scale() const {
        MOZ_ASSERT(kind() == MEM_SCALE);
        return scale_;
    }
    FloatRegister::Encoding fpu() const {
        MOZ_ASSERT(kind() == FPREG);
        return FloatRegister::Encoding(base_);
    }
    int32_t disp() const {
        MOZ_ASSERT(kind() == MEM_REG_DISP || kind() == MEM_SCALE);
        return disp_;
    }
    void* address() const {
        MOZ_ASSERT(kind() == MEM_ADDRESS32);
        return reinterpret_cast<void*>(disp_);
    }

    bool containsReg(Register r) const {
        switch (kind()) {
          case REG:          return r.encoding() == reg();
          case MEM_REG_DISP: return r.encoding() == base();
          case MEM_SCALE:    return r.encoding() == base() || r.encoding() == index();
          default: MOZ_CRASH("Unexpected Operand kind");
        }
        return false;
    }
};

class CPUInfo
{
  public:
    // As the SSE's were introduced in order, the presence of a later SSE implies
    // the presence of an earlier SSE. For example, SSE4_2 support implies SSE2 support.
    enum SSEVersion {
        UnknownSSE = 0,
        NoSSE = 1,
        SSE = 2,
        SSE2 = 3,
        SSE3 = 4,
        SSSE3 = 5,
        SSE4_1 = 6,
        SSE4_2 = 7
    };

    static SSEVersion GetSSEVersion() {
        if (maxSSEVersion == UnknownSSE)
            SetSSEVersion();

        MOZ_ASSERT(maxSSEVersion != UnknownSSE);
        MOZ_ASSERT_IF(maxEnabledSSEVersion != UnknownSSE, maxSSEVersion <= maxEnabledSSEVersion);
        return maxSSEVersion;
    }

    static bool IsAVXPresent() {
        if (MOZ_UNLIKELY(maxSSEVersion == UnknownSSE))
            SetSSEVersion();

        MOZ_ASSERT_IF(!avxEnabled, !avxPresent);
        return avxPresent;
    }

  private:
    static SSEVersion maxSSEVersion;
    static SSEVersion maxEnabledSSEVersion;
    static bool avxPresent;
    static bool avxEnabled;

    static void SetSSEVersion();

  public:
    static bool IsSSE2Present() {
#ifdef JS_CODEGEN_X64
        return true;
#else
        return GetSSEVersion() >= SSE2;
#endif
    }
    static bool IsSSE3Present()  { return GetSSEVersion() >= SSE3; }
    static bool IsSSSE3Present() { return GetSSEVersion() >= SSSE3; }
    static bool IsSSE41Present() { return GetSSEVersion() >= SSE4_1; }
    static bool IsSSE42Present() { return GetSSEVersion() >= SSE4_2; }

#ifdef JS_CODEGEN_X86
    static void SetFloatingPointDisabled() { maxEnabledSSEVersion = NoSSE; avxEnabled = false; }
#endif
    static void SetSSE3Disabled() { maxEnabledSSEVersion = SSE2; avxEnabled = false; }
    static void SetSSE4Disabled() { maxEnabledSSEVersion = SSSE3; avxEnabled = false; }
    static void SetAVXEnabled() { avxEnabled = true; }
};

class AssemblerX86Shared : public AssemblerShared
{
  protected:
    struct RelativePatch {
        int32_t offset;
        void* target;
        Relocation::Kind kind;

        RelativePatch(int32_t offset, void* target, Relocation::Kind kind)
          : offset(offset),
            target(target),
            kind(kind)
        { }
    };

    Vector<RelativePatch, 8, SystemAllocPolicy> jumps_;
    CompactBufferWriter jumpRelocations_;
    CompactBufferWriter dataRelocations_;
    CompactBufferWriter preBarriers_;

    void writeDataRelocation(ImmGCPtr ptr) {
        if (ptr.value) {
            if (gc::IsInsideNursery(ptr.value))
                embedsNurseryPointers_ = true;
            dataRelocations_.writeUnsigned(masm.currentOffset());
        }
    }
    void writePrebarrierOffset(CodeOffset label) {
        preBarriers_.writeUnsigned(label.offset());
    }

  protected:
    X86Encoding::BaseAssemblerSpecific masm;

    typedef X86Encoding::JmpSrc JmpSrc;
    typedef X86Encoding::JmpDst JmpDst;

  public:
    AssemblerX86Shared()
    {
        if (!HasAVX())
            masm.disableVEX();
    }

    enum Condition {
        Equal = X86Encoding::ConditionE,
        NotEqual = X86Encoding::ConditionNE,
        Above = X86Encoding::ConditionA,
        AboveOrEqual = X86Encoding::ConditionAE,
        Below = X86Encoding::ConditionB,
        BelowOrEqual = X86Encoding::ConditionBE,
        GreaterThan = X86Encoding::ConditionG,
        GreaterThanOrEqual = X86Encoding::ConditionGE,
        LessThan = X86Encoding::ConditionL,
        LessThanOrEqual = X86Encoding::ConditionLE,
        Overflow = X86Encoding::ConditionO,
        Signed = X86Encoding::ConditionS,
        NotSigned = X86Encoding::ConditionNS,
        Zero = X86Encoding::ConditionE,
        NonZero = X86Encoding::ConditionNE,
        Parity = X86Encoding::ConditionP,
        NoParity = X86Encoding::ConditionNP
    };

    // If this bit is set, the vucomisd operands have to be inverted.
    static const int DoubleConditionBitInvert = 0x10;

    // Bit set when a DoubleCondition does not map to a single x86 condition.
    // The macro assembler has to special-case these conditions.
    static const int DoubleConditionBitSpecial = 0x20;
    static const int DoubleConditionBits = DoubleConditionBitInvert | DoubleConditionBitSpecial;

    enum DoubleCondition {
        // These conditions will only evaluate to true if the comparison is ordered - i.e. neither operand is NaN.
        DoubleOrdered = NoParity,
        DoubleEqual = Equal | DoubleConditionBitSpecial,
        DoubleNotEqual = NotEqual,
        DoubleGreaterThan = Above,
        DoubleGreaterThanOrEqual = AboveOrEqual,
        DoubleLessThan = Above | DoubleConditionBitInvert,
        DoubleLessThanOrEqual = AboveOrEqual | DoubleConditionBitInvert,
        // If either operand is NaN, these conditions always evaluate to true.
        DoubleUnordered = Parity,
        DoubleEqualOrUnordered = Equal,
        DoubleNotEqualOrUnordered = NotEqual | DoubleConditionBitSpecial,
        DoubleGreaterThanOrUnordered = Below | DoubleConditionBitInvert,
        DoubleGreaterThanOrEqualOrUnordered = BelowOrEqual | DoubleConditionBitInvert,
        DoubleLessThanOrUnordered = Below,
        DoubleLessThanOrEqualOrUnordered = BelowOrEqual
    };

    enum NaNCond {
        NaN_HandledByCond,
        NaN_IsTrue,
        NaN_IsFalse
    };

    // If the primary condition returned by ConditionFromDoubleCondition doesn't
    // handle NaNs properly, return NaN_IsFalse if the comparison should be
    // overridden to return false on NaN, NaN_IsTrue if it should be overridden
    // to return true on NaN, or NaN_HandledByCond if no secondary check is
    // needed.
    static inline NaNCond NaNCondFromDoubleCondition(DoubleCondition cond) {
        switch (cond) {
          case DoubleOrdered:
          case DoubleNotEqual:
          case DoubleGreaterThan:
          case DoubleGreaterThanOrEqual:
          case DoubleLessThan:
          case DoubleLessThanOrEqual:
          case DoubleUnordered:
          case DoubleEqualOrUnordered:
          case DoubleGreaterThanOrUnordered:
          case DoubleGreaterThanOrEqualOrUnordered:
          case DoubleLessThanOrUnordered:
          case DoubleLessThanOrEqualOrUnordered:
            return NaN_HandledByCond;
          case DoubleEqual:
            return NaN_IsFalse;
          case DoubleNotEqualOrUnordered:
            return NaN_IsTrue;
        }

        MOZ_CRASH("Unknown double condition");
    }

    static void StaticAsserts() {
        // DoubleConditionBits should not interfere with x86 condition codes.
        JS_STATIC_ASSERT(!((Equal | NotEqual | Above | AboveOrEqual | Below |
                            BelowOrEqual | Parity | NoParity) & DoubleConditionBits));
    }

    static Condition InvertCondition(Condition cond);

    // Return the primary condition to test. Some primary conditions may not
    // handle NaNs properly and may therefore require a secondary condition.
    // Use NaNCondFromDoubleCondition to determine what else is needed.
    static inline Condition ConditionFromDoubleCondition(DoubleCondition cond) {
        return static_cast<Condition>(cond & ~DoubleConditionBits);
    }

    static void TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);

    // MacroAssemblers hold onto gcthings, so they are traced by the GC.
    void trace(JSTracer* trc);

    bool oom() const {
        return AssemblerShared::oom() ||
               masm.oom() ||
               jumpRelocations_.oom() ||
               dataRelocations_.oom() ||
               preBarriers_.oom();
    }

    void setPrinter(Sprinter* sp) {
        masm.setPrinter(sp);
    }

    static const Register getStackPointer() {
        return StackPointer;
    }

    void executableCopy(void* buffer);
    bool asmMergeWith(const AssemblerX86Shared& other) {
        MOZ_ASSERT(other.jumps_.length() == 0);
        if (!AssemblerShared::asmMergeWith(masm.size(), other))
            return false;
        return masm.appendBuffer(other.masm);
    }
    void processCodeLabels(uint8_t* rawCode);
    void copyJumpRelocationTable(uint8_t* dest);
    void copyDataRelocationTable(uint8_t* dest);
    void copyPreBarrierTable(uint8_t* dest);

    // Size of the instruction stream, in bytes.
    size_t size() const {
        return masm.size();
    }
    // Size of the jump relocation table, in bytes.
    size_t jumpRelocationTableBytes() const {
        return jumpRelocations_.length();
    }
    size_t dataRelocationTableBytes() const {
        return dataRelocations_.length();
    }
    size_t preBarrierTableBytes() const {
        return preBarriers_.length();
    }
    // Size of the data table, in bytes.
    size_t bytesNeeded() const {
        return size() +
               jumpRelocationTableBytes() +
               dataRelocationTableBytes() +
               preBarrierTableBytes();
    }

  public:
    void haltingAlign(int alignment) {
        masm.haltingAlign(alignment);
    }
    void nopAlign(int alignment) {
        masm.nopAlign(alignment);
    }
    void writeCodePointer(CodeOffset* label) {
        // A CodeOffset only has one use, bake in the "end of list" value.
        masm.jumpTablePointer(LabelBase::INVALID_OFFSET);
        label->bind(masm.size());
    }
    void movl(Imm32 imm32, Register dest) {
        masm.movl_i32r(imm32.value, dest.encoding());
    }
    void movl(Register src, Register dest) {
        masm.movl_rr(src.encoding(), dest.encoding());
    }
    void movl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.movl_rr(src.reg(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.movl_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.movl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movl_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movl(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.movl_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.movl_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.movl_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movl_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movl(Imm32 imm32, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.movl_i32r(imm32.value, dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.movl_i32m(imm32.value, dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.movl_i32m(imm32.value, dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    void xchgl(Register src, Register dest) {
        masm.xchgl_rr(src.encoding(), dest.encoding());
    }

    // Eventually vmovapd should be overloaded to support loads and
    // stores too.
    void vmovapd(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovapd_rr(src.encoding(), dest.encoding());
    }

    void vmovaps(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovaps_rr(src.encoding(), dest.encoding());
    }
    void vmovaps(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovaps_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vmovaps_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          case Operand::FPREG:
            masm.vmovaps_rr(src.fpu(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovaps(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovaps_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovaps_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovups(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovups_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vmovups_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovups(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovups_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovups_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    // vmovsd is only provided in load/store form since the
    // register-to-register form has different semantics (it doesn't clobber
    // the whole output register) and isn't needed currently.
    void vmovsd(const Address& src, FloatRegister dest) {
        masm.vmovsd_mr(src.offset, src.base.encoding(), dest.encoding());
    }
    void vmovsd(const BaseIndex& src, FloatRegister dest) {
        masm.vmovsd_mr(src.offset, src.base.encoding(), src.index.encoding(), src.scale, dest.encoding());
    }
    void vmovsd(FloatRegister src, const Address& dest) {
        masm.vmovsd_rm(src.encoding(), dest.offset, dest.base.encoding());
    }
    void vmovsd(FloatRegister src, const BaseIndex& dest) {
        masm.vmovsd_rm(src.encoding(), dest.offset, dest.base.encoding(), dest.index.encoding(), dest.scale);
    }
    // Although vmovss is not only provided in load/store form (for the same
    // reasons as vmovsd above), the register to register form should be only
    // used in contexts where we care about not clearing the higher lanes of
    // the FloatRegister.
    void vmovss(const Address& src, FloatRegister dest) {
        masm.vmovss_mr(src.offset, src.base.encoding(), dest.encoding());
    }
    void vmovss(const BaseIndex& src, FloatRegister dest) {
        masm.vmovss_mr(src.offset, src.base.encoding(), src.index.encoding(), src.scale, dest.encoding());
    }
    void vmovss(FloatRegister src, const Address& dest) {
        masm.vmovss_rm(src.encoding(), dest.offset, dest.base.encoding());
    }
    void vmovss(FloatRegister src, const BaseIndex& dest) {
        masm.vmovss_rm(src.encoding(), dest.offset, dest.base.encoding(), dest.index.encoding(), dest.scale);
    }
    void vmovss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        masm.vmovss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vmovdqu(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovdqu_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vmovdqu_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovdqu(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovdqu_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovdqu_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovdqa(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vmovdqa_rr(src.fpu(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmovdqa_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vmovdqa_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovdqa(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovdqa_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovdqa_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovdqa(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqa_rr(src.encoding(), dest.encoding());
    }
    void vcvtss2sd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtss2sd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vcvtsd2ss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtsd2ss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void movzbl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movzbl_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.movzbl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movsbl(Register src, Register dest) {
        masm.movsbl_rr(src.encoding(), dest.encoding());
    }
    void movsbl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movsbl_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.movsbl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movb(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movb_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.movb_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movb(Imm32 src, Register dest) {
        masm.movb_ir(src.value & 255, dest.encoding());
    }
    void movb(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movb_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.movb_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movb(Imm32 src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movb_im(src.value, dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.movb_im(src.value, dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movzwl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.movzwl_rr(src.reg(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.movzwl_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.movzwl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movzwl(Register src, Register dest) {
        masm.movzwl_rr(src.encoding(), dest.encoding());
    }
    void movw(const Operand& src, Register dest) {
        masm.prefix_16_for_32();
        movl(src, dest);
    }
    void movw(Imm32 src, Register dest) {
        masm.prefix_16_for_32();
        movl(src, dest);
    }
    void movw(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movw_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.movw_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movw(Imm32 src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movw_im(src.value, dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.movw_im(src.value, dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movswl(Register src, Register dest) {
        masm.movswl_rr(src.encoding(), dest.encoding());
    }
    void movswl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movswl_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.movswl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void leal(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.leal_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.leal_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

  protected:
    void jSrc(Condition cond, Label* label) {
        if (label->bound()) {
            // The jump can be immediately encoded to the correct destination.
            masm.jCC_i(static_cast<X86Encoding::Condition>(cond), JmpDst(label->offset()));
        } else {
            // Thread the jump list through the unpatched jump targets.
            JmpSrc j = masm.jCC(static_cast<X86Encoding::Condition>(cond));
            JmpSrc prev = JmpSrc(label->use(j.offset()));
            masm.setNextJump(j, prev);
        }
    }
    void jmpSrc(Label* label) {
        if (label->bound()) {
            // The jump can be immediately encoded to the correct destination.
            masm.jmp_i(JmpDst(label->offset()));
        } else {
            // Thread the jump list through the unpatched jump targets.
            JmpSrc j = masm.jmp();
            JmpSrc prev = JmpSrc(label->use(j.offset()));
            masm.setNextJump(j, prev);
        }
    }

    // Comparison of EAX against the address given by a Label.
    JmpSrc cmpSrc(Label* label) {
        JmpSrc j = masm.cmp_eax();
        if (label->bound()) {
            // The jump can be immediately patched to the correct destination.
            masm.linkJump(j, JmpDst(label->offset()));
        } else {
            // Thread the jump list through the unpatched jump targets.
            JmpSrc prev = JmpSrc(label->use(j.offset()));
            masm.setNextJump(j, prev);
        }
        return j;
    }

    JmpSrc jSrc(Condition cond, RepatchLabel* label) {
        JmpSrc j = masm.jCC(static_cast<X86Encoding::Condition>(cond));
        if (label->bound()) {
            // The jump can be immediately patched to the correct destination.
            masm.linkJump(j, JmpDst(label->offset()));
        } else {
            label->use(j.offset());
        }
        return j;
    }
    JmpSrc jmpSrc(RepatchLabel* label) {
        JmpSrc j = masm.jmp();
        if (label->bound()) {
            // The jump can be immediately patched to the correct destination.
            masm.linkJump(j, JmpDst(label->offset()));
        } else {
            // Thread the jump list through the unpatched jump targets.
            label->use(j.offset());
        }
        return j;
    }

  public:
    void nop() { masm.nop(); }
    void twoByteNop() { masm.twoByteNop(); }
    void j(Condition cond, Label* label) { jSrc(cond, label); }
    void jmp(Label* label) { jmpSrc(label); }
    void j(Condition cond, RepatchLabel* label) { jSrc(cond, label); }
    void jmp(RepatchLabel* label) { jmpSrc(label); }

    void jmp(const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.jmp_m(op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.jmp_m(op.disp(), op.base(), op.index(), op.scale());
            break;
          case Operand::REG:
            masm.jmp_r(op.reg());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void cmpEAX(Label* label) { cmpSrc(label); }
    void bind(Label* label) {
        JmpDst dst(masm.label());
        if (label->used()) {
            bool more;
            JmpSrc jmp(label->offset());
            do {
                JmpSrc next;
                more = masm.nextJump(jmp, &next);
                masm.linkJump(jmp, dst);
                jmp = next;
            } while (more);
        }
        label->bind(dst.offset());
    }
    void bind(RepatchLabel* label) {
        JmpDst dst(masm.label());
        if (label->used()) {
            JmpSrc jmp(label->offset());
            masm.linkJump(jmp, dst);
        }
        label->bind(dst.offset());
    }
    void use(CodeOffset* label) {
        label->bind(currentOffset());
    }
    uint32_t currentOffset() {
        return masm.label().offset();
    }

    // Re-routes pending jumps to a new label.
    void retargetWithOffset(size_t baseOffset, const LabelBase* label, LabelBase* target) {
        if (!label->used())
            return;
        bool more;
        JmpSrc jmp(label->offset() + baseOffset);
        do {
            JmpSrc next;
            more = masm.nextJump(jmp, &next);
            if (target->bound()) {
                // The jump can be immediately patched to the correct destination.
                masm.linkJump(jmp, JmpDst(target->offset()));
            } else {
                // Thread the jump list through the unpatched jump targets.
                JmpSrc prev(target->use(jmp.offset()));
                masm.setNextJump(jmp, prev);
            }
            jmp = JmpSrc(next.offset() + baseOffset);
        } while (more);
    }
    void retarget(Label* label, Label* target) {
        retargetWithOffset(0, label, target);
        label->reset();
    }

    static void Bind(uint8_t* raw, CodeOffset* label, const void* address) {
        if (label->bound()) {
            intptr_t offset = label->offset();
            X86Encoding::SetPointer(raw + offset, address);
        }
    }

    // See Bind and X86Encoding::setPointer.
    size_t labelToPatchOffset(CodeOffset label) {
        return label.offset() - sizeof(void*);
    }

    void ret() {
        masm.ret();
    }
    void retn(Imm32 n) {
        // Remove the size of the return address which is included in the frame.
        masm.ret_i(n.value - sizeof(void*));
    }
    CodeOffset call(Label* label) {
        if (label->bound()) {
            masm.linkJump(masm.call(), JmpDst(label->offset()));
        } else {
            JmpSrc j = masm.call();
            JmpSrc prev = JmpSrc(label->use(j.offset()));
            masm.setNextJump(j, prev);
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset call(Register reg) {
        masm.call_r(reg.encoding());
        return CodeOffset(masm.currentOffset());
    }
    void call(const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.call_r(op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.call_m(op.disp(), op.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    CodeOffset callWithPatch() {
        return CodeOffset(masm.call().offset());
    }
    void patchCall(uint32_t callerOffset, uint32_t calleeOffset) {
        unsigned char* code = masm.data();
        X86Encoding::SetRel32(code + callerOffset, code + calleeOffset);
    }

    void breakpoint() {
        masm.int3();
    }

    static bool HasSSE2() { return CPUInfo::IsSSE2Present(); }
    static bool HasSSE3() { return CPUInfo::IsSSE3Present(); }
    static bool HasSSE41() { return CPUInfo::IsSSE41Present(); }
    static bool SupportsFloatingPoint() { return CPUInfo::IsSSE2Present(); }
    static bool SupportsSimd() { return CPUInfo::IsSSE2Present(); }
    static bool HasAVX() { return CPUInfo::IsAVXPresent(); }

    void cmpl(Register rhs, Register lhs) {
        masm.cmpl_rr(rhs.encoding(), lhs.encoding());
    }
    void cmpl(const Operand& rhs, Register lhs) {
        switch (rhs.kind()) {
          case Operand::REG:
            masm.cmpl_rr(rhs.reg(), lhs.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.cmpl_mr(rhs.disp(), rhs.base(), lhs.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.cmpl_mr(rhs.address(), lhs.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void cmpl(Register rhs, const Operand& lhs) {
        switch (lhs.kind()) {
          case Operand::REG:
            masm.cmpl_rr(rhs.encoding(), lhs.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.cmpl_rm(rhs.encoding(), lhs.disp(), lhs.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.cmpl_rm(rhs.encoding(), lhs.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void cmpl(Imm32 rhs, Register lhs) {
        masm.cmpl_ir(rhs.value, lhs.encoding());
    }
    void cmpl(Imm32 rhs, const Operand& lhs) {
        switch (lhs.kind()) {
          case Operand::REG:
            masm.cmpl_ir(rhs.value, lhs.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.cmpl_im(rhs.value, lhs.disp(), lhs.base());
            break;
          case Operand::MEM_SCALE:
            masm.cmpl_im(rhs.value, lhs.disp(), lhs.base(), lhs.index(), lhs.scale());
            break;
          case Operand::MEM_ADDRESS32:
            masm.cmpl_im(rhs.value, lhs.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    CodeOffset cmplWithPatch(Imm32 rhs, Register lhs) {
        masm.cmpl_i32r(rhs.value, lhs.encoding());
        return CodeOffset(masm.currentOffset());
    }
    void cmpw(Register rhs, Register lhs) {
        masm.cmpw_rr(rhs.encoding(), lhs.encoding());
    }
    void setCC(Condition cond, Register r) {
        masm.setCC_r(static_cast<X86Encoding::Condition>(cond), r.encoding());
    }
    void testb(Register rhs, Register lhs) {
        MOZ_ASSERT(AllocatableGeneralRegisterSet(Registers::SingleByteRegs).has(rhs));
        MOZ_ASSERT(AllocatableGeneralRegisterSet(Registers::SingleByteRegs).has(lhs));
        masm.testb_rr(rhs.encoding(), lhs.encoding());
    }
    void testw(Register rhs, Register lhs) {
        masm.testw_rr(lhs.encoding(), rhs.encoding());
    }
    void testl(Register rhs, Register lhs) {
        masm.testl_rr(lhs.encoding(), rhs.encoding());
    }
    void testl(Imm32 rhs, Register lhs) {
        masm.testl_ir(rhs.value, lhs.encoding());
    }
    void testl(Imm32 rhs, const Operand& lhs) {
        switch (lhs.kind()) {
          case Operand::REG:
            masm.testl_ir(rhs.value, lhs.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.testl_i32m(rhs.value, lhs.disp(), lhs.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.testl_i32m(rhs.value, lhs.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }

    void addl(Imm32 imm, Register dest) {
        masm.addl_ir(imm.value, dest.encoding());
    }
    CodeOffset addlWithPatch(Imm32 imm, Register dest) {
        masm.addl_i32r(imm.value, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    void addl(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.addl_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.addl_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.addl_im(imm.value, op.address());
            break;
          case Operand::MEM_SCALE:
            masm.addl_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void addw(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.addw_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.addw_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.addw_im(imm.value, op.address());
            break;
          case Operand::MEM_SCALE:
            masm.addw_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void subl(Imm32 imm, Register dest) {
        masm.subl_ir(imm.value, dest.encoding());
    }
    void subl(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.subl_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.subl_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.subl_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void subw(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.subw_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.subw_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.subw_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void addl(Register src, Register dest) {
        masm.addl_rr(src.encoding(), dest.encoding());
    }
    void addl(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.addl_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.addl_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.addl_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void addw(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.addw_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.addw_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.addw_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void subl(Register src, Register dest) {
        masm.subl_rr(src.encoding(), dest.encoding());
    }
    void subl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.subl_rr(src.reg(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.subl_mr(src.disp(), src.base(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void subl(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.subl_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.subl_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.subl_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void subw(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.subw_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.subw_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.subw_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void orl(Register reg, Register dest) {
        masm.orl_rr(reg.encoding(), dest.encoding());
    }
    void orl(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.orl_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.orl_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.orl_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void orw(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.orw_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.orw_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.orw_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void orl(Imm32 imm, Register reg) {
        masm.orl_ir(imm.value, reg.encoding());
    }
    void orl(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.orl_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.orl_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.orl_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void orw(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.orw_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.orw_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.orw_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void xorl(Register src, Register dest) {
        masm.xorl_rr(src.encoding(), dest.encoding());
    }
    void xorl(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.xorl_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.xorl_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.xorl_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void xorw(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.xorw_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.xorw_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.xorw_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void xorl(Imm32 imm, Register reg) {
        masm.xorl_ir(imm.value, reg.encoding());
    }
    void xorl(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.xorl_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.xorl_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.xorl_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void xorw(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.xorw_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.xorw_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.xorw_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void andl(Register src, Register dest) {
        masm.andl_rr(src.encoding(), dest.encoding());
    }
    void andl(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.andl_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.andl_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.andl_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void andw(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.andw_rr(src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.andw_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.andw_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void andl(Imm32 imm, Register dest) {
        masm.andl_ir(imm.value, dest.encoding());
    }
    void andl(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.andl_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.andl_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.andl_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void andw(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.andw_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.andw_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.andw_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void addl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.addl_rr(src.reg(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.addl_mr(src.disp(), src.base(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void orl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.orl_rr(src.reg(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.orl_mr(src.disp(), src.base(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void xorl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.xorl_rr(src.reg(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.xorl_mr(src.disp(), src.base(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void andl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.andl_rr(src.reg(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.andl_mr(src.disp(), src.base(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void bsr(const Register& src, const Register& dest) {
        masm.bsr_rr(src.encoding(), dest.encoding());
    }
    void imull(Register multiplier) {
        masm.imull_r(multiplier.encoding());
    }
    void umull(Register multiplier) {
        masm.mull_r(multiplier.encoding());
    }
    void imull(Imm32 imm, Register dest) {
        masm.imull_ir(imm.value, dest.encoding(), dest.encoding());
    }
    void imull(Register src, Register dest) {
        masm.imull_rr(src.encoding(), dest.encoding());
    }
    void imull(Imm32 imm, Register src, Register dest) {
        masm.imull_ir(imm.value, src.encoding(), dest.encoding());
    }
    void imull(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.imull_rr(src.reg(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.imull_mr(src.disp(), src.base(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void negl(const Operand& src) {
        switch (src.kind()) {
          case Operand::REG:
            masm.negl_r(src.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.negl_m(src.disp(), src.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void negl(Register reg) {
        masm.negl_r(reg.encoding());
    }
    void notl(const Operand& src) {
        switch (src.kind()) {
          case Operand::REG:
            masm.notl_r(src.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.notl_m(src.disp(), src.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void notl(Register reg) {
        masm.notl_r(reg.encoding());
    }
    void shrl(const Imm32 imm, Register dest) {
        masm.shrl_ir(imm.value, dest.encoding());
    }
    void shll(const Imm32 imm, Register dest) {
        masm.shll_ir(imm.value, dest.encoding());
    }
    void sarl(const Imm32 imm, Register dest) {
        masm.sarl_ir(imm.value, dest.encoding());
    }
    void shrl_cl(Register dest) {
        masm.shrl_CLr(dest.encoding());
    }
    void shll_cl(Register dest) {
        masm.shll_CLr(dest.encoding());
    }
    void sarl_cl(Register dest) {
        masm.sarl_CLr(dest.encoding());
    }

    void incl(const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.incl_m32(op.disp(), op.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void lock_incl(const Operand& op) {
        masm.prefix_lock();
        incl(op);
    }

    void decl(const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.decl_m32(op.disp(), op.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void lock_decl(const Operand& op) {
        masm.prefix_lock();
        decl(op);
    }

    void addb(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.addb_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.addb_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }
    void addb(Register src, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.addb_rm(src.encoding(), op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.addb_rm(src.encoding(), op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }

    void subb(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.subb_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.subb_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }
    void subb(Register src, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.subb_rm(src.encoding(), op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.subb_rm(src.encoding(), op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }

    void andb(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.andb_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.andb_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }
    void andb(Register src, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.andb_rm(src.encoding(), op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.andb_rm(src.encoding(), op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }

    void orb(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.orb_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.orb_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }
    void orb(Register src, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.orb_rm(src.encoding(), op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.orb_rm(src.encoding(), op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }

    void xorb(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.xorb_im(imm.value, op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.xorb_im(imm.value, op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }
    void xorb(Register src, const Operand& op) {
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            masm.xorb_rm(src.encoding(), op.disp(), op.base());
            break;
          case Operand::MEM_SCALE:
            masm.xorb_rm(src.encoding(), op.disp(), op.base(), op.index(), op.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
            break;
        }
    }

    template<typename T>
    void lock_addb(T src, const Operand& op) {
        masm.prefix_lock();
        addb(src, op);
    }
    template<typename T>
    void lock_subb(T src, const Operand& op) {
        masm.prefix_lock();
        subb(src, op);
    }
    template<typename T>
    void lock_andb(T src, const Operand& op) {
        masm.prefix_lock();
        andb(src, op);
    }
    template<typename T>
    void lock_orb(T src, const Operand& op) {
        masm.prefix_lock();
        orb(src, op);
    }
    template<typename T>
    void lock_xorb(T src, const Operand& op) {
        masm.prefix_lock();
        xorb(src, op);
    }

    template<typename T>
    void lock_addw(T src, const Operand& op) {
        masm.prefix_lock();
        addw(src, op);
    }
    template<typename T>
    void lock_subw(T src, const Operand& op) {
        masm.prefix_lock();
        subw(src, op);
    }
    template<typename T>
    void lock_andw(T src, const Operand& op) {
        masm.prefix_lock();
        andw(src, op);
    }
    template<typename T>
    void lock_orw(T src, const Operand& op) {
        masm.prefix_lock();
        orw(src, op);
    }
    template<typename T>
    void lock_xorw(T src, const Operand& op) {
        masm.prefix_lock();
        xorw(src, op);
    }

    // Note, lock_addl(imm, op) is used for a memory barrier on non-SSE2 systems,
    // among other things.  Do not optimize, replace by XADDL, or similar.
    template<typename T>
    void lock_addl(T src, const Operand& op) {
        masm.prefix_lock();
        addl(src, op);
    }
    template<typename T>
    void lock_subl(T src, const Operand& op) {
        masm.prefix_lock();
        subl(src, op);
    }
    template<typename T>
    void lock_andl(T src, const Operand& op) {
        masm.prefix_lock();
        andl(src, op);
    }
    template<typename T>
    void lock_orl(T src, const Operand& op) {
        masm.prefix_lock();
        orl(src, op);
    }
    template<typename T>
    void lock_xorl(T src, const Operand& op) {
        masm.prefix_lock();
        xorl(src, op);
    }

    void lock_cmpxchgb(Register src, const Operand& mem) {
        masm.prefix_lock();
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.cmpxchgb(src.encoding(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.cmpxchgb(src.encoding(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void lock_cmpxchgw(Register src, const Operand& mem) {
        masm.prefix_lock();
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.cmpxchgw(src.encoding(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.cmpxchgw(src.encoding(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void lock_cmpxchgl(Register src, const Operand& mem) {
        masm.prefix_lock();
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.cmpxchgl(src.encoding(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.cmpxchgl(src.encoding(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    void xchgb(Register src, const Operand& mem) {
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.xchgb_rm(src.encoding(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.xchgb_rm(src.encoding(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void xchgw(Register src, const Operand& mem) {
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.xchgw_rm(src.encoding(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.xchgw_rm(src.encoding(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void xchgl(Register src, const Operand& mem) {
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.xchgl_rm(src.encoding(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.xchgl_rm(src.encoding(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    void lock_xaddb(Register srcdest, const Operand& mem) {
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.lock_xaddb_rm(srcdest.encoding(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.lock_xaddb_rm(srcdest.encoding(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void lock_xaddw(Register srcdest, const Operand& mem) {
        masm.prefix_16_for_32();
        lock_xaddl(srcdest, mem);
    }
    void lock_xaddl(Register srcdest, const Operand& mem) {
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.lock_xaddl_rm(srcdest.encoding(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.lock_xaddl_rm(srcdest.encoding(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    void push(const Imm32 imm) {
        masm.push_i(imm.value);
    }

    void push(const Operand& src) {
        switch (src.kind()) {
          case Operand::REG:
            masm.push_r(src.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.push_m(src.disp(), src.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void push(Register src) {
        masm.push_r(src.encoding());
    }
    void push(const Address& src) {
        masm.push_m(src.offset, src.base.encoding());
    }

    void pop(const Operand& src) {
        switch (src.kind()) {
          case Operand::REG:
            masm.pop_r(src.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.pop_m(src.disp(), src.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void pop(Register src) {
        masm.pop_r(src.encoding());
    }
    void pop(const Address& src) {
        masm.pop_m(src.offset, src.base.encoding());
    }

    void pushFlags() {
        masm.push_flags();
    }
    void popFlags() {
        masm.pop_flags();
    }

#ifdef JS_CODEGEN_X86
    void pushAllRegs() {
        masm.pusha();
    }
    void popAllRegs() {
        masm.popa();
    }
#endif

    // Zero-extend byte to 32-bit integer.
    void movzbl(Register src, Register dest) {
        masm.movzbl_rr(src.encoding(), dest.encoding());
    }

    void cdq() {
        masm.cdq();
    }
    void idiv(Register divisor) {
        masm.idivl_r(divisor.encoding());
    }
    void udiv(Register divisor) {
        masm.divl_r(divisor.encoding());
    }

    void vpinsrd(unsigned lane, Register src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vpinsrd_irr(lane, src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpinsrd(unsigned lane, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::REG:
            masm.vpinsrd_irr(lane, src1.reg(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpinsrd_imr(lane, src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpextrd(unsigned lane, FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vpextrd_irr(lane, src.encoding(), dest.encoding());
    }
    void vpextrd(unsigned lane, FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE41());
        switch (dest.kind()) {
          case Operand::REG:
            masm.vpextrd_irr(lane, src.encoding(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpextrd_irm(lane, src.encoding(), dest.disp(), dest.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpsrldq(Imm32 shift, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrldq_ir(shift.value, src0.encoding(), dest.encoding());
    }
    void vpsllq(Imm32 shift, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsllq_ir(shift.value, src0.encoding(), dest.encoding());
    }
    void vpsrlq(Imm32 shift, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrlq_ir(shift.value, src0.encoding(), dest.encoding());
    }
    void vpslld(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpslld_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpslld(Imm32 count, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpslld_ir(count.value, src0.encoding(), dest.encoding());
    }
    void vpsrad(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrad_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpsrad(Imm32 count, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrad_ir(count.value, src0.encoding(), dest.encoding());
    }
    void vpsrld(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrld_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpsrld(Imm32 count, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrld_ir(count.value, src0.encoding(), dest.encoding());
    }

    void vcvtsi2sd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::REG:
            masm.vcvtsi2sd_rr(src1.reg(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vcvtsi2sd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vcvtsi2sd_mr(src1.disp(), src1.base(), src1.index(), src1.scale(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vcvttsd2si(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvttsd2si_rr(src.encoding(), dest.encoding());
    }
    void vcvttss2si(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvttss2si_rr(src.encoding(), dest.encoding());
    }
    void vcvtsi2ss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::REG:
            masm.vcvtsi2ss_rr(src1.reg(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vcvtsi2ss_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vcvtsi2ss_mr(src1.disp(), src1.base(), src1.index(), src1.scale(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vcvtsi2ss(Register src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtsi2ss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vcvtsi2sd(Register src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtsi2sd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vcvttps2dq(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvttps2dq_rr(src.encoding(), dest.encoding());
    }
    void vcvtdq2ps(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtdq2ps_rr(src.encoding(), dest.encoding());
    }
    void vmovmskpd(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovmskpd_rr(src.encoding(), dest.encoding());
    }
    void vmovmskps(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovmskps_rr(src.encoding(), dest.encoding());
    }
    void vptest(FloatRegister rhs, FloatRegister lhs) {
        MOZ_ASSERT(HasSSE41());
        masm.vptest_rr(rhs.encoding(), lhs.encoding());
    }
    void vucomisd(FloatRegister rhs, FloatRegister lhs) {
        MOZ_ASSERT(HasSSE2());
        masm.vucomisd_rr(rhs.encoding(), lhs.encoding());
    }
    void vucomiss(FloatRegister rhs, FloatRegister lhs) {
        MOZ_ASSERT(HasSSE2());
        masm.vucomiss_rr(rhs.encoding(), lhs.encoding());
    }
    void vpcmpeqw(FloatRegister rhs, FloatRegister lhs, FloatRegister dst) {
        MOZ_ASSERT(HasSSE2());
        masm.vpcmpeqw_rr(rhs.encoding(), lhs.encoding(), dst.encoding());
    }
    void vpcmpeqd(const Operand& rhs, FloatRegister lhs, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (rhs.kind()) {
          case Operand::FPREG:
            masm.vpcmpeqd_rr(rhs.fpu(), lhs.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpcmpeqd_mr(rhs.disp(), rhs.base(), lhs.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpcmpeqd_mr(rhs.address(), lhs.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpcmpgtd(const Operand& rhs, FloatRegister lhs, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (rhs.kind()) {
          case Operand::FPREG:
            masm.vpcmpgtd_rr(rhs.fpu(), lhs.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpcmpgtd_mr(rhs.disp(), rhs.base(), lhs.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpcmpgtd_mr(rhs.address(), lhs.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vcmpps(uint8_t order, Operand src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        // :TODO: (Bug 1132894) See LIRGeneratorX86Shared::lowerForFPU
        // FIXME: This logic belongs in the MacroAssembler.
        if (!HasAVX() && !src0.aliases(dest)) {
            if (src1.kind() == Operand::FPREG &&
                dest.aliases(FloatRegister::FromCode(src1.fpu())))
            {
                vmovdqa(src1, ScratchSimd128Reg);
                src1 = Operand(ScratchSimd128Reg);
            }
            vmovdqa(src0, dest);
            src0 = dest;
        }
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vcmpps_rr(order, src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vcmpps_mr(order, src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vcmpps_mr(order, src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vcmpeqps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        vcmpps(X86Encoding::ConditionCmp_EQ, src1, src0, dest);
    }
    void vcmpltps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        vcmpps(X86Encoding::ConditionCmp_LT, src1, src0, dest);
    }
    void vcmpleps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        vcmpps(X86Encoding::ConditionCmp_LE, src1, src0, dest);
    }
    void vcmpunordps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        vcmpps(X86Encoding::ConditionCmp_UNORD, src1, src0, dest);
    }
    void vcmpneqps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        vcmpps(X86Encoding::ConditionCmp_NEQ, src1, src0, dest);
    }
    void vrcpps(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vrcpps_rr(src.fpu(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vrcpps_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vrcpps_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vsqrtps(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vsqrtps_rr(src.fpu(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vsqrtps_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vsqrtps_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vrsqrtps(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vrsqrtps_rr(src.fpu(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vrsqrtps_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vrsqrtps_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovd(Register src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_rr(src.encoding(), dest.encoding());
    }
    void vmovd(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_rr(src.encoding(), dest.encoding());
    }
    void vmovd(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovd_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vmovd_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovd(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovd_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovd_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovq_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovq(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovq_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vmovq_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovq_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovq(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovq_rm(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovq_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpaddd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpaddd_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpaddd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpaddd_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpsubd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpsubd_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpsubd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpsubd_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpmuludq(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpmuludq_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpmuludq(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpmuludq_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpmuludq_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpmulld(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpmulld_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpmulld_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpmulld_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vaddps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vaddps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vaddps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vaddps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vsubps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vsubps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vsubps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vsubps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmulps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmulps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmulps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmulps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vdivps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vdivps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vdivps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vdivps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmaxps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmaxps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmaxps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmaxps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vminps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vminps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vminps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vminps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vandps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vandps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vandps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vandps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vandnps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        // Negates bits of dest and then applies AND
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vandnps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vandnps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vandnps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vorps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vorps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vorps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vorps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vxorps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vxorps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vxorps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vxorps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpand(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpand_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpand(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpand_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpand_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpand_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpor(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpor_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpor(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpor_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpor_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpor_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpxor(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpxor_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpxor(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpxor_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpxor_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpxor_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpandn(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpandn_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpandn(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpandn_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpandn_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpandn_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    void vpshufd(uint32_t mask, FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpshufd_irr(mask, src.encoding(), dest.encoding());
    }
    void vpshufd(uint32_t mask, const Operand& src1, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpshufd_irr(mask, src1.fpu(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpshufd_imr(mask, src1.disp(), src1.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpshufd_imr(mask, src1.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovddup(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        masm.vmovddup_rr(src.encoding(), dest.encoding());
    }
    void vmovhlps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovhlps_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vmovlhps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovlhps_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vunpcklps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vunpcklps_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vunpcklps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vunpcklps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vunpcklps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vunpcklps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vunpckhps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vunpckhps_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vunpckhps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vunpckhps_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vunpckhps_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vunpckhps_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vshufps(uint32_t mask, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vshufps_irr(mask, src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vshufps(uint32_t mask, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vshufps_irr(mask, src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vshufps_imr(mask, src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vshufps_imr(mask, src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vaddsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vaddsd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vaddss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vaddss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vaddsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vaddsd_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vaddsd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vaddsd_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vaddss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vaddss_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vaddss_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vaddss_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vsubsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vsubsd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vsubss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vsubss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vsubsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vsubsd_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vsubsd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vsubss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vsubss_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vsubss_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmulsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmulsd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vmulsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmulsd_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmulsd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmulss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmulss_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmulss_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmulss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmulss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vdivsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vdivsd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vdivss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vdivss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vdivsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vdivsd_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vdivsd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vdivss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vdivss_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vdivss_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vxorpd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vxorpd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vxorps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vxorps_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vorpd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vorpd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vorps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vorps_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vandpd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vandpd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vandps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vandps_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vsqrtsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vsqrtsd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vsqrtss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vsqrtss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vroundsd(X86Encoding::RoundingMode mode, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vroundsd_irr(mode, src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vroundss(X86Encoding::RoundingMode mode, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vroundss_irr(mode, src1.encoding(), src0.encoding(), dest.encoding());
    }
    unsigned vinsertpsMask(SimdLane sourceLane, SimdLane destLane, unsigned zeroMask = 0)
    {
        // Note that the sourceLane bits are ignored in the case of a source
        // memory operand, and the source is the given 32-bits memory location.
        MOZ_ASSERT(zeroMask < 16);
        unsigned ret = zeroMask ;
        ret |= unsigned(destLane) << 4;
        ret |= unsigned(sourceLane) << 6;
        MOZ_ASSERT(ret < 256);
        return ret;
    }
    void vinsertps(uint32_t mask, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vinsertps_irr(mask, src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vinsertps(uint32_t mask, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vinsertps_irr(mask, src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vinsertps_imr(mask, src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    unsigned blendpsMask(bool x, bool y, bool z, bool w) {
        return (x << 0) | (y << 1) | (z << 2) | (w << 3);
    }
    void vblendps(unsigned mask, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vblendps_irr(mask, src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vblendps(unsigned mask, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vblendps_irr(mask, src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vblendps_imr(mask, src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vblendvps(FloatRegister mask, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vblendvps_rr(mask.encoding(), src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vblendvps(FloatRegister mask, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vblendvps_rr(mask.encoding(), src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vblendvps_mr(mask.encoding(), src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovsldup(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        masm.vmovsldup_rr(src.encoding(), dest.encoding());
    }
    void vmovsldup(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vmovsldup_rr(src.fpu(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmovsldup_mr(src.disp(), src.base(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovshdup(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        masm.vmovshdup_rr(src.encoding(), dest.encoding());
    }
    void vmovshdup(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vmovshdup_rr(src.fpu(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmovshdup_mr(src.disp(), src.base(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vminsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vminsd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vminsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vminsd_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vminsd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vminss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vminss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vmaxsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmaxsd_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vmaxsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmaxsd_rr(src1.fpu(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmaxsd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmaxss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmaxss_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void fisttp(const Operand& dest) {
        MOZ_ASSERT(HasSSE3());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.fisttp_m(dest.disp(), dest.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void fld(const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.fld_m(dest.disp(), dest.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void fstp(const Operand& src) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.fstp_m(src.disp(), src.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void fstp32(const Operand& src) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.fstp32_m(src.disp(), src.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    // Defined for compatibility with ARM's assembler
    uint32_t actualIndex(uint32_t x) {
        return x;
    }

    void flushBuffer() {
    }

    // Patching.

    static size_t PatchWrite_NearCallSize() {
        return 5;
    }
    static uintptr_t GetPointer(uint8_t* instPtr) {
        uintptr_t* ptr = ((uintptr_t*) instPtr) - 1;
        return *ptr;
    }
    // Write a relative call at the start location |dataLabel|.
    // Note that this DOES NOT patch data that comes before |label|.
    static void PatchWrite_NearCall(CodeLocationLabel startLabel, CodeLocationLabel target) {
        uint8_t* start = startLabel.raw();
        *start = 0xE8;
        ptrdiff_t offset = target - startLabel - PatchWrite_NearCallSize();
        MOZ_ASSERT(int32_t(offset) == offset);
        *((int32_t*) (start + 1)) = offset;
    }

    static void PatchWrite_Imm32(CodeLocationLabel dataLabel, Imm32 toWrite) {
        *((int32_t*) dataLabel.raw() - 1) = toWrite.value;
    }

    static void PatchDataWithValueCheck(CodeLocationLabel data, PatchedImmPtr newData,
                                        PatchedImmPtr expectedData) {
        // The pointer given is a pointer to *after* the data.
        uintptr_t* ptr = ((uintptr_t*) data.raw()) - 1;
        MOZ_ASSERT(*ptr == (uintptr_t)expectedData.value);
        *ptr = (uintptr_t)newData.value;
    }
    static void PatchDataWithValueCheck(CodeLocationLabel data, ImmPtr newData, ImmPtr expectedData) {
        PatchDataWithValueCheck(data, PatchedImmPtr(newData.value), PatchedImmPtr(expectedData.value));
    }

    static void PatchInstructionImmediate(uint8_t* code, PatchedImmPtr imm) {
        MOZ_CRASH("Unused.");
    }

    static uint32_t NopSize() {
        return 1;
    }
    static uint8_t* NextInstruction(uint8_t* cur, uint32_t* count) {
        MOZ_CRASH("nextInstruction NYI on x86");
    }

    // Toggle a jmp or cmp emitted by toggledJump().
    static void ToggleToJmp(CodeLocationLabel inst) {
        uint8_t* ptr = (uint8_t*)inst.raw();
        MOZ_ASSERT(*ptr == 0x3D);
        *ptr = 0xE9;
    }
    static void ToggleToCmp(CodeLocationLabel inst) {
        uint8_t* ptr = (uint8_t*)inst.raw();
        MOZ_ASSERT(*ptr == 0xE9);
        *ptr = 0x3D;
    }
    static void ToggleCall(CodeLocationLabel inst, bool enabled) {
        uint8_t* ptr = (uint8_t*)inst.raw();
        MOZ_ASSERT(*ptr == 0x3D || // CMP
                   *ptr == 0xE8);  // CALL
        *ptr = enabled ? 0xE8 : 0x3D;
    }

    MOZ_COLD void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                              const Disassembler::HeapAccess& heapAccess);
};

} // namespace jit
} // namespace js

#endif /* jit_x86_shared_Assembler_x86_shared_h */
