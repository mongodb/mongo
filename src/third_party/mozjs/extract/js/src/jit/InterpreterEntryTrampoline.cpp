/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/InterpreterEntryTrampoline.h"
#include "jit/JitRuntime.h"
#include "jit/Linker.h"
#include "vm/Interpreter.h"

#include "gc/Marking-inl.h"
#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

void js::ClearInterpreterEntryMap(JSRuntime* runtime) {
  if (runtime->hasJitRuntime() &&
      runtime->jitRuntime()->hasInterpreterEntryMap()) {
    runtime->jitRuntime()->getInterpreterEntryMap()->clear();
  }
}

void EntryTrampolineMap::traceTrampolineCode(JSTracer* trc) {
  for (jit::EntryTrampolineMap::Enum e(*this); !e.empty(); e.popFront()) {
    EntryTrampoline& trampoline = e.front().value();
    trampoline.trace(trc);
  }
}

void EntryTrampolineMap::updateScriptsAfterMovingGC(void) {
  for (jit::EntryTrampolineMap::Enum e(*this); !e.empty(); e.popFront()) {
    BaseScript* script = e.front().key();
    if (IsForwarded(script)) {
      script = Forwarded(script);
      e.rekeyFront(script);
    }
  }
}

#ifdef JSGC_HASH_TABLE_CHECKS
void EntryTrampoline::checkTrampolineAfterMovingGC() const {
  JitCode* trampoline = entryTrampoline_;
  CheckGCThingAfterMovingGC(trampoline);
}

void EntryTrampolineMap::checkScriptsAfterMovingGC() {
  gc::CheckTableAfterMovingGC(*this, [](const auto& entry) {
    BaseScript* script = entry.key();
    CheckGCThingAfterMovingGC(script);
    entry.value().checkTrampolineAfterMovingGC();
    return script;
  });
}
#endif

void JitRuntime::generateBaselineInterpreterEntryTrampoline(
    MacroAssembler& masm) {
  AutoCreatedBy acb(masm,
                    "JitRuntime::generateBaselineInterpreterEntryTrampoline");

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif
  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  Register nargs = regs.takeAny();
  Register callee = regs.takeAny();
  Register scratch = regs.takeAny();

  // Load callee token and keep it in a register as it will be used often
  Address calleeTokenAddr(
      FramePointer, BaselineInterpreterEntryFrameLayout::offsetOfCalleeToken());
  masm.loadPtr(calleeTokenAddr, callee);

  // Load argc into nargs.
  masm.loadNumActualArgs(FramePointer, nargs);

  Label notFunction;
  {
    // Check if calleetoken is script or function
    masm.branchTestPtr(Assembler::NonZero, callee, Imm32(CalleeTokenScriptBit),
                       &notFunction);

    // CalleeToken is a function, load |nformals| into scratch
    masm.movePtr(callee, scratch);
    masm.andPtr(Imm32(uint32_t(CalleeTokenMask)), scratch);
    masm.loadFunctionArgCount(scratch, scratch);

    // Take max(nformals, argc).
    Label noUnderflow;
    masm.branch32(Assembler::AboveOrEqual, nargs, scratch, &noUnderflow);
    { masm.movePtr(scratch, nargs); }
    masm.bind(&noUnderflow);

    // Add 1 to nargs if constructing.
    static_assert(
        CalleeToken_FunctionConstructing == 1,
        "Ensure that we can use the constructing bit to count the value");
    masm.movePtr(callee, scratch);
    masm.and32(Imm32(uint32_t(CalleeToken_FunctionConstructing)), scratch);
    masm.addPtr(scratch, nargs);
  }
  masm.bind(&notFunction);

  // Align stack
  masm.alignJitStackBasedOnNArgs(nargs, /*countIncludesThis = */ false);

  // Point argPtr to the topmost argument.
  static_assert(sizeof(Value) == 8,
                "Using TimesEight for scale of sizeof(Value).");
  BaseIndex topPtrAddr(FramePointer, nargs, TimesEight,
                       sizeof(BaselineInterpreterEntryFrameLayout));
  Register argPtr = nargs;
  masm.computeEffectiveAddress(topPtrAddr, argPtr);

  // Load the end address into scratch, which is the callee token.
  masm.computeEffectiveAddress(calleeTokenAddr, scratch);

  // Copy |this|+arguments
  Label loop;
  masm.bind(&loop);
  {
    masm.pushValue(Address(argPtr, 0));
    masm.subPtr(Imm32(sizeof(Value)), argPtr);
    masm.branchPtr(Assembler::Above, argPtr, scratch, &loop);
  }

  // Copy callee token
  masm.push(callee);

  // Save a new descriptor using BaselineInterpreterEntry frame type.
  masm.loadNumActualArgs(FramePointer, scratch);
  masm.pushFrameDescriptorForJitCall(FrameType::BaselineInterpreterEntry,
                                     scratch, scratch);

  // Call into baseline interpreter
  uint8_t* blinterpAddr = baselineInterpreter().codeRaw();
  masm.assertStackAlignment(JitStackAlignment, 2 * sizeof(uintptr_t));
  masm.call(ImmPtr(blinterpAddr));

  masm.moveToStackPtr(FramePointer);
  masm.pop(FramePointer);
  masm.ret();
}

void JitRuntime::generateInterpreterEntryTrampoline(MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateInterpreterEntryTrampoline");

  // If BLI is disabled, we don't need an offset.
  if (IsBaselineInterpreterEnabled()) {
    uint32_t offset = startTrampolineCode(masm);
    if (!vmInterpreterEntryOffset_) {
      vmInterpreterEntryOffset_ = offset;
    }
  }

#ifdef JS_CODEGEN_ARM64
  // Use the normal stack pointer for the initial pushes.
  masm.SetStackPointer64(sp);

  // Push lr and fp together to maintain 16-byte alignment.
  masm.push(lr, FramePointer);
  masm.moveStackPtrTo(FramePointer);

  // Save the PSP register (r28), and a scratch (r19).
  masm.push(r19, r28);

  // Setup the PSP so we can use callWithABI below.
  masm.SetStackPointer64(PseudoStackPointer64);
  masm.initPseudoStackPtr();

  Register arg0 = IntArgReg0;
  Register arg1 = IntArgReg1;
  Register scratch = r19;
#elif defined(JS_CODEGEN_X86)
  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  AllocatableRegisterSet regs(RegisterSet::Volatile());
  Register arg0 = regs.takeAnyGeneral();
  Register arg1 = regs.takeAnyGeneral();
  Register scratch = regs.takeAnyGeneral();

  // First two arguments are passed on the stack in 32-bit.
  Address cxAddr(FramePointer, 2 * sizeof(void*));
  Address stateAddr(FramePointer, 3 * sizeof(void*));
  masm.loadPtr(cxAddr, arg0);
  masm.loadPtr(stateAddr, arg1);
#else
  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  AllocatableRegisterSet regs(RegisterSet::Volatile());
  regs.take(IntArgReg0);
  regs.take(IntArgReg1);
  Register arg0 = IntArgReg0;
  Register arg1 = IntArgReg1;
  Register scratch = regs.takeAnyGeneral();
#endif

  using Fn = bool (*)(JSContext* cx, js::RunState& state);
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(arg0);  // cx
  masm.passABIArg(arg1);  // state
  masm.callWithABI<Fn, Interpret>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

#ifdef JS_CODEGEN_ARM64
  masm.syncStackPtr();
  masm.SetStackPointer64(sp);

  // Restore r28 and r19.
  masm.pop(r28, r19);

  // Restore old fp and pop lr for return.
  masm.pop(FramePointer, lr);
  masm.abiret();

  // Reset stack pointer.
  masm.SetStackPointer64(PseudoStackPointer64);
#else
  masm.moveToStackPtr(FramePointer);
  masm.pop(FramePointer);
  masm.ret();
#endif
}

JitCode* JitRuntime::generateEntryTrampolineForScript(JSContext* cx,
                                                      JSScript* script) {
  if (JitSpewEnabled(JitSpew_Codegen)) {
    UniqueChars funName;
    if (script->function() && script->function()->fullDisplayAtom()) {
      funName =
          AtomToPrintableString(cx, script->function()->fullDisplayAtom());
    }

    JitSpew(JitSpew_Codegen,
            "# Emitting Interpreter Entry Trampoline for %s (%s:%u:%u)",
            funName ? funName.get() : "*", script->filename(), script->lineno(),
            script->column().oneOriginValue());
  }

  TempAllocator temp(&cx->tempLifoAlloc());
  JitContext jctx(cx);
  StackMacroAssembler masm(cx, temp);
  AutoCreatedBy acb(masm, "JitRuntime::generateEntryTrampolineForScript");
  PerfSpewerRangeRecorder rangeRecorder(masm);

  if (IsBaselineInterpreterEnabled()) {
    generateBaselineInterpreterEntryTrampoline(masm);
    rangeRecorder.recordOffset("BaselineInterpreter", cx, script);
  }

  generateInterpreterEntryTrampoline(masm);
  rangeRecorder.recordOffset("Interpreter", cx, script);

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);
  if (!code) {
    return nullptr;
  }
  rangeRecorder.collectRangesForJitCode(code);
  JitSpew(JitSpew_Codegen, "# code = %p", code->raw());
  return code;
}
