/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm64/MacroAssembler-arm64.h"

#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"

#include "jsmath.h"

#include "jit/arm64/MoveEmitter-arm64.h"
#include "jit/arm64/SharedICRegisters-arm64.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitRuntime.h"
#include "jit/MacroAssembler.h"
#include "util/Memory.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

enum class Width { _32 = 32, _64 = 64 };

static inline ARMRegister X(Register r) { return ARMRegister(r, 64); }

static inline ARMRegister X(MacroAssembler& masm, RegisterOrSP r) {
  return masm.toARMRegister(r, 64);
}

static inline ARMRegister W(Register r) { return ARMRegister(r, 32); }

static inline ARMRegister R(Register r, Width w) {
  return ARMRegister(r, unsigned(w));
}

void MacroAssemblerCompat::boxValue(JSValueType type, Register src,
                                    Register dest) {
#ifdef DEBUG
  if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
    Label upper32BitsZeroed;
    movePtr(ImmWord(UINT32_MAX), dest);
    asMasm().branchPtr(Assembler::BelowOrEqual, src, dest, &upper32BitsZeroed);
    breakpoint();
    bind(&upper32BitsZeroed);
  }
#endif
  Orr(ARMRegister(dest, 64), ARMRegister(src, 64),
      Operand(ImmShiftedTag(type).value));
}

void MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output) {
  ARMRegister dest(output, 32);
  Fcvtns(dest, ARMFPRegister(input, 64));

  {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();

    Mov(scratch32, Operand(0xff));
    Cmp(dest, scratch32);
    Csel(dest, dest, scratch32, LessThan);
  }

  Cmp(dest, Operand(0));
  Csel(dest, dest, wzr, GreaterThan);
}

js::jit::MacroAssembler& MacroAssemblerCompat::asMasm() {
  return *static_cast<js::jit::MacroAssembler*>(this);
}

const js::jit::MacroAssembler& MacroAssemblerCompat::asMasm() const {
  return *static_cast<const js::jit::MacroAssembler*>(this);
}

vixl::MacroAssembler& MacroAssemblerCompat::asVIXL() {
  return *static_cast<vixl::MacroAssembler*>(this);
}

const vixl::MacroAssembler& MacroAssemblerCompat::asVIXL() const {
  return *static_cast<const vixl::MacroAssembler*>(this);
}

void MacroAssemblerCompat::mov(CodeLabel* label, Register dest) {
  BufferOffset bo = movePatchablePtr(ImmWord(/* placeholder */ 0), dest);
  label->patchAt()->bind(bo.getOffset());
  label->setLinkMode(CodeLabel::MoveImmediate);
}

BufferOffset MacroAssemblerCompat::movePatchablePtr(ImmPtr ptr, Register dest) {
  const size_t numInst = 1;           // Inserting one load instruction.
  const unsigned numPoolEntries = 2;  // Every pool entry is 4 bytes.
  uint8_t* literalAddr = (uint8_t*)(&ptr.value);  // TODO: Should be const.

  // Scratch space for generating the load instruction.
  //
  // allocLiteralLoadEntry() will use InsertIndexIntoTag() to store a temporary
  // index to the corresponding PoolEntry in the instruction itself.
  //
  // That index will be fixed up later when finishPool()
  // walks over all marked loads and calls PatchConstantPoolLoad().
  uint32_t instructionScratch = 0;

  // Emit the instruction mask in the scratch space.
  // The offset doesn't matter: it will be fixed up later.
  vixl::Assembler::ldr((Instruction*)&instructionScratch, ARMRegister(dest, 64),
                       0);

  // Add the entry to the pool, fix up the LDR imm19 offset,
  // and add the completed instruction to the buffer.
  return allocLiteralLoadEntry(numInst, numPoolEntries,
                               (uint8_t*)&instructionScratch, literalAddr);
}

BufferOffset MacroAssemblerCompat::movePatchablePtr(ImmWord ptr,
                                                    Register dest) {
  const size_t numInst = 1;           // Inserting one load instruction.
  const unsigned numPoolEntries = 2;  // Every pool entry is 4 bytes.
  uint8_t* literalAddr = (uint8_t*)(&ptr.value);

  // Scratch space for generating the load instruction.
  //
  // allocLiteralLoadEntry() will use InsertIndexIntoTag() to store a temporary
  // index to the corresponding PoolEntry in the instruction itself.
  //
  // That index will be fixed up later when finishPool()
  // walks over all marked loads and calls PatchConstantPoolLoad().
  uint32_t instructionScratch = 0;

  // Emit the instruction mask in the scratch space.
  // The offset doesn't matter: it will be fixed up later.
  vixl::Assembler::ldr((Instruction*)&instructionScratch, ARMRegister(dest, 64),
                       0);

  // Add the entry to the pool, fix up the LDR imm19 offset,
  // and add the completed instruction to the buffer.
  return allocLiteralLoadEntry(numInst, numPoolEntries,
                               (uint8_t*)&instructionScratch, literalAddr);
}

void MacroAssemblerCompat::loadPrivate(const Address& src, Register dest) {
  loadPtr(src, dest);
}

void MacroAssemblerCompat::handleFailureWithHandlerTail(
    Label* profilerExitTail) {
  // Fail rather than silently create wrong code.
  MOZ_RELEASE_ASSERT(GetStackPointer64().Is(PseudoStackPointer64));

  // Reserve space for exception information.
  int64_t size = (sizeof(ResumeFromException) + 7) & ~7;
  Sub(PseudoStackPointer64, PseudoStackPointer64, Operand(size));
  syncStackPtr();

  MOZ_ASSERT(!x0.Is(PseudoStackPointer64));
  Mov(x0, PseudoStackPointer64);

  // Call the handler.
  using Fn = void (*)(ResumeFromException * rfe);
  asMasm().setupUnalignedABICall(r1);
  asMasm().passABIArg(r0);
  asMasm().callWithABI<Fn, HandleException>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  Label entryFrame;
  Label catch_;
  Label finally;
  Label return_;
  Label bailout;
  Label wasm;
  Label wasmCatch;

  // Check the `asMasm` calls above didn't mess with the StackPointer identity.
  MOZ_ASSERT(GetStackPointer64().Is(PseudoStackPointer64));

  loadPtr(Address(PseudoStackPointer, offsetof(ResumeFromException, kind)), r0);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_ENTRY_FRAME),
                    &entryFrame);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_CATCH), &catch_);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_FINALLY), &finally);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_FORCED_RETURN), &return_);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_WASM), &wasm);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_WASM_CATCH), &wasmCatch);

  breakpoint();  // Invalid kind.

  // No exception handler. Load the error value, load the new stack pointer,
  // and return from the entry frame.
  bind(&entryFrame);
  moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
  loadPtr(
      Address(PseudoStackPointer, offsetof(ResumeFromException, stackPointer)),
      PseudoStackPointer);

  // `retn` does indeed sync the stack pointer, but before doing that it reads
  // from the stack.  Consequently, if we remove this call to syncStackPointer
  // then we take on the requirement to prove that the immediately preceding
  // loadPtr produces a value for PSP which maintains the SP <= PSP invariant.
  // That's a proof burden we don't want to take on.  In general it would be
  // good to move (at some time in the future, not now) to a world where
  // *every* assignment to PSP or SP is followed immediately by a copy into
  // the other register.  That would make all required correctness proofs
  // trivial in the sense that it requires only local inspection of code
  // immediately following (dominated by) any such assignment.
  syncStackPtr();
  retn(Imm32(1 * sizeof(void*)));  // Pop from stack and return.

  // If we found a catch handler, this must be a baseline frame. Restore state
  // and jump to the catch block.
  bind(&catch_);
  loadPtr(Address(PseudoStackPointer, offsetof(ResumeFromException, target)),
          r0);
  loadPtr(
      Address(PseudoStackPointer, offsetof(ResumeFromException, framePointer)),
      BaselineFrameReg);
  loadPtr(
      Address(PseudoStackPointer, offsetof(ResumeFromException, stackPointer)),
      PseudoStackPointer);
  syncStackPtr();
  Br(x0);

  // If we found a finally block, this must be a baseline frame.
  // Push two values expected by JSOp::Retsub: BooleanValue(true)
  // and the exception.
  bind(&finally);
  ARMRegister exception = x1;
  Ldr(exception, MemOperand(PseudoStackPointer64,
                            offsetof(ResumeFromException, exception)));
  Ldr(x0,
      MemOperand(PseudoStackPointer64, offsetof(ResumeFromException, target)));
  Ldr(ARMRegister(BaselineFrameReg, 64),
      MemOperand(PseudoStackPointer64,
                 offsetof(ResumeFromException, framePointer)));
  Ldr(PseudoStackPointer64,
      MemOperand(PseudoStackPointer64,
                 offsetof(ResumeFromException, stackPointer)));
  syncStackPtr();
  pushValue(BooleanValue(true));
  push(exception);
  Br(x0);

  // Only used in debug mode. Return BaselineFrame->returnValue() to the caller.
  bind(&return_);
  loadPtr(
      Address(PseudoStackPointer, offsetof(ResumeFromException, framePointer)),
      BaselineFrameReg);
  loadPtr(
      Address(PseudoStackPointer, offsetof(ResumeFromException, stackPointer)),
      PseudoStackPointer);
  // See comment further up beginning "`retn` does indeed sync the stack
  // pointer".  That comment applies here too.
  syncStackPtr();
  loadValue(
      Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue()),
      JSReturnOperand);
  movePtr(BaselineFrameReg, PseudoStackPointer);
  syncStackPtr();
  vixl::MacroAssembler::Pop(ARMRegister(BaselineFrameReg, 64));

  // If profiling is enabled, then update the lastProfilingFrame to refer to
  // caller frame before returning.
  {
    Label skipProfilingInstrumentation;
    AbsoluteAddress addressOfEnabled(
        GetJitContext()->runtime->geckoProfiler().addressOfEnabled());
    asMasm().branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                      &skipProfilingInstrumentation);
    jump(profilerExitTail);
    bind(&skipProfilingInstrumentation);
  }

  vixl::MacroAssembler::Pop(vixl::lr);
  syncStackPtr();
  vixl::MacroAssembler::Ret(vixl::lr);

  // If we are bailing out to baseline to handle an exception, jump to the
  // bailout tail stub. Load 1 (true) in x0 (ReturnReg) to indicate success.
  bind(&bailout);
  Ldr(x2, MemOperand(PseudoStackPointer64,
                     offsetof(ResumeFromException, bailoutInfo)));
  Ldr(x1,
      MemOperand(PseudoStackPointer64, offsetof(ResumeFromException, target)));
  Mov(x0, 1);
  Br(x1);

  // If we are throwing and the innermost frame was a wasm frame, reset SP and
  // FP; SP is pointing to the unwound return address to the wasm entry, so
  // we can just ret().
  bind(&wasm);
  Ldr(x29, MemOperand(PseudoStackPointer64,
                      offsetof(ResumeFromException, framePointer)));
  Ldr(PseudoStackPointer64,
      MemOperand(PseudoStackPointer64,
                 offsetof(ResumeFromException, stackPointer)));
  syncStackPtr();
  ret();

  // Found a wasm catch handler, restore state and jump to it.
  bind(&wasmCatch);
  loadPtr(Address(PseudoStackPointer, offsetof(ResumeFromException, target)),
          r0);
  loadPtr(
      Address(PseudoStackPointer, offsetof(ResumeFromException, framePointer)),
      r29);
  loadPtr(
      Address(PseudoStackPointer, offsetof(ResumeFromException, stackPointer)),
      PseudoStackPointer);
  syncStackPtr();
  Br(x0);

  MOZ_ASSERT(GetStackPointer64().Is(PseudoStackPointer64));
}

void MacroAssemblerCompat::profilerEnterFrame(Register framePtr,
                                              Register scratch) {
  profilerEnterFrame(RegisterOrSP(framePtr), scratch);
}

void MacroAssemblerCompat::profilerEnterFrame(RegisterOrSP framePtr,
                                              Register scratch) {
  asMasm().loadJSContext(scratch);
  loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
  if (IsHiddenSP(framePtr)) {
    storeStackPtr(
        Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
  } else {
    storePtr(AsRegister(framePtr),
             Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
  }
  storePtr(ImmPtr(nullptr),
           Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void MacroAssemblerCompat::profilerExitFrame() {
  jump(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
}

void MacroAssemblerCompat::breakpoint() {
  // Note, other payloads are possible, but GDB is known to misinterpret them
  // sometimes and iloop on the breakpoint instead of stopping properly.
  Brk(0);
}

// Either `any` is valid or `sixtyfour` is valid.  Return a 32-bit ARMRegister
// in the first case and an ARMRegister of the desired size in the latter case.

static inline ARMRegister SelectGPReg(AnyRegister any, Register64 sixtyfour,
                                      unsigned size = 64) {
  MOZ_ASSERT(any.isValid() != (sixtyfour != Register64::Invalid()));

  if (sixtyfour == Register64::Invalid()) {
    return ARMRegister(any.gpr(), 32);
  }

  return ARMRegister(sixtyfour.reg, size);
}

// Assert that `sixtyfour` is invalid and then return an FP register from `any`
// of the desired size.

static inline ARMFPRegister SelectFPReg(AnyRegister any, Register64 sixtyfour,
                                        unsigned size) {
  MOZ_ASSERT(sixtyfour == Register64::Invalid());
  return ARMFPRegister(any.fpu(), size);
}

void MacroAssemblerCompat::wasmLoadImpl(const wasm::MemoryAccessDesc& access,
                                        Register memoryBase_, Register ptr_,
                                        AnyRegister outany, Register64 out64) {
  uint32_t offset = access.offset();
  MOZ_ASSERT(offset < asMasm().wasmMaxOffsetGuardLimit());

  ARMRegister memoryBase(memoryBase_, 64);
  ARMRegister ptr(ptr_, 64);
  if (offset) {
    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch = temps.AcquireX();
    Add(scratch, ptr, Operand(offset));
    MemOperand srcAddr(memoryBase, scratch);
    wasmLoadImpl(access, srcAddr, outany, out64);
  } else {
    MemOperand srcAddr(memoryBase, ptr);
    wasmLoadImpl(access, srcAddr, outany, out64);
  }
}

void MacroAssemblerCompat::wasmLoadImpl(const wasm::MemoryAccessDesc& access,
                                        MemOperand srcAddr, AnyRegister outany,
                                        Register64 out64) {
  // Reg+Reg and Reg+SmallImm addressing is directly encodable in one Load
  // instruction, hence we expect exactly one instruction to be emitted in the
  // window.
  int32_t instructionsExpected = 1;

  // Splat and widen however require an additional instruction to be emitted
  // after the load, so allow one more instruction in the window.
  if (access.isSplatSimd128Load() || access.isWidenSimd128Load()) {
    MOZ_ASSERT(access.type() == Scalar::Float64);
    instructionsExpected++;
  }

  asMasm().memoryBarrierBefore(access.sync());

  {
    // The AutoForbidPoolsAndNops asserts if we emit more than the expected
    // number of instructions and thus ensures that the access metadata is
    // emitted at the address of the Load.
    AutoForbidPoolsAndNops afp(this, instructionsExpected);

    append(access, asMasm().currentOffset());
    switch (access.type()) {
      case Scalar::Int8:
        Ldrsb(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Uint8:
        Ldrb(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Int16:
        Ldrsh(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Uint16:
        Ldrh(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Int32:
        if (out64 != Register64::Invalid()) {
          Ldrsw(SelectGPReg(outany, out64), srcAddr);
        } else {
          Ldr(SelectGPReg(outany, out64, 32), srcAddr);
        }
        break;
      case Scalar::Uint32:
        Ldr(SelectGPReg(outany, out64, 32), srcAddr);
        break;
      case Scalar::Int64:
        Ldr(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Float32:
        // LDR does the right thing also for access.isZeroExtendSimd128Load()
        Ldr(SelectFPReg(outany, out64, 32), srcAddr);
        break;
      case Scalar::Float64:
        if (access.isSplatSimd128Load() || access.isWidenSimd128Load()) {
          ScratchSimd128Scope scratch_(asMasm());
          ARMFPRegister scratch = Simd1D(scratch_);
          Ldr(scratch, srcAddr);
          if (access.isSplatSimd128Load()) {
            Dup(SelectFPReg(outany, out64, 128).V2D(), scratch, 0);
          } else {
            MOZ_ASSERT(access.isWidenSimd128Load());
            switch (access.widenSimdOp()) {
              case wasm::SimdOp::I16x8LoadS8x8:
                Sshll(SelectFPReg(outany, out64, 128).V8H(), scratch.V8B(), 0);
                break;
              case wasm::SimdOp::I16x8LoadU8x8:
                Ushll(SelectFPReg(outany, out64, 128).V8H(), scratch.V8B(), 0);
                break;
              case wasm::SimdOp::I32x4LoadS16x4:
                Sshll(SelectFPReg(outany, out64, 128).V4S(), scratch.V4H(), 0);
                break;
              case wasm::SimdOp::I32x4LoadU16x4:
                Ushll(SelectFPReg(outany, out64, 128).V4S(), scratch.V4H(), 0);
                break;
              case wasm::SimdOp::I64x2LoadS32x2:
                Sshll(SelectFPReg(outany, out64, 128).V2D(), scratch.V2S(), 0);
                break;
              case wasm::SimdOp::I64x2LoadU32x2:
                Ushll(SelectFPReg(outany, out64, 128).V2D(), scratch.V2S(), 0);
                break;
              default:
                MOZ_CRASH("Unexpected widening op for wasmLoad");
            }
          }
        } else {
          // LDR does the right thing also for access.isZeroExtendSimd128Load()
          Ldr(SelectFPReg(outany, out64, 64), srcAddr);
        }
        break;
      case Scalar::Simd128:
        Ldr(SelectFPReg(outany, out64, 128), srcAddr);
        break;
      case Scalar::Uint8Clamped:
      case Scalar::BigInt64:
      case Scalar::BigUint64:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected array type");
    }
  }

  asMasm().memoryBarrierAfter(access.sync());
}

// Return true if `address` can be represented as an immediate (possibly scaled
// by the access size) in an LDR/STR type instruction.
//
// For more about the logic here, see vixl::MacroAssembler::LoadStoreMacro().
static bool IsLSImmediateOffset(uint64_t address, size_t accessByteSize) {
  // The predicates below operate on signed values only.
  if (address > INT64_MAX) {
    return false;
  }

  // The access size is always a power of 2, so computing the log amounts to
  // counting trailing zeroes.
  unsigned logAccessSize = mozilla::CountTrailingZeroes32(accessByteSize);
  return (MacroAssemblerCompat::IsImmLSUnscaled(int64_t(address)) ||
          MacroAssemblerCompat::IsImmLSScaled(int64_t(address), logAccessSize));
}

void MacroAssemblerCompat::wasmLoadAbsolute(
    const wasm::MemoryAccessDesc& access, Register memoryBase, uint64_t address,
    AnyRegister output, Register64 out64) {
  if (!IsLSImmediateOffset(address, access.byteSize())) {
    // The access will require the constant to be loaded into a temp register.
    // Do so here, to keep the logic in wasmLoadImpl() tractable wrt emitting
    // trap information.
    //
    // Almost all constant addresses will in practice be handled by a single MOV
    // so do not worry about additional optimizations here.
    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch = temps.AcquireX();
    Mov(scratch, address);
    MemOperand srcAddr(X(memoryBase), scratch);
    wasmLoadImpl(access, srcAddr, output, out64);
  } else {
    MemOperand srcAddr(X(memoryBase), address);
    wasmLoadImpl(access, srcAddr, output, out64);
  }
}

void MacroAssemblerCompat::wasmStoreImpl(const wasm::MemoryAccessDesc& access,
                                         AnyRegister valany, Register64 val64,
                                         Register memoryBase_, Register ptr_) {
  uint32_t offset = access.offset();
  MOZ_ASSERT(offset < asMasm().wasmMaxOffsetGuardLimit());

  ARMRegister memoryBase(memoryBase_, 64);
  ARMRegister ptr(ptr_, 64);
  if (offset) {
    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch = temps.AcquireX();
    Add(scratch, ptr, Operand(offset));
    MemOperand destAddr(memoryBase, scratch);
    wasmStoreImpl(access, destAddr, valany, val64);
  } else {
    MemOperand destAddr(memoryBase, ptr);
    wasmStoreImpl(access, destAddr, valany, val64);
  }
}

void MacroAssemblerCompat::wasmStoreImpl(const wasm::MemoryAccessDesc& access,
                                         MemOperand dstAddr, AnyRegister valany,
                                         Register64 val64) {
  asMasm().memoryBarrierBefore(access.sync());

  {
    // Reg+Reg addressing is directly encodable in one Store instruction, hence
    // the AutoForbidPoolsAndNops will ensure that the access metadata is
    // emitted at the address of the Store.  The AutoForbidPoolsAndNops will
    // assert if we emit more than one instruction.

    AutoForbidPoolsAndNops afp(this,
                               /* max number of instructions in scope = */ 1);

    append(access, asMasm().currentOffset());
    switch (access.type()) {
      case Scalar::Int8:
      case Scalar::Uint8:
        Strb(SelectGPReg(valany, val64), dstAddr);
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        Strh(SelectGPReg(valany, val64), dstAddr);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        Str(SelectGPReg(valany, val64), dstAddr);
        break;
      case Scalar::Int64:
        Str(SelectGPReg(valany, val64), dstAddr);
        break;
      case Scalar::Float32:
        Str(SelectFPReg(valany, val64, 32), dstAddr);
        break;
      case Scalar::Float64:
        Str(SelectFPReg(valany, val64, 64), dstAddr);
        break;
      case Scalar::Simd128:
        Str(SelectFPReg(valany, val64, 128), dstAddr);
        break;
      case Scalar::Uint8Clamped:
      case Scalar::BigInt64:
      case Scalar::BigUint64:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected array type");
    }
  }

  asMasm().memoryBarrierAfter(access.sync());
}

void MacroAssemblerCompat::wasmStoreAbsolute(
    const wasm::MemoryAccessDesc& access, AnyRegister value, Register64 value64,
    Register memoryBase, uint64_t address) {
  // See comments in wasmLoadAbsolute.
  unsigned logAccessSize = mozilla::CountTrailingZeroes32(access.byteSize());
  if (address > INT64_MAX || !(IsImmLSScaled(int64_t(address), logAccessSize) ||
                               IsImmLSUnscaled(int64_t(address)))) {
    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch = temps.AcquireX();
    Mov(scratch, address);
    MemOperand destAddr(X(memoryBase), scratch);
    wasmStoreImpl(access, destAddr, value, value64);
  } else {
    MemOperand destAddr(X(memoryBase), address);
    wasmStoreImpl(access, destAddr, value, value64);
  }
}

void MacroAssemblerCompat::compareSimd128Int(Assembler::Condition cond,
                                             ARMFPRegister dest,
                                             ARMFPRegister lhs,
                                             ARMFPRegister rhs) {
  switch (cond) {
    case Assembler::Equal:
      Cmeq(dest, lhs, rhs);
      break;
    case Assembler::NotEqual:
      Cmeq(dest, lhs, rhs);
      Mvn(dest, dest);
      break;
    case Assembler::GreaterThan:
      Cmgt(dest, lhs, rhs);
      break;
    case Assembler::GreaterThanOrEqual:
      Cmge(dest, lhs, rhs);
      break;
    case Assembler::LessThan:
      Cmgt(dest, rhs, lhs);
      break;
    case Assembler::LessThanOrEqual:
      Cmge(dest, rhs, lhs);
      break;
    case Assembler::Above:
      Cmhi(dest, lhs, rhs);
      break;
    case Assembler::AboveOrEqual:
      Cmhs(dest, lhs, rhs);
      break;
    case Assembler::Below:
      Cmhi(dest, rhs, lhs);
      break;
    case Assembler::BelowOrEqual:
      Cmhs(dest, rhs, lhs);
      break;
    default:
      MOZ_CRASH("Unexpected SIMD integer condition");
  }
}

void MacroAssemblerCompat::compareSimd128Float(Assembler::Condition cond,
                                               ARMFPRegister dest,
                                               ARMFPRegister lhs,
                                               ARMFPRegister rhs) {
  switch (cond) {
    case Assembler::Equal:
      Fcmeq(dest, lhs, rhs);
      break;
    case Assembler::NotEqual:
      Fcmeq(dest, lhs, rhs);
      Mvn(dest, dest);
      break;
    case Assembler::GreaterThan:
      Fcmgt(dest, lhs, rhs);
      break;
    case Assembler::GreaterThanOrEqual:
      Fcmge(dest, lhs, rhs);
      break;
    case Assembler::LessThan:
      Fcmgt(dest, rhs, lhs);
      break;
    case Assembler::LessThanOrEqual:
      Fcmge(dest, rhs, lhs);
      break;
    default:
      MOZ_CRASH("Unexpected SIMD integer condition");
  }
}

void MacroAssemblerCompat::rightShiftInt8x16(FloatRegister lhs, Register rhs,
                                             FloatRegister dest,
                                             bool isUnsigned) {
  ScratchSimd128Scope scratch_(asMasm());
  ARMFPRegister shift = Simd16B(scratch_);

  // Compute -(shift & 7) in all 8-bit lanes
  {
    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch = temps.AcquireW();
    And(scratch, ARMRegister(rhs, 32), 7);
    Neg(scratch, scratch);
    Dup(shift, scratch);
  }

  if (isUnsigned) {
    Ushl(Simd16B(dest), Simd16B(lhs), shift);
  } else {
    Sshl(Simd16B(dest), Simd16B(lhs), shift);
  }
}

void MacroAssemblerCompat::rightShiftInt16x8(FloatRegister lhs, Register rhs,
                                             FloatRegister dest,
                                             bool isUnsigned) {
  ScratchSimd128Scope scratch_(asMasm());
  ARMFPRegister shift = Simd8H(scratch_);

  // Compute -(shift & 15) in all 16-bit lanes
  {
    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch = temps.AcquireW();
    And(scratch, ARMRegister(rhs, 32), 15);
    Neg(scratch, scratch);
    Dup(shift, scratch);
  }

  if (isUnsigned) {
    Ushl(Simd8H(dest), Simd8H(lhs), shift);
  } else {
    Sshl(Simd8H(dest), Simd8H(lhs), shift);
  }
}

void MacroAssemblerCompat::rightShiftInt32x4(FloatRegister lhs, Register rhs,
                                             FloatRegister dest,
                                             bool isUnsigned) {
  ScratchSimd128Scope scratch_(asMasm());
  ARMFPRegister shift = Simd4S(scratch_);

  // Compute -(shift & 31) in all 32-bit lanes
  {
    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch = temps.AcquireW();
    And(scratch, ARMRegister(rhs, 32), 31);
    Neg(scratch, scratch);
    Dup(shift, scratch);
  }

  if (isUnsigned) {
    Ushl(Simd4S(dest), Simd4S(lhs), shift);
  } else {
    Sshl(Simd4S(dest), Simd4S(lhs), shift);
  }
}

void MacroAssemblerCompat::rightShiftInt64x2(FloatRegister lhs, Register rhs,
                                             FloatRegister dest,
                                             bool isUnsigned) {
  ScratchSimd128Scope scratch_(asMasm());
  ARMFPRegister shift = Simd2D(scratch_);

  // Compute -(shift & 63)
  {
    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch = temps.AcquireX();
    And(scratch, ARMRegister(rhs, 64), 63);
    Neg(scratch, scratch);
    Dup(shift, scratch);
  }

  if (isUnsigned) {
    Ushl(Simd2D(dest), Simd2D(lhs), shift);
  } else {
    Sshl(Simd2D(dest), Simd2D(lhs), shift);
  }
}

void MacroAssembler::reserveStack(uint32_t amount) {
  // TODO: This bumps |sp| every time we reserve using a second register.
  // It would save some instructions if we had a fixed frame size.
  vixl::MacroAssembler::Claim(Operand(amount));
  adjustFrame(amount);
}

void MacroAssembler::Push(RegisterOrSP reg) {
  if (IsHiddenSP(reg)) {
    push(sp);
  } else {
    push(AsRegister(reg));
  }
  adjustFrame(sizeof(intptr_t));
}

//{{{ check_macroassembler_style
// ===============================================================
// MacroAssembler high-level usage.

void MacroAssembler::flush() { Assembler::flush(); }

// ===============================================================
// Stack manipulation functions.

// Routines for saving/restoring registers on the stack.  The format is:
//
//   (highest address)
//
//   integer (X) regs in any order      size: 8 * # int regs
//
//   if # int regs is odd,
//     then an 8 byte alignment hole    size: 0 or 8
//
//   double (D) regs in any order       size: 8 * # double regs
//
//   if # double regs is odd,
//     then an 8 byte alignment hole    size: 0 or 8
//
//   vector (Q) regs in any order       size: 16 * # vector regs
//
//   (lowest address)
//
// Hence the size of the save area is 0 % 16.  And, provided that the base
// (highest) address is 16-aligned, then the vector reg save/restore accesses
// will also be 16-aligned, as will pairwise operations for the double regs.
//
// Implied by this is that the format of the double and vector dump area
// corresponds with what FloatRegister::GetPushSizeInBytes computes.
// See block comment in MacroAssembler.h for more details.

size_t MacroAssembler::PushRegsInMaskSizeInBytes(LiveRegisterSet set) {
  size_t numIntRegs = set.gprs().size();
  return ((numIntRegs + 1) & ~1) * sizeof(intptr_t) +
         FloatRegister::GetPushSizeInBytes(set.fpus());
}

// Generate code to dump the values in `set`, either on the stack if `dest` is
// `Nothing` or working backwards from the address denoted by `dest` if it is
// `Some`.  These two cases are combined so as to minimise the chance of
// mistakenly generating different formats for the same `set`, given that the
// `Some` `dest` case is used extremely rarely.
static void PushOrStoreRegsInMask(MacroAssembler* masm, LiveRegisterSet set,
                                  mozilla::Maybe<Address> dest) {
  static_assert(sizeof(FloatRegisters::RegisterContent) == 16);

  // If we're saving to arbitrary memory, check the destination is big enough.
  if (dest) {
    mozilla::DebugOnly<size_t> bytesRequired =
        masm->PushRegsInMaskSizeInBytes(set);
    MOZ_ASSERT(dest->offset >= 0);
    MOZ_ASSERT(((size_t)dest->offset) >= bytesRequired);
  }

  // Note the high limit point; we'll check it again later.
  mozilla::DebugOnly<size_t> maxExtentInitial =
      dest ? dest->offset : masm->framePushed();

  // Gather up the integer registers in groups of four, and either push each
  // group as a single transfer so as to minimise the number of stack pointer
  // changes, or write them individually to memory.  Take care to ensure the
  // space used remains 16-aligned.
  for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more();) {
    vixl::CPURegister src[4] = {vixl::NoCPUReg, vixl::NoCPUReg, vixl::NoCPUReg,
                                vixl::NoCPUReg};
    size_t i;
    for (i = 0; i < 4 && iter.more(); i++) {
      src[i] = ARMRegister(*iter, 64);
      ++iter;
    }
    MOZ_ASSERT(i > 0);

    if (i == 1 || i == 3) {
      // Ensure the stack remains 16-aligned
      MOZ_ASSERT(!iter.more());
      src[i] = vixl::xzr;
      i++;
    }
    MOZ_ASSERT(i == 2 || i == 4);

    if (dest) {
      for (size_t j = 0; j < i; j++) {
        Register ireg = Register::FromCode(src[j].IsZero() ? Registers::xzr
                                                           : src[j].code());
        dest->offset -= sizeof(intptr_t);
        masm->storePtr(ireg, *dest);
      }
    } else {
      masm->adjustFrame(i * 8);
      masm->vixl::MacroAssembler::Push(src[0], src[1], src[2], src[3]);
    }
  }

  // Now the same for the FP double registers.  Note that because of how
  // ReduceSetForPush works, an underlying AArch64 SIMD/FP register can either
  // be present as a double register, or as a V128 register, but not both.
  // Firstly, round up the registers to be pushed.

  FloatRegisterSet fpuSet(set.fpus().reduceSetForPush());
  vixl::CPURegister allSrcs[FloatRegisters::TotalPhys];
  size_t numAllSrcs = 0;

  for (FloatRegisterBackwardIterator iter(fpuSet); iter.more(); ++iter) {
    FloatRegister reg = *iter;
    if (reg.isDouble()) {
      MOZ_RELEASE_ASSERT(numAllSrcs < FloatRegisters::TotalPhys);
      allSrcs[numAllSrcs] = ARMFPRegister(reg, 64);
      numAllSrcs++;
    } else {
      MOZ_ASSERT(reg.isSimd128());
    }
  }
  MOZ_RELEASE_ASSERT(numAllSrcs <= FloatRegisters::TotalPhys);

  if ((numAllSrcs & 1) == 1) {
    // We've got an odd number of doubles.  In order to maintain 16-alignment,
    // push the last register twice.  We'll skip over the duplicate in
    // PopRegsInMaskIgnore.
    allSrcs[numAllSrcs] = allSrcs[numAllSrcs - 1];
    numAllSrcs++;
  }
  MOZ_RELEASE_ASSERT(numAllSrcs <= FloatRegisters::TotalPhys);
  MOZ_RELEASE_ASSERT((numAllSrcs & 1) == 0);

  // And now generate the transfers.
  size_t i;
  if (dest) {
    for (i = 0; i < numAllSrcs; i++) {
      FloatRegister freg =
          FloatRegister(FloatRegisters::FPRegisterID(allSrcs[i].code()),
                        FloatRegisters::Kind::Double);
      dest->offset -= sizeof(double);
      masm->storeDouble(freg, *dest);
    }
  } else {
    i = 0;
    while (i < numAllSrcs) {
      vixl::CPURegister src[4] = {vixl::NoCPUReg, vixl::NoCPUReg,
                                  vixl::NoCPUReg, vixl::NoCPUReg};
      size_t j;
      for (j = 0; j < 4 && j + i < numAllSrcs; j++) {
        src[j] = allSrcs[j + i];
      }
      masm->adjustFrame(8 * j);
      masm->vixl::MacroAssembler::Push(src[0], src[1], src[2], src[3]);
      i += j;
    }
  }
  MOZ_ASSERT(i == numAllSrcs);

  // Finally, deal with the SIMD (V128) registers.  This is a bit simpler
  // as there's no need for special-casing to maintain 16-alignment.

  numAllSrcs = 0;
  for (FloatRegisterBackwardIterator iter(fpuSet); iter.more(); ++iter) {
    FloatRegister reg = *iter;
    if (reg.isSimd128()) {
      MOZ_RELEASE_ASSERT(numAllSrcs < FloatRegisters::TotalPhys);
      allSrcs[numAllSrcs] = ARMFPRegister(reg, 128);
      numAllSrcs++;
    }
  }
  MOZ_RELEASE_ASSERT(numAllSrcs <= FloatRegisters::TotalPhys);

  // Generate the transfers.
  if (dest) {
    for (i = 0; i < numAllSrcs; i++) {
      FloatRegister freg =
          FloatRegister(FloatRegisters::FPRegisterID(allSrcs[i].code()),
                        FloatRegisters::Kind::Simd128);
      dest->offset -= FloatRegister::SizeOfSimd128;
      masm->storeUnalignedSimd128(freg, *dest);
    }
  } else {
    i = 0;
    while (i < numAllSrcs) {
      vixl::CPURegister src[4] = {vixl::NoCPUReg, vixl::NoCPUReg,
                                  vixl::NoCPUReg, vixl::NoCPUReg};
      size_t j;
      for (j = 0; j < 4 && j + i < numAllSrcs; j++) {
        src[j] = allSrcs[j + i];
      }
      masm->adjustFrame(16 * j);
      masm->vixl::MacroAssembler::Push(src[0], src[1], src[2], src[3]);
      i += j;
    }
  }
  MOZ_ASSERT(i == numAllSrcs);

  // Final overrun check.
  if (dest) {
    MOZ_ASSERT(maxExtentInitial - dest->offset ==
               masm->PushRegsInMaskSizeInBytes(set));
  } else {
    MOZ_ASSERT(masm->framePushed() - maxExtentInitial ==
               masm->PushRegsInMaskSizeInBytes(set));
  }
}

void MacroAssembler::PushRegsInMask(LiveRegisterSet set) {
  PushOrStoreRegsInMask(this, set, mozilla::Nothing());
}

void MacroAssembler::storeRegsInMask(LiveRegisterSet set, Address dest,
                                     Register scratch) {
  PushOrStoreRegsInMask(this, set, mozilla::Some(dest));
}

// This is a helper function for PopRegsInMaskIgnore below.  It emits the
// loads described by dests[0] and [1] and offsets[0] and [1], generating a
// load-pair if it can.
static void GeneratePendingLoadsThenFlush(MacroAssembler* masm,
                                          vixl::CPURegister* dests,
                                          uint32_t* offsets,
                                          uint32_t transactionSize) {
  // Generate the loads ..
  if (!dests[0].IsNone()) {
    if (!dests[1].IsNone()) {
      // [0] and [1] both present.
      if (offsets[0] + transactionSize == offsets[1]) {
        masm->Ldp(dests[0], dests[1],
                  MemOperand(masm->GetStackPointer64(), offsets[0]));
      } else {
        // Theoretically we could check for a load-pair with the destinations
        // switched, but our callers will never generate that.  Hence there's
        // no loss in giving up at this point and generating two loads.
        masm->Ldr(dests[0], MemOperand(masm->GetStackPointer64(), offsets[0]));
        masm->Ldr(dests[1], MemOperand(masm->GetStackPointer64(), offsets[1]));
      }
    } else {
      // [0] only.
      masm->Ldr(dests[0], MemOperand(masm->GetStackPointer64(), offsets[0]));
    }
  } else {
    if (!dests[1].IsNone()) {
      // [1] only.  Can't happen because callers always fill [0] before [1].
      MOZ_CRASH("GenerateLoadsThenFlush");
    } else {
      // Neither entry valid.  This can happen.
    }
  }

  // .. and flush.
  dests[0] = dests[1] = vixl::NoCPUReg;
  offsets[0] = offsets[1] = 0;
}

void MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set,
                                         LiveRegisterSet ignore) {
  mozilla::DebugOnly<size_t> framePushedInitial = framePushed();

  // The offset of the data from the stack pointer.
  uint32_t offset = 0;

  // The set of FP/SIMD registers we need to restore.
  FloatRegisterSet fpuSet(set.fpus().reduceSetForPush());

  // The set of registers to ignore.  BroadcastToAllSizes() is used to avoid
  // any ambiguities arising from (eg) `fpuSet` containing q17 but `ignore`
  // containing d17.
  FloatRegisterSet ignoreFpusBroadcasted(
      FloatRegister::BroadcastToAllSizes(ignore.fpus()));

  // First recover the SIMD (V128) registers.  This is straightforward in that
  // we don't need to think about alignment holes.

  // These three form a two-entry queue that holds loads that we know we
  // need, but which we haven't yet emitted.
  vixl::CPURegister pendingDests[2] = {vixl::NoCPUReg, vixl::NoCPUReg};
  uint32_t pendingOffsets[2] = {0, 0};
  size_t nPending = 0;

  for (FloatRegisterIterator iter(fpuSet); iter.more(); ++iter) {
    FloatRegister reg = *iter;
    if (reg.isDouble()) {
      continue;
    }
    MOZ_RELEASE_ASSERT(reg.isSimd128());

    uint32_t offsetForReg = offset;
    offset += FloatRegister::SizeOfSimd128;

    if (ignoreFpusBroadcasted.hasRegisterIndex(reg)) {
      continue;
    }

    MOZ_ASSERT(nPending <= 2);
    if (nPending == 2) {
      GeneratePendingLoadsThenFlush(this, pendingDests, pendingOffsets, 16);
      nPending = 0;
    }
    pendingDests[nPending] = ARMFPRegister(reg, 128);
    pendingOffsets[nPending] = offsetForReg;
    nPending++;
  }
  GeneratePendingLoadsThenFlush(this, pendingDests, pendingOffsets, 16);
  nPending = 0;

  MOZ_ASSERT((offset % 16) == 0);

  // Now recover the FP double registers.  This is more tricky in that we need
  // to skip over the lowest-addressed of them if the number of them was odd.

  if ((((fpuSet.bits() & FloatRegisters::AllDoubleMask).size()) & 1) == 1) {
    offset += sizeof(double);
  }

  for (FloatRegisterIterator iter(fpuSet); iter.more(); ++iter) {
    FloatRegister reg = *iter;
    if (reg.isSimd128()) {
      continue;
    }
    /* true but redundant, per loop above: MOZ_RELEASE_ASSERT(reg.isDouble()) */

    uint32_t offsetForReg = offset;
    offset += sizeof(double);

    if (ignoreFpusBroadcasted.hasRegisterIndex(reg)) {
      continue;
    }

    MOZ_ASSERT(nPending <= 2);
    if (nPending == 2) {
      GeneratePendingLoadsThenFlush(this, pendingDests, pendingOffsets, 8);
      nPending = 0;
    }
    pendingDests[nPending] = ARMFPRegister(reg, 64);
    pendingOffsets[nPending] = offsetForReg;
    nPending++;
  }
  GeneratePendingLoadsThenFlush(this, pendingDests, pendingOffsets, 8);
  nPending = 0;

  MOZ_ASSERT((offset % 16) == 0);
  MOZ_ASSERT(offset == set.fpus().getPushSizeInBytes());

  // And finally recover the integer registers, again skipping an alignment
  // hole if it exists.

  if ((set.gprs().size() & 1) == 1) {
    offset += sizeof(uint64_t);
  }

  for (GeneralRegisterIterator iter(set.gprs()); iter.more(); ++iter) {
    Register reg = *iter;

    uint32_t offsetForReg = offset;
    offset += sizeof(uint64_t);

    if (ignore.has(reg)) {
      continue;
    }

    MOZ_ASSERT(nPending <= 2);
    if (nPending == 2) {
      GeneratePendingLoadsThenFlush(this, pendingDests, pendingOffsets, 8);
      nPending = 0;
    }
    pendingDests[nPending] = ARMRegister(reg, 64);
    pendingOffsets[nPending] = offsetForReg;
    nPending++;
  }
  GeneratePendingLoadsThenFlush(this, pendingDests, pendingOffsets, 8);

  MOZ_ASSERT((offset % 16) == 0);

  size_t bytesPushed = PushRegsInMaskSizeInBytes(set);
  MOZ_ASSERT(offset == bytesPushed);
  freeStack(bytesPushed);
}

void MacroAssembler::Push(Register reg) {
  push(reg);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(Register reg1, Register reg2, Register reg3,
                          Register reg4) {
  push(reg1, reg2, reg3, reg4);
  adjustFrame(4 * sizeof(intptr_t));
}

void MacroAssembler::Push(const Imm32 imm) {
  push(imm);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(const ImmWord imm) {
  push(imm);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(const ImmPtr imm) {
  push(imm);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(const ImmGCPtr ptr) {
  push(ptr);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(FloatRegister f) {
  push(f);
  adjustFrame(sizeof(double));
}

void MacroAssembler::PushBoxed(FloatRegister reg) {
  subFromStackPtr(Imm32(sizeof(double)));
  boxDouble(reg, Address(getStackPointer(), 0));
  adjustFrame(sizeof(double));
}

void MacroAssembler::Pop(Register reg) {
  pop(reg);
  adjustFrame(-1 * int64_t(sizeof(int64_t)));
}

void MacroAssembler::Pop(FloatRegister f) {
  loadDouble(Address(getStackPointer(), 0), f);
  freeStack(sizeof(double));
}

void MacroAssembler::Pop(const ValueOperand& val) {
  pop(val);
  adjustFrame(-1 * int64_t(sizeof(int64_t)));
}

// ===============================================================
// Simple call functions.

CodeOffset MacroAssembler::call(Register reg) {
  // This sync has been observed (and is expected) to be necessary.
  // eg testcase: tests/debug/bug1107525.js
  syncStackPtr();
  Blr(ARMRegister(reg, 64));
  return CodeOffset(currentOffset());
}

CodeOffset MacroAssembler::call(Label* label) {
  // This sync has been observed (and is expected) to be necessary.
  // eg testcase: tests/basic/testBug504520Harder.js
  syncStackPtr();
  Bl(label);
  return CodeOffset(currentOffset());
}

void MacroAssembler::call(ImmPtr imm) {
  // This sync has been observed (and is expected) to be necessary.
  // eg testcase: asm.js/testTimeout5.js
  syncStackPtr();
  vixl::UseScratchRegisterScope temps(this);
  MOZ_ASSERT(temps.IsAvailable(ScratchReg64));  // ip0
  temps.Exclude(ScratchReg64);
  movePtr(imm, ScratchReg64.asUnsized());
  Blr(ScratchReg64);
}

void MacroAssembler::call(ImmWord imm) { call(ImmPtr((void*)imm.value)); }

CodeOffset MacroAssembler::call(wasm::SymbolicAddress imm) {
  vixl::UseScratchRegisterScope temps(this);
  const Register scratch = temps.AcquireX().asUnsized();
  // This sync is believed to be necessary, although no case in jit-test/tests
  // has been observed to cause SP != PSP here.
  syncStackPtr();
  movePtr(imm, scratch);
  Blr(ARMRegister(scratch, 64));
  return CodeOffset(currentOffset());
}

void MacroAssembler::call(const Address& addr) {
  vixl::UseScratchRegisterScope temps(this);
  const Register scratch = temps.AcquireX().asUnsized();
  // This sync has been observed (and is expected) to be necessary.
  // eg testcase: tests/backup-point-bug1315634.js
  syncStackPtr();
  loadPtr(addr, scratch);
  Blr(ARMRegister(scratch, 64));
}

void MacroAssembler::call(JitCode* c) {
  vixl::UseScratchRegisterScope temps(this);
  const ARMRegister scratch64 = temps.AcquireX();
  // This sync has been observed (and is expected) to be necessary.
  // eg testcase: arrays/new-array-undefined-undefined-more-args-2.js
  syncStackPtr();
  BufferOffset off = immPool64(scratch64, uint64_t(c->raw()));
  addPendingJump(off, ImmPtr(c->raw()), RelocationKind::JITCODE);
  blr(scratch64);
}

CodeOffset MacroAssembler::callWithPatch() {
  // This needs to sync.  Wasm goes through this one for intramodule calls.
  //
  // In other cases, wasm goes through masm.wasmCallImport(),
  // masm.wasmCallBuiltinInstanceMethod, masm.wasmCallIndirect, all of which
  // sync.
  //
  // This sync is believed to be necessary, although no case in jit-test/tests
  // has been observed to cause SP != PSP here.
  syncStackPtr();
  bl(0, LabelDoc());
  return CodeOffset(currentOffset());
}
void MacroAssembler::patchCall(uint32_t callerOffset, uint32_t calleeOffset) {
  Instruction* inst = getInstructionAt(BufferOffset(callerOffset - 4));
  MOZ_ASSERT(inst->IsBL());
  ptrdiff_t relTarget = (int)calleeOffset - ((int)callerOffset - 4);
  ptrdiff_t relTarget00 = relTarget >> 2;
  MOZ_RELEASE_ASSERT((relTarget & 0x3) == 0);
  MOZ_RELEASE_ASSERT(vixl::IsInt26(relTarget00));
  bl(inst, relTarget00);
}

CodeOffset MacroAssembler::farJumpWithPatch() {
  vixl::UseScratchRegisterScope temps(this);
  const ARMRegister scratch = temps.AcquireX();
  const ARMRegister scratch2 = temps.AcquireX();

  AutoForbidPoolsAndNops afp(this,
                             /* max number of instructions in scope = */ 7);

  mozilla::DebugOnly<uint32_t> before = currentOffset();

  align(8);  // At most one nop

  Label branch;
  adr(scratch2, &branch);
  ldr(scratch, vixl::MemOperand(scratch2, 4));
  add(scratch2, scratch2, scratch);
  CodeOffset offs(currentOffset());
  bind(&branch);
  br(scratch2);
  Emit(UINT32_MAX);
  Emit(UINT32_MAX);

  mozilla::DebugOnly<uint32_t> after = currentOffset();

  MOZ_ASSERT(after - before == 24 || after - before == 28);

  return offs;
}

void MacroAssembler::patchFarJump(CodeOffset farJump, uint32_t targetOffset) {
  Instruction* inst1 = getInstructionAt(BufferOffset(farJump.offset() + 4));
  Instruction* inst2 = getInstructionAt(BufferOffset(farJump.offset() + 8));

  int64_t distance = (int64_t)targetOffset - (int64_t)farJump.offset();

  MOZ_ASSERT(inst1->InstructionBits() == UINT32_MAX);
  MOZ_ASSERT(inst2->InstructionBits() == UINT32_MAX);

  inst1->SetInstructionBits((uint32_t)distance);
  inst2->SetInstructionBits((uint32_t)(distance >> 32));
}

CodeOffset MacroAssembler::nopPatchableToCall() {
  AutoForbidPoolsAndNops afp(this,
                             /* max number of instructions in scope = */ 1);
  Nop();
  return CodeOffset(currentOffset());
}

void MacroAssembler::patchNopToCall(uint8_t* call, uint8_t* target) {
  uint8_t* inst = call - 4;
  Instruction* instr = reinterpret_cast<Instruction*>(inst);
  MOZ_ASSERT(instr->IsBL() || instr->IsNOP());
  bl(instr, (target - inst) >> 2);
}

void MacroAssembler::patchCallToNop(uint8_t* call) {
  uint8_t* inst = call - 4;
  Instruction* instr = reinterpret_cast<Instruction*>(inst);
  MOZ_ASSERT(instr->IsBL() || instr->IsNOP());
  nop(instr);
}

void MacroAssembler::pushReturnAddress() {
  MOZ_RELEASE_ASSERT(!sp.Is(GetStackPointer64()), "Not valid");
  push(lr);
}

void MacroAssembler::popReturnAddress() {
  MOZ_RELEASE_ASSERT(!sp.Is(GetStackPointer64()), "Not valid");
  pop(lr);
}

// ===============================================================
// ABI function calls.

void MacroAssembler::setupUnalignedABICall(Register scratch) {
  // Because wasm operates without the need for dynamic alignment of SP, it is
  // implied that this routine should never be called when generating wasm.
  MOZ_ASSERT(!IsCompilingWasm());

  // The following won't work for SP -- needs slightly different logic.
  MOZ_RELEASE_ASSERT(GetStackPointer64().Is(PseudoStackPointer64));

  setupNativeABICall();
  dynamicAlignment_ = true;

  int64_t alignment = ~(int64_t(ABIStackAlignment) - 1);
  ARMRegister scratch64(scratch, 64);
  MOZ_ASSERT(!scratch64.Is(PseudoStackPointer64));

  // Always save LR -- Baseline ICs assume that LR isn't modified.
  push(lr);

  // Remember the stack address on entry.  This is reloaded in callWithABIPost
  // below.
  Mov(scratch64, PseudoStackPointer64);

  // Make alignment, including the effective push of the previous sp.
  Sub(PseudoStackPointer64, PseudoStackPointer64, Operand(8));
  And(PseudoStackPointer64, PseudoStackPointer64, Operand(alignment));
  syncStackPtr();

  // Store previous sp to the top of the stack, aligned.  This is also
  // reloaded in callWithABIPost.
  Str(scratch64, MemOperand(PseudoStackPointer64, 0));
}

void MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromWasm) {
  // wasm operates without the need for dynamic alignment of SP.
  MOZ_ASSERT(!(dynamicAlignment_ && callFromWasm));

  MOZ_ASSERT(inCall_);
  uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

  // ARM64 *really* wants SP to always be 16-aligned, so ensure this now.
  if (dynamicAlignment_) {
    stackForCall += ComputeByteAlignment(stackForCall, StackAlignment);
  } else {
    // This can happen when we attach out-of-line stubs for rare cases.  For
    // example CodeGenerator::visitWasmTruncateToInt32 adds an out-of-line
    // chunk.
    uint32_t alignmentAtPrologue = callFromWasm ? sizeof(wasm::Frame) : 0;
    stackForCall += ComputeByteAlignment(
        stackForCall + framePushed() + alignmentAtPrologue, ABIStackAlignment);
  }

  *stackAdjust = stackForCall;
  reserveStack(*stackAdjust);
  {
    enoughMemory_ &= moveResolver_.resolve();
    if (!enoughMemory_) {
      return;
    }
    MoveEmitter emitter(*this);
    emitter.emit(moveResolver_);
    emitter.finish();
  }

  // Call boundaries communicate stack via SP.
  // (jseward, 2021Mar03) This sync may well be redundant, given that all of
  // the MacroAssembler::call methods generate a sync before the call.
  // Removing it does not cause any failures for all of jit-tests.
  syncStackPtr();
}

void MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result,
                                     bool callFromWasm) {
  // wasm operates without the need for dynamic alignment of SP.
  MOZ_ASSERT(!(dynamicAlignment_ && callFromWasm));

  // Call boundaries communicate stack via SP, so we must resync PSP now.
  initPseudoStackPtr();

  freeStack(stackAdjust);

  if (dynamicAlignment_) {
    // This then-clause makes more sense if you first read
    // setupUnalignedABICall above.
    //
    // Restore the stack pointer from entry.  The stack pointer will have been
    // saved by setupUnalignedABICall.  This is fragile in that it assumes
    // that uses of this routine (callWithABIPost) with `dynamicAlignment_ ==
    // true` are preceded by matching calls to setupUnalignedABICall.  But
    // there's nothing that enforce that mechanically.  If we really want to
    // enforce this, we could add a debug-only CallWithABIState enum to the
    // MacroAssembler and assert that setupUnalignedABICall updates it before
    // we get here, then reset it to its initial state.
    Ldr(GetStackPointer64(), MemOperand(GetStackPointer64(), 0));
    syncStackPtr();

    // Restore LR.  This restores LR to the value stored by
    // setupUnalignedABICall, which should have been called just before
    // callWithABIPre.  This is, per the above comment, also fragile.
    pop(lr);

    // SP may be < PSP now.  That is expected from the behaviour of `pop`.  It
    // is not clear why the following `syncStackPtr` is necessary, but it is:
    // without it, the following test segfaults:
    // tests/backup-point-bug1315634.js
    syncStackPtr();
  }

  // If the ABI's return regs are where ION is expecting them, then
  // no other work needs to be done.

#ifdef DEBUG
  MOZ_ASSERT(inCall_);
  inCall_ = false;
#endif
}

void MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result) {
  vixl::UseScratchRegisterScope temps(this);
  const Register scratch = temps.AcquireX().asUnsized();
  movePtr(fun, scratch);

  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(scratch);
  callWithABIPost(stackAdjust, result);
}

void MacroAssembler::callWithABINoProfiler(const Address& fun,
                                           MoveOp::Type result) {
  vixl::UseScratchRegisterScope temps(this);
  const Register scratch = temps.AcquireX().asUnsized();
  loadPtr(fun, scratch);

  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(scratch);
  callWithABIPost(stackAdjust, result);
}

// ===============================================================
// Jit Frames.

uint32_t MacroAssembler::pushFakeReturnAddress(Register scratch) {
  enterNoPool(3);
  Label fakeCallsite;

  Adr(ARMRegister(scratch, 64), &fakeCallsite);
  Push(scratch);
  bind(&fakeCallsite);
  uint32_t pseudoReturnOffset = currentOffset();

  leaveNoPool();
  return pseudoReturnOffset;
}

bool MacroAssemblerCompat::buildOOLFakeExitFrame(void* fakeReturnAddr) {
  uint32_t descriptor = MakeFrameDescriptor(
      asMasm().framePushed(), FrameType::IonJS, ExitFrameLayout::Size());
  asMasm().Push(Imm32(descriptor));
  asMasm().Push(ImmPtr(fakeReturnAddr));
  return true;
}

// ===============================================================
// Move instructions

void MacroAssembler::moveValue(const TypedOrValueRegister& src,
                               const ValueOperand& dest) {
  if (src.hasValue()) {
    moveValue(src.valueReg(), dest);
    return;
  }

  MIRType type = src.type();
  AnyRegister reg = src.typedReg();

  if (!IsFloatingPointType(type)) {
    boxNonDouble(ValueTypeFromMIRType(type), reg.gpr(), dest);
    return;
  }

  ScratchDoubleScope scratch(*this);
  FloatRegister freg = reg.fpu();
  if (type == MIRType::Float32) {
    convertFloat32ToDouble(freg, scratch);
    freg = scratch;
  }
  boxDouble(freg, dest, scratch);
}

void MacroAssembler::moveValue(const ValueOperand& src,
                               const ValueOperand& dest) {
  if (src == dest) {
    return;
  }
  movePtr(src.valueReg(), dest.valueReg());
}

void MacroAssembler::moveValue(const Value& src, const ValueOperand& dest) {
  if (!src.isGCThing()) {
    movePtr(ImmWord(src.asRawBits()), dest.valueReg());
    return;
  }

  BufferOffset load =
      movePatchablePtr(ImmPtr(src.bitsAsPunboxPointer()), dest.valueReg());
  writeDataRelocation(src, load);
}

// ===============================================================
// Branch functions

void MacroAssembler::loadStoreBuffer(Register ptr, Register buffer) {
  if (ptr != buffer) {
    movePtr(ptr, buffer);
  }
  orPtr(Imm32(gc::ChunkMask), buffer);
  loadPtr(Address(buffer, gc::ChunkStoreBufferOffsetFromLastByte), buffer);
}

void MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr,
                                             Register temp, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  MOZ_ASSERT(ptr != temp);
  MOZ_ASSERT(ptr != ScratchReg &&
             ptr != ScratchReg2);  // Both may be used internally.
  MOZ_ASSERT(temp != ScratchReg && temp != ScratchReg2);

  movePtr(ptr, temp);
  orPtr(Imm32(gc::ChunkMask), temp);
  branchPtr(InvertCondition(cond),
            Address(temp, gc::ChunkStoreBufferOffsetFromLastByte), ImmWord(0),
            label);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              const Address& address,
                                              Register temp, Label* label) {
  branchValueIsNurseryCellImpl(cond, address, temp, label);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              ValueOperand value, Register temp,
                                              Label* label) {
  branchValueIsNurseryCellImpl(cond, value, temp, label);
}

template <typename T>
void MacroAssembler::branchValueIsNurseryCellImpl(Condition cond,
                                                  const T& value, Register temp,
                                                  Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  MOZ_ASSERT(temp != ScratchReg &&
             temp != ScratchReg2);  // Both may be used internally.

  Label done;
  branchTestGCThing(Assembler::NotEqual, value,
                    cond == Assembler::Equal ? &done : label);

  unboxGCThingForGCBarrier(value, temp);
  orPtr(Imm32(gc::ChunkMask), temp);
  branchPtr(InvertCondition(cond),
            Address(temp, gc::ChunkStoreBufferOffsetFromLastByte), ImmWord(0),
            label);

  bind(&done);
}

void MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                     const Value& rhs, Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  vixl::UseScratchRegisterScope temps(this);
  const ARMRegister scratch64 = temps.AcquireX();
  MOZ_ASSERT(scratch64.asUnsized() != lhs.valueReg());
  moveValue(rhs, ValueOperand(scratch64.asUnsized()));
  Cmp(ARMRegister(lhs.valueReg(), 64), scratch64);
  B(label, cond);
}

// ========================================================================
// Memory access primitives.
template <typename T>
void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                       MIRType valueType, const T& dest,
                                       MIRType slotType) {
  if (valueType == MIRType::Double) {
    boxDouble(value.reg().typedReg().fpu(), dest);
    return;
  }

  // For known integers and booleans, we can just store the unboxed value if
  // the slot has the same type.
  if ((valueType == MIRType::Int32 || valueType == MIRType::Boolean) &&
      slotType == valueType) {
    if (value.constant()) {
      Value val = value.value();
      if (valueType == MIRType::Int32) {
        store32(Imm32(val.toInt32()), dest);
      } else {
        store32(Imm32(val.toBoolean() ? 1 : 0), dest);
      }
    } else {
      store32(value.reg().typedReg().gpr(), dest);
    }
    return;
  }

  if (value.constant()) {
    storeValue(value.value(), dest);
  } else {
    storeValue(ValueTypeFromMIRType(valueType), value.reg().typedReg().gpr(),
               dest);
  }
}

template void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                                MIRType valueType,
                                                const Address& dest,
                                                MIRType slotType);
template void MacroAssembler::storeUnboxedValue(
    const ConstantOrRegister& value, MIRType valueType,
    const BaseObjectElementIndex& dest, MIRType slotType);

void MacroAssembler::comment(const char* msg) { Assembler::comment(msg); }

// ========================================================================
// wasm support

CodeOffset MacroAssembler::wasmTrapInstruction() {
  AutoForbidPoolsAndNops afp(this,
                             /* max number of instructions in scope = */ 1);
  CodeOffset offs(currentOffset());
  Unreachable();
  return offs;
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Register boundsCheckLimit,
                                       Label* label) {
  branch32(cond, index, boundsCheckLimit, label);
  if (JitOptions.spectreIndexMasking) {
    csel(ARMRegister(index, 32), vixl::wzr, ARMRegister(index, 32), cond);
  }
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Address boundsCheckLimit, Label* label) {
  branch32(cond, index, boundsCheckLimit, label);
  if (JitOptions.spectreIndexMasking) {
    csel(ARMRegister(index, 32), vixl::wzr, ARMRegister(index, 32), cond);
  }
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Register64 boundsCheckLimit,
                                       Label* label) {
  branchPtr(cond, index.reg, boundsCheckLimit.reg, label);
  if (JitOptions.spectreIndexMasking) {
    csel(ARMRegister(index.reg, 64), vixl::xzr, ARMRegister(index.reg, 64),
         cond);
  }
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Address boundsCheckLimit, Label* label) {
  branchPtr(InvertCondition(cond), boundsCheckLimit, index.reg, label);
  if (JitOptions.spectreIndexMasking) {
    csel(ARMRegister(index.reg, 64), vixl::xzr, ARMRegister(index.reg, 64),
         cond);
  }
}

// FCVTZU behaves as follows:
//
// on NaN it produces zero
// on too large it produces UINT_MAX (for appropriate type)
// on too small it produces zero
//
// FCVTZS behaves as follows:
//
// on NaN it produces zero
// on too large it produces INT_MAX (for appropriate type)
// on too small it produces INT_MIN (ditto)

void MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input_,
                                                Register output_,
                                                bool isSaturating,
                                                Label* oolEntry) {
  ARMRegister output(output_, 32);
  ARMFPRegister input(input_, 64);
  Fcvtzu(output, input);
  if (!isSaturating) {
    Cmp(output, 0);
    Ccmp(output, -1, vixl::ZFlag, Assembler::NotEqual);
    B(oolEntry, Assembler::Equal);
  }
}

void MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input_,
                                                 Register output_,
                                                 bool isSaturating,
                                                 Label* oolEntry) {
  ARMRegister output(output_, 32);
  ARMFPRegister input(input_, 32);
  Fcvtzu(output, input);
  if (!isSaturating) {
    Cmp(output, 0);
    Ccmp(output, -1, vixl::ZFlag, Assembler::NotEqual);
    B(oolEntry, Assembler::Equal);
  }
}

void MacroAssembler::wasmTruncateDoubleToInt32(FloatRegister input_,
                                               Register output_,
                                               bool isSaturating,
                                               Label* oolEntry) {
  ARMRegister output(output_, 32);
  ARMFPRegister input(input_, 64);
  Fcvtzs(output, input);
  if (!isSaturating) {
    Cmp(output, 0);
    Ccmp(output, INT32_MAX, vixl::ZFlag, Assembler::NotEqual);
    Ccmp(output, INT32_MIN, vixl::ZFlag, Assembler::NotEqual);
    B(oolEntry, Assembler::Equal);
  }
}

void MacroAssembler::wasmTruncateFloat32ToInt32(FloatRegister input_,
                                                Register output_,
                                                bool isSaturating,
                                                Label* oolEntry) {
  ARMRegister output(output_, 32);
  ARMFPRegister input(input_, 32);
  Fcvtzs(output, input);
  if (!isSaturating) {
    Cmp(output, 0);
    Ccmp(output, INT32_MAX, vixl::ZFlag, Assembler::NotEqual);
    Ccmp(output, INT32_MIN, vixl::ZFlag, Assembler::NotEqual);
    B(oolEntry, Assembler::Equal);
  }
}

void MacroAssembler::wasmTruncateDoubleToUInt64(
    FloatRegister input_, Register64 output_, bool isSaturating,
    Label* oolEntry, Label* oolRejoin, FloatRegister tempDouble) {
  MOZ_ASSERT(tempDouble.isInvalid());

  ARMRegister output(output_.reg, 64);
  ARMFPRegister input(input_, 64);
  Fcvtzu(output, input);
  if (!isSaturating) {
    Cmp(output, 0);
    Ccmp(output, -1, vixl::ZFlag, Assembler::NotEqual);
    B(oolEntry, Assembler::Equal);
    bind(oolRejoin);
  }
}

void MacroAssembler::wasmTruncateFloat32ToUInt64(
    FloatRegister input_, Register64 output_, bool isSaturating,
    Label* oolEntry, Label* oolRejoin, FloatRegister tempDouble) {
  MOZ_ASSERT(tempDouble.isInvalid());

  ARMRegister output(output_.reg, 64);
  ARMFPRegister input(input_, 32);
  Fcvtzu(output, input);
  if (!isSaturating) {
    Cmp(output, 0);
    Ccmp(output, -1, vixl::ZFlag, Assembler::NotEqual);
    B(oolEntry, Assembler::Equal);
    bind(oolRejoin);
  }
}

void MacroAssembler::wasmTruncateDoubleToInt64(
    FloatRegister input_, Register64 output_, bool isSaturating,
    Label* oolEntry, Label* oolRejoin, FloatRegister tempDouble) {
  MOZ_ASSERT(tempDouble.isInvalid());

  ARMRegister output(output_.reg, 64);
  ARMFPRegister input(input_, 64);
  Fcvtzs(output, input);
  if (!isSaturating) {
    Cmp(output, 0);
    Ccmp(output, INT64_MAX, vixl::ZFlag, Assembler::NotEqual);
    Ccmp(output, INT64_MIN, vixl::ZFlag, Assembler::NotEqual);
    B(oolEntry, Assembler::Equal);
    bind(oolRejoin);
  }
}

void MacroAssembler::wasmTruncateFloat32ToInt64(
    FloatRegister input_, Register64 output_, bool isSaturating,
    Label* oolEntry, Label* oolRejoin, FloatRegister tempDouble) {
  ARMRegister output(output_.reg, 64);
  ARMFPRegister input(input_, 32);
  Fcvtzs(output, input);
  if (!isSaturating) {
    Cmp(output, 0);
    Ccmp(output, INT64_MAX, vixl::ZFlag, Assembler::NotEqual);
    Ccmp(output, INT64_MIN, vixl::ZFlag, Assembler::NotEqual);
    B(oolEntry, Assembler::Equal);
    bind(oolRejoin);
  }
}

void MacroAssembler::oolWasmTruncateCheckF32ToI32(FloatRegister input,
                                                  Register output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  Label notNaN;
  branchFloat(Assembler::DoubleOrdered, input, input, &notNaN);
  wasmTrap(wasm::Trap::InvalidConversionToInteger, off);
  bind(&notNaN);

  Label isOverflow;
  const float two_31 = -float(INT32_MIN);
  ScratchFloat32Scope fpscratch(*this);
  if (flags & TRUNC_UNSIGNED) {
    loadConstantFloat32(two_31 * 2, fpscratch);
    branchFloat(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                &isOverflow);
    loadConstantFloat32(-1.0f, fpscratch);
    branchFloat(Assembler::DoubleGreaterThan, input, fpscratch, rejoin);
  } else {
    loadConstantFloat32(two_31, fpscratch);
    branchFloat(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                &isOverflow);
    loadConstantFloat32(-two_31, fpscratch);
    branchFloat(Assembler::DoubleGreaterThanOrEqual, input, fpscratch, rejoin);
  }
  bind(&isOverflow);
  wasmTrap(wasm::Trap::IntegerOverflow, off);
}

void MacroAssembler::oolWasmTruncateCheckF64ToI32(FloatRegister input,
                                                  Register output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  Label notNaN;
  branchDouble(Assembler::DoubleOrdered, input, input, &notNaN);
  wasmTrap(wasm::Trap::InvalidConversionToInteger, off);
  bind(&notNaN);

  Label isOverflow;
  const double two_31 = -double(INT32_MIN);
  ScratchDoubleScope fpscratch(*this);
  if (flags & TRUNC_UNSIGNED) {
    loadConstantDouble(two_31 * 2, fpscratch);
    branchDouble(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                 &isOverflow);
    loadConstantDouble(-1.0, fpscratch);
    branchDouble(Assembler::DoubleGreaterThan, input, fpscratch, rejoin);
  } else {
    loadConstantDouble(two_31, fpscratch);
    branchDouble(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                 &isOverflow);
    loadConstantDouble(-two_31 - 1, fpscratch);
    branchDouble(Assembler::DoubleGreaterThan, input, fpscratch, rejoin);
  }
  bind(&isOverflow);
  wasmTrap(wasm::Trap::IntegerOverflow, off);
}

void MacroAssembler::oolWasmTruncateCheckF32ToI64(FloatRegister input,
                                                  Register64 output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  Label notNaN;
  branchFloat(Assembler::DoubleOrdered, input, input, &notNaN);
  wasmTrap(wasm::Trap::InvalidConversionToInteger, off);
  bind(&notNaN);

  Label isOverflow;
  const float two_63 = -float(INT64_MIN);
  ScratchFloat32Scope fpscratch(*this);
  if (flags & TRUNC_UNSIGNED) {
    loadConstantFloat32(two_63 * 2, fpscratch);
    branchFloat(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                &isOverflow);
    loadConstantFloat32(-1.0f, fpscratch);
    branchFloat(Assembler::DoubleGreaterThan, input, fpscratch, rejoin);
  } else {
    loadConstantFloat32(two_63, fpscratch);
    branchFloat(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                &isOverflow);
    loadConstantFloat32(-two_63, fpscratch);
    branchFloat(Assembler::DoubleGreaterThanOrEqual, input, fpscratch, rejoin);
  }
  bind(&isOverflow);
  wasmTrap(wasm::Trap::IntegerOverflow, off);
}

void MacroAssembler::oolWasmTruncateCheckF64ToI64(FloatRegister input,
                                                  Register64 output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  Label notNaN;
  branchDouble(Assembler::DoubleOrdered, input, input, &notNaN);
  wasmTrap(wasm::Trap::InvalidConversionToInteger, off);
  bind(&notNaN);

  Label isOverflow;
  const double two_63 = -double(INT64_MIN);
  ScratchDoubleScope fpscratch(*this);
  if (flags & TRUNC_UNSIGNED) {
    loadConstantDouble(two_63 * 2, fpscratch);
    branchDouble(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                 &isOverflow);
    loadConstantDouble(-1.0, fpscratch);
    branchDouble(Assembler::DoubleGreaterThan, input, fpscratch, rejoin);
  } else {
    loadConstantDouble(two_63, fpscratch);
    branchDouble(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                 &isOverflow);
    loadConstantDouble(-two_63, fpscratch);
    branchDouble(Assembler::DoubleGreaterThanOrEqual, input, fpscratch, rejoin);
  }
  bind(&isOverflow);
  wasmTrap(wasm::Trap::IntegerOverflow, off);
}

void MacroAssembler::wasmLoad(const wasm::MemoryAccessDesc& access,
                              Register memoryBase, Register ptr,
                              AnyRegister output) {
  wasmLoadImpl(access, memoryBase, ptr, output, Register64::Invalid());
}

void MacroAssembler::wasmLoadI64(const wasm::MemoryAccessDesc& access,
                                 Register memoryBase, Register ptr,
                                 Register64 output) {
  wasmLoadImpl(access, memoryBase, ptr, AnyRegister(), output);
}

void MacroAssembler::wasmStore(const wasm::MemoryAccessDesc& access,
                               AnyRegister value, Register memoryBase,
                               Register ptr) {
  wasmStoreImpl(access, value, Register64::Invalid(), memoryBase, ptr);
}

void MacroAssembler::wasmStoreI64(const wasm::MemoryAccessDesc& access,
                                  Register64 value, Register memoryBase,
                                  Register ptr) {
  wasmStoreImpl(access, AnyRegister(), value, memoryBase, ptr);
}

void MacroAssembler::enterFakeExitFrameForWasm(Register cxreg, Register scratch,
                                               ExitFrameType type) {
  // Wasm stubs use the native SP, not the PSP.  Setting up the fake exit
  // frame leaves the SP mis-aligned, which is how we want it, but we must do
  // that carefully.

  linkExitFrame(cxreg, scratch);

  MOZ_RELEASE_ASSERT(sp.Is(GetStackPointer64()));

  const ARMRegister tmp(scratch, 64);

  vixl::UseScratchRegisterScope temps(this);
  const ARMRegister tmp2 = temps.AcquireX();

  Sub(sp, sp, 8);

  // Despite the above assertion, it is possible for control to flow from here
  // to the code generated by
  // MacroAssemblerCompat::handleFailureWithHandlerTail without any
  // intervening assignment to PSP.  But handleFailureWithHandlerTail assumes
  // that PSP is the active stack pointer.  Hence the following is necessary
  // for safety.  Note we can't use initPseudoStackPtr here as that would
  // generate no instructions.
  Mov(PseudoStackPointer64, sp);

  Mov(tmp, sp);  // SP may be unaligned, can't use it for memory op
  Mov(tmp2, int32_t(type));
  Str(tmp2, vixl::MemOperand(tmp, 0));
}

// ========================================================================
// Convert floating point.

bool MacroAssembler::convertUInt64ToDoubleNeedsTemp() { return false; }

void MacroAssembler::convertUInt64ToDouble(Register64 src, FloatRegister dest,
                                           Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  Ucvtf(ARMFPRegister(dest, 64), ARMRegister(src.reg, 64));
}

void MacroAssembler::convertInt64ToDouble(Register64 src, FloatRegister dest) {
  Scvtf(ARMFPRegister(dest, 64), ARMRegister(src.reg, 64));
}

void MacroAssembler::convertUInt64ToFloat32(Register64 src, FloatRegister dest,
                                            Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  Ucvtf(ARMFPRegister(dest, 32), ARMRegister(src.reg, 64));
}

void MacroAssembler::convertInt64ToFloat32(Register64 src, FloatRegister dest) {
  Scvtf(ARMFPRegister(dest, 32), ARMRegister(src.reg, 64));
}

void MacroAssembler::convertIntPtrToDouble(Register src, FloatRegister dest) {
  convertInt64ToDouble(Register64(src), dest);
}

// ========================================================================
// Primitive atomic operations.

// The computed MemOperand must be Reg+0 because the load/store exclusive
// instructions only take a single pointer register.

static MemOperand ComputePointerForAtomic(MacroAssembler& masm,
                                          const Address& address,
                                          Register scratch) {
  if (address.offset == 0) {
    return MemOperand(X(masm, address.base), 0);
  }

  masm.Add(X(scratch), X(masm, address.base), address.offset);
  return MemOperand(X(scratch), 0);
}

static MemOperand ComputePointerForAtomic(MacroAssembler& masm,
                                          const BaseIndex& address,
                                          Register scratch) {
  masm.Add(X(scratch), X(masm, address.base),
           Operand(X(address.index), vixl::LSL, address.scale));
  if (address.offset) {
    masm.Add(X(scratch), X(scratch), address.offset);
  }
  return MemOperand(X(scratch), 0);
}

// This sign extends to targetWidth and leaves any higher bits zero.

static void SignOrZeroExtend(MacroAssembler& masm, Scalar::Type srcType,
                             Width targetWidth, Register src, Register dest) {
  bool signExtend = Scalar::isSignedIntType(srcType);

  switch (Scalar::byteSize(srcType)) {
    case 1:
      if (signExtend) {
        masm.Sbfm(R(dest, targetWidth), R(src, targetWidth), 0, 7);
      } else {
        masm.Ubfm(R(dest, targetWidth), R(src, targetWidth), 0, 7);
      }
      break;
    case 2:
      if (signExtend) {
        masm.Sbfm(R(dest, targetWidth), R(src, targetWidth), 0, 15);
      } else {
        masm.Ubfm(R(dest, targetWidth), R(src, targetWidth), 0, 15);
      }
      break;
    case 4:
      if (targetWidth == Width::_64) {
        if (signExtend) {
          masm.Sbfm(X(dest), X(src), 0, 31);
        } else {
          masm.Ubfm(X(dest), X(src), 0, 31);
        }
      } else if (src != dest) {
        masm.Mov(R(dest, targetWidth), R(src, targetWidth));
      }
      break;
    case 8:
      if (src != dest) {
        masm.Mov(R(dest, targetWidth), R(src, targetWidth));
      }
      break;
    default:
      MOZ_CRASH();
  }
}

// Exclusive-loads zero-extend their values to the full width of the X register.
//
// Note, we've promised to leave the high bits of the 64-bit register clear if
// the targetWidth is 32.

static void LoadExclusive(MacroAssembler& masm,
                          const wasm::MemoryAccessDesc* access,
                          Scalar::Type srcType, Width targetWidth,
                          MemOperand ptr, Register dest) {
  bool signExtend = Scalar::isSignedIntType(srcType);

  // With this address form, a single native ldxr* will be emitted, and the
  // AutoForbidPoolsAndNops ensures that the metadata is emitted at the address
  // of the ldxr*.
  MOZ_ASSERT(ptr.IsImmediateOffset() && ptr.offset() == 0);

  switch (Scalar::byteSize(srcType)) {
    case 1: {
      {
        AutoForbidPoolsAndNops afp(
            &masm,
            /* max number of instructions in scope = */ 1);
        if (access) {
          masm.append(*access, masm.currentOffset());
        }
        masm.Ldxrb(W(dest), ptr);
      }
      if (signExtend) {
        masm.Sbfm(R(dest, targetWidth), R(dest, targetWidth), 0, 7);
      }
      break;
    }
    case 2: {
      {
        AutoForbidPoolsAndNops afp(
            &masm,
            /* max number of instructions in scope = */ 1);
        if (access) {
          masm.append(*access, masm.currentOffset());
        }
        masm.Ldxrh(W(dest), ptr);
      }
      if (signExtend) {
        masm.Sbfm(R(dest, targetWidth), R(dest, targetWidth), 0, 15);
      }
      break;
    }
    case 4: {
      {
        AutoForbidPoolsAndNops afp(
            &masm,
            /* max number of instructions in scope = */ 1);
        if (access) {
          masm.append(*access, masm.currentOffset());
        }
        masm.Ldxr(W(dest), ptr);
      }
      if (targetWidth == Width::_64 && signExtend) {
        masm.Sbfm(X(dest), X(dest), 0, 31);
      }
      break;
    }
    case 8: {
      {
        AutoForbidPoolsAndNops afp(
            &masm,
            /* max number of instructions in scope = */ 1);
        if (access) {
          masm.append(*access, masm.currentOffset());
        }
        masm.Ldxr(X(dest), ptr);
      }
      break;
    }
    default: {
      MOZ_CRASH();
    }
  }
}

static void StoreExclusive(MacroAssembler& masm, Scalar::Type type,
                           Register status, Register src, MemOperand ptr) {
  switch (Scalar::byteSize(type)) {
    case 1:
      masm.Stxrb(W(status), W(src), ptr);
      break;
    case 2:
      masm.Stxrh(W(status), W(src), ptr);
      break;
    case 4:
      masm.Stxr(W(status), W(src), ptr);
      break;
    case 8:
      masm.Stxr(W(status), X(src), ptr);
      break;
  }
}

template <typename T>
static void CompareExchange(MacroAssembler& masm,
                            const wasm::MemoryAccessDesc* access,
                            Scalar::Type type, Width targetWidth,
                            const Synchronization& sync, const T& mem,
                            Register oldval, Register newval, Register output) {
  MOZ_ASSERT(oldval != output && newval != output);

  Label again;
  Label done;

  vixl::UseScratchRegisterScope temps(&masm);

  Register scratch2 = temps.AcquireX().asUnsized();
  MemOperand ptr = ComputePointerForAtomic(masm, mem, scratch2);

  MOZ_ASSERT(ptr.base().asUnsized() != output);

  masm.memoryBarrierBefore(sync);

  Register scratch = temps.AcquireX().asUnsized();

  masm.bind(&again);
  SignOrZeroExtend(masm, type, targetWidth, oldval, scratch);
  LoadExclusive(masm, access, type, targetWidth, ptr, output);
  masm.Cmp(R(output, targetWidth), R(scratch, targetWidth));
  masm.B(&done, MacroAssembler::NotEqual);
  StoreExclusive(masm, type, scratch, newval, ptr);
  masm.Cbnz(W(scratch), &again);
  masm.bind(&done);

  masm.memoryBarrierAfter(sync);
}

template <typename T>
static void AtomicExchange(MacroAssembler& masm,
                           const wasm::MemoryAccessDesc* access,
                           Scalar::Type type, Width targetWidth,
                           const Synchronization& sync, const T& mem,
                           Register value, Register output) {
  MOZ_ASSERT(value != output);

  Label again;

  vixl::UseScratchRegisterScope temps(&masm);

  Register scratch2 = temps.AcquireX().asUnsized();
  MemOperand ptr = ComputePointerForAtomic(masm, mem, scratch2);

  masm.memoryBarrierBefore(sync);

  Register scratch = temps.AcquireX().asUnsized();

  masm.bind(&again);
  LoadExclusive(masm, access, type, targetWidth, ptr, output);
  StoreExclusive(masm, type, scratch, value, ptr);
  masm.Cbnz(W(scratch), &again);

  masm.memoryBarrierAfter(sync);
}

template <bool wantResult, typename T>
static void AtomicFetchOp(MacroAssembler& masm,
                          const wasm::MemoryAccessDesc* access,
                          Scalar::Type type, Width targetWidth,
                          const Synchronization& sync, AtomicOp op,
                          const T& mem, Register value, Register temp,
                          Register output) {
  MOZ_ASSERT(value != output);
  MOZ_ASSERT(value != temp);
  MOZ_ASSERT_IF(wantResult, output != temp);

  Label again;

  vixl::UseScratchRegisterScope temps(&masm);

  Register scratch2 = temps.AcquireX().asUnsized();
  MemOperand ptr = ComputePointerForAtomic(masm, mem, scratch2);

  masm.memoryBarrierBefore(sync);

  Register scratch = temps.AcquireX().asUnsized();

  masm.bind(&again);
  LoadExclusive(masm, access, type, targetWidth, ptr, output);
  switch (op) {
    case AtomicFetchAddOp:
      masm.Add(X(temp), X(output), X(value));
      break;
    case AtomicFetchSubOp:
      masm.Sub(X(temp), X(output), X(value));
      break;
    case AtomicFetchAndOp:
      masm.And(X(temp), X(output), X(value));
      break;
    case AtomicFetchOrOp:
      masm.Orr(X(temp), X(output), X(value));
      break;
    case AtomicFetchXorOp:
      masm.Eor(X(temp), X(output), X(value));
      break;
  }
  StoreExclusive(masm, type, scratch, temp, ptr);
  masm.Cbnz(W(scratch), &again);
  if (wantResult) {
    SignOrZeroExtend(masm, type, targetWidth, output, output);
  }

  masm.memoryBarrierAfter(sync);
}

void MacroAssembler::compareExchange(Scalar::Type type,
                                     const Synchronization& sync,
                                     const Address& mem, Register oldval,
                                     Register newval, Register output) {
  CompareExchange(*this, nullptr, type, Width::_32, sync, mem, oldval, newval,
                  output);
}

void MacroAssembler::compareExchange(Scalar::Type type,
                                     const Synchronization& sync,
                                     const BaseIndex& mem, Register oldval,
                                     Register newval, Register output) {
  CompareExchange(*this, nullptr, type, Width::_32, sync, mem, oldval, newval,
                  output);
}

void MacroAssembler::compareExchange64(const Synchronization& sync,
                                       const Address& mem, Register64 expect,
                                       Register64 replace, Register64 output) {
  CompareExchange(*this, nullptr, Scalar::Int64, Width::_64, sync, mem,
                  expect.reg, replace.reg, output.reg);
}

void MacroAssembler::compareExchange64(const Synchronization& sync,
                                       const BaseIndex& mem, Register64 expect,
                                       Register64 replace, Register64 output) {
  CompareExchange(*this, nullptr, Scalar::Int64, Width::_64, sync, mem,
                  expect.reg, replace.reg, output.reg);
}

void MacroAssembler::atomicExchange64(const Synchronization& sync,
                                      const Address& mem, Register64 value,
                                      Register64 output) {
  AtomicExchange(*this, nullptr, Scalar::Int64, Width::_64, sync, mem,
                 value.reg, output.reg);
}

void MacroAssembler::atomicExchange64(const Synchronization& sync,
                                      const BaseIndex& mem, Register64 value,
                                      Register64 output) {
  AtomicExchange(*this, nullptr, Scalar::Int64, Width::_64, sync, mem,
                 value.reg, output.reg);
}

void MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                                     Register64 value, const Address& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp<true>(*this, nullptr, Scalar::Int64, Width::_64, sync, op, mem,
                      value.reg, temp.reg, output.reg);
}

void MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                                     Register64 value, const BaseIndex& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp<true>(*this, nullptr, Scalar::Int64, Width::_64, sync, op, mem,
                      value.reg, temp.reg, output.reg);
}

void MacroAssembler::atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                                      Register64 value, const Address& mem,
                                      Register64 temp) {
  AtomicFetchOp<false>(*this, nullptr, Scalar::Int64, Width::_64, sync, op, mem,
                       value.reg, temp.reg, temp.reg);
}

void MacroAssembler::atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                                      Register64 value, const BaseIndex& mem,
                                      Register64 temp) {
  AtomicFetchOp<false>(*this, nullptr, Scalar::Int64, Width::_64, sync, op, mem,
                       value.reg, temp.reg, temp.reg);
}

void MacroAssembler::wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                                         const Address& mem, Register oldval,
                                         Register newval, Register output) {
  CompareExchange(*this, &access, access.type(), Width::_32, access.sync(), mem,
                  oldval, newval, output);
}

void MacroAssembler::wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                                         const BaseIndex& mem, Register oldval,
                                         Register newval, Register output) {
  CompareExchange(*this, &access, access.type(), Width::_32, access.sync(), mem,
                  oldval, newval, output);
}

void MacroAssembler::atomicExchange(Scalar::Type type,
                                    const Synchronization& sync,
                                    const Address& mem, Register value,
                                    Register output) {
  AtomicExchange(*this, nullptr, type, Width::_32, sync, mem, value, output);
}

void MacroAssembler::atomicExchange(Scalar::Type type,
                                    const Synchronization& sync,
                                    const BaseIndex& mem, Register value,
                                    Register output) {
  AtomicExchange(*this, nullptr, type, Width::_32, sync, mem, value, output);
}

void MacroAssembler::wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                                        const Address& mem, Register value,
                                        Register output) {
  AtomicExchange(*this, &access, access.type(), Width::_32, access.sync(), mem,
                 value, output);
}

void MacroAssembler::wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                                        const BaseIndex& mem, Register value,
                                        Register output) {
  AtomicExchange(*this, &access, access.type(), Width::_32, access.sync(), mem,
                 value, output);
}

void MacroAssembler::atomicFetchOp(Scalar::Type type,
                                   const Synchronization& sync, AtomicOp op,
                                   Register value, const Address& mem,
                                   Register temp, Register output) {
  AtomicFetchOp<true>(*this, nullptr, type, Width::_32, sync, op, mem, value,
                      temp, output);
}

void MacroAssembler::atomicFetchOp(Scalar::Type type,
                                   const Synchronization& sync, AtomicOp op,
                                   Register value, const BaseIndex& mem,
                                   Register temp, Register output) {
  AtomicFetchOp<true>(*this, nullptr, type, Width::_32, sync, op, mem, value,
                      temp, output);
}

void MacroAssembler::wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access,
                                       AtomicOp op, Register value,
                                       const Address& mem, Register temp,
                                       Register output) {
  AtomicFetchOp<true>(*this, &access, access.type(), Width::_32, access.sync(),
                      op, mem, value, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access,
                                       AtomicOp op, Register value,
                                       const BaseIndex& mem, Register temp,
                                       Register output) {
  AtomicFetchOp<true>(*this, &access, access.type(), Width::_32, access.sync(),
                      op, mem, value, temp, output);
}

void MacroAssembler::wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access,
                                        AtomicOp op, Register value,
                                        const Address& mem, Register temp) {
  AtomicFetchOp<false>(*this, &access, access.type(), Width::_32, access.sync(),
                       op, mem, value, temp, temp);
}

void MacroAssembler::wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access,
                                        AtomicOp op, Register value,
                                        const BaseIndex& mem, Register temp) {
  AtomicFetchOp<false>(*this, &access, access.type(), Width::_32, access.sync(),
                       op, mem, value, temp, temp);
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const Address& mem,
                                           Register64 expect,
                                           Register64 replace,
                                           Register64 output) {
  CompareExchange(*this, &access, Scalar::Int64, Width::_64, access.sync(), mem,
                  expect.reg, replace.reg, output.reg);
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const BaseIndex& mem,
                                           Register64 expect,
                                           Register64 replace,
                                           Register64 output) {
  CompareExchange(*this, &access, Scalar::Int64, Width::_64, access.sync(), mem,
                  expect.reg, replace.reg, output.reg);
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const Address& mem, Register64 value,
                                          Register64 output) {
  AtomicExchange(*this, &access, Scalar::Int64, Width::_64, access.sync(), mem,
                 value.reg, output.reg);
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const BaseIndex& mem,
                                          Register64 value, Register64 output) {
  AtomicExchange(*this, &access, Scalar::Int64, Width::_64, access.sync(), mem,
                 value.reg, output.reg);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const Address& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp<true>(*this, &access, Scalar::Int64, Width::_64, access.sync(),
                      op, mem, value.reg, temp.reg, output.reg);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const BaseIndex& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp<true>(*this, &access, Scalar::Int64, Width::_64, access.sync(),
                      op, mem, value.reg, temp.reg, output.reg);
}

void MacroAssembler::wasmAtomicEffectOp64(const wasm::MemoryAccessDesc& access,
                                          AtomicOp op, Register64 value,
                                          const BaseIndex& mem,
                                          Register64 temp) {
  AtomicFetchOp<false>(*this, &access, Scalar::Int64, Width::_64, access.sync(),
                       op, mem, value.reg, temp.reg, temp.reg);
}

// ========================================================================
// JS atomic operations.

template <typename T>
static void CompareExchangeJS(MacroAssembler& masm, Scalar::Type arrayType,
                              const Synchronization& sync, const T& mem,
                              Register oldval, Register newval, Register temp,
                              AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.compareExchange(arrayType, sync, mem, oldval, newval, temp);
    masm.convertUInt32ToDouble(temp, output.fpu());
  } else {
    masm.compareExchange(arrayType, sync, mem, oldval, newval, output.gpr());
  }
}

void MacroAssembler::compareExchangeJS(Scalar::Type arrayType,
                                       const Synchronization& sync,
                                       const Address& mem, Register oldval,
                                       Register newval, Register temp,
                                       AnyRegister output) {
  CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

void MacroAssembler::compareExchangeJS(Scalar::Type arrayType,
                                       const Synchronization& sync,
                                       const BaseIndex& mem, Register oldval,
                                       Register newval, Register temp,
                                       AnyRegister output) {
  CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

template <typename T>
static void AtomicExchangeJS(MacroAssembler& masm, Scalar::Type arrayType,
                             const Synchronization& sync, const T& mem,
                             Register value, Register temp,
                             AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.atomicExchange(arrayType, sync, mem, value, temp);
    masm.convertUInt32ToDouble(temp, output.fpu());
  } else {
    masm.atomicExchange(arrayType, sync, mem, value, output.gpr());
  }
}

void MacroAssembler::atomicExchangeJS(Scalar::Type arrayType,
                                      const Synchronization& sync,
                                      const Address& mem, Register value,
                                      Register temp, AnyRegister output) {
  AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

void MacroAssembler::atomicExchangeJS(Scalar::Type arrayType,
                                      const Synchronization& sync,
                                      const BaseIndex& mem, Register value,
                                      Register temp, AnyRegister output) {
  AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

template <typename T>
static void AtomicFetchOpJS(MacroAssembler& masm, Scalar::Type arrayType,
                            const Synchronization& sync, AtomicOp op,
                            Register value, const T& mem, Register temp1,
                            Register temp2, AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp2, temp1);
    masm.convertUInt32ToDouble(temp1, output.fpu());
  } else {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp1, output.gpr());
  }
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     const Synchronization& sync, AtomicOp op,
                                     Register value, const Address& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     const Synchronization& sync, AtomicOp op,
                                     Register value, const BaseIndex& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType,
                                      const Synchronization& sync, AtomicOp op,
                                      Register value, const BaseIndex& mem,
                                      Register temp) {
  AtomicFetchOp<false>(*this, nullptr, arrayType, Width::_32, sync, op, mem,
                       value, temp, temp);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType,
                                      const Synchronization& sync, AtomicOp op,
                                      Register value, const Address& mem,
                                      Register temp) {
  AtomicFetchOp<false>(*this, nullptr, arrayType, Width::_32, sync, op, mem,
                       value, temp, temp);
}

void MacroAssembler::flexibleQuotient32(Register rhs, Register srcDest,
                                        bool isUnsigned,
                                        const LiveRegisterSet&) {
  quotient32(rhs, srcDest, isUnsigned);
}

void MacroAssembler::flexibleRemainder32(Register rhs, Register srcDest,
                                         bool isUnsigned,
                                         const LiveRegisterSet&) {
  remainder32(rhs, srcDest, isUnsigned);
}

void MacroAssembler::flexibleDivMod32(Register rhs, Register srcDest,
                                      Register remOutput, bool isUnsigned,
                                      const LiveRegisterSet&) {
  vixl::UseScratchRegisterScope temps(this);
  ARMRegister scratch = temps.AcquireW();
  ARMRegister src = temps.AcquireW();

  // Preserve src for remainder computation
  Mov(src, ARMRegister(srcDest, 32));

  if (isUnsigned) {
    Udiv(ARMRegister(srcDest, 32), src, ARMRegister(rhs, 32));
  } else {
    Sdiv(ARMRegister(srcDest, 32), src, ARMRegister(rhs, 32));
  }
  // Compute remainder
  Mul(scratch, ARMRegister(srcDest, 32), ARMRegister(rhs, 32));
  Sub(ARMRegister(remOutput, 32), src, scratch);
}

CodeOffset MacroAssembler::moveNearAddressWithPatch(Register dest) {
  AutoForbidPoolsAndNops afp(this,
                             /* max number of instructions in scope = */ 1);
  CodeOffset offset(currentOffset());
  adr(ARMRegister(dest, 64), 0, LabelDoc());
  return offset;
}

void MacroAssembler::patchNearAddressMove(CodeLocationLabel loc,
                                          CodeLocationLabel target) {
  ptrdiff_t off = target - loc;
  MOZ_RELEASE_ASSERT(vixl::IsInt21(off));

  Instruction* cur = reinterpret_cast<Instruction*>(loc.raw());
  MOZ_ASSERT(cur->IsADR());

  vixl::Register rd = vixl::Register::XRegFromCode(cur->Rd());
  adr(cur, rd, off);
}

// ========================================================================
// Spectre Mitigations.

void MacroAssembler::speculationBarrier() {
  // Conditional speculation barrier.
  csdb();
}

void MacroAssembler::floorFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail) {
  floorf(src, dest, fail);
}

void MacroAssembler::floorDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  floor(src, dest, fail);
}

void MacroAssembler::ceilFloat32ToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  ceilf(src, dest, fail);
}

void MacroAssembler::ceilDoubleToInt32(FloatRegister src, Register dest,
                                       Label* fail) {
  ceil(src, dest, fail);
}

void MacroAssembler::truncFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail) {
  const ARMFPRegister src32(src, 32);

  Label done, zeroCase;

  // Convert scalar to signed 32-bit fixed-point, rounding toward zero.
  // In the case of overflow, the output is saturated.
  // In the case of NaN and -0, the output is zero.
  Fcvtzs(ARMRegister(dest, 32), src32);

  // If the output was zero, worry about special cases.
  branch32(Assembler::Equal, dest, Imm32(0), &zeroCase);

  // Fail on overflow cases.
  branch32(Assembler::Equal, dest, Imm32(INT_MAX), fail);
  branch32(Assembler::Equal, dest, Imm32(INT_MIN), fail);

  // If the output was non-zero and wasn't saturated, just return it.
  jump(&done);

  // Handle the case of a zero output:
  // 1. The input may have been NaN, requiring a failure.
  // 2. The input may have been in (-1,-0], requiring a failure.
  {
    bind(&zeroCase);

    // If input is a negative number that truncated to zero, the real
    // output should be the non-integer -0.
    // The use of "lt" instead of "lo" also catches unordered NaN input.
    Fcmp(src32, 0.0f);
    B(fail, vixl::lt);

    // Check explicitly for -0, bitwise.
    Fmov(ARMRegister(dest, 32), src32);
    branchTest32(Assembler::Signed, dest, dest, fail);
    move32(Imm32(0), dest);
  }

  bind(&done);
}

void MacroAssembler::truncDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  const ARMFPRegister src64(src, 64);

  Label done, zeroCase;

  // Convert scalar to signed 32-bit fixed-point, rounding toward zero.
  // In the case of overflow, the output is saturated.
  // In the case of NaN and -0, the output is zero.
  Fcvtzs(ARMRegister(dest, 32), src64);

  // If the output was zero, worry about special cases.
  branch32(Assembler::Equal, dest, Imm32(0), &zeroCase);

  // Fail on overflow cases.
  branch32(Assembler::Equal, dest, Imm32(INT_MAX), fail);
  branch32(Assembler::Equal, dest, Imm32(INT_MIN), fail);

  // If the output was non-zero and wasn't saturated, just return it.
  jump(&done);

  // Handle the case of a zero output:
  // 1. The input may have been NaN, requiring a failure.
  // 2. The input may have been in (-1,-0], requiring a failure.
  {
    bind(&zeroCase);

    // If input is a negative number that truncated to zero, the real
    // output should be the non-integer -0.
    // The use of "lt" instead of "lo" also catches unordered NaN input.
    Fcmp(src64, 0.0);
    B(fail, vixl::lt);

    // Check explicitly for -0, bitwise.
    Fmov(ARMRegister(dest, 64), src64);
    branchTestPtr(Assembler::Signed, dest, dest, fail);
    movePtr(ImmPtr(0), dest);
  }

  bind(&done);
}

void MacroAssembler::roundFloat32ToInt32(FloatRegister src, Register dest,
                                         FloatRegister temp, Label* fail) {
  const ARMFPRegister src32(src, 32);
  ScratchFloat32Scope scratch(*this);

  Label negative, done;

  // Branch to a slow path if input < 0.0 due to complicated rounding rules.
  // Note that Fcmp with NaN unsets the negative flag.
  Fcmp(src32, 0.0);
  B(&negative, Assembler::Condition::lo);

  // Handle the simple case of a positive input, and also -0 and NaN.
  // Rounding proceeds with consideration of the fractional part of the input:
  // 1. If > 0.5, round to integer with higher absolute value (so, up).
  // 2. If < 0.5, round to integer with lower absolute value (so, down).
  // 3. If = 0.5, round to +Infinity (so, up).
  {
    // Convert to signed 32-bit integer, rounding halfway cases away from zero.
    // In the case of overflow, the output is saturated.
    // In the case of NaN and -0, the output is zero.
    Fcvtas(ARMRegister(dest, 32), src32);
    // If the output potentially saturated, fail.
    branch32(Assembler::Equal, dest, Imm32(INT_MAX), fail);

    // If the result of the rounding was non-zero, return the output.
    // In the case of zero, the input may have been NaN or -0, which must bail.
    branch32(Assembler::NotEqual, dest, Imm32(0), &done);
    {
      // If input is NaN, comparisons set the C and V bits of the NZCV flags.
      Fcmp(src32, 0.0f);
      B(fail, Assembler::Overflow);

      // Move all 32 bits of the input into a scratch register to check for -0.
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratchGPR32 = temps.AcquireW();
      Fmov(scratchGPR32, src32);
      Cmp(scratchGPR32, vixl::Operand(uint32_t(0x80000000)));
      B(fail, Assembler::Equal);
    }

    jump(&done);
  }

  // Handle the complicated case of a negative input.
  // Rounding proceeds with consideration of the fractional part of the input:
  // 1. If > 0.5, round to integer with higher absolute value (so, down).
  // 2. If < 0.5, round to integer with lower absolute value (so, up).
  // 3. If = 0.5, round to +Infinity (so, up).
  bind(&negative);
  {
    // Inputs in [-0.5, 0) need 0.5 added; other negative inputs need
    // the biggest double less than 0.5.
    Label join;
    loadConstantFloat32(GetBiggestNumberLessThan(0.5f), temp);
    loadConstantFloat32(-0.5f, scratch);
    branchFloat(Assembler::DoubleLessThan, src, scratch, &join);
    loadConstantFloat32(0.5f, temp);
    bind(&join);

    addFloat32(src, temp);
    // Round all values toward -Infinity.
    // In the case of overflow, the output is saturated.
    // NaN and -0 are already handled by the "positive number" path above.
    Fcvtms(ARMRegister(dest, 32), temp);
    // If the output potentially saturated, fail.
    branch32(Assembler::Equal, dest, Imm32(INT_MIN), fail);

    // If output is zero, then the actual result is -0. Fail.
    branchTest32(Assembler::Zero, dest, dest, fail);
  }

  bind(&done);
}

void MacroAssembler::roundDoubleToInt32(FloatRegister src, Register dest,
                                        FloatRegister temp, Label* fail) {
  const ARMFPRegister src64(src, 64);
  ScratchDoubleScope scratch(*this);

  Label negative, done;

  // Branch to a slow path if input < 0.0 due to complicated rounding rules.
  // Note that Fcmp with NaN unsets the negative flag.
  Fcmp(src64, 0.0);
  B(&negative, Assembler::Condition::lo);

  // Handle the simple case of a positive input, and also -0 and NaN.
  // Rounding proceeds with consideration of the fractional part of the input:
  // 1. If > 0.5, round to integer with higher absolute value (so, up).
  // 2. If < 0.5, round to integer with lower absolute value (so, down).
  // 3. If = 0.5, round to +Infinity (so, up).
  {
    // Convert to signed 32-bit integer, rounding halfway cases away from zero.
    // In the case of overflow, the output is saturated.
    // In the case of NaN and -0, the output is zero.
    Fcvtas(ARMRegister(dest, 32), src64);
    // If the output potentially saturated, fail.
    branch32(Assembler::Equal, dest, Imm32(INT_MAX), fail);

    // If the result of the rounding was non-zero, return the output.
    // In the case of zero, the input may have been NaN or -0, which must bail.
    branch32(Assembler::NotEqual, dest, Imm32(0), &done);
    {
      // If input is NaN, comparisons set the C and V bits of the NZCV flags.
      Fcmp(src64, 0.0);
      B(fail, Assembler::Overflow);

      // Move all 64 bits of the input into a scratch register to check for -0.
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratchGPR64 = temps.AcquireX();
      Fmov(scratchGPR64, src64);
      Cmp(scratchGPR64, vixl::Operand(uint64_t(0x8000000000000000)));
      B(fail, Assembler::Equal);
    }

    jump(&done);
  }

  // Handle the complicated case of a negative input.
  // Rounding proceeds with consideration of the fractional part of the input:
  // 1. If > 0.5, round to integer with higher absolute value (so, down).
  // 2. If < 0.5, round to integer with lower absolute value (so, up).
  // 3. If = 0.5, round to +Infinity (so, up).
  bind(&negative);
  {
    // Inputs in [-0.5, 0) need 0.5 added; other negative inputs need
    // the biggest double less than 0.5.
    Label join;
    loadConstantDouble(GetBiggestNumberLessThan(0.5), temp);
    loadConstantDouble(-0.5, scratch);
    branchDouble(Assembler::DoubleLessThan, src, scratch, &join);
    loadConstantDouble(0.5, temp);
    bind(&join);

    addDouble(src, temp);
    // Round all values toward -Infinity.
    // In the case of overflow, the output is saturated.
    // NaN and -0 are already handled by the "positive number" path above.
    Fcvtms(ARMRegister(dest, 32), temp);
    // If the output potentially saturated, fail.
    branch32(Assembler::Equal, dest, Imm32(INT_MIN), fail);

    // If output is zero, then the actual result is -0. Fail.
    branchTest32(Assembler::Zero, dest, dest, fail);
  }

  bind(&done);
}

void MacroAssembler::nearbyIntDouble(RoundingMode mode, FloatRegister src,
                                     FloatRegister dest) {
  switch (mode) {
    case RoundingMode::Up:
      frintp(ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
      return;
    case RoundingMode::Down:
      frintm(ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
      return;
    case RoundingMode::NearestTiesToEven:
      frintn(ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
      return;
    case RoundingMode::TowardsZero:
      frintz(ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
      return;
  }
  MOZ_CRASH("unexpected mode");
}

void MacroAssembler::nearbyIntFloat32(RoundingMode mode, FloatRegister src,
                                      FloatRegister dest) {
  switch (mode) {
    case RoundingMode::Up:
      frintp(ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
      return;
    case RoundingMode::Down:
      frintm(ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
      return;
    case RoundingMode::NearestTiesToEven:
      frintn(ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
      return;
    case RoundingMode::TowardsZero:
      frintz(ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
      return;
  }
  MOZ_CRASH("unexpected mode");
}

void MacroAssembler::copySignDouble(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister output) {
  ScratchDoubleScope scratch(*this);

  // Double with only the sign bit set (= negative zero).
  loadConstantDouble(0, scratch);
  negateDouble(scratch);

  if (lhs != output) {
    moveDouble(lhs, output);
  }

  bit(ARMFPRegister(output.encoding(), vixl::VectorFormat::kFormat8B),
      ARMFPRegister(rhs.encoding(), vixl::VectorFormat::kFormat8B),
      ARMFPRegister(scratch.encoding(), vixl::VectorFormat::kFormat8B));
}

void MacroAssembler::copySignFloat32(FloatRegister lhs, FloatRegister rhs,
                                     FloatRegister output) {
  ScratchFloat32Scope scratch(*this);

  // Float with only the sign bit set (= negative zero).
  loadConstantFloat32(0, scratch);
  negateFloat(scratch);

  if (lhs != output) {
    moveFloat32(lhs, output);
  }

  bit(ARMFPRegister(output.encoding(), vixl::VectorFormat::kFormat8B),
      ARMFPRegister(rhs.encoding(), vixl::VectorFormat::kFormat8B),
      ARMFPRegister(scratch.encoding(), vixl::VectorFormat::kFormat8B));
}

//}}} check_macroassembler_style

}  // namespace jit
}  // namespace js
