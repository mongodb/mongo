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
#include "jit/x86-shared/Constants-x86-shared.h"

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

static MOZ_CONSTEXPR_VAR FloatRegister xmm0 = FloatRegister(X86Encoding::xmm0, FloatRegisters::Double);
static MOZ_CONSTEXPR_VAR FloatRegister xmm1 = FloatRegister(X86Encoding::xmm1, FloatRegisters::Double);
static MOZ_CONSTEXPR_VAR FloatRegister xmm2 = FloatRegister(X86Encoding::xmm2, FloatRegisters::Double);
static MOZ_CONSTEXPR_VAR FloatRegister xmm3 = FloatRegister(X86Encoding::xmm3, FloatRegisters::Double);
static MOZ_CONSTEXPR_VAR FloatRegister xmm4 = FloatRegister(X86Encoding::xmm4, FloatRegisters::Double);
static MOZ_CONSTEXPR_VAR FloatRegister xmm5 = FloatRegister(X86Encoding::xmm5, FloatRegisters::Double);
static MOZ_CONSTEXPR_VAR FloatRegister xmm6 = FloatRegister(X86Encoding::xmm6, FloatRegisters::Double);
static MOZ_CONSTEXPR_VAR FloatRegister xmm7 = FloatRegister(X86Encoding::xmm7, FloatRegisters::Double);

static MOZ_CONSTEXPR_VAR Register InvalidReg = { X86Encoding::invalid_reg };
static MOZ_CONSTEXPR_VAR FloatRegister InvalidFloatReg = FloatRegister();

static MOZ_CONSTEXPR_VAR Register JSReturnReg_Type = ecx;
static MOZ_CONSTEXPR_VAR Register JSReturnReg_Data = edx;
static MOZ_CONSTEXPR_VAR Register StackPointer = esp;
static MOZ_CONSTEXPR_VAR Register FramePointer = ebp;
static MOZ_CONSTEXPR_VAR Register ReturnReg = eax;
static MOZ_CONSTEXPR_VAR FloatRegister ReturnFloat32Reg = FloatRegister(X86Encoding::xmm0, FloatRegisters::Single);
static MOZ_CONSTEXPR_VAR FloatRegister ReturnDoubleReg = FloatRegister(X86Encoding::xmm0, FloatRegisters::Double);
static MOZ_CONSTEXPR_VAR FloatRegister ReturnSimd128Reg = FloatRegister(X86Encoding::xmm0, FloatRegisters::Simd128);
static MOZ_CONSTEXPR_VAR FloatRegister ScratchFloat32Reg = FloatRegister(X86Encoding::xmm7, FloatRegisters::Single);
static MOZ_CONSTEXPR_VAR FloatRegister ScratchDoubleReg = FloatRegister(X86Encoding::xmm7, FloatRegisters::Double);
static MOZ_CONSTEXPR_VAR FloatRegister ScratchSimd128Reg = FloatRegister(X86Encoding::xmm7, FloatRegisters::Simd128);

// Avoid ebp, which is the FramePointer, which is unavailable in some modes.
static MOZ_CONSTEXPR_VAR Register ArgumentsRectifierReg = esi;
static MOZ_CONSTEXPR_VAR Register CallTempReg0 = edi;
static MOZ_CONSTEXPR_VAR Register CallTempReg1 = eax;
static MOZ_CONSTEXPR_VAR Register CallTempReg2 = ebx;
static MOZ_CONSTEXPR_VAR Register CallTempReg3 = ecx;
static MOZ_CONSTEXPR_VAR Register CallTempReg4 = esi;
static MOZ_CONSTEXPR_VAR Register CallTempReg5 = edx;

// We have no arg regs, so our NonArgRegs are just our CallTempReg*
// Use "const" instead of MOZ_CONSTEXPR_VAR here to work around a bug
// of VS2015 Update 1. See bug 1229604.
static const Register CallTempNonArgRegs[] = { edi, eax, ebx, ecx, esi, edx };
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
static MOZ_CONSTEXPR_VAR uint32_t ABIStackAlignment = 16;
#else
static MOZ_CONSTEXPR_VAR uint32_t ABIStackAlignment = 4;
#endif
static MOZ_CONSTEXPR_VAR uint32_t CodeAlignment = 16;
static MOZ_CONSTEXPR_VAR uint32_t JitStackAlignment = 16;

static MOZ_CONSTEXPR_VAR uint32_t JitStackValueAlignment = JitStackAlignment / sizeof(Value);
static_assert(JitStackAlignment % sizeof(Value) == 0 && JitStackValueAlignment >= 1,
  "Stack alignment should be a non-zero multiple of sizeof(Value)");

// This boolean indicates whether we support SIMD instructions flavoured for
// this architecture or not. Rather than a method in the LIRGenerator, it is
// here such that it is accessible from the entire codebase. Once full support
// for SIMD is reached on all tier-1 platforms, this constant can be deleted.
static MOZ_CONSTEXPR_VAR bool SupportsSimd = true;
static MOZ_CONSTEXPR_VAR uint32_t SimdMemoryAlignment = 16;

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

#include "jit/x86-shared/Assembler-x86-shared.h"

namespace js {
namespace jit {

static inline void
PatchJump(CodeLocationJump jump, CodeLocationLabel label, ReprotectCode reprotect = DontReprotect)
{
#ifdef DEBUG
    // Assert that we're overwriting a jump instruction, either:
    //   0F 80+cc <imm32>, or
    //   E9 <imm32>
    unsigned char* x = (unsigned char*)jump.raw() - 5;
    MOZ_ASSERT(((*x >= 0x80 && *x <= 0x8F) && *(x - 1) == 0x0F) ||
               (*x == 0xE9));
#endif
    MaybeAutoWritableJitCode awjc(jump.raw() - 8, 8, reprotect);
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

    CodeOffset pushWithPatch(ImmWord word) {
        masm.push_i32(int32_t(word.value));
        return CodeOffset(masm.currentOffset());
    }

    void pop(FloatRegister src) {
        vmovsd(Address(StackPointer, 0), src);
        addl(Imm32(sizeof(double)), StackPointer);
    }

    CodeOffset movWithPatch(ImmWord word, Register dest) {
        movl(Imm32(word.value), dest);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movWithPatch(ImmPtr imm, Register dest) {
        return movWithPatch(ImmWord(uintptr_t(imm.value)), dest);
    }

    void movl(ImmGCPtr ptr, Register dest) {
        masm.movl_i32r(uintptr_t(ptr.value), dest.encoding());
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
        masm.movl_i32r(imm.value, dest.encoding());
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
    void mov(wasm::SymbolicAddress imm, Register dest) {
        masm.movl_i32r(-1, dest.encoding());
        append(AsmJSAbsoluteLink(CodeOffset(masm.currentOffset()), imm));
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
    void mov(CodeOffset* label, Register dest) {
        // Put a placeholder value in the instruction stream.
        masm.movl_i32r(0, dest.encoding());
        label->bind(masm.size());
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
        masm.cmpl_ir(rhs.value, lhs.encoding());
    }
    void cmpl(ImmPtr rhs, Register lhs) {
        cmpl(ImmWord(uintptr_t(rhs.value)), lhs);
    }
    void cmpl(ImmGCPtr rhs, Register lhs) {
        masm.cmpl_i32r(uintptr_t(rhs.value), lhs.encoding());
        writeDataRelocation(rhs);
    }
    void cmpl(Register rhs, Register lhs) {
        masm.cmpl_rr(rhs.encoding(), lhs.encoding());
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
    void cmpl(Register rhs, wasm::SymbolicAddress lhs) {
        masm.cmpl_rm_disp32(rhs.encoding(), (void*)-1);
        append(AsmJSAbsoluteLink(CodeOffset(masm.currentOffset()), lhs));
    }
    void cmpl(Imm32 rhs, wasm::SymbolicAddress lhs) {
        JmpSrc src = masm.cmpl_im_disp32(rhs.value, (void*)-1);
        append(AsmJSAbsoluteLink(CodeOffset(src.offset()), lhs));
    }

    void adcl(Imm32 imm, Register dest) {
        masm.adcl_ir(imm.value, dest.encoding());
    }
    void adcl(Register src, Register dest) {
        masm.adcl_rr(src.encoding(), dest.encoding());
    }

    void mull(Register multiplier) {
        masm.mull_r(multiplier.encoding());
    }

    void shldl(const Imm32 imm, Register src, Register dest) {
        masm.shldl_irr(imm.value, src.encoding(), dest.encoding());
    }
    void shrdl(const Imm32 imm, Register src, Register dest) {
        masm.shrdl_irr(imm.value, src.encoding(), dest.encoding());
    }

    void vhaddpd(FloatRegister src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE3());
        MOZ_ASSERT(src.size() == 16);
        MOZ_ASSERT(dest.size() == 16);
        masm.vhaddpd_rr(src.encoding(), dest.encoding());
    }
    void vsubpd(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        MOZ_ASSERT(src0.size() == 16);
        MOZ_ASSERT(dest.size() == 16);
        switch (src1.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vsubpd_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vsubpd_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    void vpunpckldq(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        MOZ_ASSERT(src0.size() == 16);
        MOZ_ASSERT(src1.size() == 16);
        MOZ_ASSERT(dest.size() == 16);
        masm.vpunpckldq_rr(src1.encoding(), src0.encoding(), dest.encoding());
    }
    void vpunpckldq(const Operand& src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        MOZ_ASSERT(src0.size() == 16);
        MOZ_ASSERT(dest.size() == 16);
        switch (src1.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vpunpckldq_mr(src1.disp(), src1.base(), src0.encoding(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vpunpckldq_mr(src1.address(), src0.encoding(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
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
    CodeOffset toggledCall(JitCode* target, bool enabled) {
        CodeOffset offset(size());
        JmpSrc src = enabled ? masm.call() : masm.cmp_eax();
        addPendingJump(src, ImmPtr(target->raw()), Relocation::JITCODE);
        MOZ_ASSERT_IF(!oom(), size() - offset.offset() == ToggledCallSize(nullptr));
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
    CodeOffset movlWithPatch(Imm32 imm, Register dest) {
        masm.movl_i32r(imm.value, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }

    // Load from *(base + disp32) where disp32 can be patched.
    CodeOffset movsblWithPatch(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movsbl_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movsbl_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movzblWithPatch(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movzbl_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movzbl_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movswlWithPatch(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movswl_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movswl_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movzwlWithPatch(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movzwl_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movzwl_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movlWithPatch(const Operand& src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movl_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movl_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovssWithPatch(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovss_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovss_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdWithPatch(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovd_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovd_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovqWithPatch(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovq_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovq_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovsdWithPatch(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovsd_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovsd_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovupsWithPatch(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovups_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovups_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdquWithPatch(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovdqu_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovdqu_mr(src.address(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }

    // Store to *(base + disp32) where disp32 can be patched.
    CodeOffset movbWithPatch(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movb_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movb_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movwWithPatch(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movw_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movw_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movlWithPatch(Register src, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.movl_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.movl_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdWithPatch(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovd_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovd_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovqWithPatch(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovq_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovq_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovssWithPatch(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovss_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovss_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovsdWithPatch(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovsd_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovsd_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovupsWithPatch(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovups_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovups_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdquWithPatch(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovdqu_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovdqu_rm(src.encoding(), dest.address());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
        return CodeOffset(masm.currentOffset());
    }

    // Load from *(addr + index*scale) where addr can be patched.
    CodeOffset movlWithPatch(PatchedAbsoluteAddress addr, Register index, Scale scale,
                                  Register dest)
    {
        masm.movl_mr(addr.addr, index.encoding(), scale, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }

    // Load from *src where src can be patched.
    CodeOffset movsblWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movsbl_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movzblWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movzbl_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movswlWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movswl_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movzwlWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movzwl_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movlWithPatch(PatchedAbsoluteAddress src, Register dest) {
        masm.movl_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovssWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovss_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovqWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovq_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovsdWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovsd_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdqaWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqa_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdquWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqu_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovapsWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovaps_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovupsWithPatch(PatchedAbsoluteAddress src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovups_mr(src.addr, dest.encoding());
        return CodeOffset(masm.currentOffset());
    }

    // Store to *dest where dest can be patched.
    CodeOffset movbWithPatch(Register src, PatchedAbsoluteAddress dest) {
        masm.movb_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movwWithPatch(Register src, PatchedAbsoluteAddress dest) {
        masm.movw_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset movlWithPatch(Register src, PatchedAbsoluteAddress dest) {
        masm.movl_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovssWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovss_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovd_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovqWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovq_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovsdWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovsd_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdqaWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqa_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovapsWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovaps_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovdquWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovdqu_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }
    CodeOffset vmovupsWithPatch(FloatRegister src, PatchedAbsoluteAddress dest) {
        MOZ_ASSERT(HasSSE2());
        masm.vmovups_rm(src.encoding(), dest.addr);
        return CodeOffset(masm.currentOffset());
    }

    void loadAsmJSActivation(Register dest) {
        CodeOffset label = movlWithPatch(PatchedAbsoluteAddress(), dest);
        append(AsmJSGlobalAccess(label, wasm::ActivationGlobalDataOffset));
    }
    void loadAsmJSHeapRegisterFromGlobalData() {
        // x86 doesn't have a pinned heap register.
    }

    static bool canUseInSingleByteInstruction(Register reg) {
        return X86Encoding::HasSubregL(reg.encoding());
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
