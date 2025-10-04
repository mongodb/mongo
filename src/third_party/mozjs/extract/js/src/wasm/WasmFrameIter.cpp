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
#include "js/ColumnNumber.h"  // JS::WasmFunctionIndex, LimitedColumnNumberOneOrigin, JS::TaggedColumnNumberOneOrigin, JS::TaggedColumnNumberOneOrigin
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"
#include "wasm/WasmBuiltinModuleGenerated.h"
#include "wasm/WasmDebugFrame.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmInstanceData.h"
#include "wasm/WasmPI.h"
#include "wasm/WasmStubs.h"

#include "jit/MacroAssembler-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::DebugOnly;
using mozilla::Maybe;

static Instance* ExtractCallerInstanceFromFrameWithInstances(Frame* fp) {
  return *reinterpret_cast<Instance**>(
      reinterpret_cast<uint8_t*>(fp) +
      FrameWithInstances::callerInstanceOffset());
}

static const Instance* ExtractCalleeInstanceFromFrameWithInstances(
    const Frame* fp) {
  return *reinterpret_cast<Instance* const*>(
      reinterpret_cast<const uint8_t*>(fp) +
      FrameWithInstances::calleeInstanceOffset());
}

/*****************************************************************************/
// WasmFrameIter implementation

WasmFrameIter::WasmFrameIter(JitActivation* activation, wasm::Frame* fp)
    : activation_(activation),
      code_(nullptr),
      codeRange_(nullptr),
      lineOrBytecode_(0),
      fp_(fp ? fp : activation->wasmExitFP()),
      instance_(nullptr),
      unwoundCallerFP_(nullptr),
      unwind_(Unwind::False),
      unwoundAddressOfReturnAddress_(nullptr),
      resumePCinCurrentFrame_(nullptr),
      failedUnwindSignatureMismatch_(false),
      stackSwitched_(false) {
  MOZ_ASSERT(fp_);
  instance_ = GetNearestEffectiveInstance(fp_);

  // When the stack is captured during a trap (viz., to create the .stack
  // for an Error object), use the pc/bytecode information captured by the
  // signal handler in the runtime. Take care not to use this trap unwind
  // state for wasm frames in the middle of a JitActivation, i.e., wasm frames
  // that called into JIT frames before the trap.

  if (activation->isWasmTrapping() && fp_ == activation->wasmExitFP()) {
    const TrapData& trapData = activation->wasmTrapData();
    void* unwoundPC = trapData.unwoundPC;

    code_ = &instance_->code();
    MOZ_ASSERT(code_ == LookupCode(unwoundPC));

    codeRange_ = code_->lookupFuncRange(unwoundPC);
    MOZ_ASSERT(codeRange_);

    lineOrBytecode_ = trapData.bytecodeOffset;
    failedUnwindSignatureMismatch_ = trapData.failedUnwindSignatureMismatch;

#ifdef ENABLE_WASM_TAIL_CALLS
    // The debugEnabled() relies on valid value of resumePCinCurrentFrame_
    // to identify DebugFrame. Normally this field is updated at popFrame().
    // The only case when this can happend is during IndirectCallBadSig
    // trapping and stack unwinding. The top frame will never be at ReturnStub
    // callsite, except during IndirectCallBadSig unwinding.
    const CallSite* site = code_->lookupCallSite(unwoundPC);
    if (site && site->kind() == CallSite::ReturnStub) {
      MOZ_ASSERT(trapData.trap == Trap::IndirectCallBadSig);
      resumePCinCurrentFrame_ = (uint8_t*)unwoundPC;
    }
#endif

    MOZ_ASSERT(!done());
    return;
  }

  // Otherwise, execution exits wasm code via an exit stub which sets exitFP
  // to the exit stub's frame. Thus, in this case, we want to start iteration
  // at the caller of the exit frame, whose Code, CodeRange and CallSite are
  // indicated by the returnAddress of the exit stub's frame. If the caller
  // was Ion, we can just skip the wasm frames.

  popFrame();
  MOZ_ASSERT(!done() || unwoundCallerFP_);
}

WasmFrameIter::WasmFrameIter(FrameWithInstances* fp, void* returnAddress)
    : activation_(nullptr),
      code_(nullptr),
      codeRange_(nullptr),
      lineOrBytecode_(0),
      fp_(fp),
      instance_(fp->calleeInstance()),
      unwoundCallerFP_(nullptr),
      unwind_(Unwind::False),
      unwoundAddressOfReturnAddress_(nullptr),
      resumePCinCurrentFrame_((uint8_t*)returnAddress),
      failedUnwindSignatureMismatch_(false),
      stackSwitched_(false) {
  // Specialized implementation to avoid popFrame() interation.
  // It is expected that the iterator starts at a callsite that is in
  // the function body and has instance reference.
  code_ = LookupCode(returnAddress, &codeRange_);
  MOZ_ASSERT(code_ && codeRange_ && codeRange_->kind() == CodeRange::Function);

  const CallSite* callsite = code_->lookupCallSite(returnAddress);
  MOZ_ASSERT(callsite && callsite->mightBeCrossInstance());

#ifdef ENABLE_WASM_JSPI
  stackSwitched_ = callsite->isStackSwitch();
#endif

  MOZ_ASSERT(code_ == &instance_->code());
  lineOrBytecode_ = callsite->lineOrBytecode();
  failedUnwindSignatureMismatch_ = false;

  MOZ_ASSERT(!done());
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

static inline void AssertDirectJitCall(const void* fp) {
  // Called via an inlined fast JIT to wasm call: in this case, FP is
  // pointing in the middle of the exit frame, right before the exit
  // footer; ensure the exit frame type is the expected one.
#ifdef DEBUG
  auto* jitCaller = (ExitFrameLayout*)fp;
  MOZ_ASSERT(jitCaller->footer()->type() ==
             jit::ExitFrameType::DirectWasmJitCall);
#endif
}

void WasmFrameIter::popFrame() {
  uint8_t* returnAddress = fp_->returnAddress();
  code_ = LookupCode(returnAddress, &codeRange_);
#ifdef ENABLE_WASM_JSPI
  stackSwitched_ = false;
#endif

  if (!code_) {
    // This is a direct call from the jit into the wasm function's body. The
    // call stack resembles this at this point:
    //
    // |---------------------|
    // |      JIT FRAME      |
    // | JIT FAKE EXIT FRAME | <-- fp_->callerFP_
    // |      WASM FRAME     | <-- fp_
    // |---------------------|
    //
    // fp_->callerFP_ points to the fake exit frame set up by the jit caller,
    // and the return-address-to-fp is in JIT code, thus doesn't belong to any
    // wasm instance's code (in particular, there's no associated CodeRange).
    // Mark the frame as such.
    AssertDirectJitCall(fp_->jitEntryCaller());

    unwoundCallerFP_ = fp_->jitEntryCaller();
    unwoundJitFrameType_.emplace(FrameType::Exit);

    if (unwind_ == Unwind::True) {
      activation_->setJSExitFP(unwoundCallerFP());
      unwoundAddressOfReturnAddress_ = fp_->addressOfReturnAddress();
    }

    fp_ = nullptr;
    code_ = nullptr;
    codeRange_ = nullptr;

    MOZ_ASSERT(done());
    return;
  }

  MOZ_ASSERT(codeRange_);

  Frame* prevFP = fp_;
  fp_ = fp_->wasmCaller();
  resumePCinCurrentFrame_ = returnAddress;

  if (codeRange_->isInterpEntry()) {
    // Interpreter entry has a simple frame, record FP from it.
    unwoundCallerFP_ = reinterpret_cast<uint8_t*>(fp_);

    fp_ = nullptr;
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
    unwoundCallerFP_ = reinterpret_cast<uint8_t*>(fp_);
    unwoundJitFrameType_.emplace(FrameType::JSJitToWasm);

    fp_ = nullptr;
    code_ = nullptr;
    codeRange_ = nullptr;

    if (unwind_ == Unwind::True) {
      activation_->setJSExitFP(unwoundCallerFP());
      unwoundAddressOfReturnAddress_ = prevFP->addressOfReturnAddress();
    }

    MOZ_ASSERT(done());
    return;
  }

  MOZ_ASSERT(codeRange_->kind() == CodeRange::Function);

  const CallSite* callsite = code_->lookupCallSite(returnAddress);
  MOZ_ASSERT(callsite);

  if (callsite->mightBeCrossInstance()) {
    instance_ = ExtractCallerInstanceFromFrameWithInstances(prevFP);
  }

#ifdef ENABLE_WASM_JSPI
  stackSwitched_ = callsite->isStackSwitch();
#endif

  MOZ_ASSERT(code_ == &instance()->code());
  lineOrBytecode_ = callsite->lineOrBytecode();
  failedUnwindSignatureMismatch_ = false;

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
    return cx->names().empty_;
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

unsigned WasmFrameIter::computeLine(
    JS::TaggedColumnNumberOneOrigin* column) const {
  if (instance()->isAsmJS()) {
    if (column) {
      *column =
          JS::TaggedColumnNumberOneOrigin(JS::LimitedColumnNumberOneOrigin(
              JS::WasmFunctionIndex::DefaultBinarySourceColumnNumberOneOrigin));
    }
    return lineOrBytecode_;
  }

  MOZ_ASSERT(!(codeRange_->funcIndex() &
               JS::TaggedColumnNumberOneOrigin::WasmFunctionTag));
  if (column) {
    *column = JS::TaggedColumnNumberOneOrigin(
        JS::WasmFunctionIndex(codeRange_->funcIndex()));
  }
  return lineOrBytecode_;
}

void** WasmFrameIter::unwoundAddressOfReturnAddress() const {
  MOZ_ASSERT(done());
  MOZ_ASSERT(unwind_ == Unwind::True);
  MOZ_ASSERT(unwoundAddressOfReturnAddress_);
  return unwoundAddressOfReturnAddress_;
}

bool WasmFrameIter::debugEnabled() const {
  MOZ_ASSERT(!done());

  // Metadata::debugEnabled is only set if debugging is actually enabled (both
  // requested, and available via baseline compilation), and Tier::Debug code
  // will be available.
  if (!code_->metadata().debugEnabled) {
    return false;
  }

  // Debug information is not available in prologue when the iterator is
  // failing to unwind invalid signature trap.
  if (failedUnwindSignatureMismatch_) {
    return false;
  }

  // Only non-imported functions can have debug frames.
  if (codeRange_->funcIndex() <
      code_->metadata(Tier::Debug).funcImports.length()) {
    return false;
  }

#ifdef ENABLE_WASM_TAIL_CALLS
  // Debug frame is not present at the return stub.
  const CallSite* site = code_->lookupCallSite((void*)resumePCinCurrentFrame_);
  if (site && site->kind() == CallSite::ReturnStub) {
    return false;
  }
#endif

  return true;
}

DebugFrame* WasmFrameIter::debugFrame() const {
  MOZ_ASSERT(!done());
  return DebugFrame::from(fp_);
}

bool WasmFrameIter::hasUnwoundJitFrame() const {
  return unwoundCallerFP_ && unwoundJitFrameType_.isSome();
}

jit::FrameType WasmFrameIter::unwoundJitFrameType() const {
  MOZ_ASSERT(unwoundCallerFP_);
  MOZ_ASSERT(unwoundJitFrameType_.isSome());
  return *unwoundJitFrameType_;
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
static const unsigned PoppedFPJitEntry = 0;
#elif defined(JS_CODEGEN_X86)
static const unsigned PushedRetAddr = 0;
static const unsigned PushedFP = 1;
static const unsigned SetFP = 3;
static const unsigned PoppedFP = 0;
static const unsigned PoppedFPJitEntry = 0;
#elif defined(JS_CODEGEN_ARM)
static const unsigned BeforePushRetAddr = 0;
static const unsigned PushedRetAddr = 4;
static const unsigned PushedFP = 8;
static const unsigned SetFP = 12;
static const unsigned PoppedFP = 0;
static const unsigned PoppedFPJitEntry = 0;
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
static const unsigned PoppedFPJitEntry = 8;
static_assert(BeforePushRetAddr == 0, "Required by StartUnwinding");
static_assert(PushedFP > PushedRetAddr, "Required by StartUnwinding");
#elif defined(JS_CODEGEN_MIPS64)
static const unsigned PushedRetAddr = 8;
static const unsigned PushedFP = 16;
static const unsigned SetFP = 20;
static const unsigned PoppedFP = 4;
static const unsigned PoppedFPJitEntry = 0;
#elif defined(JS_CODEGEN_LOONG64)
static const unsigned PushedRetAddr = 8;
static const unsigned PushedFP = 16;
static const unsigned SetFP = 20;
static const unsigned PoppedFP = 4;
static const unsigned PoppedFPJitEntry = 0;
#elif defined(JS_CODEGEN_RISCV64)
static const unsigned PushedRetAddr = 8;
static const unsigned PushedFP = 16;
static const unsigned SetFP = 20;
static const unsigned PoppedFP = 4;
static const unsigned PoppedFPJitEntry = 0;
#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)
// Synthetic values to satisfy asserts and avoid compiler warnings.
static const unsigned PushedRetAddr = 0;
static const unsigned PushedFP = 1;
static const unsigned SetFP = 2;
static const unsigned PoppedFP = 3;
static const unsigned PoppedFPJitEntry = 4;
#else
#  error "Unknown architecture!"
#endif

static void LoadActivation(MacroAssembler& masm, const Register& dest) {
  // WasmCall pushes a JitActivation.
  masm.loadPtr(Address(InstanceReg, wasm::Instance::offsetOfCx()), dest);
  masm.loadPtr(Address(dest, JSContext::offsetOfActivation()), dest);
}

void wasm::SetExitFP(MacroAssembler& masm, ExitReason reason,
                     Register scratch) {
  MOZ_ASSERT(!reason.isNone());

  LoadActivation(masm, scratch);

  masm.store32(
      Imm32(reason.encode()),
      Address(scratch, JitActivation::offsetOfEncodedWasmExitReason()));

  masm.orPtr(Imm32(ExitFPTag), FramePointer);
  masm.storePtr(FramePointer,
                Address(scratch, JitActivation::offsetOfPackedExitFP()));
  masm.andPtr(Imm32(int32_t(~ExitFPTag)), FramePointer);
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
  AutoCreatedBy acb(masm, "GenerateCallablePrologue");
  masm.setFramePushed(0);

  // ProfilingFrameIterator needs to know the offsets of several key
  // instructions from entry. To save space, we make these offsets static
  // constants and assert that they match the actual codegen below. On ARM,
  // this requires AutoForbidPoolsAndNops to prevent a constant pool from being
  // randomly inserted between two instructions.

#if defined(JS_CODEGEN_MIPS64)
  {
    *entry = masm.currentOffset();

    masm.ma_push(ra);
    MOZ_ASSERT_IF(!masm.oom(), PushedRetAddr == masm.currentOffset() - *entry);
    masm.ma_push(FramePointer);
    MOZ_ASSERT_IF(!masm.oom(), PushedFP == masm.currentOffset() - *entry);
    masm.moveStackPtrTo(FramePointer);
    MOZ_ASSERT_IF(!masm.oom(), SetFP == masm.currentOffset() - *entry);
  }
#elif defined(JS_CODEGEN_LOONG64)
  {
    *entry = masm.currentOffset();

    masm.ma_push(ra);
    MOZ_ASSERT_IF(!masm.oom(), PushedRetAddr == masm.currentOffset() - *entry);
    masm.ma_push(FramePointer);
    MOZ_ASSERT_IF(!masm.oom(), PushedFP == masm.currentOffset() - *entry);
    masm.moveStackPtrTo(FramePointer);
    MOZ_ASSERT_IF(!masm.oom(), SetFP == masm.currentOffset() - *entry);
  }
#elif defined(JS_CODEGEN_RISCV64)
  {
    *entry = masm.currentOffset();
    BlockTrampolinePoolScope block_trampoline_pool(&masm, 5);
    masm.ma_push(ra);
    MOZ_ASSERT_IF(!masm.oom(), PushedRetAddr == masm.currentOffset() - *entry);
    masm.ma_push(FramePointer);
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
  AutoCreatedBy acb(masm, "GenerateCallableEpilogue");

  if (framePushed) {
    masm.freeStack(framePushed);
  }

  if (!reason.isNone()) {
    ClearExitFP(masm, ABINonArgReturnVolatileReg);
  }

  DebugOnly<uint32_t> poppedFP{};

#if defined(JS_CODEGEN_MIPS64)

  masm.loadPtr(Address(StackPointer, Frame::callerFPOffset()), FramePointer);
  poppedFP = masm.currentOffset();
  masm.loadPtr(Address(StackPointer, Frame::returnAddressOffset()), ra);

  *ret = masm.currentOffset();
  masm.as_jr(ra);
  masm.addToStackPtr(Imm32(sizeof(Frame)));

#elif defined(JS_CODEGEN_LOONG64)

  masm.loadPtr(Address(StackPointer, Frame::callerFPOffset()), FramePointer);
  poppedFP = masm.currentOffset();
  masm.loadPtr(Address(StackPointer, Frame::returnAddressOffset()), ra);

  *ret = masm.currentOffset();
  masm.addToStackPtr(Imm32(sizeof(Frame)));
  masm.as_jirl(zero, ra, BOffImm16(0));

#elif defined(JS_CODEGEN_RISCV64)
  {
    BlockTrampolinePoolScope block_trampoline_pool(&masm, 20);
    masm.loadPtr(Address(StackPointer, Frame::callerFPOffset()), FramePointer);
    poppedFP = masm.currentOffset();
    masm.loadPtr(Address(StackPointer, Frame::returnAddressOffset()), ra);

    *ret = masm.currentOffset();
    masm.addToStackPtr(Imm32(sizeof(Frame)));
    masm.jalr(zero, ra, 0);
    masm.nop();
  }
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
                                    const CallIndirectId& callIndirectId,
                                    const Maybe<uint32_t>& tier1FuncIndex,
                                    FuncOffsets* offsets) {
  AutoCreatedBy acb(masm, "wasm::GenerateFunctionPrologue");

  // We are going to generate this code layout:
  // ---------------------------------------------
  // checked call entry:    callable prologue
  //                        check signature
  //                        jump functionBody ──┐
  // unchecked call entry:  callable prologue   │
  //                        functionBody  <─────┘
  // -----------------------------------------------
  // checked call entry - used for call_indirect when we have to check the
  // signature.
  //
  // unchecked call entry - used for regular direct same-instance calls.

  // The checked call entry is a call target, so must have CodeAlignment.
  // Its offset is normally zero.
  static_assert(WasmCheckedCallEntryOffset % CodeAlignment == 0,
                "code aligned");

  // Flush pending pools so they do not get dumped between the 'begin' and
  // 'uncheckedCallEntry' offsets since the difference must be less than
  // UINT8_MAX to be stored in CodeRange::funcbeginToUncheckedCallEntry_.
  // (Pending pools can be large.)
  masm.flushBuffer();
  masm.haltingAlign(CodeAlignment);

  Label functionBody;

  offsets->begin = masm.currentOffset();

  // Only first-class functions (those that can be referenced in a table) need
  // the checked call prologue w/ signature check. It is impossible to perform
  // a checked call otherwise.
  //
  // asm.js function tables are homogeneous and don't need a signature check.
  // However, they can be put in tables which expect a checked call entry point,
  // so we generate a no-op entry point for consistency. If asm.js performance
  // was important we could refine this in the future.
  if (callIndirectId.kind() != CallIndirectIdKind::None) {
    // Generate checked call entry. The BytecodeOffset of the trap is fixed up
    // to be the bytecode offset of the callsite by
    // JitActivation::startWasmTrap.
    MOZ_ASSERT_IF(!masm.oom(), masm.currentOffset() - offsets->begin ==
                                   WasmCheckedCallEntryOffset);
    uint32_t dummy;
    GenerateCallablePrologue(masm, &dummy);

    switch (callIndirectId.kind()) {
      case CallIndirectIdKind::Global: {
        Label fail;
        Register scratch1 = WasmTableCallScratchReg0;
        Register scratch2 = WasmTableCallScratchReg1;

        // Check if this function's type is exactly the expected function type
        masm.loadPtr(
            Address(InstanceReg,
                    Instance::offsetInData(
                        callIndirectId.instanceDataOffset() +
                        offsetof(wasm::TypeDefInstanceData, superTypeVector))),
            scratch1);
        masm.branchPtr(Assembler::Condition::Equal, WasmTableCallSigReg,
                       scratch1, &functionBody);

        // Otherwise, we need to see if this function's type is a sub type of
        // the expected function type. This requires us to check if the
        // expected's type is in the super type vector of this function's type.
        //
        // We can skip this if our function type has no super types.
        if (callIndirectId.hasSuperType()) {
          // Check if the expected function type was an immediate, not a
          // type definition. Because we only allow the immediate form for
          // final types without super types, this implies that we have a
          // signature mismatch.
          masm.branchTestPtr(Assembler::Condition::NonZero, WasmTableCallSigReg,
                             Imm32(FuncType::ImmediateBit), &fail);

          // Load the subtyping depth of the expected function type. Re-use the
          // index register, as it's no longer needed.
          Register subTypingDepth = WasmTableCallIndexReg;
          masm.load32(
              Address(WasmTableCallSigReg,
                      int32_t(SuperTypeVector::offsetOfSubTypingDepth())),
              subTypingDepth);

          // Perform the check
          masm.branchWasmSTVIsSubtypeDynamicDepth(scratch1, WasmTableCallSigReg,
                                                  subTypingDepth, scratch2,
                                                  &functionBody, true);
        }

        masm.bind(&fail);
        masm.wasmTrap(Trap::IndirectCallBadSig, BytecodeOffset(0));
        break;
      }
      case CallIndirectIdKind::Immediate: {
        masm.branch32(Assembler::Condition::Equal, WasmTableCallSigReg,
                      Imm32(callIndirectId.immediate()), &functionBody);
        masm.wasmTrap(Trap::IndirectCallBadSig, BytecodeOffset(0));
        break;
      }
      case CallIndirectIdKind::AsmJS:
        masm.jump(&functionBody);
        break;
      case CallIndirectIdKind::None:
        break;
    }

    // The preceding code may have generated a small constant pool to support
    // the comparison in the signature check.  But if we flush the pool here we
    // will also force the creation of an unused branch veneer in the pool for
    // the jump to functionBody from the signature check on some platforms, thus
    // needlessly inflating the size of the prologue.
    //
    // On no supported platform that uses a pool (arm, arm64) is there any risk
    // at present of that branch or other elements in the pool going out of
    // range while we're generating the following padding and prologue,
    // therefore no pool elements will be emitted in the prologue, therefore it
    // is safe not to flush here.
    //
    // We assert that this holds at runtime by comparing the expected entry
    // offset to the recorded ditto; if they are not the same then
    // GenerateCallablePrologue flushed a pool before the prologue code,
    // contrary to assumption.

    masm.nopAlign(CodeAlignment);
  }

  // Generate unchecked call entry:
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
    masm.loadPtr(Address(InstanceReg, Instance::offsetOfJumpTable()), scratch);
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
                     Imm32(ExitFPTag), &ok);
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

void wasm::GenerateJitEntryPrologue(MacroAssembler& masm,
                                    CallableOffsets* offsets) {
  masm.haltingAlign(CodeAlignment);

  {
    // Push the return address.
#if defined(JS_CODEGEN_ARM)
    AutoForbidPoolsAndNops afp(&masm,
                               /* number of instructions in scope = */ 3);
    offsets->begin = masm.currentOffset();
    static_assert(BeforePushRetAddr == 0);
    masm.push(lr);
#elif defined(JS_CODEGEN_MIPS64)
    offsets->begin = masm.currentOffset();
    masm.push(ra);
#elif defined(JS_CODEGEN_LOONG64)
    offsets->begin = masm.currentOffset();
    masm.push(ra);
#elif defined(JS_CODEGEN_RISCV64)
    BlockTrampolinePoolScope block_trampoline_pool(&masm, 10);
    offsets->begin = masm.currentOffset();
    masm.push(ra);
#elif defined(JS_CODEGEN_ARM64)
    {
      AutoForbidPoolsAndNops afp(&masm,
                                 /* number of instructions in scope = */ 4);
      offsets->begin = masm.currentOffset();
      static_assert(BeforePushRetAddr == 0);
      // Subtract from SP first as SP must be aligned before offsetting.
      masm.Sub(sp, sp, 16);
      static_assert(JitFrameLayout::offsetOfReturnAddress() == 8);
      masm.Str(ARMRegister(lr, 64), MemOperand(sp, 8));
    }
#else
    // The x86/x64 call instruction pushes the return address.
    offsets->begin = masm.currentOffset();
#endif
    MOZ_ASSERT_IF(!masm.oom(),
                  PushedRetAddr == masm.currentOffset() - offsets->begin);
    // Save jit frame pointer, so unwinding from wasm to jit frames is trivial.
#if defined(JS_CODEGEN_ARM64)
    static_assert(JitFrameLayout::offsetOfCallerFramePtr() == 0);
    masm.Str(ARMRegister(FramePointer, 64), MemOperand(sp, 0));
#else
    masm.Push(FramePointer);
#endif
    MOZ_ASSERT_IF(!masm.oom(),
                  PushedFP == masm.currentOffset() - offsets->begin);

    masm.moveStackPtrTo(FramePointer);
    MOZ_ASSERT_IF(!masm.oom(), SetFP == masm.currentOffset() - offsets->begin);
  }

  masm.setFramePushed(0);
}

void wasm::GenerateJitEntryEpilogue(MacroAssembler& masm,
                                    CallableOffsets* offsets) {
  DebugOnly<uint32_t> poppedFP{};
#ifdef JS_CODEGEN_ARM64
  {
    RegisterOrSP sp = masm.getStackPointer();
    AutoForbidPoolsAndNops afp(&masm,
                               /* number of instructions in scope = */ 5);
    masm.loadPtr(Address(sp, 8), lr);
    masm.loadPtr(Address(sp, 0), FramePointer);
    poppedFP = masm.currentOffset();

    masm.addToStackPtr(Imm32(2 * sizeof(void*)));
    // Copy SP into PSP to enforce return-point invariants (SP == PSP).
    // `addToStackPtr` won't sync them because SP is the active pointer here.
    // For the same reason, we can't use initPseudoStackPtr to do the sync, so
    // we have to do it "by hand".  Omitting this causes many tests to segfault.
    masm.moveStackPtrTo(PseudoStackPointer);

    offsets->ret = masm.currentOffset();
    masm.Ret(ARMRegister(lr, 64));
    masm.setFramePushed(0);
  }
#else
  // Forbid pools for the same reason as described in GenerateCallablePrologue.
#  if defined(JS_CODEGEN_ARM)
  AutoForbidPoolsAndNops afp(&masm, /* number of instructions in scope = */ 2);
#  endif

  masm.pop(FramePointer);
  poppedFP = masm.currentOffset();

  offsets->ret = masm.currentOffset();
  masm.ret();
#endif
  MOZ_ASSERT_IF(!masm.oom(), PoppedFPJitEntry == offsets->ret - poppedFP);
}

/*****************************************************************************/
// ProfilingFrameIterator

ProfilingFrameIterator::ProfilingFrameIterator()
    : code_(nullptr),
      codeRange_(nullptr),
      callerFP_(nullptr),
      callerPC_(nullptr),
      stackAddress_(nullptr),
      unwoundJitCallerFP_(nullptr),
      exitReason_(ExitReason::Fixed::None) {
  MOZ_ASSERT(done());
}

ProfilingFrameIterator::ProfilingFrameIterator(const JitActivation& activation)
    : code_(nullptr),
      codeRange_(nullptr),
      callerFP_(nullptr),
      callerPC_(nullptr),
      stackAddress_(nullptr),
      unwoundJitCallerFP_(nullptr),
      exitReason_(activation.wasmExitReason()) {
  initFromExitFP(activation.wasmExitFP());
}

ProfilingFrameIterator::ProfilingFrameIterator(const Frame* fp)
    : code_(nullptr),
      codeRange_(nullptr),
      callerFP_(nullptr),
      callerPC_(nullptr),
      stackAddress_(nullptr),
      unwoundJitCallerFP_(nullptr),
      exitReason_(ExitReason::Fixed::ImportJit) {
  MOZ_ASSERT(fp);
  initFromExitFP(fp);
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
    // callerFP is the value of the frame pointer register when we were called
    // from C++.
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
  endStackAddress_ = stackAddress_;
  code_ = LookupCode(fp->returnAddress(), &codeRange_);

  if (!code_) {
    // This is a direct call from the JIT, the caller FP is pointing to the JIT
    // caller's frame.
    AssertDirectJitCall(fp->jitEntryCaller());

    unwoundJitCallerFP_ = fp->jitEntryCaller();
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
      break;
    case CodeRange::JitEntry:
      callerPC_ = nullptr;
      callerFP_ = fp->rawCaller();
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

static bool IsSignatureCheckFail(uint32_t offsetInCode,
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

static bool CanUnwindSignatureCheck(uint8_t* fp) {
  const auto* frame = Frame::fromUntaggedWasmExitFP(fp);
  uint8_t* const pc = frame->returnAddress();

  const CodeRange* codeRange;
  const Code* code = LookupCode(pc, &codeRange);
  // If a JIT call or JIT/interpreter entry was found,
  // unwinding is not possible.
  return code && !codeRange->isEntry();
}

static bool GetUnwindInfo(const CodeSegment* codeSegment,
                          const CodeRange* codeRange, uint8_t* pc,
                          const CodeRangeUnwindInfo** info) {
  if (!codeSegment->isModule()) {
    return false;
  }
  if (!codeRange->isFunction() || !codeRange->funcHasUnwindInfo()) {
    return false;
  }

  const ModuleSegment* segment = codeSegment->asModule();
  *info = segment->code().lookupUnwindInfo(pc);
  return *info;
}

const Instance* js::wasm::GetNearestEffectiveInstance(const Frame* fp) {
  while (true) {
    uint8_t* returnAddress = fp->returnAddress();
    const CodeRange* codeRange = nullptr;
    const Code* code = LookupCode(returnAddress, &codeRange);

    if (!code) {
      // It is a direct call from JIT.
      AssertDirectJitCall(fp->jitEntryCaller());
      return ExtractCalleeInstanceFromFrameWithInstances(fp);
    }

    MOZ_ASSERT(codeRange);

    if (codeRange->isEntry()) {
      return ExtractCalleeInstanceFromFrameWithInstances(fp);
    }

    MOZ_ASSERT(codeRange->kind() == CodeRange::Function);
    MOZ_ASSERT(code);
    const CallSite* callsite = code->lookupCallSite(returnAddress);
    if (callsite->mightBeCrossInstance()) {
      return ExtractCalleeInstanceFromFrameWithInstances(fp);
    }

    fp = fp->wasmCaller();
  }
}

Instance* js::wasm::GetNearestEffectiveInstance(Frame* fp) {
  return const_cast<Instance*>(
      GetNearestEffectiveInstance(const_cast<const Frame*>(fp)));
}

bool js::wasm::StartUnwinding(const RegisterState& registers,
                              UnwindState* unwindState, bool* unwoundCaller) {
  // Shorthands.
  uint8_t* const pc = (uint8_t*)registers.pc;
  void** const sp = (void**)registers.sp;

  // The frame pointer might be:
  // - in the process of tagging/untagging when calling into C++ code (this
  //   happens in wasm::SetExitFP); make sure it's untagged.
  // - unreliable if it's not been set yet, in prologues.
  uint8_t* fp = Frame::isExitFP(registers.fp)
                    ? Frame::untagExitFP(registers.fp)
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
#if defined(JS_CODEGEN_MIPS64)
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
#elif defined(JS_CODEGEN_LOONG64)
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
        // On LoongArch we rely on register state instead of state saved on
        // stack until the wasm::Frame is completely built.
        // On entry the return address is in ra (registers.lr) and
        // fp holds the caller's fp.
        fixedPC = (uint8_t*)registers.lr;
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
      } else
#elif defined(JS_CODEGEN_RISCV64)
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
        // On Riscv64 we rely on register state instead of state saved on
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
        MOZ_ASSERT(frame->rawCaller() == fp);
        fixedPC = frame->returnAddress();
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
#if defined(JS_CODEGEN_MIPS64)
      } else if (offsetInCode >= codeRange->ret() - PoppedFP &&
                 offsetInCode <= codeRange->ret()) {
        // The fixedFP field of the Frame has been loaded into fp.
        // The ra and instance might also be loaded, but the Frame structure is
        // still on stack, so we can acess the ra form there.
        MOZ_ASSERT(*sp == fp);
        fixedPC = Frame::fromUntaggedWasmExitFP(sp)->returnAddress();
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
#elif defined(JS_CODEGEN_LOONG64)
      } else if (offsetInCode >= codeRange->ret() - PoppedFP &&
                 offsetInCode <= codeRange->ret()) {
        // The fixedFP field of the Frame has been loaded into fp.
        // The ra might also be loaded, but the Frame structure is still on
        // stack, so we can acess the ra from there.
        MOZ_ASSERT(*sp == fp);
        fixedPC = Frame::fromUntaggedWasmExitFP(sp)->returnAddress();
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
#elif defined(JS_CODEGEN_RISCV64)
      } else if (offsetInCode >= codeRange->ret() - PoppedFP &&
                 offsetInCode <= codeRange->ret()) {
        // The fixedFP field of the Frame has been loaded into fp.
        // The ra might also be loaded, but the Frame structure is still on
        // stack, so we can acess the ra from there.
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
        // Both the instance and fixedFP fields have been popped and fp now
        // points to the caller's frame.
        fixedPC = sp[0];
        fixedFP = fp;
        AssertMatchesCallSite(fixedPC, fixedFP);
#endif
      } else {
        if (IsSignatureCheckFail(offsetInCode, codeRange) &&
            CanUnwindSignatureCheck(fp)) {
          // Frame has been pushed and FP has been set.
          const auto* frame = Frame::fromUntaggedWasmExitFP(fp);
          fixedFP = frame->rawCaller();
          fixedPC = frame->returnAddress();
          AssertMatchesCallSite(fixedPC, fixedFP);
          break;
        }

        const CodeRangeUnwindInfo* unwindInfo;
        if (codeSegment &&
            GetUnwindInfo(codeSegment, codeRange, pc, &unwindInfo)) {
          switch (unwindInfo->unwindHow()) {
            case CodeRangeUnwindInfo::RestoreFpRa:
              fixedPC = (uint8_t*)registers.tempRA;
              fixedFP = (uint8_t*)registers.tempFP;
              break;
            case CodeRangeUnwindInfo::RestoreFp:
              fixedPC = sp[0];
              fixedFP = (uint8_t*)registers.tempFP;
              break;
            case CodeRangeUnwindInfo::UseFpLr:
              fixedPC = (uint8_t*)registers.lr;
              fixedFP = fp;
              break;
            case CodeRangeUnwindInfo::UseFp:
              fixedPC = sp[0];
              fixedFP = fp;
              break;
            default:
              MOZ_CRASH();
          }
          MOZ_ASSERT(fixedPC && fixedFP);
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
      if (offsetFromEntry < PushedFP) {
        // We haven't pushed the jit caller's frame pointer yet, thus the jit
        // frame is incomplete. During profiling frame iteration, it means that
        // the jit profiling frame iterator won't be able to unwind this frame;
        // drop it.
        return false;
      }
      if (offsetInCode >= codeRange->ret() - PoppedFPJitEntry &&
          offsetInCode <= codeRange->ret()) {
        // We've popped FP but still have to return. Similar to the
        // |offsetFromEntry < PushedFP| case above, the JIT frame is now
        // incomplete and we can't unwind.
        return false;
      }
      // Set fixedFP to the address of the JitFrameLayout on the stack.
      if (offsetFromEntry < SetFP) {
        fixedFP = reinterpret_cast<uint8_t*>(sp);
      } else {
        fixedFP = fp;
      }
      fixedPC = nullptr;
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
      unwoundJitCallerFP_(nullptr),
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
  } else {
    callerFP_ = Frame::fromUntaggedWasmExitFP(unwindState.fp)->rawCaller();
    callerPC_ = Frame::fromUntaggedWasmExitFP(unwindState.fp)->returnAddress();
  }

  code_ = unwindState.code;
  codeRange_ = unwindState.codeRange;
  stackAddress_ = state.sp;
  endStackAddress_ = state.sp;
  MOZ_ASSERT(!done());
}

void ProfilingFrameIterator::operator++() {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(!unwoundJitCallerFP_);

  if (!exitReason_.isNone()) {
    exitReason_ = ExitReason::None();
    MOZ_ASSERT(codeRange_);
    MOZ_ASSERT(!done());
    return;
  }

  if (codeRange_->isInterpEntry()) {
    codeRange_ = nullptr;
    MOZ_ASSERT(done());
    return;
  }

  if (codeRange_->isJitEntry()) {
    MOZ_ASSERT(callerFP_);
    unwoundJitCallerFP_ = callerFP_;
    callerPC_ = nullptr;
    callerFP_ = nullptr;
    codeRange_ = nullptr;
    MOZ_ASSERT(done());
    return;
  }

  MOZ_RELEASE_ASSERT(callerPC_);

  code_ = LookupCode(callerPC_, &codeRange_);

  if (!code_) {
    // The parent frame is an inlined wasm call, callerFP_ points to the fake
    // exit frame.
    MOZ_ASSERT(!codeRange_);
    AssertDirectJitCall(callerFP_);
    unwoundJitCallerFP_ = callerFP_;
    MOZ_ASSERT(done());
    return;
  }

  MOZ_ASSERT(codeRange_);

  if (codeRange_->isInterpEntry()) {
    callerPC_ = nullptr;
    callerFP_ = nullptr;
    MOZ_ASSERT(!done());
    return;
  }

  if (codeRange_->isJitEntry()) {
    MOZ_ASSERT(!done());
    return;
  }

  MOZ_ASSERT(code_ == &GetNearestEffectiveInstance(
                           Frame::fromUntaggedWasmExitFP(callerFP_))
                           ->code());

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
      return "call to native i64.trunc_f64_u (in wasm)";
    case SymbolicAddress::TruncateDoubleToInt64:
      return "call to native i64.trunc_f64_s (in wasm)";
    case SymbolicAddress::SaturatingTruncateDoubleToUint64:
      return "call to native i64.trunc_sat_f64_u (in wasm)";
    case SymbolicAddress::SaturatingTruncateDoubleToInt64:
      return "call to native i64.trunc_sat_f64_s (in wasm)";
    case SymbolicAddress::Uint64ToDouble:
      return "call to native f64.convert_i64_u (in wasm)";
    case SymbolicAddress::Uint64ToFloat32:
      return "call to native f32.convert_i64_u (in wasm)";
    case SymbolicAddress::Int64ToDouble:
      return "call to native f64.convert_i64_s (in wasm)";
    case SymbolicAddress::Int64ToFloat32:
      return "call to native f32.convert_i64_s (in wasm)";
#if defined(JS_CODEGEN_ARM)
    case SymbolicAddress::aeabi_idivmod:
      return "call to native i32.div_s (in wasm)";
    case SymbolicAddress::aeabi_uidivmod:
      return "call to native i32.div_u (in wasm)";
#endif
    case SymbolicAddress::AllocateBigInt:
      return "call to native newCell<BigInt, NoGC> (in wasm)";
    case SymbolicAddress::ModD:
      return "call to asm.js native f64 % (mod)";
    case SymbolicAddress::SinNativeD:
      return "call to asm.js native f64 Math.sin";
    case SymbolicAddress::SinFdlibmD:
      return "call to asm.js fdlibm f64 Math.sin";
    case SymbolicAddress::CosNativeD:
      return "call to asm.js native f64 Math.cos";
    case SymbolicAddress::CosFdlibmD:
      return "call to asm.js fdlibm f64 Math.cos";
    case SymbolicAddress::TanNativeD:
      return "call to asm.js native f64 Math.tan";
    case SymbolicAddress::TanFdlibmD:
      return "call to asm.js fdlibm f64 Math.tan";
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
    case SymbolicAddress::MemoryGrowM32:
      return "call to native memory.grow m32 (in wasm)";
    case SymbolicAddress::MemoryGrowM64:
      return "call to native memory.grow m64 (in wasm)";
    case SymbolicAddress::MemorySizeM32:
      return "call to native memory.size m32 (in wasm)";
    case SymbolicAddress::MemorySizeM64:
      return "call to native memory.size m64 (in wasm)";
    case SymbolicAddress::WaitI32M32:
      return "call to native i32.wait m32 (in wasm)";
    case SymbolicAddress::WaitI32M64:
      return "call to native i32.wait m64 (in wasm)";
    case SymbolicAddress::WaitI64M32:
      return "call to native i64.wait m32 (in wasm)";
    case SymbolicAddress::WaitI64M64:
      return "call to native i64.wait m64 (in wasm)";
    case SymbolicAddress::WakeM32:
      return "call to native wake m32 (in wasm)";
    case SymbolicAddress::WakeM64:
      return "call to native wake m64 (in wasm)";
    case SymbolicAddress::CoerceInPlace_JitEntry:
      return "out-of-line coercion for jit entry arguments (in wasm)";
    case SymbolicAddress::ReportV128JSCall:
      return "jit call to v128 wasm function";
    case SymbolicAddress::MemCopyM32:
    case SymbolicAddress::MemCopySharedM32:
      return "call to native memory.copy m32 function";
    case SymbolicAddress::MemCopyM64:
    case SymbolicAddress::MemCopySharedM64:
      return "call to native memory.copy m64 function";
    case SymbolicAddress::MemCopyAny:
      return "call to native memory.copy any function";
    case SymbolicAddress::DataDrop:
      return "call to native data.drop function";
    case SymbolicAddress::MemFillM32:
    case SymbolicAddress::MemFillSharedM32:
      return "call to native memory.fill m32 function";
    case SymbolicAddress::MemFillM64:
    case SymbolicAddress::MemFillSharedM64:
      return "call to native memory.fill m64 function";
    case SymbolicAddress::MemInitM32:
      return "call to native memory.init m32 function";
    case SymbolicAddress::MemInitM64:
      return "call to native memory.init m64 function";
    case SymbolicAddress::TableCopy:
      return "call to native table.copy function";
    case SymbolicAddress::TableFill:
      return "call to native table.fill function";
    case SymbolicAddress::MemDiscardM32:
    case SymbolicAddress::MemDiscardSharedM32:
      return "call to native memory.discard m32 function";
    case SymbolicAddress::MemDiscardM64:
    case SymbolicAddress::MemDiscardSharedM64:
      return "call to native memory.discard m64 function";
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
    case SymbolicAddress::PostBarrier:
    case SymbolicAddress::PostBarrierPrecise:
    case SymbolicAddress::PostBarrierPreciseWithOffset:
      return "call to native GC postbarrier (in wasm)";
    case SymbolicAddress::ExceptionNew:
      return "call to native exception new (in wasm)";
    case SymbolicAddress::ThrowException:
      return "call to native throw exception (in wasm)";
    case SymbolicAddress::StructNewIL_true:
    case SymbolicAddress::StructNewIL_false:
    case SymbolicAddress::StructNewOOL_true:
    case SymbolicAddress::StructNewOOL_false:
      return "call to native struct.new (in wasm)";
    case SymbolicAddress::ArrayNew_true:
    case SymbolicAddress::ArrayNew_false:
      return "call to native array.new (in wasm)";
    case SymbolicAddress::ArrayNewData:
      return "call to native array.new_data function";
    case SymbolicAddress::ArrayNewElem:
      return "call to native array.new_elem function";
    case SymbolicAddress::ArrayInitData:
      return "call to native array.init_data function";
    case SymbolicAddress::ArrayInitElem:
      return "call to native array.init_elem function";
    case SymbolicAddress::ArrayCopy:
      return "call to native array.copy function";
    case SymbolicAddress::SlotsToAllocKindBytesTable:
      MOZ_CRASH(
          "symbolic address was not code and should not have appeared here");
#define VISIT_BUILTIN_FUNC(op, export, sa_name, ...) \
  case SymbolicAddress::sa_name:                     \
    return "call to native " #op " builtin (in wasm)";
      FOR_EACH_BUILTIN_MODULE_FUNC(VISIT_BUILTIN_FUNC)
#undef VISIT_BUILTIN_FUNC
#ifdef ENABLE_WASM_JSPI
    case SymbolicAddress::UpdateSuspenderState:
      return "call to native update suspender state util";
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
  }

  switch (codeRange_->kind()) {
    case CodeRange::Function:
      return code_->profilingLabel(codeRange_->funcIndex());
    case CodeRange::InterpEntry:
      return "slow entry trampoline (in wasm)";
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

ProfilingFrameIterator::Category ProfilingFrameIterator::category() const {
  if (!exitReason_.isFixed() || !exitReason_.isNone() ||
      !codeRange_->isFunction()) {
    return Category::Other;
  }

  Tier tier;
  if (!code_->lookupFunctionTier(codeRange_, &tier)) {
    return Category::Other;
  }
  return tier == Tier::Optimized ? Category::Ion : Category::Baseline;
}
