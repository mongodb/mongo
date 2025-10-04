/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/MathAlgorithms.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineJIT.h"
#include "jit/CalleeToken.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/PerfSpewer.h"
#include "jit/VMFunctions.h"
#include "jit/x86/SharedICHelpers-x86.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"
#include "vm/Realm.h"
#ifdef MOZ_VTUNE
#  include "vtune/VTuneWrapper.h"
#endif

#include "jit/MacroAssembler-inl.h"
#include "vm/JSScript-inl.h"

using mozilla::IsPowerOfTwo;

using namespace js;
using namespace js::jit;

// All registers to save and restore. This includes the stack pointer, since we
// use the ability to reference register values on the stack by index.
static const LiveRegisterSet AllRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::AllMask),
                    FloatRegisterSet(FloatRegisters::AllMask));

enum EnterJitEbpArgumentOffset {
  ARG_JITCODE = 2 * sizeof(void*),
  ARG_ARGC = 3 * sizeof(void*),
  ARG_ARGV = 4 * sizeof(void*),
  ARG_STACKFRAME = 5 * sizeof(void*),
  ARG_CALLEETOKEN = 6 * sizeof(void*),
  ARG_SCOPECHAIN = 7 * sizeof(void*),
  ARG_STACKVALUES = 8 * sizeof(void*),
  ARG_RESULT = 9 * sizeof(void*)
};

// Generates a trampoline for calling Jit compiled code from a C++ function.
// The trampoline use the EnterJitCode signature, with the standard cdecl
// calling convention.
void JitRuntime::generateEnterJIT(JSContext* cx, MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateEnterJIT");

  enterJITOffset_ = startTrampolineCode(masm);

  masm.assertStackAlignment(ABIStackAlignment,
                            -int32_t(sizeof(uintptr_t)) /* return address */);

  // Save old stack frame pointer, set new stack frame pointer.
  masm.push(ebp);
  masm.movl(esp, ebp);

  // Save non-volatile registers. These must be saved by the trampoline,
  // rather than the JIT'd code, because they are scanned by the conservative
  // scanner.
  masm.push(ebx);
  masm.push(esi);
  masm.push(edi);

  // Load the number of values to be copied (argc) into eax
  masm.loadPtr(Address(ebp, ARG_ARGC), eax);

  // If we are constructing, that also needs to include newTarget
  {
    Label noNewTarget;
    masm.loadPtr(Address(ebp, ARG_CALLEETOKEN), edx);
    masm.branchTest32(Assembler::Zero, edx,
                      Imm32(CalleeToken_FunctionConstructing), &noNewTarget);

    masm.addl(Imm32(1), eax);

    masm.bind(&noNewTarget);
  }

  // eax <- 8*numValues, eax is now the offset betwen argv and the last value.
  masm.shll(Imm32(3), eax);

  // Guarantee stack alignment of Jit frames.
  //
  // This code compensates for the offset created by the copy of the vector of
  // arguments, such that the jit frame will be aligned once the return
  // address is pushed on the stack.
  //
  // In the computation of the offset, we omit the size of the JitFrameLayout
  // which is pushed on the stack, as the JitFrameLayout size is a multiple of
  // the JitStackAlignment.
  masm.movl(esp, ecx);
  masm.subl(eax, ecx);
  static_assert(
      sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");

  // ecx = ecx & 15, holds alignment.
  masm.andl(Imm32(JitStackAlignment - 1), ecx);
  masm.subl(ecx, esp);

  /***************************************************************
  Loop over argv vector, push arguments onto stack in reverse order
  ***************************************************************/

  // ebx = argv   --argv pointer is in ebp + 16
  masm.loadPtr(Address(ebp, ARG_ARGV), ebx);

  // eax = argv[8(argc)]  --eax now points one value past the last argument
  masm.addl(ebx, eax);

  // while (eax > ebx)  --while still looping through arguments
  {
    Label header, footer;
    masm.bind(&header);

    masm.cmp32(eax, ebx);
    masm.j(Assembler::BelowOrEqual, &footer);

    // eax -= 8  --move to previous argument
    masm.subl(Imm32(8), eax);

    // Push what eax points to on stack, a Value is 2 words
    masm.push(Operand(eax, 4));
    masm.push(Operand(eax, 0));

    masm.jmp(&header);
    masm.bind(&footer);
  }

  // Load the number of actual arguments.  |result| is used to store the
  // actual number of arguments without adding an extra argument to the enter
  // JIT.
  masm.mov(Operand(ebp, ARG_RESULT), eax);
  masm.unboxInt32(Address(eax, 0x0), eax);

  // Push the callee token.
  masm.push(Operand(ebp, ARG_CALLEETOKEN));

  // Load the InterpreterFrame address into the OsrFrameReg.
  // This address is also used for setting the constructing bit on all paths.
  masm.loadPtr(Address(ebp, ARG_STACKFRAME), OsrFrameReg);

  // Push the descriptor.
  masm.pushFrameDescriptorForJitCall(FrameType::CppToJSJit, eax, eax);

  CodeLabel returnLabel;
  Label oomReturnLabel;
  {
    // Handle Interpreter -> Baseline OSR.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    MOZ_ASSERT(!regs.has(ebp));
    regs.take(OsrFrameReg);
    regs.take(ReturnReg);

    Register scratch = regs.takeAny();

    Label notOsr;
    masm.branchTestPtr(Assembler::Zero, OsrFrameReg, OsrFrameReg, &notOsr);

    Register numStackValues = regs.takeAny();
    masm.loadPtr(Address(ebp, ARG_STACKVALUES), numStackValues);

    Register jitcode = regs.takeAny();
    masm.loadPtr(Address(ebp, ARG_JITCODE), jitcode);

    // Push return address.
    masm.mov(&returnLabel, scratch);
    masm.push(scratch);

    // Frame prologue.
    masm.push(ebp);
    masm.mov(esp, ebp);

    // Reserve frame.
    masm.subPtr(Imm32(BaselineFrame::Size()), esp);

    Register framePtrScratch = regs.takeAny();
    masm.touchFrameValues(numStackValues, scratch, framePtrScratch);
    masm.mov(esp, framePtrScratch);

    // Reserve space for locals and stack values.
    masm.mov(numStackValues, scratch);
    masm.shll(Imm32(3), scratch);
    masm.subPtr(scratch, esp);

    // Enter exit frame.
    masm.pushFrameDescriptor(FrameType::BaselineJS);
    masm.push(Imm32(0));  // Fake return address.
    masm.push(FramePointer);
    // No GC things to mark on the stack, push a bare token.
    masm.loadJSContext(scratch);
    masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

    masm.push(jitcode);

    using Fn = bool (*)(BaselineFrame* frame, InterpreterFrame* interpFrame,
                        uint32_t numStackValues);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(framePtrScratch);  // BaselineFrame
    masm.passABIArg(OsrFrameReg);      // InterpreterFrame
    masm.passABIArg(numStackValues);
    masm.callWithABI<Fn, jit::InitBaselineFrameForOsr>(
        ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    masm.pop(jitcode);

    MOZ_ASSERT(jitcode != ReturnReg);

    Label error;
    masm.addPtr(Imm32(ExitFrameLayout::SizeWithFooter()), esp);
    masm.branchIfFalseBool(ReturnReg, &error);

    // If OSR-ing, then emit instrumentation for setting lastProfilerFrame
    // if profiler instrumentation is enabled.
    {
      Label skipProfilingInstrumentation;
      AbsoluteAddress addressOfEnabled(
          cx->runtime()->geckoProfiler().addressOfEnabled());
      masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                    &skipProfilingInstrumentation);
      masm.profilerEnterFrame(ebp, scratch);
      masm.bind(&skipProfilingInstrumentation);
    }

    masm.jump(jitcode);

    // OOM: frame epilogue, load error value, discard return address and return.
    masm.bind(&error);
    masm.mov(ebp, esp);
    masm.pop(ebp);
    masm.addPtr(Imm32(sizeof(uintptr_t)), esp);  // Return address.
    masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    masm.jump(&oomReturnLabel);

    masm.bind(&notOsr);
    masm.loadPtr(Address(ebp, ARG_SCOPECHAIN), R1.scratchReg());
  }

  // The call will push the return address and frame pointer on the stack, thus
  // we check that the stack would be aligned once the call is complete.
  masm.assertStackAlignment(JitStackAlignment, 2 * sizeof(uintptr_t));

  /***************************************************************
      Call passed-in code, get return value and fill in the
      passed in return value pointer
  ***************************************************************/
  masm.call(Address(ebp, ARG_JITCODE));

  {
    // Interpreter -> Baseline OSR will return here.
    masm.bind(&returnLabel);
    masm.addCodeLabel(returnLabel);
    masm.bind(&oomReturnLabel);
  }

  // Restore the stack pointer so the stack looks like this:
  //  +20 ... arguments ...
  //  +16 <return>
  //  +12 ebp <- %ebp pointing here.
  //  +8  ebx
  //  +4  esi
  //  +0  edi <- %esp pointing here.
  masm.lea(Operand(ebp, -int32_t(3 * sizeof(void*))), esp);

  // Store the return value.
  masm.loadPtr(Address(ebp, ARG_RESULT), eax);
  masm.storeValue(JSReturnOperand, Operand(eax, 0));

  /**************************************************************
      Return stack and registers to correct state
  **************************************************************/

  // Restore non-volatile registers
  masm.pop(edi);
  masm.pop(esi);
  masm.pop(ebx);

  // Restore old stack frame pointer
  masm.pop(ebp);
  masm.ret();
}

// static
mozilla::Maybe<::JS::ProfilingFrameIterator::RegisterState>
JitRuntime::getCppEntryRegisters(JitFrameLayout* frameStackAddress) {
  // Not supported, or not implemented yet.
  // TODO: Implement along with the corresponding stack-walker changes, in
  // coordination with the Gecko Profiler, see bug 1635987 and follow-ups.
  return mozilla::Nothing{};
}

// Push AllRegs in a way that is compatible with RegisterDump, regardless of
// what PushRegsInMask might do to reduce the set size.
static void DumpAllRegs(MacroAssembler& masm) {
#ifdef ENABLE_WASM_SIMD
  masm.PushRegsInMask(AllRegs);
#else
  // When SIMD isn't supported, PushRegsInMask reduces the set of float
  // registers to be double-sized, while the RegisterDump expects each of
  // the float registers to have the maximal possible size
  // (Simd128DataSize). To work around this, we just spill the double
  // registers by hand here, using the register dump offset directly.
  for (GeneralRegisterBackwardIterator iter(AllRegs.gprs()); iter.more();
       ++iter) {
    masm.Push(*iter);
  }

  masm.reserveStack(sizeof(RegisterDump::FPUArray));
  for (FloatRegisterBackwardIterator iter(AllRegs.fpus()); iter.more();
       ++iter) {
    FloatRegister reg = *iter;
    Address spillAddress(StackPointer, reg.getRegisterDumpOffsetInBytes());
    masm.storeDouble(reg, spillAddress);
  }
#endif
}

void JitRuntime::generateInvalidator(MacroAssembler& masm, Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateInvalidator");

  invalidatorOffset_ = startTrampolineCode(masm);

  // We do the minimum amount of work in assembly and shunt the rest
  // off to InvalidationBailout. Assembly does:
  //
  // - Push the machine state onto the stack.
  // - Call the InvalidationBailout routine with the stack pointer.
  // - Now that the frame has been bailed out, convert the invalidated
  //   frame into an exit frame.
  // - Do the normal check-return-code-and-thunk-to-the-interpreter dance.

  // Push registers such that we can access them from [base + code].
  DumpAllRegs(masm);

  masm.movl(esp, eax);  // Argument to jit::InvalidationBailout.

  // Make space for InvalidationBailout's bailoutInfo outparam.
  masm.reserveStack(sizeof(void*));
  masm.movl(esp, ebx);

  using Fn = bool (*)(InvalidationBailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupUnalignedABICall(edx);
  masm.passABIArg(eax);
  masm.passABIArg(ebx);
  masm.callWithABI<Fn, InvalidationBailout>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(ecx);  // Get bailoutInfo outparam.

  // Pop the machine state and the dead frame.
  masm.moveToStackPtr(FramePointer);

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in ecx.
  masm.jmp(bailoutTail);
}

void JitRuntime::generateArgumentsRectifier(MacroAssembler& masm,
                                            ArgumentsRectifierKind kind) {
  AutoCreatedBy acb(masm, "JitRuntime::generateArgumentsRectifier");

  switch (kind) {
    case ArgumentsRectifierKind::Normal:
      argumentsRectifierOffset_ = startTrampolineCode(masm);
      break;
    case ArgumentsRectifierKind::TrialInlining:
      trialInliningArgumentsRectifierOffset_ = startTrampolineCode(masm);
      break;
  }

  // Caller:
  // [arg2] [arg1] [this] [ [argc] [callee] [descr] [raddr] ] <- esp

  // Frame prologue.
  //
  // NOTE: if this changes, fix the Baseline bailout code too!
  // See BaselineStackBuilder::calculatePrevFramePtr and
  // BaselineStackBuilder::buildRectifierFrame (in BaselineBailouts.cpp).
  masm.push(FramePointer);
  masm.movl(esp, FramePointer);  // Save %esp.

  // Load argc.
  masm.loadNumActualArgs(FramePointer, esi);

  // Load the number of |undefined|s to push into %ecx.
  masm.loadPtr(Address(ebp, RectifierFrameLayout::offsetOfCalleeToken()), eax);
  masm.mov(eax, ecx);
  masm.andl(Imm32(CalleeTokenMask), ecx);
  masm.loadFunctionArgCount(ecx, ecx);

  // The frame pointer and its padding are pushed on the stack.
  // Including |this|, there are (|nformals| + 1) arguments to push to the
  // stack.  Then we push a JitFrameLayout.  We compute the padding expressed
  // in the number of extra |undefined| values to push on the stack.
  static_assert(
      sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");
  static_assert(
      JitStackAlignment % sizeof(Value) == 0,
      "Ensure that we can pad the stack by pushing extra UndefinedValue");
  static_assert(IsPowerOfTwo(JitStackValueAlignment),
                "must have power of two for masm.andl to do its job");

  masm.addl(
      Imm32(JitStackValueAlignment - 1 /* for padding */ + 1 /* for |this| */),
      ecx);

  // Account for newTarget, if necessary.
  static_assert(
      CalleeToken_FunctionConstructing == 1,
      "Ensure that we can use the constructing bit to count an extra push");
  masm.mov(eax, edx);
  masm.andl(Imm32(CalleeToken_FunctionConstructing), edx);
  masm.addl(edx, ecx);

  masm.andl(Imm32(~(JitStackValueAlignment - 1)), ecx);
  masm.subl(esi, ecx);
  masm.subl(Imm32(1), ecx);  // For |this|.

  // Copy the number of actual arguments into edx.
  masm.mov(esi, edx);

  masm.moveValue(UndefinedValue(), ValueOperand(ebx, edi));

  // Caller:
  // [arg2] [arg1] [this] [ [argc] [callee] [descr] [raddr] ]
  // '-- #esi ---'
  //
  // Rectifier frame:
  // [ebp'] <- ebp [padding] <- esp [undef] [undef] [arg2] [arg1] [this]
  //                                '--- #ecx ----' '-- #esi ---'
  //
  // [ [argc] [callee] [descr] [raddr] ]

  // Push undefined.
  {
    Label undefLoopTop;
    masm.bind(&undefLoopTop);

    masm.push(ebx);  // type(undefined);
    masm.push(edi);  // payload(undefined);
    masm.subl(Imm32(1), ecx);
    masm.j(Assembler::NonZero, &undefLoopTop);
  }

  // Get the topmost argument.
  BaseIndex b(FramePointer, esi, TimesEight, sizeof(RectifierFrameLayout));
  masm.lea(Operand(b), ecx);

  // Push arguments, |nargs| + 1 times (to include |this|).
  masm.addl(Imm32(1), esi);
  {
    Label copyLoopTop;

    masm.bind(&copyLoopTop);
    masm.push(Operand(ecx, sizeof(Value) / 2));
    masm.push(Operand(ecx, 0x0));
    masm.subl(Imm32(sizeof(Value)), ecx);
    masm.subl(Imm32(1), esi);
    masm.j(Assembler::NonZero, &copyLoopTop);
  }

  {
    Label notConstructing;

    masm.mov(eax, ebx);
    masm.branchTest32(Assembler::Zero, ebx,
                      Imm32(CalleeToken_FunctionConstructing),
                      &notConstructing);

    BaseValueIndex src(FramePointer, edx,
                       sizeof(RectifierFrameLayout) + sizeof(Value));

    masm.andl(Imm32(CalleeTokenMask), ebx);
    masm.loadFunctionArgCount(ebx, ebx);

    BaseValueIndex dst(esp, ebx, sizeof(Value));

    ValueOperand newTarget(ecx, edi);

    masm.loadValue(src, newTarget);
    masm.storeValue(newTarget, dst);

    masm.bind(&notConstructing);
  }

  // Construct JitFrameLayout.
  masm.push(eax);  // callee token
  masm.pushFrameDescriptorForJitCall(FrameType::Rectifier, edx, edx);

  // Call the target function.
  masm.andl(Imm32(CalleeTokenMask), eax);
  switch (kind) {
    case ArgumentsRectifierKind::Normal:
      masm.loadJitCodeRaw(eax, eax);
      argumentsRectifierReturnOffset_ = masm.callJitNoProfiler(eax);
      break;
    case ArgumentsRectifierKind::TrialInlining:
      Label noBaselineScript, done;
      masm.loadBaselineJitCodeRaw(eax, ebx, &noBaselineScript);
      masm.callJitNoProfiler(ebx);
      masm.jump(&done);

      // See BaselineCacheIRCompiler::emitCallInlinedFunction.
      masm.bind(&noBaselineScript);
      masm.loadJitCodeRaw(eax, eax);
      masm.callJitNoProfiler(eax);
      masm.bind(&done);
      break;
  }

  masm.mov(FramePointer, StackPointer);
  masm.pop(FramePointer);
  masm.ret();
}

static void PushBailoutFrame(MacroAssembler& masm, Register spArg) {
  // Push registers such that we can access them from [base + code].
  DumpAllRegs(masm);

  // The current stack pointer is the first argument to jit::Bailout.
  masm.movl(esp, spArg);
}

static void GenerateBailoutThunk(MacroAssembler& masm, Label* bailoutTail) {
  PushBailoutFrame(masm, eax);

  // Make space for Bailout's bailoutInfo outparam.
  masm.reserveStack(sizeof(void*));
  masm.movl(esp, ebx);

  // Call the bailout function.
  using Fn = bool (*)(BailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupUnalignedABICall(ecx);
  masm.passABIArg(eax);
  masm.passABIArg(ebx);
  masm.callWithABI<Fn, Bailout>(ABIType::General,
                                CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(ecx);  // Get the bailoutInfo outparam.

  // Remove both the bailout frame and the topmost Ion frame's stack.
  masm.moveToStackPtr(FramePointer);

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in ecx.
  masm.jmp(bailoutTail);
}

void JitRuntime::generateBailoutHandler(MacroAssembler& masm,
                                        Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateBailoutHandler");

  bailoutHandlerOffset_ = startTrampolineCode(masm);

  GenerateBailoutThunk(masm, bailoutTail);
}

bool JitRuntime::generateVMWrapper(JSContext* cx, MacroAssembler& masm,
                                   VMFunctionId id, const VMFunctionData& f,
                                   DynFn nativeFun, uint32_t* wrapperOffset) {
  AutoCreatedBy acb(masm, "JitRuntime::generateVMWrapper");

  *wrapperOffset = startTrampolineCode(masm);

  // Avoid conflicts with argument registers while discarding the result after
  // the function call.
  AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

  static_assert(
      (Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
      "Wrapper register set must be a superset of Volatile register set.");

  // The context is the first argument.
  Register cxreg = regs.takeAny();

  // Stack is:
  //    ... frame ...
  //  +8  [args]
  //  +4  descriptor
  //  +0  returnAddress
  //
  // Push the frame pointer to finish the exit frame, then link it up.
  masm.Push(FramePointer);
  masm.moveStackPtrTo(FramePointer);
  masm.loadJSContext(cxreg);
  masm.enterExitFrame(cxreg, regs.getAny(), id);

  // Reserve space for the outparameter.
  masm.reserveVMFunctionOutParamSpace(f);

  masm.setupUnalignedABICallDontSaveRestoreSP();
  masm.passABIArg(cxreg);

  size_t argDisp = ExitFrameLayout::Size();

  // Copy arguments.
  for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
    switch (f.argProperties(explicitArg)) {
      case VMFunctionData::WordByValue:
        masm.passABIArg(MoveOperand(FramePointer, argDisp), ABIType::General);
        argDisp += sizeof(void*);
        break;
      case VMFunctionData::DoubleByValue:
        // We don't pass doubles in float registers on x86, so no need
        // to check for argPassedInFloatReg.
        masm.passABIArg(MoveOperand(FramePointer, argDisp), ABIType::General);
        argDisp += sizeof(void*);
        masm.passABIArg(MoveOperand(FramePointer, argDisp), ABIType::General);
        argDisp += sizeof(void*);
        break;
      case VMFunctionData::WordByRef:
        masm.passABIArg(MoveOperand(FramePointer, argDisp,
                                    MoveOperand::Kind::EffectiveAddress),
                        ABIType::General);
        argDisp += sizeof(void*);
        break;
      case VMFunctionData::DoubleByRef:
        masm.passABIArg(MoveOperand(FramePointer, argDisp,
                                    MoveOperand::Kind::EffectiveAddress),
                        ABIType::General);
        argDisp += 2 * sizeof(void*);
        break;
    }
  }

  // Copy the implicit outparam, if any.
  const int32_t outParamOffset =
      -int32_t(ExitFooterFrame::Size()) - f.sizeOfOutParamStackSlot();
  if (f.outParam != Type_Void) {
    masm.passABIArg(MoveOperand(FramePointer, outParamOffset,
                                MoveOperand::Kind::EffectiveAddress),
                    ABIType::General);
  }

  masm.callWithABI(nativeFun, ABIType::General,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  // Test for failure.
  switch (f.failType()) {
    case Type_Cell:
      masm.branchTestPtr(Assembler::Zero, eax, eax, masm.failureLabel());
      break;
    case Type_Bool:
      masm.testb(eax, eax);
      masm.j(Assembler::Zero, masm.failureLabel());
      break;
    case Type_Void:
      break;
    default:
      MOZ_CRASH("unknown failure kind");
  }

  // Load the outparam.
  masm.loadVMFunctionOutParam(f, Address(FramePointer, outParamOffset));

  // Until C++ code is instrumented against Spectre, prevent speculative
  // execution from returning any private data.
  if (f.returnsData() && JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }

  // Pop frame and restore frame pointer.
  masm.moveToStackPtr(FramePointer);
  masm.pop(FramePointer);

  // Return. Subtract sizeof(void*) for the frame pointer.
  masm.retn(Imm32(sizeof(ExitFrameLayout) - sizeof(void*) +
                  f.explicitStackSlots() * sizeof(void*) +
                  f.extraValuesToPop * sizeof(Value)));

  return true;
}

uint32_t JitRuntime::generatePreBarrier(JSContext* cx, MacroAssembler& masm,
                                        MIRType type) {
  AutoCreatedBy acb(masm, "JitRuntime::generatePreBarrier");

  uint32_t offset = startTrampolineCode(masm);

  static_assert(PreBarrierReg == edx);
  Register temp1 = eax;
  Register temp2 = ebx;
  Register temp3 = ecx;
  masm.push(temp1);
  masm.push(temp2);
  masm.push(temp3);

  Label noBarrier;
  masm.emitPreBarrierFastPath(cx->runtime(), type, temp1, temp2, temp3,
                              &noBarrier);

  // Call into C++ to mark this GC thing.
  masm.pop(temp3);
  masm.pop(temp2);
  masm.pop(temp1);

  LiveRegisterSet save;
  save.set() = RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                           FloatRegisterSet(FloatRegisters::VolatileMask));
  masm.PushRegsInMask(save);

  masm.movl(ImmPtr(cx->runtime()), ecx);

  masm.setupUnalignedABICall(eax);
  masm.passABIArg(ecx);
  masm.passABIArg(edx);
  masm.callWithABI(JitPreWriteBarrier(type));

  masm.PopRegsInMask(save);
  masm.ret();

  masm.bind(&noBarrier);
  masm.pop(temp3);
  masm.pop(temp2);
  masm.pop(temp1);
  masm.ret();

  return offset;
}

void JitRuntime::generateBailoutTailStub(MacroAssembler& masm,
                                         Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateBailoutTailStub");

  masm.bind(bailoutTail);
  masm.generateBailoutTail(edx, ecx);
}
