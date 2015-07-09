/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_Assembler_x86_shared_h
#define jit_shared_Assembler_x86_shared_h

#include <cstddef>

#include "jit/shared/Assembler-shared.h"
#include "jit/shared/BaseAssembler-x86-shared.h"

namespace js {
namespace jit {

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
    int32_t base_ : 5;
    Scale scale_ : 3;
    int32_t index_ : 5;
    int32_t disp_;

  public:
    explicit Operand(Register reg)
      : kind_(REG),
        base_(reg.code())
    { }
    explicit Operand(FloatRegister reg)
      : kind_(FPREG),
        base_(reg.code())
    { }
    explicit Operand(const Address& address)
      : kind_(MEM_REG_DISP),
        base_(address.base.code()),
        disp_(address.offset)
    { }
    explicit Operand(const BaseIndex& address)
      : kind_(MEM_SCALE),
        base_(address.base.code()),
        scale_(address.scale),
        index_(address.index.code()),
        disp_(address.offset)
    { }
    Operand(Register base, Register index, Scale scale, int32_t disp = 0)
      : kind_(MEM_SCALE),
        base_(base.code()),
        scale_(scale),
        index_(index.code()),
        disp_(disp)
    { }
    Operand(Register reg, int32_t disp)
      : kind_(MEM_REG_DISP),
        base_(reg.code()),
        disp_(disp)
    { }
    explicit Operand(AbsoluteAddress address)
      : kind_(MEM_ADDRESS32),
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
    Registers::Code reg() const {
        MOZ_ASSERT(kind() == REG);
        return (Registers::Code)base_;
    }
    Registers::Code base() const {
        MOZ_ASSERT(kind() == MEM_REG_DISP || kind() == MEM_SCALE);
        return (Registers::Code)base_;
    }
    Registers::Code index() const {
        MOZ_ASSERT(kind() == MEM_SCALE);
        return (Registers::Code)index_;
    }
    Scale scale() const {
        MOZ_ASSERT(kind() == MEM_SCALE);
        return scale_;
    }
    FloatRegisters::Code fpu() const {
        MOZ_ASSERT(kind() == FPREG);
        return (FloatRegisters::Code)base_;
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
          case REG:          return r.code() == reg();
          case MEM_REG_DISP: return r.code() == base();
          case MEM_SCALE:    return r.code() == base() || r.code() == index();
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

    Vector<CodeLabel, 0, SystemAllocPolicy> codeLabels_;
    Vector<RelativePatch, 8, SystemAllocPolicy> jumps_;
    CompactBufferWriter jumpRelocations_;
    CompactBufferWriter dataRelocations_;
    CompactBufferWriter preBarriers_;

    void writeDataRelocation(ImmGCPtr ptr) {
        if (ptr.value)
            dataRelocations_.writeUnsigned(masm.currentOffset());
    }
    void writePrebarrierOffset(CodeOffsetLabel label) {
        preBarriers_.writeUnsigned(label.offset());
    }

  protected:
    X86Encoding::BaseAssembler masm;

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

    static void FixupNurseryObjects(JSContext* cx, JitCode* code, CompactBufferReader& reader,
                                    const ObjectVector& nurseryObjects);

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

    void executableCopy(void* buffer);
    void processCodeLabels(uint8_t* rawCode);
    static int32_t ExtractCodeLabelOffset(uint8_t* code) {
        return *(uintptr_t*)code;
    }
    void copyJumpRelocationTable(uint8_t* dest);
    void copyDataRelocationTable(uint8_t* dest);
    void copyPreBarrierTable(uint8_t* dest);

    void addCodeLabel(CodeLabel label) {
        propagateOOM(codeLabels_.append(label));
    }
    size_t numCodeLabels() const {
        return codeLabels_.length();
    }
    CodeLabel codeLabel(size_t i) {
        return codeLabels_[i];
    }

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
    void align(int alignment) {
        masm.align(alignment);
    }
    void writeCodePointer(AbsoluteLabel* label) {
        MOZ_ASSERT(!label->bound());
        // Thread the patch list through the unpatched address word in the
        // instruction stream.
        masm.jumpTablePointer(label->prev());
        label->setPrev(masm.size());
    }
    void writeDoubleConstant(double d, Label* label) {
        label->bind(masm.size());
        masm.doubleConstant(d);
    }
    void writeFloatConstant(float f, Label* label) {
        label->bind(masm.size());
        masm.floatConstant(f);
    }
    void writeInt32x4Constant(const SimdConstant& v, Label* label) {
        label->bind(masm.size());
        masm.int32x4Constant(v.asInt32x4());
    }
    void writeFloat32x4Constant(const SimdConstant& v, Label* label) {
        label->bind(masm.size());
        masm.float32x4Constant(v.asFloat32x4());
    }
    void movl(Imm32 imm32, Register dest) {
        masm.movl_i32r(imm32.value, dest.code());
    }
    void movl(Register src, Register dest) {
        masm.movl_rr(src.code(), dest.code());
    }
    void movl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.movl_rr(src.reg(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.movl_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.movl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movl_mr(src.address(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movl(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.movl_rr(src.code(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.movl_rm(src.code(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.movl_rm(src.code(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movl_rm(src.code(), dest.address());
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
        masm.xchgl_rr(src.code(), dest.code());
    }

    // Eventually vmovapd should be overloaded to support loads and
    // stores too.
    void vmovapd(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovapd_rr(src.code(), dest.code());
    }

    void vmovaps(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovaps_rr(src.code(), dest.code());
    }
    void vmovaps(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovaps_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.vmovaps_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          case Operand::FPREG:
            masm.vmovaps_rr(src.fpu(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovaps(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovaps_rm(src.code(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovaps_rm(src.code(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovups(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovups_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.vmovups_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovups(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovups_rm(src.code(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovups_rm(src.code(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    // vmovsd is only provided in load/store form since the
    // register-to-register form has different semantics (it doesn't clobber
    // the whole output register) and isn't needed currently.
    void vmovsd(const Address& src, FloatRegister dest) {
        masm.vmovsd_mr(src.offset, src.base.code(), dest.code());
    }
    void vmovsd(const BaseIndex& src, FloatRegister dest) {
        masm.vmovsd_mr(src.offset, src.base.code(), src.index.code(), src.scale, dest.code());
    }
    void vmovsd(FloatRegister src, const Address& dest) {
        masm.vmovsd_rm(src.code(), dest.offset, dest.base.code());
    }
    void vmovsd(FloatRegister src, const BaseIndex& dest) {
        masm.vmovsd_rm(src.code(), dest.offset, dest.base.code(), dest.index.code(), dest.scale);
    }
    // Although vmovss is not only provided in load/store form (for the same
    // reasons as vmovsd above), the register to register form should be only
    // used in contexts where we care about not clearing the higher lanes of
    // the FloatRegister.
    void vmovss(const Address& src, FloatRegister dest) {
        masm.vmovss_mr(src.offset, src.base.code(), dest.code());
    }
    void vmovss(const BaseIndex& src, FloatRegister dest) {
        masm.vmovss_mr(src.offset, src.base.code(), src.index.code(), src.scale, dest.code());
    }
    void vmovss(FloatRegister src, const Address& dest) {
        masm.vmovss_rm(src.code(), dest.offset, dest.base.code());
    }
    void vmovss(FloatRegister src, const BaseIndex& dest) {
        masm.vmovss_rm(src.code(), dest.offset, dest.base.code(), dest.index.code(), dest.scale);
    }
    void vmovss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        masm.vmovss_rr(src1.code(), src0.code(), dest.code());
    }
    void vmovdqu(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovdqu_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.vmovdqu_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovdqu(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovdqu_rm(src.code(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovdqu_rm(src.code(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovdqa(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vmovdqa_rr(src.fpu(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmovdqa_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.vmovdqa_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovdqa(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovdqa_rm(src.code(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovdqa_rm(src.code(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovdqa(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqa_rr(src.code(), dest.code());
    }
    void vcvtss2sd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtss2sd_rr(src1.code(), src0.code(), dest.code());
    }
    void vcvtsd2ss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtsd2ss_rr(src1.code(), src0.code(), dest.code());
    }
    void movzbl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movzbl_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.movzbl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movsbl(Register src, Register dest) {
        masm.movsbl_rr(src.code(), dest.code());
    }
    void movsbl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movsbl_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.movsbl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movb(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movb_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.movb_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movb(Imm32 src, Register dest) {
        masm.movb_ir(src.value & 255, dest.code());
    }
    void movb(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movb_rm(src.code(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.movb_rm(src.code(), dest.disp(), dest.base(), dest.index(), dest.scale());
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
            masm.movzwl_rr(src.reg(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.movzwl_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.movzwl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movzwl(Register src, Register dest) {
        masm.movzwl_rr(src.code(), dest.code());
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
            masm.movw_rm(src.code(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.movw_rm(src.code(), dest.disp(), dest.base(), dest.index(), dest.scale());
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
        masm.movswl_rr(src.code(), dest.code());
    }
    void movswl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movswl_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.movswl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void leal(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.leal_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.leal_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
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
    uint32_t currentOffset() {
        return masm.label().offset();
    }

    // Re-routes pending jumps to a new label.
    void retarget(Label* label, Label* target) {
        if (label->used()) {
            bool more;
            JmpSrc jmp(label->offset());
            do {
                JmpSrc next;
                more = masm.nextJump(jmp, &next);

                if (target->bound()) {
                    // The jump can be immediately patched to the correct destination.
                    masm.linkJump(jmp, JmpDst(target->offset()));
                } else {
                    // Thread the jump list through the unpatched jump targets.
                    JmpSrc prev = JmpSrc(target->use(jmp.offset()));
                    masm.setNextJump(jmp, prev);
                }

                jmp = next;
            } while (more);
        }
        label->reset();
    }

    static void Bind(uint8_t* raw, AbsoluteLabel* label, const void* address) {
        if (label->used()) {
            intptr_t src = label->offset();
            do {
                intptr_t next = reinterpret_cast<intptr_t>(X86Encoding::GetPointer(raw + src));
                X86Encoding::SetPointer(raw + src, address);
                src = next;
            } while (src != AbsoluteLabel::INVALID_OFFSET);
        }
        label->bind();
    }

    // See Bind and X86Encoding::setPointer.
    size_t labelOffsetToPatchOffset(size_t offset) {
        return offset - sizeof(void*);
    }

    void ret() {
        masm.ret();
    }
    void retn(Imm32 n) {
        // Remove the size of the return address which is included in the frame.
        masm.ret_i(n.value - sizeof(void*));
    }
    void call(Label* label) {
        if (label->bound()) {
            masm.linkJump(masm.call(), JmpDst(label->offset()));
        } else {
            JmpSrc j = masm.call();
            JmpSrc prev = JmpSrc(label->use(j.offset()));
            masm.setNextJump(j, prev);
        }
    }
    void call(Register reg) {
        masm.call_r(reg.code());
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
        masm.cmpl_rr(rhs.code(), lhs.code());
    }
    void cmpl(const Operand& rhs, Register lhs) {
        switch (rhs.kind()) {
          case Operand::REG:
            masm.cmpl_rr(rhs.reg(), lhs.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.cmpl_mr(rhs.disp(), rhs.base(), lhs.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.cmpl_mr(rhs.address(), lhs.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void cmpl(Register rhs, const Operand& lhs) {
        switch (lhs.kind()) {
          case Operand::REG:
            masm.cmpl_rr(rhs.code(), lhs.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.cmpl_rm(rhs.code(), lhs.disp(), lhs.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.cmpl_rm(rhs.code(), lhs.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void cmpl(Imm32 rhs, Register lhs) {
        masm.cmpl_ir(rhs.value, lhs.code());
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
    CodeOffsetLabel cmplWithPatch(Imm32 rhs, Register lhs) {
        masm.cmpl_i32r(rhs.value, lhs.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    void cmpw(Register rhs, Register lhs) {
        masm.cmpw_rr(rhs.code(), lhs.code());
    }
    void setCC(Condition cond, Register r) {
        masm.setCC_r(static_cast<X86Encoding::Condition>(cond), r.code());
    }
    void testb(Register rhs, Register lhs) {
        MOZ_ASSERT(GeneralRegisterSet(Registers::SingleByteRegs).has(rhs));
        MOZ_ASSERT(GeneralRegisterSet(Registers::SingleByteRegs).has(lhs));
        masm.testb_rr(rhs.code(), lhs.code());
    }
    void testw(Register rhs, Register lhs) {
        masm.testw_rr(lhs.code(), rhs.code());
    }
    void testl(Register rhs, Register lhs) {
        masm.testl_rr(lhs.code(), rhs.code());
    }
    void testl(Imm32 rhs, Register lhs) {
        masm.testl_ir(rhs.value, lhs.code());
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
        masm.addl_ir(imm.value, dest.code());
    }
    CodeOffsetLabel addlWithPatch(Imm32 imm, Register dest) {
        masm.addl_i32r(imm.value, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
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
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    // Note, lock_addl() is used for a memory barrier on non-SSE2 systems.
    // Do not optimize, replace by XADDL, or similar.
    void lock_addl(Imm32 imm, const Operand& op) {
        masm.prefix_lock();
        addl(imm, op);
    }
    void subl(Imm32 imm, Register dest) {
        masm.subl_ir(imm.value, dest.code());
    }
    void subl(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.subl_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.subl_im(imm.value, op.disp(), op.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void addl(Register src, Register dest) {
        masm.addl_rr(src.code(), dest.code());
    }
    void subl(Register src, Register dest) {
        masm.subl_rr(src.code(), dest.code());
    }
    void subl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.subl_rr(src.reg(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.subl_mr(src.disp(), src.base(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void subl(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.subl_rr(src.code(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.subl_rm(src.code(), dest.disp(), dest.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void orl(Register reg, Register dest) {
        masm.orl_rr(reg.code(), dest.code());
    }
    void orl(Imm32 imm, Register reg) {
        masm.orl_ir(imm.value, reg.code());
    }
    void orl(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.orl_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.orl_im(imm.value, op.disp(), op.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void xorl(Register src, Register dest) {
        masm.xorl_rr(src.code(), dest.code());
    }
    void xorl(Imm32 imm, Register reg) {
        masm.xorl_ir(imm.value, reg.code());
    }
    void xorl(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.xorl_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.xorl_im(imm.value, op.disp(), op.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void andl(Register src, Register dest) {
        masm.andl_rr(src.code(), dest.code());
    }
    void andl(Imm32 imm, Register dest) {
        masm.andl_ir(imm.value, dest.code());
    }
    void andl(Imm32 imm, const Operand& op) {
        switch (op.kind()) {
          case Operand::REG:
            masm.andl_ir(imm.value, op.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.andl_im(imm.value, op.disp(), op.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void addl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.addl_rr(src.reg(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.addl_mr(src.disp(), src.base(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void orl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.orl_rr(src.reg(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.orl_mr(src.disp(), src.base(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void xorl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.xorl_rr(src.reg(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.xorl_mr(src.disp(), src.base(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void andl(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.andl_rr(src.reg(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.andl_mr(src.disp(), src.base(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void bsr(const Register& src, const Register& dest) {
        masm.bsr_rr(src.code(), dest.code());
    }
    void imull(Register multiplier) {
        masm.imull_r(multiplier.code());
    }
    void imull(Imm32 imm, Register dest) {
        masm.imull_ir(imm.value, dest.code(), dest.code());
    }
    void imull(Register src, Register dest) {
        masm.imull_rr(src.code(), dest.code());
    }
    void imull(Imm32 imm, Register src, Register dest) {
        masm.imull_ir(imm.value, src.code(), dest.code());
    }
    void imull(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::REG:
            masm.imull_rr(src.reg(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.imull_mr(src.disp(), src.base(), dest.code());
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
        masm.negl_r(reg.code());
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
        masm.notl_r(reg.code());
    }
    void shrl(const Imm32 imm, Register dest) {
        masm.shrl_ir(imm.value, dest.code());
    }
    void shll(const Imm32 imm, Register dest) {
        masm.shll_ir(imm.value, dest.code());
    }
    void sarl(const Imm32 imm, Register dest) {
        masm.sarl_ir(imm.value, dest.code());
    }
    void shrl_cl(Register dest) {
        masm.shrl_CLr(dest.code());
    }
    void shll_cl(Register dest) {
        masm.shll_CLr(dest.code());
    }
    void sarl_cl(Register dest) {
        masm.sarl_CLr(dest.code());
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

    void lock_cmpxchg8(Register src, const Operand& mem) {
        masm.prefix_lock();
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.cmpxchg8(src.code(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.cmpxchg8(src.code(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void lock_cmpxchg16(Register src, const Operand& mem) {
        masm.prefix_lock();
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.cmpxchg16(src.code(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.cmpxchg16(src.code(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void lock_cmpxchg32(Register src, const Operand& mem) {
        masm.prefix_lock();
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.cmpxchg32(src.code(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.cmpxchg32(src.code(), mem.disp(), mem.base(), mem.index(), mem.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    void lock_xaddb(Register srcdest, const Operand& mem) {
        switch (mem.kind()) {
          case Operand::MEM_REG_DISP:
            masm.lock_xaddb_rm(srcdest.code(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.lock_xaddb_rm(srcdest.code(), mem.disp(), mem.base(), mem.index(), mem.scale());
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
            masm.lock_xaddl_rm(srcdest.code(), mem.disp(), mem.base());
            break;
          case Operand::MEM_SCALE:
            masm.lock_xaddl_rm(srcdest.code(), mem.disp(), mem.base(), mem.index(), mem.scale());
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
        masm.push_r(src.code());
    }
    void push(const Address& src) {
        masm.push_m(src.offset, src.base.code());
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
        masm.pop_r(src.code());
    }
    void pop(const Address& src) {
        masm.pop_m(src.offset, src.base.code());
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
        masm.movzbl_rr(src.code(), dest.code());
    }

    void cdq() {
        masm.cdq();
    }
    void idiv(Register divisor) {
        masm.idivl_r(divisor.code());
    }
    void udiv(Register divisor) {
        masm.divl_r(divisor.code());
    }

    void vpinsrd(unsigned lane, Register src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vpinsrd_irr(lane, src1.code(), src0.code(), dest.code());
    }
    void vpinsrd(unsigned lane, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::REG:
            masm.vpinsrd_irr(lane, src1.reg(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpinsrd_imr(lane, src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpextrd(unsigned lane, FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vpextrd_irr(lane, src.code(), dest.code());
    }
    void vpextrd(unsigned lane, FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE41());
        switch (dest.kind()) {
          case Operand::REG:
            masm.vpextrd_irr(lane, src.code(), dest.reg());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpextrd_irm(lane, src.code(), dest.disp(), dest.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpsrldq(Imm32 shift, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrldq_ir(shift.value, src0.code(), dest.code());
    }
    void vpsllq(Imm32 shift, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsllq_ir(shift.value, src0.code(), dest.code());
    }
    void vpsrlq(Imm32 shift, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrlq_ir(shift.value, src0.code(), dest.code());
    }
    void vpslld(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpslld_rr(src1.code(), src0.code(), dest.code());
    }
    void vpslld(Imm32 count, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpslld_ir(count.value, src0.code(), dest.code());
    }
    void vpsrad(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrad_rr(src1.code(), src0.code(), dest.code());
    }
    void vpsrad(Imm32 count, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrad_ir(count.value, src0.code(), dest.code());
    }
    void vpsrld(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrld_rr(src1.code(), src0.code(), dest.code());
    }
    void vpsrld(Imm32 count, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpsrld_ir(count.value, src0.code(), dest.code());
    }

    void vcvtsi2sd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::REG:
            masm.vcvtsi2sd_rr(src1.reg(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vcvtsi2sd_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.vcvtsi2sd_mr(src1.disp(), src1.base(), src1.index(), src1.scale(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vcvttsd2si(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvttsd2si_rr(src.code(), dest.code());
    }
    void vcvttss2si(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvttss2si_rr(src.code(), dest.code());
    }
    void vcvtsi2ss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::REG:
            masm.vcvtsi2ss_rr(src1.reg(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vcvtsi2ss_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.vcvtsi2ss_mr(src1.disp(), src1.base(), src1.index(), src1.scale(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vcvtsi2ss(Register src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtsi2ss_rr(src1.code(), src0.code(), dest.code());
    }
    void vcvtsi2sd(Register src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtsi2sd_rr(src1.code(), src0.code(), dest.code());
    }
    void vcvttps2dq(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvttps2dq_rr(src.code(), dest.code());
    }
    void vcvtdq2ps(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vcvtdq2ps_rr(src.code(), dest.code());
    }
    void vmovmskpd(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovmskpd_rr(src.code(), dest.code());
    }
    void vmovmskps(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovmskps_rr(src.code(), dest.code());
    }
    void vptest(FloatRegister rhs, FloatRegister lhs) {
        MOZ_ASSERT(HasSSE41());
        masm.vptest_rr(rhs.code(), lhs.code());
    }
    void vucomisd(FloatRegister rhs, FloatRegister lhs) {
        MOZ_ASSERT(HasSSE2());
        masm.vucomisd_rr(rhs.code(), lhs.code());
    }
    void vucomiss(FloatRegister rhs, FloatRegister lhs) {
        MOZ_ASSERT(HasSSE2());
        masm.vucomiss_rr(rhs.code(), lhs.code());
    }
    void vpcmpeqw(FloatRegister rhs, FloatRegister lhs, FloatRegister dst) {
        MOZ_ASSERT(HasSSE2());
        masm.vpcmpeqw_rr(rhs.code(), lhs.code(), dst.code());
    }
    void vpcmpeqd(const Operand& rhs, FloatRegister lhs, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (rhs.kind()) {
          case Operand::FPREG:
            masm.vpcmpeqd_rr(rhs.fpu(), lhs.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpcmpeqd_mr(rhs.disp(), rhs.base(), lhs.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpcmpeqd_mr(rhs.address(), lhs.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpcmpgtd(const Operand& rhs, FloatRegister lhs, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (rhs.kind()) {
          case Operand::FPREG:
            masm.vpcmpgtd_rr(rhs.fpu(), lhs.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpcmpgtd_mr(rhs.disp(), rhs.base(), lhs.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpcmpgtd_mr(rhs.address(), lhs.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vcmpps(uint8_t order, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vcmpps_rr(order, src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vcmpps_mr(order, src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vcmpps_mr(order, src1.address(), src0.code(), dest.code());
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
            masm.vrcpps_rr(src.fpu(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vrcpps_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vrcpps_mr(src.address(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vsqrtps(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vsqrtps_rr(src.fpu(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vsqrtps_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vsqrtps_mr(src.address(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vrsqrtps(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vrsqrtps_rr(src.fpu(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vrsqrtps_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vrsqrtps_mr(src.address(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovd(Register src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_rr(src.code(), dest.code());
    }
    void vmovd(FloatRegister src, Register dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_rr(src.code(), dest.code());
    }
    void vmovd(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovd_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.vmovd_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovd(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovd_rm(src.code(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovd_rm(src.code(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovq_rm(src.code(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovq(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovq_mr(src.disp(), src.base(), dest.code());
            break;
          case Operand::MEM_SCALE:
            masm.vmovq_mr(src.disp(), src.base(), src.index(), src.scale(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovq_mr(src.address(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovq(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovq_rm(src.code(), dest.disp(), dest.base());
            break;
          case Operand::MEM_SCALE:
            masm.vmovq_rm(src.code(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpaddd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpaddd_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpaddd_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpaddd_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpsubd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpsubd_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpsubd_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpsubd_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpmuludq(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpmuludq_rr(src1.code(), src0.code(), dest.code());
    }
    void vpmuludq(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpmuludq_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpmuludq_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpmulld(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpmulld_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpmulld_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpmulld_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vaddps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vaddps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vaddps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vaddps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vsubps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vsubps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vsubps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vsubps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmulps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmulps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmulps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmulps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vdivps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vdivps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vdivps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vdivps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmaxps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmaxps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmaxps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmaxps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vminps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vminps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vminps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vminps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vandps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vandps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vandps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vandps_mr(src1.address(), src0.code(), dest.code());
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
            masm.vandnps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vandnps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vandnps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vorps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vorps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vorps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vorps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vxorps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vxorps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vxorps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vxorps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpand(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpand_rr(src1.code(), src0.code(), dest.code());
    }
    void vpand(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpand_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpand_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpand_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpor(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpor_rr(src1.code(), src0.code(), dest.code());
    }
    void vpor(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpor_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpor_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpor_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpxor(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpxor_rr(src1.code(), src0.code(), dest.code());
    }
    void vpxor(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpxor_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpxor_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpxor_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vpandn(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpandn_rr(src1.code(), src0.code(), dest.code());
    }
    void vpandn(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpandn_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpandn_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpandn_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    void vpshufd(uint32_t mask, FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vpshufd_irr(mask, src.code(), dest.code());
    }
    void vpshufd(uint32_t mask, const Operand& src1, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vpshufd_irr(mask, src1.fpu(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vpshufd_imr(mask, src1.disp(), src1.base(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpshufd_imr(mask, src1.address(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovhlps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovhlps_rr(src1.code(), src0.code(), dest.code());
    }
    void vmovlhps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovlhps_rr(src1.code(), src0.code(), dest.code());
    }
    void vunpcklps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vunpcklps_rr(src1.code(), src0.code(), dest.code());
    }
    void vunpcklps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vunpcklps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vunpcklps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vunpcklps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vunpckhps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vunpckhps_rr(src1.code(), src0.code(), dest.code());
    }
    void vunpckhps(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vunpckhps_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vunpckhps_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vunpckhps_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vshufps(uint32_t mask, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vshufps_irr(mask, src1.code(), src0.code(), dest.code());
    }
    void vshufps(uint32_t mask, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vshufps_irr(mask, src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vshufps_imr(mask, src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vshufps_imr(mask, src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vaddsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vaddsd_rr(src1.code(), src0.code(), dest.code());
    }
    void vaddss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vaddss_rr(src1.code(), src0.code(), dest.code());
    }
    void vaddsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vaddsd_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vaddsd_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vaddsd_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vaddss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vaddss_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vaddss_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vaddss_mr(src1.address(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vsubsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vsubsd_rr(src1.code(), src0.code(), dest.code());
    }
    void vsubss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vsubss_rr(src1.code(), src0.code(), dest.code());
    }
    void vsubsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vsubsd_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vsubsd_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vsubss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vsubss_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vsubss_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmulsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmulsd_rr(src1.code(), src0.code(), dest.code());
    }
    void vmulsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmulsd_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmulsd_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmulss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmulss_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmulss_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmulss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmulss_rr(src1.code(), src0.code(), dest.code());
    }
    void vdivsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vdivsd_rr(src1.code(), src0.code(), dest.code());
    }
    void vdivss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vdivss_rr(src1.code(), src0.code(), dest.code());
    }
    void vdivsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vdivsd_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vdivsd_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vdivss(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vdivss_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vdivss_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vxorpd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vxorpd_rr(src1.code(), src0.code(), dest.code());
    }
    void vxorps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vxorps_rr(src1.code(), src0.code(), dest.code());
    }
    void vorpd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vorpd_rr(src1.code(), src0.code(), dest.code());
    }
    void vorps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vorps_rr(src1.code(), src0.code(), dest.code());
    }
    void vandpd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vandpd_rr(src1.code(), src0.code(), dest.code());
    }
    void vandps(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vandps_rr(src1.code(), src0.code(), dest.code());
    }
    void vsqrtsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vsqrtsd_rr(src1.code(), src0.code(), dest.code());
    }
    void vsqrtss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vsqrtss_rr(src1.code(), src0.code(), dest.code());
    }
    void vroundsd(X86Encoding::RoundingMode mode, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vroundsd_irr(mode, src1.code(), src0.code(), dest.code());
    }
    void vroundss(X86Encoding::RoundingMode mode, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vroundss_irr(mode, src1.code(), src0.code(), dest.code());
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
        masm.vinsertps_irr(mask, src1.code(), src0.code(), dest.code());
    }
    void vinsertps(uint32_t mask, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vinsertps_irr(mask, src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vinsertps_imr(mask, src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    unsigned blendpsMask(bool x, bool y, bool z, bool w) {
        return x | (y << 1) | (z << 2) | (w << 3);
    }
    void vblendps(unsigned mask, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vblendps_irr(mask, src1.code(), src0.code(), dest.code());
    }
    void vblendps(unsigned mask, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vblendps_irr(mask, src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vblendps_imr(mask, src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vblendvps(FloatRegister mask, FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        masm.vblendvps_rr(mask.code(), src1.code(), src0.code(), dest.code());
    }
    void vblendvps(FloatRegister mask, const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE41());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vblendvps_rr(mask.code(), src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vblendvps_mr(mask.code(), src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovsldup(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        masm.vmovsldup_rr(src.code(), dest.code());
    }
    void vmovsldup(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vmovsldup_rr(src.fpu(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmovsldup_mr(src.disp(), src.base(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmovshdup(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        masm.vmovshdup_rr(src.code(), dest.code());
    }
    void vmovshdup(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        switch (src.kind()) {
          case Operand::FPREG:
            masm.vmovshdup_rr(src.fpu(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmovshdup_mr(src.disp(), src.base(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vminsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vminsd_rr(src1.code(), src0.code(), dest.code());
    }
    void vminsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vminsd_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vminsd_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vminss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vminss_rr(src1.code(), src0.code(), dest.code());
    }
    void vmaxsd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmaxsd_rr(src1.code(), src0.code(), dest.code());
    }
    void vmaxsd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src1.kind()) {
          case Operand::FPREG:
            masm.vmaxsd_rr(src1.fpu(), src0.code(), dest.code());
            break;
          case Operand::MEM_REG_DISP:
            masm.vmaxsd_mr(src1.disp(), src1.base(), src0.code(), dest.code());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void vmaxss(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmaxss_rr(src1.code(), src0.code(), dest.code());
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
    uint32_t actualOffset(uint32_t x) {
        return x;
    }

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

#endif /* jit_shared_Assembler_x86_shared_h */
