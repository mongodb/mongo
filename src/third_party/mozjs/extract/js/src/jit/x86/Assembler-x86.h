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

static constexpr Register eax { X86Encoding::rax };
static constexpr Register ecx { X86Encoding::rcx };
static constexpr Register edx { X86Encoding::rdx };
static constexpr Register ebx { X86Encoding::rbx };
static constexpr Register esp { X86Encoding::rsp };
static constexpr Register ebp { X86Encoding::rbp };
static constexpr Register esi { X86Encoding::rsi };
static constexpr Register edi { X86Encoding::rdi };

static constexpr FloatRegister xmm0 = FloatRegister(X86Encoding::xmm0, FloatRegisters::Double);
static constexpr FloatRegister xmm1 = FloatRegister(X86Encoding::xmm1, FloatRegisters::Double);
static constexpr FloatRegister xmm2 = FloatRegister(X86Encoding::xmm2, FloatRegisters::Double);
static constexpr FloatRegister xmm3 = FloatRegister(X86Encoding::xmm3, FloatRegisters::Double);
static constexpr FloatRegister xmm4 = FloatRegister(X86Encoding::xmm4, FloatRegisters::Double);
static constexpr FloatRegister xmm5 = FloatRegister(X86Encoding::xmm5, FloatRegisters::Double);
static constexpr FloatRegister xmm6 = FloatRegister(X86Encoding::xmm6, FloatRegisters::Double);
static constexpr FloatRegister xmm7 = FloatRegister(X86Encoding::xmm7, FloatRegisters::Double);

static constexpr Register InvalidReg { X86Encoding::invalid_reg };
static constexpr FloatRegister InvalidFloatReg = FloatRegister();

static constexpr Register JSReturnReg_Type = ecx;
static constexpr Register JSReturnReg_Data = edx;
static constexpr Register StackPointer = esp;
static constexpr Register FramePointer = ebp;
static constexpr Register ReturnReg = eax;
static constexpr Register64 ReturnReg64(edi, eax);
static constexpr FloatRegister ReturnFloat32Reg = FloatRegister(X86Encoding::xmm0, FloatRegisters::Single);
static constexpr FloatRegister ReturnDoubleReg = FloatRegister(X86Encoding::xmm0, FloatRegisters::Double);
static constexpr FloatRegister ReturnSimd128Reg = FloatRegister(X86Encoding::xmm0, FloatRegisters::Simd128);
static constexpr FloatRegister ScratchFloat32Reg = FloatRegister(X86Encoding::xmm7, FloatRegisters::Single);
static constexpr FloatRegister ScratchDoubleReg = FloatRegister(X86Encoding::xmm7, FloatRegisters::Double);
static constexpr FloatRegister ScratchSimd128Reg = FloatRegister(X86Encoding::xmm7, FloatRegisters::Simd128);

// Avoid ebp, which is the FramePointer, which is unavailable in some modes.
static constexpr Register CallTempReg0 = edi;
static constexpr Register CallTempReg1 = eax;
static constexpr Register CallTempReg2 = ebx;
static constexpr Register CallTempReg3 = ecx;
static constexpr Register CallTempReg4 = esi;
static constexpr Register CallTempReg5 = edx;

// We have no arg regs, so our NonArgRegs are just our CallTempReg*
// Use "const" instead of constexpr here to work around a bug
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
};

// These registers may be volatile or nonvolatile.
static constexpr Register ABINonArgReg0 = eax;
static constexpr Register ABINonArgReg1 = ebx;
static constexpr Register ABINonArgReg2 = ecx;

// This register may be volatile or nonvolatile. Avoid xmm7 which is the
// ScratchDoubleReg.
static constexpr FloatRegister ABINonArgDoubleReg = FloatRegister(X86Encoding::xmm0, FloatRegisters::Double);

// These registers may be volatile or nonvolatile.
// Note: these three registers are all guaranteed to be different
static constexpr Register ABINonArgReturnReg0 = ecx;
static constexpr Register ABINonArgReturnReg1 = edx;
static constexpr Register ABINonVolatileReg = ebx;

// This register is guaranteed to be clobberable during the prologue and
// epilogue of an ABI call which must preserve both ABI argument, return
// and non-volatile registers.
static constexpr Register ABINonArgReturnVolatileReg = ecx;

// TLS pointer argument register for WebAssembly functions. This must not alias
// any other register used for passing function arguments or return values.
// Preserved by WebAssembly functions.
static constexpr Register WasmTlsReg = esi;

// Registers used for asm.js/wasm table calls. These registers must be disjoint
// from the ABI argument registers, WasmTlsReg and each other.
static constexpr Register WasmTableCallScratchReg = ABINonArgReg0;
static constexpr Register WasmTableCallSigReg = ABINonArgReg1;
static constexpr Register WasmTableCallIndexReg = ABINonArgReg2;

static constexpr Register OsrFrameReg = edx;
static constexpr Register PreBarrierReg = edx;

// Registers used in the GenerateFFIIonExit Enable Activation block.
static constexpr Register WasmIonExitRegCallee = ecx;
static constexpr Register WasmIonExitRegE0 = edi;
static constexpr Register WasmIonExitRegE1 = eax;

// Registers used in the GenerateFFIIonExit Disable Activation block.
static constexpr Register WasmIonExitRegReturnData = edx;
static constexpr Register WasmIonExitRegReturnType = ecx;
static constexpr Register WasmIonExitTlsReg = esi;
static constexpr Register WasmIonExitRegD0 = edi;
static constexpr Register WasmIonExitRegD1 = eax;
static constexpr Register WasmIonExitRegD2 = ebx;

// Registerd used in RegExpMatcher instruction (do not use JSReturnOperand).
static constexpr Register RegExpMatcherRegExpReg = CallTempReg0;
static constexpr Register RegExpMatcherStringReg = CallTempReg1;
static constexpr Register RegExpMatcherLastIndexReg = CallTempReg2;

// Registerd used in RegExpTester instruction (do not use ReturnReg).
static constexpr Register RegExpTesterRegExpReg = CallTempReg0;
static constexpr Register RegExpTesterStringReg = CallTempReg2;
static constexpr Register RegExpTesterLastIndexReg = CallTempReg3;

// GCC stack is aligned on 16 bytes. Ion does not maintain this for internal
// calls. wasm code does.
#if defined(__GNUC__) && !defined(__MINGW32__)
static constexpr uint32_t ABIStackAlignment = 16;
#else
static constexpr uint32_t ABIStackAlignment = 4;
#endif
static constexpr uint32_t CodeAlignment = 16;
static constexpr uint32_t JitStackAlignment = 16;

static constexpr uint32_t JitStackValueAlignment = JitStackAlignment / sizeof(Value);
static_assert(JitStackAlignment % sizeof(Value) == 0 && JitStackValueAlignment >= 1,
  "Stack alignment should be a non-zero multiple of sizeof(Value)");

// This boolean indicates whether we support SIMD instructions flavoured for
// this architecture or not. Rather than a method in the LIRGenerator, it is
// here such that it is accessible from the entire codebase. Once full support
// for SIMD is reached on all tier-1 platforms, this constant can be deleted.
static constexpr bool SupportsSimd = true;
static constexpr uint32_t SimdMemoryAlignment = 16;

static_assert(CodeAlignment % SimdMemoryAlignment == 0,
  "Code alignment should be larger than any of the alignments which are used for "
  "the constant sections of the code buffer.  Thus it should be larger than the "
  "alignment for SIMD constants.");

static_assert(JitStackAlignment % SimdMemoryAlignment == 0,
  "Stack alignment should be larger than any of the alignments which are used for "
  "spilled values.  Thus it should be larger than the alignment for SIMD accesses.");

static const uint32_t WasmStackAlignment = SimdMemoryAlignment;

struct ImmTag : public Imm32
{
    explicit ImmTag(JSValueTag mask)
      : Imm32(int32_t(mask))
    { }
};

struct ImmType : public ImmTag
{
    explicit ImmType(JSValueType type)
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
PatchBackedge(CodeLocationJump& jump_, CodeLocationLabel label, JitZoneGroup::BackedgeTarget target)
{
    PatchJump(jump_, label);
}

static inline Operand
LowWord(const Operand& op) {
    switch (op.kind()) {
      case Operand::MEM_REG_DISP: return Operand(LowWord(op.toAddress()));
      case Operand::MEM_SCALE:    return Operand(LowWord(op.toBaseIndex()));
      default:                    MOZ_CRASH("Invalid operand type");
    }
}

static inline Operand
HighWord(const Operand& op) {
    switch (op.kind()) {
      case Operand::MEM_REG_DISP: return Operand(HighWord(op.toAddress()));
      case Operand::MEM_SCALE:    return Operand(HighWord(op.toBaseIndex()));
      default:                    MOZ_CRASH("Invalid operand type");
    }
}

// Return operand from a JS -> JS call.
static constexpr ValueOperand JSReturnOperand{JSReturnReg_Type, JSReturnReg_Data};

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
    void executableCopy(uint8_t* buffer, bool flushICache = true);

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
        append(wasm::SymbolicAccess(CodeOffset(masm.currentOffset()), imm));
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
    void mov(CodeLabel* label, Register dest) {
        // Put a placeholder value in the instruction stream.
        masm.movl_i32r(0, dest.encoding());
        label->patchAt()->bind(masm.size());
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

    void fstp32(const Operand& src) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.fstp32_m(src.disp(), src.base());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    void faddp() {
        masm.faddp();
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
        append(wasm::SymbolicAccess(CodeOffset(masm.currentOffset()), lhs));
    }
    void cmpl(Imm32 rhs, wasm::SymbolicAddress lhs) {
        JmpSrc src = masm.cmpl_im_disp32(rhs.value, (void*)-1);
        append(wasm::SymbolicAccess(CodeOffset(src.offset()), lhs));
    }

    void adcl(Imm32 imm, Register dest) {
        masm.adcl_ir(imm.value, dest.encoding());
    }
    void adcl(Register src, Register dest) {
        masm.adcl_rr(src.encoding(), dest.encoding());
    }
    void adcl(Operand src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.adcl_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.adcl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }

    void sbbl(Imm32 imm, Register dest) {
        masm.sbbl_ir(imm.value, dest.encoding());
    }
    void sbbl(Register src, Register dest) {
        masm.sbbl_rr(src.encoding(), dest.encoding());
    }
    void sbbl(Operand src, Register dest) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.sbbl_mr(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.sbbl_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
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
    void vsubpd(FloatRegister src1, FloatRegister src0, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        MOZ_ASSERT(src0.size() == 16);
        MOZ_ASSERT(dest.size() == 16);
        masm.vsubpd_rr(src1.encoding(), src0.encoding(), dest.encoding());
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

    void fild(const Operand& src) {
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.fild_m(src.disp(), src.base());
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
    void vmovss(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovss_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovss_mr(src.address(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vmovss_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
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
    void vmovsd(const Operand& src, FloatRegister dest) {
        MOZ_ASSERT(HasSSE2());
        switch (src.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovsd_mr_disp32(src.disp(), src.base(), dest.encoding());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovsd_mr(src.address(), dest.encoding());
            break;
          case Operand::MEM_SCALE:
            masm.vmovsd_mr(src.disp(), src.base(), src.index(), src.scale(), dest.encoding());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
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
    CodeOffset movlWithPatchLow(Register regLow, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP: {
            return movlWithPatch(regLow, LowWord(dest));
          }
          case Operand::MEM_ADDRESS32: {
            Operand low(PatchedAbsoluteAddress(uint32_t(dest.address()) + INT64LOW_OFFSET));
            return movlWithPatch(regLow, low);
          }
          default:
            MOZ_CRASH("unexpected operand kind");
        }
    }
    CodeOffset movlWithPatchHigh(Register regHigh, const Operand& dest) {
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP: {
            return movlWithPatch(regHigh, HighWord(dest));
          }
          case Operand::MEM_ADDRESS32: {
            Operand high(PatchedAbsoluteAddress(uint32_t(dest.address()) + INT64HIGH_OFFSET));
            return movlWithPatch(regHigh, high);
          }
          default:
            MOZ_CRASH("unexpected operand kind");
        }
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
    void vmovss(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovss_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovss_rm(src.encoding(), dest.address());
            break;
          case Operand::MEM_SCALE:
            masm.vmovss_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
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
    void vmovsd(FloatRegister src, const Operand& dest) {
        MOZ_ASSERT(HasSSE2());
        switch (dest.kind()) {
          case Operand::MEM_REG_DISP:
            masm.vmovsd_rm_disp32(src.encoding(), dest.disp(), dest.base());
            break;
          case Operand::MEM_ADDRESS32:
            masm.vmovsd_rm(src.encoding(), dest.address());
            break;
          case Operand::MEM_SCALE:
            masm.vmovsd_rm(src.encoding(), dest.disp(), dest.base(), dest.index(), dest.scale());
            break;
          default:
            MOZ_CRASH("unexpected operand kind");
        }
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
