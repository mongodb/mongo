/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2014 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmFrameIter.h"

#include "jit/JitFrames.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmTlsData.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::DebugOnly;
using mozilla::Maybe;

/*****************************************************************************/
// WasmFrameIter implementation

WasmFrameIter::WasmFrameIter(JitActivation* activation, wasm::Frame* fp)
    : activation_(activation),
      code_(nullptr),
      codeRange_(nullptr),
      lineOrBytecode_(0),
      fp_(fp ? fp : activation->wasmExitFP()),
      tls_(nullptr),
      unwoundIonCallerFP_(nullptr),
      unwoundIonFrameType_(jit::FrameType(-1)),
      unwind_(Unwind::False),
      unwoundAddressOfReturnAddress_(nullptr),
      resumePCinCurrentFrame_(nullptr) {
  MOZ_ASSERT(fp_);
  tls_ = GetNearestEffectiveTls(fp_);

  // When the stack is captured during a trap (viz., to create the .stack
  // for an Error object), use the pc/bytecode information captured by the
  // signal handler in the runtime. Take care not to use this trap unwind
  // state for wasm frames in the middle of a JitActivation, i.e., wasm frames
  // that called into JIT frames before the trap.

  if (activation->isWasmTrapping() && fp_ == activation->wasmExitFP()) {
    const TrapData& trapData = activation->wasmTrapData();
    void* unwoundPC = trapData.unwoundPC;

    code_ = &tls_->instance->code();
    MOZ_ASSERT(code_ == LookupCode(unwoundPC));

    codeRange_ = code_->lookupFuncRange(unwoundPC);
    MOZ_ASSERT(codeRange_);

    lineOrBytecode_ = trapData.bytecodeOffset;

    MOZ_ASSERT(!done());
    return;
  }

  // Otherwise, execution exits wasm code via an exit stub which sets exitFP
  // to the exit stub's frame. Thus, in this case, we want to start iteration
  // at the caller of the exit frame, whose Code, CodeRange and CallSite are
  // indicated by the returnAddress of the exit stub's frame. If the caller
  // was Ion, we can just skip the wasm frames.

  popFrame();
  MOZ_ASSERT(!done() || unwoundIonCallerFP_);
}

bool WasmFrameIter::done() const {
  MOZ_ASSERT(!!fp_ == !!code_);
  MOZ_ASSERT(!!fp_ == !!codeRange_);
  return !fp_;
}

void WasmFrameIter::operator++() {
  MOZ_ASSERT(!done());

  // When the iterator is set to unwind, each time the iterator pops a frame,
  // the JitActivation is updated so that the just-popped frame is no longer
  // visible. This is necessary since Debugger::onLeaveFrame is called before
  // popping each frame and, once onLeaveFrame is called for a given frame,
  // that frame must not be visible to subsequent stack iteration (or it
  // could be added as a "new" frame just as it becomes garbage).  When the
  // frame is trapping, then exitFP is included in the callstack (otherwise,
  // it is skipped, as explained above). So to unwind the innermost frame, we
  // just clear the trapping state.

  if (unwind_ == Unwind::True) {
    if (activation_->isWasmTrapping()) {
      activation_->finishWasmTrap();
    }
    activation_->setWasmExitFP(fp_);
  }

  popFrame();
}

void WasmFrameIter::popFrame() {
  if (fp_->callerIsExitOrJitEntryFP()) {
    // We run into a frame pointer which has the low bit set,
    // indicating this is a direct call from the jit into the wasm
    // function's body. The call stack resembles this at this point:
    //
    // |---------------------|
    // |      JIT FRAME      |
    // | JIT FAKE EXIT FRAME | <-- tagged fp_->callerFP_
    // |      WASM FRAME     | <-- fp_
    // |---------------------|
    //
    // fp_->callerFP_ points to the fake exit frame set up by the jit caller,
    // and the return-address-to-fp is in JIT code, thus doesn't belong to any
    // wasm instance's code (in particular, there's no associated CodeRange).
    // Mark the frame as such and untag FP.
    MOZ_ASSERT(!LookupCode(fp_->returnAddress()));

    unwoundIonCallerFP_ = fp_->jitEntryCaller();
    unwoundIonFrameType_ = FrameType::Exit;

    if (unwind_ == Unwind::True) {
      activation_->setJSExitFP(unwoundIonCallerFP());
      unwoundAddressOfReturnAddress_ = fp_->addressOfReturnAddress();
    }

    fp_ = nullptr;
    code_ = nullptr;
    codeRange_ = nullptr;

    MOZ_ASSERT(done());
    return;
  }

  Frame* prevFP = fp_;
  fp_ = fp_->wasmCaller();
  resumePCinCurrentFrame_ = prevFP->returnAddress();

  if (!fp_) {
    code_ = nullptr;
    codeRange_ = nullptr;

    if (unwind_ == Unwind::True) {
      // We're exiting via the interpreter entry; we can safely reset
      // exitFP.
      activation_->setWasmExitFP(nullptr);
      unwoundAddressOfReturnAddress_ = prevFP->addressOfReturnAddress();
    }

    MOZ_ASSERT(done());
    return;
  }

  void* returnAddress = prevFP->returnAddress();
  code_ = LookupCode(returnAddress, &codeRange_);
  MOZ_ASSERT(codeRange_);

  if (codeRange_->isJitEntry()) {
    // This wasm function has been called through the generic JIT entry by
    // a JIT caller, so the call stack resembles this:
    //
    // |---------------------|
    // |      JIT FRAME      |
    // |  JSJIT TO WASM EXIT | <-- fp_
    // |    WASM JIT ENTRY   | <-- prevFP (already unwound)
    // |      WASM FRAME     | (already unwound)
    // |---------------------|
    //
    // The next value of FP is just a regular jit frame used as a marker to
    // know that we should transition to a JSJit frame iterator.
    unwoundIonCallerFP_ = reinterpret_cast<uint8_t*>(fp_);
    unwoundIonFrameType_ = FrameType::JSJitToWasm;

    fp_ = nullptr;
    code_ = nullptr;
    codeRange_ = nullptr;

    if (unwind_ == Unwind::True) {
      activation_->setJSExitFP(unwoundIonCallerFP());
      unwoundAddressOfReturnAddress_ = prevFP->addressOfReturnAddress();
    }

    MOZ_ASSERT(done());
    return;
  }

  MOZ_ASSERT(codeRange_->kind() == CodeRange::Function);

  const CallSite* callsite = code_->lookupCallSite(returnAddress);
  MOZ_ASSERT(callsite);

  if (callsite->mightBeCrossInstance()) {
    tls_ = ExtractCallerTlsFromFrameWithTls(prevFP);
  }

  MOZ_ASSERT(code_ == &tls()->instance->code());
  lineOrBytecode_ = callsite->lineOrBytecode();

  MOZ_ASSERT(!done());
}

const char* WasmFrameIter::filename() const {
  MOZ_ASSERT(!done());
  return code_->metadata().filename.get();
}

const char16_t* WasmFrameIter::displayURL() const {
  MOZ_ASSERT(!done());
  return code_->metadata().displayURL();
}

bool WasmFrameIter::mutedErrors() const {
  MOZ_ASSERT(!done());
  return code_->metadata().mutedErrors();
}

JSAtom* WasmFrameIter::functionDisplayAtom() const {
  MOZ_ASSERT(!done());

  JSContext* cx = activation_->cx();
  JSAtom* atom = instance()->getFuncDisplayAtom(cx, codeRange_->funcIndex());
  if (!atom) {
    cx->clearPendingException();
    return cx->names().empty;
  }

  return atom;
}

unsigned WasmFrameIter::lineOrBytecode() const {
  MOZ_ASSERT(!done());
  return lineOrBytecode_;
}

uint32_t WasmFrameIter::funcIndex() const {
  MOZ_ASSERT(!done());
  return codeRange_->funcIndex();
}

unsigned WasmFrameIter::computeLine(uint32_t* column) const {
  if (instance()->isAsmJS()) {
    if (column) {
      *column = 1;
    }
    return lineOrBytecode_;
  }

  // As a terrible hack to avoid changing the tons of places that pass around
  // (url, line, column) tuples to instead passing around a Variant that
  // stores a (url, func-index, bytecode-offset) tuple for wasm frames,
  // wasm stuffs its tuple into the existing (url, line, column) tuple,
  // tagging the high bit of the column to indicate "this is a wasm frame".
  // When knowing clients see this bit, they shall render the tuple
  // (url, line, column|bit) as "url:wasm-function[column]:0xline" according
  // to the WebAssembly Web API's Developer-Facing Display Conventions.
  //   https://webassembly.github.io/spec/web-api/index.html#conventions
  // The wasm bytecode offset continues to be passed as the JS line to avoid
  // breaking existing devtools code written when this used to be the case.

  MOZ_ASSERT(!(codeRange_->funcIndex() & ColumnBit));
  if (column) {
    *column = codeRange_->funcIndex() | ColumnBit;
  }
  return lineOrBytecode_;
}

Instance* WasmFrameIter::instance() const {
  MOZ_ASSERT(!done());
  return tls_->instance;
}

void** WasmFrameIter::unwoundAddressOfReturnAddress() const {
  MOZ_ASSERT(done());
  MOZ_ASSERT(unwind_ == Unwind::True);
  MOZ_ASSERT(unwoundAddressOfReturnAddress_);
  return unwoundAddressOfReturnAddress_;
}

bool WasmFrameIter::debugEnabled() const {
  MOZ_ASSERT(!done());

  // Only non-imported functions can have debug frames.
  //
  // Metadata::debugEnabled is only set if debugging is actually enabled (both
  // requested, and available via baseline compilation), and Tier::Debug code
  // will be available.
  return code_->metadata().debugEnabled &&
         codeRange_->funcIndex() >=
             code_->metadata(Tier::Debug).funcImports.length();
}

DebugFrame* WasmFrameIter::debugFrame() const {
  MOZ_ASSERT(!done());
  return DebugFrame::from(fp_);
}

jit::FrameType WasmFrameIter::unwoundIonFrameType() const {
  MOZ_ASSERT(unwoundIonCallerFP_);
  MOZ_ASSERT(unwoundIonFrameType_ != jit::FrameType(-1));
  return unwoundIonFrameType_;
}

uint8_t* WasmFrameIter::resumePCinCurrentFrame() const {
  if (resumePCinCurrentFrame_) {
    return resumePCinCurrentFrame_;
  }
  MOZ_ASSERT(activation_->isWasmTrapping());
  // The next instruction is the instruction following the trap instruction.
  return (uint8_t*)activation_->wasmTrapData().resumePC;
}

/*****************************************************************************/
// Prologue/epilogue code generation

// These constants reflect statically-determined offsets in the
// prologue/epilogue. The offsets are dynamically asserted during code
// generation.
#if defined(JS_CODEGEN_X64)
static const unsigned PushedRetAddr = 0;
static const unsigned PushedFP = 1;
static const unsigned SetFP = 4;
static const unsigned PoppedFP = 0;
#elif defined(JS_CODEGEN_X86)
static const unsigned PushedRetAddr = 0;
static const unsigned PushedFP = 1;
static const unsigned SetFP = 3;
static const unsigned PoppedFP = 0;
#elif defined(JS_CODEGEN_ARM)
static const unsigned BeforePushRetAddr = 0;
static const unsigned PushedRetAddr = 4;
static const unsigned PushedFP = 8;
static const unsigned SetFP = 12;
static const unsigned PoppedFP = 0;
#elif defined(JS_CODEGEN_ARM64)
// On ARM64 we do not use push or pop; the prologues and epilogues are
// structured differently due to restrictions on SP alignment.  Even so,
// PushedRetAddr and PushedFP are used in some restricted contexts
// and must be superficially meaningful.
static const unsigned BeforePushRetAddr = 0;
static const unsigned PushedRetAddr = 8;
static const unsigned PushedFP = 12;
static const unsigned SetFP = 16;
static const unsigned PoppedFP = 4;
static_assert(BeforePushRetAddr == 0, "Required by StartUnwinding");
static_assert(PushedFP > PushedRetAddr, "Required by StartUnwinding");
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
static const unsigned PushedRetAddr = 8;
static const unsigned PushedFP = 12;
static const unsigned SetFP = 16;
static const unsigned PoppedFP = 4;
#elif defined(JS_CODEGEN_NONE)
// Synthetic values to satisfy asserts and avoid compiler warnings.
static const unsigned PushedRetAddr = 0;
static const unsigned PushedFP = 1;
static const unsigned SetFP = 2;
static const unsigned PoppedFP = 3;
#else
#  error "Unknown architecture!"
#endif
static constexpr unsigned SetJitEntryFP = PushedRetAddr + SetFP - PushedFP;

static void LoadActivation(MacroAssembler& masm, const Register& dest) {
  // WasmCall pushes a JitActivation.
  masm.loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, cx)), dest);
  masm.loadPtr(Address(dest, JSContext::offsetOfActivation()), dest);
}

void wasm::SetExitFP(MacroAssembler& masm, ExitReason reason,
                     Register scratch) {
  MOZ_ASSERT(!reason.isNone());

  LoadActivation(masm, scratch);

  masm.store32(
      Imm32(reason.encode()),
      Address(scratch, JitActivation::offsetOfEncodedWasmExitReason()));

  masm.orPtr(Imm32(ExitOrJitEntryFPTag), FramePointer);
  masm.storePtr(FramePointer,
                Address(scratch, JitActivation::offsetOfPackedExitFP()));
  masm.andPtr(Imm32(int32_t(~ExitOrJitEntryFPTag)), FramePointer);
}

void wasm::ClearExitFP(MacroAssembler& masm, Register scratch) {
  LoadActivation(masm, scratch);
  masm.storePtr(ImmWord(0x0),
                Address(scratch, JitActivation::offsetOfPackedExitFP()));
  masm.store32(
      Imm32(0x0),
      Address(scratch, JitActivation::offsetOfEncodedWasmExitReason()));
}

static void GenerateCallablePrologue(MacroAssembler& masm, uint32_t* entry) {
  masm.setFramePushed(0);

  // ProfilingFrameIterator needs to know the offsets of several key
  // instructions from entry. To save space, we make these offsets static
  // constants and assert that they match the actual codegen below. On ARM,
  // this requires AutoForbidPoolsAndNops to prevent a constant pool from being
  // randomly inserted between two instructions.

  // The size of the prologue is constrained to be no larger than the difference
  // between WasmCheckedTailEntryOffset and WasmCheckedCallEntryOffset; to
  // conserve code space / avoid excessive padding, this difference is made as
  // tight as possible.

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  {
    *entry = masm.currentOffset();

    masm.subFromStackPtr(Imm32(sizeof(Frame)));
    masm.storePtr(ra, Address(StackPointer, Frame::returnAddressOffset()));
    MOZ_ASSERT_IF(!masm.oom(), PushedRetAddr == masm.currentOffset() - *entry);
    masm.storePtr(FramePointer, Address(StackPointer, Frame::callerFPOffset()));
    MOZ_ASSERT_IF(!masm.oom(), PushedFP == masm.currentOffset() - *entry);
    masm.moveStackPtrTo(FramePointer);
    MOZ_ASSERT_IF(!masm.oom(), SetFP == masm.currentOffset() - *entry);
  }
#elif defined(JS_CODEGEN_ARM64)
  {
    // We do not use the PseudoStackPointer.  However, we may be called in a
    // context -- compilation using Ion -- in which the PseudoStackPointer is
    // in use.  Rather than risk confusion in the uses of `masm` here, let's
    // just switch in the real SP, do what we need to do, and restore the
    // existing setting afterwards.
    const vixl::Register stashedSPreg = masm.GetStackPointer64();
    masm.SetStackPointer64(vixl::sp);

    AutoForbidPoolsAndNops afp(&masm,
                               /* number of instructions in scope = */ 4);

    *entry = masm.currentOffset();

    masm.Sub(sp, sp, sizeof(Frame));
    masm.Str(ARMRegister(lr, 64), MemOperand(sp, Frame::returnAddressOffset()));
    MOZ_ASSERT_IF(!masm.oom(), PushedRetAddr == masm.currentOffset() - *entry);
    masm.Str(ARMRegister(FramePointer, 64),
             MemOperand(sp, Frame::callerFPOffset()));
    MOZ_ASSERT_IF(!masm.oom(), PushedFP == masm.currentOffset() - *entry);
    masm.Mov(ARMRegister(FramePointer, 64), sp);
    MOZ_ASSERT_IF(!masm.oom(), SetFP == masm.currentOffset() - *entry);

    // And restore the SP-reg setting, per comment above.
    masm.SetStackPointer64(stashedSPreg);
  }
#else
  {
#  if defined(JS_CODEGEN_ARM)
    AutoForbidPoolsAndNops afp(&masm,
                               /* number of instructions in scope = */ 3);

    *entry = masm.currentOffset();

    static_assert(BeforePushRetAddr == 0);
    masm.push(lr);
#  else
    *entry = masm.currentOffset();
    // The x86/x64 call instruction pushes the return address.
#  endif

    MOZ_ASSERT_IF(!masm.oom(), PushedRetAddr == masm.currentOffset() - *entry);
    masm.push(FramePointer);
    MOZ_ASSERT_IF(!masm.oom(), PushedFP == masm.currentOffset() - *entry);
    masm.moveStackPtrTo(FramePointer);
    MOZ_ASSERT_IF(!masm.oom(), SetFP == masm.currentOffset() - *entry);
  }
#endif
}

static void GenerateCallableEpilogue(MacroAssembler& masm, unsigned framePushed,
                                     ExitReason reason, uint32_t* ret) {
  if (framePushed) {
    masm.freeStack(framePushed);
  }

  if (!reason.isNone()) {
    ClearExitFP(masm, ABINonArgReturnVolatileReg);
  }

  DebugOnly<uint32_t> poppedFP;

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)

  masm.loadPtr(Address(StackPointer, Frame::callerFPOffset()), FramePointer);
  poppedFP = masm.currentOffset();
  masm.loadPtr(Address(StackPointer, Frame::returnAddressOffset()), ra);

  *ret = masm.currentOffset();
  masm.as_jr(ra);
  masm.addToStackPtr(Imm32(sizeof(Frame)));

#elif defined(JS_CODEGEN_ARM64)

  // See comment at equivalent place in |GenerateCallablePrologue| above.
  const vixl::Register stashedSPreg = masm.GetStackPointer64();
  masm.SetStackPointer64(vixl::sp);

  AutoForbidPoolsAndNops afp(&masm, /* number of instructions in scope = */ 5);

  masm.Ldr(ARMRegister(FramePointer, 64),
           MemOperand(sp, Frame::callerFPOffset()));
  poppedFP = masm.currentOffset();

  masm.Ldr(ARMRegister(lr, 64), MemOperand(sp, Frame::returnAddressOffset()));
  *ret = masm.currentOffset();

  masm.Add(sp, sp, sizeof(Frame));

  // Reinitialise PSP from SP. This is less than elegant because the prologue
  // operates on the raw stack pointer SP and does not keep the PSP in sync.
  // We can't use initPseudoStackPtr here because we just set up masm to not
  // use it.  Hence we have to do it "by hand".
  masm.Mov(PseudoStackPointer64, vixl::sp);

  masm.Ret(ARMRegister(lr, 64));

  // See comment at equivalent place in |GenerateCallablePrologue| above.
  masm.SetStackPointer64(stashedSPreg);

#else
  // Forbid pools for the same reason as described in GenerateCallablePrologue.
#  if defined(JS_CODEGEN_ARM)
  AutoForbidPoolsAndNops afp(&masm, /* number of instructions in scope = */ 6);
#  endif

  // There is an important ordering constraint here: fp must be repointed to
  // the caller's frame before any field of the frame currently pointed to by
  // fp is popped: asynchronous signal handlers (which use stack space
  // starting at sp) could otherwise clobber these fields while they are still
  // accessible via fp (fp fields are read during frame iteration which is
  // *also* done asynchronously).

  masm.pop(FramePointer);
  poppedFP = masm.currentOffset();

  *ret = masm.currentOffset();
  masm.ret();

#endif

  MOZ_ASSERT_IF(!masm.oom(), PoppedFP == *ret - poppedFP);
}

void wasm::GenerateFunctionPrologue(MacroAssembler& masm,
                                    const TypeIdDesc& funcTypeId,
                                    const Maybe<uint32_t>& tier1FuncIndex,
                                    FuncOffsets* offsets) {
  // These constants reflect statically-determined offsets between a function's
  // checked call entry and the checked tail's entry, see diagram below.  The
  // Entry is a call target, so must have CodeAlignment, but the TailEntry is
  // only a jump target from a stub.
  //
  // The CheckedCallEntryOffset is normally zero.
  //
  // CheckedTailEntryOffset > CheckedCallEntryOffset, and if CPSIZE is the size
  // of the callable prologue then TailEntryOffset - CallEntryOffset >= CPSIZE.
  // It is a goal to keep that difference as small as possible to reduce the
  // amount of padding inserted in the prologue.
  static_assert(WasmCheckedCallEntryOffset % CodeAlignment == 0,
                "code aligned");
  static_assert(WasmCheckedTailEntryOffset > WasmCheckedCallEntryOffset);

  // Flush pending pools so they do not get dumped between the 'begin' and
  // 'uncheckedCallEntry' offsets since the difference must be less than
  // UINT8_MAX to be stored in CodeRange::funcbeginToUncheckedCallEntry_.
  // (Pending pools can be large.)
  masm.flushBuffer();
  masm.haltingAlign(CodeAlignment);

  // We are going to generate the next code layout:
  // ---------------------------------------------
  // checked call entry:    callable prologue
  // checked tail entry:    check signature
  //                        jump functionBody
  // unchecked call entry:  callable prologue
  //                        functionBody
  // -----------------------------------------------
  // checked call entry - used for call_indirect when we have to check the
  // signature.
  //
  // checked tail entry - used by indirect call trampolines which already
  // had pushed Frame on the callee’s behalf.
  //
  // unchecked call entry - used for regular direct same-instance calls.

  Label functionBody;

  // Generate checked call entry. The BytecodeOffset of the trap is fixed up to
  // be the bytecode offset of the callsite by JitActivation::startWasmTrap.
  offsets->begin = masm.currentOffset();
  MOZ_ASSERT_IF(!masm.oom(), masm.currentOffset() - offsets->begin ==
                                 WasmCheckedCallEntryOffset);
  uint32_t dummy;
  GenerateCallablePrologue(masm, &dummy);

  // Check that we did not overshoot the space budget for the prologue.
  MOZ_ASSERT_IF(!masm.oom(), masm.currentOffset() - offsets->begin <=
                                 WasmCheckedTailEntryOffset);

  // Pad to WasmCheckedTailEntryOffset.  Don't use nopAlign because the target
  // offset is not necessarily a power of two.  The expected number of NOPs here
  // is very small.
  while (masm.currentOffset() - offsets->begin < WasmCheckedTailEntryOffset) {
    masm.nop();
  }

  // Signature check starts at WasmCheckedTailEntryOffset.
  MOZ_ASSERT_IF(!masm.oom(), masm.currentOffset() - offsets->begin ==
                                 WasmCheckedTailEntryOffset);
  switch (funcTypeId.kind()) {
    case TypeIdDescKind::Global: {
      Register scratch = WasmTableCallScratchReg0;
      masm.loadWasmGlobalPtr(funcTypeId.globalDataOffset(), scratch);
      masm.branchPtr(Assembler::Condition::Equal, WasmTableCallSigReg, scratch,
                     &functionBody);
      masm.wasmTrap(Trap::IndirectCallBadSig, BytecodeOffset(0));
      break;
    }
    case TypeIdDescKind::Immediate: {
      masm.branch32(Assembler::Condition::Equal, WasmTableCallSigReg,
                    Imm32(funcTypeId.immediate()), &functionBody);
      masm.wasmTrap(Trap::IndirectCallBadSig, BytecodeOffset(0));
      break;
    }
    case TypeIdDescKind::None:
      masm.jump(&functionBody);
      break;
  }

  // The preceding code may have generated a small constant pool to support the
  // comparison in the signature check.  But if we flush the pool here we will
  // also force the creation of an unused branch veneer in the pool for the jump
  // to functionBody from the signature check on some platforms, thus needlessly
  // inflating the size of the prologue.
  //
  // On no supported platform that uses a pool (arm, arm64) is there any risk at
  // present of that branch or other elements in the pool going out of range
  // while we're generating the following padding and prologue, therefore no
  // pool elements will be emitted in the prologue, therefore it is safe not to
  // flush here.
  //
  // We assert that this holds at runtime by comparing the expected entry offset
  // to the recorded ditto; if they are not the same then
  // GenerateCallablePrologue flushed a pool before the prologue code, contrary
  // to assumption.

  // Generate unchecked call entry:
  masm.nopAlign(CodeAlignment);
  DebugOnly<uint32_t> expectedEntry = masm.currentOffset();
  GenerateCallablePrologue(masm, &offsets->uncheckedCallEntry);
  MOZ_ASSERT(expectedEntry == offsets->uncheckedCallEntry);
  masm.bind(&functionBody);
#ifdef JS_CODEGEN_ARM64
  // GenerateCallablePrologue creates a prologue which operates on the raw
  // stack pointer and does not keep the PSP in sync.  So we have to resync it
  // here.  But we can't use initPseudoStackPtr here because masm may not be
  // set up to use it, depending on which compiler is in use.  Hence do it
  // "manually".
  masm.Mov(PseudoStackPointer64, vixl::sp);
#endif

  // See comment block in WasmCompile.cpp for an explanation tiering.
  if (tier1FuncIndex) {
    Register scratch = ABINonArgReg0;
    masm.loadPtr(Address(WasmTlsReg, offsetof(TlsData, jumpTable)), scratch);
    masm.jump(Address(scratch, *tier1FuncIndex * sizeof(uintptr_t)));
  }

  offsets->tierEntry = masm.currentOffset();

  MOZ_ASSERT(masm.framePushed() == 0);
}

void wasm::GenerateFunctionEpilogue(MacroAssembler& masm, unsigned framePushed,
                                    FuncOffsets* offsets) {
  // Inverse of GenerateFunctionPrologue:
  MOZ_ASSERT(masm.framePushed() == framePushed);
  GenerateCallableEpilogue(masm, framePushed, ExitReason::None(),
                           &offsets->ret);
  MOZ_ASSERT(masm.framePushed() == 0);
}

void wasm::GenerateExitPrologue(MacroAssembler& masm, unsigned framePushed,
                                ExitReason reason, CallableOffsets* offsets) {
  masm.haltingAlign(CodeAlignment);

  GenerateCallablePrologue(masm, &offsets->begin);

  // This frame will be exiting compiled code to C++ so record the fp and
  // reason in the JitActivation so the frame iterators can unwind.
  SetExitFP(masm, reason, ABINonArgReturnVolatileReg);

  MOZ_ASSERT(masm.framePushed() == 0);
  masm.reserveStack(framePushed);
}

void wasm::GenerateExitEpilogue(MacroAssembler& masm, unsigned framePushed,
                                ExitReason reason, CallableOffsets* offsets) {
  // Inverse of GenerateExitPrologue:
  MOZ_ASSERT(masm.framePushed() == framePushed);
  GenerateCallableEpilogue(masm, framePushed, reason, &offsets->ret);
  MOZ_ASSERT(masm.framePushed() == 0);
}

static void AssertNoWasmExitFPInJitExit(MacroAssembler& masm) {
  // As a general stack invariant, if Activation::packedExitFP is tagged as
  // wasm, it must point to a valid wasm::Frame. The JIT exit stub calls into
  // JIT code and thus does not really exit, thus, when entering/leaving the
  // JIT exit stub from/to normal wasm code, packedExitFP is not tagged wasm.
#ifdef DEBUG
  Register scratch = ABINonArgReturnReg0;
  LoadActivation(masm, scratch);

  Label ok;
  masm.branchTestPtr(Assembler::Zero,
                     Address(scratch, JitActivation::offsetOfPackedExitFP()),
                     Imm32(ExitOrJitEntryFPTag), &ok);
  masm.breakpoint();
  masm.bind(&ok);
#endif
}

void wasm::GenerateJitExitPrologue(MacroAssembler& masm, unsigned framePushed,
                                   CallableOffsets* offsets) {
  masm.haltingAlign(CodeAlignment);

  GenerateCallablePrologue(masm, &offsets->begin);
  AssertNoWasmExitFPInJitExit(masm);

  MOZ_ASSERT(masm.framePushed() == 0);
  masm.reserveStack(framePushed);
}

void wasm::GenerateJitExitEpilogue(MacroAssembler& masm, unsigned framePushed,
                                   CallableOffsets* offsets) {
  // Inverse of GenerateJitExitPrologue:
  MOZ_ASSERT(masm.framePushed() == framePushed);
  AssertNoWasmExitFPInJitExit(masm);
  GenerateCallableEpilogue(masm, framePushed, ExitReason::None(),
                           &offsets->ret);
  MOZ_ASSERT(masm.framePushed() == 0);
}

void wasm::GenerateJitEntryPrologue(MacroAssembler& masm, Offsets* offsets) {
  masm.haltingAlign(CodeAlignment);

  {
#if defined(JS_CODEGEN_ARM)
    AutoForbidPoolsAndNops afp(&masm,
                               /* number of instructions in scope = */ 2);
    offsets->begin = masm.currentOffset();
    static_assert(BeforePushRetAddr == 0);
    masm.push(lr);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    offsets->begin = masm.currentOffset();
    masm.push(ra);
#elif defined(JS_CODEGEN_ARM64)
    AutoForbidPoolsAndNops afp(&masm,
                               /* number of instructions in scope = */ 3);
    offsets->begin = masm.currentOffset();
    static_assert(BeforePushRetAddr == 0);
    // Subtract from SP first as SP must be aligned before offsetting.
    masm.Sub(sp, sp, 8);
    masm.storePtr(lr, Address(masm.getStackPointer(), 0));
    masm.adjustFrame(8);
#else
    // The x86/x64 call instruction pushes the return address.
    offsets->begin = masm.currentOffset();
#endif
    MOZ_ASSERT_IF(!masm.oom(),
                  PushedRetAddr == masm.currentOffset() - offsets->begin);

    // Save jit frame pointer, so unwinding from wasm to jit frames is trivial.
    masm.moveStackPtrTo(FramePointer);
    MOZ_ASSERT_IF(!masm.oom(),
                  SetJitEntryFP == masm.currentOffset() - offsets->begin);
  }

  masm.setFramePushed(0);
}

/*****************************************************************************/
// ProfilingFrameIterator

ProfilingFrameIterator::ProfilingFrameIterator()
    : code_(nullptr),
      codeRange_(nullptr),
      callerFP_(nullptr),
      callerPC_(nullptr),
      stackAddress_(nullptr),
      unwoundIonCallerFP_(nullptr),
      exitReason_(ExitReason::Fixed::None) {
  MOZ_ASSERT(done());
}

ProfilingFrameIterator::ProfilingFrameIterator(const JitActivation& activation)
    : code_(nullptr),
      codeRange_(nullptr),
      callerFP_(nullptr),
      callerPC_(nullptr),
      stackAddress_(nullptr),
      unwoundIonCallerFP_(nullptr),
      exitReason_(activation.wasmExitReason()) {
  initFromExitFP(activation.wasmExitFP());
}

ProfilingFrameIterator::ProfilingFrameIterator(const Frame* fp)
    : code_(nullptr),
      codeRange_(nullptr),
      callerFP_(nullptr),
      callerPC_(nullptr),
      stackAddress_(nullptr),
      unwoundIonCallerFP_(nullptr),
      exitReason_(ExitReason::Fixed::ImportJit) {
  MOZ_ASSERT(fp);
  initFromExitFP(fp);
}

static inline void AssertDirectJitCall(const void* fp) {
  // Called via an inlined fast JIT to wasm call: in this case, FP is
  // pointing in the middle of the exit frame, right before the exit
  // footer; ensure the exit frame type is the expected one.
#ifdef DEBUG
  if (Frame::isExitOrJitEntryFP(fp)) {
    fp = Frame::toJitEntryCaller(fp);
  }
  auto* jitCaller = (ExitFrameLayout*)fp;
  MOZ_ASSERT(jitCaller->footer()->type() ==
             jit::ExitFrameType::DirectWasmJitCall);
#endif
}

static inline void AssertMatchesCallSite(void* callerPC, uint8_t* callerFP) {
#ifdef DEBUG
  const CodeRange* callerCodeRange;
  const Code* code = LookupCode(callerPC, &callerCodeRange);

  if (!code) {
    AssertDirectJitCall(callerFP);
    return;
  }

  MOZ_ASSERT(callerCodeRange);

  if (callerCodeRange->isInterpEntry()) {
    MOZ_ASSERT(callerFP == nullptr);
    return;
  }

  if (callerCodeRange->isJitEntry()) {
    MOZ_ASSERT(callerFP != nullptr);
    return;
  }

  const CallSite* callsite = code->lookupCallSite(callerPC);
  MOZ_ASSERT(callsite);
#endif
}

void ProfilingFrameIterator::initFromExitFP(const Frame* fp) {
  MOZ_ASSERT(fp);
  stackAddress_ = (void*)fp;
  code_ = LookupCode(fp->returnAddress(), &codeRange_);

  if (!code_) {
    // This is a direct call from the JIT, the caller FP is pointing to a
    // tagged JIT caller's frame.
    AssertDirectJitCall(fp->jitEntryCaller());

    unwoundIonCallerFP_ = fp->jitEntryCaller();
    MOZ_ASSERT(done());
    return;
  }

  MOZ_ASSERT(codeRange_);

  // Since we don't have the pc for fp, start unwinding at the caller of fp.
  // This means that the innermost frame is skipped. This is fine because:
  //  - for import exit calls, the innermost frame is a thunk, so the first
  //    frame that shows up is the function calling the import;
  //  - for Math and other builtin calls, we note the absence of an exit
  //    reason and inject a fake "builtin" frame; and
  switch (codeRange_->kind()) {
    case CodeRange::InterpEntry:
      callerPC_ = nullptr;
      callerFP_ = nullptr;
      codeRange_ = nullptr;
      exitReason_ = ExitReason(ExitReason::Fixed::FakeInterpEntry);
      break;
    case CodeRange::JitEntry:
      callerPC_ = nullptr;
      callerFP_ = nullptr;
      unwoundIonCallerFP_ = fp->rawCaller();
      break;
    case CodeRange::Function:
      fp = fp->wasmCaller();
      callerPC_ = fp->returnAddress();
      callerFP_ = fp->rawCaller();
      AssertMatchesCallSite(callerPC_, callerFP_);
      break;
    case CodeRange::ImportJitExit:
    case CodeRange::ImportInterpExit:
    case CodeRange::BuiltinThunk:
    case CodeRange::TrapExit:
    case CodeRange::DebugTrap:
    case CodeRange::Throw:
    case CodeRange::FarJumpIsland:
      MOZ_CRASH("Unexpected CodeRange kind");
  }

  MOZ_ASSERT(!done());
}

static bool isSignatureCheckFail(uint32_t offsetInCode,
                                 const CodeRange* codeRange) {
  if (!codeRange->isFunction()) {
    return false;
  }
  // checked call entry:    1. push Frame
  //                        2. set FP
  //                        3. signature check <--- check if we are here.
  //                        4. jump 7
  // unchecked call entry:  5. push Frame
  //                        6. set FP
  //                        7. function's code
  return offsetInCode < codeRange->funcUncheckedCallEntry() &&
         (offsetInCode - codeRange->funcCheckedCallEntry()) > SetFP;
}

const TlsData* js::wasm::GetNearestEffectiveTls(const Frame* fp) {
  while (true) {
    if (fp->callerIsExitOrJitEntryFP()) {
      // It is a direct call from JIT.
      MOZ_ASSERT(!LookupCode(fp->returnAddress()));
      return ExtractCalleeTlsFromFrameWithTls(fp);
    }

    uint8_t* returnAddress = fp->returnAddress();
    const CodeRange* codeRange = nullptr;
    const Code* code = LookupCode(returnAddress, &codeRange);
    MOZ_ASSERT(codeRange);

    if (codeRange->isEntry()) {
      return ExtractCalleeTlsFromFrameWithTls(fp);
    }

    MOZ_ASSERT(codeRange->kind() == CodeRange::Function);
    MOZ_ASSERT(code);
    const CallSite* callsite = code->lookupCallSite(returnAddress);
    if (callsite->mightBeCrossInstance()) {
      return ExtractCalleeTlsFromFrameWithTls(fp);
    }

    fp = fp->wasmCaller();
  }
}

TlsData* js::wasm::GetNearestEffectiveTls(Frame* fp) {
  return const_cast<TlsData*>(
      GetNearestEffectiveTls(const_cast<const Frame*>(fp)));
}

bool js::wasm::StartUnwinding(const RegisterState& registers,
                              UnwindState* unwindState, bool* unwoundCaller) {
  // Shorthands.
  uint8_t* const pc = (uint8_t*)registers.pc;
  void** const sp = (void**)registers.sp;

  // The frame pointer might be:
  // - in the process of tagging/untagging when calling into the JITs;
  // make sure it's untagged.
  // - tagged by an direct JIT call.
  // - unreliable if it's not been set yet, in prologues.
  uint8_t* fp = Frame::isExitOrJitEntryFP(registers.fp)
                    ? Frame::toJitEntryCaller(registers.fp)
                    : reinterpret_cast<uint8_t*>(registers.fp);

  // Get the CodeRange describing pc and the base address to which the
  // CodeRange is relative. If the pc is not in a wasm module or a builtin
  // thunk, then execution must be entering from or leaving to the C++ caller
  // that pushed the JitActivation.
  const CodeRange* codeRange;
  uint8_t* codeBase;
  const Code* code = nullptr;

  const CodeSegment* codeSegment = LookupCodeSegment(pc, &codeRange);
  if (codeSegment) {
    code = &codeSegment->code();
    codeBase = codeSegment->base();
    MOZ_ASSERT(codeRange);
  } else if (!LookupBuiltinThunk(pc, &codeRange, &codeBase)) {
    return false;
  }

  // When the pc is inside the prologue/epilogue, the innermost call's Frame
  // is not complete and thus fp points to the second-to-innermost call's
  // Frame. Since fp can only tell you about its caller, naively unwinding
  // while pc is in the prologue/epilogue would skip the second-to-innermost
  // call. To avoid this problem, we use the static structure of the code in
  // the prologue and epilogue to do the Right Thing.
  uint32_t offsetInCode = pc - codeBase;
  MOZ_ASSERT(offsetInCode >= codeRange->begin());
  MOZ_ASSERT(offsetInCode < codeRange->end());

  // Compute the offset of the pc from the (unchecked call) entry of the code
  // range. The checked call entry and the unchecked call entry have common
  // prefix, so pc before signature check in the checked call entry is
  // equivalent to the pc of the unchecked-call-entry. Thus, we can simplify the
  // below case analysis by redirecting all pc-in-checked-call-entry before
  // signature check cases to the pc-at-unchecked-call-entry case.
  uint32_t offsetFromEntry;
  if (codeRange->isFunction()) {
    if (offsetInCode < codeRange->funcUncheckedCallEntry()) {
      offsetFromEntry = offsetInCode - codeRange->funcCheckedCallEntry();
    } else {
      offsetFromEntry = offsetInCode - codeRange->funcUncheckedCallEntry();
    }
  } else {
    offsetFromEntry = offsetInCode - codeRange->begin();
  }

  // Most cases end up unwinding to the caller state; not unwinding is the
  // exception here.
  *unwoundCaller = true;

  uint8_t* fixedFP = nullptr;
  void* fixedPC = nullptr;
  switch (codeRange->kind()) {
    case CodeRange::Function:
    case CodeRange::FarJumpIsland:
    case CodeRange::ImportJitExit:
    case CodeRange::ImportInterpExit:
    case CodeRange::BuiltinThunk:
    case CodeRange::DebugTrap:
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
      if (codeRange->isThunk()) {
        // The FarJumpIsland sequence temporary scrambles ra.
        // Don't unwind to caller.
        fixedPC = pc;
        fixedFP = fp;
        *unwoundCaller = false;
        AssertMatchesCallSite(
            Frame::fromUntaggedWasmExitFP(fp)->returnAddress(),
            Frame::fromUntaggedWasmExitFP(fp)->rawCaller());
      } else if (offsetFromEntry < PushedFP) {
        // On MIPS we rely on register state instead of state saved on
        // stack until the wasm::Frame is completely built.
        // On entry the return address is in ra (registers.lr) and
        // fp holds the caller's fp.
        fixedPC = (uint8_t*)registers.lr;
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
      } else
#elif defined(JS_CODEGEN_ARM64)
      if (offsetFromEntry < PushedFP || codeRange->isThunk()) {
        // Constraints above ensure that this covers BeforePushRetAddr and
        // PushedRetAddr.
        //
        // On ARM64 we subtract the size of the Frame from SP and then store
        // values into the stack.  Execution can be interrupted at various
        // places in that sequence.  We rely on the register state for our
        // values.
        fixedPC = (uint8_t*)registers.lr;
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
      } else
#elif defined(JS_CODEGEN_ARM)
      if (offsetFromEntry == BeforePushRetAddr || codeRange->isThunk()) {
        // The return address is still in lr and fp holds the caller's fp.
        fixedPC = (uint8_t*)registers.lr;
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
      } else
#endif
          if (offsetFromEntry == PushedRetAddr || codeRange->isThunk()) {
        // The return address has been pushed on the stack but fp still
        // points to the caller's fp.
        fixedPC = sp[0];
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
      } else if (offsetFromEntry == PushedFP) {
        // The full Frame has been pushed; fp is still the caller's fp.
        const auto* frame = Frame::fromUntaggedWasmExitFP(sp);
        DebugOnly<const uint8_t*> caller = frame->callerIsExitOrJitEntryFP()
                                               ? frame->jitEntryCaller()
                                               : frame->rawCaller();
        MOZ_ASSERT(caller == fp);
        fixedPC = frame->returnAddress();
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
      } else if (offsetInCode >= codeRange->ret() - PoppedFP &&
                 offsetInCode <= codeRange->ret()) {
        // The fixedFP field of the Frame has been loaded into fp.
        // The ra and TLS might also be loaded, but the Frame structure is
        // still on stack, so we can acess the ra form there.
        MOZ_ASSERT(*sp == fp);
        fixedPC = Frame::fromUntaggedWasmExitFP(sp)->returnAddress();
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
#elif defined(JS_CODEGEN_ARM64)
        // The stack pointer does not move until all values have
        // been restored so several cases can be coalesced here.
      } else if (offsetInCode >= codeRange->ret() - PoppedFP &&
                 offsetInCode <= codeRange->ret()) {
        fixedPC = Frame::fromUntaggedWasmExitFP(sp)->returnAddress();
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
#else
      } else if (offsetInCode >= codeRange->ret() - PoppedFP &&
                 offsetInCode < codeRange->ret()) {
        // The fixedFP field of the Frame has been popped into fp.
        fixedPC = sp[1];
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
      } else if (offsetInCode == codeRange->ret()) {
        // Both the TLS and fixedFP fields have been popped and fp now
        // points to the caller's frame.
        fixedPC = sp[0];
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
#endif
      } else {
        if (codeRange->kind() == CodeRange::ImportJitExit) {
          // The jit exit contains a range where the value of FP can't be
          // trusted. Technically, we could recover fp from sp, but since
          // the range is so short, for now just drop the stack.
          if (offsetInCode >= codeRange->jitExitUntrustedFPStart() &&
              offsetInCode < codeRange->jitExitUntrustedFPEnd()) {
            return false;
          }
        }

        if (isSignatureCheckFail(offsetInCode, codeRange)) {
          // Frame have been pushed and FP has been set.
          const auto* frame = Frame::fromUntaggedWasmExitFP(fp);
          fixedFP = frame->rawCaller();
          fixedPC = frame->returnAddress();
          AssertMatchesCallSite(fixedPC, fixedFP);
          break;
        }

        // Not in the prologue/epilogue.
        fixedPC = pc;
        fixedFP = fp;
        *unwoundCaller = false;
        AssertMatchesCallSite(
            Frame::fromUntaggedWasmExitFP(fp)->returnAddress(),
            Frame::fromUntaggedWasmExitFP(fp)->rawCaller());
        break;
      }
      break;
    case CodeRange::TrapExit:
      // These code stubs execute after the prologue/epilogue have completed
      // so pc/fp contains the right values here.
      fixedPC = pc;
      fixedFP = fp;
      *unwoundCaller = false;
      AssertMatchesCallSite(Frame::fromUntaggedWasmExitFP(fp)->returnAddress(),
                            Frame::fromUntaggedWasmExitFP(fp)->rawCaller());
      break;
    case CodeRange::InterpEntry:
      // The entry trampoline is the final frame in an wasm JitActivation. The
      // entry trampoline also doesn't GeneratePrologue/Epilogue so we can't
      // use the general unwinding logic above.
      break;
    case CodeRange::JitEntry:
      // There's a jit frame above the current one; we don't care about pc
      // since the Jit entry frame is a jit frame which can be considered as
      // an exit frame.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
      if (offsetFromEntry < PushedRetAddr) {
        // We haven't pushed the jit return address yet, thus the jit
        // frame is incomplete. During profiling frame iteration, it means
        // that the jit profiling frame iterator won't be able to unwind
        // this frame; drop it.
        return false;
      }
#endif
      fixedFP =
          offsetFromEntry < SetJitEntryFP ? reinterpret_cast<uint8_t*>(sp) : fp;
      fixedPC = nullptr;

      // On the error return path, FP might be set to FailFP. Ignore these
      // transient frames.
      if (intptr_t(fixedFP) == (FailFP & ~ExitOrJitEntryFPTag)) {
        return false;
      }
      break;
    case CodeRange::Throw:
      // The throw stub executes a small number of instructions before popping
      // the entire activation. To simplify testing, we simply pretend throw
      // stubs have already popped the entire stack.
      return false;
  }

  unwindState->code = code;
  unwindState->codeRange = codeRange;
  unwindState->fp = fixedFP;
  unwindState->pc = fixedPC;
  return true;
}

ProfilingFrameIterator::ProfilingFrameIterator(const JitActivation& activation,
                                               const RegisterState& state)
    : code_(nullptr),
      codeRange_(nullptr),
      callerFP_(nullptr),
      callerPC_(nullptr),
      stackAddress_(nullptr),
      unwoundIonCallerFP_(nullptr),
      exitReason_(ExitReason::Fixed::None) {
  // Let wasmExitFP take precedence to StartUnwinding when it is set since
  // during the body of an exit stub, the register state may not be valid
  // causing StartUnwinding() to abandon unwinding this activation.
  if (activation.hasWasmExitFP()) {
    exitReason_ = activation.wasmExitReason();
    initFromExitFP(activation.wasmExitFP());
    return;
  }

  bool unwoundCaller;
  UnwindState unwindState;
  if (!StartUnwinding(state, &unwindState, &unwoundCaller)) {
    MOZ_ASSERT(done());
    return;
  }

  MOZ_ASSERT(unwindState.codeRange);

  if (unwoundCaller) {
    callerFP_ = unwindState.fp;
    callerPC_ = unwindState.pc;
    // In the case of a function call, if the original FP value is tagged,
    // then we're being called through a direct JIT call (the interpreter
    // and the jit entry don't set FP's low bit). We can't observe
    // transient tagged values of FP (during wasm::SetExitFP) here because
    // StartUnwinding would not have unwound then.
    if (unwindState.codeRange->isFunction() &&
        Frame::isExitOrJitEntryFP(reinterpret_cast<uint8_t*>(state.fp))) {
      unwoundIonCallerFP_ = callerFP_;
    }
  } else {
    callerFP_ = Frame::fromUntaggedWasmExitFP(unwindState.fp)->rawCaller();
    callerPC_ = Frame::fromUntaggedWasmExitFP(unwindState.fp)->returnAddress();
    // See comment above. The only way to get a tagged FP here means that
    // the caller is a fast JIT caller which called into a wasm function.
    if (Frame::isExitOrJitEntryFP(callerFP_)) {
      MOZ_ASSERT(unwindState.codeRange->isFunction());
      unwoundIonCallerFP_ = Frame::toJitEntryCaller(callerFP_);
    }
  }

  if (unwindState.codeRange->isJitEntry()) {
    MOZ_ASSERT(!unwoundIonCallerFP_);
    unwoundIonCallerFP_ = callerFP_;
  }

  if (unwindState.codeRange->isInterpEntry()) {
    unwindState.codeRange = nullptr;
    exitReason_ = ExitReason(ExitReason::Fixed::FakeInterpEntry);
  }

  code_ = unwindState.code;
  codeRange_ = unwindState.codeRange;
  stackAddress_ = state.sp;
  MOZ_ASSERT(!done());
}

void ProfilingFrameIterator::operator++() {
  if (!exitReason_.isNone()) {
    DebugOnly<bool> wasInterpEntry = exitReason_.isInterpEntry();
    exitReason_ = ExitReason::None();
    MOZ_ASSERT((!codeRange_) == wasInterpEntry);
    MOZ_ASSERT(done() == wasInterpEntry);
    return;
  }

  if (unwoundIonCallerFP_) {
    MOZ_ASSERT(codeRange_->isFunction() || codeRange_->isJitEntry());
    callerPC_ = nullptr;
    callerFP_ = nullptr;
    codeRange_ = nullptr;
    MOZ_ASSERT(done());
    return;
  }

  if (!callerPC_) {
    MOZ_ASSERT(!callerFP_);
    codeRange_ = nullptr;
    MOZ_ASSERT(done());
    return;
  }

  if (!callerFP_) {
    MOZ_ASSERT(LookupCode(callerPC_, &codeRange_) == code_);
    MOZ_ASSERT(codeRange_->kind() == CodeRange::InterpEntry);
    exitReason_ = ExitReason(ExitReason::Fixed::FakeInterpEntry);
    codeRange_ = nullptr;
    callerPC_ = nullptr;
    MOZ_ASSERT(!done());
    return;
  }

  code_ = LookupCode(callerPC_, &codeRange_);

  if (!code_ && Frame::isExitOrJitEntryFP(callerFP_)) {
    // The parent frame is an inlined wasm call, the tagged FP points to
    // the fake exit frame.
    MOZ_ASSERT(!codeRange_);
    AssertDirectJitCall(callerFP_);
    unwoundIonCallerFP_ = Frame::toJitEntryCaller(callerFP_);
    MOZ_ASSERT(done());
    return;
  }

  MOZ_ASSERT(codeRange_);

  if (codeRange_->isJitEntry()) {
    unwoundIonCallerFP_ = callerFP_;
    MOZ_ASSERT(!done());
    return;
  }

  MOZ_ASSERT(code_ ==
             &GetNearestEffectiveTls(Frame::fromUntaggedWasmExitFP(callerFP_))
                  ->instance->code());

  switch (codeRange_->kind()) {
    case CodeRange::Function:
    case CodeRange::ImportJitExit:
    case CodeRange::ImportInterpExit:
    case CodeRange::BuiltinThunk:
    case CodeRange::TrapExit:
    case CodeRange::DebugTrap:
    case CodeRange::FarJumpIsland: {
      stackAddress_ = callerFP_;
      const auto* frame = Frame::fromUntaggedWasmExitFP(callerFP_);
      callerPC_ = frame->returnAddress();
      AssertMatchesCallSite(callerPC_, frame->rawCaller());
      callerFP_ = frame->rawCaller();
      break;
    }
    case CodeRange::InterpEntry:
      MOZ_CRASH("should have had null caller fp");
    case CodeRange::JitEntry:
      MOZ_CRASH("should have been guarded above");
    case CodeRange::Throw:
      MOZ_CRASH("code range doesn't have frame");
  }

  MOZ_ASSERT(!done());
}

static const char* ThunkedNativeToDescription(SymbolicAddress func) {
  MOZ_ASSERT(NeedsBuiltinThunk(func));
  switch (func) {
    case SymbolicAddress::HandleDebugTrap:
    case SymbolicAddress::HandleThrow:
    case SymbolicAddress::HandleTrap:
    case SymbolicAddress::CallImport_General:
    case SymbolicAddress::CoerceInPlace_ToInt32:
    case SymbolicAddress::CoerceInPlace_ToNumber:
    case SymbolicAddress::CoerceInPlace_ToBigInt:
    case SymbolicAddress::BoxValue_Anyref:
      MOZ_ASSERT(!NeedsBuiltinThunk(func),
                 "not in sync with NeedsBuiltinThunk");
      break;
    case SymbolicAddress::ToInt32:
      return "call to asm.js native ToInt32 coercion (in wasm)";
    case SymbolicAddress::DivI64:
      return "call to native i64.div_s (in wasm)";
    case SymbolicAddress::UDivI64:
      return "call to native i64.div_u (in wasm)";
    case SymbolicAddress::ModI64:
      return "call to native i64.rem_s (in wasm)";
    case SymbolicAddress::UModI64:
      return "call to native i64.rem_u (in wasm)";
    case SymbolicAddress::TruncateDoubleToUint64:
      return "call to native i64.trunc_u/f64 (in wasm)";
    case SymbolicAddress::TruncateDoubleToInt64:
      return "call to native i64.trunc_s/f64 (in wasm)";
    case SymbolicAddress::SaturatingTruncateDoubleToUint64:
      return "call to native i64.trunc_u:sat/f64 (in wasm)";
    case SymbolicAddress::SaturatingTruncateDoubleToInt64:
      return "call to native i64.trunc_s:sat/f64 (in wasm)";
    case SymbolicAddress::Uint64ToDouble:
      return "call to native f64.convert_u/i64 (in wasm)";
    case SymbolicAddress::Uint64ToFloat32:
      return "call to native f32.convert_u/i64 (in wasm)";
    case SymbolicAddress::Int64ToDouble:
      return "call to native f64.convert_s/i64 (in wasm)";
    case SymbolicAddress::Int64ToFloat32:
      return "call to native f32.convert_s/i64 (in wasm)";
#if defined(JS_CODEGEN_ARM)
    case SymbolicAddress::aeabi_idivmod:
      return "call to native i32.div_s (in wasm)";
    case SymbolicAddress::aeabi_uidivmod:
      return "call to native i32.div_u (in wasm)";
#endif
    case SymbolicAddress::AllocateBigInt:
      return "call to native Allocate<BigInt, NoGC> (in wasm)";
    case SymbolicAddress::ModD:
      return "call to asm.js native f64 % (mod)";
    case SymbolicAddress::SinD:
      return "call to asm.js native f64 Math.sin";
    case SymbolicAddress::CosD:
      return "call to asm.js native f64 Math.cos";
    case SymbolicAddress::TanD:
      return "call to asm.js native f64 Math.tan";
    case SymbolicAddress::ASinD:
      return "call to asm.js native f64 Math.asin";
    case SymbolicAddress::ACosD:
      return "call to asm.js native f64 Math.acos";
    case SymbolicAddress::ATanD:
      return "call to asm.js native f64 Math.atan";
    case SymbolicAddress::CeilD:
      return "call to native f64.ceil (in wasm)";
    case SymbolicAddress::CeilF:
      return "call to native f32.ceil (in wasm)";
    case SymbolicAddress::FloorD:
      return "call to native f64.floor (in wasm)";
    case SymbolicAddress::FloorF:
      return "call to native f32.floor (in wasm)";
    case SymbolicAddress::TruncD:
      return "call to native f64.trunc (in wasm)";
    case SymbolicAddress::TruncF:
      return "call to native f32.trunc (in wasm)";
    case SymbolicAddress::NearbyIntD:
      return "call to native f64.nearest (in wasm)";
    case SymbolicAddress::NearbyIntF:
      return "call to native f32.nearest (in wasm)";
    case SymbolicAddress::ExpD:
      return "call to asm.js native f64 Math.exp";
    case SymbolicAddress::LogD:
      return "call to asm.js native f64 Math.log";
    case SymbolicAddress::PowD:
      return "call to asm.js native f64 Math.pow";
    case SymbolicAddress::ATan2D:
      return "call to asm.js native f64 Math.atan2";
    case SymbolicAddress::MemoryGrow:
      return "call to native memory.grow (in wasm)";
    case SymbolicAddress::MemorySize:
      return "call to native memory.size (in wasm)";
    case SymbolicAddress::WaitI32:
      return "call to native i32.wait (in wasm)";
    case SymbolicAddress::WaitI64:
      return "call to native i64.wait (in wasm)";
    case SymbolicAddress::Wake:
      return "call to native wake (in wasm)";
    case SymbolicAddress::CoerceInPlace_JitEntry:
      return "out-of-line coercion for jit entry arguments (in wasm)";
    case SymbolicAddress::ReportV128JSCall:
      return "jit call to v128 wasm function";
    case SymbolicAddress::MemCopy32:
    case SymbolicAddress::MemCopyShared32:
      return "call to native memory.copy function";
    case SymbolicAddress::DataDrop:
      return "call to native data.drop function";
    case SymbolicAddress::MemFill32:
    case SymbolicAddress::MemFillShared32:
      return "call to native memory.fill function";
    case SymbolicAddress::MemInit32:
      return "call to native memory.init function";
    case SymbolicAddress::TableCopy:
      return "call to native table.copy function";
    case SymbolicAddress::TableFill:
      return "call to native table.fill function";
    case SymbolicAddress::ElemDrop:
      return "call to native elem.drop function";
    case SymbolicAddress::TableGet:
      return "call to native table.get function";
    case SymbolicAddress::TableGrow:
      return "call to native table.grow function";
    case SymbolicAddress::TableInit:
      return "call to native table.init function";
    case SymbolicAddress::TableSet:
      return "call to native table.set function";
    case SymbolicAddress::TableSize:
      return "call to native table.size function";
    case SymbolicAddress::RefFunc:
      return "call to native ref.func function";
    case SymbolicAddress::PreBarrierFiltering:
      return "call to native filtering GC prebarrier (in wasm)";
    case SymbolicAddress::PostBarrier:
      return "call to native GC postbarrier (in wasm)";
    case SymbolicAddress::PostBarrierFiltering:
      return "call to native filtering GC postbarrier (in wasm)";
    case SymbolicAddress::StructNew:
      return "call to native struct.new (in wasm)";
#if defined(ENABLE_WASM_EXCEPTIONS)
    case SymbolicAddress::ExceptionNew:
      return "call to native exception new (in wasm)";
    case SymbolicAddress::ThrowException:
      return "call to native throw exception (in wasm)";
    case SymbolicAddress::GetLocalExceptionIndex:
      return "call to native get the local index of an exn's tag (in wasm)";
    case SymbolicAddress::PushRefIntoExn:
      return "call to native that pushes a ref value into an exn (in wasm)";
#endif
    case SymbolicAddress::ArrayNew:
      return "call to native array.new (in wasm)";
    case SymbolicAddress::RefTest:
      return "call to native ref.test (in wasm)";
    case SymbolicAddress::RttSub:
      return "call to native rtt.sub (in wasm)";
    case SymbolicAddress::InlineTypedObjectClass:
      MOZ_CRASH();
#if defined(JS_CODEGEN_MIPS32)
    case SymbolicAddress::js_jit_gAtomic64Lock:
      MOZ_CRASH();
#endif
#ifdef WASM_CODEGEN_DEBUG
    case SymbolicAddress::PrintI32:
    case SymbolicAddress::PrintPtr:
    case SymbolicAddress::PrintF32:
    case SymbolicAddress::PrintF64:
    case SymbolicAddress::PrintText:
#endif
    case SymbolicAddress::Limit:
      break;
  }
  return "?";
}

const char* ProfilingFrameIterator::label() const {
  MOZ_ASSERT(!done());

  // Use the same string for both time inside and under so that the two
  // entries will be coalesced by the profiler.
  // Must be kept in sync with /tools/profiler/tests/test_asm.js
  static const char importJitDescription[] = "fast exit trampoline (in wasm)";
  static const char importInterpDescription[] =
      "slow exit trampoline (in wasm)";
  static const char builtinNativeDescription[] =
      "fast exit trampoline to native (in wasm)";
  static const char trapDescription[] = "trap handling (in wasm)";
  static const char debugTrapDescription[] = "debug trap handling (in wasm)";

  if (!exitReason_.isFixed()) {
    return ThunkedNativeToDescription(exitReason_.symbolic());
  }

  switch (exitReason_.fixed()) {
    case ExitReason::Fixed::None:
      break;
    case ExitReason::Fixed::ImportJit:
      return importJitDescription;
    case ExitReason::Fixed::ImportInterp:
      return importInterpDescription;
    case ExitReason::Fixed::BuiltinNative:
      return builtinNativeDescription;
    case ExitReason::Fixed::Trap:
      return trapDescription;
    case ExitReason::Fixed::DebugTrap:
      return debugTrapDescription;
    case ExitReason::Fixed::FakeInterpEntry:
      return "slow entry trampoline (in wasm)";
  }

  switch (codeRange_->kind()) {
    case CodeRange::Function:
      return code_->profilingLabel(codeRange_->funcIndex());
    case CodeRange::InterpEntry:
      MOZ_CRASH("should be an ExitReason");
    case CodeRange::JitEntry:
      return "fast entry trampoline (in wasm)";
    case CodeRange::ImportJitExit:
      return importJitDescription;
    case CodeRange::BuiltinThunk:
      return builtinNativeDescription;
    case CodeRange::ImportInterpExit:
      return importInterpDescription;
    case CodeRange::TrapExit:
      return trapDescription;
    case CodeRange::DebugTrap:
      return debugTrapDescription;
    case CodeRange::FarJumpIsland:
      return "interstitial (in wasm)";
    case CodeRange::Throw:
      MOZ_CRASH("does not have a frame");
  }

  MOZ_CRASH("bad code range kind");
}
