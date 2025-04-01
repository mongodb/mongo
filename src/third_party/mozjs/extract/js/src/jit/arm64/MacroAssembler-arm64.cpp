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
#include "vm/BigIntType.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"
#include "vm/StringType.h"
#include "wasm/WasmStubs.h"

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

#ifdef ENABLE_WASM_SIMD
bool MacroAssembler::MustMaskShiftCountSimd128(wasm::SimdOp op, int32_t* mask) {
  switch (op) {
    case wasm::SimdOp::I8x16Shl:
    case wasm::SimdOp::I8x16ShrU:
    case wasm::SimdOp::I8x16ShrS:
      *mask = 7;
      break;
    case wasm::SimdOp::I16x8Shl:
    case wasm::SimdOp::I16x8ShrU:
    case wasm::SimdOp::I16x8ShrS:
      *mask = 15;
      break;
    case wasm::SimdOp::I32x4Shl:
    case wasm::SimdOp::I32x4ShrU:
    case wasm::SimdOp::I32x4ShrS:
      *mask = 31;
      break;
    case wasm::SimdOp::I64x2Shl:
    case wasm::SimdOp::I64x2ShrU:
    case wasm::SimdOp::I64x2ShrS:
      *mask = 63;
      break;
    default:
      MOZ_CRASH("Unexpected shift operation");
  }
  return true;
}
#endif

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

void MacroAssemblerCompat::handleFailureWithHandlerTail(Label* profilerExitTail,
                                                        Label* bailoutTail) {
  // Fail rather than silently create wrong code.
  MOZ_RELEASE_ASSERT(GetStackPointer64().Is(PseudoStackPointer64));

  // Reserve space for exception information.
  int64_t size = (sizeof(ResumeFromException) + 7) & ~7;
  Sub(PseudoStackPointer64, PseudoStackPointer64, Operand(size));
  syncStackPtr();

  MOZ_ASSERT(!x0.Is(PseudoStackPointer64));
  Mov(x0, PseudoStackPointer64);

  // Call the handler.
  using Fn = void (*)(ResumeFromException* rfe);
  asMasm().setupUnalignedABICall(r1);
  asMasm().passABIArg(r0);
  asMasm().callWithABI<Fn, HandleException>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  Label entryFrame;
  Label catch_;
  Label finally;
  Label returnBaseline;
  Label returnIon;
  Label bailout;
  Label wasm;
  Label wasmCatch;

  // Check the `asMasm` calls above didn't mess with the StackPointer identity.
  MOZ_ASSERT(GetStackPointer64().Is(PseudoStackPointer64));

  loadPtr(Address(PseudoStackPointer, ResumeFromException::offsetOfKind()), r0);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ExceptionResumeKind::EntryFrame), &entryFrame);
  asMasm().branch32(Assembler::Equal, r0, Imm32(ExceptionResumeKind::Catch),
                    &catch_);
  asMasm().branch32(Assembler::Equal, r0, Imm32(ExceptionResumeKind::Finally),
                    &finally);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ExceptionResumeKind::ForcedReturnBaseline),
                    &returnBaseline);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ExceptionResumeKind::ForcedReturnIon), &returnIon);
  asMasm().branch32(Assembler::Equal, r0, Imm32(ExceptionResumeKind::Bailout),
                    &bailout);
  asMasm().branch32(Assembler::Equal, r0, Imm32(ExceptionResumeKind::Wasm),
                    &wasm);
  asMasm().branch32(Assembler::Equal, r0, Imm32(ExceptionResumeKind::WasmCatch),
                    &wasmCatch);

  breakpoint();  // Invalid kind.

  // No exception handler. Load the error value, restore state and return from
  // the entry frame.
  bind(&entryFrame);
  moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
  loadPtr(
      Address(PseudoStackPointer, ResumeFromException::offsetOfFramePointer()),
      FramePointer);
  loadPtr(
      Address(PseudoStackPointer, ResumeFromException::offsetOfStackPointer()),
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
  loadPtr(Address(PseudoStackPointer, ResumeFromException::offsetOfTarget()),
          r0);
  loadPtr(
      Address(PseudoStackPointer, ResumeFromException::offsetOfFramePointer()),
      FramePointer);
  loadPtr(
      Address(PseudoStackPointer, ResumeFromException::offsetOfStackPointer()),
      PseudoStackPointer);
  syncStackPtr();
  Br(x0);

  // If we found a finally block, this must be a baseline frame. Push three
  // values expected by the finally block: the exception, the exception stack,
  // and BooleanValue(true).
  bind(&finally);
  ARMRegister exception = x1;
  Ldr(exception, MemOperand(PseudoStackPointer64,
                            ResumeFromException::offsetOfException()));

  ARMRegister exceptionStack = x2;
  Ldr(exceptionStack,
      MemOperand(PseudoStackPointer64,
                 ResumeFromException::offsetOfExceptionStack()));

  Ldr(x0,
      MemOperand(PseudoStackPointer64, ResumeFromException::offsetOfTarget()));
  Ldr(ARMRegister(FramePointer, 64),
      MemOperand(PseudoStackPointer64,
                 ResumeFromException::offsetOfFramePointer()));
  Ldr(PseudoStackPointer64,
      MemOperand(PseudoStackPointer64,
                 ResumeFromException::offsetOfStackPointer()));
  syncStackPtr();
  push(exception);
  push(exceptionStack);
  pushValue(BooleanValue(true));
  Br(x0);

  // Return BaselineFrame->returnValue() to the caller.
  // Used in debug mode and for GeneratorReturn.
  Label profilingInstrumentation;
  bind(&returnBaseline);
  loadPtr(
      Address(PseudoStackPointer, ResumeFromException::offsetOfFramePointer()),
      FramePointer);
  loadPtr(
      Address(PseudoStackPointer, ResumeFromException::offsetOfStackPointer()),
      PseudoStackPointer);
  // See comment further up beginning "`retn` does indeed sync the stack
  // pointer".  That comment applies here too.
  syncStackPtr();
  loadValue(Address(FramePointer, BaselineFrame::reverseOffsetOfReturnValue()),
            JSReturnOperand);
  jump(&profilingInstrumentation);

  // Return the given value to the caller.
  bind(&returnIon);
  loadValue(
      Address(PseudoStackPointer, ResumeFromException::offsetOfException()),
      JSReturnOperand);
  loadPtr(
      Address(PseudoStackPointer, offsetof(ResumeFromException, framePointer)),
      FramePointer);
  loadPtr(
      Address(PseudoStackPointer, offsetof(ResumeFromException, stackPointer)),
      PseudoStackPointer);
  syncStackPtr();

  // If profiling is enabled, then update the lastProfilingFrame to refer to
  // caller frame before returning. This code is shared by ForcedReturnIon
  // and ForcedReturnBaseline.
  bind(&profilingInstrumentation);
  {
    Label skipProfilingInstrumentation;
    AbsoluteAddress addressOfEnabled(
        asMasm().runtime()->geckoProfiler().addressOfEnabled());
    asMasm().branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                      &skipProfilingInstrumentation);
    jump(profilerExitTail);
    bind(&skipProfilingInstrumentation);
  }

  movePtr(FramePointer, PseudoStackPointer);
  syncStackPtr();
  vixl::MacroAssembler::Pop(ARMRegister(FramePointer, 64));

  vixl::MacroAssembler::Pop(vixl::lr);
  syncStackPtr();
  vixl::MacroAssembler::Ret(vixl::lr);

  // If we are bailing out to baseline to handle an exception, jump to the
  // bailout tail stub. Load 1 (true) in x0 (ReturnReg) to indicate success.
  bind(&bailout);
  Ldr(x2, MemOperand(PseudoStackPointer64,
                     ResumeFromException::offsetOfBailoutInfo()));
  Ldr(PseudoStackPointer64,
      MemOperand(PseudoStackPointer64,
                 ResumeFromException::offsetOfStackPointer()));
  syncStackPtr();
  Mov(x0, 1);
  jump(bailoutTail);

  // If we are throwing and the innermost frame was a wasm frame, reset SP and
  // FP; SP is pointing to the unwound return address to the wasm entry, so
  // we can just ret().
  bind(&wasm);
  Ldr(x29, MemOperand(PseudoStackPointer64,
                      ResumeFromException::offsetOfFramePointer()));
  Ldr(PseudoStackPointer64,
      MemOperand(PseudoStackPointer64,
                 ResumeFromException::offsetOfStackPointer()));
  syncStackPtr();
  Mov(x23, int64_t(wasm::FailInstanceReg));
  ret();

  // Found a wasm catch handler, restore state and jump to it.
  bind(&wasmCatch);
  wasm::GenerateJumpToCatchHandler(asMasm(), PseudoStackPointer, r0, r1);

  MOZ_ASSERT(GetStackPointer64().Is(PseudoStackPointer64));
}

void MacroAssemblerCompat::profilerEnterFrame(Register framePtr,
                                              Register scratch) {
  asMasm().loadJSContext(scratch);
  loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
  storePtr(framePtr,
           Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
  storePtr(ImmPtr(nullptr),
           Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void MacroAssemblerCompat::profilerExitFrame() {
  jump(asMasm().runtime()->jitRuntime()->getProfilerExitFrameTail());
}

Assembler::Condition MacroAssemblerCompat::testStringTruthy(
    bool truthy, const ValueOperand& value) {
  vixl::UseScratchRegisterScope temps(this);
  const Register scratch = temps.AcquireX().asUnsized();
  const ARMRegister scratch32(scratch, 32);
  const ARMRegister scratch64(scratch, 64);

  MOZ_ASSERT(value.valueReg() != scratch);

  unboxString(value, scratch);
  Ldr(scratch32, MemOperand(scratch64, JSString::offsetOfLength()));
  Cmp(scratch32, Operand(0));
  return truthy ? Condition::NonZero : Condition::Zero;
}

Assembler::Condition MacroAssemblerCompat::testBigIntTruthy(
    bool truthy, const ValueOperand& value) {
  vixl::UseScratchRegisterScope temps(this);
  const Register scratch = temps.AcquireX().asUnsized();

  MOZ_ASSERT(value.valueReg() != scratch);

  unboxBigInt(value, scratch);
  load32(Address(scratch, BigInt::offsetOfDigitLength()), scratch);
  cmp32(scratch, Imm32(0));
  return truthy ? Condition::NonZero : Condition::Zero;
}

void MacroAssemblerCompat::breakpoint() {
  // Note, other payloads are possible, but GDB is known to misinterpret them
  // sometimes and iloop on the breakpoint instead of stopping properly.
  Brk(0xf000);
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
  access.assertOffsetInGuardPages();
  uint32_t offset = access.offset();

  MOZ_ASSERT(memoryBase_ != ptr_);

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
  MOZ_ASSERT_IF(access.isSplatSimd128Load() || access.isWidenSimd128Load(),
                access.type() == Scalar::Float64);

  // NOTE: the generated code must match the assembly code in gen_load in
  // GenerateAtomicOperations.py
  asMasm().memoryBarrierBefore(access.sync());

  FaultingCodeOffset fco;
  switch (access.type()) {
    case Scalar::Int8:
      fco = Ldrsb(SelectGPReg(outany, out64), srcAddr);
      break;
    case Scalar::Uint8:
      fco = Ldrb(SelectGPReg(outany, out64), srcAddr);
      break;
    case Scalar::Int16:
      fco = Ldrsh(SelectGPReg(outany, out64), srcAddr);
      break;
    case Scalar::Uint16:
      fco = Ldrh(SelectGPReg(outany, out64), srcAddr);
      break;
    case Scalar::Int32:
      if (out64 != Register64::Invalid()) {
        fco = Ldrsw(SelectGPReg(outany, out64), srcAddr);
      } else {
        fco = Ldr(SelectGPReg(outany, out64, 32), srcAddr);
      }
      break;
    case Scalar::Uint32:
      fco = Ldr(SelectGPReg(outany, out64, 32), srcAddr);
      break;
    case Scalar::Int64:
      fco = Ldr(SelectGPReg(outany, out64), srcAddr);
      break;
    case Scalar::Float32:
      // LDR does the right thing also for access.isZeroExtendSimd128Load()
      fco = Ldr(SelectFPReg(outany, out64, 32), srcAddr);
      break;
    case Scalar::Float64:
      if (access.isSplatSimd128Load() || access.isWidenSimd128Load()) {
        ScratchSimd128Scope scratch_(asMasm());
        ARMFPRegister scratch = Simd1D(scratch_);
        fco = Ldr(scratch, srcAddr);
        if (access.isSplatSimd128Load()) {
          Dup(SelectFPReg(outany, out64, 128).V2D(), scratch, 0);
        } else {
          MOZ_ASSERT(access.isWidenSimd128Load());
          switch (access.widenSimdOp()) {
            case wasm::SimdOp::V128Load8x8S:
              Sshll(SelectFPReg(outany, out64, 128).V8H(), scratch.V8B(), 0);
              break;
            case wasm::SimdOp::V128Load8x8U:
              Ushll(SelectFPReg(outany, out64, 128).V8H(), scratch.V8B(), 0);
              break;
            case wasm::SimdOp::V128Load16x4S:
              Sshll(SelectFPReg(outany, out64, 128).V4S(), scratch.V4H(), 0);
              break;
            case wasm::SimdOp::V128Load16x4U:
              Ushll(SelectFPReg(outany, out64, 128).V4S(), scratch.V4H(), 0);
              break;
            case wasm::SimdOp::V128Load32x2S:
              Sshll(SelectFPReg(outany, out64, 128).V2D(), scratch.V2S(), 0);
              break;
            case wasm::SimdOp::V128Load32x2U:
              Ushll(SelectFPReg(outany, out64, 128).V2D(), scratch.V2S(), 0);
              break;
            default:
              MOZ_CRASH("Unexpected widening op for wasmLoad");
          }
        }
      } else {
        // LDR does the right thing also for access.isZeroExtendSimd128Load()
        fco = Ldr(SelectFPReg(outany, out64, 64), srcAddr);
      }
      break;
    case Scalar::Simd128:
      fco = Ldr(SelectFPReg(outany, out64, 128), srcAddr);
      break;
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::Float16:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected array type");
  }

  append(access, wasm::TrapMachineInsnForLoad(byteSize(access.type())), fco);

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
  access.assertOffsetInGuardPages();
  uint32_t offset = access.offset();

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
  // NOTE: the generated code must match the assembly code in gen_store in
  // GenerateAtomicOperations.py
  asMasm().memoryBarrierBefore(access.sync());

  FaultingCodeOffset fco;
  switch (access.type()) {
    case Scalar::Int8:
    case Scalar::Uint8:
      fco = Strb(SelectGPReg(valany, val64), dstAddr);
      break;
    case Scalar::Int16:
    case Scalar::Uint16:
      fco = Strh(SelectGPReg(valany, val64), dstAddr);
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      fco = Str(SelectGPReg(valany, val64), dstAddr);
      break;
    case Scalar::Int64:
      fco = Str(SelectGPReg(valany, val64), dstAddr);
      break;
    case Scalar::Float32:
      fco = Str(SelectFPReg(valany, val64, 32), dstAddr);
      break;
    case Scalar::Float64:
      fco = Str(SelectFPReg(valany, val64, 64), dstAddr);
      break;
    case Scalar::Simd128:
      fco = Str(SelectFPReg(valany, val64, 128), dstAddr);
      break;
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::Float16:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected array type");
  }

  append(access, wasm::TrapMachineInsnForStore(byteSize(access.type())), fco);

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

  Dup(shift, ARMRegister(rhs, 32));
  Neg(shift, shift);

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

  Dup(shift, ARMRegister(rhs, 32));
  Neg(shift, shift);

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

  Dup(shift, ARMRegister(rhs, 32));
  Neg(shift, shift);

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

  Dup(shift, ARMRegister(rhs, 64));
  Neg(shift, shift);

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
        MacroAssembler::PushRegsInMaskSizeInBytes(set);
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
               MacroAssembler::PushRegsInMaskSizeInBytes(set));
  } else {
    MOZ_ASSERT(masm->framePushed() - maxExtentInitial ==
               MacroAssembler::PushRegsInMaskSizeInBytes(set));
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

void MacroAssembler::freeStackTo(uint32_t framePushed) {
  MOZ_ASSERT(framePushed <= framePushed_);
  Sub(GetStackPointer64(), X(FramePointer), Operand(int32_t(framePushed)));
  syncStackPtr();
  framePushed_ = framePushed;
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

  assertStackAlignment(ABIStackAlignment);
}

void MacroAssembler::callWithABIPost(uint32_t stackAdjust, ABIType result,
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

void MacroAssembler::callWithABINoProfiler(Register fun, ABIType result) {
  vixl::UseScratchRegisterScope temps(this);
  const Register scratch = temps.AcquireX().asUnsized();
  movePtr(fun, scratch);

  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(scratch);
  callWithABIPost(stackAdjust, result);
}

void MacroAssembler::callWithABINoProfiler(const Address& fun, ABIType result) {
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
  asMasm().PushFrameDescriptor(FrameType::IonJS);
  asMasm().Push(ImmPtr(fakeReturnAddr));
  asMasm().Push(FramePointer);
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
  And(ARMRegister(buffer, 64), ARMRegister(ptr, 64),
      Operand(int32_t(~gc::ChunkMask)));
  loadPtr(Address(buffer, gc::ChunkStoreBufferOffset), buffer);
}

void MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr,
                                             Register temp, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  MOZ_ASSERT(ptr != temp);
  MOZ_ASSERT(ptr != ScratchReg &&
             ptr != ScratchReg2);  // Both may be used internally.
  MOZ_ASSERT(temp != ScratchReg && temp != ScratchReg2);

  And(ARMRegister(temp, 64), ARMRegister(ptr, 64),
      Operand(int32_t(~gc::ChunkMask)));
  branchPtr(InvertCondition(cond), Address(temp, gc::ChunkStoreBufferOffset),
            ImmWord(0), label);
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

  getGCThingValueChunk(value, temp);
  branchPtr(InvertCondition(cond), Address(temp, gc::ChunkStoreBufferOffset),
            ImmWord(0), label);

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
                                       MIRType valueType, const T& dest) {
  MOZ_ASSERT(valueType < MIRType::Value);

  if (valueType == MIRType::Double) {
    boxDouble(value.reg().typedReg().fpu(), dest);
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
                                                const Address& dest);
template void MacroAssembler::storeUnboxedValue(
    const ConstantOrRegister& value, MIRType valueType,
    const BaseObjectElementIndex& dest);

void MacroAssembler::comment(const char* msg) { Assembler::comment(msg); }

// ========================================================================
// wasm support

FaultingCodeOffset MacroAssembler::wasmTrapInstruction() {
  AutoForbidPoolsAndNops afp(this,
                             /* max number of instructions in scope = */ 1);
  FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
  Unreachable();
  return fco;
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Register boundsCheckLimit, Label* ok) {
  branch32(cond, index, boundsCheckLimit, ok);
  if (JitOptions.spectreIndexMasking) {
    csel(ARMRegister(index, 32), vixl::wzr, ARMRegister(index, 32), cond);
  }
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Address boundsCheckLimit, Label* ok) {
  branch32(cond, index, boundsCheckLimit, ok);
  if (JitOptions.spectreIndexMasking) {
    csel(ARMRegister(index, 32), vixl::wzr, ARMRegister(index, 32), cond);
  }
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Register64 boundsCheckLimit, Label* ok) {
  branchPtr(cond, index.reg, boundsCheckLimit.reg, ok);
  if (JitOptions.spectreIndexMasking) {
    csel(ARMRegister(index.reg, 64), vixl::xzr, ARMRegister(index.reg, 64),
         cond);
  }
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Address boundsCheckLimit, Label* ok) {
  branchPtr(InvertCondition(cond), boundsCheckLimit, index.reg, ok);
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
  // Wasm stubs use the native SP, not the PSP.

  linkExitFrame(cxreg, scratch);

  MOZ_RELEASE_ASSERT(sp.Is(GetStackPointer64()));

  // SP has to be 16-byte aligned when we do a load/store, so push |type| twice
  // and then add 8 bytes to SP. This leaves SP unaligned.
  move32(Imm32(int32_t(type)), scratch);
  push(scratch, scratch);
  Add(sp, sp, 8);

  // Despite the above assertion, it is possible for control to flow from here
  // to the code generated by
  // MacroAssemblerCompat::handleFailureWithHandlerTail without any
  // intervening assignment to PSP.  But handleFailureWithHandlerTail assumes
  // that PSP is the active stack pointer.  Hence the following is necessary
  // for safety.  Note we can't use initPseudoStackPtr here as that would
  // generate no instructions.
  Mov(PseudoStackPointer64, sp);
}

void MacroAssembler::widenInt32(Register r) {
  move32To64ZeroExtend(r, Register64(r));
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
  // AutoForbidPoolsAndNops ensures that the metadata is emitted at the
  // address of the ldxr*.  Note that the use of AutoForbidPoolsAndNops is now
  // a "second class" solution; the right way to do this would be to have the
  // masm.<LoadInsn> calls produce an FaultingCodeOffset, and hand that value to
  // `masm.append`.
  MOZ_ASSERT(ptr.IsImmediateOffset() && ptr.offset() == 0);

  switch (Scalar::byteSize(srcType)) {
    case 1: {
      {
        AutoForbidPoolsAndNops afp(
            &masm,
            /* max number of instructions in scope = */ 1);
        if (access) {
          masm.append(*access, wasm::TrapMachineInsn::Load8,
                      FaultingCodeOffset(masm.currentOffset()));
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
          masm.append(*access, wasm::TrapMachineInsn::Load16,
                      FaultingCodeOffset(masm.currentOffset()));
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
          masm.append(*access, wasm::TrapMachineInsn::Load32,
                      FaultingCodeOffset(masm.currentOffset()));
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
          masm.append(*access, wasm::TrapMachineInsn::Load64,
                      FaultingCodeOffset(masm.currentOffset()));
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
  // Note, these are not decorated with a TrapSite only because they are
  // assumed to be preceded by a LoadExclusive to the same address, of the
  // same width, so that will always take the page fault if the address is bad.
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

static bool HasAtomicInstructions(MacroAssembler& masm) {
  return masm.asVIXL().GetCPUFeatures()->Has(vixl::CPUFeatures::kAtomics);
}

static inline bool SupportedAtomicInstructionOperands(Scalar::Type type,
                                                      Width targetWidth) {
  if (targetWidth == Width::_32) {
    return byteSize(type) <= 4;
  }
  if (targetWidth == Width::_64) {
    return byteSize(type) == 8;
  }
  return false;
}

template <typename T>
static void CompareExchange(MacroAssembler& masm,
                            const wasm::MemoryAccessDesc* access,
                            Scalar::Type type, Width targetWidth,
                            Synchronization sync, const T& mem, Register oldval,
                            Register newval, Register output) {
  MOZ_ASSERT(oldval != output && newval != output);

  vixl::UseScratchRegisterScope temps(&masm);

  Register ptrScratch = temps.AcquireX().asUnsized();
  MemOperand ptr = ComputePointerForAtomic(masm, mem, ptrScratch);

  MOZ_ASSERT(ptr.base().asUnsized() != output);

  if (HasAtomicInstructions(masm) &&
      SupportedAtomicInstructionOperands(type, targetWidth)) {
    masm.Mov(X(output), X(oldval));
    // Capal is using same atomic mechanism as Ldxr/Stxr, and
    // consider it is the same for "Inner Shareable" domain.
    // Not updated gen_cmpxchg in GenerateAtomicOperations.py.
    masm.memoryBarrierBefore(sync);
    {
      AutoForbidPoolsAndNops afp(&masm, /* number of insns = */ 1);
      if (access) {
        masm.append(*access, wasm::TrapMachineInsn::Atomic,
                    FaultingCodeOffset(masm.currentOffset()));
      }
      switch (byteSize(type)) {
        case 1:
          masm.Casalb(R(output, targetWidth), R(newval, targetWidth), ptr);
          break;
        case 2:
          masm.Casalh(R(output, targetWidth), R(newval, targetWidth), ptr);
          break;
        case 4:
        case 8:
          masm.Casal(R(output, targetWidth), R(newval, targetWidth), ptr);
          break;
        default:
          MOZ_CRASH("CompareExchange unsupported type");
      }
    }
    masm.memoryBarrierAfter(sync);
    SignOrZeroExtend(masm, type, targetWidth, output, output);
    return;
  }

  // The target doesn't support atomics, so generate a LL-SC loop. This requires
  // only AArch64 v8.0.
  Label again;
  Label done;

  // NOTE: the generated code must match the assembly code in gen_cmpxchg in
  // GenerateAtomicOperations.py
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
                           Synchronization sync, const T& mem, Register value,
                           Register output) {
  MOZ_ASSERT(value != output);

  vixl::UseScratchRegisterScope temps(&masm);

  Register ptrScratch = temps.AcquireX().asUnsized();
  MemOperand ptr = ComputePointerForAtomic(masm, mem, ptrScratch);

  if (HasAtomicInstructions(masm) &&
      SupportedAtomicInstructionOperands(type, targetWidth)) {
    // Swpal is using same atomic mechanism as Ldxr/Stxr, and
    // consider it is the same for "Inner Shareable" domain.
    // Not updated gen_exchange in GenerateAtomicOperations.py.
    masm.memoryBarrierBefore(sync);
    {
      AutoForbidPoolsAndNops afp(&masm, /* number of insns = */ 1);
      if (access) {
        masm.append(*access, wasm::TrapMachineInsn::Atomic,
                    FaultingCodeOffset(masm.currentOffset()));
      }
      switch (byteSize(type)) {
        case 1:
          masm.Swpalb(R(value, targetWidth), R(output, targetWidth), ptr);
          break;
        case 2:
          masm.Swpalh(R(value, targetWidth), R(output, targetWidth), ptr);
          break;
        case 4:
        case 8:
          masm.Swpal(R(value, targetWidth), R(output, targetWidth), ptr);
          break;
        default:
          MOZ_CRASH("AtomicExchange unsupported type");
      }
    }
    masm.memoryBarrierAfter(sync);
    SignOrZeroExtend(masm, type, targetWidth, output, output);
    return;
  }

  // The target doesn't support atomics, so generate a LL-SC loop. This requires
  // only AArch64 v8.0.
  Label again;

  // NOTE: the generated code must match the assembly code in gen_exchange in
  // GenerateAtomicOperations.py
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
                          Synchronization sync, AtomicOp op, const T& mem,
                          Register value, Register temp, Register output) {
  MOZ_ASSERT(value != output);
  MOZ_ASSERT(value != temp);
  MOZ_ASSERT_IF(wantResult, output != temp);

  vixl::UseScratchRegisterScope temps(&masm);

  Register ptrScratch = temps.AcquireX().asUnsized();
  MemOperand ptr = ComputePointerForAtomic(masm, mem, ptrScratch);

  if (HasAtomicInstructions(masm) &&
      SupportedAtomicInstructionOperands(type, targetWidth) &&
      !isFloatingType(type)) {
    // LdXXXal/StXXXl is using same atomic mechanism as Ldxr/Stxr, and
    // consider it is the same for "Inner Shareable" domain.
    // Not updated gen_fetchop in GenerateAtomicOperations.py.
    masm.memoryBarrierBefore(sync);

#define FETCH_OP_CASE(op, arg)                                                \
  {                                                                           \
    AutoForbidPoolsAndNops afp(&masm, /* num insns = */ 1);                   \
    if (access) {                                                             \
      masm.append(*access, wasm::TrapMachineInsn::Atomic,                     \
                  FaultingCodeOffset(masm.currentOffset()));                  \
    }                                                                         \
    switch (byteSize(type)) {                                                 \
      case 1:                                                                 \
        if (wantResult) {                                                     \
          masm.Ld##op##alb(R(arg, targetWidth), R(output, targetWidth), ptr); \
        } else {                                                              \
          masm.St##op##lb(R(arg, targetWidth), ptr);                          \
        }                                                                     \
        break;                                                                \
      case 2:                                                                 \
        if (wantResult) {                                                     \
          masm.Ld##op##alh(R(arg, targetWidth), R(output, targetWidth), ptr); \
        } else {                                                              \
          masm.St##op##lh(R(arg, targetWidth), ptr);                          \
        }                                                                     \
        break;                                                                \
      case 4:                                                                 \
      case 8:                                                                 \
        if (wantResult) {                                                     \
          masm.Ld##op##al(R(arg, targetWidth), R(output, targetWidth), ptr);  \
        } else {                                                              \
          masm.St##op##l(R(arg, targetWidth), ptr);                           \
        }                                                                     \
        break;                                                                \
      default:                                                                \
        MOZ_CRASH("AtomicFetchOp unsupported type");                          \
    }                                                                         \
  }

    switch (op) {
      case AtomicOp::Add:
        FETCH_OP_CASE(add, value);
        break;
      case AtomicOp::Sub: {
        Register scratch = temps.AcquireX().asUnsized();
        masm.Neg(X(scratch), X(value));
        FETCH_OP_CASE(add, scratch);
        break;
      }
      case AtomicOp::And: {
        Register scratch = temps.AcquireX().asUnsized();
        masm.Eor(X(scratch), X(value), Operand(~0));
        FETCH_OP_CASE(clr, scratch);
        break;
      }
      case AtomicOp::Or:
        FETCH_OP_CASE(set, value);
        break;
      case AtomicOp::Xor:
        FETCH_OP_CASE(eor, value);
        break;
    }
    masm.memoryBarrierAfter(sync);
    if (wantResult) {
      SignOrZeroExtend(masm, type, targetWidth, output, output);
    }
    return;
  }

#undef FETCH_OP_CASE

  // The target doesn't support atomics, so generate a LL-SC loop. This requires
  // only AArch64 v8.0.
  Label again;

  // NOTE: the generated code must match the assembly code in gen_fetchop in
  // GenerateAtomicOperations.py
  masm.memoryBarrierBefore(sync);

  Register scratch = temps.AcquireX().asUnsized();

  masm.bind(&again);
  LoadExclusive(masm, access, type, targetWidth, ptr, output);
  switch (op) {
    case AtomicOp::Add:
      masm.Add(X(temp), X(output), X(value));
      break;
    case AtomicOp::Sub:
      masm.Sub(X(temp), X(output), X(value));
      break;
    case AtomicOp::And:
      masm.And(X(temp), X(output), X(value));
      break;
    case AtomicOp::Or:
      masm.Orr(X(temp), X(output), X(value));
      break;
    case AtomicOp::Xor:
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

void MacroAssembler::compareExchange(Scalar::Type type, Synchronization sync,
                                     const Address& mem, Register oldval,
                                     Register newval, Register output) {
  CompareExchange(*this, nullptr, type, Width::_32, sync, mem, oldval, newval,
                  output);
}

void MacroAssembler::compareExchange(Scalar::Type type, Synchronization sync,
                                     const BaseIndex& mem, Register oldval,
                                     Register newval, Register output) {
  CompareExchange(*this, nullptr, type, Width::_32, sync, mem, oldval, newval,
                  output);
}

void MacroAssembler::compareExchange64(Synchronization sync, const Address& mem,
                                       Register64 expect, Register64 replace,
                                       Register64 output) {
  CompareExchange(*this, nullptr, Scalar::Int64, Width::_64, sync, mem,
                  expect.reg, replace.reg, output.reg);
}

void MacroAssembler::compareExchange64(Synchronization sync,
                                       const BaseIndex& mem, Register64 expect,
                                       Register64 replace, Register64 output) {
  CompareExchange(*this, nullptr, Scalar::Int64, Width::_64, sync, mem,
                  expect.reg, replace.reg, output.reg);
}

void MacroAssembler::atomicExchange64(Synchronization sync, const Address& mem,
                                      Register64 value, Register64 output) {
  AtomicExchange(*this, nullptr, Scalar::Int64, Width::_64, sync, mem,
                 value.reg, output.reg);
}

void MacroAssembler::atomicExchange64(Synchronization sync,
                                      const BaseIndex& mem, Register64 value,
                                      Register64 output) {
  AtomicExchange(*this, nullptr, Scalar::Int64, Width::_64, sync, mem,
                 value.reg, output.reg);
}

void MacroAssembler::atomicFetchOp64(Synchronization sync, AtomicOp op,
                                     Register64 value, const Address& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp<true>(*this, nullptr, Scalar::Int64, Width::_64, sync, op, mem,
                      value.reg, temp.reg, output.reg);
}

void MacroAssembler::atomicFetchOp64(Synchronization sync, AtomicOp op,
                                     Register64 value, const BaseIndex& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp<true>(*this, nullptr, Scalar::Int64, Width::_64, sync, op, mem,
                      value.reg, temp.reg, output.reg);
}

void MacroAssembler::atomicEffectOp64(Synchronization sync, AtomicOp op,
                                      Register64 value, const Address& mem,
                                      Register64 temp) {
  AtomicFetchOp<false>(*this, nullptr, Scalar::Int64, Width::_64, sync, op, mem,
                       value.reg, temp.reg, temp.reg);
}

void MacroAssembler::atomicEffectOp64(Synchronization sync, AtomicOp op,
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

void MacroAssembler::atomicExchange(Scalar::Type type, Synchronization sync,
                                    const Address& mem, Register value,
                                    Register output) {
  AtomicExchange(*this, nullptr, type, Width::_32, sync, mem, value, output);
}

void MacroAssembler::atomicExchange(Scalar::Type type, Synchronization sync,
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

void MacroAssembler::atomicFetchOp(Scalar::Type type, Synchronization sync,
                                   AtomicOp op, Register value,
                                   const Address& mem, Register temp,
                                   Register output) {
  AtomicFetchOp<true>(*this, nullptr, type, Width::_32, sync, op, mem, value,
                      temp, output);
}

void MacroAssembler::atomicFetchOp(Scalar::Type type, Synchronization sync,
                                   AtomicOp op, Register value,
                                   const BaseIndex& mem, Register temp,
                                   Register output) {
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
                              Synchronization sync, const T& mem,
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
                                       Synchronization sync, const Address& mem,
                                       Register oldval, Register newval,
                                       Register temp, AnyRegister output) {
  CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

void MacroAssembler::compareExchangeJS(Scalar::Type arrayType,
                                       Synchronization sync,
                                       const BaseIndex& mem, Register oldval,
                                       Register newval, Register temp,
                                       AnyRegister output) {
  CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

template <typename T>
static void AtomicExchangeJS(MacroAssembler& masm, Scalar::Type arrayType,
                             Synchronization sync, const T& mem, Register value,
                             Register temp, AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.atomicExchange(arrayType, sync, mem, value, temp);
    masm.convertUInt32ToDouble(temp, output.fpu());
  } else {
    masm.atomicExchange(arrayType, sync, mem, value, output.gpr());
  }
}

void MacroAssembler::atomicExchangeJS(Scalar::Type arrayType,
                                      Synchronization sync, const Address& mem,
                                      Register value, Register temp,
                                      AnyRegister output) {
  AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

void MacroAssembler::atomicExchangeJS(Scalar::Type arrayType,
                                      Synchronization sync,
                                      const BaseIndex& mem, Register value,
                                      Register temp, AnyRegister output) {
  AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

template <typename T>
static void AtomicFetchOpJS(MacroAssembler& masm, Scalar::Type arrayType,
                            Synchronization sync, AtomicOp op, Register value,
                            const T& mem, Register temp1, Register temp2,
                            AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp2, temp1);
    masm.convertUInt32ToDouble(temp1, output.fpu());
  } else {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp1, output.gpr());
  }
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     Synchronization sync, AtomicOp op,
                                     Register value, const Address& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     Synchronization sync, AtomicOp op,
                                     Register value, const BaseIndex& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType,
                                      Synchronization sync, AtomicOp op,
                                      Register value, const BaseIndex& mem,
                                      Register temp) {
  AtomicFetchOp<false>(*this, nullptr, arrayType, Width::_32, sync, op, mem,
                       value, temp, temp);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType,
                                      Synchronization sync, AtomicOp op,
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
  ARMFPRegister iFlt(src, 32);
  ARMRegister o64(dest, 64);
  ARMRegister o32(dest, 32);

  Label handleZero;
  Label fin;

  // Handle ±0 and NaN first.
  Fcmp(iFlt, 0.0);
  B(Assembler::Equal, &handleZero);
  // NaN is always a bail condition, just bail directly.
  B(Assembler::Overflow, fail);

  // Round towards negative infinity.
  Fcvtms(o64, iFlt);

  // Sign extend lower 32 bits to test if the result isn't an Int32.
  Cmp(o64, Operand(o64, vixl::SXTW));
  B(NotEqual, fail);

  // Clear upper 32 bits.
  Uxtw(o64, o64);
  B(&fin);

  bind(&handleZero);
  // Move the top word of the float into the output reg, if it is non-zero,
  // then the original value was -0.0.
  Fmov(o32, iFlt);
  Cbnz(o32, fail);
  bind(&fin);
}

void MacroAssembler::floorDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  ARMFPRegister iDbl(src, 64);
  ARMRegister o64(dest, 64);
  ARMRegister o32(dest, 32);

  Label handleZero;
  Label fin;

  // Handle ±0 and NaN first.
  Fcmp(iDbl, 0.0);
  B(Assembler::Equal, &handleZero);
  // NaN is always a bail condition, just bail directly.
  B(Assembler::Overflow, fail);

  // Round towards negative infinity.
  Fcvtms(o64, iDbl);

  // Sign extend lower 32 bits to test if the result isn't an Int32.
  Cmp(o64, Operand(o64, vixl::SXTW));
  B(NotEqual, fail);

  // Clear upper 32 bits.
  Uxtw(o64, o64);
  B(&fin);

  bind(&handleZero);
  // Move the top word of the double into the output reg, if it is non-zero,
  // then the original value was -0.0.
  Fmov(o64, iDbl);
  Cbnz(o64, fail);
  bind(&fin);
}

void MacroAssembler::ceilFloat32ToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  ARMFPRegister iFlt(src, 32);
  ARMRegister o64(dest, 64);
  ARMRegister o32(dest, 32);

  Label handleZero;
  Label fin;

  // Round towards positive infinity.
  Fcvtps(o64, iFlt);

  // Sign extend lower 32 bits to test if the result isn't an Int32.
  Cmp(o64, Operand(o64, vixl::SXTW));
  B(NotEqual, fail);

  // We have to check for (-1, -0] and NaN when the result is zero.
  Cbz(o64, &handleZero);

  // Clear upper 32 bits.
  Uxtw(o64, o64);
  B(&fin);

  // Bail if the input is in (-1, -0] or NaN.
  bind(&handleZero);
  // Move the top word of the float into the output reg, if it is non-zero,
  // then the original value wasn't +0.0.
  Fmov(o32, iFlt);
  Cbnz(o32, fail);
  bind(&fin);
}

void MacroAssembler::ceilDoubleToInt32(FloatRegister src, Register dest,
                                       Label* fail) {
  ARMFPRegister iDbl(src, 64);
  ARMRegister o64(dest, 64);
  ARMRegister o32(dest, 32);

  Label handleZero;
  Label fin;

  // Round towards positive infinity.
  Fcvtps(o64, iDbl);

  // Sign extend lower 32 bits to test if the result isn't an Int32.
  Cmp(o64, Operand(o64, vixl::SXTW));
  B(NotEqual, fail);

  // We have to check for (-1, -0] and NaN when the result is zero.
  Cbz(o64, &handleZero);

  // Clear upper 32 bits.
  Uxtw(o64, o64);
  B(&fin);

  // Bail if the input is in (-1, -0] or NaN.
  bind(&handleZero);
  // Move the top word of the double into the output reg, if it is non-zero,
  // then the original value wasn't +0.0.
  Fmov(o64, iDbl);
  Cbnz(o64, fail);
  bind(&fin);
}

void MacroAssembler::truncFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail) {
  ARMFPRegister src32(src, 32);
  ARMRegister dest32(dest, 32);
  ARMRegister dest64(dest, 64);

  Label done, zeroCase;

  // Convert scalar to signed 64-bit fixed-point, rounding toward zero.
  // In the case of overflow, the output is saturated.
  // In the case of NaN and -0, the output is zero.
  Fcvtzs(dest64, src32);

  // If the output was zero, worry about special cases.
  Cbz(dest64, &zeroCase);

  // Sign extend lower 32 bits to test if the result isn't an Int32.
  Cmp(dest64, Operand(dest64, vixl::SXTW));
  B(NotEqual, fail);

  // Clear upper 32 bits.
  Uxtw(dest64, dest64);

  // If the output was non-zero and wasn't saturated, just return it.
  B(&done);

  // Handle the case of a zero output:
  // 1. The input may have been NaN, requiring a failure.
  // 2. The input may have been in (-1,-0], requiring a failure.
  {
    bind(&zeroCase);

    // Combine test for negative and NaN values using a single bitwise
    // operation.
    //
    // | Decimal number | Bitwise representation |
    // |----------------|------------------------|
    // | -0             | 8000'0000              |
    // | +0             | 0000'0000              |
    // | +1             | 3f80'0000              |
    // |  NaN (or +Inf) | 7fyx'xxxx, y >= 8      |
    // | -NaN (or -Inf) | ffyx'xxxx, y >= 8      |
    //
    // If any of two most significant bits is set, the number isn't in [0, 1).
    // (Recall that floating point numbers, except for NaN, are strictly ordered
    // when comparing their bitwise representation as signed integers.)

    Fmov(dest32, src32);
    Lsr(dest32, dest32, 30);
    Cbnz(dest32, fail);
  }

  bind(&done);
}

void MacroAssembler::truncDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  ARMFPRegister src64(src, 64);
  ARMRegister dest64(dest, 64);
  ARMRegister dest32(dest, 32);

  Label done, zeroCase;

  // Convert scalar to signed 64-bit fixed-point, rounding toward zero.
  // In the case of overflow, the output is saturated.
  // In the case of NaN and -0, the output is zero.
  Fcvtzs(dest64, src64);

  // If the output was zero, worry about special cases.
  Cbz(dest64, &zeroCase);

  // Sign extend lower 32 bits to test if the result isn't an Int32.
  Cmp(dest64, Operand(dest64, vixl::SXTW));
  B(NotEqual, fail);

  // Clear upper 32 bits.
  Uxtw(dest64, dest64);

  // If the output was non-zero and wasn't saturated, just return it.
  B(&done);

  // Handle the case of a zero output:
  // 1. The input may have been NaN, requiring a failure.
  // 2. The input may have been in (-1,-0], requiring a failure.
  {
    bind(&zeroCase);

    // Combine test for negative and NaN values using a single bitwise
    // operation.
    //
    // | Decimal number | Bitwise representation |
    // |----------------|------------------------|
    // | -0             | 8000'0000'0000'0000    |
    // | +0             | 0000'0000'0000'0000    |
    // | +1             | 3ff0'0000'0000'0000    |
    // |  NaN (or +Inf) | 7ffx'xxxx'xxxx'xxxx    |
    // | -NaN (or -Inf) | fffx'xxxx'xxxx'xxxx    |
    //
    // If any of two most significant bits is set, the number isn't in [0, 1).
    // (Recall that floating point numbers, except for NaN, are strictly ordered
    // when comparing their bitwise representation as signed integers.)

    Fmov(dest64, src64);
    Lsr(dest64, dest64, 62);
    Cbnz(dest64, fail);
  }

  bind(&done);
}

void MacroAssembler::roundFloat32ToInt32(FloatRegister src, Register dest,
                                         FloatRegister temp, Label* fail) {
  ARMFPRegister src32(src, 32);
  ARMRegister dest32(dest, 32);
  ARMRegister dest64(dest, 64);

  Label negative, saturated, done;

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
    // Convert to signed 64-bit integer, rounding halfway cases away from zero.
    // In the case of overflow, the output is saturated.
    // In the case of NaN and -0, the output is zero.
    Fcvtas(dest64, src32);

    // In the case of zero, the input may have been NaN or -0, which must bail.
    Cbnz(dest64, &saturated);

    // Combine test for -0 and NaN values using a single bitwise operation.
    // See truncFloat32ToInt32 for an explanation.
    Fmov(dest32, src32);
    Lsr(dest32, dest32, 30);
    Cbnz(dest32, fail);

    B(&done);
  }

  // Handle the complicated case of a negative input.
  // Rounding proceeds with consideration of the fractional part of the input:
  // 1. If > 0.5, round to integer with higher absolute value (so, down).
  // 2. If < 0.5, round to integer with lower absolute value (so, up).
  // 3. If = 0.5, round to +Infinity (so, up).
  bind(&negative);
  {
    // Inputs in [-0.5, 0) are rounded to -0. Fail.
    loadConstantFloat32(-0.5f, temp);
    branchFloat(Assembler::DoubleGreaterThanOrEqual, src, temp, fail);

    // Other negative inputs need the biggest double less than 0.5 added.
    loadConstantFloat32(GetBiggestNumberLessThan(0.5f), temp);
    addFloat32(src, temp);

    // Round all values toward -Infinity.
    // In the case of overflow, the output is saturated.
    // NaN and -0 are already handled by the "positive number" path above.
    Fcvtms(dest64, temp);
  }

  bind(&saturated);

  // Sign extend lower 32 bits to test if the result isn't an Int32.
  Cmp(dest64, Operand(dest64, vixl::SXTW));
  B(NotEqual, fail);

  // Clear upper 32 bits.
  Uxtw(dest64, dest64);

  bind(&done);
}

void MacroAssembler::roundDoubleToInt32(FloatRegister src, Register dest,
                                        FloatRegister temp, Label* fail) {
  ARMFPRegister src64(src, 64);
  ARMRegister dest64(dest, 64);
  ARMRegister dest32(dest, 32);

  Label negative, saturated, done;

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
    // Convert to signed 64-bit integer, rounding halfway cases away from zero.
    // In the case of overflow, the output is saturated.
    // In the case of NaN and -0, the output is zero.
    Fcvtas(dest64, src64);

    // In the case of zero, the input may have been NaN or -0, which must bail.
    Cbnz(dest64, &saturated);

    // Combine test for -0 and NaN values using a single bitwise operation.
    // See truncDoubleToInt32 for an explanation.
    Fmov(dest64, src64);
    Lsr(dest64, dest64, 62);
    Cbnz(dest64, fail);

    B(&done);
  }

  // Handle the complicated case of a negative input.
  // Rounding proceeds with consideration of the fractional part of the input:
  // 1. If > 0.5, round to integer with higher absolute value (so, down).
  // 2. If < 0.5, round to integer with lower absolute value (so, up).
  // 3. If = 0.5, round to +Infinity (so, up).
  bind(&negative);
  {
    // Inputs in [-0.5, 0) are rounded to -0. Fail.
    loadConstantDouble(-0.5, temp);
    branchDouble(Assembler::DoubleGreaterThanOrEqual, src, temp, fail);

    // Other negative inputs need the biggest double less than 0.5 added.
    loadConstantDouble(GetBiggestNumberLessThan(0.5), temp);
    addDouble(src, temp);

    // Round all values toward -Infinity.
    // In the case of overflow, the output is saturated.
    // NaN and -0 are already handled by the "positive number" path above.
    Fcvtms(dest64, temp);
  }

  bind(&saturated);

  // Sign extend lower 32 bits to test if the result isn't an Int32.
  Cmp(dest64, Operand(dest64, vixl::SXTW));
  B(NotEqual, fail);

  // Clear upper 32 bits.
  Uxtw(dest64, dest64);

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

  // Double with only the sign bit set
  loadConstantDouble(-0.0, scratch);

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

  // Float with only the sign bit set
  loadConstantFloat32(-0.0f, scratch);

  if (lhs != output) {
    moveFloat32(lhs, output);
  }

  bit(ARMFPRegister(output.encoding(), vixl::VectorFormat::kFormat8B),
      ARMFPRegister(rhs.encoding(), vixl::VectorFormat::kFormat8B),
      ARMFPRegister(scratch.encoding(), vixl::VectorFormat::kFormat8B));
}

void MacroAssembler::shiftIndex32AndAdd(Register indexTemp32, int shift,
                                        Register pointer) {
  Add(ARMRegister(pointer, 64), ARMRegister(pointer, 64),
      Operand(ARMRegister(indexTemp32, 64), vixl::LSL, shift));
}

#ifdef ENABLE_WASM_TAIL_CALLS
void MacroAssembler::wasmMarkCallAsSlow() { Mov(x28, x28); }

const int32_t SlowCallMarker = 0xaa1c03fc;

void MacroAssembler::wasmCheckSlowCallsite(Register ra, Label* notSlow,
                                           Register temp1, Register temp2) {
  MOZ_ASSERT(ra != temp2);
  Ldr(W(temp2), MemOperand(X(ra), 0));
  Cmp(W(temp2), Operand(SlowCallMarker));
  B(Assembler::NotEqual, notSlow);
}

CodeOffset MacroAssembler::wasmMarkedSlowCall(const wasm::CallSiteDesc& desc,
                                              const Register reg) {
  AutoForbidPoolsAndNops afp(this, !GetStackPointer64().Is(vixl::sp) ? 3 : 2);
  CodeOffset offset = call(desc, reg);
  wasmMarkCallAsSlow();
  return offset;
}
#endif  // ENABLE_WASM_TAIL_CALLS

//}}} check_macroassembler_style

}  // namespace jit
}  // namespace js
