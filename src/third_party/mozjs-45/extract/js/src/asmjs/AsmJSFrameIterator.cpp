/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

#include "asmjs/AsmJSFrameIterator.h"

#include "asmjs/AsmJSModule.h"
#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::DebugOnly;

/*****************************************************************************/
// AsmJSFrameIterator implementation

static void*
ReturnAddressFromFP(void* fp)
{
    return reinterpret_cast<AsmJSFrame*>(fp)->returnAddress;
}

static uint8_t*
CallerFPFromFP(void* fp)
{
    return reinterpret_cast<AsmJSFrame*>(fp)->callerFP;
}

AsmJSFrameIterator::AsmJSFrameIterator(const AsmJSActivation& activation)
  : module_(&activation.module()),
    fp_(activation.fp())
{
    if (!fp_)
        return;
    settle();
}

void
AsmJSFrameIterator::operator++()
{
    MOZ_ASSERT(!done());
    DebugOnly<uint8_t*> oldfp = fp_;
    fp_ += callsite_->stackDepth();
    MOZ_ASSERT_IF(module_->profilingEnabled(), fp_ == CallerFPFromFP(oldfp));
    settle();
}

void
AsmJSFrameIterator::settle()
{
    void* returnAddress = ReturnAddressFromFP(fp_);

    const AsmJSModule::CodeRange* codeRange = module_->lookupCodeRange(returnAddress);
    MOZ_ASSERT(codeRange);
    codeRange_ = codeRange;

    switch (codeRange->kind()) {
      case AsmJSModule::CodeRange::Function:
        callsite_ = module_->lookupCallSite(returnAddress);
        MOZ_ASSERT(callsite_);
        break;
      case AsmJSModule::CodeRange::Entry:
        fp_ = nullptr;
        MOZ_ASSERT(done());
        break;
      case AsmJSModule::CodeRange::JitFFI:
      case AsmJSModule::CodeRange::SlowFFI:
      case AsmJSModule::CodeRange::Interrupt:
      case AsmJSModule::CodeRange::Inline:
      case AsmJSModule::CodeRange::Thunk:
        MOZ_CRASH("Should not encounter an exit during iteration");
    }
}

JSAtom*
AsmJSFrameIterator::functionDisplayAtom() const
{
    MOZ_ASSERT(!done());
    return reinterpret_cast<const AsmJSModule::CodeRange*>(codeRange_)->functionName(*module_);
}

unsigned
AsmJSFrameIterator::computeLine(uint32_t* column) const
{
    MOZ_ASSERT(!done());
    if (column)
        *column = callsite_->column();
    return callsite_->line();
}

/*****************************************************************************/
// Prologue/epilogue code generation

// These constants reflect statically-determined offsets in the profiling
// prologue/epilogue. The offsets are dynamically asserted during code
// generation.
#if defined(JS_CODEGEN_X64)
# if defined(DEBUG)
static const unsigned PushedRetAddr = 0;
static const unsigned PostStorePrePopFP = 0;
# endif
static const unsigned PushedFP = 13;
static const unsigned StoredFP = 20;
#elif defined(JS_CODEGEN_X86)
# if defined(DEBUG)
static const unsigned PushedRetAddr = 0;
static const unsigned PostStorePrePopFP = 0;
# endif
static const unsigned PushedFP = 8;
static const unsigned StoredFP = 11;
#elif defined(JS_CODEGEN_ARM)
static const unsigned PushedRetAddr = 4;
static const unsigned PushedFP = 16;
static const unsigned StoredFP = 20;
static const unsigned PostStorePrePopFP = 4;
#elif defined(JS_CODEGEN_ARM64)
static const unsigned PushedRetAddr = 0;
static const unsigned PushedFP = 0;
static const unsigned StoredFP = 0;
static const unsigned PostStorePrePopFP = 0;
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
static const unsigned PushedRetAddr = 8;
static const unsigned PushedFP = 24;
static const unsigned StoredFP = 28;
static const unsigned PostStorePrePopFP = 4;
#elif defined(JS_CODEGEN_NONE)
# if defined(DEBUG)
static const unsigned PushedRetAddr = 0;
static const unsigned PostStorePrePopFP = 0;
# endif
static const unsigned PushedFP = 1;
static const unsigned StoredFP = 1;
#else
# error "Unknown architecture!"
#endif

static void
PushRetAddr(MacroAssembler& masm)
{
#if defined(JS_CODEGEN_ARM)
    masm.push(lr);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    masm.push(ra);
#else
    // The x86/x64 call instruction pushes the return address.
#endif
}

// Generate a prologue that maintains AsmJSActivation::fp as the virtual frame
// pointer so that AsmJSProfilingFrameIterator can walk the stack at any pc in
// generated code.
static void
GenerateProfilingPrologue(MacroAssembler& masm, unsigned framePushed, ExitReason reason,
                          AsmJSProfilingOffsets* offsets, Label* maybeEntry = nullptr)
{
#if !defined (JS_CODEGEN_ARM)
    Register scratch = ABIArgGenerator::NonArg_VolatileReg;
#else
    // Unfortunately, there are no unused non-arg volatile registers on ARM --
    // the MacroAssembler claims both lr and ip -- so we use the second scratch
    // register (lr) and be very careful not to call any methods that use it.
    Register scratch = lr;
    masm.setSecondScratchReg(InvalidReg);
#endif

    // AsmJSProfilingFrameIterator needs to know the offsets of several key
    // instructions from entry. To save space, we make these offsets static
    // constants and assert that they match the actual codegen below. On ARM,
    // this requires AutoForbidPools to prevent a constant pool from being
    // randomly inserted between two instructions.
    {
#if defined(JS_CODEGEN_ARM)
        AutoForbidPools afp(&masm, /* number of instructions in scope = */ 5);
#endif

        offsets->begin = masm.currentOffset();
        if (maybeEntry)
            masm.bind(maybeEntry);

        PushRetAddr(masm);
        MOZ_ASSERT_IF(!masm.oom(), PushedRetAddr == masm.currentOffset() - offsets->begin);

        masm.loadAsmJSActivation(scratch);
        masm.push(Address(scratch, AsmJSActivation::offsetOfFP()));
        MOZ_ASSERT_IF(!masm.oom(), PushedFP == masm.currentOffset() - offsets->begin);

        masm.storePtr(masm.getStackPointer(), Address(scratch, AsmJSActivation::offsetOfFP()));
        MOZ_ASSERT_IF(!masm.oom(), StoredFP == masm.currentOffset() - offsets->begin);
    }

    if (reason.kind() != ExitReason::None) {
        masm.store32_NoSecondScratch(Imm32(reason.pack()),
                                     Address(scratch, AsmJSActivation::offsetOfPackedExitReason()));
    }

#if defined(JS_CODEGEN_ARM)
    masm.setSecondScratchReg(lr);
#endif

    if (framePushed)
        masm.subFromStackPtr(Imm32(framePushed));
}

// Generate the inverse of GenerateProfilingPrologue.
static void
GenerateProfilingEpilogue(MacroAssembler& masm, unsigned framePushed, ExitReason reason,
                          AsmJSProfilingOffsets* offsets)
{
    Register scratch = ABIArgGenerator::NonReturn_VolatileReg0;
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    Register scratch2 = ABIArgGenerator::NonReturn_VolatileReg1;
#endif

    if (framePushed)
        masm.addToStackPtr(Imm32(framePushed));

    masm.loadAsmJSActivation(scratch);

    if (reason.kind() != ExitReason::None) {
        masm.store32(Imm32(ExitReason::None),
                     Address(scratch, AsmJSActivation::offsetOfPackedExitReason()));
    }

    // AsmJSProfilingFrameIterator assumes fixed offsets of the last few
    // instructions from profilingReturn, so AutoForbidPools to ensure that
    // unintended instructions are not automatically inserted.
    {
#if defined(JS_CODEGEN_ARM)
        AutoForbidPools afp(&masm, /* number of instructions in scope = */ 4);
#endif

        // sp protects the stack from clobber via asynchronous signal handlers
        // and the async interrupt exit. Since activation.fp can be read at any
        // time and still points to the current frame, be careful to only update
        // sp after activation.fp has been repointed to the caller's frame.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        masm.loadPtr(Address(masm.getStackPointer(), 0), scratch2);
        masm.storePtr(scratch2, Address(scratch, AsmJSActivation::offsetOfFP()));
        DebugOnly<uint32_t> prePop = masm.currentOffset();
        masm.addToStackPtr(Imm32(sizeof(void *)));
        MOZ_ASSERT_IF(!masm.oom(), PostStorePrePopFP == masm.currentOffset() - prePop);
#else
        masm.pop(Address(scratch, AsmJSActivation::offsetOfFP()));
        MOZ_ASSERT(PostStorePrePopFP == 0);
#endif

        offsets->profilingReturn = masm.currentOffset();
        masm.ret();
    }
}

// In profiling mode, we need to maintain fp so that we can unwind the stack at
// any pc. In non-profiling mode, the only way to observe AsmJSActivation::fp is
// to call out to C++ so, as an optimization, we don't update fp. To avoid
// recompilation when the profiling mode is toggled, we generate both prologues
// a priori and switch between prologues when the profiling mode is toggled.
// Specifically, AsmJSModule::setProfilingEnabled patches all callsites to
// either call the profiling or non-profiling entry point.
void
js::GenerateAsmJSFunctionPrologue(MacroAssembler& masm, unsigned framePushed,
                                  AsmJSFunctionOffsets* offsets)
{
#if defined(JS_CODEGEN_ARM)
    // Flush pending pools so they do not get dumped between the 'begin' and
    // 'entry' offsets since the difference must be less than UINT8_MAX.
    masm.flushBuffer();
#endif

    masm.haltingAlign(CodeAlignment);

    GenerateProfilingPrologue(masm, framePushed, ExitReason::None, offsets);
    Label body;
    masm.jump(&body);

    // Generate normal prologue:
    masm.haltingAlign(CodeAlignment);
    offsets->nonProfilingEntry = masm.currentOffset();
    PushRetAddr(masm);
    masm.subFromStackPtr(Imm32(framePushed + AsmJSFrameBytesAfterReturnAddress));

    // Prologue join point, body begin:
    masm.bind(&body);
    masm.setFramePushed(framePushed);
}

// Similar to GenerateAsmJSFunctionPrologue (see comment), we generate both a
// profiling and non-profiling epilogue a priori. When the profiling mode is
// toggled, AsmJSModule::setProfilingEnabled patches the 'profiling jump' to
// either be a nop (falling through to the normal prologue) or a jump (jumping
// to the profiling epilogue).
void
js::GenerateAsmJSFunctionEpilogue(MacroAssembler& masm, unsigned framePushed,
                                  AsmJSFunctionOffsets* offsets)
{
    MOZ_ASSERT(masm.framePushed() == framePushed);

#if defined(JS_CODEGEN_ARM)
    // Flush pending pools so they do not get dumped between the profilingReturn
    // and profilingJump/profilingEpilogue offsets since the difference must be
    // less than UINT8_MAX.
    masm.flushBuffer();
#endif

    // Generate a nop that is overwritten by a jump to the profiling epilogue
    // when profiling is enabled.
    {
#if defined(JS_CODEGEN_ARM)
        // Forbid pools from being inserted between the profilingJump label and
        // the nop since we need the location of the actual nop to patch it.
        AutoForbidPools afp(&masm, 1);
#endif

        // The exact form of this instruction must be kept consistent with the
        // patching in AsmJSModule::setProfilingEnabled.
        offsets->profilingJump = masm.currentOffset();
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        masm.twoByteNop();
#elif defined(JS_CODEGEN_ARM)
        masm.nop();
#elif defined(JS_CODEGEN_MIPS32)
        masm.nop();
        masm.nop();
        masm.nop();
        masm.nop();
#elif defined(JS_CODEGEN_MIPS64)
        masm.nop();
        masm.nop();
        masm.nop();
        masm.nop();
        masm.nop();
        masm.nop();
#endif
    }

    // Normal epilogue:
    masm.addToStackPtr(Imm32(framePushed + AsmJSFrameBytesAfterReturnAddress));
    masm.ret();
    masm.setFramePushed(0);

    // Profiling epilogue:
    offsets->profilingEpilogue = masm.currentOffset();
    GenerateProfilingEpilogue(masm, framePushed, ExitReason::None, offsets);
}

void
js::GenerateAsmJSExitPrologue(MacroAssembler& masm, unsigned framePushed, ExitReason reason,
                              AsmJSProfilingOffsets* offsets, Label* maybeEntry)
{
    masm.haltingAlign(CodeAlignment);
    GenerateProfilingPrologue(masm, framePushed, reason, offsets, maybeEntry);
    masm.setFramePushed(framePushed);
}

void
js::GenerateAsmJSExitEpilogue(MacroAssembler& masm, unsigned framePushed, ExitReason reason,
                              AsmJSProfilingOffsets* offsets)
{
    // Inverse of GenerateAsmJSExitPrologue:
    MOZ_ASSERT(masm.framePushed() == framePushed);
    GenerateProfilingEpilogue(masm, framePushed, reason, offsets);
    masm.setFramePushed(0);
}

/*****************************************************************************/
// AsmJSProfilingFrameIterator

AsmJSProfilingFrameIterator::AsmJSProfilingFrameIterator(const AsmJSActivation& activation)
  : module_(&activation.module()),
    callerFP_(nullptr),
    callerPC_(nullptr),
    stackAddress_(nullptr),
    exitReason_(ExitReason::None),
    codeRange_(nullptr)
{
    // If profiling hasn't been enabled for this module, then CallerFPFromFP
    // will be trash, so ignore the entire activation. In practice, this only
    // happens if profiling is enabled while module->active() (in this case,
    // profiling will be enabled when the module becomes inactive and gets
    // called again).
    if (!module_->profilingEnabled()) {
        MOZ_ASSERT(done());
        return;
    }

    initFromFP(activation);
}

static inline void
AssertMatchesCallSite(const AsmJSModule& module, const AsmJSModule::CodeRange* calleeCodeRange,
                      void* callerPC, void* callerFP, void* fp)
{
#ifdef DEBUG
    const AsmJSModule::CodeRange* callerCodeRange = module.lookupCodeRange(callerPC);
    MOZ_ASSERT(callerCodeRange);
    if (callerCodeRange->isEntry()) {
        MOZ_ASSERT(callerFP == nullptr);
        return;
    }

    const CallSite* callsite = module.lookupCallSite(callerPC);
    if (calleeCodeRange->isThunk()) {
        MOZ_ASSERT(!callsite);
        MOZ_ASSERT(callerCodeRange->isFunction());
    } else {
        MOZ_ASSERT(callsite);
        MOZ_ASSERT(callerFP == (uint8_t*)fp + callsite->stackDepth());
    }
#endif
}

void
AsmJSProfilingFrameIterator::initFromFP(const AsmJSActivation& activation)
{
    uint8_t* fp = activation.fp();

    // If a signal was handled while entering an activation, the frame will
    // still be null.
    if (!fp) {
        MOZ_ASSERT(done());
        return;
    }

    // Since we don't have the pc for fp, start unwinding at the caller of fp
    // (ReturnAddressFromFP(fp)). This means that the innermost frame is
    // skipped. This is fine because:
    //  - for FFI calls, the innermost frame is a thunk, so the first frame that
    //    shows up is the function calling the FFI;
    //  - for Math and other builtin calls, when profiling is activated, we
    //    patch all call sites to instead call through a thunk; and
    //  - for interrupts, we just accept that we'll lose the innermost frame.
    void* pc = ReturnAddressFromFP(fp);
    const AsmJSModule::CodeRange* codeRange = module_->lookupCodeRange(pc);
    MOZ_ASSERT(codeRange);
    codeRange_ = codeRange;
    stackAddress_ = fp;

    switch (codeRange->kind()) {
      case AsmJSModule::CodeRange::Entry:
        callerPC_ = nullptr;
        callerFP_ = nullptr;
        break;
      case AsmJSModule::CodeRange::Function:
        fp = CallerFPFromFP(fp);
        callerPC_ = ReturnAddressFromFP(fp);
        callerFP_ = CallerFPFromFP(fp);
        AssertMatchesCallSite(*module_, codeRange, callerPC_, callerFP_, fp);
        break;
      case AsmJSModule::CodeRange::JitFFI:
      case AsmJSModule::CodeRange::SlowFFI:
      case AsmJSModule::CodeRange::Interrupt:
      case AsmJSModule::CodeRange::Inline:
      case AsmJSModule::CodeRange::Thunk:
        MOZ_CRASH("Unexpected CodeRange kind");
    }

    // Despite the above reasoning for skipping a frame, we do actually want FFI
    // trampolines and interrupts to show up in the profile (so they can
    // accumulate self time and explain performance faults). To do this, an
    // "exit reason" is stored on all the paths leaving asm.js and this iterator
    // treats this exit reason as its own frame. If we have exited asm.js code
    // without setting an exit reason, the reason will be None and this means
    // the code was asynchronously interrupted.
    exitReason_ = activation.exitReason();
    if (exitReason_.kind() == ExitReason::None)
        exitReason_ = ExitReason::Interrupt;

    MOZ_ASSERT(!done());
}

typedef JS::ProfilingFrameIterator::RegisterState RegisterState;

AsmJSProfilingFrameIterator::AsmJSProfilingFrameIterator(const AsmJSActivation& activation,
                                                         const RegisterState& state)
  : module_(&activation.module()),
    callerFP_(nullptr),
    callerPC_(nullptr),
    exitReason_(ExitReason::None),
    codeRange_(nullptr)
{
    // If profiling hasn't been enabled for this module, then CallerFPFromFP
    // will be trash, so ignore the entire activation. In practice, this only
    // happens if profiling is enabled while module->active() (in this case,
    // profiling will be enabled when the module becomes inactive and gets
    // called again).
    if (!module_->profilingEnabled()) {
        MOZ_ASSERT(done());
        return;
    }

    // If pc isn't in the module, we must have exited the asm.js module via an
    // exit trampoline or signal handler.
    if (!module_->containsCodePC(state.pc)) {
        initFromFP(activation);
        return;
    }

    // Note: fp may be null while entering and leaving the activation.
    uint8_t* fp = activation.fp();

    const AsmJSModule::CodeRange* codeRange = module_->lookupCodeRange(state.pc);
    switch (codeRange->kind()) {
      case AsmJSModule::CodeRange::Function:
      case AsmJSModule::CodeRange::JitFFI:
      case AsmJSModule::CodeRange::SlowFFI:
      case AsmJSModule::CodeRange::Interrupt:
      case AsmJSModule::CodeRange::Thunk: {
        // When the pc is inside the prologue/epilogue, the innermost
        // call's AsmJSFrame is not complete and thus fp points to the the
        // second-to-innermost call's AsmJSFrame. Since fp can only tell you
        // about its caller (via ReturnAddressFromFP(fp)), naively unwinding
        // while pc is in the prologue/epilogue would skip the second-to-
        // innermost call. To avoid this problem, we use the static structure of
        // the code in the prologue and epilogue to do the Right Thing.
        uint32_t offsetInModule = (uint8_t*)state.pc - module_->codeBase();
        MOZ_ASSERT(offsetInModule < module_->codeBytes());
        MOZ_ASSERT(offsetInModule >= codeRange->begin());
        MOZ_ASSERT(offsetInModule < codeRange->end());
        uint32_t offsetInCodeRange = offsetInModule - codeRange->begin();
        void** sp = (void**)state.sp;
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        if (offsetInCodeRange < PushedRetAddr) {
            // First instruction of the ARM/MIPS function; the return address is
            // still in lr and fp still holds the caller's fp.
            callerPC_ = state.lr;
            callerFP_ = fp;
            AssertMatchesCallSite(*module_, codeRange, callerPC_, callerFP_, sp - 2);
        } else if (offsetInModule == codeRange->profilingReturn() - PostStorePrePopFP) {
            // Second-to-last instruction of the ARM/MIPS function; fp points to
            // the caller's fp; have not yet popped AsmJSFrame.
            callerPC_ = ReturnAddressFromFP(sp);
            callerFP_ = CallerFPFromFP(sp);
            AssertMatchesCallSite(*module_, codeRange, callerPC_, callerFP_, sp);
        } else
#endif
        if (offsetInCodeRange < PushedFP || offsetInModule == codeRange->profilingReturn()) {
            // The return address has been pushed on the stack but not fp; fp
            // still points to the caller's fp.
            callerPC_ = *sp;
            callerFP_ = fp;
            AssertMatchesCallSite(*module_, codeRange, callerPC_, callerFP_, sp - 1);
        } else if (offsetInCodeRange < StoredFP) {
            // The full AsmJSFrame has been pushed; fp still points to the
            // caller's frame.
            MOZ_ASSERT(fp == CallerFPFromFP(sp));
            callerPC_ = ReturnAddressFromFP(sp);
            callerFP_ = CallerFPFromFP(sp);
            AssertMatchesCallSite(*module_, codeRange, callerPC_, callerFP_, sp);
        } else {
            // Not in the prologue/epilogue.
            callerPC_ = ReturnAddressFromFP(fp);
            callerFP_ = CallerFPFromFP(fp);
            AssertMatchesCallSite(*module_, codeRange, callerPC_, callerFP_, fp);
        }
        break;
      }
      case AsmJSModule::CodeRange::Entry: {
        // The entry trampoline is the final frame in an AsmJSActivation. The entry
        // trampoline also doesn't GenerateAsmJSPrologue/Epilogue so we can't use
        // the general unwinding logic above.
        MOZ_ASSERT(!fp);
        callerPC_ = nullptr;
        callerFP_ = nullptr;
        break;
      }
      case AsmJSModule::CodeRange::Inline: {
        // The throw stub clears AsmJSActivation::fp on it's way out.
        if (!fp) {
            MOZ_ASSERT(done());
            return;
        }

        // Most inline code stubs execute after the prologue/epilogue have
        // completed so we can simply unwind based on fp. The only exception is
        // the async interrupt stub, since it can be executed at any time.
        // However, the async interrupt is super rare, so we can tolerate
        // skipped frames. Thus, we use simply unwind based on fp.
        callerPC_ = ReturnAddressFromFP(fp);
        callerFP_ = CallerFPFromFP(fp);
        AssertMatchesCallSite(*module_, codeRange, callerPC_, callerFP_, fp);
        break;
      }
    }

    codeRange_ = codeRange;
    stackAddress_ = state.sp;
    MOZ_ASSERT(!done());
}

void
AsmJSProfilingFrameIterator::operator++()
{
    if (exitReason_.kind() != ExitReason::None) {
        MOZ_ASSERT(codeRange_);
        exitReason_ = ExitReason::None;
        MOZ_ASSERT(!done());
        return;
    }

    if (!callerPC_) {
        MOZ_ASSERT(!callerFP_);
        codeRange_ = nullptr;
        MOZ_ASSERT(done());
        return;
    }

    MOZ_ASSERT(callerPC_);
    const AsmJSModule::CodeRange* codeRange = module_->lookupCodeRange(callerPC_);
    MOZ_ASSERT(codeRange);
    codeRange_ = codeRange;

    switch (codeRange->kind()) {
      case AsmJSModule::CodeRange::Entry:
        MOZ_ASSERT(callerFP_ == nullptr);
        callerPC_ = nullptr;
        break;
      case AsmJSModule::CodeRange::Function:
      case AsmJSModule::CodeRange::JitFFI:
      case AsmJSModule::CodeRange::SlowFFI:
      case AsmJSModule::CodeRange::Interrupt:
      case AsmJSModule::CodeRange::Inline:
      case AsmJSModule::CodeRange::Thunk:
        stackAddress_ = callerFP_;
        callerPC_ = ReturnAddressFromFP(callerFP_);
        AssertMatchesCallSite(*module_, codeRange, callerPC_, CallerFPFromFP(callerFP_), callerFP_);
        callerFP_ = CallerFPFromFP(callerFP_);
        break;
    }

    MOZ_ASSERT(!done());
}

static const char*
BuiltinToName(Builtin builtin)
{
    // Note: this label is regexp-matched by
    // devtools/client/profiler/cleopatra/js/parserWorker.js.

    switch (builtin) {
      case Builtin::ToInt32:         return "ToInt32 (in asm.js)";
#if defined(JS_CODEGEN_ARM)
      case Builtin::aeabi_idivmod:   return "software idivmod (in asm.js)";
      case Builtin::aeabi_uidivmod:  return "software uidivmod (in asm.js)";
      case Builtin::AtomicCmpXchg:   return "Atomics.compareExchange (in asm.js)";
      case Builtin::AtomicXchg:      return "Atomics.exchange (in asm.js)";
      case Builtin::AtomicFetchAdd:  return "Atomics.add (in asm.js)";
      case Builtin::AtomicFetchSub:  return "Atomics.sub (in asm.js)";
      case Builtin::AtomicFetchAnd:  return "Atomics.and (in asm.js)";
      case Builtin::AtomicFetchOr:   return "Atomics.or (in asm.js)";
      case Builtin::AtomicFetchXor:  return "Atomics.xor (in asm.js)";
#endif
      case Builtin::ModD:    return "fmod (in asm.js)";
      case Builtin::SinD:    return "Math.sin (in asm.js)";
      case Builtin::CosD:    return "Math.cos (in asm.js)";
      case Builtin::TanD:    return "Math.tan (in asm.js)";
      case Builtin::ASinD:   return "Math.asin (in asm.js)";
      case Builtin::ACosD:   return "Math.acos (in asm.js)";
      case Builtin::ATanD:   return "Math.atan (in asm.js)";
      case Builtin::CeilD:
      case Builtin::CeilF:   return "Math.ceil (in asm.js)";
      case Builtin::FloorD:
      case Builtin::FloorF:  return "Math.floor (in asm.js)";
      case Builtin::ExpD:    return "Math.exp (in asm.js)";
      case Builtin::LogD:    return "Math.log (in asm.js)";
      case Builtin::PowD:    return "Math.pow (in asm.js)";
      case Builtin::ATan2D:  return "Math.atan2 (in asm.js)";
      case Builtin::Limit:   break;
    }
    MOZ_CRASH("symbolic immediate not a builtin");
}

const char*
AsmJSProfilingFrameIterator::label() const
{
    MOZ_ASSERT(!done());

    // Use the same string for both time inside and under so that the two
    // entries will be coalesced by the profiler.
    //
    // NB: these labels are regexp-matched by
    //     devtools/client/profiler/cleopatra/js/parserWorker.js.
    const char* jitFFIDescription = "fast FFI trampoline (in asm.js)";
    const char* slowFFIDescription = "slow FFI trampoline (in asm.js)";
    const char* interruptDescription = "interrupt due to out-of-bounds or long execution (in asm.js)";

    switch (exitReason_.kind()) {
      case ExitReason::None:
        break;
      case ExitReason::Jit:
        return jitFFIDescription;
      case ExitReason::Slow:
        return slowFFIDescription;
      case ExitReason::Interrupt:
        return interruptDescription;
      case ExitReason::Builtin:
        return BuiltinToName(exitReason_.builtin());
    }

    auto codeRange = reinterpret_cast<const AsmJSModule::CodeRange*>(codeRange_);
    switch (codeRange->kind()) {
      case AsmJSModule::CodeRange::Function:  return codeRange->functionProfilingLabel(*module_);
      case AsmJSModule::CodeRange::Entry:     return "entry trampoline (in asm.js)";
      case AsmJSModule::CodeRange::JitFFI:    return jitFFIDescription;
      case AsmJSModule::CodeRange::SlowFFI:   return slowFFIDescription;
      case AsmJSModule::CodeRange::Interrupt: return interruptDescription;
      case AsmJSModule::CodeRange::Inline:    return "inline stub (in asm.js)";
      case AsmJSModule::CodeRange::Thunk:     return BuiltinToName(codeRange->thunkTarget());
    }

    MOZ_CRASH("bad code range kind");
}
