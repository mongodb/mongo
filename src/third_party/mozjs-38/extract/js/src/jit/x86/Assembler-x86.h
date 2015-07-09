/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_Assembler_x86_h
#define jit_x86_Assembler_x86_h

#include "mozilla/ArrayUtils.h"

#include "jit/CompactBuffer.h"
#include "jit/IonCode.h"
#include "jit/JitCompartment.h"
#include "jit/shared/Assembler-shared.h"
#include "jit/shared/Constants-x86-shared.h"

namespace js {
namespace jit {

static MOZ_CONSTEXPR_VAR Register eax = { X86Encoding::rax };
static MOZ_CONSTEXPR_VAR Register ecx = { X86Encoding::rcx };
static MOZ_CONSTEXPR_VAR Register edx = { X86Encoding::rdx };
static MOZ_CONSTEXPR_VAR Register ebx = { X86Encoding::rbx };
static MOZ_CONSTEXPR_VAR Register esp = { X86Encoding::rsp };
static MOZ_CONSTEXPR_VAR Register ebp = { X86Encoding::rbp };
static MOZ_CONSTEXPR_VAR Register esi = { X86Encoding::rsi };
static MOZ_CONSTEXPR_VAR Register edi = { X86Encoding::rdi };

static MOZ_CONSTEXPR_VAR FloatRegister xmm0 = { X86Encoding::xmm0 };
static MOZ_CONSTEXPR_VAR FloatRegister xmm1 = { X86Encoding::xmm1 };
static MOZ_CONSTEXPR_VAR FloatRegister xmm2 = { X86Encoding::xmm2 };
static MOZ_CONSTEXPR_VAR FloatRegister xmm3 = { X86Encoding::xmm3 };
static MOZ_CONSTEXPR_VAR FloatRegister xmm4 = { X86Encoding::xmm4 };
static MOZ_CONSTEXPR_VAR FloatRegister xmm5 = { X86Encoding::xmm5 };
static MOZ_CONSTEXPR_VAR FloatRegister xmm6 = { X86Encoding::xmm6 };
static MOZ_CONSTEXPR_VAR FloatRegister xmm7 = { X86Encoding::xmm7 };

static MOZ_CONSTEXPR_VAR Register InvalidReg = { X86Encoding::invalid_reg };
static MOZ_CONSTEXPR_VAR FloatRegister InvalidFloatReg = { X86Encoding::invalid_xmm };

static MOZ_CONSTEXPR_VAR Register JSReturnReg_Type = ecx;
static MOZ_CONSTEXPR_VAR Register JSReturnReg_Data = edx;
static MOZ_CONSTEXPR_VAR Register StackPointer = esp;
static MOZ_CONSTEXPR_VAR Register FramePointer = ebp;
static MOZ_CONSTEXPR_VAR Register ReturnReg = eax;
static MOZ_CONSTEXPR_VAR FloatRegister ReturnFloat32Reg = xmm0;
static MOZ_CONSTEXPR_VAR FloatRegister ScratchFloat32Reg = xmm7;
static MOZ_CONSTEXPR_VAR FloatRegister ReturnDoubleReg = xmm0;
static MOZ_CONSTEXPR_VAR FloatRegister ScratchDoubleReg = xmm7;
static MOZ_CONSTEXPR_VAR FloatRegister ReturnSimdReg = xmm0;
static MOZ_CONSTEXPR_VAR FloatRegister ScratchSimdReg = xmm7;

// Avoid ebp, which is the FramePointer, which is unavailable in some modes.
static MOZ_CONSTEXPR_VAR Register ArgumentsRectifierReg = esi;
static MOZ_CONSTEXPR_VAR Register CallTempReg0 = edi;
static MOZ_CONSTEXPR_VAR Register CallTempReg1 = eax;
static MOZ_CONSTEXPR_VAR Register CallTempReg2 = ebx;
static MOZ_CONSTEXPR_VAR Register CallTempReg3 = ecx;
static MOZ_CONSTEXPR_VAR Register CallTempReg4 = esi;
static MOZ_CONSTEXPR_VAR Register CallTempReg5 = edx;

// We have no arg regs, so our NonArgRegs are just our CallTempReg*
static MOZ_CONSTEXPR_VAR Register CallTempNonArgRegs[] = { edi, eax, ebx, ecx, esi, edx };
static const uint32_t NumCallTempNonArgRegs =
    mozilla::ArrayLength(CallTempNonArgRegs);

class ABIArgGenerator
{
    uint32_t stackOffset_;
    ABIArg current_;

  public:
    ABIArgGenerator();
    ABIArg next(MIRType argType);
    ABIArg& current() { return current_; }
    uint32_t stackBytesConsumedSoFar() const { return stackOffset_; }

    // Note: these registers are all guaranteed to be different
    static const Register NonArgReturnReg0;
    static const Register NonArgReturnReg1;
    static const Register NonVolatileReg;
    static const Register NonArg_VolatileReg;
    static const Register NonReturn_VolatileReg0;
};

static MOZ_CONSTEXPR_VAR Register OsrFrameReg = edx;
static MOZ_CONSTEXPR_VAR Register PreBarrierReg = edx;

// Registers used in the GenerateFFIIonExit Enable Activation block.
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegCallee = ecx;
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegE0 = edi;
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegE1 = eax;
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegE2 = ebx;
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegE3 = edx;

// Registers used in the GenerateFFIIonExit Disable Activation block.
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegReturnData = edx;
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegReturnType = ecx;
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegD0 = edi;
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegD1 = eax;
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegD2 = esi;

// GCC stack is aligned on 16 bytes. Ion does not maintain this for internal
// calls. asm.js code does.
#if defined(__GNUC__)
static const uint32_t ABIStackAlignment = 16;
#else
static const uint32_t ABIStackAlignment = 4;
#endif
static const uint32_t CodeAlignment = 16;
static const uint32_t JitStackAlignment = 16;

// This boolean indicates whether we support SIMD instructions flavoured for
// this architecture or not. Rather than a method in the LIRGenerator, it is
// here such that it is accessible from the entire codebase. Once full support
// for SIMD is reached on all tier-1 platforms, this constant can be deleted.
static const bool SupportsSimd = true;
static const uint32_t SimdMemoryAlignment = 16;

static_assert(CodeAlignment % SimdMemoryAlignment == 0,
  "Code alignment should be larger than any of the alignments which are used for "
  "the constant sections of the code buffer.  Thus it should be larger than the "
  "alignment for SIMD constants.");

static_assert(JitStackAlignment % SimdMemoryAlignment == 0,
  "Stack alignment should be larger than any of the alignments which are used for "
  "spilled values.  Thus it should be larger than the alignment for SIMD accesses.");

static const uint32_t AsmJSStackAlignment = SimdMemoryAlignment;

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

static const Scale ScalePointer = TimesFour;

} // namespace jit
} // namespace js

#include "jit/shared/Assembler-x86-shared.h"

namespace js {
namespace jit {

static inline void
PatchJump(CodeLocationJump jump, CodeLocationLabel label)
{
#ifdef DEBUG
    // Assert that we're overwriting a jump instruction, either:
    //   0F 80+cc <imm32>, or
    //   E9 <imm32>
    unsigned char* x = (unsigned char*)jump.raw() - 5;
    MOZ_ASSERT(((*x >= 0x80 && *x <= 0x8F) && *(x - 1) == 0x0F) ||
               (*x == 0xE9));
#endif
    X86Encoding::SetRel32(jump.raw(), label.raw());
}
static inline void
PatchBackedge(CodeLocationJump& jump_, CodeLocationLabel label, JitRuntime::BackedgeTarget target)
{
    PatchJump(jump_, label);
}

// Return operand from a JS -> JS call.
static const ValueOperand JSReturnOperand = ValueOperand(JSReturnReg_Type, JSReturnReg_Data);

class Assembler : public AssemblerX86Shared
{
    void writeRelocation(JmpSrc src) {
        jumpRelocations_.writeUnsigned(src.offset());
    }
    void addPendingJump(JmpSrc src, ImmPtr target, Relocation::Kind kind) {
        enoughMemory_ &= jumps_.append(RelativePatch(src.offset(), target.value, kind));
        if (kind == Relocation::JITCODE)
            writeRelocation(src);
    }

  public:
    using AssemblerX86Shared::movl;
    using AssemblerX86Shared::j;
    using AssemblerX86Shared::jmp;
    using AssemblerX86Shared::vmovsd;
    using AssemblerX86Shared::vmovss;
    using AssemblerX86Shared::retarget;
    using AssemblerX86Shared::cmpl;
    using AssemblerX86Shared::call;
    using AssemblerX86Shared::push;
    using AssemblerX86Shared::pop;

    static void TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);

    // Copy the assembly code to the given buffer, and perform any pending
    // relocations relying on the target address.
    void executableCopy(uint8_t* buffer);

    // Actual assembly emitting functions.

    void push(ImmGCPtr ptr) {
        masm.push_i32(int32_t(ptr.value));
        writeDataRelocation(ptr);
    }
    void push(ImmMaybeNurseryPtr ptr) {
        push(noteMaybeNurseryPtr(ptr));
    }
    void push(const ImmWord imm) {
        push(Imm32(imm.value));
    }
    void push(const ImmPtr imm) {
        push(ImmWord(uintptr_t(imm.value)));
    }
    void push(FloatRegister src) {
        subl(Imm32(sizeof(double)), StackPointer);
        vmovsd(src, Address(StackPointer, 0));
    }

    CodeOffsetLabel pushWithPatch(ImmWord word) {
        masm.push_i32(int32_t(word.value));
        return CodeOffsetLabel(masm.currentOffset());
    }

    void pop(FloatRegister src) {
        vmovsd(Address(StackPointer, 0), src);
        addl(Imm32(sizeof(double)), StackPointer);
    }

    CodeOffsetLabel movWithPatch(ImmWord word, Register dest) {
        movl(Imm32(word.value), dest);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movWithPatch(ImmPtr imm, Register dest) {
        return movWithPatch(ImmWord(uintptr_t(imm.value)), dest);
    }

    void movl(ImmGCPtr ptr, Register dest) {
        masm.movl_i32r(uintptr_t(ptr.value), dest.code());
        writeDataRelocation(ptr);
    }
    void movl(ImmGCPtr ptr, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::REG:
            masm.movl_i32r(uintptr_t(ptr.value), dest.reg());
            writeDataRelocation(ptr);
            break;
          case Operand::MEM_REG_DISP:
            masm.movl_i32m(uintptr_t(ptr.value), dest.disp(), dest.base());
            writeDataRelocation(ptr);
            break;
          case Operand::MEM_SCALE:
            masm.movl_i32m(uintptr_t(ptr.value), dest.disp(), dest.base(), dest.index(), dest.scale());
            writeDataRelocation(ptr);
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void movl(ImmWord imm, Register dest) {
        masm.movl_i32r(imm.value, dest.code());
    }
    void movl(ImmPtr imm, Register dest) {
        movl(ImmWord(uintptr_t(imm.value)), dest);
    }
    void mov(ImmWord imm, Register dest) {
        // Use xor for setting registers to zero, as it is specially optimized
        // for this purpose on modern hardware. Note that it does clobber FLAGS
        // though.
        if (imm.value == 0)
            xorl(dest, dest);
        else
            movl(imm, dest);
    }
    void mov(ImmPtr imm, Register dest) {
        mov(ImmWord(uintptr_t(imm.value)), dest);
    }
    void mov(AsmJSImmPtr imm, Register dest) {
        masm.movl_i32r(-1, dest.code());
        append(AsmJSAbsoluteLink(CodeOffsetLabel(masm.currentOffset()), imm.kind()));
    }
    void mov(const Operand& src, Register dest) {
        movl(src, dest);
    }
    void mov(Register src, const Operand& dest) {
        movl(src, dest);
    }
    void mov(Imm32 imm, const Operand& dest) {
        movl(imm, dest);
    }
    void mov(AbsoluteLabel* label, Register dest) {
        MOZ_ASSERT(!label->bound());
        // Thread the patch list through the unpatched address word in the
        // instruction stream.
        masm.movl_i32r(label->prev(), dest.code());
        label->setPrev(masm.size());
    }
    void mov(Register src, Register dest) {
        movl(src, dest);
    }
    void xchg(Register src, Register dest) {
        xchgl(src, dest);
    }
    void lea(const Operand& src, Register dest) {
        return leal(src, dest);
    }

    void fld32(const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.fld32_m(dest.disp(), dest.base());
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

    void cmpl(ImmWord rhs, Register lhs) {
        masm.cmpl_ir(rhs.value, lhs.code());
    }
    void cmpl(ImmPtr rhs, Register lhs) {
        cmpl(ImmWord(uintptr_t(rhs.value)), lhs);
    }
    void cmpl(ImmGCPtr rhs, Register lhs) {
        masm.cmpl_i32r(uintptr_t(rhs.value), lhs.code());
        writeDataRelocation(rhs);
    }
    void cmpl(Register rhs, Register lhs) {
        masm.cmpl_rr(rhs.code(), lhs.code());
    }
    void cmpl(ImmGCPtr rhs, const Operand& lhs) {
        switch (lhs.kind()) {
          case Operand::REG:
            masm.cmpl_i32r(uintptr_t(rhs.value), lhs.reg());
            writeDataRelocation(rhs);
            break;
          case Operand::MEM_REG_DISP:
            masm.cmpl_i32m(uintptr_t(rhs.value), lhs.disp(), lhs.base());
            writeDataRelocation(rhs);
            break;
          case Operand::MEM_ADDRESS32:
            masm.cmpl_i32m(uintptr_t(rhs.value), lhs.address());
            writeDataRelocation(rhs);
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void cmpl(ImmMaybeNurseryPtr rhs, const Operand& lhs) {
        cmpl(noteMaybeNurseryPtr(rhs), lhs);
    }
    void cmpl(Register rhs, AsmJSAbsoluteAddress lhs) {
        masm.cmpl_rm_disp32(rhs.code(), (void*)-1);
        append(AsmJSAbsoluteLink(CodeOffsetLabel(masm.currentOffset()), lhs.kind()));
    }
    void cmpl(Imm32 rhs, AsmJSAbsoluteAddress lhs) {
        JmpSrc src = masm.cmpl_im_disp32(rhs.value, (void*)-1);
        append(AsmJSAbsoluteLink(CodeOffsetLabel(src.offset()), lhs.kind()));
    }

    void jmp(ImmPtr target, Relocation::Kind reloc = Relocation::HARDCODED) {
        JmpSrc src = masm.jmp();
        addPendingJump(src, target, reloc);
    }
    void j(Condition cond, ImmPtr target,
           Relocation::Kind reloc = Relocation::HARDCODED) {
        JmpSrc src = masm.jCC(static_cast<X86Encoding::Condition>(cond));
        addPendingJump(src, target, reloc);
    }

    void jmp(JitCode* target) {
        jmp(ImmPtr(target->raw()), Relocation::JITCODE);
    }
    void j(Condition cond, JitCode* target) {
        j(cond, ImmPtr(target->raw()), Relocation::JITCODE);
    }
    void call(JitCode* target) {
        JmpSrc src = masm.call();
        addPendingJump(src, ImmPtr(target->raw()), Relocation::JITCODE);
    }
    void call(ImmWord target) {
        call(ImmPtr((void*)target.value));
    }
    void call(ImmPtr target) {
        JmpSrc src = masm.call();
        addPendingJump(src, target, Relocation::HARDCODED);
    }

    // Emit a CALL or CMP (nop) instruction. ToggleCall can be used to patch
    // this instruction.
    CodeOffsetLabel toggledCall(JitCode* target, bool enabled) {
        CodeOffsetLabel offset(size());
        JmpSrc src = enabled ? masm.call() : masm.cmp_eax();
        addPendingJump(src, ImmPtr(target->raw()), Relocation::JITCODE);
        MOZ_ASSERT(size() - offset.offset() == ToggledCallSize(nullptr));
        return offset;
    }

    static size_t ToggledCallSize(uint8_t* code) {
        // Size of a call instruction.
        return 5;
    }

    // Re-routes pending jumps to an external target, flushing the label in the
    // process.
    void retarget(Label* label, ImmPtr target, Relocation::Kind reloc) {
        if (label->used()) {
            bool more;
            X86Encoding::JmpSrc jmp(label->offset());
            do {
                X86Encoding::JmpSrc next;
                more = masm.nextJump(jmp, &next);
                addPendingJump(jmp, target, reloc);
                jmp = next;
            } while (more);
        }
        label->reset();
    }

    // Move a 32-bit immediate into a register where the immediate can be
    // patched.
    CodeOffsetLabel movlWithPatch(Imm32 imm, Register dest) {
        masm.movl_i32r(imm.value, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }

    // Load from *(base + disp32) where disp32 can be patched.
    CodeOffsetLabel movsblWithPatch(Address src, Register dest) {
        masm.movsbl_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movzblWithPatch(Address src, Register dest) {
        masm.movzbl_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movswlWithPatch(Address src, Register dest) {
        masm.movswl_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movzwlWithPatch(Address src, Register dest) {
        masm.movzwl_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movlWithPatch(Address src, Register dest) {
        masm.movl_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovssWithPatch(Address src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovss_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdWithPatch(Address src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovqWithPatch(Address src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovq_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovsdWithPatch(Address src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovsd_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovupsWithPatch(Address src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovups_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdquWithPatch(Address src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqu_mr_disp32(src.offset, src.base.code(), dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }

    // Store to *(base + disp32) where disp32 can be patched.
    CodeOffsetLabel movbWithPatch(Register src, Address dest) {
        masm.movb_rm_disp32(src.code(), dest.offset, dest.base.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movwWithPatch(Register src, Address dest) {
        masm.movw_rm_disp32(src.code(), dest.offset, dest.base.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movlWithPatch(Register src, Address dest) {
        masm.movl_rm_disp32(src.code(), dest.offset, dest.base.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdWithPatch(FloatRegister src, Address dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_rm_disp32(src.code(), dest.offset, dest.base.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovqWithPatch(FloatRegister src, Address dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovq_rm_disp32(src.code(), dest.offset, dest.base.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovssWithPatch(FloatRegister src, Address dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovss_rm_disp32(src.code(), dest.offset, dest.base.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovsdWithPatch(FloatRegister src, Address dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovsd_rm_disp32(src.code(), dest.offset, dest.base.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovupsWithPatch(FloatRegister src, Address dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovups_rm_disp32(src.code(), dest.offset, dest.base.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdquWithPatch(FloatRegister src, Address dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqu_rm_disp32(src.code(), dest.offset, dest.base.code());
        return CodeOffsetLabel(masm.currentOffset());
    }

    // Load from *(addr + index*scale) where addr can be patched.
    CodeOffsetLabel movlWithPatch(PatchedAbsoluteAddress addr, Register index, Scale scale,
                                  Register dest)
    {
        masm.movl_mr(addr.addr, index.code(), scale, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }

    // Load from *src where src can be patched.
    CodeOffsetLabel movsblWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movsbl_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movzblWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movzbl_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movswlWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movswl_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movzwlWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movzwl_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movlWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movl_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovssWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovss_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovqWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovq_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovsdWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovsd_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdqaWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqa_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdquWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqu_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovapsWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovaps_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovupsWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovups_mr(src.addr, dest.code());
        return CodeOffsetLabel(masm.currentOffset());
    }

    // Store to *dest where dest can be patched.
    CodeOffsetLabel movbWithPatch(Register src, PatchedAbsoluteAddress dest) {
        masm.movb_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movwWithPatch(Register src, PatchedAbsoluteAddress dest) {
        masm.movw_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel movlWithPatch(Register src, PatchedAbsoluteAddress dest) {
        masm.movl_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovssWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovss_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovqWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovq_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovsdWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovsd_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdqaWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqa_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovapsWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovaps_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovdquWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqu_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }
    CodeOffsetLabel vmovupsWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovups_rm(src.code(), dest.addr);
        return CodeOffsetLabel(masm.currentOffset());
    }

    void loadAsmJSActivation(Register dest) {
        CodeOffsetLabel label = movlWithPatch(PatchedAbsoluteAddress(), dest);
        append(AsmJSGlobalAccess(label, AsmJSActivationGlobalDataOffset));
    }
    void loadAsmJSHeapRegisterFromGlobalData() {
        // x86 doesn't have a pinned heap register.
    }

    static bool canUseInSingleByteInstruction(Register reg) {
        return X86Encoding::HasSubregL(reg.code());
    }
};

// Get a register in which we plan to put a quantity that will be used as an
// integer argument.  This differs from GetIntArgReg in that if we have no more
// actual argument registers to use we will fall back on using whatever
// CallTempReg* don't overlap the argument registers, and only fail once those
// run out too.
static inline bool
GetTempRegForIntArg(uint32_t usedIntArgs, uint32_t usedFloatArgs, Register* out)
{
    if (usedIntArgs >= NumCallTempNonArgRegs)
        return false;
    *out = CallTempNonArgRegs[usedIntArgs];
    return true;
}

} // namespace jit
} // namespace js

#endif /* jit_x86_Assembler_x86_h */
