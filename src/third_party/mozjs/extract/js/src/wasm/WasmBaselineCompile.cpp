/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
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

/*
 * [SMDOC] WebAssembly baseline compiler (RabaldrMonkey)
 *
 * For now, see WasmBCClass.h for general comments about the compiler's
 * structure.
 *
 * ----------------
 *
 * General assumptions for 32-bit vs 64-bit code:
 *
 * - A 32-bit register can be extended in-place to a 64-bit register on 64-bit
 *   systems.
 *
 * - Code that knows that Register64 has a '.reg' member on 64-bit systems and
 *   '.high' and '.low' members on 32-bit systems, or knows the implications
 *   thereof, is #ifdef JS_PUNBOX64.  All other code is #if(n)?def JS_64BIT.
 *
 * Coding standards are a little fluid:
 *
 * - In "small" code generating functions (eg emitMultiplyF64, emitQuotientI32,
 *   and surrounding functions; most functions fall into this class) where the
 *   meaning is obvious:
 *
 *   Old school:
 *   - if there is a single source + destination register, it is called 'r'
 *   - if there is one source and a different destination, they are called 'rs'
 *     and 'rd'
 *   - if there is one source + destination register and another source register
 *     they are called 'r' and 'rs'
 *   - if there are two source registers and a destination register they are
 *     called 'rs0', 'rs1', and 'rd'.
 *
 *   The new thing:
 *   - what is called 'r' in the old-school naming scheme is increasingly called
 *     'rsd' in source+dest cases.
 *
 * - Generic temp registers are named /temp[0-9]?/ not /tmp[0-9]?/.
 *
 * - Registers can be named non-generically for their function ('rp' for the
 *   'pointer' register and 'rv' for the 'value' register are typical) and those
 *   names may or may not have an 'r' prefix.
 *
 * - "Larger" code generating functions make their own rules.
 */

#include "wasm/WasmBaselineCompile.h"

#include "wasm/WasmAnyRef.h"
#include "wasm/WasmBCClass.h"
#include "wasm/WasmBCDefs.h"
#include "wasm/WasmBCFrame.h"
#include "wasm/WasmBCRegDefs.h"
#include "wasm/WasmBCStk.h"
#include "wasm/WasmValType.h"

#include "jit/MacroAssembler-inl.h"
#include "wasm/WasmBCClass-inl.h"
#include "wasm/WasmBCCodegen-inl.h"
#include "wasm/WasmBCRegDefs-inl.h"
#include "wasm/WasmBCRegMgmt-inl.h"
#include "wasm/WasmBCStkMgmt-inl.h"

namespace js {
namespace wasm {

using namespace js::jit;

////////////////////////////////////////////////////////////
//
// Out of line code management.

// The baseline compiler will use OOL code more sparingly than Ion since our
// code is not high performance and frills like code density and branch
// prediction friendliness will be less important.
class OutOfLineCode : public TempObject {
 private:
  NonAssertingLabel entry_;
  NonAssertingLabel rejoin_;
  StackHeight stackHeight_;

 public:
  OutOfLineCode() : stackHeight_(StackHeight::Invalid()) {}

  Label* entry() { return &entry_; }
  Label* rejoin() { return &rejoin_; }

  void setStackHeight(StackHeight stackHeight) {
    MOZ_ASSERT(!stackHeight_.isValid());
    stackHeight_ = stackHeight;
  }

  void bind(BaseStackFrame* fr, MacroAssembler* masm) {
    MOZ_ASSERT(stackHeight_.isValid());
    masm->bind(&entry_);
    fr->setStackHeight(stackHeight_);
  }

  // The generate() method must be careful about register use because it will be
  // invoked when there is a register assignment in the BaseCompiler that does
  // not correspond to the available registers when the generated OOL code is
  // executed.  The register allocator *must not* be called.
  //
  // The best strategy is for the creator of the OOL object to allocate all
  // temps that the OOL code will need.
  //
  // Input, output, and temp registers are embedded in the OOL object and are
  // known to the code generator.
  //
  // Scratch registers are available to use in OOL code.
  //
  // All other registers must be explicitly saved and restored by the OOL code
  // before being used.

  virtual void generate(MacroAssembler* masm) = 0;
};

OutOfLineCode* BaseCompiler::addOutOfLineCode(OutOfLineCode* ool) {
  if (!ool || !outOfLine_.append(ool)) {
    return nullptr;
  }
  ool->setStackHeight(fr.stackHeight());
  return ool;
}

bool BaseCompiler::generateOutOfLineCode() {
  for (auto* ool : outOfLine_) {
    if (!ool->entry()->used()) {
      continue;
    }
    ool->bind(&fr, &masm);
    ool->generate(&masm);
  }

  return !masm.oom();
}

//////////////////////////////////////////////////////////////////////////////
//
// Sundry code generation.

bool BaseCompiler::addInterruptCheck() {
#ifdef RABALDR_PIN_INSTANCE
  Register tmp(InstanceReg);
#else
  ScratchI32 tmp(*this);
  fr.loadInstancePtr(tmp);
#endif
  Label ok;
  masm.branch32(Assembler::Equal,
                Address(tmp, wasm::Instance::offsetOfInterrupt()), Imm32(0),
                &ok);
  masm.wasmTrap(wasm::Trap::CheckInterrupt, bytecodeOffset());
  masm.bind(&ok);
  return createStackMap("addInterruptCheck");
}

void BaseCompiler::checkDivideByZero(RegI32 rhs) {
  Label nonZero;
  masm.branchTest32(Assembler::NonZero, rhs, rhs, &nonZero);
  trap(Trap::IntegerDivideByZero);
  masm.bind(&nonZero);
}

void BaseCompiler::checkDivideByZero(RegI64 r) {
  Label nonZero;
  ScratchI32 scratch(*this);
  masm.branchTest64(Assembler::NonZero, r, r, scratch, &nonZero);
  trap(Trap::IntegerDivideByZero);
  masm.bind(&nonZero);
}

void BaseCompiler::checkDivideSignedOverflow(RegI32 rhs, RegI32 srcDest,
                                             Label* done, bool zeroOnOverflow) {
  Label notMin;
  masm.branch32(Assembler::NotEqual, srcDest, Imm32(INT32_MIN), &notMin);
  if (zeroOnOverflow) {
    masm.branch32(Assembler::NotEqual, rhs, Imm32(-1), &notMin);
    moveImm32(0, srcDest);
    masm.jump(done);
  } else {
    masm.branch32(Assembler::NotEqual, rhs, Imm32(-1), &notMin);
    trap(Trap::IntegerOverflow);
  }
  masm.bind(&notMin);
}

void BaseCompiler::checkDivideSignedOverflow(RegI64 rhs, RegI64 srcDest,
                                             Label* done, bool zeroOnOverflow) {
  Label notmin;
  masm.branch64(Assembler::NotEqual, srcDest, Imm64(INT64_MIN), &notmin);
  masm.branch64(Assembler::NotEqual, rhs, Imm64(-1), &notmin);
  if (zeroOnOverflow) {
    masm.xor64(srcDest, srcDest);
    masm.jump(done);
  } else {
    trap(Trap::IntegerOverflow);
  }
  masm.bind(&notmin);
}

void BaseCompiler::jumpTable(const LabelVector& labels, Label* theTable) {
  // Flush constant pools to ensure that the table is never interrupted by
  // constant pool entries.
  masm.flush();

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
  // Prevent nop sequences to appear in the jump table.
  AutoForbidNops afn(&masm);
#endif
  masm.bind(theTable);

  for (const auto& label : labels) {
    CodeLabel cl;
    masm.writeCodePointer(&cl);
    cl.target()->bind(label.offset());
    masm.addCodeLabel(cl);
  }
}

void BaseCompiler::tableSwitch(Label* theTable, RegI32 switchValue,
                               Label* dispatchCode) {
  masm.bind(dispatchCode);

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  ScratchI32 scratch(*this);
  CodeLabel tableCl;

  masm.mov(&tableCl, scratch);

  tableCl.target()->bind(theTable->offset());
  masm.addCodeLabel(tableCl);

  masm.jmp(Operand(scratch, switchValue, ScalePointer));
#elif defined(JS_CODEGEN_ARM)
  // Flush constant pools: offset must reflect the distance from the MOV
  // to the start of the table; as the address of the MOV is given by the
  // label, nothing must come between the bind() and the ma_mov().
  AutoForbidPoolsAndNops afp(&masm,
                             /* number of instructions in scope = */ 5);

  ScratchI32 scratch(*this);

  // Compute the offset from the ma_mov instruction to the jump table.
  Label here;
  masm.bind(&here);
  uint32_t offset = here.offset() - theTable->offset();

  // Read PC+8
  masm.ma_mov(pc, scratch);

  // ARM scratch register is required by ma_sub.
  ScratchRegisterScope arm_scratch(*this);

  // Compute the absolute table base pointer into `scratch`, offset by 8
  // to account for the fact that ma_mov read PC+8.
  masm.ma_sub(Imm32(offset + 8), scratch, arm_scratch);

  // Jump indirect via table element.
  masm.ma_ldr(DTRAddr(scratch, DtrRegImmShift(switchValue, LSL, 2)), pc, Offset,
              Assembler::Always);
#elif defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
  ScratchI32 scratch(*this);
  CodeLabel tableCl;

  masm.ma_li(scratch, &tableCl);

  tableCl.target()->bind(theTable->offset());
  masm.addCodeLabel(tableCl);

  masm.branchToComputedAddress(BaseIndex(scratch, switchValue, ScalePointer));
#elif defined(JS_CODEGEN_ARM64)
  AutoForbidPoolsAndNops afp(&masm,
                             /* number of instructions in scope = */ 4);

  ScratchI32 scratch(*this);

  ARMRegister s(scratch, 64);
  ARMRegister v(switchValue, 64);
  masm.Adr(s, theTable);
  masm.Add(s, s, Operand(v, vixl::LSL, 3));
  masm.Ldr(s, MemOperand(s, 0));
  masm.Br(s);
#else
  MOZ_CRASH("BaseCompiler platform hook: tableSwitch");
#endif
}

// Helpers for accessing the "baseline scratch" areas: all targets
void BaseCompiler::stashWord(RegPtr instancePtr, size_t index, RegPtr r) {
  MOZ_ASSERT(r != instancePtr);
  MOZ_ASSERT(index < Instance::N_BASELINE_SCRATCH_WORDS);
  masm.storePtr(r,
                Address(instancePtr, Instance::offsetOfBaselineScratchWords() +
                                         index * sizeof(uintptr_t)));
}

void BaseCompiler::unstashWord(RegPtr instancePtr, size_t index, RegPtr r) {
  MOZ_ASSERT(index < Instance::N_BASELINE_SCRATCH_WORDS);
  masm.loadPtr(Address(instancePtr, Instance::offsetOfBaselineScratchWords() +
                                        index * sizeof(uintptr_t)),
               r);
}

// Helpers for accessing the "baseline scratch" areas: X86 only
#ifdef JS_CODEGEN_X86
void BaseCompiler::stashI64(RegPtr regForInstance, RegI64 r) {
  static_assert(sizeof(uintptr_t) == 4);
  MOZ_ASSERT(Instance::sizeOfBaselineScratchWords() >= 8);
  MOZ_ASSERT(regForInstance != r.low && regForInstance != r.high);
#  ifdef RABALDR_PIN_INSTANCE
#    error "Pinned instance not expected"
#  endif
  fr.loadInstancePtr(regForInstance);
  masm.store32(
      r.low, Address(regForInstance, Instance::offsetOfBaselineScratchWords()));
  masm.store32(r.high, Address(regForInstance,
                               Instance::offsetOfBaselineScratchWords() + 4));
}

void BaseCompiler::unstashI64(RegPtr regForInstance, RegI64 r) {
  static_assert(sizeof(uintptr_t) == 4);
  MOZ_ASSERT(Instance::sizeOfBaselineScratchWords() >= 8);
#  ifdef RABALDR_PIN_INSTANCE
#    error "Pinned instance not expected"
#  endif
  fr.loadInstancePtr(regForInstance);
  if (regForInstance == r.low) {
    masm.load32(
        Address(regForInstance, Instance::offsetOfBaselineScratchWords() + 4),
        r.high);
    masm.load32(
        Address(regForInstance, Instance::offsetOfBaselineScratchWords()),
        r.low);
  } else {
    masm.load32(
        Address(regForInstance, Instance::offsetOfBaselineScratchWords()),
        r.low);
    masm.load32(
        Address(regForInstance, Instance::offsetOfBaselineScratchWords() + 4),
        r.high);
  }
}
#endif

//////////////////////////////////////////////////////////////////////////////
//
// Function entry and exit

bool BaseCompiler::beginFunction() {
  AutoCreatedBy acb(masm, "(wasm)BaseCompiler::beginFunction");

  JitSpew(JitSpew_Codegen, "# ========================================");
  JitSpew(JitSpew_Codegen, "# Emitting wasm baseline code");
  JitSpew(JitSpew_Codegen,
          "# beginFunction: start of function prologue for index %d",
          (int)func_.index);

  // Make a start on the stackmap for this function.  Inspect the args so
  // as to determine which of them are both in-memory and pointer-typed, and
  // add entries to machineStackTracker as appropriate.

  ArgTypeVector args(funcType());
  size_t inboundStackArgBytes = StackArgAreaSizeUnaligned(args);
  MOZ_ASSERT(inboundStackArgBytes % sizeof(void*) == 0);
  stackMapGenerator_.numStackArgBytes = inboundStackArgBytes;

  MOZ_ASSERT(stackMapGenerator_.machineStackTracker.length() == 0);
  if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
          stackMapGenerator_.numStackArgBytes / sizeof(void*))) {
    return false;
  }

  // Identify GC-managed pointers passed on the stack.
  for (WasmABIArgIter i(args); !i.done(); i++) {
    ABIArg argLoc = *i;
    if (argLoc.kind() == ABIArg::Stack &&
        args[i.index()] == MIRType::WasmAnyRef) {
      uint32_t offset = argLoc.offsetFromArgBase();
      MOZ_ASSERT(offset < inboundStackArgBytes);
      MOZ_ASSERT(offset % sizeof(void*) == 0);
      stackMapGenerator_.machineStackTracker.setGCPointer(offset /
                                                          sizeof(void*));
    }
  }

  GenerateFunctionPrologue(
      masm, CallIndirectId::forFunc(moduleEnv_, func_.index),
      compilerEnv_.mode() == CompileMode::Tier1 ? Some(func_.index) : Nothing(),
      &offsets_);

  // GenerateFunctionPrologue pushes exactly one wasm::Frame's worth of
  // stuff, and none of the values are GC pointers.  Hence:
  if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
          sizeof(Frame) / sizeof(void*))) {
    return false;
  }

  // Initialize DebugFrame fields before the stack overflow trap so that
  // we have the invariant that all observable Frames in a debugEnabled
  // Module have valid DebugFrames.
  if (compilerEnv_.debugEnabled()) {
#ifdef JS_CODEGEN_ARM64
    static_assert(DebugFrame::offsetOfFrame() % WasmStackAlignment == 0,
                  "aligned");
#endif
    masm.reserveStack(DebugFrame::offsetOfFrame());
    if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
            DebugFrame::offsetOfFrame() / sizeof(void*))) {
      return false;
    }

    masm.store32(Imm32(func_.index), Address(masm.getStackPointer(),
                                             DebugFrame::offsetOfFuncIndex()));
    masm.store32(Imm32(0),
                 Address(masm.getStackPointer(), DebugFrame::offsetOfFlags()));

    // No need to initialize cachedReturnJSValue_ or any ref-typed spilled
    // register results, as they are traced if and only if a corresponding
    // flag (hasCachedReturnJSValue or hasSpilledRefRegisterResult) is set.
  }

  // Generate a stack-overflow check and its associated stackmap.

  fr.checkStack(ABINonArgReg0, BytecodeOffset(func_.lineOrBytecode));

  ExitStubMapVector extras;
  if (!stackMapGenerator_.generateStackmapEntriesForTrapExit(args, &extras)) {
    return false;
  }
  if (!createStackMap("stack check", extras, masm.currentOffset(),
                      HasDebugFrameWithLiveRefs::No)) {
    return false;
  }

  size_t reservedBytes = fr.fixedAllocSize() - masm.framePushed();
  MOZ_ASSERT(0 == (reservedBytes % sizeof(void*)));

  masm.reserveStack(reservedBytes);
  fr.onFixedStackAllocated();
  if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
          reservedBytes / sizeof(void*))) {
    return false;
  }

  // Locals are stack allocated.  Mark ref-typed ones in the stackmap
  // accordingly.
  for (const Local& l : localInfo_) {
    // Locals that are stack arguments were already added to the stackmap
    // before pushing the frame.
    if (l.type == MIRType::WasmAnyRef && !l.isStackArgument()) {
      uint32_t offs = fr.localOffsetFromSp(l);
      MOZ_ASSERT(0 == (offs % sizeof(void*)));
      stackMapGenerator_.machineStackTracker.setGCPointer(offs / sizeof(void*));
    }
  }

  // Copy arguments from registers to stack.
  for (WasmABIArgIter i(args); !i.done(); i++) {
    if (args.isSyntheticStackResultPointerArg(i.index())) {
      // If there are stack results and the pointer to stack results
      // was passed in a register, store it to the stack.
      if (i->argInRegister()) {
        fr.storeIncomingStackResultAreaPtr(RegPtr(i->gpr()));
      }
      // If we're in a debug frame, copy the stack result pointer arg
      // to a well-known place.
      if (compilerEnv_.debugEnabled()) {
        Register target = ABINonArgReturnReg0;
        fr.loadIncomingStackResultAreaPtr(RegPtr(target));
        size_t debugFrameOffset =
            masm.framePushed() - DebugFrame::offsetOfFrame();
        size_t debugStackResultsPointerOffset =
            debugFrameOffset + DebugFrame::offsetOfStackResultsPointer();
        masm.storePtr(target, Address(masm.getStackPointer(),
                                      debugStackResultsPointerOffset));
      }
      continue;
    }
    if (!i->argInRegister()) {
      continue;
    }
    Local& l = localInfo_[args.naturalIndex(i.index())];
    switch (i.mirType()) {
      case MIRType::Int32:
        fr.storeLocalI32(RegI32(i->gpr()), l);
        break;
      case MIRType::Int64:
        fr.storeLocalI64(RegI64(i->gpr64()), l);
        break;
      case MIRType::WasmAnyRef: {
        DebugOnly<uint32_t> offs = fr.localOffsetFromSp(l);
        MOZ_ASSERT(0 == (offs % sizeof(void*)));
        fr.storeLocalRef(RegRef(i->gpr()), l);
        // We should have just visited this local in the preceding loop.
        MOZ_ASSERT(stackMapGenerator_.machineStackTracker.isGCPointer(
            offs / sizeof(void*)));
        break;
      }
      case MIRType::Double:
        fr.storeLocalF64(RegF64(i->fpu()), l);
        break;
      case MIRType::Float32:
        fr.storeLocalF32(RegF32(i->fpu()), l);
        break;
#ifdef ENABLE_WASM_SIMD
      case MIRType::Simd128:
        fr.storeLocalV128(RegV128(i->fpu()), l);
        break;
#endif
      default:
        MOZ_CRASH("Function argument type");
    }
  }

  fr.zeroLocals(&ra);
  fr.storeInstancePtr(InstanceReg);

  if (compilerEnv_.debugEnabled()) {
    insertBreakablePoint(CallSiteDesc::EnterFrame);
    if (!createStackMap("debug: enter-frame breakpoint")) {
      return false;
    }
  }

  JitSpew(JitSpew_Codegen,
          "# beginFunction: enter body with masm.framePushed = %u",
          masm.framePushed());
  MOZ_ASSERT(stackMapGenerator_.framePushedAtEntryToBody.isNothing());
  stackMapGenerator_.framePushedAtEntryToBody.emplace(masm.framePushed());

  return true;
}

bool BaseCompiler::endFunction() {
  AutoCreatedBy acb(masm, "(wasm)BaseCompiler::endFunction");

  JitSpew(JitSpew_Codegen, "# endFunction: start of function epilogue");

  // Always branch to returnLabel_.
  masm.breakpoint();

  // Patch the add in the prologue so that it checks against the correct
  // frame size. Flush the constant pool in case it needs to be patched.
  masm.flush();

  // Precondition for patching.
  if (masm.oom()) {
    return false;
  }

  fr.patchCheckStack();

  masm.bind(&returnLabel_);

  ResultType resultType(ResultType::Vector(funcType().results()));

  popStackReturnValues(resultType);

  if (compilerEnv_.debugEnabled()) {
    // Store and reload the return value from DebugFrame::return so that
    // it can be clobbered, and/or modified by the debug trap.
    saveRegisterReturnValues(resultType);
    insertBreakablePoint(CallSiteDesc::Breakpoint);
    if (!createStackMap("debug: return-point breakpoint",
                        HasDebugFrameWithLiveRefs::Maybe)) {
      return false;
    }
    insertBreakablePoint(CallSiteDesc::LeaveFrame);
    if (!createStackMap("debug: leave-frame breakpoint",
                        HasDebugFrameWithLiveRefs::Maybe)) {
      return false;
    }
    restoreRegisterReturnValues(resultType);
  }

#ifndef RABALDR_PIN_INSTANCE
  // To satisy instance extent invariant we need to reload InstanceReg because
  // baseline can clobber it.
  fr.loadInstancePtr(InstanceReg);
#endif
  GenerateFunctionEpilogue(masm, fr.fixedAllocSize(), &offsets_);

#if defined(JS_ION_PERF)
  // FIXME - profiling code missing.  No bug for this.

  // Note the end of the inline code and start of the OOL code.
  // gen->perfSpewer().noteEndInlineCode(masm);
#endif

  JitSpew(JitSpew_Codegen, "# endFunction: end of function epilogue");
  JitSpew(JitSpew_Codegen, "# endFunction: start of OOL code");
  if (!generateOutOfLineCode()) {
    return false;
  }
  JitSpew(JitSpew_Codegen, "# endFunction: end of OOL code");

  JitSpew(JitSpew_Codegen, "# endFunction: end of OOL code");
  if (compilerEnv_.debugEnabled()) {
    JitSpew(JitSpew_Codegen, "# endFunction: start of debug trap stub");
    insertBreakpointStub();
    JitSpew(JitSpew_Codegen, "# endFunction: end of debug trap stub");
  }

  offsets_.end = masm.currentOffset();

  if (!fr.checkStackHeight()) {
    return decoder_.fail(decoder_.beginOffset(), "stack frame is too large");
  }

  JitSpew(JitSpew_Codegen, "# endFunction: end of OOL code for index %d",
          (int)func_.index);
  return !masm.oom();
}

//////////////////////////////////////////////////////////////////////////////
//
// Debugger API.

void BaseCompiler::insertBreakablePoint(CallSiteDesc::Kind kind) {
#ifndef RABALDR_PIN_INSTANCE
  fr.loadInstancePtr(InstanceReg);
#endif

  // The breakpoint code must call the breakpoint handler installed on the
  // instance if it is not null.  There is one breakable point before
  // every bytecode, and one at the beginning and at the end of the function.
  //
  // There are many constraints:
  //
  //  - Code should be read-only; we do not want to patch
  //  - The breakpoint code should be as dense as possible, given the volume of
  //    breakable points
  //  - The handler-is-null case should be as fast as we can make it
  //
  // The scratch register is available here.
  //
  // An unconditional callout would be densest but is too slow.  The best
  // balance results from an inline test for null with a conditional call.  The
  // best code sequence is platform-dependent.
  //
  // The conditional call goes to a stub attached to the function that performs
  // further filtering before calling the breakpoint handler.
#if defined(JS_CODEGEN_X64)
  // REX 83 MODRM OFFS IB
  static_assert(Instance::offsetOfDebugTrapHandler() < 128);
  masm.cmpq(Imm32(0), Operand(Address(InstanceReg,
                                      Instance::offsetOfDebugTrapHandler())));

  // 74 OFFS
  Label L;
  L.bind(masm.currentOffset() + 7);
  masm.j(Assembler::Zero, &L);

  // E8 OFFS OFFS OFFS OFFS
  masm.call(&debugTrapStub_);
  masm.append(CallSiteDesc(iter_.lastOpcodeOffset(), kind),
              CodeOffset(masm.currentOffset()));

  // Branch destination
  MOZ_ASSERT_IF(!masm.oom(), masm.currentOffset() == uint32_t(L.offset()));
#elif defined(JS_CODEGEN_X86)
  // 83 MODRM OFFS IB
  static_assert(Instance::offsetOfDebugTrapHandler() < 128);
  masm.cmpl(Imm32(0), Operand(Address(InstanceReg,
                                      Instance::offsetOfDebugTrapHandler())));

  // 74 OFFS
  Label L;
  L.bind(masm.currentOffset() + 7);
  masm.j(Assembler::Zero, &L);

  // E8 OFFS OFFS OFFS OFFS
  masm.call(&debugTrapStub_);
  masm.append(CallSiteDesc(iter_.lastOpcodeOffset(), kind),
              CodeOffset(masm.currentOffset()));

  // Branch destination
  MOZ_ASSERT_IF(!masm.oom(), masm.currentOffset() == uint32_t(L.offset()));
#elif defined(JS_CODEGEN_ARM64)
  ScratchPtr scratch(*this);
  ARMRegister tmp(scratch, 64);
  Label L;
  masm.Ldr(tmp, MemOperand(Address(InstanceReg,
                                   Instance::offsetOfDebugTrapHandler())));
  masm.Cbz(tmp, &L);
  masm.Bl(&debugTrapStub_);
  masm.append(CallSiteDesc(iter_.lastOpcodeOffset(), kind),
              CodeOffset(masm.currentOffset()));
  masm.bind(&L);
#elif defined(JS_CODEGEN_ARM)
  ScratchPtr scratch(*this);
  masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugTrapHandler()),
               scratch);
  masm.ma_orr(scratch, scratch, SetCC);
  masm.ma_bl(&debugTrapStub_, Assembler::NonZero);
  masm.append(CallSiteDesc(iter_.lastOpcodeOffset(), kind),
              CodeOffset(masm.currentOffset()));
#elif defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_RISCV64)
  ScratchPtr scratch(*this);
  Label L;
  masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugTrapHandler()),
               scratch);
  masm.branchPtr(Assembler::Equal, scratch, ImmWord(0), &L);
  masm.call(&debugTrapStub_);
  masm.append(CallSiteDesc(iter_.lastOpcodeOffset(), kind),
              CodeOffset(masm.currentOffset()));
  masm.bind(&L);
#else
  MOZ_CRASH("BaseCompiler platform hook: insertBreakablePoint");
#endif
}

void BaseCompiler::insertBreakpointStub() {
  // The debug trap stub performs out-of-line filtering before jumping to the
  // debug trap handler if necessary.  The trap handler returns directly to
  // the breakable point.
  //
  // NOTE, the link register is live here on platforms that have LR.
  //
  // The scratch register is available here (as it was at the call site).
  //
  // It's useful for the debug trap stub to be compact, as every function gets
  // one.

  Label L;
  masm.bind(&debugTrapStub_);

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  {
    ScratchPtr scratch(*this);

    // Get the per-instance table of filtering bits.
    masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugFilter()),
                 scratch);

    // Check the filter bit.  There is one bit per function in the module.
    // Table elements are 32-bit because the masm makes that convenient.
    masm.branchTest32(Assembler::NonZero, Address(scratch, func_.index / 32),
                      Imm32(1 << (func_.index % 32)), &L);

    // Fast path: return to the execution.
    masm.ret();
  }
#elif defined(JS_CODEGEN_ARM64)
  {
    ScratchPtr scratch(*this);

    // Logic as above, except abiret to jump to the LR directly
    masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugFilter()),
                 scratch);
    masm.branchTest32(Assembler::NonZero, Address(scratch, func_.index / 32),
                      Imm32(1 << (func_.index % 32)), &L);
    masm.abiret();
  }
#elif defined(JS_CODEGEN_ARM)
  {
    // We must be careful not to use the SecondScratchRegister, which usually
    // is LR, as LR is live here.  This means avoiding masm abstractions such
    // as branchTest32.

    static_assert(ScratchRegister != lr);
    static_assert(Instance::offsetOfDebugFilter() < 0x1000);

    ScratchRegisterScope tmp1(masm);
    ScratchI32 tmp2(*this);
    masm.ma_ldr(
        DTRAddr(InstanceReg, DtrOffImm(Instance::offsetOfDebugFilter())), tmp1);
    masm.ma_mov(Imm32(func_.index / 32), tmp2);
    masm.ma_ldr(DTRAddr(tmp1, DtrRegImmShift(tmp2, LSL, 0)), tmp2);
    masm.ma_tst(tmp2, Imm32(1 << func_.index % 32), tmp1, Assembler::Always);
    masm.ma_bx(lr, Assembler::Zero);
  }
#elif defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_RISCV64)
  {
    ScratchPtr scratch(*this);

    // Logic same as ARM64.
    masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugFilter()),
                 scratch);
    masm.branchTest32(Assembler::NonZero, Address(scratch, func_.index / 32),
                      Imm32(1 << (func_.index % 32)), &L);
    masm.abiret();
  }
#else
  MOZ_CRASH("BaseCompiler platform hook: endFunction");
#endif

  // Jump to the debug trap handler.
  masm.bind(&L);
  masm.jump(Address(InstanceReg, Instance::offsetOfDebugTrapHandler()));
}

void BaseCompiler::saveRegisterReturnValues(const ResultType& resultType) {
  MOZ_ASSERT(compilerEnv_.debugEnabled());
  size_t debugFrameOffset = masm.framePushed() - DebugFrame::offsetOfFrame();
  size_t registerResultIdx = 0;
  for (ABIResultIter i(resultType); !i.done(); i.next()) {
    const ABIResult result = i.cur();
    if (!result.inRegister()) {
#ifdef DEBUG
      for (i.next(); !i.done(); i.next()) {
        MOZ_ASSERT(!i.cur().inRegister());
      }
#endif
      break;
    }

    size_t resultOffset = DebugFrame::offsetOfRegisterResult(registerResultIdx);
    Address dest(masm.getStackPointer(), debugFrameOffset + resultOffset);
    switch (result.type().kind()) {
      case ValType::I32:
        masm.store32(RegI32(result.gpr()), dest);
        break;
      case ValType::I64:
        masm.store64(RegI64(result.gpr64()), dest);
        break;
      case ValType::F64:
        masm.storeDouble(RegF64(result.fpr()), dest);
        break;
      case ValType::F32:
        masm.storeFloat32(RegF32(result.fpr()), dest);
        break;
      case ValType::Ref: {
        uint32_t flag =
            DebugFrame::hasSpilledRegisterRefResultBitMask(registerResultIdx);
        // Tell Instance::traceFrame that we have a pointer to trace.
        masm.or32(Imm32(flag),
                  Address(masm.getStackPointer(),
                          debugFrameOffset + DebugFrame::offsetOfFlags()));
        masm.storePtr(RegRef(result.gpr()), dest);
        break;
      }
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        masm.storeUnalignedSimd128(RegV128(result.fpr()), dest);
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
    }
    registerResultIdx++;
  }
}

void BaseCompiler::restoreRegisterReturnValues(const ResultType& resultType) {
  MOZ_ASSERT(compilerEnv_.debugEnabled());
  size_t debugFrameOffset = masm.framePushed() - DebugFrame::offsetOfFrame();
  size_t registerResultIdx = 0;
  for (ABIResultIter i(resultType); !i.done(); i.next()) {
    const ABIResult result = i.cur();
    if (!result.inRegister()) {
#ifdef DEBUG
      for (i.next(); !i.done(); i.next()) {
        MOZ_ASSERT(!i.cur().inRegister());
      }
#endif
      break;
    }
    size_t resultOffset =
        DebugFrame::offsetOfRegisterResult(registerResultIdx++);
    Address src(masm.getStackPointer(), debugFrameOffset + resultOffset);
    switch (result.type().kind()) {
      case ValType::I32:
        masm.load32(src, RegI32(result.gpr()));
        break;
      case ValType::I64:
        masm.load64(src, RegI64(result.gpr64()));
        break;
      case ValType::F64:
        masm.loadDouble(src, RegF64(result.fpr()));
        break;
      case ValType::F32:
        masm.loadFloat32(src, RegF32(result.fpr()));
        break;
      case ValType::Ref:
        masm.loadPtr(src, RegRef(result.gpr()));
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        masm.loadUnalignedSimd128(src, RegV128(result.fpr()));
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
    }
  }
}

//////////////////////////////////////////////////////////////////////////////
//
// Results and block parameters

void BaseCompiler::popStackReturnValues(const ResultType& resultType) {
  uint32_t bytes = ABIResultIter::MeasureStackBytes(resultType);
  if (bytes == 0) {
    return;
  }
  Register target = ABINonArgReturnReg0;
  Register temp = ABINonArgReturnReg1;
  fr.loadIncomingStackResultAreaPtr(RegPtr(target));
  fr.popStackResultsToMemory(target, bytes, temp);
}

// TODO / OPTIMIZE (Bug 1316818): At the moment we use the Wasm
// inter-procedure ABI for block returns, which allocates ReturnReg as the
// single block result register.  It is possible other choices would lead to
// better register allocation, as ReturnReg is often first in the register set
// and will be heavily wanted by the register allocator that uses takeFirst().
//
// Obvious options:
//  - pick a register at the back of the register set
//  - pick a random register per block (different blocks have
//    different join regs)

void BaseCompiler::popRegisterResults(ABIResultIter& iter) {
  // Pop register results.  Note that in the single-value case, popping to a
  // register may cause a sync(); for multi-value we sync'd already.
  for (; !iter.done(); iter.next()) {
    const ABIResult& result = iter.cur();
    if (!result.inRegister()) {
      // TODO / OPTIMIZE: We sync here to avoid solving the general parallel
      // move problem in popStackResults.  However we could avoid syncing the
      // values that are going to registers anyway, if they are already in
      // registers.
      sync();
      break;
    }
    switch (result.type().kind()) {
      case ValType::I32:
        popI32(RegI32(result.gpr()));
        break;
      case ValType::I64:
        popI64(RegI64(result.gpr64()));
        break;
      case ValType::F32:
        popF32(RegF32(result.fpr()));
        break;
      case ValType::F64:
        popF64(RegF64(result.fpr()));
        break;
      case ValType::Ref:
        popRef(RegRef(result.gpr()));
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        popV128(RegV128(result.fpr()));
#else
        MOZ_CRASH("No SIMD support");
#endif
    }
  }
}

void BaseCompiler::popStackResults(ABIResultIter& iter, StackHeight stackBase) {
  MOZ_ASSERT(!iter.done());

  // The iterator should be advanced beyond register results, and register
  // results should be popped already from the value stack.
  uint32_t alreadyPopped = iter.index();

  // At this point, only stack arguments are remaining.  Iterate through them
  // to measure how much stack space they will take up.
  for (; !iter.done(); iter.next()) {
    MOZ_ASSERT(iter.cur().onStack());
  }

  // Calculate the space needed to store stack results, in bytes.
  uint32_t stackResultBytes = iter.stackBytesConsumedSoFar();
  MOZ_ASSERT(stackResultBytes);

  // Compute the stack height including the stack results.  Note that it's
  // possible that this call expands the stack, for example if some of the
  // results are supplied by constants and so are not already on the machine
  // stack.
  uint32_t endHeight = fr.prepareStackResultArea(stackBase, stackResultBytes);

  // Find a free GPR to use when shuffling stack values.  If none is
  // available, push ReturnReg and restore it after we're done.
  bool saved = false;
  RegPtr temp = ra.needTempPtr(RegPtr(ReturnReg), &saved);

  // The sequence of Stk values is in the same order on the machine stack as
  // the result locations, but there is a complication: constant values are
  // not actually pushed on the machine stack.  (At this point registers and
  // locals have been spilled already.)  So, moving the Stk values into place
  // isn't simply a shuffle-down or shuffle-up operation.  There is a part of
  // the Stk sequence that shuffles toward the FP, a part that's already in
  // place, and a part that shuffles toward the SP.  After shuffling, we have
  // to materialize the constants.

  // Shuffle mem values toward the frame pointer, copying deepest values
  // first.  Stop when we run out of results, get to a register result, or
  // find a Stk value that is closer to the FP than the result.
  for (iter.switchToPrev(); !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    if (!result.onStack()) {
      break;
    }
    MOZ_ASSERT(result.stackOffset() < stackResultBytes);
    uint32_t destHeight = endHeight - result.stackOffset();
    uint32_t stkBase = stk_.length() - (iter.count() - alreadyPopped);
    Stk& v = stk_[stkBase + iter.index()];
    if (v.isMem()) {
      uint32_t srcHeight = v.offs();
      if (srcHeight <= destHeight) {
        break;
      }
      fr.shuffleStackResultsTowardFP(srcHeight, destHeight, result.size(),
                                     temp);
    }
  }

  // Reset iterator and skip register results.
  for (iter.reset(); !iter.done(); iter.next()) {
    if (iter.cur().onStack()) {
      break;
    }
  }

  // Revisit top stack values, shuffling mem values toward the stack pointer,
  // copying shallowest values first.
  for (; !iter.done(); iter.next()) {
    const ABIResult& result = iter.cur();
    MOZ_ASSERT(result.onStack());
    MOZ_ASSERT(result.stackOffset() < stackResultBytes);
    uint32_t destHeight = endHeight - result.stackOffset();
    Stk& v = stk_[stk_.length() - (iter.index() - alreadyPopped) - 1];
    if (v.isMem()) {
      uint32_t srcHeight = v.offs();
      if (srcHeight >= destHeight) {
        break;
      }
      fr.shuffleStackResultsTowardSP(srcHeight, destHeight, result.size(),
                                     temp);
    }
  }

  // Reset iterator and skip register results, which are already popped off
  // the value stack.
  for (iter.reset(); !iter.done(); iter.next()) {
    if (iter.cur().onStack()) {
      break;
    }
  }

  // Materialize constants and pop the remaining items from the value stack.
  for (; !iter.done(); iter.next()) {
    const ABIResult& result = iter.cur();
    uint32_t resultHeight = endHeight - result.stackOffset();
    Stk& v = stk_.back();
    switch (v.kind()) {
      case Stk::ConstI32:
#if defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
        fr.storeImmediatePtrToStack(v.i32val_, resultHeight, temp);
#else
        fr.storeImmediatePtrToStack(uint32_t(v.i32val_), resultHeight, temp);
#endif
        break;
      case Stk::ConstF32:
        fr.storeImmediateF32ToStack(v.f32val_, resultHeight, temp);
        break;
      case Stk::ConstI64:
        fr.storeImmediateI64ToStack(v.i64val_, resultHeight, temp);
        break;
      case Stk::ConstF64:
        fr.storeImmediateF64ToStack(v.f64val_, resultHeight, temp);
        break;
#ifdef ENABLE_WASM_SIMD
      case Stk::ConstV128:
        fr.storeImmediateV128ToStack(v.v128val_, resultHeight, temp);
        break;
#endif
      case Stk::ConstRef:
        fr.storeImmediatePtrToStack(v.refval_, resultHeight, temp);
        break;
      case Stk::MemRef:
        // Update bookkeeping as we pop the Stk entry.
        stackMapGenerator_.memRefsOnStk--;
        break;
      default:
        MOZ_ASSERT(v.isMem());
        break;
    }
    stk_.popBack();
  }

  ra.freeTempPtr(temp, saved);

  // This will pop the stack if needed.
  fr.finishStackResultArea(stackBase, stackResultBytes);
}

void BaseCompiler::popBlockResults(ResultType type, StackHeight stackBase,
                                   ContinuationKind kind) {
  if (!type.empty()) {
    ABIResultIter iter(type);
    popRegisterResults(iter);
    if (!iter.done()) {
      popStackResults(iter, stackBase);
      // Because popStackResults might clobber the stack, it leaves the stack
      // pointer already in the right place for the continuation, whether the
      // continuation is a jump or fallthrough.
      return;
    }
  }
  // We get here if there are no stack results.  For a fallthrough, the stack
  // is already at the right height.  For a jump, we may need to pop the stack
  // pointer if the continuation's stack height is lower than the current
  // stack height.
  if (kind == ContinuationKind::Jump) {
    fr.popStackBeforeBranch(stackBase, type);
  }
}

// This function is similar to popBlockResults, but additionally handles the
// implicit exception pointer that is pushed to the value stack on entry to
// a catch handler by dropping it appropriately.
void BaseCompiler::popCatchResults(ResultType type, StackHeight stackBase) {
  if (!type.empty()) {
    ABIResultIter iter(type);
    popRegisterResults(iter);
    if (!iter.done()) {
      popStackResults(iter, stackBase);
      // Since popStackResults clobbers the stack, we only need to free the
      // exception off of the value stack.
      popValueStackBy(1);
    } else {
      // If there are no stack results, we have to adjust the stack by
      // dropping the exception reference that's now on the stack.
      dropValue();
    }
  } else {
    dropValue();
  }
  fr.popStackBeforeBranch(stackBase, type);
}

Stk BaseCompiler::captureStackResult(const ABIResult& result,
                                     StackHeight resultsBase,
                                     uint32_t stackResultBytes) {
  MOZ_ASSERT(result.onStack());
  uint32_t offs = fr.locateStackResult(result, resultsBase, stackResultBytes);
  return Stk::StackResult(result.type(), offs);
}

// TODO: It may be fruitful to inline the fast path here, as it will be common.

bool BaseCompiler::pushResults(ResultType type, StackHeight resultsBase) {
  if (type.empty()) {
    return true;
  }

  if (type.length() > 1) {
    // Reserve extra space on the stack for all the values we'll push.
    // Multi-value push is not accounted for by the pre-sizing of the stack in
    // the decoding loop.
    //
    // Also make sure we leave headroom for other pushes that will occur after
    // pushing results, just to be safe.
    if (!stk_.reserve(stk_.length() + type.length() + MaxPushesPerOpcode)) {
      return false;
    }
  }

  // We need to push the results in reverse order, so first iterate through
  // all results to determine the locations of stack result types.
  ABIResultIter iter(type);
  while (!iter.done()) {
    iter.next();
  }
  uint32_t stackResultBytes = iter.stackBytesConsumedSoFar();
  for (iter.switchToPrev(); !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    if (!result.onStack()) {
      break;
    }
    Stk v = captureStackResult(result, resultsBase, stackResultBytes);
    push(v);
    if (v.kind() == Stk::MemRef) {
      stackMapGenerator_.memRefsOnStk++;
    }
  }

  for (; !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    MOZ_ASSERT(result.inRegister());
    switch (result.type().kind()) {
      case ValType::I32:
        pushI32(RegI32(result.gpr()));
        break;
      case ValType::I64:
        pushI64(RegI64(result.gpr64()));
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        pushV128(RegV128(result.fpr()));
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
      case ValType::F32:
        pushF32(RegF32(result.fpr()));
        break;
      case ValType::F64:
        pushF64(RegF64(result.fpr()));
        break;
      case ValType::Ref:
        pushRef(RegRef(result.gpr()));
        break;
    }
  }

  return true;
}

bool BaseCompiler::pushBlockResults(ResultType type) {
  return pushResults(type, controlItem().stackHeight);
}

// A combination of popBlockResults + pushBlockResults, used when entering a
// block with a control-flow join (loops) or split (if) to shuffle the
// fallthrough block parameters into the locations expected by the
// continuation.
bool BaseCompiler::topBlockParams(ResultType type) {
  // This function should only be called when entering a block with a
  // control-flow join at the entry, where there are no live temporaries in
  // the current block.
  StackHeight base = controlItem().stackHeight;
  MOZ_ASSERT(fr.stackResultsBase(stackConsumed(type.length())) == base);
  popBlockResults(type, base, ContinuationKind::Fallthrough);
  return pushBlockResults(type);
}

// A combination of popBlockResults + pushBlockResults, used before branches
// where we don't know the target (br_if / br_table).  If and when the branch
// is taken, the stack results will be shuffled down into place.  For br_if
// that has fallthrough, the parameters for the untaken branch flow through to
// the continuation.
bool BaseCompiler::topBranchParams(ResultType type, StackHeight* height) {
  if (type.empty()) {
    *height = fr.stackHeight();
    return true;
  }
  // There may be temporary values that need spilling; delay computation of
  // the stack results base until after the popRegisterResults(), which spills
  // if needed.
  ABIResultIter iter(type);
  popRegisterResults(iter);
  StackHeight base = fr.stackResultsBase(stackConsumed(iter.remaining()));
  if (!iter.done()) {
    popStackResults(iter, base);
  }
  if (!pushResults(type, base)) {
    return false;
  }
  *height = base;
  return true;
}

// Conditional branches with fallthrough are preceded by a topBranchParams, so
// we know that there are no stack results that need to be materialized.  In
// that case, we can just shuffle the whole block down before popping the
// stack.
void BaseCompiler::shuffleStackResultsBeforeBranch(StackHeight srcHeight,
                                                   StackHeight destHeight,
                                                   ResultType type) {
  uint32_t stackResultBytes = 0;

  if (ABIResultIter::HasStackResults(type)) {
    MOZ_ASSERT(stk_.length() >= type.length());
    ABIResultIter iter(type);
    for (; !iter.done(); iter.next()) {
#ifdef DEBUG
      const ABIResult& result = iter.cur();
      const Stk& v = stk_[stk_.length() - iter.index() - 1];
      MOZ_ASSERT(v.isMem() == result.onStack());
#endif
    }

    stackResultBytes = iter.stackBytesConsumedSoFar();
    MOZ_ASSERT(stackResultBytes > 0);

    if (srcHeight != destHeight) {
      // Find a free GPR to use when shuffling stack values.  If none
      // is available, push ReturnReg and restore it after we're done.
      bool saved = false;
      RegPtr temp = ra.needTempPtr(RegPtr(ReturnReg), &saved);
      fr.shuffleStackResultsTowardFP(srcHeight, destHeight, stackResultBytes,
                                     temp);
      ra.freeTempPtr(temp, saved);
    }
  }

  fr.popStackBeforeBranch(destHeight, stackResultBytes);
}

bool BaseCompiler::insertDebugCollapseFrame() {
  if (!compilerEnv_.debugEnabled()) {
    return true;
  }
  insertBreakablePoint(CallSiteDesc::CollapseFrame);
  return createStackMap("debug: collapse-frame breakpoint",
                        HasDebugFrameWithLiveRefs::Maybe);
}

//////////////////////////////////////////////////////////////////////////////
//
// Function calls.

void BaseCompiler::beginCall(
    FunctionCall& call, UseABI useABI,
    RestoreRegisterStateAndRealm restoreRegisterStateAndRealm) {
  MOZ_ASSERT_IF(
      useABI == UseABI::Builtin,
      restoreRegisterStateAndRealm == RestoreRegisterStateAndRealm::False);

  call.restoreRegisterStateAndRealm =
      restoreRegisterStateAndRealm == RestoreRegisterStateAndRealm::True;
  call.usesSystemAbi = useABI == UseABI::System;

  if (call.usesSystemAbi) {
    // Call-outs need to use the appropriate system ABI.
#if defined(JS_CODEGEN_ARM)
    call.hardFP = UseHardFpABI();
    call.abi.setUseHardFp(call.hardFP);
#endif
  } else {
#if defined(JS_CODEGEN_ARM)
    MOZ_ASSERT(call.hardFP, "All private ABIs pass FP arguments in registers");
#endif
  }

  // Use masm.framePushed() because the value we want here does not depend
  // on the height of the frame's stack area, but the actual size of the
  // allocated frame.
  call.frameAlignAdjustment = ComputeByteAlignment(
      masm.framePushed() + sizeof(Frame), JitStackAlignment);
}

void BaseCompiler::endCall(FunctionCall& call, size_t stackSpace) {
  size_t adjustment = call.stackArgAreaSize + call.frameAlignAdjustment;
  fr.freeArgAreaAndPopBytes(adjustment, stackSpace);

  MOZ_ASSERT(stackMapGenerator_.framePushedExcludingOutboundCallArgs.isSome());
  stackMapGenerator_.framePushedExcludingOutboundCallArgs.reset();

  if (call.restoreRegisterStateAndRealm) {
    // The instance has been clobbered, so always reload
    fr.loadInstancePtr(InstanceReg);
    masm.loadWasmPinnedRegsFromInstance();
    masm.switchToWasmInstanceRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);
  } else if (call.usesSystemAbi) {
    // On x86 there are no pinned registers, so don't waste time
    // reloading the instance.
#ifndef JS_CODEGEN_X86
    // The instance has been clobbered, so always reload
    fr.loadInstancePtr(InstanceReg);
    masm.loadWasmPinnedRegsFromInstance();
#endif
  }
}

void BaseCompiler::startCallArgs(size_t stackArgAreaSizeUnaligned,
                                 FunctionCall* call) {
  size_t stackArgAreaSizeAligned =
      AlignStackArgAreaSize(stackArgAreaSizeUnaligned);
  MOZ_ASSERT(stackArgAreaSizeUnaligned <= stackArgAreaSizeAligned);

  // Record the masm.framePushed() value at this point, before we push args
  // for the call and any required alignment space. This defines the lower limit
  // of the stackmap that will be created for this call.
  MOZ_ASSERT(
      stackMapGenerator_.framePushedExcludingOutboundCallArgs.isNothing());
  stackMapGenerator_.framePushedExcludingOutboundCallArgs.emplace(
      // However much we've pushed so far
      masm.framePushed() +
      // Extra space we'll push to get the frame aligned
      call->frameAlignAdjustment);

  call->stackArgAreaSize = stackArgAreaSizeAligned;

  size_t adjustment = call->stackArgAreaSize + call->frameAlignAdjustment;
  fr.allocArgArea(adjustment);
}

ABIArg BaseCompiler::reservePointerArgument(FunctionCall* call) {
  return call->abi.next(MIRType::Pointer);
}

// TODO / OPTIMIZE (Bug 1316821): Note passArg is used only in one place.
// (Or it was, until Luke wandered through, but that can be fixed again.)
// I'm not saying we should manually inline it, but we could hoist the
// dispatch into the caller and have type-specific implementations of
// passArg: passArgI32(), etc.  Then those might be inlined, at least in PGO
// builds.
//
// The bulk of the work here (60%) is in the next() call, though.
//
// Notably, since next() is so expensive, StackArgAreaSizeUnaligned()
// becomes expensive too.
//
// Somehow there could be a trick here where the sequence of argument types
// (read from the input stream) leads to a cached entry for
// StackArgAreaSizeUnaligned() and for how to pass arguments...
//
// But at least we could reduce the cost of StackArgAreaSizeUnaligned() by
// first reading the argument types into a (reusable) vector, then we have
// the outgoing size at low cost, and then we can pass args based on the
// info we read.

void BaseCompiler::passArg(ValType type, const Stk& arg, FunctionCall* call) {
  switch (type.kind()) {
    case ValType::I32: {
      ABIArg argLoc = call->abi.next(MIRType::Int32);
      if (argLoc.kind() == ABIArg::Stack) {
        ScratchI32 scratch(*this);
        loadI32(arg, scratch);
        masm.store32(scratch, Address(masm.getStackPointer(),
                                      argLoc.offsetFromArgBase()));
      } else {
        loadI32(arg, RegI32(argLoc.gpr()));
      }
      break;
    }
    case ValType::I64: {
      ABIArg argLoc = call->abi.next(MIRType::Int64);
      if (argLoc.kind() == ABIArg::Stack) {
        ScratchI32 scratch(*this);
#ifdef JS_PUNBOX64
        loadI64(arg, fromI32(scratch));
        masm.storePtr(scratch, Address(masm.getStackPointer(),
                                       argLoc.offsetFromArgBase()));
#else
        loadI64Low(arg, scratch);
        masm.store32(scratch, LowWord(Address(masm.getStackPointer(),
                                              argLoc.offsetFromArgBase())));
        loadI64High(arg, scratch);
        masm.store32(scratch, HighWord(Address(masm.getStackPointer(),
                                               argLoc.offsetFromArgBase())));
#endif
      } else {
        loadI64(arg, RegI64(argLoc.gpr64()));
      }
      break;
    }
    case ValType::V128: {
#ifdef ENABLE_WASM_SIMD
      ABIArg argLoc = call->abi.next(MIRType::Simd128);
      switch (argLoc.kind()) {
        case ABIArg::Stack: {
          ScratchV128 scratch(*this);
          loadV128(arg, scratch);
          masm.storeUnalignedSimd128(
              (RegV128)scratch,
              Address(masm.getStackPointer(), argLoc.offsetFromArgBase()));
          break;
        }
        case ABIArg::GPR: {
          MOZ_CRASH("Unexpected parameter passing discipline");
        }
        case ABIArg::FPU: {
          loadV128(arg, RegV128(argLoc.fpu()));
          break;
        }
#  if defined(JS_CODEGEN_REGISTER_PAIR)
        case ABIArg::GPR_PAIR: {
          MOZ_CRASH("Unexpected parameter passing discipline");
        }
#  endif
        case ABIArg::Uninitialized:
          MOZ_CRASH("Uninitialized ABIArg kind");
      }
      break;
#else
      MOZ_CRASH("No SIMD support");
#endif
    }
    case ValType::F64: {
      ABIArg argLoc = call->abi.next(MIRType::Double);
      switch (argLoc.kind()) {
        case ABIArg::Stack: {
          ScratchF64 scratch(*this);
          loadF64(arg, scratch);
          masm.storeDouble(scratch, Address(masm.getStackPointer(),
                                            argLoc.offsetFromArgBase()));
          break;
        }
#if defined(JS_CODEGEN_REGISTER_PAIR)
        case ABIArg::GPR_PAIR: {
#  if defined(JS_CODEGEN_ARM)
          ScratchF64 scratch(*this);
          loadF64(arg, scratch);
          masm.ma_vxfer(scratch, argLoc.evenGpr(), argLoc.oddGpr());
          break;
#  else
          MOZ_CRASH("BaseCompiler platform hook: passArg F64 pair");
#  endif
        }
#endif
        case ABIArg::FPU: {
          loadF64(arg, RegF64(argLoc.fpu()));
          break;
        }
        case ABIArg::GPR: {
          MOZ_CRASH("Unexpected parameter passing discipline");
        }
        case ABIArg::Uninitialized:
          MOZ_CRASH("Uninitialized ABIArg kind");
      }
      break;
    }
    case ValType::F32: {
      ABIArg argLoc = call->abi.next(MIRType::Float32);
      switch (argLoc.kind()) {
        case ABIArg::Stack: {
          ScratchF32 scratch(*this);
          loadF32(arg, scratch);
          masm.storeFloat32(scratch, Address(masm.getStackPointer(),
                                             argLoc.offsetFromArgBase()));
          break;
        }
        case ABIArg::GPR: {
          ScratchF32 scratch(*this);
          loadF32(arg, scratch);
          masm.moveFloat32ToGPR(scratch, argLoc.gpr());
          break;
        }
        case ABIArg::FPU: {
          loadF32(arg, RegF32(argLoc.fpu()));
          break;
        }
#if defined(JS_CODEGEN_REGISTER_PAIR)
        case ABIArg::GPR_PAIR: {
          MOZ_CRASH("Unexpected parameter passing discipline");
        }
#endif
        case ABIArg::Uninitialized:
          MOZ_CRASH("Uninitialized ABIArg kind");
      }
      break;
    }
    case ValType::Ref: {
      ABIArg argLoc = call->abi.next(MIRType::WasmAnyRef);
      if (argLoc.kind() == ABIArg::Stack) {
        ScratchRef scratch(*this);
        loadRef(arg, scratch);
        masm.storePtr(scratch, Address(masm.getStackPointer(),
                                       argLoc.offsetFromArgBase()));
      } else {
        loadRef(arg, RegRef(argLoc.gpr()));
      }
      break;
    }
  }
}

CodeOffset BaseCompiler::callDefinition(uint32_t funcIndex,
                                        const FunctionCall& call) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteDesc::Func);
  return masm.call(desc, funcIndex);
}

CodeOffset BaseCompiler::callSymbolic(SymbolicAddress callee,
                                      const FunctionCall& call) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteDesc::Symbolic);
  return masm.call(desc, callee);
}

// Precondition: sync()

class OutOfLineAbortingTrap : public OutOfLineCode {
  Trap trap_;
  BytecodeOffset off_;

 public:
  OutOfLineAbortingTrap(Trap trap, BytecodeOffset off)
      : trap_(trap), off_(off) {}

  virtual void generate(MacroAssembler* masm) override {
    masm->wasmTrap(trap_, off_);
    MOZ_ASSERT(!rejoin()->bound());
  }
};

#ifdef ENABLE_WASM_TAIL_CALLS
static ReturnCallAdjustmentInfo BuildReturnCallAdjustmentInfo(
    const FuncType& callerType, const FuncType& calleeType) {
  return ReturnCallAdjustmentInfo(
      StackArgAreaSizeUnaligned(ArgTypeVector(calleeType)),
      StackArgAreaSizeUnaligned(ArgTypeVector(callerType)));
}
#endif

bool BaseCompiler::callIndirect(uint32_t funcTypeIndex, uint32_t tableIndex,
                                const Stk& indexVal, const FunctionCall& call,
                                bool tailCall, CodeOffset* fastCallOffset,
                                CodeOffset* slowCallOffset) {
  CallIndirectId callIndirectId =
      CallIndirectId::forFuncType(moduleEnv_, funcTypeIndex);
  MOZ_ASSERT(callIndirectId.kind() != CallIndirectIdKind::AsmJS);

  const TableDesc& table = moduleEnv_.tables[tableIndex];

  loadI32(indexVal, RegI32(WasmTableCallIndexReg));

  CallSiteDesc desc(bytecodeOffset(), CallSiteDesc::Indirect);
  CalleeDesc callee =
      CalleeDesc::wasmTable(moduleEnv_, table, tableIndex, callIndirectId);
  OutOfLineCode* oob = addOutOfLineCode(
      new (alloc_) OutOfLineAbortingTrap(Trap::OutOfBounds, bytecodeOffset()));
  if (!oob) {
    return false;
  }
  Label* nullCheckFailed = nullptr;
#ifndef WASM_HAS_HEAPREG
  OutOfLineCode* nullref = addOutOfLineCode(new (alloc_) OutOfLineAbortingTrap(
      Trap::IndirectCallToNull, bytecodeOffset()));
  if (!nullref) {
    return false;
  }
  nullCheckFailed = nullref->entry();
#endif
  if (!tailCall) {
    masm.wasmCallIndirect(desc, callee, oob->entry(), nullCheckFailed,
                          mozilla::Nothing(), fastCallOffset, slowCallOffset);
  } else {
#ifdef ENABLE_WASM_TAIL_CALLS
    ReturnCallAdjustmentInfo retCallInfo = BuildReturnCallAdjustmentInfo(
        this->funcType(), (*moduleEnv_.types)[funcTypeIndex].funcType());
    masm.wasmReturnCallIndirect(desc, callee, oob->entry(), nullCheckFailed,
                                mozilla::Nothing(), retCallInfo);
#else
    MOZ_CRASH("not available");
#endif
  }
  return true;
}

#ifdef ENABLE_WASM_GC
void BaseCompiler::callRef(const Stk& calleeRef, const FunctionCall& call,
                           CodeOffset* fastCallOffset,
                           CodeOffset* slowCallOffset) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteDesc::FuncRef);
  CalleeDesc callee = CalleeDesc::wasmFuncRef();

  loadRef(calleeRef, RegRef(WasmCallRefReg));
  masm.wasmCallRef(desc, callee, fastCallOffset, slowCallOffset);
}

#  ifdef ENABLE_WASM_TAIL_CALLS
void BaseCompiler::returnCallRef(const Stk& calleeRef, const FunctionCall& call,
                                 const FuncType* funcType) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteDesc::FuncRef);
  CalleeDesc callee = CalleeDesc::wasmFuncRef();

  loadRef(calleeRef, RegRef(WasmCallRefReg));
  ReturnCallAdjustmentInfo retCallInfo =
      BuildReturnCallAdjustmentInfo(this->funcType(), *funcType);
  masm.wasmReturnCallRef(desc, callee, retCallInfo);
}
#  endif

#endif

// Precondition: sync()

CodeOffset BaseCompiler::callImport(unsigned instanceDataOffset,
                                    const FunctionCall& call) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteDesc::Import);
  CalleeDesc callee = CalleeDesc::import(instanceDataOffset);
  return masm.wasmCallImport(desc, callee);
}

CodeOffset BaseCompiler::builtinCall(SymbolicAddress builtin,
                                     const FunctionCall& call) {
  return callSymbolic(builtin, call);
}

CodeOffset BaseCompiler::builtinInstanceMethodCall(
    const SymbolicAddressSignature& builtin, const ABIArg& instanceArg,
    const FunctionCall& call) {
#ifndef RABALDR_PIN_INSTANCE
  // Builtin method calls assume the instance register has been set.
  fr.loadInstancePtr(InstanceReg);
#endif
  CallSiteDesc desc(bytecodeOffset(), CallSiteDesc::Symbolic);
  return masm.wasmCallBuiltinInstanceMethod(desc, instanceArg, builtin.identity,
                                            builtin.failureMode);
}

bool BaseCompiler::pushCallResults(const FunctionCall& call, ResultType type,
                                   const StackResultsLoc& loc) {
#if defined(JS_CODEGEN_ARM)
  // pushResults currently bypasses special case code in captureReturnedFxx()
  // that converts GPR results to FPR results for systemABI+softFP.  If we
  // ever start using that combination for calls we need more code.  This
  // assert is stronger than we need - we only care about results in return
  // registers - but that's OK.
  MOZ_ASSERT(!call.usesSystemAbi || call.hardFP);
#endif
  return pushResults(type, fr.stackResultsBase(loc.bytes()));
}

//////////////////////////////////////////////////////////////////////////////
//
// Exception handling

// Abstracted helper for throwing, used for throw, rethrow, and rethrowing
// at the end of a series of catch blocks (if none matched the exception).
bool BaseCompiler::throwFrom(RegRef exn) {
  pushRef(exn);

  // ThrowException invokes a trap, and the rest is dead code.
  return emitInstanceCall(SASigThrowException);
}

void BaseCompiler::loadTag(RegPtr instance, uint32_t tagIndex, RegRef tagDst) {
  size_t offset =
      Instance::offsetInData(moduleEnv_.offsetOfTagInstanceData(tagIndex));
  masm.loadPtr(Address(instance, offset), tagDst);
}

void BaseCompiler::consumePendingException(RegPtr instance, RegRef* exnDst,
                                           RegRef* tagDst) {
  RegPtr pendingAddr = RegPtr(PreBarrierReg);
  needPtr(pendingAddr);
  masm.computeEffectiveAddress(
      Address(instance, Instance::offsetOfPendingException()), pendingAddr);
  *exnDst = needRef();
  masm.loadPtr(Address(pendingAddr, 0), *exnDst);
  emitBarrieredClear(pendingAddr);

  *tagDst = needRef();
  masm.computeEffectiveAddress(
      Address(instance, Instance::offsetOfPendingExceptionTag()), pendingAddr);
  masm.loadPtr(Address(pendingAddr, 0), *tagDst);
  emitBarrieredClear(pendingAddr);
  freePtr(pendingAddr);
}

bool BaseCompiler::startTryNote(size_t* tryNoteIndex) {
  // Check the previous try note to ensure that we don't share an edge with
  // it that could lead to ambiguity. Insert a nop, if required.
  TryNoteVector& tryNotes = masm.tryNotes();
  if (tryNotes.length() > 0) {
    const TryNote& previous = tryNotes.back();
    uint32_t currentOffset = masm.currentOffset();
    if (previous.tryBodyBegin() == currentOffset ||
        previous.tryBodyEnd() == currentOffset) {
      masm.nop();
    }
  }

  // Mark the beginning of the try note
  wasm::TryNote tryNote = wasm::TryNote();
  tryNote.setTryBodyBegin(masm.currentOffset());
  return masm.append(tryNote, tryNoteIndex);
}

void BaseCompiler::finishTryNote(size_t tryNoteIndex) {
  TryNoteVector& tryNotes = masm.tryNotes();
  TryNote& tryNote = tryNotes[tryNoteIndex];

  // Disallow zero-length try notes by inserting a no-op
  if (tryNote.tryBodyBegin() == masm.currentOffset()) {
    masm.nop();
  }

  // Check the most recent finished try note to ensure that we don't share an
  // edge with it that could lead to ambiguity. Insert a nop, if required.
  //
  // Notice that finishTryNote is called in LIFO order -- using depth-first
  // search numbering to see if we are traversing back from a nested try to a
  // parent try, where we may need to ensure that the end offsets do not
  // coincide.
  //
  // In the case the tryNodeIndex >= mostRecentFinishedTryNoteIndex_, we have
  // finished a try that began after the most recent finished try, and so
  // startTryNote will take care of any nops.
  if (tryNoteIndex < mostRecentFinishedTryNoteIndex_) {
    const TryNote& previous = tryNotes[mostRecentFinishedTryNoteIndex_];
    uint32_t currentOffset = masm.currentOffset();
    if (previous.tryBodyEnd() == currentOffset) {
      masm.nop();
    }
  }
  mostRecentFinishedTryNoteIndex_ = tryNoteIndex;

  // Don't set the end of the try note if we've OOM'ed, as the above nop's may
  // not have been placed. This is okay as this compilation will be thrown
  // away.
  if (masm.oom()) {
    return;
  }

  // Mark the end of the try note
  tryNote.setTryBodyEnd(masm.currentOffset());
}

////////////////////////////////////////////////////////////
//
// Platform-specific popping and register targeting.

// The simple popping methods pop values into targeted registers; the caller
// can free registers using standard functions.  These are always called
// popXForY where X says something about types and Y something about the
// operation being targeted.

RegI32 BaseCompiler::needRotate64Temp() {
#if defined(JS_CODEGEN_X86)
  return needI32();
#elif defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM) ||    \
    defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_RISCV64)
  return RegI32::Invalid();
#else
  MOZ_CRASH("BaseCompiler platform hook: needRotate64Temp");
#endif
}

void BaseCompiler::popAndAllocateForDivAndRemI32(RegI32* r0, RegI32* r1,
                                                 RegI32* reserved) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // r0 must be eax, and edx will be clobbered.
  need2xI32(specific_.eax, specific_.edx);
  *r1 = popI32();
  *r0 = popI32ToSpecific(specific_.eax);
  *reserved = specific_.edx;
#else
  pop2xI32(r0, r1);
#endif
}

static void QuotientI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd,
                        RegI32 reserved, IsUnsigned isUnsigned) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  masm.quotient32(rs, rsd, reserved, isUnsigned);
#else
  masm.quotient32(rs, rsd, isUnsigned);
#endif
}

static void RemainderI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd,
                         RegI32 reserved, IsUnsigned isUnsigned) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  masm.remainder32(rs, rsd, reserved, isUnsigned);
#else
  masm.remainder32(rs, rsd, isUnsigned);
#endif
}

void BaseCompiler::popAndAllocateForMulI64(RegI64* r0, RegI64* r1,
                                           RegI32* temp) {
#if defined(JS_CODEGEN_X64)
  pop2xI64(r0, r1);
#elif defined(JS_CODEGEN_X86)
  // lhsDest must be edx:eax and rhs must not be that.
  needI64(specific_.edx_eax);
  *r1 = popI64();
  *r0 = popI64ToSpecific(specific_.edx_eax);
  *temp = needI32();
#elif defined(JS_CODEGEN_MIPS64)
  pop2xI64(r0, r1);
#elif defined(JS_CODEGEN_ARM)
  pop2xI64(r0, r1);
  *temp = needI32();
#elif defined(JS_CODEGEN_ARM64)
  pop2xI64(r0, r1);
#elif defined(JS_CODEGEN_LOONG64)
  pop2xI64(r0, r1);
#elif defined(JS_CODEGEN_RISCV64)
  pop2xI64(r0, r1);
#else
  MOZ_CRASH("BaseCompiler porting interface: popAndAllocateForMulI64");
#endif
}

#ifndef RABALDR_INT_DIV_I64_CALLOUT

void BaseCompiler::popAndAllocateForDivAndRemI64(RegI64* r0, RegI64* r1,
                                                 RegI64* reserved,
                                                 IsRemainder isRemainder) {
#  if defined(JS_CODEGEN_X64)
  // r0 must be rax, and rdx will be clobbered.
  need2xI64(specific_.rax, specific_.rdx);
  *r1 = popI64();
  *r0 = popI64ToSpecific(specific_.rax);
  *reserved = specific_.rdx;
#  elif defined(JS_CODEGEN_ARM64)
  pop2xI64(r0, r1);
  if (isRemainder) {
    *reserved = needI64();
  }
#  else
  pop2xI64(r0, r1);
#  endif
}

static void QuotientI64(MacroAssembler& masm, RegI64 rhs, RegI64 srcDest,
                        RegI64 reserved, IsUnsigned isUnsigned) {
#  if defined(JS_CODEGEN_X64)
  // The caller must set up the following situation.
  MOZ_ASSERT(srcDest.reg == rax);
  MOZ_ASSERT(reserved.reg == rdx);
  if (isUnsigned) {
    masm.xorq(rdx, rdx);
    masm.udivq(rhs.reg);
  } else {
    masm.cqo();
    masm.idivq(rhs.reg);
  }
#  elif defined(JS_CODEGEN_MIPS64)
  MOZ_ASSERT(reserved.isInvalid());
  if (isUnsigned) {
    masm.as_ddivu(srcDest.reg, rhs.reg);
  } else {
    masm.as_ddiv(srcDest.reg, rhs.reg);
  }
  masm.as_mflo(srcDest.reg);
#  elif defined(JS_CODEGEN_ARM64)
  MOZ_ASSERT(reserved.isInvalid());
  ARMRegister sd(srcDest.reg, 64);
  ARMRegister r(rhs.reg, 64);
  if (isUnsigned) {
    masm.Udiv(sd, sd, r);
  } else {
    masm.Sdiv(sd, sd, r);
  }
#  elif defined(JS_CODEGEN_LOONG64)
  if (isUnsigned) {
    masm.as_div_du(srcDest.reg, srcDest.reg, rhs.reg);
  } else {
    masm.as_div_d(srcDest.reg, srcDest.reg, rhs.reg);
  }
#  elif defined(JS_CODEGEN_RISCV64)
  if (isUnsigned) {
    masm.divu(srcDest.reg, srcDest.reg, rhs.reg);
  } else {
    masm.div(srcDest.reg, srcDest.reg, rhs.reg);
  }
#  else
  MOZ_CRASH("BaseCompiler platform hook: quotientI64");
#  endif
}

static void RemainderI64(MacroAssembler& masm, RegI64 rhs, RegI64 srcDest,
                         RegI64 reserved, IsUnsigned isUnsigned) {
#  if defined(JS_CODEGEN_X64)
  // The caller must set up the following situation.
  MOZ_ASSERT(srcDest.reg == rax);
  MOZ_ASSERT(reserved.reg == rdx);

  if (isUnsigned) {
    masm.xorq(rdx, rdx);
    masm.udivq(rhs.reg);
  } else {
    masm.cqo();
    masm.idivq(rhs.reg);
  }
  masm.movq(rdx, rax);
#  elif defined(JS_CODEGEN_MIPS64)
  MOZ_ASSERT(reserved.isInvalid());
  if (isUnsigned) {
    masm.as_ddivu(srcDest.reg, rhs.reg);
  } else {
    masm.as_ddiv(srcDest.reg, rhs.reg);
  }
  masm.as_mfhi(srcDest.reg);
#  elif defined(JS_CODEGEN_ARM64)
  ARMRegister sd(srcDest.reg, 64);
  ARMRegister r(rhs.reg, 64);
  ARMRegister t(reserved.reg, 64);
  if (isUnsigned) {
    masm.Udiv(t, sd, r);
  } else {
    masm.Sdiv(t, sd, r);
  }
  masm.Mul(t, t, r);
  masm.Sub(sd, sd, t);
#  elif defined(JS_CODEGEN_LOONG64)
  if (isUnsigned) {
    masm.as_mod_du(srcDest.reg, srcDest.reg, rhs.reg);
  } else {
    masm.as_mod_d(srcDest.reg, srcDest.reg, rhs.reg);
  }
#  elif defined(JS_CODEGEN_RISCV64)
  if (isUnsigned) {
    masm.remu(srcDest.reg, srcDest.reg, rhs.reg);
  } else {
    masm.rem(srcDest.reg, srcDest.reg, rhs.reg);
  }
#  else
  MOZ_CRASH("BaseCompiler platform hook: remainderI64");
#  endif
}

#endif  // RABALDR_INT_DIV_I64_CALLOUT

RegI32 BaseCompiler::popI32RhsForShift() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // r1 must be ecx for a variable shift, unless BMI2 is available.
  if (!Assembler::HasBMI2()) {
    return popI32(specific_.ecx);
  }
#endif
  RegI32 r = popI32();
#if defined(JS_CODEGEN_ARM)
  masm.and32(Imm32(31), r);
#endif
  return r;
}

RegI32 BaseCompiler::popI32RhsForShiftI64() {
#if defined(JS_CODEGEN_X86)
  // A limitation in the x86 masm requires ecx here
  return popI32(specific_.ecx);
#elif defined(JS_CODEGEN_X64)
  if (!Assembler::HasBMI2()) {
    return popI32(specific_.ecx);
  }
  return popI32();
#else
  return popI32();
#endif
}

RegI64 BaseCompiler::popI64RhsForShift() {
#if defined(JS_CODEGEN_X86)
  // r1 must be ecx for a variable shift.
  needI32(specific_.ecx);
  return popI64ToSpecific(widenI32(specific_.ecx));
#else
#  if defined(JS_CODEGEN_X64)
  // r1 must be rcx for a variable shift, unless BMI2 is available.
  if (!Assembler::HasBMI2()) {
    needI64(specific_.rcx);
    return popI64ToSpecific(specific_.rcx);
  }
#  endif
  // No masking is necessary on 64-bit platforms, and on arm32 the masm
  // implementation masks.
  return popI64();
#endif
}

RegI32 BaseCompiler::popI32RhsForRotate() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // r1 must be ecx for a variable rotate.
  return popI32(specific_.ecx);
#else
  return popI32();
#endif
}

RegI64 BaseCompiler::popI64RhsForRotate() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // r1 must be ecx for a variable rotate.
  needI32(specific_.ecx);
  return popI64ToSpecific(widenI32(specific_.ecx));
#else
  return popI64();
#endif
}

void BaseCompiler::popI32ForSignExtendI64(RegI64* r0) {
#if defined(JS_CODEGEN_X86)
  // r0 must be edx:eax for cdq
  need2xI32(specific_.edx, specific_.eax);
  *r0 = specific_.edx_eax;
  popI32ToSpecific(specific_.eax);
#else
  *r0 = widenI32(popI32());
#endif
}

void BaseCompiler::popI64ForSignExtendI64(RegI64* r0) {
#if defined(JS_CODEGEN_X86)
  // r0 must be edx:eax for cdq
  need2xI32(specific_.edx, specific_.eax);
  // Low on top, high underneath
  *r0 = popI64ToSpecific(specific_.edx_eax);
#else
  *r0 = popI64();
#endif
}

class OutOfLineTruncateCheckF32OrF64ToI32 : public OutOfLineCode {
  AnyReg src;
  RegI32 dest;
  TruncFlags flags;
  BytecodeOffset off;

 public:
  OutOfLineTruncateCheckF32OrF64ToI32(AnyReg src, RegI32 dest, TruncFlags flags,
                                      BytecodeOffset off)
      : src(src), dest(dest), flags(flags), off(off) {}

  virtual void generate(MacroAssembler* masm) override {
    if (src.tag == AnyReg::F32) {
      masm->oolWasmTruncateCheckF32ToI32(src.f32(), dest, flags, off, rejoin());
    } else if (src.tag == AnyReg::F64) {
      masm->oolWasmTruncateCheckF64ToI32(src.f64(), dest, flags, off, rejoin());
    } else {
      MOZ_CRASH("unexpected type");
    }
  }
};

bool BaseCompiler::truncateF32ToI32(RegF32 src, RegI32 dest, TruncFlags flags) {
  BytecodeOffset off = bytecodeOffset();
  OutOfLineCode* ool =
      addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI32(
          AnyReg(src), dest, flags, off));
  if (!ool) {
    return false;
  }
  bool isSaturating = flags & TRUNC_SATURATING;
  if (flags & TRUNC_UNSIGNED) {
    masm.wasmTruncateFloat32ToUInt32(src, dest, isSaturating, ool->entry());
  } else {
    masm.wasmTruncateFloat32ToInt32(src, dest, isSaturating, ool->entry());
  }
  masm.bind(ool->rejoin());
  return true;
}

bool BaseCompiler::truncateF64ToI32(RegF64 src, RegI32 dest, TruncFlags flags) {
  BytecodeOffset off = bytecodeOffset();
  OutOfLineCode* ool =
      addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI32(
          AnyReg(src), dest, flags, off));
  if (!ool) {
    return false;
  }
  bool isSaturating = flags & TRUNC_SATURATING;
  if (flags & TRUNC_UNSIGNED) {
    masm.wasmTruncateDoubleToUInt32(src, dest, isSaturating, ool->entry());
  } else {
    masm.wasmTruncateDoubleToInt32(src, dest, isSaturating, ool->entry());
  }
  masm.bind(ool->rejoin());
  return true;
}

class OutOfLineTruncateCheckF32OrF64ToI64 : public OutOfLineCode {
  AnyReg src;
  RegI64 dest;
  TruncFlags flags;
  BytecodeOffset off;

 public:
  OutOfLineTruncateCheckF32OrF64ToI64(AnyReg src, RegI64 dest, TruncFlags flags,
                                      BytecodeOffset off)
      : src(src), dest(dest), flags(flags), off(off) {}

  virtual void generate(MacroAssembler* masm) override {
    if (src.tag == AnyReg::F32) {
      masm->oolWasmTruncateCheckF32ToI64(src.f32(), dest, flags, off, rejoin());
    } else if (src.tag == AnyReg::F64) {
      masm->oolWasmTruncateCheckF64ToI64(src.f64(), dest, flags, off, rejoin());
    } else {
      MOZ_CRASH("unexpected type");
    }
  }
};

#ifndef RABALDR_FLOAT_TO_I64_CALLOUT

RegF64 BaseCompiler::needTempForFloatingToI64(TruncFlags flags) {
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  if (flags & TRUNC_UNSIGNED) {
    return needF64();
  }
#  endif
  return RegF64::Invalid();
}

bool BaseCompiler::truncateF32ToI64(RegF32 src, RegI64 dest, TruncFlags flags,
                                    RegF64 temp) {
  OutOfLineCode* ool =
      addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(
          AnyReg(src), dest, flags, bytecodeOffset()));
  if (!ool) {
    return false;
  }
  bool isSaturating = flags & TRUNC_SATURATING;
  if (flags & TRUNC_UNSIGNED) {
    masm.wasmTruncateFloat32ToUInt64(src, dest, isSaturating, ool->entry(),
                                     ool->rejoin(), temp);
  } else {
    masm.wasmTruncateFloat32ToInt64(src, dest, isSaturating, ool->entry(),
                                    ool->rejoin(), temp);
  }
  return true;
}

bool BaseCompiler::truncateF64ToI64(RegF64 src, RegI64 dest, TruncFlags flags,
                                    RegF64 temp) {
  OutOfLineCode* ool =
      addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(
          AnyReg(src), dest, flags, bytecodeOffset()));
  if (!ool) {
    return false;
  }
  bool isSaturating = flags & TRUNC_SATURATING;
  if (flags & TRUNC_UNSIGNED) {
    masm.wasmTruncateDoubleToUInt64(src, dest, isSaturating, ool->entry(),
                                    ool->rejoin(), temp);
  } else {
    masm.wasmTruncateDoubleToInt64(src, dest, isSaturating, ool->entry(),
                                   ool->rejoin(), temp);
  }
  return true;
}

#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT

RegI32 BaseCompiler::needConvertI64ToFloatTemp(ValType to, bool isUnsigned) {
  bool needs = false;
  if (to == ValType::F64) {
    needs = isUnsigned && masm.convertUInt64ToDoubleNeedsTemp();
  } else {
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    needs = true;
#  endif
  }
  return needs ? needI32() : RegI32::Invalid();
}

void BaseCompiler::convertI64ToF32(RegI64 src, bool isUnsigned, RegF32 dest,
                                   RegI32 temp) {
  if (isUnsigned) {
    masm.convertUInt64ToFloat32(src, dest, temp);
  } else {
    masm.convertInt64ToFloat32(src, dest);
  }
}

void BaseCompiler::convertI64ToF64(RegI64 src, bool isUnsigned, RegF64 dest,
                                   RegI32 temp) {
  if (isUnsigned) {
    masm.convertUInt64ToDouble(src, dest, temp);
  } else {
    masm.convertInt64ToDouble(src, dest);
  }
}

#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

//////////////////////////////////////////////////////////////////////
//
// Global variable access.

Address BaseCompiler::addressOfGlobalVar(const GlobalDesc& global, RegPtr tmp) {
  uint32_t globalToInstanceOffset = Instance::offsetInData(global.offset());
#ifdef RABALDR_PIN_INSTANCE
  movePtr(RegPtr(InstanceReg), tmp);
#else
  fr.loadInstancePtr(tmp);
#endif
  if (global.isIndirect()) {
    masm.loadPtr(Address(tmp, globalToInstanceOffset), tmp);
    return Address(tmp, 0);
  }
  return Address(tmp, globalToInstanceOffset);
}

//////////////////////////////////////////////////////////////////////
//
// Table access.

Address BaseCompiler::addressOfTableField(uint32_t tableIndex,
                                          uint32_t fieldOffset,
                                          RegPtr instance) {
  uint32_t tableToInstanceOffset = wasm::Instance::offsetInData(
      moduleEnv_.offsetOfTableInstanceData(tableIndex) + fieldOffset);
  return Address(instance, tableToInstanceOffset);
}

void BaseCompiler::loadTableLength(uint32_t tableIndex, RegPtr instance,
                                   RegI32 length) {
  masm.load32(addressOfTableField(
                  tableIndex, offsetof(TableInstanceData, length), instance),
              length);
}

void BaseCompiler::loadTableElements(uint32_t tableIndex, RegPtr instance,
                                     RegPtr elements) {
  masm.loadPtr(addressOfTableField(
                   tableIndex, offsetof(TableInstanceData, elements), instance),
               elements);
}

//////////////////////////////////////////////////////////////////////////////
//
// Basic emitters for simple operators.

static void AddI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.add32(rs, rsd);
}

static void AddImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.add32(Imm32(c), rsd);
}

static void SubI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.sub32(rs, rsd);
}

static void SubImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.sub32(Imm32(c), rsd);
}

static void MulI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.mul32(rs, rsd);
}

static void OrI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.or32(rs, rsd);
}

static void OrImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.or32(Imm32(c), rsd);
}

static void AndI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.and32(rs, rsd);
}

static void AndImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.and32(Imm32(c), rsd);
}

static void XorI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.xor32(rs, rsd);
}

static void XorImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.xor32(Imm32(c), rsd);
}

static void ClzI32(MacroAssembler& masm, RegI32 rsd) {
  masm.clz32(rsd, rsd, IsKnownNotZero(false));
}

static void CtzI32(MacroAssembler& masm, RegI32 rsd) {
  masm.ctz32(rsd, rsd, IsKnownNotZero(false));
}

// Currently common to PopcntI32 and PopcntI64
static RegI32 PopcntTemp(BaseCompiler& bc) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  return AssemblerX86Shared::HasPOPCNT() ? RegI32::Invalid() : bc.needI32();
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) ||    \
    defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
  return bc.needI32();
#else
  MOZ_CRASH("BaseCompiler platform hook: PopcntTemp");
#endif
}

static void PopcntI32(BaseCompiler& bc, RegI32 rsd, RegI32 temp) {
  bc.masm.popcnt32(rsd, rsd, temp);
}

static void ShlI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.lshift32(rs, rsd);
}

static void ShlImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.lshift32(Imm32(c & 31), rsd);
}

static void ShrI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rshift32Arithmetic(rs, rsd);
}

static void ShrImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rshift32Arithmetic(Imm32(c & 31), rsd);
}

static void ShrUI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rshift32(rs, rsd);
}

static void ShrUImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rshift32(Imm32(c & 31), rsd);
}

static void RotlI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rotateLeft(rs, rsd, rsd);
}

static void RotlImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rotateLeft(Imm32(c & 31), rsd, rsd);
}

static void RotrI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rotateRight(rs, rsd, rsd);
}

static void RotrImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rotateRight(Imm32(c & 31), rsd, rsd);
}

static void EqzI32(MacroAssembler& masm, RegI32 rsd) {
  masm.cmp32Set(Assembler::Equal, rsd, Imm32(0), rsd);
}

static void WrapI64ToI32(MacroAssembler& masm, RegI64 rs, RegI32 rd) {
  masm.move64To32(rs, rd);
}

static void AddI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.add64(rs, rsd);
}

static void AddImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.add64(Imm64(c), rsd);
}

static void SubI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.sub64(rs, rsd);
}

static void SubImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.sub64(Imm64(c), rsd);
}

static void OrI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.or64(rs, rsd);
}

static void OrImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.or64(Imm64(c), rsd);
}

static void AndI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.and64(rs, rsd);
}

static void AndImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.and64(Imm64(c), rsd);
}

static void XorI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.xor64(rs, rsd);
}

static void XorImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.xor64(Imm64(c), rsd);
}

static void ClzI64(BaseCompiler& bc, RegI64 rsd) {
  bc.masm.clz64(rsd, bc.lowPart(rsd));
  bc.maybeClearHighPart(rsd);
}

static void CtzI64(BaseCompiler& bc, RegI64 rsd) {
  bc.masm.ctz64(rsd, bc.lowPart(rsd));
  bc.maybeClearHighPart(rsd);
}

static void PopcntI64(BaseCompiler& bc, RegI64 rsd, RegI32 temp) {
  bc.masm.popcnt64(rsd, rsd, temp);
}

static void ShlI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd) {
  bc.masm.lshift64(bc.lowPart(rs), rsd);
}

static void ShlImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.lshift64(Imm32(c & 63), rsd);
}

static void ShrI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd) {
  bc.masm.rshift64Arithmetic(bc.lowPart(rs), rsd);
}

static void ShrImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.rshift64Arithmetic(Imm32(c & 63), rsd);
}

static void ShrUI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd) {
  bc.masm.rshift64(bc.lowPart(rs), rsd);
}

static void ShrUImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.rshift64(Imm32(c & 63), rsd);
}

static void EqzI64(MacroAssembler& masm, RegI64 rs, RegI32 rd) {
#ifdef JS_PUNBOX64
  masm.cmpPtrSet(Assembler::Equal, rs.reg, ImmWord(0), rd);
#else
  MOZ_ASSERT(rs.low == rd);
  masm.or32(rs.high, rs.low);
  masm.cmp32Set(Assembler::Equal, rs.low, Imm32(0), rd);
#endif
}

static void AddF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.addDouble(rs, rsd);
}

static void SubF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.subDouble(rs, rsd);
}

static void MulF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.mulDouble(rs, rsd);
}

static void DivF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.divDouble(rs, rsd);
}

static void MinF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): see comment in MinF32.
#ifdef RABALDR_SCRATCH_F64
  ScratchF64 zero(bc.ra);
#else
  ScratchF64 zero(bc.masm);
#endif
  bc.masm.loadConstantDouble(0, zero);
  bc.masm.subDouble(zero, rsd);
  bc.masm.subDouble(zero, rs);
  bc.masm.minDouble(rs, rsd, HandleNaNSpecially(true));
}

static void MaxF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): see comment in MinF32.
#ifdef RABALDR_SCRATCH_F64
  ScratchF64 zero(bc.ra);
#else
  ScratchF64 zero(bc.masm);
#endif
  bc.masm.loadConstantDouble(0, zero);
  bc.masm.subDouble(zero, rsd);
  bc.masm.subDouble(zero, rs);
  bc.masm.maxDouble(rs, rsd, HandleNaNSpecially(true));
}

static void CopysignF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd,
                        RegI64 temp0, RegI64 temp1) {
  masm.moveDoubleToGPR64(rsd, temp0);
  masm.moveDoubleToGPR64(rs, temp1);
  masm.and64(Imm64(INT64_MAX), temp0);
  masm.and64(Imm64(INT64_MIN), temp1);
  masm.or64(temp1, temp0);
  masm.moveGPR64ToDouble(temp0, rsd);
}

static void AbsF64(MacroAssembler& masm, RegF64 rsd) {
  masm.absDouble(rsd, rsd);
}

static void NegateF64(MacroAssembler& masm, RegF64 rsd) {
  masm.negateDouble(rsd);
}

static void SqrtF64(MacroAssembler& masm, RegF64 rsd) {
  masm.sqrtDouble(rsd, rsd);
}

static void AddF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.addFloat32(rs, rsd);
}

static void SubF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.subFloat32(rs, rsd);
}

static void MulF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.mulFloat32(rs, rsd);
}

static void DivF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.divFloat32(rs, rsd);
}

static void MinF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): Don't do this if one of the operands
  // is known to be a constant.
#ifdef RABALDR_SCRATCH_F32
  ScratchF32 zero(bc.ra);
#else
  ScratchF32 zero(bc.masm);
#endif
  bc.masm.loadConstantFloat32(0.f, zero);
  bc.masm.subFloat32(zero, rsd);
  bc.masm.subFloat32(zero, rs);
  bc.masm.minFloat32(rs, rsd, HandleNaNSpecially(true));
}

static void MaxF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): see comment in MinF32.
#ifdef RABALDR_SCRATCH_F32
  ScratchF32 zero(bc.ra);
#else
  ScratchF32 zero(bc.masm);
#endif
  bc.masm.loadConstantFloat32(0.f, zero);
  bc.masm.subFloat32(zero, rsd);
  bc.masm.subFloat32(zero, rs);
  bc.masm.maxFloat32(rs, rsd, HandleNaNSpecially(true));
}

static void CopysignF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd,
                        RegI32 temp0, RegI32 temp1) {
  masm.moveFloat32ToGPR(rsd, temp0);
  masm.moveFloat32ToGPR(rs, temp1);
  masm.and32(Imm32(INT32_MAX), temp0);
  masm.and32(Imm32(INT32_MIN), temp1);
  masm.or32(temp1, temp0);
  masm.moveGPRToFloat32(temp0, rsd);
}

static void AbsF32(MacroAssembler& masm, RegF32 rsd) {
  masm.absFloat32(rsd, rsd);
}

static void NegateF32(MacroAssembler& masm, RegF32 rsd) {
  masm.negateFloat(rsd);
}

static void SqrtF32(MacroAssembler& masm, RegF32 rsd) {
  masm.sqrtFloat32(rsd, rsd);
}

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
static void ConvertI64ToF32(MacroAssembler& masm, RegI64 rs, RegF32 rd) {
  masm.convertInt64ToFloat32(rs, rd);
}

static void ConvertI64ToF64(MacroAssembler& masm, RegI64 rs, RegF64 rd) {
  masm.convertInt64ToDouble(rs, rd);
}
#endif

static void ReinterpretF32AsI32(MacroAssembler& masm, RegF32 rs, RegI32 rd) {
  masm.moveFloat32ToGPR(rs, rd);
}

static void ReinterpretF64AsI64(MacroAssembler& masm, RegF64 rs, RegI64 rd) {
  masm.moveDoubleToGPR64(rs, rd);
}

static void ConvertF64ToF32(MacroAssembler& masm, RegF64 rs, RegF32 rd) {
  masm.convertDoubleToFloat32(rs, rd);
}

static void ConvertI32ToF32(MacroAssembler& masm, RegI32 rs, RegF32 rd) {
  masm.convertInt32ToFloat32(rs, rd);
}

static void ConvertU32ToF32(MacroAssembler& masm, RegI32 rs, RegF32 rd) {
  masm.convertUInt32ToFloat32(rs, rd);
}

static void ConvertF32ToF64(MacroAssembler& masm, RegF32 rs, RegF64 rd) {
  masm.convertFloat32ToDouble(rs, rd);
}

static void ConvertI32ToF64(MacroAssembler& masm, RegI32 rs, RegF64 rd) {
  masm.convertInt32ToDouble(rs, rd);
}

static void ConvertU32ToF64(MacroAssembler& masm, RegI32 rs, RegF64 rd) {
  masm.convertUInt32ToDouble(rs, rd);
}

static void ReinterpretI32AsF32(MacroAssembler& masm, RegI32 rs, RegF32 rd) {
  masm.moveGPRToFloat32(rs, rd);
}

static void ReinterpretI64AsF64(MacroAssembler& masm, RegI64 rs, RegF64 rd) {
  masm.moveGPR64ToDouble(rs, rd);
}

static void ExtendI32_8(BaseCompiler& bc, RegI32 rsd) {
#ifdef JS_CODEGEN_X86
  if (!bc.ra.isSingleByteI32(rsd)) {
    ScratchI8 scratch(bc.ra);
    bc.masm.move32(rsd, scratch);
    bc.masm.move8SignExtend(scratch, rsd);
    return;
  }
#endif
  bc.masm.move8SignExtend(rsd, rsd);
}

static void ExtendI32_16(MacroAssembler& masm, RegI32 rsd) {
  masm.move16SignExtend(rsd, rsd);
}

void BaseCompiler::emitMultiplyI64() {
  RegI64 r, rs;
  RegI32 temp;
  popAndAllocateForMulI64(&r, &rs, &temp);
  masm.mul64(rs, r, temp);
  maybeFree(temp);
  freeI64(rs);
  pushI64(r);
}

template <typename RegType, typename IntType>
void BaseCompiler::quotientOrRemainder(
    RegType rs, RegType rsd, RegType reserved, IsUnsigned isUnsigned,
    ZeroOnOverflow zeroOnOverflow, bool isConst, IntType c,
    void (*operate)(MacroAssembler& masm, RegType rs, RegType rsd,
                    RegType reserved, IsUnsigned isUnsigned)) {
  Label done;
  if (!isConst || c == 0) {
    checkDivideByZero(rs);
  }
  if (!isUnsigned && (!isConst || c == -1)) {
    checkDivideSignedOverflow(rs, rsd, &done, zeroOnOverflow);
  }
  operate(masm, rs, rsd, reserved, isUnsigned);
  masm.bind(&done);
}

void BaseCompiler::emitQuotientI32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI32 r = popI32();
      Label positive;
      masm.branchTest32(Assembler::NotSigned, r, r, &positive);
      masm.add32(Imm32(c - 1), r);
      masm.bind(&positive);

      masm.rshift32Arithmetic(Imm32(power & 31), r);
      pushI32(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    popAndAllocateForDivAndRemI32(&r, &rs, &reserved);
    quotientOrRemainder(rs, r, reserved, IsUnsigned(false),
                        ZeroOnOverflow(false), isConst, c, QuotientI32);
    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitQuotientU32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI32 r = popI32();
      masm.rshift32(Imm32(power & 31), r);
      pushI32(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    popAndAllocateForDivAndRemI32(&r, &rs, &reserved);
    quotientOrRemainder(rs, r, reserved, IsUnsigned(true),
                        ZeroOnOverflow(false), isConst, c, QuotientI32);
    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitRemainderI32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI32 r = popI32();
    RegI32 temp = needI32();
    moveI32(r, temp);

    Label positive;
    masm.branchTest32(Assembler::NotSigned, temp, temp, &positive);
    masm.add32(Imm32(c - 1), temp);
    masm.bind(&positive);

    masm.rshift32Arithmetic(Imm32(power & 31), temp);
    masm.lshift32(Imm32(power & 31), temp);
    masm.sub32(temp, r);
    freeI32(temp);

    pushI32(r);
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    popAndAllocateForDivAndRemI32(&r, &rs, &reserved);
    quotientOrRemainder(rs, r, reserved, IsUnsigned(false),
                        ZeroOnOverflow(true), isConst, c, RemainderI32);
    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitRemainderU32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI32 r = popI32();
    masm.and32(Imm32(c - 1), r);
    pushI32(r);
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    popAndAllocateForDivAndRemI32(&r, &rs, &reserved);
    quotientOrRemainder(rs, r, reserved, IsUnsigned(true), ZeroOnOverflow(true),
                        isConst, c, RemainderI32);
    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

#ifndef RABALDR_INT_DIV_I64_CALLOUT
void BaseCompiler::emitQuotientI64() {
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI64 r = popI64();
      Label positive;
      masm.branchTest64(Assembler::NotSigned, r, r, RegI32::Invalid(),
                        &positive);
      masm.add64(Imm64(c - 1), r);
      masm.bind(&positive);

      masm.rshift64Arithmetic(Imm32(power & 63), r);
      pushI64(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    popAndAllocateForDivAndRemI64(&r, &rs, &reserved, IsRemainder(false));
    quotientOrRemainder(rs, r, reserved, IsUnsigned(false),
                        ZeroOnOverflow(false), isConst, c, QuotientI64);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitQuotientU64() {
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI64 r = popI64();
      masm.rshift64(Imm32(power & 63), r);
      pushI64(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    popAndAllocateForDivAndRemI64(&r, &rs, &reserved, IsRemainder(false));
    quotientOrRemainder(rs, r, reserved, IsUnsigned(true),
                        ZeroOnOverflow(false), isConst, c, QuotientI64);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitRemainderI64() {
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI64 r = popI64();
    RegI64 temp = needI64();
    moveI64(r, temp);

    Label positive;
    masm.branchTest64(Assembler::NotSigned, temp, temp, RegI32::Invalid(),
                      &positive);
    masm.add64(Imm64(c - 1), temp);
    masm.bind(&positive);

    masm.rshift64Arithmetic(Imm32(power & 63), temp);
    masm.lshift64(Imm32(power & 63), temp);
    masm.sub64(temp, r);
    freeI64(temp);

    pushI64(r);
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    popAndAllocateForDivAndRemI64(&r, &rs, &reserved, IsRemainder(true));
    quotientOrRemainder(rs, r, reserved, IsUnsigned(false),
                        ZeroOnOverflow(true), isConst, c, RemainderI64);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitRemainderU64() {
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI64 r = popI64();
    masm.and64(Imm64(c - 1), r);
    pushI64(r);
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    popAndAllocateForDivAndRemI64(&r, &rs, &reserved, IsRemainder(true));
    quotientOrRemainder(rs, r, reserved, IsUnsigned(true), ZeroOnOverflow(true),
                        isConst, c, RemainderI64);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
}
#endif  // RABALDR_INT_DIV_I64_CALLOUT

void BaseCompiler::emitRotrI64() {
  int64_t c;
  if (popConst(&c)) {
    RegI64 r = popI64();
    RegI32 temp = needRotate64Temp();
    masm.rotateRight64(Imm32(c & 63), r, r, temp);
    maybeFree(temp);
    pushI64(r);
  } else {
    RegI64 rs = popI64RhsForRotate();
    RegI64 r = popI64();
    masm.rotateRight64(lowPart(rs), r, r, maybeHighPart(rs));
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitRotlI64() {
  int64_t c;
  if (popConst(&c)) {
    RegI64 r = popI64();
    RegI32 temp = needRotate64Temp();
    masm.rotateLeft64(Imm32(c & 63), r, r, temp);
    maybeFree(temp);
    pushI64(r);
  } else {
    RegI64 rs = popI64RhsForRotate();
    RegI64 r = popI64();
    masm.rotateLeft64(lowPart(rs), r, r, maybeHighPart(rs));
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitEqzI32() {
  if (sniffConditionalControlEqz(ValType::I32)) {
    return;
  }
  emitUnop(EqzI32);
}

void BaseCompiler::emitEqzI64() {
  if (sniffConditionalControlEqz(ValType::I64)) {
    return;
  }
  emitUnop(EqzI64);
}

template <TruncFlags flags>
bool BaseCompiler::emitTruncateF32ToI32() {
  RegF32 rs = popF32();
  RegI32 rd = needI32();
  if (!truncateF32ToI32(rs, rd, flags)) {
    return false;
  }
  freeF32(rs);
  pushI32(rd);
  return true;
}

template <TruncFlags flags>
bool BaseCompiler::emitTruncateF64ToI32() {
  RegF64 rs = popF64();
  RegI32 rd = needI32();
  if (!truncateF64ToI32(rs, rd, flags)) {
    return false;
  }
  freeF64(rs);
  pushI32(rd);
  return true;
}

#ifndef RABALDR_FLOAT_TO_I64_CALLOUT
template <TruncFlags flags>
bool BaseCompiler::emitTruncateF32ToI64() {
  RegF32 rs = popF32();
  RegI64 rd = needI64();
  RegF64 temp = needTempForFloatingToI64(flags);
  if (!truncateF32ToI64(rs, rd, flags, temp)) {
    return false;
  }
  maybeFree(temp);
  freeF32(rs);
  pushI64(rd);
  return true;
}

template <TruncFlags flags>
bool BaseCompiler::emitTruncateF64ToI64() {
  RegF64 rs = popF64();
  RegI64 rd = needI64();
  RegF64 temp = needTempForFloatingToI64(flags);
  if (!truncateF64ToI64(rs, rd, flags, temp)) {
    return false;
  }
  maybeFree(temp);
  freeF64(rs);
  pushI64(rd);
  return true;
}
#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

void BaseCompiler::emitExtendI64_8() {
  RegI64 r;
  popI64ForSignExtendI64(&r);
  masm.move8To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendI64_16() {
  RegI64 r;
  popI64ForSignExtendI64(&r);
  masm.move16To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendI64_32() {
  RegI64 r;
  popI64ForSignExtendI64(&r);
  masm.move32To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendI32ToI64() {
  RegI64 r;
  popI32ForSignExtendI64(&r);
  masm.move32To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendU32ToI64() {
  RegI32 rs = popI32();
  RegI64 rd = widenI32(rs);
  masm.move32To64ZeroExtend(rs, rd);
  pushI64(rd);
}

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
void BaseCompiler::emitConvertU64ToF32() {
  RegI64 rs = popI64();
  RegF32 rd = needF32();
  RegI32 temp = needConvertI64ToFloatTemp(ValType::F32, IsUnsigned(true));
  convertI64ToF32(rs, IsUnsigned(true), rd, temp);
  maybeFree(temp);
  freeI64(rs);
  pushF32(rd);
}

void BaseCompiler::emitConvertU64ToF64() {
  RegI64 rs = popI64();
  RegF64 rd = needF64();
  RegI32 temp = needConvertI64ToFloatTemp(ValType::F64, IsUnsigned(true));
  convertI64ToF64(rs, IsUnsigned(true), rd, temp);
  maybeFree(temp);
  freeI64(rs);
  pushF64(rd);
}
#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

////////////////////////////////////////////////////////////
//
// Machinery for optimized conditional branches.
//
// To disable this optimization it is enough always to return false from
// sniffConditionalControl{Cmp,Eqz}.

struct BranchState {
  union {
    struct {
      RegI32 lhs;
      RegI32 rhs;
      int32_t imm;
      bool rhsImm;
    } i32;
    struct {
      RegI64 lhs;
      RegI64 rhs;
      int64_t imm;
      bool rhsImm;
    } i64;
    struct {
      RegF32 lhs;
      RegF32 rhs;
    } f32;
    struct {
      RegF64 lhs;
      RegF64 rhs;
    } f64;
  };

  Label* const label;             // The target of the branch, never NULL
  const StackHeight stackHeight;  // The stack base above which to place
  // stack-spilled block results, if
  // hasBlockResults().
  const bool invertBranch;      // If true, invert the sense of the branch
  const ResultType resultType;  // The result propagated along the edges

  explicit BranchState(Label* label)
      : label(label),
        stackHeight(StackHeight::Invalid()),
        invertBranch(false),
        resultType(ResultType::Empty()) {}

  BranchState(Label* label, bool invertBranch)
      : label(label),
        stackHeight(StackHeight::Invalid()),
        invertBranch(invertBranch),
        resultType(ResultType::Empty()) {}

  BranchState(Label* label, StackHeight stackHeight, bool invertBranch,
              ResultType resultType)
      : label(label),
        stackHeight(stackHeight),
        invertBranch(invertBranch),
        resultType(resultType) {}

  bool hasBlockResults() const { return stackHeight.isValid(); }
};

void BaseCompiler::setLatentCompare(Assembler::Condition compareOp,
                                    ValType operandType) {
  latentOp_ = LatentOp::Compare;
  latentType_ = operandType;
  latentIntCmp_ = compareOp;
}

void BaseCompiler::setLatentCompare(Assembler::DoubleCondition compareOp,
                                    ValType operandType) {
  latentOp_ = LatentOp::Compare;
  latentType_ = operandType;
  latentDoubleCmp_ = compareOp;
}

void BaseCompiler::setLatentEqz(ValType operandType) {
  latentOp_ = LatentOp::Eqz;
  latentType_ = operandType;
}

bool BaseCompiler::hasLatentOp() const { return latentOp_ != LatentOp::None; }

void BaseCompiler::resetLatentOp() { latentOp_ = LatentOp::None; }

// Emit a conditional branch that optionally and optimally cleans up the CPU
// stack before we branch.
//
// Cond is either Assembler::Condition or Assembler::DoubleCondition.
//
// Lhs is RegI32, RegI64, or RegF32, RegF64, or RegRef.
//
// Rhs is either the same as Lhs, or an immediate expression compatible with
// Lhs "when applicable".

template <typename Cond, typename Lhs, typename Rhs>
bool BaseCompiler::jumpConditionalWithResults(BranchState* b, Cond cond,
                                              Lhs lhs, Rhs rhs) {
  if (b->hasBlockResults()) {
    StackHeight resultsBase(0);
    if (!topBranchParams(b->resultType, &resultsBase)) {
      return false;
    }
    if (b->stackHeight != resultsBase) {
      Label notTaken;
      branchTo(b->invertBranch ? cond : Assembler::InvertCondition(cond), lhs,
               rhs, &notTaken);

      // Shuffle stack args.
      shuffleStackResultsBeforeBranch(resultsBase, b->stackHeight,
                                      b->resultType);
      masm.jump(b->label);
      masm.bind(&notTaken);
      return true;
    }
  }

  branchTo(b->invertBranch ? Assembler::InvertCondition(cond) : cond, lhs, rhs,
           b->label);
  return true;
}

#ifdef ENABLE_WASM_GC
bool BaseCompiler::jumpConditionalWithResults(BranchState* b, RegRef object,
                                              RefType sourceType,
                                              RefType destType,
                                              bool onSuccess) {
  // Temporarily take the result registers so that branchIfRefSubtype
  // doesn't use them.
  needIntegerResultRegisters(b->resultType);
  BranchIfRefSubtypeRegisters regs =
      allocRegistersForBranchIfRefSubtype(destType);
  freeIntegerResultRegisters(b->resultType);

  if (b->hasBlockResults()) {
    StackHeight resultsBase(0);
    if (!topBranchParams(b->resultType, &resultsBase)) {
      return false;
    }
    if (b->stackHeight != resultsBase) {
      Label notTaken;

      masm.branchWasmRefIsSubtype(
          object, sourceType, destType, &notTaken,
          /*onSuccess=*/b->invertBranch ? onSuccess : !onSuccess, regs.superSTV,
          regs.scratch1, regs.scratch2);
      freeRegistersForBranchIfRefSubtype(regs);

      // Shuffle stack args.
      shuffleStackResultsBeforeBranch(resultsBase, b->stackHeight,
                                      b->resultType);
      masm.jump(b->label);
      masm.bind(&notTaken);
      return true;
    }
  }

  masm.branchWasmRefIsSubtype(
      object, sourceType, destType, b->label,
      /*onSuccess=*/b->invertBranch ? !onSuccess : onSuccess, regs.superSTV,
      regs.scratch1, regs.scratch2);
  freeRegistersForBranchIfRefSubtype(regs);
  return true;
}
#endif

// sniffConditionalControl{Cmp,Eqz} may modify the latentWhatever_ state in
// the BaseCompiler so that a subsequent conditional branch can be compiled
// optimally.  emitBranchSetup() and emitBranchPerform() will consume that
// state.  If the latter methods are not called because deadCode_ is true
// then the compiler MUST instead call resetLatentOp() to reset the state.

template <typename Cond>
bool BaseCompiler::sniffConditionalControlCmp(Cond compareOp,
                                              ValType operandType) {
  MOZ_ASSERT(latentOp_ == LatentOp::None,
             "Latent comparison state not properly reset");

#ifdef JS_CODEGEN_X86
  // On x86, latent i64 binary comparisons use too many registers: the
  // reserved join register and the lhs and rhs operands require six, but we
  // only have five.
  if (operandType == ValType::I64) {
    return false;
  }
#endif

  // No optimization for pointer compares yet.
  if (operandType.isRefRepr()) {
    return false;
  }

  OpBytes op{};
  iter_.peekOp(&op);
  switch (op.b0) {
    case uint16_t(Op::BrIf):
    case uint16_t(Op::If):
    case uint16_t(Op::SelectNumeric):
    case uint16_t(Op::SelectTyped):
      setLatentCompare(compareOp, operandType);
      return true;
    default:
      return false;
  }
}

bool BaseCompiler::sniffConditionalControlEqz(ValType operandType) {
  MOZ_ASSERT(latentOp_ == LatentOp::None,
             "Latent comparison state not properly reset");

  OpBytes op{};
  iter_.peekOp(&op);
  switch (op.b0) {
    case uint16_t(Op::BrIf):
    case uint16_t(Op::SelectNumeric):
    case uint16_t(Op::SelectTyped):
    case uint16_t(Op::If):
      setLatentEqz(operandType);
      return true;
    default:
      return false;
  }
}

void BaseCompiler::emitBranchSetup(BranchState* b) {
  // Avoid allocating operands to latentOp_ to result registers.
  if (b->hasBlockResults()) {
    needResultRegisters(b->resultType);
  }

  // Set up fields so that emitBranchPerform() need not switch on latentOp_.
  switch (latentOp_) {
    case LatentOp::None: {
      latentIntCmp_ = Assembler::NotEqual;
      latentType_ = ValType::I32;
      b->i32.lhs = popI32();
      b->i32.rhsImm = true;
      b->i32.imm = 0;
      break;
    }
    case LatentOp::Compare: {
      switch (latentType_.kind()) {
        case ValType::I32: {
          if (popConst(&b->i32.imm)) {
            b->i32.lhs = popI32();
            b->i32.rhsImm = true;
          } else {
            pop2xI32(&b->i32.lhs, &b->i32.rhs);
            b->i32.rhsImm = false;
          }
          break;
        }
        case ValType::I64: {
          pop2xI64(&b->i64.lhs, &b->i64.rhs);
          b->i64.rhsImm = false;
          break;
        }
        case ValType::F32: {
          pop2xF32(&b->f32.lhs, &b->f32.rhs);
          break;
        }
        case ValType::F64: {
          pop2xF64(&b->f64.lhs, &b->f64.rhs);
          break;
        }
        default: {
          MOZ_CRASH("Unexpected type for LatentOp::Compare");
        }
      }
      break;
    }
    case LatentOp::Eqz: {
      switch (latentType_.kind()) {
        case ValType::I32: {
          latentIntCmp_ = Assembler::Equal;
          b->i32.lhs = popI32();
          b->i32.rhsImm = true;
          b->i32.imm = 0;
          break;
        }
        case ValType::I64: {
          latentIntCmp_ = Assembler::Equal;
          b->i64.lhs = popI64();
          b->i64.rhsImm = true;
          b->i64.imm = 0;
          break;
        }
        default: {
          MOZ_CRASH("Unexpected type for LatentOp::Eqz");
        }
      }
      break;
    }
  }

  if (b->hasBlockResults()) {
    freeResultRegisters(b->resultType);
  }
}

bool BaseCompiler::emitBranchPerform(BranchState* b) {
  switch (latentType_.kind()) {
    case ValType::I32: {
      if (b->i32.rhsImm) {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i32.lhs,
                                        Imm32(b->i32.imm))) {
          return false;
        }
      } else {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i32.lhs,
                                        b->i32.rhs)) {
          return false;
        }
        freeI32(b->i32.rhs);
      }
      freeI32(b->i32.lhs);
      break;
    }
    case ValType::I64: {
      if (b->i64.rhsImm) {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i64.lhs,
                                        Imm64(b->i64.imm))) {
          return false;
        }
      } else {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i64.lhs,
                                        b->i64.rhs)) {
          return false;
        }
        freeI64(b->i64.rhs);
      }
      freeI64(b->i64.lhs);
      break;
    }
    case ValType::F32: {
      if (!jumpConditionalWithResults(b, latentDoubleCmp_, b->f32.lhs,
                                      b->f32.rhs)) {
        return false;
      }
      freeF32(b->f32.lhs);
      freeF32(b->f32.rhs);
      break;
    }
    case ValType::F64: {
      if (!jumpConditionalWithResults(b, latentDoubleCmp_, b->f64.lhs,
                                      b->f64.rhs)) {
        return false;
      }
      freeF64(b->f64.lhs);
      freeF64(b->f64.rhs);
      break;
    }
    default: {
      MOZ_CRASH("Unexpected type for LatentOp::Compare");
    }
  }
  resetLatentOp();
  return true;
}

// For blocks and loops and ifs:
//
//  - Sync the value stack before going into the block in order to simplify exit
//    from the block: all exits from the block can assume that there are no
//    live registers except the one carrying the exit value.
//  - The block can accumulate a number of dead values on the stacks, so when
//    branching out of the block or falling out at the end be sure to
//    pop the appropriate stacks back to where they were on entry, while
//    preserving the exit value.
//  - A continue branch in a loop is much like an exit branch, but the branch
//    value must not be preserved.
//  - The exit value is always in a designated join register (type dependent).

bool BaseCompiler::emitBlock() {
  ResultType params;
  if (!iter_.readBlock(&params)) {
    return false;
  }

  if (!deadCode_) {
    sync();  // Simplifies branching out from block
  }

  initControl(controlItem(), params);

  return true;
}

bool BaseCompiler::endBlock(ResultType type) {
  Control& block = controlItem();

  if (deadCode_) {
    // Block does not fall through; reset stack.
    fr.resetStackHeight(block.stackHeight, type);
    popValueStackTo(block.stackSize);
  } else {
    // If the block label is used, we have a control join, so we need to shuffle
    // fallthrough values into place.  Otherwise if it's not a control join, we
    // can leave the value stack alone.
    MOZ_ASSERT(stk_.length() == block.stackSize + type.length());
    if (block.label.used()) {
      popBlockResults(type, block.stackHeight, ContinuationKind::Fallthrough);
    }
    block.bceSafeOnExit &= bceSafe_;
  }

  // Bind after cleanup: branches out will have popped the stack.
  if (block.label.used()) {
    masm.bind(&block.label);
    if (deadCode_) {
      captureResultRegisters(type);
      deadCode_ = false;
    }
    if (!pushBlockResults(type)) {
      return false;
    }
  }

  bceSafe_ = block.bceSafeOnExit;

  return true;
}

bool BaseCompiler::emitLoop() {
  ResultType params;
  if (!iter_.readLoop(&params)) {
    return false;
  }

  if (!deadCode_) {
    sync();  // Simplifies branching out from block
  }

  initControl(controlItem(), params);
  bceSafe_ = 0;

  if (!deadCode_) {
    // Loop entry is a control join, so shuffle the entry parameters into the
    // well-known locations.
    if (!topBlockParams(params)) {
      return false;
    }
    masm.nopAlign(CodeAlignment);
    masm.bind(&controlItem(0).label);
    // The interrupt check barfs if there are live registers.
    sync();
    if (!addInterruptCheck()) {
      return false;
    }
  }

  return true;
}

// The bodies of the "then" and "else" arms can be arbitrary sequences
// of expressions, they push control and increment the nesting and can
// even be targeted by jumps.  A branch to the "if" block branches to
// the exit of the if, ie, it's like "break".  Consider:
//
//      (func (result i32)
//       (if (i32.const 1)
//           (begin (br 1) (unreachable))
//           (begin (unreachable)))
//       (i32.const 1))
//
// The branch causes neither of the unreachable expressions to be
// evaluated.

bool BaseCompiler::emitIf() {
  ResultType params;
  Nothing unused_cond;
  if (!iter_.readIf(&params, &unused_cond)) {
    return false;
  }

  BranchState b(&controlItem().otherLabel, InvertBranch(true));
  if (!deadCode_) {
    needResultRegisters(params);
    emitBranchSetup(&b);
    freeResultRegisters(params);
    sync();
  } else {
    resetLatentOp();
  }

  initControl(controlItem(), params);

  if (!deadCode_) {
    // Because params can flow immediately to results in the case of an empty
    // "then" or "else" block, and the result of an if/then is a join in
    // general, we shuffle params eagerly to the result allocations.
    if (!topBlockParams(params)) {
      return false;
    }
    if (!emitBranchPerform(&b)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::endIfThen(ResultType type) {
  Control& ifThen = controlItem();

  // The parameters to the "if" logically flow to both the "then" and "else"
  // blocks, but the "else" block is empty.  Since we know that the "if"
  // type-checks, that means that the "else" parameters are the "else" results,
  // and that the "if"'s result type is the same as its parameter type.

  if (deadCode_) {
    // "then" arm does not fall through; reset stack.
    fr.resetStackHeight(ifThen.stackHeight, type);
    popValueStackTo(ifThen.stackSize);
    if (!ifThen.deadOnArrival) {
      captureResultRegisters(type);
    }
  } else {
    MOZ_ASSERT(stk_.length() == ifThen.stackSize + type.length());
    // Assume we have a control join, so place results in block result
    // allocations.
    popBlockResults(type, ifThen.stackHeight, ContinuationKind::Fallthrough);
    MOZ_ASSERT(!ifThen.deadOnArrival);
  }

  if (ifThen.otherLabel.used()) {
    masm.bind(&ifThen.otherLabel);
  }

  if (ifThen.label.used()) {
    masm.bind(&ifThen.label);
  }

  if (!deadCode_) {
    ifThen.bceSafeOnExit &= bceSafe_;
  }

  deadCode_ = ifThen.deadOnArrival;
  if (!deadCode_) {
    if (!pushBlockResults(type)) {
      return false;
    }
  }

  bceSafe_ = ifThen.bceSafeOnExit & ifThen.bceSafeOnEntry;

  return true;
}

bool BaseCompiler::emitElse() {
  ResultType params, results;
  BaseNothingVector unused_thenValues{};

  if (!iter_.readElse(&params, &results, &unused_thenValues)) {
    return false;
  }

  Control& ifThenElse = controlItem(0);

  // See comment in endIfThenElse, below.

  // Exit the "then" branch.

  ifThenElse.deadThenBranch = deadCode_;

  if (deadCode_) {
    fr.resetStackHeight(ifThenElse.stackHeight, results);
    popValueStackTo(ifThenElse.stackSize);
  } else {
    MOZ_ASSERT(stk_.length() == ifThenElse.stackSize + results.length());
    popBlockResults(results, ifThenElse.stackHeight, ContinuationKind::Jump);
    freeResultRegisters(results);
    MOZ_ASSERT(!ifThenElse.deadOnArrival);
  }

  if (!deadCode_) {
    masm.jump(&ifThenElse.label);
  }

  if (ifThenElse.otherLabel.used()) {
    masm.bind(&ifThenElse.otherLabel);
  }

  // Reset to the "else" branch.

  if (!deadCode_) {
    ifThenElse.bceSafeOnExit &= bceSafe_;
  }

  deadCode_ = ifThenElse.deadOnArrival;
  bceSafe_ = ifThenElse.bceSafeOnEntry;

  fr.resetStackHeight(ifThenElse.stackHeight, params);

  if (!deadCode_) {
    captureResultRegisters(params);
    if (!pushBlockResults(params)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::endIfThenElse(ResultType type) {
  Control& ifThenElse = controlItem();

  // The expression type is not a reliable guide to what we'll find
  // on the stack, we could have (if E (i32.const 1) (unreachable))
  // in which case the "else" arm is AnyType but the type of the
  // full expression is I32.  So restore whatever's there, not what
  // we want to find there.  The "then" arm has the same constraint.

  if (deadCode_) {
    // "then" arm does not fall through; reset stack.
    fr.resetStackHeight(ifThenElse.stackHeight, type);
    popValueStackTo(ifThenElse.stackSize);
  } else {
    MOZ_ASSERT(stk_.length() == ifThenElse.stackSize + type.length());
    // Assume we have a control join, so place results in block result
    // allocations.
    popBlockResults(type, ifThenElse.stackHeight,
                    ContinuationKind::Fallthrough);
    ifThenElse.bceSafeOnExit &= bceSafe_;
    MOZ_ASSERT(!ifThenElse.deadOnArrival);
  }

  if (ifThenElse.label.used()) {
    masm.bind(&ifThenElse.label);
  }

  bool joinLive =
      !ifThenElse.deadOnArrival &&
      (!ifThenElse.deadThenBranch || !deadCode_ || ifThenElse.label.bound());

  if (joinLive) {
    // No values were provided by the "then" path, but capture the values
    // provided by the "else" path.
    if (deadCode_) {
      captureResultRegisters(type);
    }
    deadCode_ = false;
  }

  bceSafe_ = ifThenElse.bceSafeOnExit;

  if (!deadCode_) {
    if (!pushBlockResults(type)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::emitEnd() {
  LabelKind kind;
  ResultType type;
  BaseNothingVector unused_values{};
  if (!iter_.readEnd(&kind, &type, &unused_values, &unused_values)) {
    return false;
  }

  // Every label case is responsible to pop the control item at the appropriate
  // time for the label case
  switch (kind) {
    case LabelKind::Body:
      if (!endBlock(type)) {
        return false;
      }
      doReturn(ContinuationKind::Fallthrough);
      iter_.popEnd();
      MOZ_ASSERT(iter_.controlStackEmpty());
      return iter_.endFunction(iter_.end());
    case LabelKind::Block:
      if (!endBlock(type)) {
        return false;
      }
      iter_.popEnd();
      break;
    case LabelKind::Loop:
      // The end of a loop isn't a branch target, so we can just leave its
      // results on the expression stack to be consumed by the outer block.
      iter_.popEnd();
      break;
    case LabelKind::Then:
      if (!endIfThen(type)) {
        return false;
      }
      iter_.popEnd();
      break;
    case LabelKind::Else:
      if (!endIfThenElse(type)) {
        return false;
      }
      iter_.popEnd();
      break;
    case LabelKind::Try:
    case LabelKind::Catch:
    case LabelKind::CatchAll:
      if (!endTryCatch(type)) {
        return false;
      }
      iter_.popEnd();
      break;
    case LabelKind::TryTable:
      if (!endTryTable(type)) {
        return false;
      }
      iter_.popEnd();
      break;
  }

  return true;
}

bool BaseCompiler::emitBr() {
  uint32_t relativeDepth;
  ResultType type;
  BaseNothingVector unused_values{};
  if (!iter_.readBr(&relativeDepth, &type, &unused_values)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  // Save any values in the designated join registers, as if the target block
  // returned normally.

  popBlockResults(type, target.stackHeight, ContinuationKind::Jump);
  masm.jump(&target.label);

  // The registers holding the join values are free for the remainder of this
  // block.

  freeResultRegisters(type);

  deadCode_ = true;

  return true;
}

bool BaseCompiler::emitBrIf() {
  uint32_t relativeDepth;
  ResultType type;
  BaseNothingVector unused_values{};
  Nothing unused_condition;
  if (!iter_.readBrIf(&relativeDepth, &type, &unused_values,
                      &unused_condition)) {
    return false;
  }

  if (deadCode_) {
    resetLatentOp();
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  BranchState b(&target.label, target.stackHeight, InvertBranch(false), type);
  emitBranchSetup(&b);
  return emitBranchPerform(&b);
}

#ifdef ENABLE_WASM_GC
bool BaseCompiler::emitBrOnNull() {
  MOZ_ASSERT(!hasLatentOp());

  uint32_t relativeDepth;
  ResultType type;
  BaseNothingVector unused_values{};
  Nothing unused_condition;
  if (!iter_.readBrOnNull(&relativeDepth, &type, &unused_values,
                          &unused_condition)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  BranchState b(&target.label, target.stackHeight, InvertBranch(false), type);
  if (b.hasBlockResults()) {
    needResultRegisters(b.resultType);
  }
  RegRef ref = popRef();
  if (b.hasBlockResults()) {
    freeResultRegisters(b.resultType);
  }
  if (!jumpConditionalWithResults(&b, Assembler::Equal, ref,
                                  ImmWord(AnyRef::NullRefValue))) {
    return false;
  }
  pushRef(ref);

  return true;
}

bool BaseCompiler::emitBrOnNonNull() {
  MOZ_ASSERT(!hasLatentOp());

  uint32_t relativeDepth;
  ResultType type;
  BaseNothingVector unused_values{};
  Nothing unused_condition;
  if (!iter_.readBrOnNonNull(&relativeDepth, &type, &unused_values,
                             &unused_condition)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  BranchState b(&target.label, target.stackHeight, InvertBranch(false), type);
  MOZ_ASSERT(b.hasBlockResults(), "br_on_non_null has block results");

  // Don't allocate the result register used in the branch
  needIntegerResultRegisters(b.resultType);

  // Get the ref from the top of the stack
  RegRef refCondition = popRef();

  // Create a copy of the ref for passing to the on_non_null label,
  // the original ref is used in the condition.
  RegRef ref = needRef();
  moveRef(refCondition, ref);
  pushRef(ref);

  freeIntegerResultRegisters(b.resultType);

  if (!jumpConditionalWithResults(&b, Assembler::NotEqual, refCondition,
                                  ImmWord(AnyRef::NullRefValue))) {
    return false;
  }

  freeRef(refCondition);

  // Dropping null reference.
  dropValue();

  return true;
}
#endif

bool BaseCompiler::emitBrTable() {
  Uint32Vector depths;
  uint32_t defaultDepth;
  ResultType branchParams;
  BaseNothingVector unused_values{};
  Nothing unused_index;
  // N.B., `branchParams' gets set to the type of the default branch target.  In
  // the presence of subtyping, it could be that the different branch targets
  // have different types.  Here we rely on the assumption that the value
  // representations (e.g. Stk value types) of all branch target types are the
  // same, in the baseline compiler.  Notably, this means that all Ref types
  // should be represented the same.
  if (!iter_.readBrTable(&depths, &defaultDepth, &branchParams, &unused_values,
                         &unused_index)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  // Don't use param registers for rc
  needIntegerResultRegisters(branchParams);

  // Table switch value always on top.
  RegI32 rc = popI32();

  freeIntegerResultRegisters(branchParams);

  StackHeight resultsBase(0);
  if (!topBranchParams(branchParams, &resultsBase)) {
    return false;
  }

  Label dispatchCode;
  masm.branch32(Assembler::Below, rc, Imm32(depths.length()), &dispatchCode);

  // This is the out-of-range stub.  rc is dead here but we don't need it.

  shuffleStackResultsBeforeBranch(
      resultsBase, controlItem(defaultDepth).stackHeight, branchParams);
  controlItem(defaultDepth).bceSafeOnExit &= bceSafe_;
  masm.jump(&controlItem(defaultDepth).label);

  // Emit stubs.  rc is dead in all of these but we don't need it.
  //
  // The labels in the vector are in the TempAllocator and will
  // be freed by and by.
  //
  // TODO / OPTIMIZE (Bug 1316804): Branch directly to the case code if we
  // can, don't emit an intermediate stub.

  LabelVector stubs;
  if (!stubs.reserve(depths.length())) {
    return false;
  }

  for (uint32_t depth : depths) {
    stubs.infallibleEmplaceBack(NonAssertingLabel());
    masm.bind(&stubs.back());
    shuffleStackResultsBeforeBranch(resultsBase, controlItem(depth).stackHeight,
                                    branchParams);
    controlItem(depth).bceSafeOnExit &= bceSafe_;
    masm.jump(&controlItem(depth).label);
  }

  // Emit table.

  Label theTable;
  jumpTable(stubs, &theTable);

  // Emit indirect jump.  rc is live here.

  tableSwitch(&theTable, rc, &dispatchCode);

  deadCode_ = true;

  // Clean up.

  freeI32(rc);
  popValueStackBy(branchParams.length());

  return true;
}

bool BaseCompiler::emitTry() {
  ResultType params;
  if (!iter_.readTry(&params)) {
    return false;
  }

  if (!deadCode_) {
    // Simplifies jumping out, but it is also necessary so that control
    // can re-enter the catch handler without restoring registers.
    sync();
  }

  initControl(controlItem(), params);

  if (!deadCode_) {
    // Be conservative for BCE due to complex control flow in try blocks.
    controlItem().bceSafeOnExit = 0;
    if (!startTryNote(&controlItem().tryNoteIndex)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::emitTryTable() {
  ResultType params;
  TryTableCatchVector catches;
  if (!iter_.readTryTable(&params, &catches)) {
    return false;
  }

  if (!deadCode_) {
    // Simplifies jumping out, but it is also necessary so that control
    // can re-enter the catch handler without restoring registers.
    sync();
  }

  initControl(controlItem(), params);
  // Be conservative for BCE due to complex control flow in try blocks.
  controlItem().bceSafeOnExit = 0;

  // Don't emit a landing pad if this whole try is dead code
  if (deadCode_) {
    return true;
  }

  // Emit a landing pad that exceptions will jump into. Jump over it for now.
  Label skipLandingPad;
  masm.jump(&skipLandingPad);

  StackHeight prePadHeight = fr.stackHeight();
  uint32_t padOffset = masm.currentOffset();
  uint32_t padStackHeight = masm.framePushed();

  // InstanceReg is live and contains this function's instance by the exception
  // handling resume method. We keep it alive for use in loading the tag for
  // each catch handler.
  RegPtr instance = RegPtr(InstanceReg);
#ifndef RABALDR_PIN_INSTANCE
  needPtr(instance);
#endif

  // Load exception and tag from instance, clearing it in the process.
  RegRef exn;
  RegRef exnTag;
  consumePendingException(instance, &exn, &exnTag);

  // Get a register to hold the tags for each catch
  RegRef catchTag = needRef();

  bool hadCatchAll = false;
  for (const TryTableCatch& tryTableCatch : catches) {
    ResultType labelParams = ResultType::Vector(tryTableCatch.labelType);

    Control& target = controlItem(tryTableCatch.labelRelativeDepth);
    target.bceSafeOnExit = 0;

    // Handle a catch_all by jumping to the target block
    if (tryTableCatch.tagIndex == CatchAllIndex) {
      // Capture the exnref if it has been requested, or else free it.
      if (tryTableCatch.captureExnRef) {
        pushRef(exn);
      } else {
        freeRef(exn);
      }
      // Free all of the other registers
      freeRef(exnTag);
      freeRef(catchTag);
#ifndef RABALDR_PIN_INSTANCE
      freePtr(instance);
#endif

      // Pop the results needed for the target branch and perform the jump
      popBlockResults(labelParams, target.stackHeight, ContinuationKind::Jump);
      masm.jump(&target.label);
      freeResultRegisters(labelParams);

      // Break from the loop and skip the implicit rethrow that's needed
      // if we didn't have a catch_all
      hadCatchAll = true;
      break;
    }

    // This is a `catch $t`, load the tag type we're trying to match
    const TagType& tagType = *moduleEnv_.tags[tryTableCatch.tagIndex].type;
    const TagOffsetVector& tagOffsets = tagType.argOffsets();
    ResultType tagParams = tagType.resultType();

    // Load the tag for this catch and compare it against the exception's tag.
    // If they don't match, skip to the next catch handler.
    Label skipCatch;
    loadTag(instance, tryTableCatch.tagIndex, catchTag);
    masm.branchPtr(Assembler::NotEqual, exnTag, catchTag, &skipCatch);

    // The tags and instance are dead after we've had a match, free them
    freeRef(exnTag);
    freeRef(catchTag);
#ifndef RABALDR_PIN_INSTANCE
    freePtr(instance);
#endif

    // Allocate a register to hold the exception data pointer
    RegPtr data = needPtr();

    // Unpack the tag and jump to the block
    masm.loadPtr(Address(exn, (int32_t)WasmExceptionObject::offsetOfData()),
                 data);
    // This method can increase stk_.length() by an unbounded amount, so we need
    // to perform an allocation here to accomodate the variable number of
    // values. There is enough headroom for the fixed number of values.  The
    // general case is handled in emitBody.
    if (!stk_.reserve(stk_.length() + labelParams.length())) {
      return false;
    }

    for (uint32_t i = 0; i < tagParams.length(); i++) {
      int32_t offset = tagOffsets[i];
      switch (tagParams[i].kind()) {
        case ValType::I32: {
          RegI32 reg = needI32();
          masm.load32(Address(data, offset), reg);
          pushI32(reg);
          break;
        }
        case ValType::I64: {
          RegI64 reg = needI64();
          masm.load64(Address(data, offset), reg);
          pushI64(reg);
          break;
        }
        case ValType::F32: {
          RegF32 reg = needF32();
          masm.loadFloat32(Address(data, offset), reg);
          pushF32(reg);
          break;
        }
        case ValType::F64: {
          RegF64 reg = needF64();
          masm.loadDouble(Address(data, offset), reg);
          pushF64(reg);
          break;
        }
        case ValType::V128: {
#ifdef ENABLE_WASM_SIMD
          RegV128 reg = needV128();
          masm.loadUnalignedSimd128(Address(data, offset), reg);
          pushV128(reg);
          break;
#else
          MOZ_CRASH("No SIMD support");
#endif
        }
        case ValType::Ref: {
          RegRef reg = needRef();
          masm.loadPtr(Address(data, offset), reg);
          pushRef(reg);
          break;
        }
      }
    }

    // The exception data pointer is no longer live after unpacking the
    // exception
    freePtr(data);

    // Capture the exnref if it has been requested, or else free it.
    if (tryTableCatch.captureExnRef) {
      pushRef(exn);
    } else {
      freeRef(exn);
    }

    // Pop the results needed for the target branch and perform the jump
    popBlockResults(labelParams, target.stackHeight, ContinuationKind::Jump);
    masm.jump(&target.label);
    freeResultRegisters(labelParams);

    // Reset the stack height for the skip to the next catch handler
    fr.setStackHeight(prePadHeight);
    masm.bind(&skipCatch);

    // Reset ownership of the registers for the next catch handler we emit
    needRef(exn);
    needRef(exnTag);
    needRef(catchTag);
#ifndef RABALDR_PIN_INSTANCE
    needPtr(instance);
#endif
  }

  if (!hadCatchAll) {
    // Free all registers, except for the exception
    freeRef(exnTag);
    freeRef(catchTag);
#ifndef RABALDR_PIN_INSTANCE
    freePtr(instance);
#endif

    // If none of the tag checks succeed and there is no catch_all,
    // then we rethrow the exception
    if (!throwFrom(exn)) {
      return false;
    }
  } else {
    // All registers should have been freed by the catch_all
    MOZ_ASSERT(isAvailableRef(exn));
    MOZ_ASSERT(isAvailableRef(exnTag));
    MOZ_ASSERT(isAvailableRef(catchTag));
#ifndef RABALDR_PIN_INSTANCE
    MOZ_ASSERT(isAvailablePtr(instance));
#endif
  }

  // Reset stack height for skipLandingPad, and bind it
  fr.setStackHeight(prePadHeight);
  masm.bind(&skipLandingPad);

  // Start the try note for this try block, after the landing pad
  if (!startTryNote(&controlItem().tryNoteIndex)) {
    return false;
  }

  // Mark the try note to start at the landing pad we created above
  TryNoteVector& tryNotes = masm.tryNotes();
  TryNote& tryNote = tryNotes[controlItem().tryNoteIndex];
  tryNote.setLandingPad(padOffset, padStackHeight);

  return true;
}

void BaseCompiler::emitCatchSetup(LabelKind kind, Control& tryCatch,
                                  const ResultType& resultType) {
  // Catch ends the try or last catch, so we finish this like endIfThen.
  if (deadCode_) {
    fr.resetStackHeight(tryCatch.stackHeight, resultType);
    popValueStackTo(tryCatch.stackSize);
  } else {
    // If the previous block is a catch, we need to handle the extra exception
    // reference on the stack (for rethrow) and thus the stack size is 1 more.
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize + resultType.length() +
                                    (kind == LabelKind::Try ? 0 : 1));
    // Try jumps to the end of the try-catch block unless a throw is done.
    if (kind == LabelKind::Try) {
      popBlockResults(resultType, tryCatch.stackHeight, ContinuationKind::Jump);
    } else {
      popCatchResults(resultType, tryCatch.stackHeight);
    }
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize);
    freeResultRegisters(resultType);
    MOZ_ASSERT(!tryCatch.deadOnArrival);
  }

  // Reset to this "catch" branch.
  deadCode_ = tryCatch.deadOnArrival;

  // We use the empty result type here because catch does *not* take the
  // try-catch block parameters.
  fr.resetStackHeight(tryCatch.stackHeight, ResultType::Empty());

  if (deadCode_) {
    return;
  }

  bceSafe_ = 0;

  // The end of the previous try/catch jumps to the join point.
  masm.jump(&tryCatch.label);

  // Note end of try block for finding the catch block target. This needs
  // to happen after the stack is reset to the correct height.
  if (kind == LabelKind::Try) {
    finishTryNote(controlItem().tryNoteIndex);
  }
}

bool BaseCompiler::emitCatch() {
  LabelKind kind;
  uint32_t tagIndex;
  ResultType paramType, resultType;
  BaseNothingVector unused_tryValues{};

  if (!iter_.readCatch(&kind, &tagIndex, &paramType, &resultType,
                       &unused_tryValues)) {
    return false;
  }

  Control& tryCatch = controlItem();

  emitCatchSetup(kind, tryCatch, resultType);

  if (deadCode_) {
    return true;
  }

  // Construct info used for the exception landing pad.
  CatchInfo catchInfo(tagIndex);
  if (!tryCatch.catchInfos.emplaceBack(catchInfo)) {
    return false;
  }

  masm.bind(&tryCatch.catchInfos.back().label);

  // Extract the arguments in the exception package and push them.
  const SharedTagType& tagType = moduleEnv_.tags[tagIndex].type;
  const ValTypeVector& params = tagType->argTypes();
  const TagOffsetVector& offsets = tagType->argOffsets();

  // The landing pad uses the block return protocol to communicate the
  // exception object pointer to the catch block.
  ResultType exnResult = ResultType::Single(RefType::extern_());
  captureResultRegisters(exnResult);
  if (!pushBlockResults(exnResult)) {
    return false;
  }
  RegRef exn = popRef();
  RegPtr data = needPtr();

  masm.loadPtr(Address(exn, (int32_t)WasmExceptionObject::offsetOfData()),
               data);

  // This method can increase stk_.length() by an unbounded amount, so we need
  // to perform an allocation here to accomodate the variable number of values.
  // There is enough headroom for the fixed number of values.  The general case
  // is handled in emitBody.
  if (!stk_.reserve(stk_.length() + params.length() + 1)) {
    return false;
  }

  // This reference is pushed onto the stack because a potential rethrow
  // may need to access it. It is always popped at the end of the block.
  pushRef(exn);

  for (uint32_t i = 0; i < params.length(); i++) {
    int32_t offset = offsets[i];
    switch (params[i].kind()) {
      case ValType::I32: {
        RegI32 reg = needI32();
        masm.load32(Address(data, offset), reg);
        pushI32(reg);
        break;
      }
      case ValType::I64: {
        RegI64 reg = needI64();
        masm.load64(Address(data, offset), reg);
        pushI64(reg);
        break;
      }
      case ValType::F32: {
        RegF32 reg = needF32();
        masm.loadFloat32(Address(data, offset), reg);
        pushF32(reg);
        break;
      }
      case ValType::F64: {
        RegF64 reg = needF64();
        masm.loadDouble(Address(data, offset), reg);
        pushF64(reg);
        break;
      }
      case ValType::V128: {
#ifdef ENABLE_WASM_SIMD
        RegV128 reg = needV128();
        masm.loadUnalignedSimd128(Address(data, offset), reg);
        pushV128(reg);
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
      }
      case ValType::Ref: {
        RegRef reg = needRef();
        masm.loadPtr(Address(data, offset), reg);
        pushRef(reg);
        break;
      }
    }
  }
  freePtr(data);

  return true;
}

bool BaseCompiler::emitCatchAll() {
  LabelKind kind;
  ResultType paramType, resultType;
  BaseNothingVector unused_tryValues{};

  if (!iter_.readCatchAll(&kind, &paramType, &resultType, &unused_tryValues)) {
    return false;
  }

  Control& tryCatch = controlItem();

  emitCatchSetup(kind, tryCatch, resultType);

  if (deadCode_) {
    return true;
  }

  CatchInfo catchInfo(CatchAllIndex);
  if (!tryCatch.catchInfos.emplaceBack(catchInfo)) {
    return false;
  }

  masm.bind(&tryCatch.catchInfos.back().label);

  // The landing pad uses the block return protocol to communicate the
  // exception object pointer to the catch block.
  ResultType exnResult = ResultType::Single(RefType::extern_());
  captureResultRegisters(exnResult);
  // This reference is pushed onto the stack because a potential rethrow
  // may need to access it. It is always popped at the end of the block.
  return pushBlockResults(exnResult);
}

bool BaseCompiler::emitDelegate() {
  uint32_t relativeDepth;
  ResultType resultType;
  BaseNothingVector unused_tryValues{};

  if (!iter_.readDelegate(&relativeDepth, &resultType, &unused_tryValues)) {
    return false;
  }

  if (!endBlock(resultType)) {
    return false;
  }

  if (controlItem().deadOnArrival) {
    return true;
  }

  // Mark the end of the try body. This may insert a nop.
  finishTryNote(controlItem().tryNoteIndex);

  // If the target block is a non-try block, skip over it and find the next
  // try block or the very last block (to re-throw out of the function).
  Control& lastBlock = controlOutermost();
  while (controlKind(relativeDepth) != LabelKind::Try &&
         controlKind(relativeDepth) != LabelKind::TryTable &&
         &controlItem(relativeDepth) != &lastBlock) {
    relativeDepth++;
  }
  Control& target = controlItem(relativeDepth);
  TryNoteVector& tryNotes = masm.tryNotes();
  TryNote& delegateTryNote = tryNotes[controlItem().tryNoteIndex];

  if (&target == &lastBlock) {
    // A delegate targeting the function body block means that any exception
    // in this try needs to be propagated to the caller function. We use the
    // delegate code offset of `0` as that will be in the prologue and cannot
    // have a try note.
    delegateTryNote.setDelegate(0);
  } else {
    // Delegate to one byte inside the beginning of the target try note, as
    // that's when matches hit. Try notes are guaranteed to not be empty either
    // and so this will not miss either.
    const TryNote& targetTryNote = tryNotes[target.tryNoteIndex];
    delegateTryNote.setDelegate(targetTryNote.tryBodyBegin() + 1);
  }

  return true;
}

bool BaseCompiler::endTryCatch(ResultType type) {
  Control& tryCatch = controlItem();
  LabelKind tryKind = controlKind(0);

  if (deadCode_) {
    fr.resetStackHeight(tryCatch.stackHeight, type);
    popValueStackTo(tryCatch.stackSize);
  } else {
    // If the previous block is a catch, we must handle the extra exception
    // reference on the stack (for rethrow) and thus the stack size is 1 more.
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize + type.length() +
                                    (tryKind == LabelKind::Try ? 0 : 1));
    // Assume we have a control join, so place results in block result
    // allocations and also handle the implicit exception reference if needed.
    if (tryKind == LabelKind::Try) {
      popBlockResults(type, tryCatch.stackHeight, ContinuationKind::Jump);
    } else {
      popCatchResults(type, tryCatch.stackHeight);
    }
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize);
    // Since we will emit a landing pad after this and jump over it to get to
    // the control join, we free these here and re-capture at the join.
    freeResultRegisters(type);
    masm.jump(&tryCatch.label);
    MOZ_ASSERT(!tryCatch.bceSafeOnExit);
    MOZ_ASSERT(!tryCatch.deadOnArrival);
  }

  deadCode_ = tryCatch.deadOnArrival;

  if (deadCode_) {
    return true;
  }

  // Create landing pad for all catch handlers in this block.
  // When used for a catchless try block, this will generate a landing pad
  // with no handlers and only the fall-back rethrow.

  // The stack height also needs to be set not for a block result, but for the
  // entry to the exception handlers. This is reset again below for the join.
  StackHeight prePadHeight = fr.stackHeight();
  fr.setStackHeight(tryCatch.stackHeight);

  // If we are in a catchless try block, then there were no catch blocks to
  // mark the end of the try note, so we need to end it here.
  if (tryKind == LabelKind::Try) {
    // Mark the end of the try body. This may insert a nop.
    finishTryNote(controlItem().tryNoteIndex);
  }

  // The landing pad begins at this point
  TryNoteVector& tryNotes = masm.tryNotes();
  TryNote& tryNote = tryNotes[controlItem().tryNoteIndex];
  tryNote.setLandingPad(masm.currentOffset(), masm.framePushed());

  // Store the Instance that was left in InstanceReg by the exception
  // handling mechanism, that is this frame's Instance but with the exception
  // filled in Instance::pendingException.
  fr.storeInstancePtr(InstanceReg);

  // Load exception pointer from Instance and make sure that it is
  // saved before the following call will clear it.
  RegRef exn;
  RegRef tag;
  consumePendingException(RegPtr(InstanceReg), &exn, &tag);

  // Get a register to hold the tags for each catch
  RegRef catchTag = needRef();

  // Ensure that the exception is assigned to the block return register
  // before branching to a handler.
  pushRef(exn);
  ResultType exnResult = ResultType::Single(RefType::extern_());
  popBlockResults(exnResult, tryCatch.stackHeight, ContinuationKind::Jump);
  freeResultRegisters(exnResult);

  bool hasCatchAll = false;
  for (CatchInfo& info : tryCatch.catchInfos) {
    if (info.tagIndex != CatchAllIndex) {
      MOZ_ASSERT(!hasCatchAll);
      loadTag(RegPtr(InstanceReg), info.tagIndex, catchTag);
      masm.branchPtr(Assembler::Equal, tag, catchTag, &info.label);
    } else {
      masm.jump(&info.label);
      hasCatchAll = true;
    }
  }
  freeRef(catchTag);
  freeRef(tag);

  // If none of the tag checks succeed and there is no catch_all,
  // then we rethrow the exception.
  if (!hasCatchAll) {
    captureResultRegisters(exnResult);
    if (!pushBlockResults(exnResult) || !throwFrom(popRef())) {
      return false;
    }
  }

  // Reset stack height for join.
  fr.setStackHeight(prePadHeight);

  // Create join point.
  if (tryCatch.label.used()) {
    masm.bind(&tryCatch.label);
  }

  captureResultRegisters(type);
  deadCode_ = tryCatch.deadOnArrival;
  bceSafe_ = tryCatch.bceSafeOnExit;

  return pushBlockResults(type);
}

bool BaseCompiler::endTryTable(ResultType type) {
  if (!controlItem().deadOnArrival) {
    // Mark the end of the try body. This may insert a nop.
    finishTryNote(controlItem().tryNoteIndex);
  }
  return endBlock(type);
}

bool BaseCompiler::emitThrow() {
  uint32_t tagIndex;
  BaseNothingVector unused_argValues{};

  if (!iter_.readThrow(&tagIndex, &unused_argValues)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const TagDesc& tagDesc = moduleEnv_.tags[tagIndex];
  const ResultType& params = tagDesc.type->resultType();
  const TagOffsetVector& offsets = tagDesc.type->argOffsets();

  // Load the tag object
#ifdef RABALDR_PIN_INSTANCE
  RegPtr instance(InstanceReg);
#else
  RegPtr instance = needPtr();
  fr.loadInstancePtr(instance);
#endif
  RegRef tag = needRef();
  loadTag(instance, tagIndex, tag);
#ifndef RABALDR_PIN_INSTANCE
  freePtr(instance);
#endif

  // Create the new exception object that we will throw.
  pushRef(tag);
  if (!emitInstanceCall(SASigExceptionNew)) {
    return false;
  }

  // Get registers for exn and data, excluding the prebarrier register
  needPtr(RegPtr(PreBarrierReg));
  RegRef exn = popRef();
  RegPtr data = needPtr();
  freePtr(RegPtr(PreBarrierReg));

  masm.loadPtr(Address(exn, WasmExceptionObject::offsetOfData()), data);

  for (int32_t i = params.length() - 1; i >= 0; i--) {
    uint32_t offset = offsets[i];
    switch (params[i].kind()) {
      case ValType::I32: {
        RegI32 reg = popI32();
        masm.store32(reg, Address(data, offset));
        freeI32(reg);
        break;
      }
      case ValType::I64: {
        RegI64 reg = popI64();
        masm.store64(reg, Address(data, offset));
        freeI64(reg);
        break;
      }
      case ValType::F32: {
        RegF32 reg = popF32();
        masm.storeFloat32(reg, Address(data, offset));
        freeF32(reg);
        break;
      }
      case ValType::F64: {
        RegF64 reg = popF64();
        masm.storeDouble(reg, Address(data, offset));
        freeF64(reg);
        break;
      }
      case ValType::V128: {
#ifdef ENABLE_WASM_SIMD
        RegV128 reg = popV128();
        masm.storeUnalignedSimd128(reg, Address(data, offset));
        freeV128(reg);
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
      }
      case ValType::Ref: {
        RegPtr valueAddr(PreBarrierReg);
        needPtr(valueAddr);
        masm.computeEffectiveAddress(Address(data, offset), valueAddr);
        RegRef rv = popRef();
        pushPtr(data);
        // emitBarrieredStore preserves exn, rv
        if (!emitBarrieredStore(Some(exn), valueAddr, rv,
                                PreBarrierKind::Normal,
                                PostBarrierKind::Imprecise)) {
          return false;
        }
        popPtr(data);
        freeRef(rv);
        break;
      }
    }
  }
  freePtr(data);

  deadCode_ = true;

  return throwFrom(exn);
}

bool BaseCompiler::emitThrowRef() {
  Nothing unused{};

  if (!iter_.readThrowRef(&unused)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef exn = popRef();
  Label ok;
  masm.branchWasmAnyRefIsNull(false, exn, &ok);
  trap(Trap::NullPointerDereference);
  masm.bind(&ok);
  deadCode_ = true;
  return throwFrom(exn);
}

bool BaseCompiler::emitRethrow() {
  uint32_t relativeDepth;
  if (!iter_.readRethrow(&relativeDepth)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  Control& tryCatch = controlItem(relativeDepth);
  RegRef exn = needRef();
  peekRefAt(tryCatch.stackSize, exn);

  deadCode_ = true;

  return throwFrom(exn);
}

bool BaseCompiler::emitDrop() {
  if (!iter_.readDrop()) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  dropValue();
  return true;
}

void BaseCompiler::doReturn(ContinuationKind kind) {
  if (deadCode_) {
    return;
  }

  StackHeight height = controlOutermost().stackHeight;
  ResultType type = ResultType::Vector(funcType().results());
  popBlockResults(type, height, kind);
  masm.jump(&returnLabel_);
  freeResultRegisters(type);
}

bool BaseCompiler::emitReturn() {
  BaseNothingVector unused_values{};
  if (!iter_.readReturn(&unused_values)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  doReturn(ContinuationKind::Jump);
  deadCode_ = true;

  return true;
}

template <typename T>
bool BaseCompiler::emitCallArgs(const ValTypeVector& argTypes, T results,
                                FunctionCall* baselineCall,
                                CalleeOnStack calleeOnStack) {
  MOZ_ASSERT(!deadCode_);

  ArgTypeVector args(argTypes, results.stackResults());
  uint32_t naturalArgCount = argTypes.length();
  uint32_t abiArgCount = args.lengthWithStackResults();
  startCallArgs(StackArgAreaSizeUnaligned(args), baselineCall);

  // Args are deeper on the stack than the stack result area, if any.
  size_t argsDepth = results.onStackCount();
  // They're deeper than the callee too, for callIndirect.
  if (calleeOnStack == CalleeOnStack::True) {
    argsDepth++;
  }

  for (size_t i = 0; i < abiArgCount; ++i) {
    if (args.isNaturalArg(i)) {
      size_t naturalIndex = args.naturalIndex(i);
      size_t stackIndex = naturalArgCount - 1 - naturalIndex + argsDepth;
      passArg(argTypes[naturalIndex], peek(stackIndex), baselineCall);
    } else {
      // The synthetic stack result area pointer.
      ABIArg argLoc = baselineCall->abi.next(MIRType::Pointer);
      if (argLoc.kind() == ABIArg::Stack) {
        ScratchPtr scratch(*this);
        results.getStackResultArea(fr, scratch);
        masm.storePtr(scratch, Address(masm.getStackPointer(),
                                       argLoc.offsetFromArgBase()));
      } else {
        results.getStackResultArea(fr, RegPtr(argLoc.gpr()));
      }
    }
  }

#ifndef RABALDR_PIN_INSTANCE
  fr.loadInstancePtr(InstanceReg);
#endif
  return true;
}

void BaseCompiler::pushReturnValueOfCall(const FunctionCall& call,
                                         MIRType type) {
  switch (type) {
    case MIRType::Int32: {
      RegI32 rv = captureReturnedI32();
      pushI32(rv);
      break;
    }
    case MIRType::Int64: {
      RegI64 rv = captureReturnedI64();
      pushI64(rv);
      break;
    }
    case MIRType::Float32: {
      RegF32 rv = captureReturnedF32(call);
      pushF32(rv);
      break;
    }
    case MIRType::Double: {
      RegF64 rv = captureReturnedF64(call);
      pushF64(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case MIRType::Simd128: {
      RegV128 rv = captureReturnedV128(call);
      pushV128(rv);
      break;
    }
#endif
    case MIRType::WasmAnyRef: {
      RegRef rv = captureReturnedRef();
      pushRef(rv);
      break;
    }
    default:
      // In particular, passing |type| as MIRType::Void or MIRType::Pointer to
      // this function is an error.
      MOZ_CRASH("Function return type");
  }
}

bool BaseCompiler::pushStackResultsForCall(const ResultType& type, RegPtr temp,
                                           StackResultsLoc* loc) {
  if (!ABIResultIter::HasStackResults(type)) {
    return true;
  }

  // This method can increase stk_.length() by an unbounded amount, so we need
  // to perform an allocation here to accomodate the variable number of values.
  // There is enough headroom for any fixed number of values.  The general case
  // is handled in emitBody.
  if (!stk_.reserve(stk_.length() + type.length())) {
    return false;
  }

  // Measure stack results.
  ABIResultIter i(type);
  size_t count = 0;
  for (; !i.done(); i.next()) {
    if (i.cur().onStack()) {
      count++;
    }
  }
  uint32_t bytes = i.stackBytesConsumedSoFar();

  // Reserve space for the stack results.
  StackHeight resultsBase = fr.stackHeight();
  uint32_t height = fr.prepareStackResultArea(resultsBase, bytes);

  // Push Stk values onto the value stack, and zero out Ref values.
  for (i.switchToPrev(); !i.done(); i.prev()) {
    const ABIResult& result = i.cur();
    if (result.onStack()) {
      Stk v = captureStackResult(result, resultsBase, bytes);
      push(v);
      if (v.kind() == Stk::MemRef) {
        stackMapGenerator_.memRefsOnStk++;
        fr.storeImmediatePtrToStack(intptr_t(0), v.offs(), temp);
      }
    }
  }

  *loc = StackResultsLoc(bytes, count, height);

  return true;
}

// After a call, some results may be written to the stack result locations that
// are pushed on the machine stack after any stack args.  If there are stack
// args and stack results, these results need to be shuffled down, as the args
// are "consumed" by the call.
void BaseCompiler::popStackResultsAfterCall(const StackResultsLoc& results,
                                            uint32_t stackArgBytes) {
  if (results.bytes() != 0) {
    popValueStackBy(results.count());
    if (stackArgBytes != 0) {
      uint32_t srcHeight = results.height();
      MOZ_ASSERT(srcHeight >= stackArgBytes + results.bytes());
      uint32_t destHeight = srcHeight - stackArgBytes;

      fr.shuffleStackResultsTowardFP(srcHeight, destHeight, results.bytes(),
                                     ABINonArgReturnVolatileReg);
    }
  }
}

// For now, always sync() at the beginning of the call to easily save live
// values.
//
// TODO / OPTIMIZE (Bug 1316806): We may be able to avoid a full sync(), since
// all we want is to save live registers that won't be saved by the callee or
// that we need for outgoing args - we don't need to sync the locals.  We can
// just push the necessary registers, it'll be like a lightweight sync.
//
// Even some of the pushing may be unnecessary if the registers will be consumed
// by the call, because then what we want is parallel assignment to the argument
// registers or onto the stack for outgoing arguments.  A sync() is just
// simpler.

bool BaseCompiler::emitCall() {
  uint32_t funcIndex;
  BaseNothingVector args_{};
  if (!iter_.readCall(&funcIndex, &args_)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  sync();

  const FuncType& funcType = *moduleEnv_.funcs[funcIndex].type;
  bool import = moduleEnv_.funcIsImport(funcIndex);

  uint32_t numArgs = funcType.args().length();
  size_t stackArgBytes = stackConsumed(numArgs);

  ResultType resultType(ResultType::Vector(funcType.results()));
  StackResultsLoc results;
  if (!pushStackResultsForCall(resultType, RegPtr(ABINonArgReg0), &results)) {
    return false;
  }

  FunctionCall baselineCall{};
  beginCall(baselineCall, UseABI::Wasm,
            import ? RestoreRegisterStateAndRealm::True
                   : RestoreRegisterStateAndRealm::False);

  if (!emitCallArgs(funcType.args(), NormalCallResults(results), &baselineCall,
                    CalleeOnStack::False)) {
    return false;
  }

  CodeOffset raOffset;
  if (import) {
    raOffset = callImport(moduleEnv_.offsetOfFuncImportInstanceData(funcIndex),
                          baselineCall);
  } else {
    raOffset = callDefinition(funcIndex, baselineCall);
  }

  if (!createStackMap("emitCall", raOffset)) {
    return false;
  }

  popStackResultsAfterCall(results, stackArgBytes);

  endCall(baselineCall, stackArgBytes);

  popValueStackBy(numArgs);

  captureCallResultRegisters(resultType);
  return pushCallResults(baselineCall, resultType, results);
}

#ifdef ENABLE_WASM_TAIL_CALLS
bool BaseCompiler::emitReturnCall() {
  uint32_t funcIndex;
  BaseNothingVector args_{};
  if (!iter_.readReturnCall(&funcIndex, &args_)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  sync();
  if (!insertDebugCollapseFrame()) {
    return false;
  }

  const FuncType& funcType = *moduleEnv_.funcs[funcIndex].type;
  bool import = moduleEnv_.funcIsImport(funcIndex);

  uint32_t numArgs = funcType.args().length();

  FunctionCall baselineCall{};
  beginCall(baselineCall, UseABI::Wasm,
            import ? RestoreRegisterStateAndRealm::True
                   : RestoreRegisterStateAndRealm::False);

  if (!emitCallArgs(funcType.args(), TailCallResults(funcType), &baselineCall,
                    CalleeOnStack::False)) {
    return false;
  }

  ReturnCallAdjustmentInfo retCallInfo =
      BuildReturnCallAdjustmentInfo(this->funcType(), funcType);

  if (import) {
    CallSiteDesc desc(bytecodeOffset(), CallSiteDesc::Import);
    CalleeDesc callee = CalleeDesc::import(
        moduleEnv_.offsetOfFuncImportInstanceData(funcIndex));
    masm.wasmReturnCallImport(desc, callee, retCallInfo);
  } else {
    CallSiteDesc desc(bytecodeOffset(), CallSiteDesc::ReturnFunc);
    masm.wasmReturnCall(desc, funcIndex, retCallInfo);
  }

  MOZ_ASSERT(stackMapGenerator_.framePushedExcludingOutboundCallArgs.isSome());
  stackMapGenerator_.framePushedExcludingOutboundCallArgs.reset();

  popValueStackBy(numArgs);

  deadCode_ = true;
  return true;
}
#endif

bool BaseCompiler::emitCallIndirect() {
  uint32_t funcTypeIndex;
  uint32_t tableIndex;
  Nothing callee_;
  BaseNothingVector args_{};
  if (!iter_.readCallIndirect(&funcTypeIndex, &tableIndex, &callee_, &args_)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  sync();

  const FuncType& funcType = (*moduleEnv_.types)[funcTypeIndex].funcType();

  // Stack: ... arg1 .. argn callee

  uint32_t numArgs = funcType.args().length() + 1;
  size_t stackArgBytes = stackConsumed(numArgs);

  ResultType resultType(ResultType::Vector(funcType.results()));
  StackResultsLoc results;
  if (!pushStackResultsForCall(resultType, RegPtr(ABINonArgReg0), &results)) {
    return false;
  }

  FunctionCall baselineCall{};
  // State and realm are restored as needed by by callIndirect (really by
  // MacroAssembler::wasmCallIndirect).
  beginCall(baselineCall, UseABI::Wasm, RestoreRegisterStateAndRealm::False);

  if (!emitCallArgs(funcType.args(), NormalCallResults(results), &baselineCall,
                    CalleeOnStack::True)) {
    return false;
  }

  const Stk& callee = peek(results.count());
  CodeOffset fastCallOffset;
  CodeOffset slowCallOffset;
  if (!callIndirect(funcTypeIndex, tableIndex, callee, baselineCall,
                    /*tailCall*/ false, &fastCallOffset, &slowCallOffset)) {
    return false;
  }
  if (!createStackMap("emitCallIndirect", fastCallOffset)) {
    return false;
  }
  if (!createStackMap("emitCallIndirect", slowCallOffset)) {
    return false;
  }

  popStackResultsAfterCall(results, stackArgBytes);

  endCall(baselineCall, stackArgBytes);

  popValueStackBy(numArgs);

  captureCallResultRegisters(resultType);
  return pushCallResults(baselineCall, resultType, results);
}

#ifdef ENABLE_WASM_TAIL_CALLS
bool BaseCompiler::emitReturnCallIndirect() {
  uint32_t funcTypeIndex;
  uint32_t tableIndex;
  Nothing callee_;
  BaseNothingVector args_{};
  if (!iter_.readReturnCallIndirect(&funcTypeIndex, &tableIndex, &callee_,
                                    &args_)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  sync();
  if (!insertDebugCollapseFrame()) {
    return false;
  }

  const FuncType& funcType = (*moduleEnv_.types)[funcTypeIndex].funcType();

  // Stack: ... arg1 .. argn callee

  uint32_t numArgs = funcType.args().length() + 1;

  FunctionCall baselineCall{};
  // State and realm are restored as needed by by callIndirect (really by
  // MacroAssembler::wasmCallIndirect).
  beginCall(baselineCall, UseABI::Wasm, RestoreRegisterStateAndRealm::False);

  if (!emitCallArgs(funcType.args(), TailCallResults(funcType), &baselineCall,
                    CalleeOnStack::True)) {
    return false;
  }

  const Stk& callee = peek(0);
  CodeOffset fastCallOffset;
  CodeOffset slowCallOffset;
  if (!callIndirect(funcTypeIndex, tableIndex, callee, baselineCall,
                    /*tailCall*/ true, &fastCallOffset, &slowCallOffset)) {
    return false;
  }

  MOZ_ASSERT(stackMapGenerator_.framePushedExcludingOutboundCallArgs.isSome());
  stackMapGenerator_.framePushedExcludingOutboundCallArgs.reset();

  popValueStackBy(numArgs);

  deadCode_ = true;
  return true;
}
#endif

#ifdef ENABLE_WASM_GC
bool BaseCompiler::emitCallRef() {
  const FuncType* funcType;
  Nothing unused_callee;
  BaseNothingVector unused_args{};
  if (!iter_.readCallRef(&funcType, &unused_callee, &unused_args)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  sync();

  // Stack: ... arg1 .. argn callee

  uint32_t numArgs = funcType->args().length() + 1;
  size_t stackArgBytes = stackConsumed(numArgs);

  ResultType resultType(ResultType::Vector(funcType->results()));
  StackResultsLoc results;
  if (!pushStackResultsForCall(resultType, RegPtr(ABINonArgReg0), &results)) {
    return false;
  }

  FunctionCall baselineCall{};
  // State and realm are restored as needed by by callRef (really by
  // MacroAssembler::wasmCallRef).
  beginCall(baselineCall, UseABI::Wasm, RestoreRegisterStateAndRealm::False);

  if (!emitCallArgs(funcType->args(), NormalCallResults(results), &baselineCall,
                    CalleeOnStack::True)) {
    return false;
  }

  const Stk& callee = peek(results.count());
  CodeOffset fastCallOffset;
  CodeOffset slowCallOffset;
  callRef(callee, baselineCall, &fastCallOffset, &slowCallOffset);
  if (!createStackMap("emitCallRef", fastCallOffset)) {
    return false;
  }
  if (!createStackMap("emitCallRef", slowCallOffset)) {
    return false;
  }

  popStackResultsAfterCall(results, stackArgBytes);

  endCall(baselineCall, stackArgBytes);

  popValueStackBy(numArgs);

  captureCallResultRegisters(resultType);
  return pushCallResults(baselineCall, resultType, results);
}

#  ifdef ENABLE_WASM_TAIL_CALLS
bool BaseCompiler::emitReturnCallRef() {
  const FuncType* funcType;
  Nothing unused_callee;
  BaseNothingVector unused_args{};
  if (!iter_.readReturnCallRef(&funcType, &unused_callee, &unused_args)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  sync();
  if (!insertDebugCollapseFrame()) {
    return false;
  }

  // Stack: ... arg1 .. argn callee

  uint32_t numArgs = funcType->args().length() + 1;

  FunctionCall baselineCall{};
  // State and realm are restored as needed by by callRef (really by
  // MacroAssembler::wasmCallRef).
  beginCall(baselineCall, UseABI::Wasm, RestoreRegisterStateAndRealm::False);

  if (!emitCallArgs(funcType->args(), TailCallResults(*funcType), &baselineCall,
                    CalleeOnStack::True)) {
    return false;
  }

  const Stk& callee = peek(0);
  returnCallRef(callee, baselineCall, funcType);

  MOZ_ASSERT(stackMapGenerator_.framePushedExcludingOutboundCallArgs.isSome());
  stackMapGenerator_.framePushedExcludingOutboundCallArgs.reset();

  popValueStackBy(numArgs);

  deadCode_ = true;
  return true;
}
#  endif

#endif

void BaseCompiler::emitRound(RoundingMode roundingMode, ValType operandType) {
  if (operandType == ValType::F32) {
    RegF32 f0 = popF32();
    roundF32(roundingMode, f0);
    pushF32(f0);
  } else if (operandType == ValType::F64) {
    RegF64 f0 = popF64();
    roundF64(roundingMode, f0);
    pushF64(f0);
  } else {
    MOZ_CRASH("unexpected type");
  }
}

bool BaseCompiler::emitUnaryMathBuiltinCall(SymbolicAddress callee,
                                            ValType operandType) {
  Nothing operand_;
  if (!iter_.readUnary(operandType, &operand_)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RoundingMode roundingMode;
  if (IsRoundingFunction(callee, &roundingMode) &&
      supportsRoundInstruction(roundingMode)) {
    emitRound(roundingMode, operandType);
    return true;
  }

  sync();

  ValTypeVector& signature = operandType == ValType::F32 ? SigF_ : SigD_;
  ValType retType = operandType;
  uint32_t numArgs = signature.length();
  size_t stackSpace = stackConsumed(numArgs);

  FunctionCall baselineCall{};
  beginCall(baselineCall, UseABI::Builtin, RestoreRegisterStateAndRealm::False);

  if (!emitCallArgs(signature, NoCallResults(), &baselineCall,
                    CalleeOnStack::False)) {
    return false;
  }

  CodeOffset raOffset = builtinCall(callee, baselineCall);
  if (!createStackMap("emitUnaryMathBuiltin[..]", raOffset)) {
    return false;
  }

  endCall(baselineCall, stackSpace);

  popValueStackBy(numArgs);

  pushReturnValueOfCall(baselineCall, retType.toMIRType());

  return true;
}

#ifdef RABALDR_INT_DIV_I64_CALLOUT
bool BaseCompiler::emitDivOrModI64BuiltinCall(SymbolicAddress callee,
                                              ValType operandType) {
  MOZ_ASSERT(operandType == ValType::I64);
  MOZ_ASSERT(!deadCode_);

  sync();

  needI64(specific_.abiReturnRegI64);

  RegI64 rhs = popI64();
  RegI64 srcDest = popI64ToSpecific(specific_.abiReturnRegI64);

  Label done;

  checkDivideByZero(rhs);

  if (callee == SymbolicAddress::DivI64) {
    checkDivideSignedOverflow(rhs, srcDest, &done, ZeroOnOverflow(false));
  } else if (callee == SymbolicAddress::ModI64) {
    checkDivideSignedOverflow(rhs, srcDest, &done, ZeroOnOverflow(true));
  }

  masm.setupWasmABICall();
  masm.passABIArg(srcDest.high);
  masm.passABIArg(srcDest.low);
  masm.passABIArg(rhs.high);
  masm.passABIArg(rhs.low);
  CodeOffset raOffset = masm.callWithABI(
      bytecodeOffset(), callee, mozilla::Some(fr.getInstancePtrOffset()));
  if (!createStackMap("emitDivOrModI64Bui[..]", raOffset)) {
    return false;
  }

  masm.bind(&done);

  freeI64(rhs);
  pushI64(srcDest);
  return true;
}
#endif  // RABALDR_INT_DIV_I64_CALLOUT

#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
bool BaseCompiler::emitConvertInt64ToFloatingCallout(SymbolicAddress callee,
                                                     ValType operandType,
                                                     ValType resultType) {
  sync();

  RegI64 input = popI64();

  FunctionCall call{};

  masm.setupWasmABICall();
#  ifdef JS_PUNBOX64
  MOZ_CRASH("BaseCompiler platform hook: emitConvertInt64ToFloatingCallout");
#  else
  masm.passABIArg(input.high);
  masm.passABIArg(input.low);
#  endif
  CodeOffset raOffset = masm.callWithABI(
      bytecodeOffset(), callee, mozilla::Some(fr.getInstancePtrOffset()),
      resultType == ValType::F32 ? ABIType::Float32 : ABIType::Float64);
  if (!createStackMap("emitConvertInt64To[..]", raOffset)) {
    return false;
  }

  freeI64(input);

  if (resultType == ValType::F32) {
    pushF32(captureReturnedF32(call));
  } else {
    pushF64(captureReturnedF64(call));
  }

  return true;
}
#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
// `Callee` always takes a double, so a float32 input must be converted.
bool BaseCompiler::emitConvertFloatingToInt64Callout(SymbolicAddress callee,
                                                     ValType operandType,
                                                     ValType resultType) {
  RegF64 doubleInput;
  if (operandType == ValType::F32) {
    doubleInput = needF64();
    RegF32 input = popF32();
    masm.convertFloat32ToDouble(input, doubleInput);
    freeF32(input);
  } else {
    doubleInput = popF64();
  }

  // We may need the value after the call for the ool check.
  RegF64 otherReg = needF64();
  moveF64(doubleInput, otherReg);
  pushF64(otherReg);

  sync();

  FunctionCall call{};

  masm.setupWasmABICall();
  masm.passABIArg(doubleInput, ABIType::Float64);
  CodeOffset raOffset = masm.callWithABI(
      bytecodeOffset(), callee, mozilla::Some(fr.getInstancePtrOffset()));
  if (!createStackMap("emitConvertFloatin[..]", raOffset)) {
    return false;
  }

  freeF64(doubleInput);

  RegI64 rv = captureReturnedI64();

  RegF64 inputVal = popF64();

  TruncFlags flags = 0;
  if (callee == SymbolicAddress::TruncateDoubleToUint64) {
    flags |= TRUNC_UNSIGNED;
  }
  if (callee == SymbolicAddress::SaturatingTruncateDoubleToInt64 ||
      callee == SymbolicAddress::SaturatingTruncateDoubleToUint64) {
    flags |= TRUNC_SATURATING;
  }

  // If we're saturating, the callout will always produce the final result
  // value. Otherwise, the callout value will return 0x8000000000000000
  // and we need to produce traps.
  OutOfLineCode* ool = nullptr;
  if (!(flags & TRUNC_SATURATING)) {
    // The OOL check just succeeds or fails, it does not generate a value.
    ool = addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(
        AnyReg(inputVal), rv, flags, bytecodeOffset()));
    if (!ool) {
      return false;
    }

    masm.branch64(Assembler::Equal, rv, Imm64(0x8000000000000000),
                  ool->entry());
    masm.bind(ool->rejoin());
  }

  pushI64(rv);
  freeF64(inputVal);

  return true;
}
#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

bool BaseCompiler::emitGetLocal() {
  uint32_t slot;
  if (!iter_.readGetLocal(locals_, &slot)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  // Local loads are pushed unresolved, ie, they may be deferred
  // until needed, until they may be affected by a store, or until a
  // sync.  This is intended to reduce register pressure.

  switch (locals_[slot].kind()) {
    case ValType::I32:
      pushLocalI32(slot);
      break;
    case ValType::I64:
      pushLocalI64(slot);
      break;
    case ValType::V128:
#ifdef ENABLE_WASM_SIMD
      pushLocalV128(slot);
      break;
#else
      MOZ_CRASH("No SIMD support");
#endif
    case ValType::F64:
      pushLocalF64(slot);
      break;
    case ValType::F32:
      pushLocalF32(slot);
      break;
    case ValType::Ref:
      pushLocalRef(slot);
      break;
  }

  return true;
}

template <bool isSetLocal>
bool BaseCompiler::emitSetOrTeeLocal(uint32_t slot) {
  if (deadCode_) {
    return true;
  }

  bceLocalIsUpdated(slot);
  switch (locals_[slot].kind()) {
    case ValType::I32: {
      RegI32 rv = popI32();
      syncLocal(slot);
      fr.storeLocalI32(rv, localFromSlot(slot, MIRType::Int32));
      if (isSetLocal) {
        freeI32(rv);
      } else {
        pushI32(rv);
      }
      break;
    }
    case ValType::I64: {
      RegI64 rv = popI64();
      syncLocal(slot);
      fr.storeLocalI64(rv, localFromSlot(slot, MIRType::Int64));
      if (isSetLocal) {
        freeI64(rv);
      } else {
        pushI64(rv);
      }
      break;
    }
    case ValType::F64: {
      RegF64 rv = popF64();
      syncLocal(slot);
      fr.storeLocalF64(rv, localFromSlot(slot, MIRType::Double));
      if (isSetLocal) {
        freeF64(rv);
      } else {
        pushF64(rv);
      }
      break;
    }
    case ValType::F32: {
      RegF32 rv = popF32();
      syncLocal(slot);
      fr.storeLocalF32(rv, localFromSlot(slot, MIRType::Float32));
      if (isSetLocal) {
        freeF32(rv);
      } else {
        pushF32(rv);
      }
      break;
    }
    case ValType::V128: {
#ifdef ENABLE_WASM_SIMD
      RegV128 rv = popV128();
      syncLocal(slot);
      fr.storeLocalV128(rv, localFromSlot(slot, MIRType::Simd128));
      if (isSetLocal) {
        freeV128(rv);
      } else {
        pushV128(rv);
      }
      break;
#else
      MOZ_CRASH("No SIMD support");
#endif
    }
    case ValType::Ref: {
      RegRef rv = popRef();
      syncLocal(slot);
      fr.storeLocalRef(rv, localFromSlot(slot, MIRType::WasmAnyRef));
      if (isSetLocal) {
        freeRef(rv);
      } else {
        pushRef(rv);
      }
      break;
    }
  }

  return true;
}

bool BaseCompiler::emitSetLocal() {
  uint32_t slot;
  Nothing unused_value;
  if (!iter_.readSetLocal(locals_, &slot, &unused_value)) {
    return false;
  }
  return emitSetOrTeeLocal<true>(slot);
}

bool BaseCompiler::emitTeeLocal() {
  uint32_t slot;
  Nothing unused_value;
  if (!iter_.readTeeLocal(locals_, &slot, &unused_value)) {
    return false;
  }
  return emitSetOrTeeLocal<false>(slot);
}

bool BaseCompiler::emitGetGlobal() {
  uint32_t id;
  if (!iter_.readGetGlobal(&id)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const GlobalDesc& global = moduleEnv_.globals[id];

  if (global.isConstant()) {
    LitVal value = global.constantValue();
    switch (value.type().kind()) {
      case ValType::I32:
        pushI32(value.i32());
        break;
      case ValType::I64:
        pushI64(value.i64());
        break;
      case ValType::F32:
        pushF32(value.f32());
        break;
      case ValType::F64:
        pushF64(value.f64());
        break;
      case ValType::Ref:
        pushRef(intptr_t(value.ref().forCompiledCode()));
        break;
#ifdef ENABLE_WASM_SIMD
      case ValType::V128:
        pushV128(value.v128());
        break;
#endif
      default:
        MOZ_CRASH("Global constant type");
    }
    return true;
  }

  switch (global.type().kind()) {
    case ValType::I32: {
      RegI32 rv = needI32();
      ScratchPtr tmp(*this);
      masm.load32(addressOfGlobalVar(global, tmp), rv);
      pushI32(rv);
      break;
    }
    case ValType::I64: {
      RegI64 rv = needI64();
      ScratchPtr tmp(*this);
      masm.load64(addressOfGlobalVar(global, tmp), rv);
      pushI64(rv);
      break;
    }
    case ValType::F32: {
      RegF32 rv = needF32();
      ScratchPtr tmp(*this);
      masm.loadFloat32(addressOfGlobalVar(global, tmp), rv);
      pushF32(rv);
      break;
    }
    case ValType::F64: {
      RegF64 rv = needF64();
      ScratchPtr tmp(*this);
      masm.loadDouble(addressOfGlobalVar(global, tmp), rv);
      pushF64(rv);
      break;
    }
    case ValType::Ref: {
      RegRef rv = needRef();
      ScratchPtr tmp(*this);
      masm.loadPtr(addressOfGlobalVar(global, tmp), rv);
      pushRef(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 rv = needV128();
      ScratchPtr tmp(*this);
      masm.loadUnalignedSimd128(addressOfGlobalVar(global, tmp), rv);
      pushV128(rv);
      break;
    }
#endif
    default:
      MOZ_CRASH("Global variable type");
      break;
  }
  return true;
}

bool BaseCompiler::emitSetGlobal() {
  uint32_t id;
  Nothing unused_value;
  if (!iter_.readSetGlobal(&id, &unused_value)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const GlobalDesc& global = moduleEnv_.globals[id];

  switch (global.type().kind()) {
    case ValType::I32: {
      RegI32 rv = popI32();
      ScratchPtr tmp(*this);
      masm.store32(rv, addressOfGlobalVar(global, tmp));
      freeI32(rv);
      break;
    }
    case ValType::I64: {
      RegI64 rv = popI64();
      ScratchPtr tmp(*this);
      masm.store64(rv, addressOfGlobalVar(global, tmp));
      freeI64(rv);
      break;
    }
    case ValType::F32: {
      RegF32 rv = popF32();
      ScratchPtr tmp(*this);
      masm.storeFloat32(rv, addressOfGlobalVar(global, tmp));
      freeF32(rv);
      break;
    }
    case ValType::F64: {
      RegF64 rv = popF64();
      ScratchPtr tmp(*this);
      masm.storeDouble(rv, addressOfGlobalVar(global, tmp));
      freeF64(rv);
      break;
    }
    case ValType::Ref: {
      RegPtr valueAddr(PreBarrierReg);
      needPtr(valueAddr);
      {
        ScratchPtr tmp(*this);
        masm.computeEffectiveAddress(addressOfGlobalVar(global, tmp),
                                     valueAddr);
      }
      RegRef rv = popRef();
      // emitBarrieredStore preserves rv
      if (!emitBarrieredStore(Nothing(), valueAddr, rv, PreBarrierKind::Normal,
                              PostBarrierKind::Imprecise)) {
        return false;
      }
      freeRef(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 rv = popV128();
      ScratchPtr tmp(*this);
      masm.storeUnalignedSimd128(rv, addressOfGlobalVar(global, tmp));
      freeV128(rv);
      break;
    }
#endif
    default:
      MOZ_CRASH("Global variable type");
      break;
  }
  return true;
}

bool BaseCompiler::emitLoad(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoad(type, Scalar::byteSize(viewType), &addr)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(),
                          hugeMemoryEnabled(addr.memoryIndex));
  loadCommon(&access, AccessCheck(), type);
  return true;
}

bool BaseCompiler::emitStore(ValType resultType, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused_value;
  if (!iter_.readStore(resultType, Scalar::byteSize(viewType), &addr,
                       &unused_value)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(),
                          hugeMemoryEnabled(addr.memoryIndex));
  storeCommon(&access, AccessCheck(), resultType);
  return true;
}

bool BaseCompiler::emitSelect(bool typed) {
  StackType type;
  Nothing unused_trueValue;
  Nothing unused_falseValue;
  Nothing unused_condition;
  if (!iter_.readSelect(typed, &type, &unused_trueValue, &unused_falseValue,
                        &unused_condition)) {
    return false;
  }

  if (deadCode_) {
    resetLatentOp();
    return true;
  }

  // I32 condition on top, then false, then true.

  Label done;
  BranchState b(&done);
  emitBranchSetup(&b);

  switch (type.valType().kind()) {
    case ValType::I32: {
      RegI32 r, rs;
      pop2xI32(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveI32(rs, r);
      masm.bind(&done);
      freeI32(rs);
      pushI32(r);
      break;
    }
    case ValType::I64: {
#ifdef JS_CODEGEN_X86
      // There may be as many as four Int64 values in registers at a time: two
      // for the latent branch operands, and two for the true/false values we
      // normally pop before executing the branch.  On x86 this is one value
      // too many, so we need to generate more complicated code here, and for
      // simplicity's sake we do so even if the branch operands are not Int64.
      // However, the resulting control flow diamond is complicated since the
      // arms of the diamond will have to stay synchronized with respect to
      // their evaluation stack and regalloc state.  To simplify further, we
      // use a double branch and a temporary boolean value for now.
      RegI32 temp = needI32();
      moveImm32(0, temp);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveImm32(1, temp);
      masm.bind(&done);

      Label trueValue;
      RegI64 r, rs;
      pop2xI64(&r, &rs);
      masm.branch32(Assembler::Equal, temp, Imm32(0), &trueValue);
      moveI64(rs, r);
      masm.bind(&trueValue);
      freeI32(temp);
      freeI64(rs);
      pushI64(r);
#else
      RegI64 r, rs;
      pop2xI64(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveI64(rs, r);
      masm.bind(&done);
      freeI64(rs);
      pushI64(r);
#endif
      break;
    }
    case ValType::F32: {
      RegF32 r, rs;
      pop2xF32(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveF32(rs, r);
      masm.bind(&done);
      freeF32(rs);
      pushF32(r);
      break;
    }
    case ValType::F64: {
      RegF64 r, rs;
      pop2xF64(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveF64(rs, r);
      masm.bind(&done);
      freeF64(rs);
      pushF64(r);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 r, rs;
      pop2xV128(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveV128(rs, r);
      masm.bind(&done);
      freeV128(rs);
      pushV128(r);
      break;
    }
#endif
    case ValType::Ref: {
      RegRef r, rs;
      pop2xRef(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveRef(rs, r);
      masm.bind(&done);
      freeRef(rs);
      pushRef(r);
      break;
    }
    default: {
      MOZ_CRASH("select type");
    }
  }

  return true;
}

void BaseCompiler::emitCompareI32(Assembler::Condition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::I32);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  int32_t c;
  if (popConst(&c)) {
    RegI32 r = popI32();
    masm.cmp32Set(compareOp, r, Imm32(c), r);
    pushI32(r);
  } else {
    RegI32 r, rs;
    pop2xI32(&r, &rs);
    masm.cmp32Set(compareOp, r, rs, r);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitCompareI64(Assembler::Condition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::I64);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  RegI64 rs0, rs1;
  pop2xI64(&rs0, &rs1);
  RegI32 rd(fromI64(rs0));
  cmp64Set(compareOp, rs0, rs1, rd);
  freeI64(rs1);
  freeI64Except(rs0, rd);
  pushI32(rd);
}

void BaseCompiler::emitCompareF32(Assembler::DoubleCondition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::F32);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  Label across;
  RegF32 rs0, rs1;
  pop2xF32(&rs0, &rs1);
  RegI32 rd = needI32();
  moveImm32(1, rd);
  masm.branchFloat(compareOp, rs0, rs1, &across);
  moveImm32(0, rd);
  masm.bind(&across);
  freeF32(rs0);
  freeF32(rs1);
  pushI32(rd);
}

void BaseCompiler::emitCompareF64(Assembler::DoubleCondition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::F64);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  Label across;
  RegF64 rs0, rs1;
  pop2xF64(&rs0, &rs1);
  RegI32 rd = needI32();
  moveImm32(1, rd);
  masm.branchDouble(compareOp, rs0, rs1, &across);
  moveImm32(0, rd);
  masm.bind(&across);
  freeF64(rs0);
  freeF64(rs1);
  pushI32(rd);
}

void BaseCompiler::emitCompareRef(Assembler::Condition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(!sniffConditionalControlCmp(compareOp, compareType));

  RegRef rs1, rs2;
  pop2xRef(&rs1, &rs2);
  RegI32 rd = needI32();
  masm.cmpPtrSet(compareOp, rs1, rs2, rd);
  freeRef(rs1);
  freeRef(rs2);
  pushI32(rd);
}

bool BaseCompiler::emitInstanceCall(const SymbolicAddressSignature& builtin) {
  // See declaration (WasmBCClass.h) for info on the relationship between the
  // compiler's value stack and the argument order for the to-be-called
  // function.
  const MIRType* argTypes = builtin.argTypes;
  MOZ_ASSERT(argTypes[0] == MIRType::Pointer);

  sync();

  uint32_t numNonInstanceArgs = builtin.numArgs - 1 /* instance */;
  size_t stackSpace = stackConsumed(numNonInstanceArgs);

  FunctionCall baselineCall{};
  beginCall(baselineCall, UseABI::System, RestoreRegisterStateAndRealm::True);

  ABIArg instanceArg = reservePointerArgument(&baselineCall);

  startCallArgs(StackArgAreaSizeUnaligned(builtin), &baselineCall);
  for (uint32_t i = 1; i < builtin.numArgs; i++) {
    ValType t;
    switch (argTypes[i]) {
      case MIRType::Int32:
        t = ValType::I32;
        break;
      case MIRType::Int64:
        t = ValType::I64;
        break;
      case MIRType::Float32:
        t = ValType::F32;
        break;
      case MIRType::WasmAnyRef:
        t = RefType::extern_();
        break;
      case MIRType::Pointer:
        // Instance function args can now be uninterpreted pointers (eg, for
        // the cases PostBarrier and PostBarrierFilter) so we simply treat
        // them like the equivalently sized integer.
        t = ValType::fromMIRType(TargetWordMIRType());
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }
    passArg(t, peek(numNonInstanceArgs - i), &baselineCall);
  }
  CodeOffset raOffset =
      builtinInstanceMethodCall(builtin, instanceArg, baselineCall);
  if (!createStackMap("emitInstanceCall", raOffset)) {
    return false;
  }

  endCall(baselineCall, stackSpace);

  popValueStackBy(numNonInstanceArgs);

  // Note, many clients of emitInstanceCall currently assume that pushing the
  // result here does not destroy ReturnReg.
  //
  // Furthermore, clients assume that if builtin.retType != MIRType::None, the
  // callee will have returned a result and left it in ReturnReg for us to
  // find, and that that register will not be destroyed here (or above).

  // For the return type only, MIRType::None is used to indicate that the
  // call doesn't return a result, that is, returns a C/C++ "void".

  if (builtin.retType != MIRType::None) {
    pushReturnValueOfCall(baselineCall, builtin.retType);
  }
  return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// Reference types.

bool BaseCompiler::emitRefFunc() {
  return emitInstanceCallOp<uint32_t>(SASigRefFunc,
                                      [this](uint32_t* funcIndex) -> bool {
                                        return iter_.readRefFunc(funcIndex);
                                      });
}

bool BaseCompiler::emitRefNull() {
  RefType type;
  if (!iter_.readRefNull(&type)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  pushRef(AnyRef::NullRefValue);
  return true;
}

bool BaseCompiler::emitRefIsNull() {
  Nothing nothing;
  if (!iter_.readRefIsNull(&nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef r = popRef();
  RegI32 rd = narrowRef(r);

  masm.cmpPtrSet(Assembler::Equal, r, ImmWord(AnyRef::NullRefValue), rd);
  pushI32(rd);
  return true;
}

#ifdef ENABLE_WASM_GC
bool BaseCompiler::emitRefAsNonNull() {
  Nothing nothing;
  if (!iter_.readRefAsNonNull(&nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef rp = popRef();
  Label ok;
  masm.branchWasmAnyRefIsNull(false, rp, &ok);
  trap(Trap::NullPointerDereference);
  masm.bind(&ok);
  pushRef(rp);

  return true;
}
#endif

//////////////////////////////////////////////////////////////////////////////
//
// Atomic operations.

bool BaseCompiler::emitAtomicCmpXchg(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused{};
  if (!iter_.readAtomicCmpXchg(&addr, type, Scalar::byteSize(viewType), &unused,
                               &unused)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Full());
  atomicCmpXchg(&access, type);
  return true;
}

bool BaseCompiler::emitAtomicLoad(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readAtomicLoad(&addr, type, Scalar::byteSize(viewType))) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Load());
  atomicLoad(&access, type);
  return true;
}

bool BaseCompiler::emitAtomicRMW(ValType type, Scalar::Type viewType,
                                 AtomicOp op) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused_value;
  if (!iter_.readAtomicRMW(&addr, type, Scalar::byteSize(viewType),
                           &unused_value)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Full());
  atomicRMW(&access, type, op);
  return true;
}

bool BaseCompiler::emitAtomicStore(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused_value;
  if (!iter_.readAtomicStore(&addr, type, Scalar::byteSize(viewType),
                             &unused_value)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Store());
  atomicStore(&access, type);
  return true;
}

bool BaseCompiler::emitAtomicXchg(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused_value;
  if (!iter_.readAtomicRMW(&addr, type, Scalar::byteSize(viewType),
                           &unused_value)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Full());
  atomicXchg(&access, type);
  return true;
}

bool BaseCompiler::emitWait(ValType type, uint32_t byteSize) {
  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readWait(&addr, type, byteSize, &nothing, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(
      addr.memoryIndex,
      type.kind() == ValType::I32 ? Scalar::Int32 : Scalar::Int64, addr.align,
      addr.offset, bytecodeOffset(), hugeMemoryEnabled(addr.memoryIndex));
  return atomicWait(type, &access);
}

bool BaseCompiler::emitWake() {
  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readWake(&addr, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, Scalar::Int32, addr.align,
                          addr.offset, bytecodeOffset(),
                          hugeMemoryEnabled(addr.memoryIndex));
  return atomicWake(&access);
}

bool BaseCompiler::emitFence() {
  if (!iter_.readFence()) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  masm.memoryBarrier(MembarFull);
  return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// Bulk memory operations.

bool BaseCompiler::emitMemoryGrow() {
  uint32_t memoryIndex;
  Nothing nothing;
  if (!iter_.readMemoryGrow(&memoryIndex, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }

  pushI32(memoryIndex);
  return emitInstanceCall(isMem32(memoryIndex) ? SASigMemoryGrowM32
                                               : SASigMemoryGrowM64);
}

bool BaseCompiler::emitMemorySize() {
  uint32_t memoryIndex;
  if (!iter_.readMemorySize(&memoryIndex)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }

  pushI32(memoryIndex);
  return emitInstanceCall(isMem32(memoryIndex) ? SASigMemorySizeM32
                                               : SASigMemorySizeM64);
}

bool BaseCompiler::emitMemCopy() {
  uint32_t dstMemIndex = 0;
  uint32_t srcMemIndex = 0;
  Nothing nothing;
  if (!iter_.readMemOrTableCopy(true, &dstMemIndex, &nothing, &srcMemIndex,
                                &nothing, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }

  if (dstMemIndex == 0 && srcMemIndex == 0 && isMem32(dstMemIndex)) {
    int32_t signedLength;
    if (peekConst(&signedLength) && signedLength != 0 &&
        uint32_t(signedLength) <= MaxInlineMemoryCopyLength) {
      memCopyInlineM32();
      return true;
    }
  }

  return memCopyCall(dstMemIndex, srcMemIndex);
}

bool BaseCompiler::memCopyCall(uint32_t dstMemIndex, uint32_t srcMemIndex) {
  // Common and optimized path for when the src/dest memories are the same
  if (dstMemIndex == srcMemIndex) {
    bool mem32 = isMem32(dstMemIndex);
    pushHeapBase(dstMemIndex);
    return emitInstanceCall(
        usesSharedMemory(dstMemIndex)
            ? (mem32 ? SASigMemCopySharedM32 : SASigMemCopySharedM64)
            : (mem32 ? SASigMemCopyM32 : SASigMemCopyM64));
  }

  // Do the general-purpose fallback for copying between any combination of
  // memories. This works by moving everything to the lowest-common denominator.
  // i32 indices are promoted to i64, and non-shared memories are treated as
  // shared.
  IndexType dstIndexType = moduleEnv_.memories[dstMemIndex].indexType();
  IndexType srcIndexType = moduleEnv_.memories[srcMemIndex].indexType();
  IndexType lenIndexType =
      (dstIndexType == IndexType::I32 || srcIndexType == IndexType::I32)
          ? IndexType::I32
          : IndexType::I64;

  // Pop the operands off of the stack and widen them
  RegI64 len = popIndexToInt64(lenIndexType);
#ifdef JS_CODEGEN_X86
  {
    // Stash the length value to prevent running out of registers
    ScratchPtr scratch(*this);
    stashI64(scratch, len);
    freeI64(len);
  }
#endif
  RegI64 srcIndex = popIndexToInt64(srcIndexType);
  RegI64 dstIndex = popIndexToInt64(dstIndexType);

  pushI64(dstIndex);
  pushI64(srcIndex);
#ifdef JS_CODEGEN_X86
  {
    // Unstash the length value and push it back on the stack
    ScratchPtr scratch(*this);
    len = needI64();
    unstashI64(scratch, len);
  }
#endif
  pushI64(len);
  pushI32(dstMemIndex);
  pushI32(srcMemIndex);
  return emitInstanceCall(SASigMemCopyAny);
}

bool BaseCompiler::emitMemFill() {
  uint32_t memoryIndex;
  Nothing nothing;
  if (!iter_.readMemFill(&memoryIndex, &nothing, &nothing, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }

  if (memoryIndex == 0 && isMem32(memoryIndex)) {
    int32_t signedLength;
    int32_t signedValue;
    if (peek2xConst(&signedLength, &signedValue) && signedLength != 0 &&
        uint32_t(signedLength) <= MaxInlineMemoryFillLength) {
      memFillInlineM32();
      return true;
    }
  }
  return memFillCall(memoryIndex);
}

bool BaseCompiler::memFillCall(uint32_t memoryIndex) {
  pushHeapBase(memoryIndex);
  return emitInstanceCall(
      usesSharedMemory(memoryIndex)
          ? (isMem32(memoryIndex) ? SASigMemFillSharedM32
                                  : SASigMemFillSharedM64)
          : (isMem32(memoryIndex) ? SASigMemFillM32 : SASigMemFillM64));
}

bool BaseCompiler::emitMemInit() {
  uint32_t segIndex;
  uint32_t memIndex;
  Nothing nothing;
  if (!iter_.readMemOrTableInit(/*isMem*/ true, &segIndex, &memIndex, &nothing,
                                &nothing, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }

  pushI32(segIndex);
  pushI32(memIndex);
  return emitInstanceCall(isMem32(memIndex) ? SASigMemInitM32
                                            : SASigMemInitM64);
}

//////////////////////////////////////////////////////////////////////////////
//
// Bulk table operations.

bool BaseCompiler::emitTableCopy() {
  uint32_t dstMemOrTableIndex = 0;
  uint32_t srcMemOrTableIndex = 0;
  Nothing nothing;
  if (!iter_.readMemOrTableCopy(false, &dstMemOrTableIndex, &nothing,
                                &srcMemOrTableIndex, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  pushI32(dstMemOrTableIndex);
  pushI32(srcMemOrTableIndex);
  return emitInstanceCall(SASigTableCopy);
}

bool BaseCompiler::emitTableInit() {
  return emitInstanceCallOp<uint32_t, uint32_t>(
      SASigTableInit,
      [this](uint32_t* segIndex, uint32_t* dstTableIndex) -> bool {
        Nothing nothing;
        return iter_.readMemOrTableInit(/*isMem*/ false, segIndex,
                                        dstTableIndex, &nothing, &nothing,
                                        &nothing);
      });
}

bool BaseCompiler::emitTableFill() {
  // fill(start:u32, val:ref, len:u32, table:u32) -> void
  return emitInstanceCallOp<uint32_t>(
      SASigTableFill, [this](uint32_t* tableIndex) -> bool {
        Nothing nothing;
        return iter_.readTableFill(tableIndex, &nothing, &nothing, &nothing);
      });
}

bool BaseCompiler::emitMemDiscard() {
  uint32_t memoryIndex;
  Nothing nothing;
  if (!iter_.readMemDiscard(&memoryIndex, &nothing, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }

  pushHeapBase(memoryIndex);
  return emitInstanceCall(
      usesSharedMemory(memoryIndex)
          ? (isMem32(memoryIndex) ? SASigMemDiscardSharedM32
                                  : SASigMemDiscardSharedM64)
          : (isMem32(memoryIndex) ? SASigMemDiscardM32 : SASigMemDiscardM64));
}

bool BaseCompiler::emitTableGet() {
  uint32_t tableIndex;
  Nothing nothing;
  if (!iter_.readTableGet(&tableIndex, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  if (moduleEnv_.tables[tableIndex].elemType.tableRepr() == TableRepr::Ref) {
    return emitTableGetAnyRef(tableIndex);
  }
  pushI32(tableIndex);
  // get(index:u32, table:u32) -> AnyRef
  return emitInstanceCall(SASigTableGet);
}

bool BaseCompiler::emitTableGrow() {
  // grow(initValue:anyref, delta:u32, table:u32) -> u32
  return emitInstanceCallOp<uint32_t>(
      SASigTableGrow, [this](uint32_t* tableIndex) -> bool {
        Nothing nothing;
        return iter_.readTableGrow(tableIndex, &nothing, &nothing);
      });
}

bool BaseCompiler::emitTableSet() {
  uint32_t tableIndex;
  Nothing nothing;
  if (!iter_.readTableSet(&tableIndex, &nothing, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  if (moduleEnv_.tables[tableIndex].elemType.tableRepr() == TableRepr::Ref) {
    return emitTableSetAnyRef(tableIndex);
  }
  pushI32(tableIndex);
  // set(index:u32, value:ref, table:u32) -> void
  return emitInstanceCall(SASigTableSet);
}

bool BaseCompiler::emitTableSize() {
  uint32_t tableIndex;
  if (!iter_.readTableSize(&tableIndex)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }

  RegPtr instance = needPtr();
  RegI32 length = needI32();

  fr.loadInstancePtr(instance);
  loadTableLength(tableIndex, instance, length);

  pushI32(length);
  freePtr(instance);
  return true;
}

void BaseCompiler::emitTableBoundsCheck(uint32_t tableIndex, RegI32 index,
                                        RegPtr instance) {
  Label ok;
  masm.wasmBoundsCheck32(
      Assembler::Condition::Below, index,
      addressOfTableField(tableIndex, offsetof(TableInstanceData, length),
                          instance),
      &ok);
  masm.wasmTrap(wasm::Trap::OutOfBounds, bytecodeOffset());
  masm.bind(&ok);
}

bool BaseCompiler::emitTableGetAnyRef(uint32_t tableIndex) {
  RegPtr instance = needPtr();
  RegPtr elements = needPtr();
  RegI32 index = popI32();

  fr.loadInstancePtr(instance);
  emitTableBoundsCheck(tableIndex, index, instance);
  loadTableElements(tableIndex, instance, elements);
  masm.loadPtr(BaseIndex(elements, index, ScalePointer), elements);

  pushRef(RegRef(elements));
  freeI32(index);
  freePtr(instance);

  return true;
}

bool BaseCompiler::emitTableSetAnyRef(uint32_t tableIndex) {
  // Create temporaries for valueAddr that is not in the prebarrier register
  // and can be consumed by the barrier operation
  RegPtr valueAddr = RegPtr(PreBarrierReg);
  needPtr(valueAddr);

  RegPtr instance = needPtr();
  RegPtr elements = needPtr();
  RegRef value = popRef();
  RegI32 index = popI32();

  // x86 is one register too short for this operation, shuffle `value` back
  // onto the stack until it is needed.
#ifdef JS_CODEGEN_X86
  pushRef(value);
#endif

  fr.loadInstancePtr(instance);
  emitTableBoundsCheck(tableIndex, index, instance);
  loadTableElements(tableIndex, instance, elements);
  masm.computeEffectiveAddress(BaseIndex(elements, index, ScalePointer),
                               valueAddr);

  freeI32(index);
  freePtr(elements);
  freePtr(instance);

#ifdef JS_CODEGEN_X86
  value = popRef();
#endif

  if (!emitBarrieredStore(Nothing(), valueAddr, value, PreBarrierKind::Normal,
                          PostBarrierKind::Precise)) {
    return false;
  }
  freeRef(value);
  return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// Data and element segment management.

bool BaseCompiler::emitDataOrElemDrop(bool isData) {
  return emitInstanceCallOp<uint32_t>(
      isData ? SASigDataDrop : SASigElemDrop, [&](uint32_t* segIndex) -> bool {
        return iter_.readDataOrElemDrop(isData, segIndex);
      });
}

//////////////////////////////////////////////////////////////////////////////
//
// General object support.

void BaseCompiler::emitPreBarrier(RegPtr valueAddr) {
  Label skipBarrier;
  ScratchPtr scratch(*this);

#ifdef RABALDR_PIN_INSTANCE
  Register instance(InstanceReg);
#else
  Register instance(scratch);
  fr.loadInstancePtr(instance);
#endif

  EmitWasmPreBarrierGuard(masm, instance, scratch, Address(valueAddr, 0),
                          &skipBarrier, nullptr);

#ifndef RABALDR_PIN_INSTANCE
  fr.loadInstancePtr(instance);
#endif
#ifdef JS_CODEGEN_ARM64
  // The prebarrier stub assumes the PseudoStackPointer is set up.  It is OK
  // to just move the sp to x28 here because x28 is not being used by the
  // baseline compiler and need not be saved or restored.
  MOZ_ASSERT(!GeneralRegisterSet::All().hasRegisterIndex(x28.asUnsized()));
  masm.Mov(x28, sp);
#endif
  // The prebarrier call preserves all volatile registers
  EmitWasmPreBarrierCallImmediate(masm, instance, scratch, valueAddr,
                                  /*valueOffset=*/0);

  masm.bind(&skipBarrier);
}

bool BaseCompiler::emitPostBarrierImprecise(const Maybe<RegRef>& object,
                                            RegPtr valueAddr, RegRef value) {
  // We must force a sync before the guard so that locals are in a consistent
  // location for whether or not the post-barrier call is taken.
  sync();

  // Emit a guard to skip the post-barrier call if it is not needed.
  Label skipBarrier;
  RegPtr otherScratch = needPtr();
  EmitWasmPostBarrierGuard(masm, object, otherScratch, value, &skipBarrier);
  freePtr(otherScratch);

  // Push `object` and `value` to preserve them across the call.
  if (object) {
    pushRef(*object);
  }
  pushRef(value);

  // The `valueAddr` is a raw pointer to the cell within some GC object or
  // instance area, and we are careful so that the GC will not run while the
  // post-barrier call is active, so push a uintptr_t value.
  pushPtr(valueAddr);
  if (!emitInstanceCall(SASigPostBarrier)) {
    return false;
  }

  // Restore `object` and `value`.
  popRef(value);
  if (object) {
    popRef(*object);
  }

  masm.bind(&skipBarrier);
  return true;
}

bool BaseCompiler::emitPostBarrierPrecise(const Maybe<RegRef>& object,
                                          RegPtr valueAddr, RegRef prevValue,
                                          RegRef value) {
  // Push `object` and `value` to preserve them across the call.
  if (object) {
    pushRef(*object);
  }
  pushRef(value);

  // Push the arguments and call the precise post-barrier
  pushPtr(valueAddr);
  pushRef(prevValue);
  if (!emitInstanceCall(SASigPostBarrierPrecise)) {
    return false;
  }

  // Restore `object` and `value`.
  popRef(value);
  if (object) {
    popRef(*object);
  }

  return true;
}

bool BaseCompiler::emitBarrieredStore(const Maybe<RegRef>& object,
                                      RegPtr valueAddr, RegRef value,
                                      PreBarrierKind preBarrierKind,
                                      PostBarrierKind postBarrierKind) {
  // The pre-barrier preserves all allocated registers.
  if (preBarrierKind == PreBarrierKind::Normal) {
    emitPreBarrier(valueAddr);
  }

  // The precise post-barrier requires the previous value stored in the field,
  // in order to know if the previous store buffer entry needs to be removed.
  RegRef prevValue;
  if (postBarrierKind == PostBarrierKind::Precise) {
    prevValue = needRef();
    masm.loadPtr(Address(valueAddr, 0), prevValue);
  }

  // Store the value
  masm.storePtr(value, Address(valueAddr, 0));

  // The post-barrier preserves object and value.
  if (postBarrierKind == PostBarrierKind::Precise) {
    return emitPostBarrierPrecise(object, valueAddr, prevValue, value);
  }
  return emitPostBarrierImprecise(object, valueAddr, value);
}

void BaseCompiler::emitBarrieredClear(RegPtr valueAddr) {
  // The pre-barrier preserves all allocated registers.
  emitPreBarrier(valueAddr);

  // Store null
  masm.storePtr(ImmWord(AnyRef::NullRefValue), Address(valueAddr, 0));

  // No post-barrier is needed, as null does not require a store buffer entry
}

//////////////////////////////////////////////////////////////////////////////
//
// GC proposal.

#ifdef ENABLE_WASM_GC

RegPtr BaseCompiler::loadTypeDefInstanceData(uint32_t typeIndex) {
  RegPtr rp = needPtr();
  RegPtr instance;
#  ifndef RABALDR_PIN_INSTANCE
  instance = rp;
  fr.loadInstancePtr(instance);
#  else
  // We can use the pinned instance register.
  instance = RegPtr(InstanceReg);
#  endif
  masm.computeEffectiveAddress(
      Address(instance, Instance::offsetInData(
                            moduleEnv_.offsetOfTypeDefInstanceData(typeIndex))),
      rp);
  return rp;
}

RegPtr BaseCompiler::loadSuperTypeVector(uint32_t typeIndex) {
  RegPtr rp = needPtr();
  RegPtr instance;
#  ifndef RABALDR_PIN_INSTANCE
  // We need to load the instance register, but can use the destination
  // register as a temporary.
  instance = rp;
  fr.loadInstancePtr(rp);
#  else
  // We can use the pinned instance register.
  instance = RegPtr(InstanceReg);
#  endif
  masm.loadPtr(
      Address(instance, Instance::offsetInData(
                            moduleEnv_.offsetOfSuperTypeVector(typeIndex))),
      rp);
  return rp;
}

/* static */
void BaseCompiler::SignalNullCheck::emitNullCheck(BaseCompiler* bc, RegRef rp) {
  Label ok;
  MacroAssembler& masm = bc->masm;
  masm.branchTestPtr(Assembler::NonZero, rp, rp, &ok);
  bc->trap(Trap::NullPointerDereference);
  masm.bind(&ok);
}

/* static */
void BaseCompiler::SignalNullCheck::emitTrapSite(BaseCompiler* bc,
                                                 FaultingCodeOffset fco,
                                                 TrapMachineInsn tmi) {
  wasm::BytecodeOffset trapOffset(bc->bytecodeOffset());
  MacroAssembler& masm = bc->masm;
  masm.append(wasm::Trap::NullPointerDereference,
              wasm::TrapSite(tmi, fco, trapOffset));
}

template <typename NullCheckPolicy>
RegPtr BaseCompiler::emitGcArrayGetData(RegRef rp) {
  // `rp` points at a WasmArrayObject.  Return a reg holding the value of its
  // `data_` field.
  RegPtr rdata = needPtr();
  FaultingCodeOffset fco =
      masm.loadPtr(Address(rp, WasmArrayObject::offsetOfData()), rdata);
  NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsnForLoadWord());
  return rdata;
}

template <typename NullCheckPolicy>
RegI32 BaseCompiler::emitGcArrayGetNumElements(RegRef rp) {
  // `rp` points at a WasmArrayObject.  Return a reg holding the value of its
  // `numElements_` field.
  STATIC_ASSERT_WASMARRAYELEMENTS_NUMELEMENTS_IS_U32;
  RegI32 numElements = needI32();
  FaultingCodeOffset fco = masm.load32(
      Address(rp, WasmArrayObject::offsetOfNumElements()), numElements);
  NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Load32);
  return numElements;
}

void BaseCompiler::emitGcArrayBoundsCheck(RegI32 index, RegI32 numElements) {
  Label inBounds;
  masm.branch32(Assembler::Below, index, numElements, &inBounds);
  masm.wasmTrap(Trap::OutOfBounds, bytecodeOffset());
  masm.bind(&inBounds);
}

template <typename T, typename NullCheckPolicy>
void BaseCompiler::emitGcGet(StorageType type, FieldWideningOp wideningOp,
                             const T& src) {
  switch (type.kind()) {
    case StorageType::I8: {
      MOZ_ASSERT(wideningOp != FieldWideningOp::None);
      RegI32 r = needI32();
      FaultingCodeOffset fco;
      if (wideningOp == FieldWideningOp::Unsigned) {
        fco = masm.load8ZeroExtend(src, r);
      } else {
        fco = masm.load8SignExtend(src, r);
      }
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Load8);
      pushI32(r);
      break;
    }
    case StorageType::I16: {
      MOZ_ASSERT(wideningOp != FieldWideningOp::None);
      RegI32 r = needI32();
      FaultingCodeOffset fco;
      if (wideningOp == FieldWideningOp::Unsigned) {
        fco = masm.load16ZeroExtend(src, r);
      } else {
        fco = masm.load16SignExtend(src, r);
      }
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Load16);
      pushI32(r);
      break;
    }
    case StorageType::I32: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegI32 r = needI32();
      FaultingCodeOffset fco = masm.load32(src, r);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Load32);
      pushI32(r);
      break;
    }
    case StorageType::I64: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegI64 r = needI64();
#  ifdef JS_64BIT
      FaultingCodeOffset fco = masm.load64(src, r);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Load64);
#  else
      FaultingCodeOffsetPair fcop = masm.load64(src, r);
      NullCheckPolicy::emitTrapSite(this, fcop.first, TrapMachineInsn::Load32);
      NullCheckPolicy::emitTrapSite(this, fcop.second, TrapMachineInsn::Load32);
#  endif
      pushI64(r);
      break;
    }
    case StorageType::F32: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegF32 r = needF32();
      FaultingCodeOffset fco = masm.loadFloat32(src, r);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Load32);
      pushF32(r);
      break;
    }
    case StorageType::F64: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegF64 r = needF64();
      FaultingCodeOffset fco = masm.loadDouble(src, r);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Load64);
      pushF64(r);
      break;
    }
#  ifdef ENABLE_WASM_SIMD
    case StorageType::V128: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegV128 r = needV128();
      FaultingCodeOffset fco = masm.loadUnalignedSimd128(src, r);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Load128);
      pushV128(r);
      break;
    }
#  endif
    case StorageType::Ref: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegRef r = needRef();
      FaultingCodeOffset fco = masm.loadPtr(src, r);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsnForLoadWord());
      pushRef(r);
      break;
    }
    default: {
      MOZ_CRASH("Unexpected field type");
    }
  }
}

template <typename T, typename NullCheckPolicy>
void BaseCompiler::emitGcSetScalar(const T& dst, StorageType type,
                                   AnyReg value) {
  switch (type.kind()) {
    case StorageType::I8: {
      FaultingCodeOffset fco = masm.store8(value.i32(), dst);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Store8);
      break;
    }
    case StorageType::I16: {
      FaultingCodeOffset fco = masm.store16(value.i32(), dst);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Store16);
      break;
    }
    case StorageType::I32: {
      FaultingCodeOffset fco = masm.store32(value.i32(), dst);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Store32);
      break;
    }
    case StorageType::I64: {
#  ifdef JS_64BIT
      FaultingCodeOffset fco = masm.store64(value.i64(), dst);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Store64);
#  else
      FaultingCodeOffsetPair fcop = masm.store64(value.i64(), dst);
      NullCheckPolicy::emitTrapSite(this, fcop.first, TrapMachineInsn::Store32);
      NullCheckPolicy::emitTrapSite(this, fcop.second,
                                    TrapMachineInsn::Store32);
#  endif
      break;
    }
    case StorageType::F32: {
      FaultingCodeOffset fco = masm.storeFloat32(value.f32(), dst);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Store32);
      break;
    }
    case StorageType::F64: {
      FaultingCodeOffset fco = masm.storeDouble(value.f64(), dst);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Store64);
      break;
    }
#  ifdef ENABLE_WASM_SIMD
    case StorageType::V128: {
      FaultingCodeOffset fco = masm.storeUnalignedSimd128(value.v128(), dst);
      NullCheckPolicy::emitTrapSite(this, fco, TrapMachineInsn::Store128);
      break;
    }
#  endif
    default: {
      MOZ_CRASH("Unexpected field type");
    }
  }
}

template <typename NullCheckPolicy>
bool BaseCompiler::emitGcStructSet(RegRef object, RegPtr areaBase,
                                   uint32_t areaOffset, StorageType type,
                                   AnyReg value,
                                   PreBarrierKind preBarrierKind) {
  // Easy path if the field is a scalar
  if (!type.isRefRepr()) {
    emitGcSetScalar<Address, NullCheckPolicy>(Address(areaBase, areaOffset),
                                              type, value);
    freeAny(value);
    return true;
  }

  // Create temporary for the valueAddr that is not in the prebarrier register
  // and can be consumed by the barrier operation
  RegPtr valueAddr = RegPtr(PreBarrierReg);
  needPtr(valueAddr);
  masm.computeEffectiveAddress(Address(areaBase, areaOffset), valueAddr);

  NullCheckPolicy::emitNullCheck(this, object);

  // emitBarrieredStore preserves object and value
  if (!emitBarrieredStore(Some(object), valueAddr, value.ref(), preBarrierKind,
                          PostBarrierKind::Imprecise)) {
    return false;
  }
  freeRef(value.ref());

  return true;
}

bool BaseCompiler::emitGcArraySet(RegRef object, RegPtr data, RegI32 index,
                                  const ArrayType& arrayType, AnyReg value,
                                  PreBarrierKind preBarrierKind) {
  // Try to use a base index store instruction if the field type fits in a
  // shift immediate. If not we shift the index manually and then unshift
  // it after the store. We don't use an extra register for this because we
  // don't have any to spare on x86.
  uint32_t shift = arrayType.elementType().indexingShift();
  Scale scale;
  bool shiftedIndex = false;
  if (IsShiftInScaleRange(shift)) {
    scale = ShiftToScale(shift);
  } else {
    masm.lshiftPtr(Imm32(shift), index);
    scale = TimesOne;
    shiftedIndex = true;
  }
  auto unshiftIndex = mozilla::MakeScopeExit([&] {
    if (shiftedIndex) {
      masm.rshiftPtr(Imm32(shift), index);
    }
  });

  // Easy path if the field is a scalar
  if (!arrayType.elementType().isRefRepr()) {
    emitGcSetScalar<BaseIndex, NoNullCheck>(BaseIndex(data, index, scale, 0),
                                            arrayType.elementType(), value);
    return true;
  }

  // Create temporaries for valueAddr that is not in the prebarrier register
  // and can be consumed by the barrier operation
  RegPtr valueAddr = RegPtr(PreBarrierReg);
  needPtr(valueAddr);
  masm.computeEffectiveAddress(BaseIndex(data, index, scale, 0), valueAddr);

  // Save state for after barriered write
  pushPtr(data);
  pushI32(index);

  // emitBarrieredStore preserves object and value
  if (!emitBarrieredStore(Some(object), valueAddr, value.ref(), preBarrierKind,
                          PostBarrierKind::Imprecise)) {
    return false;
  }

  // Restore state
  popI32(index);
  popPtr(data);

  return true;
}

template <bool ZeroFields>
bool BaseCompiler::emitStructAlloc(uint32_t typeIndex, RegRef* object,
                                   bool* isOutlineStruct, RegPtr* outlineBase) {
  const TypeDef& typeDef = (*moduleEnv_.types)[typeIndex];
  const StructType& structType = typeDef.structType();
  gc::AllocKind allocKind = WasmStructObject::allocKindForTypeDef(&typeDef);

  *isOutlineStruct = WasmStructObject::requiresOutlineBytes(structType.size_);

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  needPtr(RegPtr(PreBarrierReg));

  *object = RegRef();

  // Allocate an uninitialized struct. This requires the type definition
  // for the struct to be pushed on the stack. This will trap on OOM.
  if (*isOutlineStruct) {
    pushPtr(loadTypeDefInstanceData(typeIndex));
    if (!emitInstanceCall(ZeroFields ? SASigStructNewOOL_true
                                     : SASigStructNewOOL_false)) {
      return false;
    }
    *object = popRef();
  } else {
    // We eagerly sync the value stack to the machine stack here so as not to
    // confuse things with the conditional instance call below.
    sync();

    RegPtr instance;
    *object = RegRef(ReturnReg);
    needRef(*object);
#  ifndef RABALDR_PIN_INSTANCE
    // We reuse the result register for the instance.
    instance = RegPtr(ReturnReg);
    fr.loadInstancePtr(instance);
#  else
    // We can use the pinned instance register.
    instance = RegPtr(InstanceReg);
#  endif

    RegPtr typeDefData = loadTypeDefInstanceData(typeIndex);
    RegPtr temp1 = needPtr();
    RegPtr temp2 = needPtr();

    Label success;
    Label fail;
    masm.wasmNewStructObject(instance, *object, typeDefData, temp1, temp2,
                             &fail, allocKind, ZeroFields);
    freePtr(temp1);
    freePtr(temp2);
    masm.jump(&success);

    masm.bind(&fail);
    freeRef(*object);
    pushPtr(typeDefData);
    if (!emitInstanceCall(ZeroFields ? SASigStructNewIL_true
                                     : SASigStructNewIL_false)) {
      return false;
    }
    *object = popRef();
    MOZ_ASSERT(*object == RegRef(ReturnReg));

    masm.bind(&success);
  }

  *outlineBase = *isOutlineStruct ? needPtr() : RegPtr();

  // Free the barrier reg for later use
  freePtr(RegPtr(PreBarrierReg));

  return true;
}

bool BaseCompiler::emitStructNew() {
  uint32_t typeIndex;
  BaseNothingVector args{};
  if (!iter_.readStructNew(&typeIndex, &args)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const TypeDef& typeDef = (*moduleEnv_.types)[typeIndex];
  const StructType& structType = typeDef.structType();

  RegRef object;
  RegPtr outlineBase;
  bool isOutlineStruct;
  if (!emitStructAlloc<false>(typeIndex, &object, &isOutlineStruct,
                              &outlineBase)) {
    return false;
  }

  // Optimization opportunity: Iterate backward to pop arguments off the
  // stack.  This will generate more instructions than we want, since we
  // really only need to pop the stack once at the end, not for every element,
  // but to do better we need a bit more machinery to load elements off the
  // stack into registers.

  // Optimization opportunity: when the value being stored is a known
  // zero/null we need store nothing.  This case may be somewhat common
  // because struct.new forces a value to be specified for every field.

  // Optimization opportunity: this loop reestablishes the outline base pointer
  // every iteration, which really isn't very clever.  It would be better to
  // establish it once before we start, then re-set it if/when we transition
  // from the out-of-line area back to the in-line area.  That would however
  // require making ::emitGcStructSet preserve that register, which it
  // currently doesn't.

  uint32_t fieldIndex = structType.fields_.length();
  while (fieldIndex-- > 0) {
    const FieldType& field = structType.fields_[fieldIndex];
    StorageType type = field.type;
    uint32_t fieldOffset = structType.fieldOffset(fieldIndex);

    bool areaIsOutline;
    uint32_t areaOffset;
    WasmStructObject::fieldOffsetToAreaAndOffset(type, fieldOffset,
                                                 &areaIsOutline, &areaOffset);

    // Reserve the barrier reg if we might need it for this store
    if (type.isRefRepr()) {
      needPtr(RegPtr(PreBarrierReg));
    }
    AnyReg value = popAny();
    // Free the barrier reg now that we've loaded the value
    if (type.isRefRepr()) {
      freePtr(RegPtr(PreBarrierReg));
    }

    if (areaIsOutline) {
      // Load the outline data pointer
      masm.loadPtr(Address(object, WasmStructObject::offsetOfOutlineData()),
                   outlineBase);

      // Consumes value and outline data, object is preserved by this call.
      if (!emitGcStructSet<NoNullCheck>(object, outlineBase, areaOffset, type,
                                        value, PreBarrierKind::None)) {
        return false;
      }
    } else {
      // Consumes value. object is unchanged by this call.
      if (!emitGcStructSet<NoNullCheck>(
              object, RegPtr(object),
              WasmStructObject::offsetOfInlineData() + areaOffset, type, value,
              PreBarrierKind::None)) {
        return false;
      }
    }
  }

  if (isOutlineStruct) {
    freePtr(outlineBase);
  }
  pushRef(object);

  return true;
}

bool BaseCompiler::emitStructNewDefault() {
  uint32_t typeIndex;
  if (!iter_.readStructNewDefault(&typeIndex)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }

  RegRef object;
  bool isOutlineStruct;
  RegPtr outlineBase;
  if (!emitStructAlloc<true>(typeIndex, &object, &isOutlineStruct,
                             &outlineBase)) {
    return false;
  }

  if (isOutlineStruct) {
    freePtr(outlineBase);
  }
  pushRef(object);

  return true;
}

bool BaseCompiler::emitStructGet(FieldWideningOp wideningOp) {
  uint32_t typeIndex;
  uint32_t fieldIndex;
  Nothing nothing;
  if (!iter_.readStructGet(&typeIndex, &fieldIndex, wideningOp, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const StructType& structType = (*moduleEnv_.types)[typeIndex].structType();

  // Decide whether we're accessing inline or outline, and at what offset
  StorageType fieldType = structType.fields_[fieldIndex].type;
  uint32_t fieldOffset = structType.fieldOffset(fieldIndex);

  bool areaIsOutline;
  uint32_t areaOffset;
  WasmStructObject::fieldOffsetToAreaAndOffset(fieldType, fieldOffset,
                                               &areaIsOutline, &areaOffset);

  RegRef object = popRef();
  if (areaIsOutline) {
    RegPtr outlineBase = needPtr();
    FaultingCodeOffset fco = masm.loadPtr(
        Address(object, WasmStructObject::offsetOfOutlineData()), outlineBase);
    SignalNullCheck::emitTrapSite(this, fco, TrapMachineInsnForLoadWord());
    // Load the value
    emitGcGet<Address, NoNullCheck>(fieldType, wideningOp,
                                    Address(outlineBase, areaOffset));
    freePtr(outlineBase);
  } else {
    // Load the value
    emitGcGet<Address, SignalNullCheck>(
        fieldType, wideningOp,
        Address(object, WasmStructObject::offsetOfInlineData() + areaOffset));
  }
  freeRef(object);

  return true;
}

bool BaseCompiler::emitStructSet() {
  uint32_t typeIndex;
  uint32_t fieldIndex;
  Nothing nothing;
  if (!iter_.readStructSet(&typeIndex, &fieldIndex, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const StructType& structType = (*moduleEnv_.types)[typeIndex].structType();
  const FieldType& structField = structType.fields_[fieldIndex];

  // Decide whether we're accessing inline or outline, and at what offset
  StorageType fieldType = structType.fields_[fieldIndex].type;
  uint32_t fieldOffset = structType.fieldOffset(fieldIndex);

  bool areaIsOutline;
  uint32_t areaOffset;
  WasmStructObject::fieldOffsetToAreaAndOffset(fieldType, fieldOffset,
                                               &areaIsOutline, &areaOffset);

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  if (structField.type.isRefRepr()) {
    needPtr(RegPtr(PreBarrierReg));
  }

  RegPtr outlineBase = areaIsOutline ? needPtr() : RegPtr();
  AnyReg value = popAny();
  RegRef object = popRef();

  // Free the barrier reg after we've allocated all registers
  if (structField.type.isRefRepr()) {
    freePtr(RegPtr(PreBarrierReg));
  }

  // Make outlineBase point at the first byte of the relevant area
  if (areaIsOutline) {
    FaultingCodeOffset fco = masm.loadPtr(
        Address(object, WasmStructObject::offsetOfOutlineData()), outlineBase);
    SignalNullCheck::emitTrapSite(this, fco, TrapMachineInsnForLoadWord());
    if (!emitGcStructSet<NoNullCheck>(object, outlineBase, areaOffset,
                                      fieldType, value,
                                      PreBarrierKind::Normal)) {
      return false;
    }
  } else {
    // Consumes value. object is unchanged by this call.
    if (!emitGcStructSet<SignalNullCheck>(
            object, RegPtr(object),
            WasmStructObject::offsetOfInlineData() + areaOffset, fieldType,
            value, PreBarrierKind::Normal)) {
      return false;
    }
  }

  if (areaIsOutline) {
    freePtr(outlineBase);
  }
  freeRef(object);

  return true;
}

template <bool ZeroFields>
bool BaseCompiler::emitArrayAlloc(uint32_t typeIndex, RegRef object,
                                  RegI32 numElements, uint32_t elemSize) {
  // We eagerly sync the value stack to the machine stack here so as not to
  // confuse things with the conditional instance call below.
  sync();

  RegPtr instance;
#  ifndef RABALDR_PIN_INSTANCE
  // We reuse the object register for the instance. This is ok because object is
  // not live until instance is dead.
  instance = RegPtr(object);
  fr.loadInstancePtr(instance);
#  else
  // We can use the pinned instance register.
  instance = RegPtr(InstanceReg);
#  endif

  RegPtr typeDefData = loadTypeDefInstanceData(typeIndex);
  RegPtr temp = needPtr();

  Label success;
  Label fail;
  masm.wasmNewArrayObject(instance, object, numElements, typeDefData, temp,
                          &fail, elemSize, ZeroFields);
  freePtr(temp);
  masm.jump(&success);

  masm.bind(&fail);
  freeRef(object);
  pushI32(numElements);
  pushPtr(typeDefData);
  if (!emitInstanceCall(ZeroFields ? SASigArrayNew_true
                                   : SASigArrayNew_false)) {
    return false;
  }
  popRef(object);

  masm.bind(&success);
  return true;
}

template <bool ZeroFields>
bool BaseCompiler::emitArrayAllocFixed(uint32_t typeIndex, RegRef object,
                                       uint32_t numElements,
                                       uint32_t elemSize) {
  // The maximum number of elements for array.new_fixed enforced in validation
  // should always prevent overflow here.
  MOZ_ASSERT(WasmArrayObject::calcStorageBytesChecked(elemSize, numElements)
                 .isValid());

  SymbolicAddressSignature fun =
      ZeroFields ? SASigArrayNew_true : SASigArrayNew_false;

  uint32_t storageBytes =
      WasmArrayObject::calcStorageBytes(elemSize, numElements);
  if (storageBytes > WasmArrayObject_MaxInlineBytes) {
    RegPtr typeDefData = loadTypeDefInstanceData(typeIndex);
    freeRef(object);
    pushI32(numElements);
    pushPtr(typeDefData);
    if (!emitInstanceCall(fun)) {
      return false;
    }
    popRef(object);

    return true;
  }

  // We eagerly sync the value stack to the machine stack here so as not to
  // confuse things with the conditional instance call below.
  sync();

  RegPtr instance;
#  ifndef RABALDR_PIN_INSTANCE
  // We reuse the object register for the instance. This is ok because object is
  // not live until instance is dead.
  instance = RegPtr(object);
  fr.loadInstancePtr(instance);
#  else
  // We can use the pinned instance register.
  instance = RegPtr(InstanceReg);
#  endif

  RegPtr typeDefData = loadTypeDefInstanceData(typeIndex);
  RegPtr temp1 = needPtr();
  RegPtr temp2 = needPtr();

  Label success;
  Label fail;
  masm.wasmNewArrayObjectFixed(instance, object, typeDefData, temp1, temp2,
                               &fail, numElements, storageBytes, ZeroFields);
  freePtr(temp1);
  freePtr(temp2);
  masm.jump(&success);

  masm.bind(&fail);
  freeRef(object);
  pushI32(numElements);
  pushPtr(typeDefData);
  if (!emitInstanceCall(fun)) {
    return false;
  }
  popRef(object);

  masm.bind(&success);

  return true;
}

bool BaseCompiler::emitArrayNew() {
  uint32_t typeIndex;
  Nothing nothing;
  if (!iter_.readArrayNew(&typeIndex, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const ArrayType& arrayType = (*moduleEnv_.types)[typeIndex].arrayType();

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  if (arrayType.elementType().isRefRepr()) {
    needPtr(RegPtr(PreBarrierReg));
  }

  RegRef object = needRef();
  RegI32 numElements = popI32();
  if (!emitArrayAlloc<false>(typeIndex, object, numElements,
                             arrayType.elementType().size())) {
    return false;
  }

  AnyReg value = popAny();

  // Acquire the data pointer from the object
  RegPtr rdata = emitGcArrayGetData<NoNullCheck>(object);

  // Acquire the number of elements again
  numElements = emitGcArrayGetNumElements<NoNullCheck>(object);

  // Free the barrier reg after we've allocated all registers
  if (arrayType.elementType().isRefRepr()) {
    freePtr(RegPtr(PreBarrierReg));
  }

  // Perform an initialization loop using `numElements` as the loop variable,
  // counting down to zero.
  Label done;
  Label loop;
  // Skip initialization if numElements = 0
  masm.branch32(Assembler::Equal, numElements, Imm32(0), &done);
  masm.bind(&loop);

  // Move to the next element
  masm.sub32(Imm32(1), numElements);

  // Assign value to array[numElements]. All registers are preserved
  if (!emitGcArraySet(object, rdata, numElements, arrayType, value,
                      PreBarrierKind::None)) {
    return false;
  }

  // Loop back if there are still elements to initialize
  masm.branch32(Assembler::GreaterThan, numElements, Imm32(0), &loop);
  masm.bind(&done);

  freeI32(numElements);
  freeAny(value);
  freePtr(rdata);
  pushRef(object);

  return true;
}

bool BaseCompiler::emitArrayNewFixed() {
  uint32_t typeIndex, numElements;
  BaseNothingVector nothings{};
  if (!iter_.readArrayNewFixed(&typeIndex, &numElements, &nothings)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const ArrayType& arrayType = (*moduleEnv_.types)[typeIndex].arrayType();

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  bool avoidPreBarrierReg = arrayType.elementType().isRefRepr();
  if (avoidPreBarrierReg) {
    needPtr(RegPtr(PreBarrierReg));
  }

  RegRef object = needRef();
  if (!emitArrayAllocFixed<false>(typeIndex, object, numElements,
                                  arrayType.elementType().size())) {
    return false;
  }

  // Acquire the data pointer from the object
  RegPtr rdata = emitGcArrayGetData<NoNullCheck>(object);

  // Free the barrier reg if we previously reserved it.
  if (avoidPreBarrierReg) {
    freePtr(RegPtr(PreBarrierReg));
  }

  // These together ensure that the max value of `index` in the loop below
  // remains comfortably below the 2^31 boundary.  See comments on equivalent
  // assertions in EmitArrayNewFixed in WasmIonCompile.cpp for explanation.
  static_assert(16 /* sizeof v128 */ * MaxFunctionBytes <=
                MaxArrayPayloadBytes);
  MOZ_RELEASE_ASSERT(numElements <= MaxFunctionBytes);

  // Generate straight-line initialization code.  We could do better here if
  // there was a version of ::emitGcArraySet that took `index` as a `uint32_t`
  // rather than a general value-in-a-reg.
  for (uint32_t forwardIndex = 0; forwardIndex < numElements; forwardIndex++) {
    uint32_t reverseIndex = numElements - forwardIndex - 1;
    if (avoidPreBarrierReg) {
      needPtr(RegPtr(PreBarrierReg));
    }
    AnyReg value = popAny();
    pushI32(reverseIndex);
    RegI32 index = popI32();
    if (avoidPreBarrierReg) {
      freePtr(RegPtr(PreBarrierReg));
    }
    if (!emitGcArraySet(object, rdata, index, arrayType, value,
                        PreBarrierKind::None)) {
      return false;
    }
    freeI32(index);
    freeAny(value);
  }

  freePtr(rdata);

  pushRef(object);
  return true;
}

bool BaseCompiler::emitArrayNewDefault() {
  uint32_t typeIndex;
  Nothing nothing;
  if (!iter_.readArrayNewDefault(&typeIndex, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const ArrayType& arrayType = (*moduleEnv_.types)[typeIndex].arrayType();

  RegRef object = needRef();
  RegI32 numElements = popI32();
  if (!emitArrayAlloc<true>(typeIndex, object, numElements,
                            arrayType.elementType().size())) {
    return false;
  }

  pushRef(object);
  return true;
}

bool BaseCompiler::emitArrayNewData() {
  uint32_t typeIndex, segIndex;
  Nothing nothing;
  if (!iter_.readArrayNewData(&typeIndex, &segIndex, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  pushPtr(loadTypeDefInstanceData(typeIndex));
  pushI32(int32_t(segIndex));

  // The call removes 4 items from the stack: the segment byte offset and
  // number of elements (operands to array.new_data), and the type data and
  // seg index as pushed above.
  return emitInstanceCall(SASigArrayNewData);
}

bool BaseCompiler::emitArrayNewElem() {
  uint32_t typeIndex, segIndex;
  Nothing nothing;
  if (!iter_.readArrayNewElem(&typeIndex, &segIndex, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  pushPtr(loadTypeDefInstanceData(typeIndex));
  pushI32(int32_t(segIndex));

  // The call removes 4 items from the stack: the segment element offset and
  // number of elements (operands to array.new_elem), and the type data and
  // seg index as pushed above.
  return emitInstanceCall(SASigArrayNewElem);
}

bool BaseCompiler::emitArrayInitData() {
  uint32_t typeIndex, segIndex;
  Nothing nothing;
  if (!iter_.readArrayInitData(&typeIndex, &segIndex, &nothing, &nothing,
                               &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  pushPtr(loadTypeDefInstanceData(typeIndex));
  pushI32(int32_t(segIndex));

  // The call removes 6 items from the stack: the array, array index, segment
  // byte offset, and number of elements (operands to array.init_data), and the
  // type data and seg index as pushed above.
  return emitInstanceCall(SASigArrayInitData);
}

bool BaseCompiler::emitArrayInitElem() {
  uint32_t typeIndex, segIndex;
  Nothing nothing;
  if (!iter_.readArrayInitElem(&typeIndex, &segIndex, &nothing, &nothing,
                               &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  pushPtr(loadTypeDefInstanceData(typeIndex));
  pushI32(int32_t(segIndex));

  // The call removes 6 items from the stack: the array, array index, segment
  // offset, and number of elements (operands to array.init_elem), and the type
  // data and seg index as pushed above.
  return emitInstanceCall(SASigArrayInitElem);
}

bool BaseCompiler::emitArrayGet(FieldWideningOp wideningOp) {
  uint32_t typeIndex;
  Nothing nothing;
  if (!iter_.readArrayGet(&typeIndex, wideningOp, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const ArrayType& arrayType = (*moduleEnv_.types)[typeIndex].arrayType();

  RegI32 index = popI32();
  RegRef rp = popRef();

  // Acquire the number of elements
  RegI32 numElements = emitGcArrayGetNumElements<SignalNullCheck>(rp);

  // Bounds check the index
  emitGcArrayBoundsCheck(index, numElements);
  freeI32(numElements);

  // Acquire the data pointer from the object
  RegPtr rdata = emitGcArrayGetData<NoNullCheck>(rp);

  // Load the value
  uint32_t shift = arrayType.elementType().indexingShift();
  if (IsShiftInScaleRange(shift)) {
    emitGcGet<BaseIndex, NoNullCheck>(
        arrayType.elementType(), wideningOp,
        BaseIndex(rdata, index, ShiftToScale(shift), 0));
  } else {
    masm.lshiftPtr(Imm32(shift), index);
    emitGcGet<BaseIndex, NoNullCheck>(arrayType.elementType(), wideningOp,
                                      BaseIndex(rdata, index, TimesOne, 0));
  }

  freePtr(rdata);
  freeRef(rp);
  freeI32(index);

  return true;
}

bool BaseCompiler::emitArraySet() {
  uint32_t typeIndex;
  Nothing nothing;
  if (!iter_.readArraySet(&typeIndex, &nothing, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const ArrayType& arrayType = (*moduleEnv_.types)[typeIndex].arrayType();

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  if (arrayType.elementType().isRefRepr()) {
    needPtr(RegPtr(PreBarrierReg));
  }

  AnyReg value = popAny();
  RegI32 index = popI32();
  RegRef rp = popRef();

  // Acquire the number of elements
  RegI32 numElements = emitGcArrayGetNumElements<SignalNullCheck>(rp);

  // Bounds check the index
  emitGcArrayBoundsCheck(index, numElements);
  freeI32(numElements);

  // Acquire the data pointer from the object
  RegPtr rdata = emitGcArrayGetData<NoNullCheck>(rp);

  // Free the barrier reg after we've allocated all registers
  if (arrayType.elementType().isRefRepr()) {
    freePtr(RegPtr(PreBarrierReg));
  }

  // All registers are preserved. This isn't strictly necessary, as we'll just
  // be freeing them all after this is done. But this is needed for repeated
  // assignments used in array.new/new_default.
  if (!emitGcArraySet(rp, rdata, index, arrayType, value,
                      PreBarrierKind::Normal)) {
    return false;
  }

  freePtr(rdata);
  freeRef(rp);
  freeI32(index);
  freeAny(value);

  return true;
}

bool BaseCompiler::emitArrayLen() {
  Nothing nothing;
  if (!iter_.readArrayLen(&nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef rp = popRef();

  // Acquire the number of elements
  RegI32 numElements = emitGcArrayGetNumElements<SignalNullCheck>(rp);
  pushI32(numElements);

  freeRef(rp);

  return true;
}

bool BaseCompiler::emitArrayCopy() {
  int32_t elemSize;
  bool elemsAreRefTyped;
  Nothing nothing;
  if (!iter_.readArrayCopy(&elemSize, &elemsAreRefTyped, &nothing, &nothing,
                           &nothing, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  // The helper needs to know the element size. If copying ref values, the size
  // is negated to signal to the helper that it needs to do GC barriers and
  // such.
  pushI32(elemsAreRefTyped ? -elemSize : elemSize);

  return emitInstanceCall(SASigArrayCopy);
}

bool BaseCompiler::emitArrayFill() {
  AutoCreatedBy acb(masm, "(wasm)BaseCompiler::emitArrayFill");
  uint32_t typeIndex;
  Nothing nothing;
  if (!iter_.readArrayFill(&typeIndex, &nothing, &nothing, &nothing,
                           &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const TypeDef& typeDef = moduleEnv_.types->type(typeIndex);
  const ArrayType& arrayType = typeDef.arrayType();
  StorageType elementType = arrayType.elementType();

  // On x86 (32-bit), we are very short of registers, hence the code
  // generation scheme is less straightforward than it might otherwise be.
  //
  // Comment lines below of the form
  //
  //   3: foo bar xyzzy
  //
  // mean "at this point there are 3 integer regs in use (not including the
  // possible 2 needed for `value` in the worst case), and those 3 regs hold
  // the values for `foo`, `bar` and `xyzzy`.
  //
  // On x86, the worst case, we only have 3 int regs available (not including
  // `value`), thusly:
  //
  // - as a basis, we have: eax ebx ecx edx esi edi
  //
  // - ebx is "kind of" not available, since it is reserved as a wasm-baseline
  //   scratch register (called `RabaldrScratchI32` on most targets, but
  //   acquired below using `ScratchI32(..)`).
  //
  // - `value` isn't needed till the initialisation loop itself, hence it
  //   would seem attractive to spill it across the range-check and setup code
  //   that precedes the loop.  However, it has variable type and regclass
  //   (eg, it could be v128 or f32 or f64), which make spilling it somewhat
  //   complex, so we bite the bullet and just allocate it at the start of the
  //   routine.
  //
  // - Consequently we have the following two cases as worst-cases:
  //
  //   * `value` is I64, so uses two registers on x86.
  //   * `value` is reftyped, using one register, but also making
  //     PreBarrierReg unavailable.
  //
  // - As a result of all of the above, we have only three registers, and
  //   RabaldrScratchI32, to work with.  And 4 spill-slot words; however ..
  //
  // - .. to spill/reload from a spill slot, we need a register to point at
  //   the instance.  That makes it even worse on x86 since there's no
  //   reserved instance reg; hence we have to use one of our 3 for it.  This
  //   is indicated explictly in the code below.
  //
  // There are many comment lines indicating the current disposition of the 3
  // regs.
  //
  // This generates somewhat clunky code for the setup and bounds test, but
  // the actual initialisation loop still has everything in registers, so
  // should run fast.
  //
  // The same scheme is used for all targets.  Hence they all get clunky setup
  // code.  Considering that this is a baseline compiler and also that the
  // loop itself is all-in-regs, this seems like a not-bad compromise compared
  // to having two different implementations for x86 vs everything else.

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  if (elementType.isRefRepr()) {
    needPtr(RegPtr(PreBarrierReg));
  }

  // Set up a pointer to the Instance, so we can do manual spills/reloads
#  ifdef RABALDR_PIN_INSTANCE
  RegPtr instancePtr = RegPtr(InstanceReg);
#  else
  RegPtr instancePtr = needPtr();
  fr.loadInstancePtr(instancePtr);
#  endif

  // Pull operands off the instance stack and stash the non-Any ones
  RegI32 numElements = popI32();
  stashWord(instancePtr, 0, RegPtr(numElements));
  freeI32(numElements);
  numElements = RegI32::Invalid();

  AnyReg value = popAny();

  RegI32 index = popI32();
  stashWord(instancePtr, 1, RegPtr(index));
  freeI32(index);
  index = RegI32::Invalid();

  RegRef rp = popRef();
  stashWord(instancePtr, 2, RegPtr(rp));
  // 2: instancePtr rp

  // Acquire the actual length of the array
  RegI32 arrayNumElements = emitGcArrayGetNumElements<SignalNullCheck>(rp);
  // 3: instancePtr rp arrayNumElements

  // Drop rp
  freeRef(rp);
  rp = RegRef::Invalid();
  // 2: instancePtr arrayNumElements

  // Reload index
  index = needI32();
  unstashWord(instancePtr, 1, RegPtr(index));
  // 3: instancePtr arrayNumElements index

  // Reload numElements into the reg that currently holds instancePtr
#  ifdef RABALDR_PIN_INSTANCE
  numElements = needI32();
#  else
  numElements = RegI32(instancePtr);
#  endif
  unstashWord(instancePtr, 0, RegPtr(numElements));
  instancePtr = RegPtr::Invalid();
  // 3: arrayNumElements index numElements

  // Do the bounds check.  For this we will need to get hold of the
  // wasm-baseline's scratch register.
  {
    ScratchI32 scratch(*this);
    MOZ_ASSERT(RegI32(scratch) != arrayNumElements);
    MOZ_ASSERT(RegI32(scratch) != index);
    MOZ_ASSERT(RegI32(scratch) != numElements);
    masm.wasmBoundsCheckRange32(index, numElements, arrayNumElements, scratch,
                                bytecodeOffset());
  }
  // 3: arrayNumElements index numElements

  // Drop arrayNumElements and numElements
  freeI32(arrayNumElements);
  arrayNumElements = RegI32::Invalid();
  freeI32(numElements);
  numElements = RegI32::Invalid();
  // 1: index

  // Re-set-up the instance pointer; we had to ditch it earlier.
#  ifdef RABALDR_PIN_INSTANCE
  instancePtr = RegPtr(InstanceReg);
#  else
  instancePtr = needPtr();
  fr.loadInstancePtr(instancePtr);
#  endif
  // 2: index instancePtr

  // Reload rp
  rp = needRef();
  unstashWord(instancePtr, 2, RegPtr(rp));
  // 3: index instancePtr rp

  // Drop instancePtr
#  ifdef RABALDR_PIN_INSTANCE
  instancePtr = RegPtr::Invalid();
#  else
  freePtr(instancePtr);
  instancePtr = RegPtr::Invalid();
#  endif
  // 2: index rp

  // Acquire the data pointer from the object
  RegPtr rdata = emitGcArrayGetData<NoNullCheck>(rp);
  // 3: index rp rdata

  // Currently `rdata` points at the start of the array data area.  Move it
  // forwards by `index` units so as to make it point at the start of the area
  // to be filled.
  uint32_t shift = arrayType.elementType().indexingShift();
  if (shift > 0) {
    masm.lshift32(Imm32(shift), index);
    // `index` is a 32 bit value, so we must zero-extend it to 64 bits before
    // adding it on to `rdata`.
#  ifdef JS_64BIT
    masm.move32To64ZeroExtend(index, widenI32(index));
#  endif
  }
  masm.addPtr(index, rdata);
  // `index` is not used after this point.

  // 3: index rp rdata
  // index is now (more or less) meaningless
  // rdata now points exactly at the start of the fill area
  // rp points at the array object

  // Drop `index`; we no longer need it.
  freeI32(index);
  index = RegI32::Invalid();
  // 2: rp rdata

  // Re-re-set-up the instance pointer; we had to ditch it earlier.
#  ifdef RABALDR_PIN_INSTANCE
  instancePtr = RegPtr(InstanceReg);
#  else
  instancePtr = needPtr();
  fr.loadInstancePtr(instancePtr);
#  endif
  // 3: rp rdata instancePtr

  // And reload numElements.
#  ifdef RABALDR_PIN_INSTANCE
  numElements = needI32();
  unstashWord(instancePtr, 0, RegPtr(numElements));
  instancePtr = RegPtr::Invalid();
#  else
  numElements = RegI32(instancePtr);
  unstashWord(instancePtr, 0, RegPtr(numElements));
  instancePtr = RegPtr::Invalid();
#  endif
  // 3: numElements rp rdata

  // Free the barrier reg after we've allocated all registers
  if (elementType.isRefRepr()) {
    freePtr(RegPtr(PreBarrierReg));
  }

  // Perform an initialization loop using `numElements` as the loop variable,
  // starting at `numElements` and counting down to zero.
  Label done;
  Label loop;
  // Skip initialization if numElements = 0
  masm.branch32(Assembler::Equal, numElements, Imm32(0), &done);
  masm.bind(&loop);

  // Move to the next element
  masm.sub32(Imm32(1), numElements);

  // Assign value to rdata[numElements]. All registers are preserved.
  if (!emitGcArraySet(rp, rdata, numElements, arrayType, value,
                      PreBarrierKind::None)) {
    return false;
  }

  // Loop back if there are still elements to initialize
  masm.branch32(Assembler::NotEqual, numElements, Imm32(0), &loop);
  masm.bind(&done);

  freePtr(rdata);
  freeRef(rp);
  freeI32(numElements);
  freeAny(value);
  MOZ_ASSERT(index == RegPtr::Invalid());
  MOZ_ASSERT(instancePtr == RegPtr::Invalid());
  MOZ_ASSERT(arrayNumElements == RegI32::Invalid());

  return true;
}

bool BaseCompiler::emitRefI31() {
  Nothing value;
  if (!iter_.readConversion(ValType::I32,
                            ValType(RefType::i31().asNonNullable()), &value)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegI32 intValue = popI32();
  RegRef i31Value = needRef();
  masm.truncate32ToWasmI31Ref(intValue, i31Value);
  freeI32(intValue);
  pushRef(i31Value);
  return true;
}

bool BaseCompiler::emitI31Get(FieldWideningOp wideningOp) {
  MOZ_ASSERT(wideningOp != FieldWideningOp::None);

  Nothing value;
  if (!iter_.readConversion(ValType(RefType::i31()), ValType::I32, &value)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef i31Value = popRef();
  RegI32 intValue = needI32();

  Label success;
  masm.branchWasmAnyRefIsNull(false, i31Value, &success);
  trap(Trap::NullPointerDereference);
  masm.bind(&success);

  if (wideningOp == FieldWideningOp::Signed) {
    masm.convertWasmI31RefTo32Signed(i31Value, intValue);
  } else {
    masm.convertWasmI31RefTo32Unsigned(i31Value, intValue);
  }
  freeRef(i31Value);
  pushI32(intValue);
  return true;
}

BranchIfRefSubtypeRegisters BaseCompiler::allocRegistersForBranchIfRefSubtype(
    RefType destType) {
  BranchWasmRefIsSubtypeRegisters needs =
      MacroAssembler::regsForBranchWasmRefIsSubtype(destType);
  return BranchIfRefSubtypeRegisters{
      .superSTV = needs.needSuperSTV
                      ? loadSuperTypeVector(
                            moduleEnv_.types->indexOf(*destType.typeDef()))
                      : RegPtr::Invalid(),
      .scratch1 = needs.needScratch1 ? needI32() : RegI32::Invalid(),
      .scratch2 = needs.needScratch2 ? needI32() : RegI32::Invalid(),
  };
}

void BaseCompiler::freeRegistersForBranchIfRefSubtype(
    const BranchIfRefSubtypeRegisters& regs) {
  if (regs.superSTV.isValid()) {
    freePtr(regs.superSTV);
  }
  if (regs.scratch1.isValid()) {
    freeI32(regs.scratch1);
  }
  if (regs.scratch2.isValid()) {
    freeI32(regs.scratch2);
  }
}

bool BaseCompiler::emitRefTest(bool nullable) {
  Nothing nothing;
  RefType sourceType;
  RefType destType;
  if (!iter_.readRefTest(nullable, &sourceType, &destType, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  Label success;
  Label join;
  RegRef ref = popRef();
  RegI32 result = needI32();

  BranchIfRefSubtypeRegisters regs =
      allocRegistersForBranchIfRefSubtype(destType);
  masm.branchWasmRefIsSubtype(ref, sourceType, destType, &success,
                              /*onSuccess=*/true, regs.superSTV, regs.scratch1,
                              regs.scratch2);
  freeRegistersForBranchIfRefSubtype(regs);

  masm.xor32(result, result);
  masm.jump(&join);
  masm.bind(&success);
  masm.move32(Imm32(1), result);
  masm.bind(&join);

  pushI32(result);
  freeRef(ref);

  return true;
}

bool BaseCompiler::emitRefCast(bool nullable) {
  Nothing nothing;
  RefType sourceType;
  RefType destType;
  if (!iter_.readRefCast(nullable, &sourceType, &destType, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef ref = popRef();

  Label success;
  BranchIfRefSubtypeRegisters regs =
      allocRegistersForBranchIfRefSubtype(destType);
  masm.branchWasmRefIsSubtype(ref, sourceType, destType, &success,
                              /*onSuccess=*/true, regs.superSTV, regs.scratch1,
                              regs.scratch2);
  freeRegistersForBranchIfRefSubtype(regs);

  masm.wasmTrap(Trap::BadCast, bytecodeOffset());
  masm.bind(&success);
  pushRef(ref);

  return true;
}

bool BaseCompiler::emitBrOnCastCommon(bool onSuccess,
                                      uint32_t labelRelativeDepth,
                                      const ResultType& labelType,
                                      RefType sourceType, RefType destType) {
  Control& target = controlItem(labelRelativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  // 3. br_if $l : [T*, ref] -> [T*, ref]
  BranchState b(&target.label, target.stackHeight, InvertBranch(false),
                labelType);

  // Don't allocate the result register used in the branch
  if (b.hasBlockResults()) {
    needIntegerResultRegisters(b.resultType);
  }

  // Get the ref from the top of the stack
  RegRef refCondition = popRef();

  // Create a copy of the ref for passing to the on_cast label,
  // the original ref is used in the condition.
  RegRef ref = needRef();
  moveRef(refCondition, ref);
  pushRef(ref);

  if (b.hasBlockResults()) {
    freeIntegerResultRegisters(b.resultType);
  }

  if (!jumpConditionalWithResults(&b, refCondition, sourceType, destType,
                                  onSuccess)) {
    return false;
  }
  freeRef(refCondition);

  return true;
}

bool BaseCompiler::emitBrOnCast(bool onSuccess) {
  MOZ_ASSERT(!hasLatentOp());

  uint32_t labelRelativeDepth;
  RefType sourceType;
  RefType destType;
  ResultType labelType;
  BaseNothingVector unused_values{};
  if (!iter_.readBrOnCast(onSuccess, &labelRelativeDepth, &sourceType,
                          &destType, &labelType, &unused_values)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  return emitBrOnCastCommon(onSuccess, labelRelativeDepth, labelType,
                            sourceType, destType);
}

bool BaseCompiler::emitAnyConvertExtern() {
  // any.convert_extern is a no-op because anyref and extern share the same
  // representation
  Nothing nothing;
  return iter_.readRefConversion(RefType::extern_(), RefType::any(), &nothing);
}

bool BaseCompiler::emitExternConvertAny() {
  // extern.convert_any is a no-op because anyref and extern share the same
  // representation
  Nothing nothing;
  return iter_.readRefConversion(RefType::any(), RefType::extern_(), &nothing);
}

#endif  // ENABLE_WASM_GC

//////////////////////////////////////////////////////////////////////////////
//
// SIMD and Relaxed SIMD.

#ifdef ENABLE_WASM_SIMD

// Emitter trampolines used by abstracted SIMD operations.  Naming here follows
// the SIMD spec pretty closely.

static void AndV128(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.bitwiseAndSimd128(rs, rsd);
}

static void OrV128(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.bitwiseOrSimd128(rs, rsd);
}

static void XorV128(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.bitwiseXorSimd128(rs, rsd);
}

static void AddI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt8x16(rsd, rs, rsd);
}

static void AddI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt16x8(rsd, rs, rsd);
}

static void AddI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt32x4(rsd, rs, rsd);
}

static void AddF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addFloat32x4(rsd, rs, rsd);
}

static void AddI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt64x2(rsd, rs, rsd);
}

static void AddF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addFloat64x2(rsd, rs, rsd);
}

static void AddSatI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addSatInt8x16(rsd, rs, rsd);
}

static void AddSatUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAddSatInt8x16(rsd, rs, rsd);
}

static void AddSatI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addSatInt16x8(rsd, rs, rsd);
}

static void AddSatUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAddSatInt16x8(rsd, rs, rsd);
}

static void SubI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt8x16(rsd, rs, rsd);
}

static void SubI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt16x8(rsd, rs, rsd);
}

static void SubI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt32x4(rsd, rs, rsd);
}

static void SubF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subFloat32x4(rsd, rs, rsd);
}

static void SubI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt64x2(rsd, rs, rsd);
}

static void SubF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subFloat64x2(rsd, rs, rsd);
}

static void SubSatI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subSatInt8x16(rsd, rs, rsd);
}

static void SubSatUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedSubSatInt8x16(rsd, rs, rsd);
}

static void SubSatI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subSatInt16x8(rsd, rs, rsd);
}

static void SubSatUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedSubSatInt16x8(rsd, rs, rsd);
}

static void MulI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulInt16x8(rsd, rs, rsd);
}

static void MulI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulInt32x4(rsd, rs, rsd);
}

static void MulF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulFloat32x4(rsd, rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void MulI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp) {
  masm.mulInt64x2(rsd, rs, rsd, temp);
}
#  elif defined(JS_CODEGEN_ARM64)
static void MulI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.mulInt64x2(rsd, rs, rsd, temp1, temp2);
}
#  endif

static void MulF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulFloat64x2(rsd, rs, rsd);
}

static void DivF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.divFloat32x4(rsd, rs, rsd);
}

static void DivF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.divFloat64x2(rsd, rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void MinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.minFloat32x4(rsd, rs, rsd, temp1, temp2);
}

static void MinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.minFloat64x2(rsd, rs, rsd, temp1, temp2);
}

static void MaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.maxFloat32x4(rsd, rs, rsd, temp1, temp2);
}

static void MaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.maxFloat64x2(rsd, rs, rsd, temp1, temp2);
}

static void PMinF32x4(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMinFloat32x4(rsd, rs);
}

static void PMinF64x2(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMinFloat64x2(rsd, rs);
}

static void PMaxF32x4(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMaxFloat32x4(rsd, rs);
}

static void PMaxF64x2(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMaxFloat64x2(rsd, rs);
}
#  elif defined(JS_CODEGEN_ARM64)
static void MinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat32x4(rs, rsd);
}

static void MinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat64x2(rs, rsd);
}

static void MaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat32x4(rs, rsd);
}

static void MaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat64x2(rs, rsd);
}

static void PMinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMinFloat32x4(rs, rsd);
}

static void PMinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMinFloat64x2(rs, rsd);
}

static void PMaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMaxFloat32x4(rs, rsd);
}

static void PMaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMaxFloat64x2(rs, rsd);
}
#  endif

static void DotI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.widenDotInt16x8(rsd, rs, rsd);
}

static void ExtMulLowI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulLowInt8x16(rsd, rs, rsd);
}

static void ExtMulHighI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulHighInt8x16(rsd, rs, rsd);
}

static void ExtMulLowUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulLowInt8x16(rsd, rs, rsd);
}

static void ExtMulHighUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulHighInt8x16(rsd, rs, rsd);
}

static void ExtMulLowI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulLowInt16x8(rsd, rs, rsd);
}

static void ExtMulHighI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulHighInt16x8(rsd, rs, rsd);
}

static void ExtMulLowUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulLowInt16x8(rsd, rs, rsd);
}

static void ExtMulHighUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulHighInt16x8(rsd, rs, rsd);
}

static void ExtMulLowI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulLowInt32x4(rsd, rs, rsd);
}

static void ExtMulHighI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulHighInt32x4(rsd, rs, rsd);
}

static void ExtMulLowUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulLowInt32x4(rsd, rs, rsd);
}

static void ExtMulHighUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulHighInt32x4(rsd, rs, rsd);
}

static void Q15MulrSatS(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.q15MulrSatInt16x8(rsd, rs, rsd);
}

static void CmpI8x16(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareInt8x16(cond, rs, rsd);
}

static void CmpI16x8(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareInt16x8(cond, rs, rsd);
}

static void CmpI32x4(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareInt32x4(cond, rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void CmpI64x2ForEquality(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd) {
  masm.compareForEqualityInt64x2(cond, rsd, rs, rsd);
}

static void CmpI64x2ForOrdering(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd, RegV128 temp1,
                                RegV128 temp2) {
  masm.compareForOrderingInt64x2(cond, rsd, rs, rsd, temp1, temp2);
}
#  else
static void CmpI64x2ForEquality(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd) {
  masm.compareInt64x2(cond, rs, rsd);
}

static void CmpI64x2ForOrdering(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd) {
  masm.compareInt64x2(cond, rs, rsd);
}
#  endif  // JS_CODEGEN_X86 || JS_CODEGEN_X64

static void CmpUI8x16(MacroAssembler& masm, Assembler::Condition cond,
                      RegV128 rs, RegV128 rsd) {
  masm.compareInt8x16(cond, rs, rsd);
}

static void CmpUI16x8(MacroAssembler& masm, Assembler::Condition cond,
                      RegV128 rs, RegV128 rsd) {
  masm.compareInt16x8(cond, rs, rsd);
}

static void CmpUI32x4(MacroAssembler& masm, Assembler::Condition cond,
                      RegV128 rs, RegV128 rsd) {
  masm.compareInt32x4(cond, rs, rsd);
}

static void CmpF32x4(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareFloat32x4(cond, rs, rsd);
}

static void CmpF64x2(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareFloat64x2(cond, rs, rsd);
}

static void NegI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt8x16(rs, rd);
}

static void NegI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt16x8(rs, rd);
}

static void NegI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt32x4(rs, rd);
}

static void NegI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt64x2(rs, rd);
}

static void NegF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negFloat32x4(rs, rd);
}

static void NegF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negFloat64x2(rs, rd);
}

static void AbsF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absFloat32x4(rs, rd);
}

static void AbsF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absFloat64x2(rs, rd);
}

static void SqrtF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.sqrtFloat32x4(rs, rd);
}

static void SqrtF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.sqrtFloat64x2(rs, rd);
}

static void CeilF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.ceilFloat32x4(rs, rd);
}

static void FloorF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.floorFloat32x4(rs, rd);
}

static void TruncF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.truncFloat32x4(rs, rd);
}

static void NearestF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.nearestFloat32x4(rs, rd);
}

static void CeilF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.ceilFloat64x2(rs, rd);
}

static void FloorF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.floorFloat64x2(rs, rd);
}

static void TruncF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.truncFloat64x2(rs, rd);
}

static void NearestF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.nearestFloat64x2(rs, rd);
}

static void NotV128(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.bitwiseNotSimd128(rs, rd);
}

static void ExtAddPairwiseI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extAddPairwiseInt8x16(rs, rsd);
}

static void ExtAddPairwiseUI8x16(MacroAssembler& masm, RegV128 rs,
                                 RegV128 rsd) {
  masm.unsignedExtAddPairwiseInt8x16(rs, rsd);
}

static void ExtAddPairwiseI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extAddPairwiseInt16x8(rs, rsd);
}

static void ExtAddPairwiseUI16x8(MacroAssembler& masm, RegV128 rs,
                                 RegV128 rsd) {
  masm.unsignedExtAddPairwiseInt16x8(rs, rsd);
}

static void ShiftOpMask(MacroAssembler& masm, SimdOp op, RegI32 in,
                        RegI32 out) {
  int32_t maskBits;

  masm.mov(in, out);
  if (MacroAssembler::MustMaskShiftCountSimd128(op, &maskBits)) {
    masm.and32(Imm32(maskBits), out);
  }
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void ShiftLeftI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp1, RegV128 temp2) {
  ShiftOpMask(masm, SimdOp::I8x16Shl, rs, temp1);
  masm.leftShiftInt8x16(temp1, rsd, temp2);
}

static void ShiftLeftI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8Shl, rs, temp);
  masm.leftShiftInt16x8(temp, rsd);
}

static void ShiftLeftI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4Shl, rs, temp);
  masm.leftShiftInt32x4(temp, rsd);
}

static void ShiftLeftI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2Shl, rs, temp);
  masm.leftShiftInt64x2(temp, rsd);
}

static void ShiftRightI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp1, RegV128 temp2) {
  ShiftOpMask(masm, SimdOp::I8x16ShrS, rs, temp1);
  masm.rightShiftInt8x16(temp1, rsd, temp2);
}

static void ShiftRightUI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp1, RegV128 temp2) {
  ShiftOpMask(masm, SimdOp::I8x16ShrU, rs, temp1);
  masm.unsignedRightShiftInt8x16(temp1, rsd, temp2);
}

static void ShiftRightI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8ShrS, rs, temp);
  masm.rightShiftInt16x8(temp, rsd);
}

static void ShiftRightUI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8ShrU, rs, temp);
  masm.unsignedRightShiftInt16x8(temp, rsd);
}

static void ShiftRightI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4ShrS, rs, temp);
  masm.rightShiftInt32x4(temp, rsd);
}

static void ShiftRightUI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4ShrU, rs, temp);
  masm.unsignedRightShiftInt32x4(temp, rsd);
}

static void ShiftRightUI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2ShrU, rs, temp);
  masm.unsignedRightShiftInt64x2(temp, rsd);
}
#  elif defined(JS_CODEGEN_ARM64)
static void ShiftLeftI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I8x16Shl, rs, temp);
  masm.leftShiftInt8x16(rsd, temp, rsd);
}

static void ShiftLeftI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8Shl, rs, temp);
  masm.leftShiftInt16x8(rsd, temp, rsd);
}

static void ShiftLeftI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4Shl, rs, temp);
  masm.leftShiftInt32x4(rsd, temp, rsd);
}

static void ShiftLeftI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2Shl, rs, temp);
  masm.leftShiftInt64x2(rsd, temp, rsd);
}

static void ShiftRightI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I8x16ShrS, rs, temp);
  masm.rightShiftInt8x16(rsd, temp, rsd);
}

static void ShiftRightUI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I8x16ShrU, rs, temp);
  masm.unsignedRightShiftInt8x16(rsd, temp, rsd);
}

static void ShiftRightI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8ShrS, rs, temp);
  masm.rightShiftInt16x8(rsd, temp, rsd);
}

static void ShiftRightUI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8ShrU, rs, temp);
  masm.unsignedRightShiftInt16x8(rsd, temp, rsd);
}

static void ShiftRightI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4ShrS, rs, temp);
  masm.rightShiftInt32x4(rsd, temp, rsd);
}

static void ShiftRightUI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4ShrU, rs, temp);
  masm.unsignedRightShiftInt32x4(rsd, temp, rsd);
}

static void ShiftRightI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2ShrS, rs, temp);
  masm.rightShiftInt64x2(rsd, temp, rsd);
}

static void ShiftRightUI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2ShrU, rs, temp);
  masm.unsignedRightShiftInt64x2(rsd, temp, rsd);
}
#  endif

static void AverageUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAverageInt8x16(rsd, rs, rsd);
}

static void AverageUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAverageInt16x8(rsd, rs, rsd);
}

static void MinI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minInt8x16(rsd, rs, rsd);
}

static void MinUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMinInt8x16(rsd, rs, rsd);
}

static void MaxI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxInt8x16(rsd, rs, rsd);
}

static void MaxUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMaxInt8x16(rsd, rs, rsd);
}

static void MinI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minInt16x8(rsd, rs, rsd);
}

static void MinUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMinInt16x8(rsd, rs, rsd);
}

static void MaxI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxInt16x8(rsd, rs, rsd);
}

static void MaxUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMaxInt16x8(rsd, rs, rsd);
}

static void MinI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minInt32x4(rsd, rs, rsd);
}

static void MinUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMinInt32x4(rsd, rs, rsd);
}

static void MaxI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxInt32x4(rsd, rs, rsd);
}

static void MaxUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMaxInt32x4(rsd, rs, rsd);
}

static void NarrowI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.narrowInt16x8(rsd, rs, rsd);
}

static void NarrowUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedNarrowInt16x8(rsd, rs, rsd);
}

static void NarrowI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.narrowInt32x4(rsd, rs, rsd);
}

static void NarrowUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedNarrowInt32x4(rsd, rs, rsd);
}

static void WidenLowI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenLowInt8x16(rs, rd);
}

static void WidenHighI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenHighInt8x16(rs, rd);
}

static void WidenLowUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenLowInt8x16(rs, rd);
}

static void WidenHighUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenHighInt8x16(rs, rd);
}

static void WidenLowI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenLowInt16x8(rs, rd);
}

static void WidenHighI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenHighInt16x8(rs, rd);
}

static void WidenLowUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenLowInt16x8(rs, rd);
}

static void WidenHighUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenHighInt16x8(rs, rd);
}

static void WidenLowI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenLowInt32x4(rs, rd);
}

static void WidenHighI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenHighInt32x4(rs, rd);
}

static void WidenLowUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenLowInt32x4(rs, rd);
}

static void WidenHighUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenHighInt32x4(rs, rd);
}

#  if defined(JS_CODEGEN_ARM64)
static void PopcntI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.popcntInt8x16(rs, rd);
}
#  else
static void PopcntI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                        RegV128 temp) {
  masm.popcntInt8x16(rs, rd, temp);
}
#  endif  // JS_CODEGEN_ARM64

static void AbsI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt8x16(rs, rd);
}

static void AbsI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt16x8(rs, rd);
}

static void AbsI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt32x4(rs, rd);
}

static void AbsI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt64x2(rs, rd);
}

static void ExtractLaneI8x16(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI32 rd) {
  masm.extractLaneInt8x16(laneIndex, rs, rd);
}

static void ExtractLaneUI8x16(MacroAssembler& masm, uint32_t laneIndex,
                              RegV128 rs, RegI32 rd) {
  masm.unsignedExtractLaneInt8x16(laneIndex, rs, rd);
}

static void ExtractLaneI16x8(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI32 rd) {
  masm.extractLaneInt16x8(laneIndex, rs, rd);
}

static void ExtractLaneUI16x8(MacroAssembler& masm, uint32_t laneIndex,
                              RegV128 rs, RegI32 rd) {
  masm.unsignedExtractLaneInt16x8(laneIndex, rs, rd);
}

static void ExtractLaneI32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI32 rd) {
  masm.extractLaneInt32x4(laneIndex, rs, rd);
}

static void ExtractLaneI64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI64 rd) {
  masm.extractLaneInt64x2(laneIndex, rs, rd);
}

static void ExtractLaneF32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegF32 rd) {
  masm.extractLaneFloat32x4(laneIndex, rs, rd);
}

static void ExtractLaneF64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegF64 rd) {
  masm.extractLaneFloat64x2(laneIndex, rs, rd);
}

static void ReplaceLaneI8x16(MacroAssembler& masm, uint32_t laneIndex,
                             RegI32 rs, RegV128 rsd) {
  masm.replaceLaneInt8x16(laneIndex, rs, rsd);
}

static void ReplaceLaneI16x8(MacroAssembler& masm, uint32_t laneIndex,
                             RegI32 rs, RegV128 rsd) {
  masm.replaceLaneInt16x8(laneIndex, rs, rsd);
}

static void ReplaceLaneI32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegI32 rs, RegV128 rsd) {
  masm.replaceLaneInt32x4(laneIndex, rs, rsd);
}

static void ReplaceLaneI64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegI64 rs, RegV128 rsd) {
  masm.replaceLaneInt64x2(laneIndex, rs, rsd);
}

static void ReplaceLaneF32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegF32 rs, RegV128 rsd) {
  masm.replaceLaneFloat32x4(laneIndex, rs, rsd);
}

static void ReplaceLaneF64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegF64 rs, RegV128 rsd) {
  masm.replaceLaneFloat64x2(laneIndex, rs, rsd);
}

static void SplatI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rd) {
  masm.splatX16(rs, rd);
}

static void SplatI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rd) {
  masm.splatX8(rs, rd);
}

static void SplatI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rd) {
  masm.splatX4(rs, rd);
}

static void SplatI64x2(MacroAssembler& masm, RegI64 rs, RegV128 rd) {
  masm.splatX2(rs, rd);
}

static void SplatF32x4(MacroAssembler& masm, RegF32 rs, RegV128 rd) {
  masm.splatX4(rs, rd);
}

static void SplatF64x2(MacroAssembler& masm, RegF64 rs, RegV128 rd) {
  masm.splatX2(rs, rd);
}

// This is the same op independent of lanes: it tests for any nonzero bit.
static void AnyTrue(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.anyTrueSimd128(rs, rd);
}

static void AllTrueI8x16(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt8x16(rs, rd);
}

static void AllTrueI16x8(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt16x8(rs, rd);
}

static void AllTrueI32x4(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt32x4(rs, rd);
}

static void AllTrueI64x2(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt64x2(rs, rd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void BitmaskI8x16(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt8x16(rs, rd);
}

static void BitmaskI16x8(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt16x8(rs, rd);
}

static void BitmaskI32x4(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt32x4(rs, rd);
}

static void BitmaskI64x2(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt64x2(rs, rd);
}
#  elif defined(JS_CODEGEN_ARM64)
static void BitmaskI8x16(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt8x16(rs, rd, temp);
}

static void BitmaskI16x8(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt16x8(rs, rd, temp);
}

static void BitmaskI32x4(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt32x4(rs, rd, temp);
}

static void BitmaskI64x2(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt64x2(rs, rd, temp);
}
#  endif

static void Swizzle(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.swizzleInt8x16(rsd, rs, rsd);
}

static void ConvertI32x4ToF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertInt32x4ToFloat32x4(rs, rd);
}

static void ConvertUI32x4ToF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedConvertInt32x4ToFloat32x4(rs, rd);
}

static void ConvertF32x4ToI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.truncSatFloat32x4ToInt32x4(rs, rd);
}

#  if defined(JS_CODEGEN_ARM64)
static void ConvertF32x4ToUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedTruncSatFloat32x4ToInt32x4(rs, rd);
}
#  else
static void ConvertF32x4ToUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                                 RegV128 temp) {
  masm.unsignedTruncSatFloat32x4ToInt32x4(rs, rd, temp);
}
#  endif  // JS_CODEGEN_ARM64

static void ConvertI32x4ToF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertInt32x4ToFloat64x2(rs, rd);
}

static void ConvertUI32x4ToF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedConvertInt32x4ToFloat64x2(rs, rd);
}

static void ConvertF64x2ToI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                                RegV128 temp) {
  masm.truncSatFloat64x2ToInt32x4(rs, rd, temp);
}

static void ConvertF64x2ToUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                                 RegV128 temp) {
  masm.unsignedTruncSatFloat64x2ToInt32x4(rs, rd, temp);
}

static void DemoteF64x2ToF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertFloat64x2ToFloat32x4(rs, rd);
}

static void PromoteF32x4ToF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertFloat32x4ToFloat64x2(rs, rd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void BitselectV128(MacroAssembler& masm, RegV128 rhs, RegV128 control,
                          RegV128 lhsDest, RegV128 temp) {
  // Ideally, we would have temp=control, and we can probably get away with
  // just doing that, but don't worry about it yet.
  masm.bitwiseSelectSimd128(control, lhsDest, rhs, lhsDest, temp);
}
#  elif defined(JS_CODEGEN_ARM64)
static void BitselectV128(MacroAssembler& masm, RegV128 rhs, RegV128 control,
                          RegV128 lhsDest, RegV128 temp) {
  // The masm interface is not great for the baseline compiler here, but it's
  // optimal for Ion, so just work around it.
  masm.moveSimd128(control, temp);
  masm.bitwiseSelectSimd128(lhsDest, rhs, temp);
  masm.moveSimd128(temp, lhsDest);
}
#  endif

#  ifdef ENABLE_WASM_RELAXED_SIMD
static void RelaxedMaddF32x4(MacroAssembler& masm, RegV128 rs1, RegV128 rs2,
                             RegV128 rsd) {
  masm.fmaFloat32x4(rs1, rs2, rsd);
}

static void RelaxedNmaddF32x4(MacroAssembler& masm, RegV128 rs1, RegV128 rs2,
                              RegV128 rsd) {
  masm.fnmaFloat32x4(rs1, rs2, rsd);
}

static void RelaxedMaddF64x2(MacroAssembler& masm, RegV128 rs1, RegV128 rs2,
                             RegV128 rsd) {
  masm.fmaFloat64x2(rs1, rs2, rsd);
}

static void RelaxedNmaddF64x2(MacroAssembler& masm, RegV128 rs1, RegV128 rs2,
                              RegV128 rsd) {
  masm.fnmaFloat64x2(rs1, rs2, rsd);
}

static void RelaxedSwizzle(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.swizzleInt8x16Relaxed(rsd, rs, rsd);
}

static void RelaxedMinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat32x4Relaxed(rs, rsd);
}

static void RelaxedMaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat32x4Relaxed(rs, rsd);
}

static void RelaxedMinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat64x2Relaxed(rs, rsd);
}

static void RelaxedMaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat64x2Relaxed(rs, rsd);
}

static void RelaxedConvertF32x4ToI32x4(MacroAssembler& masm, RegV128 rs,
                                       RegV128 rd) {
  masm.truncFloat32x4ToInt32x4Relaxed(rs, rd);
}

static void RelaxedConvertF32x4ToUI32x4(MacroAssembler& masm, RegV128 rs,
                                        RegV128 rd) {
  masm.unsignedTruncFloat32x4ToInt32x4Relaxed(rs, rd);
}

static void RelaxedConvertF64x2ToI32x4(MacroAssembler& masm, RegV128 rs,
                                       RegV128 rd) {
  masm.truncFloat64x2ToInt32x4Relaxed(rs, rd);
}

static void RelaxedConvertF64x2ToUI32x4(MacroAssembler& masm, RegV128 rs,
                                        RegV128 rd) {
  masm.unsignedTruncFloat64x2ToInt32x4Relaxed(rs, rd);
}

static void RelaxedQ15MulrS(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.q15MulrInt16x8Relaxed(rsd, rs, rsd);
}

static void DotI8x16I7x16S(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.dotInt8x16Int7x16(rsd, rs, rsd);
}

void BaseCompiler::emitDotI8x16I7x16AddS() {
  RegV128 rsd = popV128();
  RegV128 rs0, rs1;
  pop2xV128(&rs0, &rs1);
#    if defined(JS_CODEGEN_ARM64)
  RegV128 temp = needV128();
  masm.dotInt8x16Int7x16ThenAdd(rs0, rs1, rsd, temp);
  freeV128(temp);
#    else
  masm.dotInt8x16Int7x16ThenAdd(rs0, rs1, rsd);
#    endif
  freeV128(rs1);
  freeV128(rs0);
  pushV128(rsd);
}
#  endif  // ENABLE_WASM_RELAXED_SIMD

void BaseCompiler::emitVectorAndNot() {
  // We want x & ~y but the available operation is ~x & y, so reverse the
  // operands.
  RegV128 r, rs;
  pop2xV128(&r, &rs);
  masm.bitwiseNotAndSimd128(r, rs);
  freeV128(r);
  pushV128(rs);
}

bool BaseCompiler::emitLoadSplat(Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoadSplat(Scalar::byteSize(viewType), &addr)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(),
                          hugeMemoryEnabled(addr.memoryIndex));
  loadSplat(&access);
  return true;
}

bool BaseCompiler::emitLoadZero(Scalar::Type viewType) {
  // LoadZero has the structure of LoadSplat, so reuse the reader.
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoadSplat(Scalar::byteSize(viewType), &addr)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(),
                          hugeMemoryEnabled(addr.memoryIndex));
  loadZero(&access);
  return true;
}

bool BaseCompiler::emitLoadExtend(Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoadExtend(&addr)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, Scalar::Int64, addr.align,
                          addr.offset, bytecodeOffset(),
                          hugeMemoryEnabled(addr.memoryIndex));
  loadExtend(&access, viewType);
  return true;
}

bool BaseCompiler::emitLoadLane(uint32_t laneSize) {
  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  uint32_t laneIndex;
  if (!iter_.readLoadLane(laneSize, &addr, &laneIndex, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  Scalar::Type viewType;
  switch (laneSize) {
    case 1:
      viewType = Scalar::Uint8;
      break;
    case 2:
      viewType = Scalar::Uint16;
      break;
    case 4:
      viewType = Scalar::Int32;
      break;
    case 8:
      viewType = Scalar::Int64;
      break;
    default:
      MOZ_CRASH("unsupported laneSize");
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(),
                          hugeMemoryEnabled(addr.memoryIndex));
  loadLane(&access, laneIndex);
  return true;
}

bool BaseCompiler::emitStoreLane(uint32_t laneSize) {
  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  uint32_t laneIndex;
  if (!iter_.readStoreLane(laneSize, &addr, &laneIndex, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  Scalar::Type viewType;
  switch (laneSize) {
    case 1:
      viewType = Scalar::Uint8;
      break;
    case 2:
      viewType = Scalar::Uint16;
      break;
    case 4:
      viewType = Scalar::Int32;
      break;
    case 8:
      viewType = Scalar::Int64;
      break;
    default:
      MOZ_CRASH("unsupported laneSize");
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          bytecodeOffset(),
                          hugeMemoryEnabled(addr.memoryIndex));
  storeLane(&access, laneIndex);
  return true;
}

bool BaseCompiler::emitVectorShuffle() {
  Nothing unused_a, unused_b;
  V128 shuffleMask;

  if (!iter_.readVectorShuffle(&unused_a, &unused_b, &shuffleMask)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegV128 rd, rs;
  pop2xV128(&rd, &rs);

  masm.shuffleInt8x16(shuffleMask.bytes, rs, rd);

  freeV128(rs);
  pushV128(rd);

  return true;
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
bool BaseCompiler::emitVectorShiftRightI64x2() {
  Nothing unused_a, unused_b;

  if (!iter_.readVectorShift(&unused_a, &unused_b)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegI32 count = popI32RhsForShiftI64();
  RegV128 lhsDest = popV128();
  RegI64 tmp = needI64();
  masm.and32(Imm32(63), count);
  masm.extractLaneInt64x2(0, lhsDest, tmp);
  masm.rshift64Arithmetic(count, tmp);
  masm.replaceLaneInt64x2(0, tmp, lhsDest);
  masm.extractLaneInt64x2(1, lhsDest, tmp);
  masm.rshift64Arithmetic(count, tmp);
  masm.replaceLaneInt64x2(1, tmp, lhsDest);
  freeI64(tmp);
  freeI32(count);
  pushV128(lhsDest);

  return true;
}
#  endif
#endif  // ENABLE_WASM_SIMD

#ifdef ENABLE_WASM_RELAXED_SIMD
bool BaseCompiler::emitVectorLaneSelect() {
  Nothing unused_a, unused_b, unused_c;

  if (!iter_.readTernary(ValType::V128, &unused_a, &unused_b, &unused_c)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  RegV128 mask = popV128(RegV128(vmm0));
  RegV128 rhsDest = popV128();
  RegV128 lhs = popV128();
  masm.laneSelectSimd128(mask, lhs, rhsDest, rhsDest);
  freeV128(lhs);
  freeV128(mask);
  pushV128(rhsDest);
#  elif defined(JS_CODEGEN_ARM64)
  RegV128 maskDest = popV128();
  RegV128 rhs = popV128();
  RegV128 lhs = popV128();
  masm.laneSelectSimd128(maskDest, lhs, rhs, maskDest);
  freeV128(lhs);
  freeV128(rhs);
  pushV128(maskDest);
#  endif

  return true;
}
#endif  // ENABLE_WASM_RELAXED_SIMD

//////////////////////////////////////////////////////////////////////////////
//
// "Builtin module funcs" - magically imported functions for internal use.

bool BaseCompiler::emitCallBuiltinModuleFunc() {
  const BuiltinModuleFunc* builtinModuleFunc;

  BaseNothingVector params;
  if (!iter_.readCallBuiltinModuleFunc(&builtinModuleFunc, &params)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  if (builtinModuleFunc->usesMemory()) {
    // The final parameter of an builtinModuleFunc is implicitly the heap base
    pushHeapBase(0);
  }

  // Call the builtinModuleFunc
  return emitInstanceCall(*builtinModuleFunc->sig());
}

//////////////////////////////////////////////////////////////////////////////
//
// Function bodies - main opcode dispatch loop.

bool BaseCompiler::emitBody() {
  AutoCreatedBy acb(masm, "(wasm)BaseCompiler::emitBody");

  MOZ_ASSERT(stackMapGenerator_.framePushedAtEntryToBody.isSome());

  if (!iter_.startFunction(func_.index, locals_)) {
    return false;
  }

  initControl(controlItem(), ResultType::Empty());

  for (;;) {
    Nothing unused_a, unused_b, unused_c;
    (void)unused_a;
    (void)unused_b;
    (void)unused_c;

#ifdef DEBUG
    performRegisterLeakCheck();
    assertStackInvariants();
#endif

#define dispatchBinary0(doEmit, type)             \
  iter_.readBinary(type, &unused_a, &unused_b) && \
      (deadCode_ || (doEmit(), true))

#define dispatchBinary1(arg1, type)               \
  iter_.readBinary(type, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(arg1), true))

#define dispatchBinary2(arg1, arg2, type)         \
  iter_.readBinary(type, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(arg1, arg2), true))

#define dispatchBinary3(arg1, arg2, arg3, type)   \
  iter_.readBinary(type, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(arg1, arg2, arg3), true))

#define dispatchUnary0(doEmit, type) \
  iter_.readUnary(type, &unused_a) && (deadCode_ || (doEmit(), true))

#define dispatchUnary1(arg1, type) \
  iter_.readUnary(type, &unused_a) && (deadCode_ || (emitUnop(arg1), true))

#define dispatchUnary2(arg1, arg2, type) \
  iter_.readUnary(type, &unused_a) &&    \
      (deadCode_ || (emitUnop(arg1, arg2), true))

#define dispatchTernary0(doEmit, type)                        \
  iter_.readTernary(type, &unused_a, &unused_b, &unused_c) && \
      (deadCode_ || (doEmit(), true))

#define dispatchTernary1(arg1, type)                          \
  iter_.readTernary(type, &unused_a, &unused_b, &unused_c) && \
      (deadCode_ || (emitTernary(arg1), true))

#define dispatchTernary2(arg1, type)                          \
  iter_.readTernary(type, &unused_a, &unused_b, &unused_c) && \
      (deadCode_ || (emitTernaryResultLast(arg1), true))

#define dispatchComparison0(doEmit, operandType, compareOp)  \
  iter_.readComparison(operandType, &unused_a, &unused_b) && \
      (deadCode_ || (doEmit(compareOp, operandType), true))

#define dispatchConversion0(doEmit, inType, outType)  \
  iter_.readConversion(inType, outType, &unused_a) && \
      (deadCode_ || (doEmit(), true))

#define dispatchConversion1(arg1, inType, outType)    \
  iter_.readConversion(inType, outType, &unused_a) && \
      (deadCode_ || (emitUnop(arg1), true))

#define dispatchConversionOOM(doEmit, inType, outType) \
  iter_.readConversion(inType, outType, &unused_a) && (deadCode_ || doEmit())

#define dispatchCalloutConversionOOM(doEmit, symbol, inType, outType) \
  iter_.readConversion(inType, outType, &unused_a) &&                 \
      (deadCode_ || doEmit(symbol, inType, outType))

#define dispatchIntDivCallout(doEmit, symbol, type) \
  iter_.readBinary(type, &unused_a, &unused_b) &&   \
      (deadCode_ || doEmit(symbol, type))

#define dispatchVectorBinary(op)                           \
  iter_.readBinary(ValType::V128, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(op), true))

#define dispatchVectorUnary(op)                \
  iter_.readUnary(ValType::V128, &unused_a) && \
      (deadCode_ || (emitUnop(op), true))

#define dispatchVectorComparison(op, compareOp)            \
  iter_.readBinary(ValType::V128, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(compareOp, op), true))

#define dispatchVectorVariableShift(op)          \
  iter_.readVectorShift(&unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(op), true))

#define dispatchExtractLane(op, outType, laneLimit)                   \
  iter_.readExtractLane(outType, laneLimit, &laneIndex, &unused_a) && \
      (deadCode_ || (emitUnop(laneIndex, op), true))

#define dispatchReplaceLane(op, inType, laneLimit)                \
  iter_.readReplaceLane(inType, laneLimit, &laneIndex, &unused_a, \
                        &unused_b) &&                             \
      (deadCode_ || (emitBinop(laneIndex, op), true))

#define dispatchSplat(op, inType)                           \
  iter_.readConversion(inType, ValType::V128, &unused_a) && \
      (deadCode_ || (emitUnop(op), true))

#define dispatchVectorReduction(op)                               \
  iter_.readConversion(ValType::V128, ValType::I32, &unused_a) && \
      (deadCode_ || (emitUnop(op), true))

#ifdef DEBUG
    // Check that the number of ref-typed entries in the operand stack matches
    // reality.
#  define CHECK_POINTER_COUNT                                             \
    do {                                                                  \
      MOZ_ASSERT(countMemRefsOnStk() == stackMapGenerator_.memRefsOnStk); \
    } while (0)
#else
#  define CHECK_POINTER_COUNT \
    do {                      \
    } while (0)
#endif

#define CHECK(E) \
  if (!(E)) return false
#define NEXT()           \
  {                      \
    CHECK_POINTER_COUNT; \
    continue;            \
  }
#define CHECK_NEXT(E)     \
  if (!(E)) return false; \
  {                       \
    CHECK_POINTER_COUNT;  \
    continue;             \
  }

    // Opcodes that push more than MaxPushesPerOpcode (anything with multiple
    // results) will perform additional reservation.
    CHECK(stk_.reserve(stk_.length() + MaxPushesPerOpcode));

    OpBytes op{};
    CHECK(iter_.readOp(&op));

    // When compilerEnv_.debugEnabled(), some operators get a breakpoint site.
    if (compilerEnv_.debugEnabled() && op.shouldHaveBreakpoint()) {
      if (previousBreakablePoint_ != masm.currentOffset()) {
        // TODO sync only registers that can be clobbered by the exit
        // prologue/epilogue or disable these registers for use in
        // baseline compiler when compilerEnv_.debugEnabled() is set.
        sync();

        insertBreakablePoint(CallSiteDesc::Breakpoint);
        if (!createStackMap("debug: per-insn breakpoint")) {
          return false;
        }
        previousBreakablePoint_ = masm.currentOffset();
      }
    }

    // Going below framePushedAtEntryToBody would imply that we've
    // popped off the machine stack, part of the frame created by
    // beginFunction().
    MOZ_ASSERT(masm.framePushed() >=
               stackMapGenerator_.framePushedAtEntryToBody.value());

    // At this point we're definitely not generating code for a function call.
    MOZ_ASSERT(
        stackMapGenerator_.framePushedExcludingOutboundCallArgs.isNothing());

    switch (op.b0) {
      case uint16_t(Op::End):
        if (!emitEnd()) {
          return false;
        }
        if (iter_.controlStackEmpty()) {
          return true;
        }
        NEXT();

      // Control opcodes
      case uint16_t(Op::Nop):
        CHECK_NEXT(iter_.readNop());
      case uint16_t(Op::Drop):
        CHECK_NEXT(emitDrop());
      case uint16_t(Op::Block):
        CHECK_NEXT(emitBlock());
      case uint16_t(Op::Loop):
        CHECK_NEXT(emitLoop());
      case uint16_t(Op::If):
        CHECK_NEXT(emitIf());
      case uint16_t(Op::Else):
        CHECK_NEXT(emitElse());
      case uint16_t(Op::Try):
        CHECK_NEXT(emitTry());
      case uint16_t(Op::Catch):
        CHECK_NEXT(emitCatch());
      case uint16_t(Op::CatchAll):
        CHECK_NEXT(emitCatchAll());
      case uint16_t(Op::Delegate):
        CHECK(emitDelegate());
        iter_.popDelegate();
        NEXT();
      case uint16_t(Op::Throw):
        CHECK_NEXT(emitThrow());
      case uint16_t(Op::Rethrow):
        CHECK_NEXT(emitRethrow());
      case uint16_t(Op::ThrowRef):
        if (!moduleEnv_.exnrefEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitThrowRef());
      case uint16_t(Op::TryTable):
        if (!moduleEnv_.exnrefEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitTryTable());
      case uint16_t(Op::Br):
        CHECK_NEXT(emitBr());
      case uint16_t(Op::BrIf):
        CHECK_NEXT(emitBrIf());
      case uint16_t(Op::BrTable):
        CHECK_NEXT(emitBrTable());
      case uint16_t(Op::Return):
        CHECK_NEXT(emitReturn());
      case uint16_t(Op::Unreachable):
        CHECK(iter_.readUnreachable());
        if (!deadCode_) {
          trap(Trap::Unreachable);
          deadCode_ = true;
        }
        NEXT();

      // Calls
      case uint16_t(Op::Call):
        CHECK_NEXT(emitCall());
      case uint16_t(Op::CallIndirect):
        CHECK_NEXT(emitCallIndirect());
#ifdef ENABLE_WASM_TAIL_CALLS
      case uint16_t(Op::ReturnCall):
        if (!moduleEnv_.tailCallsEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitReturnCall());
      case uint16_t(Op::ReturnCallIndirect):
        if (!moduleEnv_.tailCallsEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitReturnCallIndirect());
#endif
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::CallRef):
        if (!moduleEnv_.gcEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitCallRef());
#  ifdef ENABLE_WASM_TAIL_CALLS
      case uint16_t(Op::ReturnCallRef):
        if (!moduleEnv_.gcEnabled() || !moduleEnv_.tailCallsEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitReturnCallRef());
#  endif
#endif

      // Locals and globals
      case uint16_t(Op::LocalGet):
        CHECK_NEXT(emitGetLocal());
      case uint16_t(Op::LocalSet):
        CHECK_NEXT(emitSetLocal());
      case uint16_t(Op::LocalTee):
        CHECK_NEXT(emitTeeLocal());
      case uint16_t(Op::GlobalGet):
        CHECK_NEXT(emitGetGlobal());
      case uint16_t(Op::GlobalSet):
        CHECK_NEXT(emitSetGlobal());
      case uint16_t(Op::TableGet):
        CHECK_NEXT(emitTableGet());
      case uint16_t(Op::TableSet):
        CHECK_NEXT(emitTableSet());

      // Select
      case uint16_t(Op::SelectNumeric):
        CHECK_NEXT(emitSelect(/*typed*/ false));
      case uint16_t(Op::SelectTyped):
        CHECK_NEXT(emitSelect(/*typed*/ true));

      // I32
      case uint16_t(Op::I32Const): {
        int32_t i32;
        CHECK(iter_.readI32Const(&i32));
        if (!deadCode_) {
          pushI32(i32);
        }
        NEXT();
      }
      case uint16_t(Op::I32Add):
        CHECK_NEXT(dispatchBinary2(AddI32, AddImmI32, ValType::I32));
      case uint16_t(Op::I32Sub):
        CHECK_NEXT(dispatchBinary2(SubI32, SubImmI32, ValType::I32));
      case uint16_t(Op::I32Mul):
        CHECK_NEXT(dispatchBinary1(MulI32, ValType::I32));
      case uint16_t(Op::I32DivS):
        CHECK_NEXT(dispatchBinary0(emitQuotientI32, ValType::I32));
      case uint16_t(Op::I32DivU):
        CHECK_NEXT(dispatchBinary0(emitQuotientU32, ValType::I32));
      case uint16_t(Op::I32RemS):
        CHECK_NEXT(dispatchBinary0(emitRemainderI32, ValType::I32));
      case uint16_t(Op::I32RemU):
        CHECK_NEXT(dispatchBinary0(emitRemainderU32, ValType::I32));
      case uint16_t(Op::I32Eqz):
        CHECK_NEXT(dispatchConversion0(emitEqzI32, ValType::I32, ValType::I32));
      case uint16_t(Op::I32TruncF32S):
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF32ToI32<0>, ValType::F32,
                                         ValType::I32));
      case uint16_t(Op::I32TruncF32U):
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF32ToI32<TRUNC_UNSIGNED>,
                                         ValType::F32, ValType::I32));
      case uint16_t(Op::I32TruncF64S):
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF64ToI32<0>, ValType::F64,
                                         ValType::I32));
      case uint16_t(Op::I32TruncF64U):
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF64ToI32<TRUNC_UNSIGNED>,
                                         ValType::F64, ValType::I32));
      case uint16_t(Op::I32WrapI64):
        CHECK_NEXT(
            dispatchConversion1(WrapI64ToI32, ValType::I64, ValType::I32));
      case uint16_t(Op::I32ReinterpretF32):
        CHECK_NEXT(dispatchConversion1(ReinterpretF32AsI32, ValType::F32,
                                       ValType::I32));
      case uint16_t(Op::I32Clz):
        CHECK_NEXT(dispatchUnary1(ClzI32, ValType::I32));
      case uint16_t(Op::I32Ctz):
        CHECK_NEXT(dispatchUnary1(CtzI32, ValType::I32));
      case uint16_t(Op::I32Popcnt):
        CHECK_NEXT(dispatchUnary2(PopcntI32, PopcntTemp, ValType::I32));
      case uint16_t(Op::I32Or):
        CHECK_NEXT(dispatchBinary2(OrI32, OrImmI32, ValType::I32));
      case uint16_t(Op::I32And):
        CHECK_NEXT(dispatchBinary2(AndI32, AndImmI32, ValType::I32));
      case uint16_t(Op::I32Xor):
        CHECK_NEXT(dispatchBinary2(XorI32, XorImmI32, ValType::I32));
      case uint16_t(Op::I32Shl):
        CHECK_NEXT(dispatchBinary3(
            ShlI32, ShlImmI32, &BaseCompiler::popI32RhsForShift, ValType::I32));
      case uint16_t(Op::I32ShrS):
        CHECK_NEXT(dispatchBinary3(
            ShrI32, ShrImmI32, &BaseCompiler::popI32RhsForShift, ValType::I32));
      case uint16_t(Op::I32ShrU):
        CHECK_NEXT(dispatchBinary3(ShrUI32, ShrUImmI32,
                                   &BaseCompiler::popI32RhsForShift,
                                   ValType::I32));
      case uint16_t(Op::I32Load8S):
        CHECK_NEXT(emitLoad(ValType::I32, Scalar::Int8));
      case uint16_t(Op::I32Load8U):
        CHECK_NEXT(emitLoad(ValType::I32, Scalar::Uint8));
      case uint16_t(Op::I32Load16S):
        CHECK_NEXT(emitLoad(ValType::I32, Scalar::Int16));
      case uint16_t(Op::I32Load16U):
        CHECK_NEXT(emitLoad(ValType::I32, Scalar::Uint16));
      case uint16_t(Op::I32Load):
        CHECK_NEXT(emitLoad(ValType::I32, Scalar::Int32));
      case uint16_t(Op::I32Store8):
        CHECK_NEXT(emitStore(ValType::I32, Scalar::Int8));
      case uint16_t(Op::I32Store16):
        CHECK_NEXT(emitStore(ValType::I32, Scalar::Int16));
      case uint16_t(Op::I32Store):
        CHECK_NEXT(emitStore(ValType::I32, Scalar::Int32));
      case uint16_t(Op::I32Rotr):
        CHECK_NEXT(dispatchBinary3(RotrI32, RotrImmI32,
                                   &BaseCompiler::popI32RhsForRotate,
                                   ValType::I32));
      case uint16_t(Op::I32Rotl):
        CHECK_NEXT(dispatchBinary3(RotlI32, RotlImmI32,
                                   &BaseCompiler::popI32RhsForRotate,
                                   ValType::I32));

      // I64
      case uint16_t(Op::I64Const): {
        int64_t i64;
        CHECK(iter_.readI64Const(&i64));
        if (!deadCode_) {
          pushI64(i64);
        }
        NEXT();
      }
      case uint16_t(Op::I64Add):
        CHECK_NEXT(dispatchBinary2(AddI64, AddImmI64, ValType::I64));
      case uint16_t(Op::I64Sub):
        CHECK_NEXT(dispatchBinary2(SubI64, SubImmI64, ValType::I64));
      case uint16_t(Op::I64Mul):
        CHECK_NEXT(dispatchBinary0(emitMultiplyI64, ValType::I64));
      case uint16_t(Op::I64DivS):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
        CHECK_NEXT(dispatchIntDivCallout(
            emitDivOrModI64BuiltinCall, SymbolicAddress::DivI64, ValType::I64));
#else
        CHECK_NEXT(dispatchBinary0(emitQuotientI64, ValType::I64));
#endif
      case uint16_t(Op::I64DivU):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
        CHECK_NEXT(dispatchIntDivCallout(emitDivOrModI64BuiltinCall,
                                         SymbolicAddress::UDivI64,
                                         ValType::I64));
#else
        CHECK_NEXT(dispatchBinary0(emitQuotientU64, ValType::I64));
#endif
      case uint16_t(Op::I64RemS):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
        CHECK_NEXT(dispatchIntDivCallout(
            emitDivOrModI64BuiltinCall, SymbolicAddress::ModI64, ValType::I64));
#else
        CHECK_NEXT(dispatchBinary0(emitRemainderI64, ValType::I64));
#endif
      case uint16_t(Op::I64RemU):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
        CHECK_NEXT(dispatchIntDivCallout(emitDivOrModI64BuiltinCall,
                                         SymbolicAddress::UModI64,
                                         ValType::I64));
#else
        CHECK_NEXT(dispatchBinary0(emitRemainderU64, ValType::I64));
#endif
      case uint16_t(Op::I64TruncF32S):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
        CHECK_NEXT(
            dispatchCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                         SymbolicAddress::TruncateDoubleToInt64,
                                         ValType::F32, ValType::I64));
#else
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF32ToI64<0>, ValType::F32,
                                         ValType::I64));
#endif
      case uint16_t(Op::I64TruncF32U):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertFloatingToInt64Callout,
            SymbolicAddress::TruncateDoubleToUint64, ValType::F32,
            ValType::I64));
#else
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF32ToI64<TRUNC_UNSIGNED>,
                                         ValType::F32, ValType::I64));
#endif
      case uint16_t(Op::I64TruncF64S):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
        CHECK_NEXT(
            dispatchCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                         SymbolicAddress::TruncateDoubleToInt64,
                                         ValType::F64, ValType::I64));
#else
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF64ToI64<0>, ValType::F64,
                                         ValType::I64));
#endif
      case uint16_t(Op::I64TruncF64U):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertFloatingToInt64Callout,
            SymbolicAddress::TruncateDoubleToUint64, ValType::F64,
            ValType::I64));
#else
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF64ToI64<TRUNC_UNSIGNED>,
                                         ValType::F64, ValType::I64));
#endif
      case uint16_t(Op::I64ExtendI32S):
        CHECK_NEXT(dispatchConversion0(emitExtendI32ToI64, ValType::I32,
                                       ValType::I64));
      case uint16_t(Op::I64ExtendI32U):
        CHECK_NEXT(dispatchConversion0(emitExtendU32ToI64, ValType::I32,
                                       ValType::I64));
      case uint16_t(Op::I64ReinterpretF64):
        CHECK_NEXT(dispatchConversion1(ReinterpretF64AsI64, ValType::F64,
                                       ValType::I64));
      case uint16_t(Op::I64Or):
        CHECK_NEXT(dispatchBinary2(OrI64, OrImmI64, ValType::I64));
      case uint16_t(Op::I64And):
        CHECK_NEXT(dispatchBinary2(AndI64, AndImmI64, ValType::I64));
      case uint16_t(Op::I64Xor):
        CHECK_NEXT(dispatchBinary2(XorI64, XorImmI64, ValType::I64));
      case uint16_t(Op::I64Shl):
        CHECK_NEXT(dispatchBinary3(
            ShlI64, ShlImmI64, &BaseCompiler::popI64RhsForShift, ValType::I64));
      case uint16_t(Op::I64ShrS):
        CHECK_NEXT(dispatchBinary3(
            ShrI64, ShrImmI64, &BaseCompiler::popI64RhsForShift, ValType::I64));
      case uint16_t(Op::I64ShrU):
        CHECK_NEXT(dispatchBinary3(ShrUI64, ShrUImmI64,
                                   &BaseCompiler::popI64RhsForShift,
                                   ValType::I64));
      case uint16_t(Op::I64Rotr):
        CHECK_NEXT(dispatchBinary0(emitRotrI64, ValType::I64));
      case uint16_t(Op::I64Rotl):
        CHECK_NEXT(dispatchBinary0(emitRotlI64, ValType::I64));
      case uint16_t(Op::I64Clz):
        CHECK_NEXT(dispatchUnary1(ClzI64, ValType::I64));
      case uint16_t(Op::I64Ctz):
        CHECK_NEXT(dispatchUnary1(CtzI64, ValType::I64));
      case uint16_t(Op::I64Popcnt):
        CHECK_NEXT(dispatchUnary2(PopcntI64, PopcntTemp, ValType::I64));
      case uint16_t(Op::I64Eqz):
        CHECK_NEXT(dispatchConversion0(emitEqzI64, ValType::I64, ValType::I32));
      case uint16_t(Op::I64Load8S):
        CHECK_NEXT(emitLoad(ValType::I64, Scalar::Int8));
      case uint16_t(Op::I64Load16S):
        CHECK_NEXT(emitLoad(ValType::I64, Scalar::Int16));
      case uint16_t(Op::I64Load32S):
        CHECK_NEXT(emitLoad(ValType::I64, Scalar::Int32));
      case uint16_t(Op::I64Load8U):
        CHECK_NEXT(emitLoad(ValType::I64, Scalar::Uint8));
      case uint16_t(Op::I64Load16U):
        CHECK_NEXT(emitLoad(ValType::I64, Scalar::Uint16));
      case uint16_t(Op::I64Load32U):
        CHECK_NEXT(emitLoad(ValType::I64, Scalar::Uint32));
      case uint16_t(Op::I64Load):
        CHECK_NEXT(emitLoad(ValType::I64, Scalar::Int64));
      case uint16_t(Op::I64Store8):
        CHECK_NEXT(emitStore(ValType::I64, Scalar::Int8));
      case uint16_t(Op::I64Store16):
        CHECK_NEXT(emitStore(ValType::I64, Scalar::Int16));
      case uint16_t(Op::I64Store32):
        CHECK_NEXT(emitStore(ValType::I64, Scalar::Int32));
      case uint16_t(Op::I64Store):
        CHECK_NEXT(emitStore(ValType::I64, Scalar::Int64));

      // F32
      case uint16_t(Op::F32Const): {
        float f32;
        CHECK(iter_.readF32Const(&f32));
        if (!deadCode_) {
          pushF32(f32);
        }
        NEXT();
      }
      case uint16_t(Op::F32Add):
        CHECK_NEXT(dispatchBinary1(AddF32, ValType::F32))
      case uint16_t(Op::F32Sub):
        CHECK_NEXT(dispatchBinary1(SubF32, ValType::F32));
      case uint16_t(Op::F32Mul):
        CHECK_NEXT(dispatchBinary1(MulF32, ValType::F32));
      case uint16_t(Op::F32Div):
        CHECK_NEXT(dispatchBinary1(DivF32, ValType::F32));
      case uint16_t(Op::F32Min):
        CHECK_NEXT(dispatchBinary1(MinF32, ValType::F32));
      case uint16_t(Op::F32Max):
        CHECK_NEXT(dispatchBinary1(MaxF32, ValType::F32));
      case uint16_t(Op::F32Neg):
        CHECK_NEXT(dispatchUnary1(NegateF32, ValType::F32));
      case uint16_t(Op::F32Abs):
        CHECK_NEXT(dispatchUnary1(AbsF32, ValType::F32));
      case uint16_t(Op::F32Sqrt):
        CHECK_NEXT(dispatchUnary1(SqrtF32, ValType::F32));
      case uint16_t(Op::F32Ceil):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::CeilF, ValType::F32));
      case uint16_t(Op::F32Floor):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::FloorF, ValType::F32));
      case uint16_t(Op::F32DemoteF64):
        CHECK_NEXT(
            dispatchConversion1(ConvertF64ToF32, ValType::F64, ValType::F32));
      case uint16_t(Op::F32ConvertI32S):
        CHECK_NEXT(
            dispatchConversion1(ConvertI32ToF32, ValType::I32, ValType::F32));
      case uint16_t(Op::F32ConvertI32U):
        CHECK_NEXT(
            dispatchConversion1(ConvertU32ToF32, ValType::I32, ValType::F32));
      case uint16_t(Op::F32ConvertI64S):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertInt64ToFloatingCallout, SymbolicAddress::Int64ToFloat32,
            ValType::I64, ValType::F32));
#else
        CHECK_NEXT(
            dispatchConversion1(ConvertI64ToF32, ValType::I64, ValType::F32));
#endif
      case uint16_t(Op::F32ConvertI64U):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertInt64ToFloatingCallout, SymbolicAddress::Uint64ToFloat32,
            ValType::I64, ValType::F32));
#else
        CHECK_NEXT(dispatchConversion0(emitConvertU64ToF32, ValType::I64,
                                       ValType::F32));
#endif
      case uint16_t(Op::F32ReinterpretI32):
        CHECK_NEXT(dispatchConversion1(ReinterpretI32AsF32, ValType::I32,
                                       ValType::F32));
      case uint16_t(Op::F32Load):
        CHECK_NEXT(emitLoad(ValType::F32, Scalar::Float32));
      case uint16_t(Op::F32Store):
        CHECK_NEXT(emitStore(ValType::F32, Scalar::Float32));
      case uint16_t(Op::F32CopySign):
        CHECK_NEXT(dispatchBinary1(CopysignF32, ValType::F32));
      case uint16_t(Op::F32Nearest):
        CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::NearbyIntF,
                                            ValType::F32));
      case uint16_t(Op::F32Trunc):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::TruncF, ValType::F32));

      // F64
      case uint16_t(Op::F64Const): {
        double f64;
        CHECK(iter_.readF64Const(&f64));
        if (!deadCode_) {
          pushF64(f64);
        }
        NEXT();
      }
      case uint16_t(Op::F64Add):
        CHECK_NEXT(dispatchBinary1(AddF64, ValType::F64))
      case uint16_t(Op::F64Sub):
        CHECK_NEXT(dispatchBinary1(SubF64, ValType::F64));
      case uint16_t(Op::F64Mul):
        CHECK_NEXT(dispatchBinary1(MulF64, ValType::F64));
      case uint16_t(Op::F64Div):
        CHECK_NEXT(dispatchBinary1(DivF64, ValType::F64));
      case uint16_t(Op::F64Min):
        CHECK_NEXT(dispatchBinary1(MinF64, ValType::F64));
      case uint16_t(Op::F64Max):
        CHECK_NEXT(dispatchBinary1(MaxF64, ValType::F64));
      case uint16_t(Op::F64Neg):
        CHECK_NEXT(dispatchUnary1(NegateF64, ValType::F64));
      case uint16_t(Op::F64Abs):
        CHECK_NEXT(dispatchUnary1(AbsF64, ValType::F64));
      case uint16_t(Op::F64Sqrt):
        CHECK_NEXT(dispatchUnary1(SqrtF64, ValType::F64));
      case uint16_t(Op::F64Ceil):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::CeilD, ValType::F64));
      case uint16_t(Op::F64Floor):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::FloorD, ValType::F64));
      case uint16_t(Op::F64PromoteF32):
        CHECK_NEXT(
            dispatchConversion1(ConvertF32ToF64, ValType::F32, ValType::F64));
      case uint16_t(Op::F64ConvertI32S):
        CHECK_NEXT(
            dispatchConversion1(ConvertI32ToF64, ValType::I32, ValType::F64));
      case uint16_t(Op::F64ConvertI32U):
        CHECK_NEXT(
            dispatchConversion1(ConvertU32ToF64, ValType::I32, ValType::F64));
      case uint16_t(Op::F64ConvertI64S):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertInt64ToFloatingCallout, SymbolicAddress::Int64ToDouble,
            ValType::I64, ValType::F64));
#else
        CHECK_NEXT(
            dispatchConversion1(ConvertI64ToF64, ValType::I64, ValType::F64));
#endif
      case uint16_t(Op::F64ConvertI64U):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertInt64ToFloatingCallout, SymbolicAddress::Uint64ToDouble,
            ValType::I64, ValType::F64));
#else
        CHECK_NEXT(dispatchConversion0(emitConvertU64ToF64, ValType::I64,
                                       ValType::F64));
#endif
      case uint16_t(Op::F64Load):
        CHECK_NEXT(emitLoad(ValType::F64, Scalar::Float64));
      case uint16_t(Op::F64Store):
        CHECK_NEXT(emitStore(ValType::F64, Scalar::Float64));
      case uint16_t(Op::F64ReinterpretI64):
        CHECK_NEXT(dispatchConversion1(ReinterpretI64AsF64, ValType::I64,
                                       ValType::F64));
      case uint16_t(Op::F64CopySign):
        CHECK_NEXT(dispatchBinary1(CopysignF64, ValType::F64));
      case uint16_t(Op::F64Nearest):
        CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::NearbyIntD,
                                            ValType::F64));
      case uint16_t(Op::F64Trunc):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::TruncD, ValType::F64));

      // Comparisons
      case uint16_t(Op::I32Eq):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::Equal));
      case uint16_t(Op::I32Ne):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::NotEqual));
      case uint16_t(Op::I32LtS):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::LessThan));
      case uint16_t(Op::I32LeS):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::LessThanOrEqual));
      case uint16_t(Op::I32GtS):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::GreaterThan));
      case uint16_t(Op::I32GeS):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::GreaterThanOrEqual));
      case uint16_t(Op::I32LtU):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::Below));
      case uint16_t(Op::I32LeU):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::BelowOrEqual));
      case uint16_t(Op::I32GtU):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::Above));
      case uint16_t(Op::I32GeU):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::AboveOrEqual));
      case uint16_t(Op::I64Eq):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::Equal));
      case uint16_t(Op::I64Ne):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::NotEqual));
      case uint16_t(Op::I64LtS):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::LessThan));
      case uint16_t(Op::I64LeS):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::LessThanOrEqual));
      case uint16_t(Op::I64GtS):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::GreaterThan));
      case uint16_t(Op::I64GeS):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::GreaterThanOrEqual));
      case uint16_t(Op::I64LtU):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::Below));
      case uint16_t(Op::I64LeU):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::BelowOrEqual));
      case uint16_t(Op::I64GtU):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::Above));
      case uint16_t(Op::I64GeU):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::AboveOrEqual));
      case uint16_t(Op::F32Eq):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleEqual));
      case uint16_t(Op::F32Ne):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleNotEqualOrUnordered));
      case uint16_t(Op::F32Lt):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleLessThan));
      case uint16_t(Op::F32Le):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleLessThanOrEqual));
      case uint16_t(Op::F32Gt):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleGreaterThan));
      case uint16_t(Op::F32Ge):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleGreaterThanOrEqual));
      case uint16_t(Op::F64Eq):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleEqual));
      case uint16_t(Op::F64Ne):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleNotEqualOrUnordered));
      case uint16_t(Op::F64Lt):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleLessThan));
      case uint16_t(Op::F64Le):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleLessThanOrEqual));
      case uint16_t(Op::F64Gt):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleGreaterThan));
      case uint16_t(Op::F64Ge):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleGreaterThanOrEqual));

      // Sign extensions
      case uint16_t(Op::I32Extend8S):
        CHECK_NEXT(
            dispatchConversion1(ExtendI32_8, ValType::I32, ValType::I32));
      case uint16_t(Op::I32Extend16S):
        CHECK_NEXT(
            dispatchConversion1(ExtendI32_16, ValType::I32, ValType::I32));
      case uint16_t(Op::I64Extend8S):
        CHECK_NEXT(
            dispatchConversion0(emitExtendI64_8, ValType::I64, ValType::I64));
      case uint16_t(Op::I64Extend16S):
        CHECK_NEXT(
            dispatchConversion0(emitExtendI64_16, ValType::I64, ValType::I64));
      case uint16_t(Op::I64Extend32S):
        CHECK_NEXT(
            dispatchConversion0(emitExtendI64_32, ValType::I64, ValType::I64));

      // Memory Related
      case uint16_t(Op::MemoryGrow):
        CHECK_NEXT(emitMemoryGrow());
      case uint16_t(Op::MemorySize):
        CHECK_NEXT(emitMemorySize());

#ifdef ENABLE_WASM_GC
      case uint16_t(Op::RefAsNonNull):
        if (!moduleEnv_.gcEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitRefAsNonNull());
      case uint16_t(Op::BrOnNull):
        if (!moduleEnv_.gcEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitBrOnNull());
      case uint16_t(Op::BrOnNonNull):
        if (!moduleEnv_.gcEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitBrOnNonNull());
#endif
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::RefEq):
        if (!moduleEnv_.gcEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(dispatchComparison0(emitCompareRef, RefType::eq(),
                                       Assembler::Equal));
#endif
      case uint16_t(Op::RefFunc):
        CHECK_NEXT(emitRefFunc());
        break;
      case uint16_t(Op::RefNull):
        CHECK_NEXT(emitRefNull());
        break;
      case uint16_t(Op::RefIsNull):
        CHECK_NEXT(emitRefIsNull());
        break;

#ifdef ENABLE_WASM_GC
      // "GC" operations
      case uint16_t(Op::GcPrefix): {
        if (!moduleEnv_.gcEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(GcOp::StructNew):
            CHECK_NEXT(emitStructNew());
          case uint32_t(GcOp::StructNewDefault):
            CHECK_NEXT(emitStructNewDefault());
          case uint32_t(GcOp::StructGet):
            CHECK_NEXT(emitStructGet(FieldWideningOp::None));
          case uint32_t(GcOp::StructGetS):
            CHECK_NEXT(emitStructGet(FieldWideningOp::Signed));
          case uint32_t(GcOp::StructGetU):
            CHECK_NEXT(emitStructGet(FieldWideningOp::Unsigned));
          case uint32_t(GcOp::StructSet):
            CHECK_NEXT(emitStructSet());
          case uint32_t(GcOp::ArrayNew):
            CHECK_NEXT(emitArrayNew());
          case uint32_t(GcOp::ArrayNewFixed):
            CHECK_NEXT(emitArrayNewFixed());
          case uint32_t(GcOp::ArrayNewDefault):
            CHECK_NEXT(emitArrayNewDefault());
          case uint32_t(GcOp::ArrayNewData):
            CHECK_NEXT(emitArrayNewData());
          case uint32_t(GcOp::ArrayNewElem):
            CHECK_NEXT(emitArrayNewElem());
          case uint32_t(GcOp::ArrayInitData):
            CHECK_NEXT(emitArrayInitData());
          case uint32_t(GcOp::ArrayInitElem):
            CHECK_NEXT(emitArrayInitElem());
          case uint32_t(GcOp::ArrayGet):
            CHECK_NEXT(emitArrayGet(FieldWideningOp::None));
          case uint32_t(GcOp::ArrayGetS):
            CHECK_NEXT(emitArrayGet(FieldWideningOp::Signed));
          case uint32_t(GcOp::ArrayGetU):
            CHECK_NEXT(emitArrayGet(FieldWideningOp::Unsigned));
          case uint32_t(GcOp::ArraySet):
            CHECK_NEXT(emitArraySet());
          case uint32_t(GcOp::ArrayLen):
            CHECK_NEXT(emitArrayLen());
          case uint32_t(GcOp::ArrayCopy):
            CHECK_NEXT(emitArrayCopy());
          case uint32_t(GcOp::ArrayFill):
            CHECK_NEXT(emitArrayFill());
          case uint32_t(GcOp::RefI31):
            CHECK_NEXT(emitRefI31());
          case uint32_t(GcOp::I31GetS):
            CHECK_NEXT(emitI31Get(FieldWideningOp::Signed));
          case uint32_t(GcOp::I31GetU):
            CHECK_NEXT(emitI31Get(FieldWideningOp::Unsigned));
          case uint32_t(GcOp::RefTest):
            CHECK_NEXT(emitRefTest(/*nullable=*/false));
          case uint32_t(GcOp::RefTestNull):
            CHECK_NEXT(emitRefTest(/*nullable=*/true));
          case uint32_t(GcOp::RefCast):
            CHECK_NEXT(emitRefCast(/*nullable=*/false));
          case uint32_t(GcOp::RefCastNull):
            CHECK_NEXT(emitRefCast(/*nullable=*/true));
          case uint32_t(GcOp::BrOnCast):
            CHECK_NEXT(emitBrOnCast(/*onSuccess=*/true));
          case uint32_t(GcOp::BrOnCastFail):
            CHECK_NEXT(emitBrOnCast(/*onSuccess=*/false));
          case uint16_t(GcOp::AnyConvertExtern):
            CHECK_NEXT(emitAnyConvertExtern());
          case uint16_t(GcOp::ExternConvertAny):
            CHECK_NEXT(emitExternConvertAny());
          default:
            break;
        }  // switch (op.b1)
        return iter_.unrecognizedOpcode(&op);
      }
#endif

#ifdef ENABLE_WASM_SIMD
      // SIMD operations
      case uint16_t(Op::SimdPrefix): {
        uint32_t laneIndex;
        if (!moduleEnv_.simdAvailable()) {
          return iter_.unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(SimdOp::I8x16ExtractLaneS):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneI8x16, ValType::I32, 16));
          case uint32_t(SimdOp::I8x16ExtractLaneU):
            CHECK_NEXT(
                dispatchExtractLane(ExtractLaneUI8x16, ValType::I32, 16));
          case uint32_t(SimdOp::I16x8ExtractLaneS):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneI16x8, ValType::I32, 8));
          case uint32_t(SimdOp::I16x8ExtractLaneU):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneUI16x8, ValType::I32, 8));
          case uint32_t(SimdOp::I32x4ExtractLane):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneI32x4, ValType::I32, 4));
          case uint32_t(SimdOp::I64x2ExtractLane):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneI64x2, ValType::I64, 2));
          case uint32_t(SimdOp::F32x4ExtractLane):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneF32x4, ValType::F32, 4));
          case uint32_t(SimdOp::F64x2ExtractLane):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneF64x2, ValType::F64, 2));
          case uint32_t(SimdOp::I8x16Splat):
            CHECK_NEXT(dispatchSplat(SplatI8x16, ValType::I32));
          case uint32_t(SimdOp::I16x8Splat):
            CHECK_NEXT(dispatchSplat(SplatI16x8, ValType::I32));
          case uint32_t(SimdOp::I32x4Splat):
            CHECK_NEXT(dispatchSplat(SplatI32x4, ValType::I32));
          case uint32_t(SimdOp::I64x2Splat):
            CHECK_NEXT(dispatchSplat(SplatI64x2, ValType::I64));
          case uint32_t(SimdOp::F32x4Splat):
            CHECK_NEXT(dispatchSplat(SplatF32x4, ValType::F32));
          case uint32_t(SimdOp::F64x2Splat):
            CHECK_NEXT(dispatchSplat(SplatF64x2, ValType::F64));
          case uint32_t(SimdOp::V128AnyTrue):
            CHECK_NEXT(dispatchVectorReduction(AnyTrue));
          case uint32_t(SimdOp::I8x16AllTrue):
            CHECK_NEXT(dispatchVectorReduction(AllTrueI8x16));
          case uint32_t(SimdOp::I16x8AllTrue):
            CHECK_NEXT(dispatchVectorReduction(AllTrueI16x8));
          case uint32_t(SimdOp::I32x4AllTrue):
            CHECK_NEXT(dispatchVectorReduction(AllTrueI32x4));
          case uint32_t(SimdOp::I64x2AllTrue):
            CHECK_NEXT(dispatchVectorReduction(AllTrueI64x2));
          case uint32_t(SimdOp::I8x16Bitmask):
            CHECK_NEXT(dispatchVectorReduction(BitmaskI8x16));
          case uint32_t(SimdOp::I16x8Bitmask):
            CHECK_NEXT(dispatchVectorReduction(BitmaskI16x8));
          case uint32_t(SimdOp::I32x4Bitmask):
            CHECK_NEXT(dispatchVectorReduction(BitmaskI32x4));
          case uint32_t(SimdOp::I64x2Bitmask):
            CHECK_NEXT(dispatchVectorReduction(BitmaskI64x2));
          case uint32_t(SimdOp::I8x16ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneI8x16, ValType::I32, 16));
          case uint32_t(SimdOp::I16x8ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneI16x8, ValType::I32, 8));
          case uint32_t(SimdOp::I32x4ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneI32x4, ValType::I32, 4));
          case uint32_t(SimdOp::I64x2ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneI64x2, ValType::I64, 2));
          case uint32_t(SimdOp::F32x4ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneF32x4, ValType::F32, 4));
          case uint32_t(SimdOp::F64x2ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneF64x2, ValType::F64, 2));
          case uint32_t(SimdOp::I8x16Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpI8x16, Assembler::Equal));
          case uint32_t(SimdOp::I8x16Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpI8x16, Assembler::NotEqual));
          case uint32_t(SimdOp::I8x16LtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI8x16, Assembler::LessThan));
          case uint32_t(SimdOp::I8x16LtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI8x16, Assembler::Below));
          case uint32_t(SimdOp::I8x16GtS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI8x16, Assembler::GreaterThan));
          case uint32_t(SimdOp::I8x16GtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI8x16, Assembler::Above));
          case uint32_t(SimdOp::I8x16LeS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI8x16, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::I8x16LeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI8x16, Assembler::BelowOrEqual));
          case uint32_t(SimdOp::I8x16GeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI8x16,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::I8x16GeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI8x16, Assembler::AboveOrEqual));
          case uint32_t(SimdOp::I16x8Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpI16x8, Assembler::Equal));
          case uint32_t(SimdOp::I16x8Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpI16x8, Assembler::NotEqual));
          case uint32_t(SimdOp::I16x8LtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI16x8, Assembler::LessThan));
          case uint32_t(SimdOp::I16x8LtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI16x8, Assembler::Below));
          case uint32_t(SimdOp::I16x8GtS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI16x8, Assembler::GreaterThan));
          case uint32_t(SimdOp::I16x8GtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI16x8, Assembler::Above));
          case uint32_t(SimdOp::I16x8LeS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI16x8, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::I16x8LeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI16x8, Assembler::BelowOrEqual));
          case uint32_t(SimdOp::I16x8GeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI16x8,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::I16x8GeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI16x8, Assembler::AboveOrEqual));
          case uint32_t(SimdOp::I32x4Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpI32x4, Assembler::Equal));
          case uint32_t(SimdOp::I32x4Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpI32x4, Assembler::NotEqual));
          case uint32_t(SimdOp::I32x4LtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI32x4, Assembler::LessThan));
          case uint32_t(SimdOp::I32x4LtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI32x4, Assembler::Below));
          case uint32_t(SimdOp::I32x4GtS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI32x4, Assembler::GreaterThan));
          case uint32_t(SimdOp::I32x4GtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI32x4, Assembler::Above));
          case uint32_t(SimdOp::I32x4LeS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI32x4, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::I32x4LeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI32x4, Assembler::BelowOrEqual));
          case uint32_t(SimdOp::I32x4GeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI32x4,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::I32x4GeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI32x4, Assembler::AboveOrEqual));
          case uint32_t(SimdOp::I64x2Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForEquality,
                                                Assembler::Equal));
          case uint32_t(SimdOp::I64x2Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForEquality,
                                                Assembler::NotEqual));
          case uint32_t(SimdOp::I64x2LtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForOrdering,
                                                Assembler::LessThan));
          case uint32_t(SimdOp::I64x2GtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForOrdering,
                                                Assembler::GreaterThan));
          case uint32_t(SimdOp::I64x2LeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForOrdering,
                                                Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::I64x2GeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForOrdering,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::F32x4Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpF32x4, Assembler::Equal));
          case uint32_t(SimdOp::F32x4Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpF32x4, Assembler::NotEqual));
          case uint32_t(SimdOp::F32x4Lt):
            CHECK_NEXT(dispatchVectorComparison(CmpF32x4, Assembler::LessThan));
          case uint32_t(SimdOp::F32x4Gt):
            CHECK_NEXT(
                dispatchVectorComparison(CmpF32x4, Assembler::GreaterThan));
          case uint32_t(SimdOp::F32x4Le):
            CHECK_NEXT(
                dispatchVectorComparison(CmpF32x4, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::F32x4Ge):
            CHECK_NEXT(dispatchVectorComparison(CmpF32x4,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::F64x2Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpF64x2, Assembler::Equal));
          case uint32_t(SimdOp::F64x2Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpF64x2, Assembler::NotEqual));
          case uint32_t(SimdOp::F64x2Lt):
            CHECK_NEXT(dispatchVectorComparison(CmpF64x2, Assembler::LessThan));
          case uint32_t(SimdOp::F64x2Gt):
            CHECK_NEXT(
                dispatchVectorComparison(CmpF64x2, Assembler::GreaterThan));
          case uint32_t(SimdOp::F64x2Le):
            CHECK_NEXT(
                dispatchVectorComparison(CmpF64x2, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::F64x2Ge):
            CHECK_NEXT(dispatchVectorComparison(CmpF64x2,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::V128And):
            CHECK_NEXT(dispatchVectorBinary(AndV128));
          case uint32_t(SimdOp::V128Or):
            CHECK_NEXT(dispatchVectorBinary(OrV128));
          case uint32_t(SimdOp::V128Xor):
            CHECK_NEXT(dispatchVectorBinary(XorV128));
          case uint32_t(SimdOp::V128AndNot):
            CHECK_NEXT(dispatchBinary0(emitVectorAndNot, ValType::V128));
          case uint32_t(SimdOp::I8x16AvgrU):
            CHECK_NEXT(dispatchVectorBinary(AverageUI8x16));
          case uint32_t(SimdOp::I16x8AvgrU):
            CHECK_NEXT(dispatchVectorBinary(AverageUI16x8));
          case uint32_t(SimdOp::I8x16Add):
            CHECK_NEXT(dispatchVectorBinary(AddI8x16));
          case uint32_t(SimdOp::I8x16AddSatS):
            CHECK_NEXT(dispatchVectorBinary(AddSatI8x16));
          case uint32_t(SimdOp::I8x16AddSatU):
            CHECK_NEXT(dispatchVectorBinary(AddSatUI8x16));
          case uint32_t(SimdOp::I8x16Sub):
            CHECK_NEXT(dispatchVectorBinary(SubI8x16));
          case uint32_t(SimdOp::I8x16SubSatS):
            CHECK_NEXT(dispatchVectorBinary(SubSatI8x16));
          case uint32_t(SimdOp::I8x16SubSatU):
            CHECK_NEXT(dispatchVectorBinary(SubSatUI8x16));
          case uint32_t(SimdOp::I8x16MinS):
            CHECK_NEXT(dispatchVectorBinary(MinI8x16));
          case uint32_t(SimdOp::I8x16MinU):
            CHECK_NEXT(dispatchVectorBinary(MinUI8x16));
          case uint32_t(SimdOp::I8x16MaxS):
            CHECK_NEXT(dispatchVectorBinary(MaxI8x16));
          case uint32_t(SimdOp::I8x16MaxU):
            CHECK_NEXT(dispatchVectorBinary(MaxUI8x16));
          case uint32_t(SimdOp::I16x8Add):
            CHECK_NEXT(dispatchVectorBinary(AddI16x8));
          case uint32_t(SimdOp::I16x8AddSatS):
            CHECK_NEXT(dispatchVectorBinary(AddSatI16x8));
          case uint32_t(SimdOp::I16x8AddSatU):
            CHECK_NEXT(dispatchVectorBinary(AddSatUI16x8));
          case uint32_t(SimdOp::I16x8Sub):
            CHECK_NEXT(dispatchVectorBinary(SubI16x8));
          case uint32_t(SimdOp::I16x8SubSatS):
            CHECK_NEXT(dispatchVectorBinary(SubSatI16x8));
          case uint32_t(SimdOp::I16x8SubSatU):
            CHECK_NEXT(dispatchVectorBinary(SubSatUI16x8));
          case uint32_t(SimdOp::I16x8Mul):
            CHECK_NEXT(dispatchVectorBinary(MulI16x8));
          case uint32_t(SimdOp::I16x8MinS):
            CHECK_NEXT(dispatchVectorBinary(MinI16x8));
          case uint32_t(SimdOp::I16x8MinU):
            CHECK_NEXT(dispatchVectorBinary(MinUI16x8));
          case uint32_t(SimdOp::I16x8MaxS):
            CHECK_NEXT(dispatchVectorBinary(MaxI16x8));
          case uint32_t(SimdOp::I16x8MaxU):
            CHECK_NEXT(dispatchVectorBinary(MaxUI16x8));
          case uint32_t(SimdOp::I32x4Add):
            CHECK_NEXT(dispatchVectorBinary(AddI32x4));
          case uint32_t(SimdOp::I32x4Sub):
            CHECK_NEXT(dispatchVectorBinary(SubI32x4));
          case uint32_t(SimdOp::I32x4Mul):
            CHECK_NEXT(dispatchVectorBinary(MulI32x4));
          case uint32_t(SimdOp::I32x4MinS):
            CHECK_NEXT(dispatchVectorBinary(MinI32x4));
          case uint32_t(SimdOp::I32x4MinU):
            CHECK_NEXT(dispatchVectorBinary(MinUI32x4));
          case uint32_t(SimdOp::I32x4MaxS):
            CHECK_NEXT(dispatchVectorBinary(MaxI32x4));
          case uint32_t(SimdOp::I32x4MaxU):
            CHECK_NEXT(dispatchVectorBinary(MaxUI32x4));
          case uint32_t(SimdOp::I64x2Add):
            CHECK_NEXT(dispatchVectorBinary(AddI64x2));
          case uint32_t(SimdOp::I64x2Sub):
            CHECK_NEXT(dispatchVectorBinary(SubI64x2));
          case uint32_t(SimdOp::I64x2Mul):
            CHECK_NEXT(dispatchVectorBinary(MulI64x2));
          case uint32_t(SimdOp::F32x4Add):
            CHECK_NEXT(dispatchVectorBinary(AddF32x4));
          case uint32_t(SimdOp::F32x4Sub):
            CHECK_NEXT(dispatchVectorBinary(SubF32x4));
          case uint32_t(SimdOp::F32x4Mul):
            CHECK_NEXT(dispatchVectorBinary(MulF32x4));
          case uint32_t(SimdOp::F32x4Div):
            CHECK_NEXT(dispatchVectorBinary(DivF32x4));
          case uint32_t(SimdOp::F32x4Min):
            CHECK_NEXT(dispatchVectorBinary(MinF32x4));
          case uint32_t(SimdOp::F32x4Max):
            CHECK_NEXT(dispatchVectorBinary(MaxF32x4));
          case uint32_t(SimdOp::F64x2Add):
            CHECK_NEXT(dispatchVectorBinary(AddF64x2));
          case uint32_t(SimdOp::F64x2Sub):
            CHECK_NEXT(dispatchVectorBinary(SubF64x2));
          case uint32_t(SimdOp::F64x2Mul):
            CHECK_NEXT(dispatchVectorBinary(MulF64x2));
          case uint32_t(SimdOp::F64x2Div):
            CHECK_NEXT(dispatchVectorBinary(DivF64x2));
          case uint32_t(SimdOp::F64x2Min):
            CHECK_NEXT(dispatchVectorBinary(MinF64x2));
          case uint32_t(SimdOp::F64x2Max):
            CHECK_NEXT(dispatchVectorBinary(MaxF64x2));
          case uint32_t(SimdOp::I8x16NarrowI16x8S):
            CHECK_NEXT(dispatchVectorBinary(NarrowI16x8));
          case uint32_t(SimdOp::I8x16NarrowI16x8U):
            CHECK_NEXT(dispatchVectorBinary(NarrowUI16x8));
          case uint32_t(SimdOp::I16x8NarrowI32x4S):
            CHECK_NEXT(dispatchVectorBinary(NarrowI32x4));
          case uint32_t(SimdOp::I16x8NarrowI32x4U):
            CHECK_NEXT(dispatchVectorBinary(NarrowUI32x4));
          case uint32_t(SimdOp::I8x16Swizzle):
            CHECK_NEXT(dispatchVectorBinary(Swizzle));
          case uint32_t(SimdOp::F32x4PMax):
            CHECK_NEXT(dispatchVectorBinary(PMaxF32x4));
          case uint32_t(SimdOp::F32x4PMin):
            CHECK_NEXT(dispatchVectorBinary(PMinF32x4));
          case uint32_t(SimdOp::F64x2PMax):
            CHECK_NEXT(dispatchVectorBinary(PMaxF64x2));
          case uint32_t(SimdOp::F64x2PMin):
            CHECK_NEXT(dispatchVectorBinary(PMinF64x2));
          case uint32_t(SimdOp::I32x4DotI16x8S):
            CHECK_NEXT(dispatchVectorBinary(DotI16x8));
          case uint32_t(SimdOp::I16x8ExtmulLowI8x16S):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowI8x16));
          case uint32_t(SimdOp::I16x8ExtmulHighI8x16S):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighI8x16));
          case uint32_t(SimdOp::I16x8ExtmulLowI8x16U):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowUI8x16));
          case uint32_t(SimdOp::I16x8ExtmulHighI8x16U):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighUI8x16));
          case uint32_t(SimdOp::I32x4ExtmulLowI16x8S):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowI16x8));
          case uint32_t(SimdOp::I32x4ExtmulHighI16x8S):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighI16x8));
          case uint32_t(SimdOp::I32x4ExtmulLowI16x8U):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowUI16x8));
          case uint32_t(SimdOp::I32x4ExtmulHighI16x8U):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighUI16x8));
          case uint32_t(SimdOp::I64x2ExtmulLowI32x4S):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowI32x4));
          case uint32_t(SimdOp::I64x2ExtmulHighI32x4S):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighI32x4));
          case uint32_t(SimdOp::I64x2ExtmulLowI32x4U):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowUI32x4));
          case uint32_t(SimdOp::I64x2ExtmulHighI32x4U):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighUI32x4));
          case uint32_t(SimdOp::I16x8Q15MulrSatS):
            CHECK_NEXT(dispatchVectorBinary(Q15MulrSatS));
          case uint32_t(SimdOp::I8x16Neg):
            CHECK_NEXT(dispatchVectorUnary(NegI8x16));
          case uint32_t(SimdOp::I16x8Neg):
            CHECK_NEXT(dispatchVectorUnary(NegI16x8));
          case uint32_t(SimdOp::I16x8ExtendLowI8x16S):
            CHECK_NEXT(dispatchVectorUnary(WidenLowI8x16));
          case uint32_t(SimdOp::I16x8ExtendHighI8x16S):
            CHECK_NEXT(dispatchVectorUnary(WidenHighI8x16));
          case uint32_t(SimdOp::I16x8ExtendLowI8x16U):
            CHECK_NEXT(dispatchVectorUnary(WidenLowUI8x16));
          case uint32_t(SimdOp::I16x8ExtendHighI8x16U):
            CHECK_NEXT(dispatchVectorUnary(WidenHighUI8x16));
          case uint32_t(SimdOp::I32x4Neg):
            CHECK_NEXT(dispatchVectorUnary(NegI32x4));
          case uint32_t(SimdOp::I32x4ExtendLowI16x8S):
            CHECK_NEXT(dispatchVectorUnary(WidenLowI16x8));
          case uint32_t(SimdOp::I32x4ExtendHighI16x8S):
            CHECK_NEXT(dispatchVectorUnary(WidenHighI16x8));
          case uint32_t(SimdOp::I32x4ExtendLowI16x8U):
            CHECK_NEXT(dispatchVectorUnary(WidenLowUI16x8));
          case uint32_t(SimdOp::I32x4ExtendHighI16x8U):
            CHECK_NEXT(dispatchVectorUnary(WidenHighUI16x8));
          case uint32_t(SimdOp::I32x4TruncSatF32x4S):
            CHECK_NEXT(dispatchVectorUnary(ConvertF32x4ToI32x4));
          case uint32_t(SimdOp::I32x4TruncSatF32x4U):
            CHECK_NEXT(dispatchVectorUnary(ConvertF32x4ToUI32x4));
          case uint32_t(SimdOp::I64x2Neg):
            CHECK_NEXT(dispatchVectorUnary(NegI64x2));
          case uint32_t(SimdOp::I64x2ExtendLowI32x4S):
            CHECK_NEXT(dispatchVectorUnary(WidenLowI32x4));
          case uint32_t(SimdOp::I64x2ExtendHighI32x4S):
            CHECK_NEXT(dispatchVectorUnary(WidenHighI32x4));
          case uint32_t(SimdOp::I64x2ExtendLowI32x4U):
            CHECK_NEXT(dispatchVectorUnary(WidenLowUI32x4));
          case uint32_t(SimdOp::I64x2ExtendHighI32x4U):
            CHECK_NEXT(dispatchVectorUnary(WidenHighUI32x4));
          case uint32_t(SimdOp::F32x4Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsF32x4));
          case uint32_t(SimdOp::F32x4Neg):
            CHECK_NEXT(dispatchVectorUnary(NegF32x4));
          case uint32_t(SimdOp::F32x4Sqrt):
            CHECK_NEXT(dispatchVectorUnary(SqrtF32x4));
          case uint32_t(SimdOp::F32x4ConvertI32x4S):
            CHECK_NEXT(dispatchVectorUnary(ConvertI32x4ToF32x4));
          case uint32_t(SimdOp::F32x4ConvertI32x4U):
            CHECK_NEXT(dispatchVectorUnary(ConvertUI32x4ToF32x4));
          case uint32_t(SimdOp::F32x4DemoteF64x2Zero):
            CHECK_NEXT(dispatchVectorUnary(DemoteF64x2ToF32x4));
          case uint32_t(SimdOp::F64x2PromoteLowF32x4):
            CHECK_NEXT(dispatchVectorUnary(PromoteF32x4ToF64x2));
          case uint32_t(SimdOp::F64x2ConvertLowI32x4S):
            CHECK_NEXT(dispatchVectorUnary(ConvertI32x4ToF64x2));
          case uint32_t(SimdOp::F64x2ConvertLowI32x4U):
            CHECK_NEXT(dispatchVectorUnary(ConvertUI32x4ToF64x2));
          case uint32_t(SimdOp::I32x4TruncSatF64x2SZero):
            CHECK_NEXT(dispatchVectorUnary(ConvertF64x2ToI32x4));
          case uint32_t(SimdOp::I32x4TruncSatF64x2UZero):
            CHECK_NEXT(dispatchVectorUnary(ConvertF64x2ToUI32x4));
          case uint32_t(SimdOp::F64x2Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsF64x2));
          case uint32_t(SimdOp::F64x2Neg):
            CHECK_NEXT(dispatchVectorUnary(NegF64x2));
          case uint32_t(SimdOp::F64x2Sqrt):
            CHECK_NEXT(dispatchVectorUnary(SqrtF64x2));
          case uint32_t(SimdOp::V128Not):
            CHECK_NEXT(dispatchVectorUnary(NotV128));
          case uint32_t(SimdOp::I8x16Popcnt):
            CHECK_NEXT(dispatchVectorUnary(PopcntI8x16));
          case uint32_t(SimdOp::I8x16Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsI8x16));
          case uint32_t(SimdOp::I16x8Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsI16x8));
          case uint32_t(SimdOp::I32x4Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsI32x4));
          case uint32_t(SimdOp::I64x2Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsI64x2));
          case uint32_t(SimdOp::F32x4Ceil):
            CHECK_NEXT(dispatchVectorUnary(CeilF32x4));
          case uint32_t(SimdOp::F32x4Floor):
            CHECK_NEXT(dispatchVectorUnary(FloorF32x4));
          case uint32_t(SimdOp::F32x4Trunc):
            CHECK_NEXT(dispatchVectorUnary(TruncF32x4));
          case uint32_t(SimdOp::F32x4Nearest):
            CHECK_NEXT(dispatchVectorUnary(NearestF32x4));
          case uint32_t(SimdOp::F64x2Ceil):
            CHECK_NEXT(dispatchVectorUnary(CeilF64x2));
          case uint32_t(SimdOp::F64x2Floor):
            CHECK_NEXT(dispatchVectorUnary(FloorF64x2));
          case uint32_t(SimdOp::F64x2Trunc):
            CHECK_NEXT(dispatchVectorUnary(TruncF64x2));
          case uint32_t(SimdOp::F64x2Nearest):
            CHECK_NEXT(dispatchVectorUnary(NearestF64x2));
          case uint32_t(SimdOp::I16x8ExtaddPairwiseI8x16S):
            CHECK_NEXT(dispatchVectorUnary(ExtAddPairwiseI8x16));
          case uint32_t(SimdOp::I16x8ExtaddPairwiseI8x16U):
            CHECK_NEXT(dispatchVectorUnary(ExtAddPairwiseUI8x16));
          case uint32_t(SimdOp::I32x4ExtaddPairwiseI16x8S):
            CHECK_NEXT(dispatchVectorUnary(ExtAddPairwiseI16x8));
          case uint32_t(SimdOp::I32x4ExtaddPairwiseI16x8U):
            CHECK_NEXT(dispatchVectorUnary(ExtAddPairwiseUI16x8));
          case uint32_t(SimdOp::I8x16Shl):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftLeftI8x16));
          case uint32_t(SimdOp::I8x16ShrS):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightI8x16));
          case uint32_t(SimdOp::I8x16ShrU):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightUI8x16));
          case uint32_t(SimdOp::I16x8Shl):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftLeftI16x8));
          case uint32_t(SimdOp::I16x8ShrS):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightI16x8));
          case uint32_t(SimdOp::I16x8ShrU):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightUI16x8));
          case uint32_t(SimdOp::I32x4Shl):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftLeftI32x4));
          case uint32_t(SimdOp::I32x4ShrS):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightI32x4));
          case uint32_t(SimdOp::I32x4ShrU):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightUI32x4));
          case uint32_t(SimdOp::I64x2Shl):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftLeftI64x2));
          case uint32_t(SimdOp::I64x2ShrS):
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
            CHECK_NEXT(emitVectorShiftRightI64x2());
#  else
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightI64x2));
#  endif
          case uint32_t(SimdOp::I64x2ShrU):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightUI64x2));
          case uint32_t(SimdOp::V128Bitselect):
            CHECK_NEXT(dispatchTernary1(BitselectV128, ValType::V128));
          case uint32_t(SimdOp::I8x16Shuffle):
            CHECK_NEXT(emitVectorShuffle());
          case uint32_t(SimdOp::V128Const): {
            V128 v128;
            CHECK(iter_.readV128Const(&v128));
            if (!deadCode_) {
              pushV128(v128);
            }
            NEXT();
          }
          case uint32_t(SimdOp::V128Load):
            CHECK_NEXT(emitLoad(ValType::V128, Scalar::Simd128));
          case uint32_t(SimdOp::V128Load8Splat):
            CHECK_NEXT(emitLoadSplat(Scalar::Uint8));
          case uint32_t(SimdOp::V128Load16Splat):
            CHECK_NEXT(emitLoadSplat(Scalar::Uint16));
          case uint32_t(SimdOp::V128Load32Splat):
            CHECK_NEXT(emitLoadSplat(Scalar::Uint32));
          case uint32_t(SimdOp::V128Load64Splat):
            CHECK_NEXT(emitLoadSplat(Scalar::Int64));
          case uint32_t(SimdOp::V128Load8x8S):
            CHECK_NEXT(emitLoadExtend(Scalar::Int8));
          case uint32_t(SimdOp::V128Load8x8U):
            CHECK_NEXT(emitLoadExtend(Scalar::Uint8));
          case uint32_t(SimdOp::V128Load16x4S):
            CHECK_NEXT(emitLoadExtend(Scalar::Int16));
          case uint32_t(SimdOp::V128Load16x4U):
            CHECK_NEXT(emitLoadExtend(Scalar::Uint16));
          case uint32_t(SimdOp::V128Load32x2S):
            CHECK_NEXT(emitLoadExtend(Scalar::Int32));
          case uint32_t(SimdOp::V128Load32x2U):
            CHECK_NEXT(emitLoadExtend(Scalar::Uint32));
          case uint32_t(SimdOp::V128Load32Zero):
            CHECK_NEXT(emitLoadZero(Scalar::Float32));
          case uint32_t(SimdOp::V128Load64Zero):
            CHECK_NEXT(emitLoadZero(Scalar::Float64));
          case uint32_t(SimdOp::V128Store):
            CHECK_NEXT(emitStore(ValType::V128, Scalar::Simd128));
          case uint32_t(SimdOp::V128Load8Lane):
            CHECK_NEXT(emitLoadLane(1));
          case uint32_t(SimdOp::V128Load16Lane):
            CHECK_NEXT(emitLoadLane(2));
          case uint32_t(SimdOp::V128Load32Lane):
            CHECK_NEXT(emitLoadLane(4));
          case uint32_t(SimdOp::V128Load64Lane):
            CHECK_NEXT(emitLoadLane(8));
          case uint32_t(SimdOp::V128Store8Lane):
            CHECK_NEXT(emitStoreLane(1));
          case uint32_t(SimdOp::V128Store16Lane):
            CHECK_NEXT(emitStoreLane(2));
          case uint32_t(SimdOp::V128Store32Lane):
            CHECK_NEXT(emitStoreLane(4));
          case uint32_t(SimdOp::V128Store64Lane):
            CHECK_NEXT(emitStoreLane(8));
#  ifdef ENABLE_WASM_RELAXED_SIMD
          case uint32_t(SimdOp::F32x4RelaxedMadd):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchTernary2(RelaxedMaddF32x4, ValType::V128));
          case uint32_t(SimdOp::F32x4RelaxedNmadd):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchTernary2(RelaxedNmaddF32x4, ValType::V128));
          case uint32_t(SimdOp::F64x2RelaxedMadd):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchTernary2(RelaxedMaddF64x2, ValType::V128));
          case uint32_t(SimdOp::F64x2RelaxedNmadd):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchTernary2(RelaxedNmaddF64x2, ValType::V128));
            break;
          case uint32_t(SimdOp::I8x16RelaxedLaneSelect):
          case uint32_t(SimdOp::I16x8RelaxedLaneSelect):
          case uint32_t(SimdOp::I32x4RelaxedLaneSelect):
          case uint32_t(SimdOp::I64x2RelaxedLaneSelect):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(emitVectorLaneSelect());
          case uint32_t(SimdOp::F32x4RelaxedMin):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorBinary(RelaxedMinF32x4));
          case uint32_t(SimdOp::F32x4RelaxedMax):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorBinary(RelaxedMaxF32x4));
          case uint32_t(SimdOp::F64x2RelaxedMin):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorBinary(RelaxedMinF64x2));
          case uint32_t(SimdOp::F64x2RelaxedMax):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorBinary(RelaxedMaxF64x2));
          case uint32_t(SimdOp::I32x4RelaxedTruncF32x4S):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorUnary(RelaxedConvertF32x4ToI32x4));
          case uint32_t(SimdOp::I32x4RelaxedTruncF32x4U):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorUnary(RelaxedConvertF32x4ToUI32x4));
          case uint32_t(SimdOp::I32x4RelaxedTruncF64x2SZero):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorUnary(RelaxedConvertF64x2ToI32x4));
          case uint32_t(SimdOp::I32x4RelaxedTruncF64x2UZero):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorUnary(RelaxedConvertF64x2ToUI32x4));
          case uint32_t(SimdOp::I8x16RelaxedSwizzle):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorBinary(RelaxedSwizzle));
          case uint32_t(SimdOp::I16x8RelaxedQ15MulrS):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorBinary(RelaxedQ15MulrS));
          case uint32_t(SimdOp::I16x8DotI8x16I7x16S):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchVectorBinary(DotI8x16I7x16S));
          case uint32_t(SimdOp::I32x4DotI8x16I7x16AddS):
            if (!moduleEnv_.v128RelaxedEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(dispatchTernary0(emitDotI8x16I7x16AddS, ValType::V128));
#  endif
          default:
            break;
        }  // switch (op.b1)
        return iter_.unrecognizedOpcode(&op);
      }
#endif  // ENABLE_WASM_SIMD

      // "Miscellaneous" operations
      case uint16_t(Op::MiscPrefix): {
        switch (op.b1) {
          case uint32_t(MiscOp::I32TruncSatF32S):
            CHECK_NEXT(
                dispatchConversionOOM(emitTruncateF32ToI32<TRUNC_SATURATING>,
                                      ValType::F32, ValType::I32));
          case uint32_t(MiscOp::I32TruncSatF32U):
            CHECK_NEXT(dispatchConversionOOM(
                emitTruncateF32ToI32<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                ValType::F32, ValType::I32));
          case uint32_t(MiscOp::I32TruncSatF64S):
            CHECK_NEXT(
                dispatchConversionOOM(emitTruncateF64ToI32<TRUNC_SATURATING>,
                                      ValType::F64, ValType::I32));
          case uint32_t(MiscOp::I32TruncSatF64U):
            CHECK_NEXT(dispatchConversionOOM(
                emitTruncateF64ToI32<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                ValType::F64, ValType::I32));
          case uint32_t(MiscOp::I64TruncSatF32S):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(dispatchCalloutConversionOOM(
                emitConvertFloatingToInt64Callout,
                SymbolicAddress::SaturatingTruncateDoubleToInt64, ValType::F32,
                ValType::I64));
#else
            CHECK_NEXT(
                dispatchConversionOOM(emitTruncateF32ToI64<TRUNC_SATURATING>,
                                      ValType::F32, ValType::I64));
#endif
          case uint32_t(MiscOp::I64TruncSatF32U):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(dispatchCalloutConversionOOM(
                emitConvertFloatingToInt64Callout,
                SymbolicAddress::SaturatingTruncateDoubleToUint64, ValType::F32,
                ValType::I64));
#else
            CHECK_NEXT(dispatchConversionOOM(
                emitTruncateF32ToI64<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                ValType::F32, ValType::I64));
#endif
          case uint32_t(MiscOp::I64TruncSatF64S):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(dispatchCalloutConversionOOM(
                emitConvertFloatingToInt64Callout,
                SymbolicAddress::SaturatingTruncateDoubleToInt64, ValType::F64,
                ValType::I64));
#else
            CHECK_NEXT(
                dispatchConversionOOM(emitTruncateF64ToI64<TRUNC_SATURATING>,
                                      ValType::F64, ValType::I64));
#endif
          case uint32_t(MiscOp::I64TruncSatF64U):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(dispatchCalloutConversionOOM(
                emitConvertFloatingToInt64Callout,
                SymbolicAddress::SaturatingTruncateDoubleToUint64, ValType::F64,
                ValType::I64));
#else
            CHECK_NEXT(dispatchConversionOOM(
                emitTruncateF64ToI64<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                ValType::F64, ValType::I64));
#endif
          case uint32_t(MiscOp::MemoryCopy):
            CHECK_NEXT(emitMemCopy());
          case uint32_t(MiscOp::DataDrop):
            CHECK_NEXT(emitDataOrElemDrop(/*isData=*/true));
          case uint32_t(MiscOp::MemoryFill):
            CHECK_NEXT(emitMemFill());
#ifdef ENABLE_WASM_MEMORY_CONTROL
          case uint32_t(MiscOp::MemoryDiscard): {
            if (!moduleEnv_.memoryControlEnabled()) {
              return iter_.unrecognizedOpcode(&op);
            }
            CHECK_NEXT(emitMemDiscard());
          }
#endif
          case uint32_t(MiscOp::MemoryInit):
            CHECK_NEXT(emitMemInit());
          case uint32_t(MiscOp::TableCopy):
            CHECK_NEXT(emitTableCopy());
          case uint32_t(MiscOp::ElemDrop):
            CHECK_NEXT(emitDataOrElemDrop(/*isData=*/false));
          case uint32_t(MiscOp::TableInit):
            CHECK_NEXT(emitTableInit());
          case uint32_t(MiscOp::TableFill):
            CHECK_NEXT(emitTableFill());
          case uint32_t(MiscOp::TableGrow):
            CHECK_NEXT(emitTableGrow());
          case uint32_t(MiscOp::TableSize):
            CHECK_NEXT(emitTableSize());
          default:
            break;
        }  // switch (op.b1)
        return iter_.unrecognizedOpcode(&op);
      }

      // Thread operations
      case uint16_t(Op::ThreadPrefix): {
        // Though thread ops can be used on nonshared memories, we make them
        // unavailable if shared memory has been disabled in the prefs, for
        // maximum predictability and safety and consistency with JS.
        if (moduleEnv_.sharedMemoryEnabled() == Shareable::False) {
          return iter_.unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(ThreadOp::Wake):
            CHECK_NEXT(emitWake());

          case uint32_t(ThreadOp::I32Wait):
            CHECK_NEXT(emitWait(ValType::I32, 4));
          case uint32_t(ThreadOp::I64Wait):
            CHECK_NEXT(emitWait(ValType::I64, 8));
          case uint32_t(ThreadOp::Fence):
            CHECK_NEXT(emitFence());

          case uint32_t(ThreadOp::I32AtomicLoad):
            CHECK_NEXT(emitAtomicLoad(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicLoad):
            CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicLoad8U):
            CHECK_NEXT(emitAtomicLoad(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicLoad16U):
            CHECK_NEXT(emitAtomicLoad(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicLoad8U):
            CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicLoad16U):
            CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicLoad32U):
            CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicStore):
            CHECK_NEXT(emitAtomicStore(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicStore):
            CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicStore8U):
            CHECK_NEXT(emitAtomicStore(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicStore16U):
            CHECK_NEXT(emitAtomicStore(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicStore8U):
            CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicStore16U):
            CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicStore32U):
            CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicAdd):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::Add));
          case uint32_t(ThreadOp::I64AtomicAdd):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::Add));
          case uint32_t(ThreadOp::I32AtomicAdd8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::Add));
          case uint32_t(ThreadOp::I32AtomicAdd16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::Add));
          case uint32_t(ThreadOp::I64AtomicAdd8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::Add));
          case uint32_t(ThreadOp::I64AtomicAdd16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::Add));
          case uint32_t(ThreadOp::I64AtomicAdd32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::Add));

          case uint32_t(ThreadOp::I32AtomicSub):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::Sub));
          case uint32_t(ThreadOp::I64AtomicSub):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::Sub));
          case uint32_t(ThreadOp::I32AtomicSub8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::Sub));
          case uint32_t(ThreadOp::I32AtomicSub16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::Sub));
          case uint32_t(ThreadOp::I64AtomicSub8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::Sub));
          case uint32_t(ThreadOp::I64AtomicSub16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::Sub));
          case uint32_t(ThreadOp::I64AtomicSub32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::Sub));

          case uint32_t(ThreadOp::I32AtomicAnd):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::And));
          case uint32_t(ThreadOp::I64AtomicAnd):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::And));
          case uint32_t(ThreadOp::I32AtomicAnd8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::And));
          case uint32_t(ThreadOp::I32AtomicAnd16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::And));
          case uint32_t(ThreadOp::I64AtomicAnd8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::And));
          case uint32_t(ThreadOp::I64AtomicAnd16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::And));
          case uint32_t(ThreadOp::I64AtomicAnd32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::And));

          case uint32_t(ThreadOp::I32AtomicOr):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::Or));
          case uint32_t(ThreadOp::I64AtomicOr):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::Or));
          case uint32_t(ThreadOp::I32AtomicOr8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::Or));
          case uint32_t(ThreadOp::I32AtomicOr16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::Or));
          case uint32_t(ThreadOp::I64AtomicOr8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::Or));
          case uint32_t(ThreadOp::I64AtomicOr16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::Or));
          case uint32_t(ThreadOp::I64AtomicOr32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::Or));

          case uint32_t(ThreadOp::I32AtomicXor):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::Xor));
          case uint32_t(ThreadOp::I64AtomicXor):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::Xor));
          case uint32_t(ThreadOp::I32AtomicXor8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::Xor));
          case uint32_t(ThreadOp::I32AtomicXor16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::Xor));
          case uint32_t(ThreadOp::I64AtomicXor8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::Xor));
          case uint32_t(ThreadOp::I64AtomicXor16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::Xor));
          case uint32_t(ThreadOp::I64AtomicXor32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::Xor));

          case uint32_t(ThreadOp::I32AtomicXchg):
            CHECK_NEXT(emitAtomicXchg(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicXchg):
            CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicXchg8U):
            CHECK_NEXT(emitAtomicXchg(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicXchg16U):
            CHECK_NEXT(emitAtomicXchg(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicXchg8U):
            CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicXchg16U):
            CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicXchg32U):
            CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicCmpXchg):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicCmpXchg):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicCmpXchg8U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicCmpXchg16U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicCmpXchg8U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicCmpXchg16U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicCmpXchg32U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Uint32));

          default:
            return iter_.unrecognizedOpcode(&op);
        }
        break;
      }

      // asm.js and other private operations
      case uint16_t(Op::MozPrefix): {
        if (op.b1 != uint32_t(MozOp::CallBuiltinModuleFunc) ||
            !moduleEnv_.isBuiltinModule()) {
          return iter_.unrecognizedOpcode(&op);
        }
        // Call a private builtin module func
        CHECK_NEXT(emitCallBuiltinModuleFunc());
      }

      default:
        return iter_.unrecognizedOpcode(&op);
    }

#undef CHECK
#undef NEXT
#undef CHECK_NEXT
#undef CHECK_POINTER_COUNT
#undef dispatchBinary0
#undef dispatchBinary1
#undef dispatchBinary2
#undef dispatchBinary3
#undef dispatchUnary0
#undef dispatchUnary1
#undef dispatchUnary2
#undef dispatchComparison0
#undef dispatchConversion0
#undef dispatchConversion1
#undef dispatchConversionOOM
#undef dispatchCalloutConversionOOM
#undef dispatchIntDivCallout
#undef dispatchVectorBinary
#undef dispatchVectorUnary
#undef dispatchVectorComparison
#undef dispatchExtractLane
#undef dispatchReplaceLane
#undef dispatchSplat
#undef dispatchVectorReduction

    MOZ_CRASH("unreachable");
  }

  MOZ_CRASH("unreachable");
}

//////////////////////////////////////////////////////////////////////////////
//
// Various helpers.

void BaseCompiler::assertResultRegistersAvailable(ResultType type) {
#ifdef DEBUG
  for (ABIResultIter iter(type); !iter.done(); iter.next()) {
    ABIResult result = iter.cur();
    if (!result.inRegister()) {
      return;
    }
    switch (result.type().kind()) {
      case ValType::I32:
        MOZ_ASSERT(isAvailableI32(RegI32(result.gpr())));
        break;
      case ValType::I64:
        MOZ_ASSERT(isAvailableI64(RegI64(result.gpr64())));
        break;
      case ValType::V128:
#  ifdef ENABLE_WASM_SIMD
        MOZ_ASSERT(isAvailableV128(RegV128(result.fpr())));
        break;
#  else
        MOZ_CRASH("No SIMD support");
#  endif
      case ValType::F32:
        MOZ_ASSERT(isAvailableF32(RegF32(result.fpr())));
        break;
      case ValType::F64:
        MOZ_ASSERT(isAvailableF64(RegF64(result.fpr())));
        break;
      case ValType::Ref:
        MOZ_ASSERT(isAvailableRef(RegRef(result.gpr())));
        break;
    }
  }
#endif
}

void BaseCompiler::saveTempPtr(const RegPtr& r) {
  MOZ_ASSERT(!ra.isAvailablePtr(r));
  fr.pushGPR(r);
  ra.freePtr(r);
  MOZ_ASSERT(ra.isAvailablePtr(r));
}

void BaseCompiler::restoreTempPtr(const RegPtr& r) {
  MOZ_ASSERT(ra.isAvailablePtr(r));
  ra.needPtr(r);
  fr.popGPR(r);
  MOZ_ASSERT(!ra.isAvailablePtr(r));
}

#ifdef DEBUG
void BaseCompiler::performRegisterLeakCheck() {
  BaseRegAlloc::LeakCheck check(ra);
  for (auto& item : stk_) {
    switch (item.kind_) {
      case Stk::RegisterI32:
        check.addKnownI32(item.i32reg());
        break;
      case Stk::RegisterI64:
        check.addKnownI64(item.i64reg());
        break;
      case Stk::RegisterF32:
        check.addKnownF32(item.f32reg());
        break;
      case Stk::RegisterF64:
        check.addKnownF64(item.f64reg());
        break;
#  ifdef ENABLE_WASM_SIMD
      case Stk::RegisterV128:
        check.addKnownV128(item.v128reg());
        break;
#  endif
      case Stk::RegisterRef:
        check.addKnownRef(item.refReg());
        break;
      default:
        break;
    }
  }
}

void BaseCompiler::assertStackInvariants() const {
  if (deadCode_) {
    // Nonlocal control flow can pass values in stack locations in a way that
    // isn't accounted for by the value stack.  In dead code, which occurs
    // after unconditional non-local control flow, there is no invariant to
    // assert.
    return;
  }
  size_t size = 0;
  for (const Stk& v : stk_) {
    switch (v.kind()) {
      case Stk::MemRef:
        size += BaseStackFrame::StackSizeOfPtr;
        break;
      case Stk::MemI32:
        size += BaseStackFrame::StackSizeOfPtr;
        break;
      case Stk::MemI64:
        size += BaseStackFrame::StackSizeOfInt64;
        break;
      case Stk::MemF64:
        size += BaseStackFrame::StackSizeOfDouble;
        break;
      case Stk::MemF32:
        size += BaseStackFrame::StackSizeOfFloat;
        break;
#  ifdef ENABLE_WASM_SIMD
      case Stk::MemV128:
        size += BaseStackFrame::StackSizeOfV128;
        break;
#  endif
      default:
        MOZ_ASSERT(!v.isMem());
        break;
    }
  }
  MOZ_ASSERT(size == fr.dynamicHeight());
}

void BaseCompiler::showStack(const char* who) const {
  fprintf(stderr, "Stack at %s {{\n", who);
  size_t n = 0;
  for (const Stk& elem : stk_) {
    fprintf(stderr, "  [%zu] ", n++);
    elem.showStackElem();
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "}}\n");
}
#endif

//////////////////////////////////////////////////////////////////////////////
//
// Main compilation logic.

bool BaseCompiler::emitFunction() {
  AutoCreatedBy acb(masm, "(wasm)BaseCompiler::emitFunction");

  if (!beginFunction()) {
    return false;
  }

  if (!emitBody()) {
    return false;
  }

  return endFunction();
}

BaseCompiler::BaseCompiler(const ModuleEnvironment& moduleEnv,
                           const CompilerEnvironment& compilerEnv,
                           const FuncCompileInput& func,
                           const ValTypeVector& locals,
                           const RegisterOffsets& trapExitLayout,
                           size_t trapExitLayoutNumWords, Decoder& decoder,
                           StkVector& stkSource, TempAllocator* alloc,
                           MacroAssembler* masm, StackMaps* stackMaps)
    :  // Environment
      moduleEnv_(moduleEnv),
      compilerEnv_(compilerEnv),
      func_(func),
      locals_(locals),
      previousBreakablePoint_(UINT32_MAX),
      stkSource_(stkSource),
      // Output-only data structures
      alloc_(alloc->fallible()),
      masm(*masm),
      // Compilation state
      decoder_(decoder),
      iter_(moduleEnv, decoder),
      fr(*masm),
      stackMapGenerator_(stackMaps, trapExitLayout, trapExitLayoutNumWords,
                         *masm),
      deadCode_(false),
      // Init value is selected to ensure proper logic in finishTryNote.
      mostRecentFinishedTryNoteIndex_(0),
      bceSafe_(0),
      latentOp_(LatentOp::None),
      latentType_(ValType::I32),
      latentIntCmp_(Assembler::Equal),
      latentDoubleCmp_(Assembler::DoubleEqual) {
  // Our caller, BaselineCompileFunctions, will lend us the vector contents to
  // use for the eval stack.  To get hold of those contents, we'll temporarily
  // installing an empty one in its place.
  MOZ_ASSERT(stk_.empty());
  stk_.swap(stkSource_);

  // Assuming that previously processed wasm functions are well formed, the
  // eval stack should now be empty.  But empty it anyway; any non-emptyness
  // at this point will cause chaos.
  stk_.clear();
}

BaseCompiler::~BaseCompiler() {
  stk_.swap(stkSource_);
  // We've returned the eval stack vector contents to our caller,
  // BaselineCompileFunctions.  We expect the vector we get in return to be
  // empty since that's what we swapped for the stack vector in our
  // constructor.
  MOZ_ASSERT(stk_.empty());
}

bool BaseCompiler::init() {
  // We may lift this restriction in the future.
  for (uint32_t memoryIndex = 0; memoryIndex < moduleEnv_.memories.length();
       memoryIndex++) {
    MOZ_ASSERT_IF(isMem64(memoryIndex),
                  !moduleEnv_.hugeMemoryEnabled(memoryIndex));
  }
  // asm.js is not supported in baseline
  MOZ_ASSERT(!moduleEnv_.isAsmJS());
  // Only asm.js modules have call site line numbers
  MOZ_ASSERT(func_.callSiteLineNums.empty());

  ra.init(this);

  if (!SigD_.append(ValType::F64)) {
    return false;
  }
  if (!SigF_.append(ValType::F32)) {
    return false;
  }

  ArgTypeVector args(funcType());
  return fr.setupLocals(locals_, args, compilerEnv_.debugEnabled(),
                        &localInfo_);
}

FuncOffsets BaseCompiler::finish() {
  MOZ_ASSERT(iter_.done(), "all bytes must be consumed");
  MOZ_ASSERT(stk_.empty());
  MOZ_ASSERT(stackMapGenerator_.memRefsOnStk == 0);

  masm.flushBuffer();

  return offsets_;
}

}  // namespace wasm
}  // namespace js

bool js::wasm::BaselinePlatformSupport() {
#if defined(JS_CODEGEN_ARM)
  // Simplifying assumption: require SDIV and UDIV.
  //
  // I have no good data on ARM populations allowing me to say that
  // X% of devices in the market implement SDIV and UDIV.  However,
  // they are definitely implemented on the Cortex-A7 and Cortex-A15
  // and on all ARMv8 systems.
  if (!HasIDIV()) {
    return false;
  }
#endif
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) ||        \
    defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) ||      \
    defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
  return true;
#else
  return false;
#endif
}

bool js::wasm::BaselineCompileFunctions(const ModuleEnvironment& moduleEnv,
                                        const CompilerEnvironment& compilerEnv,
                                        LifoAlloc& lifo,
                                        const FuncCompileInputVector& inputs,
                                        CompiledCode* code,
                                        UniqueChars* error) {
  MOZ_ASSERT(compilerEnv.tier() == Tier::Baseline);
  MOZ_ASSERT(moduleEnv.kind == ModuleKind::Wasm);

  // The MacroAssembler will sometimes access the jitContext.

  TempAllocator alloc(&lifo);
  JitContext jitContext;
  MOZ_ASSERT(IsCompilingWasm());
  WasmMacroAssembler masm(alloc, moduleEnv);

  // Swap in already-allocated empty vectors to avoid malloc/free.
  MOZ_ASSERT(code->empty());
  if (!code->swap(masm)) {
    return false;
  }

  // Create a description of the stack layout created by GenerateTrapExit().
  RegisterOffsets trapExitLayout;
  size_t trapExitLayoutNumWords;
  GenerateTrapExitRegisterOffsets(&trapExitLayout, &trapExitLayoutNumWords);

  // The compiler's operand stack.  We reuse it across all functions so as to
  // avoid malloc/free.  Presize it to 128 elements in the hope of avoiding
  // reallocation later.
  StkVector stk;
  if (!stk.reserve(128)) {
    return false;
  }

  for (const FuncCompileInput& func : inputs) {
    Decoder d(func.begin, func.end, func.lineOrBytecode, error);

    // Build the local types vector.

    ValTypeVector locals;
    if (!DecodeLocalEntriesWithParams(d, moduleEnv, func.index, &locals)) {
      return false;
    }

    size_t unwindInfoBefore = masm.codeRangeUnwindInfos().length();

    // One-pass baseline compilation.

    BaseCompiler f(moduleEnv, compilerEnv, func, locals, trapExitLayout,
                   trapExitLayoutNumWords, d, stk, &alloc, &masm,
                   &code->stackMaps);
    if (!f.init()) {
      return false;
    }
    if (!f.emitFunction()) {
      return false;
    }
    FuncOffsets offsets(f.finish());
    bool hasUnwindInfo =
        unwindInfoBefore != masm.codeRangeUnwindInfos().length();
    if (!code->codeRanges.emplaceBack(func.index, func.lineOrBytecode, offsets,
                                      hasUnwindInfo)) {
      return false;
    }

    // Record observed feature usage
    code->featureUsage |= f.iter_.featureUsage();
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  return code->swap(masm);
}
