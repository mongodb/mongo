/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCodeGen.h"

#include "mozilla/Casting.h"

#include "gc/GC.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRGenerator.h"
#include "jit/CalleeToken.h"
#include "jit/FixedList.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/JitcodeMap.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "jit/PerfSpewer.h"
#include "jit/SharedICHelpers.h"
#include "jit/TemplateObject.h"
#include "jit/TrialInlining.h"
#include "jit/VMFunctions.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/UniquePtr.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BuiltinObjectKind.h"
#include "vm/EnvironmentObject.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/Interpreter.h"
#include "vm/JSFunction.h"
#include "vm/Time.h"
#ifdef MOZ_VTUNE
#  include "vtune/VTuneWrapper.h"
#endif

#include "debugger/DebugAPI-inl.h"
#include "jit/BaselineFrameInfo-inl.h"
#include "jit/JitHints-inl.h"
#include "jit/JitScript-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/SharedICHelpers-inl.h"
#include "jit/TemplateObject-inl.h"
#include "jit/VMFunctionList-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using JS::TraceKind;

using mozilla::AssertedCast;
using mozilla::Maybe;

namespace js {

class PlainObject;

namespace jit {

BaselineCompilerHandler::BaselineCompilerHandler(JSContext* cx,
                                                 MacroAssembler& masm,
                                                 TempAllocator& alloc,
                                                 JSScript* script)
    : frame_(script, masm),
      alloc_(alloc),
      analysis_(alloc, script),
#ifdef DEBUG
      masm_(masm),
#endif
      script_(script),
      pc_(script->code()),
      icEntryIndex_(0),
      compileDebugInstrumentation_(script->isDebuggee()),
      ionCompileable_(IsIonEnabled(cx) && CanIonCompileScript(cx, script)) {
}

BaselineInterpreterHandler::BaselineInterpreterHandler(JSContext* cx,
                                                       MacroAssembler& masm)
    : frame_(masm) {}

template <typename Handler>
template <typename... HandlerArgs>
BaselineCodeGen<Handler>::BaselineCodeGen(JSContext* cx, TempAllocator& alloc,
                                          HandlerArgs&&... args)
    : handler(cx, masm, std::forward<HandlerArgs>(args)...),
      cx(cx),
      masm(cx, alloc),
      frame(handler.frame()) {}

BaselineCompiler::BaselineCompiler(JSContext* cx, TempAllocator& alloc,
                                   JSScript* script)
    : BaselineCodeGen(cx, alloc, /* HandlerArgs = */ alloc, script) {
#ifdef JS_CODEGEN_NONE
  MOZ_CRASH();
#endif
}

BaselineInterpreterGenerator::BaselineInterpreterGenerator(JSContext* cx,
                                                           TempAllocator& alloc)
    : BaselineCodeGen(cx, alloc /* no handlerArgs */) {}

bool BaselineCompilerHandler::init(JSContext* cx) {
  if (!analysis_.init(alloc_)) {
    return false;
  }

  uint32_t len = script_->length();

  if (!labels_.init(alloc_, len)) {
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    new (&labels_[i]) Label();
  }

  if (!frame_.init(alloc_)) {
    return false;
  }

  return true;
}

bool BaselineCompiler::init() {
  if (!handler.init(cx)) {
    return false;
  }

  return true;
}

bool BaselineCompilerHandler::recordCallRetAddr(JSContext* cx,
                                                RetAddrEntry::Kind kind,
                                                uint32_t retOffset) {
  uint32_t pcOffset = script_->pcToOffset(pc_);

  // Entries must be sorted by pcOffset for binary search to work.
  // See BaselineScript::retAddrEntryFromPCOffset.
  MOZ_ASSERT_IF(!retAddrEntries_.empty(),
                retAddrEntries_.back().pcOffset() <= pcOffset);

  // Similarly, entries must be sorted by return offset and this offset must be
  // unique. See BaselineScript::retAddrEntryFromReturnOffset.
  MOZ_ASSERT_IF(!retAddrEntries_.empty() && !masm_.oom(),
                retAddrEntries_.back().returnOffset().offset() < retOffset);

  if (!retAddrEntries_.emplaceBack(pcOffset, kind, CodeOffset(retOffset))) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool BaselineInterpreterHandler::recordCallRetAddr(JSContext* cx,
                                                   RetAddrEntry::Kind kind,
                                                   uint32_t retOffset) {
  switch (kind) {
    case RetAddrEntry::Kind::DebugPrologue:
      MOZ_ASSERT(callVMOffsets_.debugPrologueOffset == 0,
                 "expected single DebugPrologue call");
      callVMOffsets_.debugPrologueOffset = retOffset;
      break;
    case RetAddrEntry::Kind::DebugEpilogue:
      MOZ_ASSERT(callVMOffsets_.debugEpilogueOffset == 0,
                 "expected single DebugEpilogue call");
      callVMOffsets_.debugEpilogueOffset = retOffset;
      break;
    case RetAddrEntry::Kind::DebugAfterYield:
      MOZ_ASSERT(callVMOffsets_.debugAfterYieldOffset == 0,
                 "expected single DebugAfterYield call");
      callVMOffsets_.debugAfterYieldOffset = retOffset;
      break;
    default:
      break;
  }

  return true;
}

bool BaselineInterpreterHandler::addDebugInstrumentationOffset(
    JSContext* cx, CodeOffset offset) {
  if (!debugInstrumentationOffsets_.append(offset.offset())) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

MethodStatus BaselineCompiler::compile() {
  AutoCreatedBy acb(masm, "BaselineCompiler::compile");

  Rooted<JSScript*> script(cx, handler.script());
  JitSpew(JitSpew_BaselineScripts, "Baseline compiling script %s:%u:%u (%p)",
          script->filename(), script->lineno(),
          script->column().oneOriginValue(), script.get());

  JitSpew(JitSpew_Codegen, "# Emitting baseline code for script %s:%u:%u",
          script->filename(), script->lineno(),
          script->column().oneOriginValue());

  AutoIncrementalTimer timer(cx->realm()->timers.baselineCompileTime);

  AutoKeepJitScripts keepJitScript(cx);
  if (!script->ensureHasJitScript(cx, keepJitScript)) {
    return Method_Error;
  }

  // When code coverage is enabled, we have to create the ScriptCounts if they
  // do not exist.
  if (!script->hasScriptCounts() && cx->realm()->collectCoverageForDebug()) {
    if (!script->initScriptCounts(cx)) {
      return Method_Error;
    }
  }

  if (!JitOptions.disableJitHints &&
      cx->runtime()->jitRuntime()->hasJitHintsMap()) {
    JitHintsMap* jitHints = cx->runtime()->jitRuntime()->getJitHintsMap();
    jitHints->setEagerBaselineHint(script);
  }

  // Suppress GC during compilation.
  gc::AutoSuppressGC suppressGC(cx);

  if (!script->jitScript()->ensureHasCachedBaselineJitData(cx, script)) {
    return Method_Error;
  }

  MOZ_ASSERT(!script->hasBaselineScript());

  perfSpewer_.recordOffset(masm, "Prologue");
  if (!emitPrologue()) {
    return Method_Error;
  }

  MethodStatus status = emitBody();
  if (status != Method_Compiled) {
    return status;
  }

  perfSpewer_.recordOffset(masm, "Epilogue");
  if (!emitEpilogue()) {
    return Method_Error;
  }

  perfSpewer_.recordOffset(masm, "OOLPostBarrierSlot");
  if (!emitOutOfLinePostBarrierSlot()) {
    return Method_Error;
  }

  AutoCreatedBy acb2(masm, "exception_tail");
  Linker linker(masm);
  if (masm.oom()) {
    ReportOutOfMemory(cx);
    return Method_Error;
  }

  JitCode* code = linker.newCode(cx, CodeKind::Baseline);
  if (!code) {
    return Method_Error;
  }

  UniquePtr<BaselineScript> baselineScript(
      BaselineScript::New(
          cx, warmUpCheckPrologueOffset_.offset(),
          profilerEnterFrameToggleOffset_.offset(),
          profilerExitFrameToggleOffset_.offset(),
          handler.retAddrEntries().length(), handler.osrEntries().length(),
          debugTrapEntries_.length(), script->resumeOffsets().size()),
      JS::DeletePolicy<BaselineScript>(cx->runtime()));
  if (!baselineScript) {
    return Method_Error;
  }

  baselineScript->setMethod(code);

  JitSpew(JitSpew_BaselineScripts,
          "Created BaselineScript %p (raw %p) for %s:%u:%u",
          (void*)baselineScript.get(), (void*)code->raw(), script->filename(),
          script->lineno(), script->column().oneOriginValue());

  baselineScript->copyRetAddrEntries(handler.retAddrEntries().begin());
  baselineScript->copyOSREntries(handler.osrEntries().begin());
  baselineScript->copyDebugTrapEntries(debugTrapEntries_.begin());

  // If profiler instrumentation is enabled, toggle instrumentation on.
  if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(
          cx->runtime())) {
    baselineScript->toggleProfilerInstrumentation(true);
  }

  // Compute native resume addresses for the script's resume offsets.
  baselineScript->computeResumeNativeOffsets(script, resumeOffsetEntries_);

  if (compileDebugInstrumentation()) {
    baselineScript->setHasDebugInstrumentation();
  }

  // Always register a native => bytecode mapping entry, since profiler can be
  // turned on with baseline jitcode on stack, and baseline jitcode cannot be
  // invalidated.
  {
    JitSpew(JitSpew_Profiling,
            "Added JitcodeGlobalEntry for baseline script %s:%u:%u (%p)",
            script->filename(), script->lineno(),
            script->column().oneOriginValue(), baselineScript.get());

    // Generate profiling string.
    UniqueChars str = GeckoProfilerRuntime::allocProfileString(cx, script);
    if (!str) {
      return Method_Error;
    }

    auto entry = MakeJitcodeGlobalEntry<BaselineEntry>(
        cx, code, code->raw(), code->rawEnd(), script, std::move(str));
    if (!entry) {
      return Method_Error;
    }

    JitcodeGlobalTable* globalTable =
        cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (!globalTable->addEntry(std::move(entry))) {
      ReportOutOfMemory(cx);
      return Method_Error;
    }

    // Mark the jitcode as having a bytecode map.
    code->setHasBytecodeMap();
  }

  script->jitScript()->setBaselineScript(script, baselineScript.release());

  perfSpewer_.saveProfile(cx, script, code);

#ifdef MOZ_VTUNE
  vtune::MarkScript(code, script, "baseline");
#endif

  return Method_Compiled;
}

// On most platforms we use a dedicated bytecode PC register to avoid many
// dependent loads and stores for sequences of simple bytecode ops. This
// register must be saved/restored around VM and IC calls.
//
// On 32-bit x86 we don't have enough registers for this (because R0-R2 require
// 6 registers) so there we always store the pc on the frame.
static constexpr bool HasInterpreterPCReg() {
  return InterpreterPCReg != InvalidReg;
}

static Register LoadBytecodePC(MacroAssembler& masm, Register scratch) {
  if (HasInterpreterPCReg()) {
    return InterpreterPCReg;
  }

  Address pcAddr(FramePointer, BaselineFrame::reverseOffsetOfInterpreterPC());
  masm.loadPtr(pcAddr, scratch);
  return scratch;
}

static void LoadInt8Operand(MacroAssembler& masm, Register dest) {
  Register pc = LoadBytecodePC(masm, dest);
  masm.load8SignExtend(Address(pc, sizeof(jsbytecode)), dest);
}

static void LoadUint8Operand(MacroAssembler& masm, Register dest) {
  Register pc = LoadBytecodePC(masm, dest);
  masm.load8ZeroExtend(Address(pc, sizeof(jsbytecode)), dest);
}

static void LoadUint16Operand(MacroAssembler& masm, Register dest) {
  Register pc = LoadBytecodePC(masm, dest);
  masm.load16ZeroExtend(Address(pc, sizeof(jsbytecode)), dest);
}

static void LoadInt32Operand(MacroAssembler& masm, Register dest) {
  Register pc = LoadBytecodePC(masm, dest);
  masm.load32(Address(pc, sizeof(jsbytecode)), dest);
}

static void LoadInt32OperandSignExtendToPtr(MacroAssembler& masm, Register pc,
                                            Register dest) {
  masm.load32SignExtendToPtr(Address(pc, sizeof(jsbytecode)), dest);
}

static void LoadUint24Operand(MacroAssembler& masm, size_t offset,
                              Register dest) {
  // Load the opcode and operand, then left shift to discard the opcode.
  Register pc = LoadBytecodePC(masm, dest);
  masm.load32(Address(pc, offset), dest);
  masm.rshift32(Imm32(8), dest);
}

static void LoadInlineValueOperand(MacroAssembler& masm, ValueOperand dest) {
  // Note: the Value might be unaligned but as above we rely on all our
  // platforms having appropriate support for unaligned accesses (except for
  // floating point instructions on ARM).
  Register pc = LoadBytecodePC(masm, dest.scratchReg());
  masm.loadUnalignedValue(Address(pc, sizeof(jsbytecode)), dest);
}

template <>
void BaselineCompilerCodeGen::loadScript(Register dest) {
  masm.movePtr(ImmGCPtr(handler.script()), dest);
}

template <>
void BaselineInterpreterCodeGen::loadScript(Register dest) {
  masm.loadPtr(frame.addressOfInterpreterScript(), dest);
}

template <>
void BaselineCompilerCodeGen::saveInterpreterPCReg() {}

template <>
void BaselineInterpreterCodeGen::saveInterpreterPCReg() {
  if (HasInterpreterPCReg()) {
    masm.storePtr(InterpreterPCReg, frame.addressOfInterpreterPC());
  }
}

template <>
void BaselineCompilerCodeGen::restoreInterpreterPCReg() {}

template <>
void BaselineInterpreterCodeGen::restoreInterpreterPCReg() {
  if (HasInterpreterPCReg()) {
    masm.loadPtr(frame.addressOfInterpreterPC(), InterpreterPCReg);
  }
}

template <>
void BaselineCompilerCodeGen::emitInitializeLocals() {
  // Initialize all locals to |undefined|. Lexical bindings are temporal
  // dead zoned in bytecode.

  size_t n = frame.nlocals();
  if (n == 0) {
    return;
  }

  // Use R0 to minimize code size. If the number of locals to push is <
  // LOOP_UNROLL_FACTOR, then the initialization pushes are emitted directly
  // and inline.  Otherwise, they're emitted in a partially unrolled loop.
  static const size_t LOOP_UNROLL_FACTOR = 4;
  size_t toPushExtra = n % LOOP_UNROLL_FACTOR;

  masm.moveValue(UndefinedValue(), R0);

  // Handle any extra pushes left over by the optional unrolled loop below.
  for (size_t i = 0; i < toPushExtra; i++) {
    masm.pushValue(R0);
  }

  // Partially unrolled loop of pushes.
  if (n >= LOOP_UNROLL_FACTOR) {
    size_t toPush = n - toPushExtra;
    MOZ_ASSERT(toPush % LOOP_UNROLL_FACTOR == 0);
    MOZ_ASSERT(toPush >= LOOP_UNROLL_FACTOR);
    masm.move32(Imm32(toPush), R1.scratchReg());
    // Emit unrolled loop with 4 pushes per iteration.
    Label pushLoop;
    masm.bind(&pushLoop);
    for (size_t i = 0; i < LOOP_UNROLL_FACTOR; i++) {
      masm.pushValue(R0);
    }
    masm.branchSub32(Assembler::NonZero, Imm32(LOOP_UNROLL_FACTOR),
                     R1.scratchReg(), &pushLoop);
  }
}

template <>
void BaselineInterpreterCodeGen::emitInitializeLocals() {
  // Push |undefined| for all locals.

  Register scratch = R0.scratchReg();
  loadScript(scratch);
  masm.loadPtr(Address(scratch, JSScript::offsetOfSharedData()), scratch);
  masm.loadPtr(Address(scratch, SharedImmutableScriptData::offsetOfISD()),
               scratch);
  masm.load32(Address(scratch, ImmutableScriptData::offsetOfNfixed()), scratch);

  Label top, done;
  masm.branchTest32(Assembler::Zero, scratch, scratch, &done);
  masm.bind(&top);
  {
    masm.pushValue(UndefinedValue());
    masm.branchSub32(Assembler::NonZero, Imm32(1), scratch, &top);
  }
  masm.bind(&done);
}

// On input:
//  R2.scratchReg() contains object being written to.
//  Called with the baseline stack synced, except for R0 which is preserved.
//  All other registers are usable as scratch.
// This calls:
//    void PostWriteBarrier(JSRuntime* rt, JSObject* obj);
template <typename Handler>
bool BaselineCodeGen<Handler>::emitOutOfLinePostBarrierSlot() {
  AutoCreatedBy acb(masm,
                    "BaselineCodeGen<Handler>::emitOutOfLinePostBarrierSlot");

  if (!postBarrierSlot_.used()) {
    return true;
  }

  masm.bind(&postBarrierSlot_);

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  Register objReg = R2.scratchReg();

  // Check one element cache to avoid VM call.
  Label skipBarrier;
  auto* lastCellAddr = cx->runtime()->gc.addressOfLastBufferedWholeCell();
  masm.branchPtr(Assembler::Equal, AbsoluteAddress(lastCellAddr), objReg,
                 &skipBarrier);

  saveInterpreterPCReg();

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  MOZ_ASSERT(!regs.has(FramePointer));
  regs.take(R0);
  regs.take(objReg);
  Register scratch = regs.takeAny();

  masm.pushValue(R0);

  using Fn = void (*)(JSRuntime* rt, js::gc::Cell* cell);
  masm.setupUnalignedABICall(scratch);
  masm.movePtr(ImmPtr(cx->runtime()), scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(objReg);
  masm.callWithABI<Fn, PostWriteBarrier>();

  restoreInterpreterPCReg();

  masm.popValue(R0);

  masm.bind(&skipBarrier);
  masm.ret();
  return true;
}

// Scan the a cache IR stub's fields and create an allocation site for any that
// refer to the catch-all unknown allocation site. This will be the case for
// stubs created when running in the interpreter. This happens on transition to
// baseline.
static bool CreateAllocSitesForCacheIRStub(JSScript* script, uint32_t pcOffset,
                                           ICCacheIRStub* stub) {
  const CacheIRStubInfo* stubInfo = stub->stubInfo();
  uint8_t* stubData = stub->stubDataStart();

  ICScript* icScript = script->jitScript()->icScript();

  uint32_t field = 0;
  size_t offset = 0;
  while (true) {
    StubField::Type fieldType = stubInfo->fieldType(field);
    if (fieldType == StubField::Type::Limit) {
      break;
    }

    if (fieldType == StubField::Type::AllocSite) {
      gc::AllocSite* site =
          stubInfo->getPtrStubField<ICCacheIRStub, gc::AllocSite>(stub, offset);
      if (site->kind() == gc::AllocSite::Kind::Unknown) {
        gc::AllocSite* newSite =
            icScript->getOrCreateAllocSite(script, pcOffset);
        if (!newSite) {
          return false;
        }

        stubInfo->replaceStubRawWord(stubData, offset, uintptr_t(site),
                                     uintptr_t(newSite));
      }
    }

    field++;
    offset += StubField::sizeInBytes(fieldType);
  }

  return true;
}

static void CreateAllocSitesForICChain(JSScript* script, uint32_t pcOffset,
                                       uint32_t entryIndex) {
  JitScript* jitScript = script->jitScript();
  ICStub* stub = jitScript->icEntry(entryIndex).firstStub();

  while (!stub->isFallback()) {
    if (!CreateAllocSitesForCacheIRStub(script, pcOffset,
                                        stub->toCacheIRStub())) {
      // This is an optimization and safe to skip if we hit OOM or per-zone
      // limit.
      return;
    }
    stub = stub->toCacheIRStub()->next();
  }
}

template <>
bool BaselineCompilerCodeGen::emitNextIC() {
  AutoCreatedBy acb(masm, "emitNextIC");

  // Emit a call to an IC stored in JitScript. Calls to this must match the
  // ICEntry order in JitScript: first the non-op IC entries for |this| and
  // formal arguments, then the for-op IC entries for JOF_IC ops.

  JSScript* script = handler.script();
  uint32_t pcOffset = script->pcToOffset(handler.pc());

  // We don't use every ICEntry and we can skip unreachable ops, so we have
  // to loop until we find an ICEntry for the current pc.
  const ICFallbackStub* stub;
  uint32_t entryIndex;
  do {
    stub = script->jitScript()->fallbackStub(handler.icEntryIndex());
    entryIndex = handler.icEntryIndex();
    handler.moveToNextICEntry();
  } while (stub->pcOffset() < pcOffset);

  MOZ_ASSERT(stub->pcOffset() == pcOffset);
  MOZ_ASSERT(BytecodeOpHasIC(JSOp(*handler.pc())));

  if (BytecodeOpCanHaveAllocSite(JSOp(*handler.pc()))) {
    CreateAllocSitesForICChain(script, pcOffset, entryIndex);
  }

  // Load stub pointer into ICStubReg.
  masm.loadPtr(frame.addressOfICScript(), ICStubReg);
  size_t firstStubOffset = ICScript::offsetOfFirstStub(entryIndex);
  masm.loadPtr(Address(ICStubReg, firstStubOffset), ICStubReg);

  CodeOffset returnOffset;
  EmitCallIC(masm, &returnOffset);

  RetAddrEntry::Kind kind = RetAddrEntry::Kind::IC;
  if (!handler.retAddrEntries().emplaceBack(pcOffset, kind, returnOffset)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

template <>
bool BaselineInterpreterCodeGen::emitNextIC() {
  saveInterpreterPCReg();
  masm.loadPtr(frame.addressOfInterpreterICEntry(), ICStubReg);
  masm.loadPtr(Address(ICStubReg, ICEntry::offsetOfFirstStub()), ICStubReg);
  masm.call(Address(ICStubReg, ICStub::offsetOfStubCode()));
  uint32_t returnOffset = masm.currentOffset();
  restoreInterpreterPCReg();

  // If this is an IC for a bytecode op where Ion may inline scripts, we need to
  // record the return offset for Ion bailouts.
  if (handler.currentOp()) {
    JSOp op = *handler.currentOp();
    MOZ_ASSERT(BytecodeOpHasIC(op));
    if (IsIonInlinableOp(op)) {
      if (!handler.icReturnOffsets().emplaceBack(returnOffset, op)) {
        return false;
      }
    }
  }

  return true;
}

template <>
void BaselineCompilerCodeGen::computeFrameSize(Register dest) {
  MOZ_ASSERT(!inCall_, "must not be called in the middle of a VM call");
  masm.move32(Imm32(frame.frameSize()), dest);
}

template <>
void BaselineInterpreterCodeGen::computeFrameSize(Register dest) {
  // dest := FramePointer - StackPointer.
  MOZ_ASSERT(!inCall_, "must not be called in the middle of a VM call");
  masm.mov(FramePointer, dest);
  masm.subStackPtrFrom(dest);
}

template <typename Handler>
void BaselineCodeGen<Handler>::prepareVMCall() {
  pushedBeforeCall_ = masm.framePushed();
#ifdef DEBUG
  inCall_ = true;
#endif

  // Ensure everything is synced.
  frame.syncStack(0);
}

template <>
void BaselineCompilerCodeGen::storeFrameSizeAndPushDescriptor(
    uint32_t argSize, Register scratch) {
#ifdef DEBUG
  masm.store32(Imm32(frame.frameSize()), frame.addressOfDebugFrameSize());
#endif

  masm.pushFrameDescriptor(FrameType::BaselineJS);
}

template <>
void BaselineInterpreterCodeGen::storeFrameSizeAndPushDescriptor(
    uint32_t argSize, Register scratch) {
#ifdef DEBUG
  // Store the frame size without VMFunction arguments in debug builds.
  // scratch := FramePointer - StackPointer - argSize.
  masm.mov(FramePointer, scratch);
  masm.subStackPtrFrom(scratch);
  masm.sub32(Imm32(argSize), scratch);
  masm.store32(scratch, frame.addressOfDebugFrameSize());
#endif

  masm.pushFrameDescriptor(FrameType::BaselineJS);
}

static uint32_t GetVMFunctionArgSize(const VMFunctionData& fun) {
  return fun.explicitStackSlots() * sizeof(void*);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::callVMInternal(VMFunctionId id,
                                              RetAddrEntry::Kind kind,
                                              CallVMPhase phase) {
#ifdef DEBUG
  // Assert prepareVMCall() has been called.
  MOZ_ASSERT(inCall_);
  inCall_ = false;
#endif

  TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(id);
  const VMFunctionData& fun = GetVMFunction(id);

  uint32_t argSize = GetVMFunctionArgSize(fun);

  // Assert all arguments were pushed.
  MOZ_ASSERT(masm.framePushed() - pushedBeforeCall_ == argSize);

  saveInterpreterPCReg();

  if (phase == CallVMPhase::AfterPushingLocals) {
    storeFrameSizeAndPushDescriptor(argSize, R0.scratchReg());
  } else {
    MOZ_ASSERT(phase == CallVMPhase::BeforePushingLocals);
#ifdef DEBUG
    uint32_t frameBaseSize = BaselineFrame::frameSizeForNumValueSlots(0);
    masm.store32(Imm32(frameBaseSize), frame.addressOfDebugFrameSize());
#endif
    masm.pushFrameDescriptor(FrameType::BaselineJS);
  }
  // Perform the call.
  masm.call(code);
  uint32_t callOffset = masm.currentOffset();

  // Pop arguments from framePushed.
  masm.implicitPop(argSize);

  restoreInterpreterPCReg();

  return handler.recordCallRetAddr(cx, kind, callOffset);
}

template <typename Handler>
template <typename Fn, Fn fn>
bool BaselineCodeGen<Handler>::callVM(RetAddrEntry::Kind kind,
                                      CallVMPhase phase) {
  VMFunctionId fnId = VMFunctionToId<Fn, fn>::id;
  return callVMInternal(fnId, kind, phase);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitStackCheck() {
  Label skipCall;
  if (handler.mustIncludeSlotsInStackCheck()) {
    // Subtract the size of script->nslots() first.
    Register scratch = R1.scratchReg();
    masm.moveStackPtrTo(scratch);
    subtractScriptSlotsSize(scratch, R2.scratchReg());
    masm.branchPtr(Assembler::BelowOrEqual,
                   AbsoluteAddress(cx->addressOfJitStackLimit()), scratch,
                   &skipCall);
  } else {
    masm.branchStackPtrRhs(Assembler::BelowOrEqual,
                           AbsoluteAddress(cx->addressOfJitStackLimit()),
                           &skipCall);
  }

  prepareVMCall();
  masm.loadBaselineFramePtr(FramePointer, R1.scratchReg());
  pushArg(R1.scratchReg());

  const CallVMPhase phase = CallVMPhase::BeforePushingLocals;
  const RetAddrEntry::Kind kind = RetAddrEntry::Kind::StackCheck;

  using Fn = bool (*)(JSContext*, BaselineFrame*);
  if (!callVM<Fn, CheckOverRecursedBaseline>(kind, phase)) {
    return false;
  }

  masm.bind(&skipCall);
  return true;
}

static void EmitCallFrameIsDebuggeeCheck(MacroAssembler& masm) {
  using Fn = void (*)(BaselineFrame* frame);
  masm.setupUnalignedABICall(R0.scratchReg());
  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
  masm.passABIArg(R0.scratchReg());
  masm.callWithABI<Fn, FrameIsDebuggeeCheck>();
}

template <>
bool BaselineCompilerCodeGen::emitIsDebuggeeCheck() {
  if (handler.compileDebugInstrumentation()) {
    EmitCallFrameIsDebuggeeCheck(masm);
  }
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emitIsDebuggeeCheck() {
  // Use a toggled jump to call FrameIsDebuggeeCheck only if the debugger is
  // enabled.
  //
  // TODO(bug 1522394): consider having a cx->realm->isDebuggee guard before the
  // call. Consider moving the callWithABI out-of-line.

  Label skipCheck;
  CodeOffset toggleOffset = masm.toggledJump(&skipCheck);
  {
    saveInterpreterPCReg();
    EmitCallFrameIsDebuggeeCheck(masm);
    restoreInterpreterPCReg();
  }
  masm.bind(&skipCheck);
  return handler.addDebugInstrumentationOffset(cx, toggleOffset);
}

static void MaybeIncrementCodeCoverageCounter(MacroAssembler& masm,
                                              JSScript* script,
                                              jsbytecode* pc) {
  if (!script->hasScriptCounts()) {
    return;
  }
  PCCounts* counts = script->maybeGetPCCounts(pc);
  uint64_t* counterAddr = &counts->numExec();
  masm.inc64(AbsoluteAddress(counterAddr));
}

template <>
bool BaselineCompilerCodeGen::emitHandleCodeCoverageAtPrologue() {
  // If the main instruction is not a jump target, then we emit the
  // corresponding code coverage counter.
  JSScript* script = handler.script();
  jsbytecode* main = script->main();
  if (!BytecodeIsJumpTarget(JSOp(*main))) {
    MaybeIncrementCodeCoverageCounter(masm, script, main);
  }
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emitHandleCodeCoverageAtPrologue() {
  Label skipCoverage;
  CodeOffset toggleOffset = masm.toggledJump(&skipCoverage);
  masm.call(handler.codeCoverageAtPrologueLabel());
  masm.bind(&skipCoverage);
  return handler.codeCoverageOffsets().append(toggleOffset.offset());
}

template <>
void BaselineCompilerCodeGen::subtractScriptSlotsSize(Register reg,
                                                      Register scratch) {
  uint32_t slotsSize = handler.script()->nslots() * sizeof(Value);
  masm.subPtr(Imm32(slotsSize), reg);
}

template <>
void BaselineInterpreterCodeGen::subtractScriptSlotsSize(Register reg,
                                                         Register scratch) {
  // reg = reg - script->nslots() * sizeof(Value)
  MOZ_ASSERT(reg != scratch);
  loadScript(scratch);
  masm.loadPtr(Address(scratch, JSScript::offsetOfSharedData()), scratch);
  masm.loadPtr(Address(scratch, SharedImmutableScriptData::offsetOfISD()),
               scratch);
  masm.load32(Address(scratch, ImmutableScriptData::offsetOfNslots()), scratch);
  static_assert(sizeof(Value) == 8,
                "shift by 3 below assumes Value is 8 bytes");
  masm.lshiftPtr(Imm32(3), scratch);
  masm.subPtr(scratch, reg);
}

template <>
void BaselineCompilerCodeGen::loadGlobalLexicalEnvironment(Register dest) {
  MOZ_ASSERT(!handler.script()->hasNonSyntacticScope());
  masm.movePtr(ImmGCPtr(&cx->global()->lexicalEnvironment()), dest);
}

template <>
void BaselineInterpreterCodeGen::loadGlobalLexicalEnvironment(Register dest) {
  masm.loadGlobalObjectData(dest);
  masm.loadPtr(Address(dest, GlobalObjectData::offsetOfLexicalEnvironment()),
               dest);
}

template <>
void BaselineCompilerCodeGen::pushGlobalLexicalEnvironmentValue(
    ValueOperand scratch) {
  frame.push(ObjectValue(cx->global()->lexicalEnvironment()));
}

template <>
void BaselineInterpreterCodeGen::pushGlobalLexicalEnvironmentValue(
    ValueOperand scratch) {
  loadGlobalLexicalEnvironment(scratch.scratchReg());
  masm.tagValue(JSVAL_TYPE_OBJECT, scratch.scratchReg(), scratch);
  frame.push(scratch);
}

template <>
void BaselineCompilerCodeGen::loadGlobalThisValue(ValueOperand dest) {
  JSObject* thisObj = cx->global()->lexicalEnvironment().thisObject();
  masm.moveValue(ObjectValue(*thisObj), dest);
}

template <>
void BaselineInterpreterCodeGen::loadGlobalThisValue(ValueOperand dest) {
  Register scratch = dest.scratchReg();
  loadGlobalLexicalEnvironment(scratch);
  static constexpr size_t SlotOffset =
      GlobalLexicalEnvironmentObject::offsetOfThisValueSlot();
  masm.loadValue(Address(scratch, SlotOffset), dest);
}

template <>
void BaselineCompilerCodeGen::pushScriptArg() {
  pushArg(ImmGCPtr(handler.script()));
}

template <>
void BaselineInterpreterCodeGen::pushScriptArg() {
  pushArg(frame.addressOfInterpreterScript());
}

template <>
void BaselineCompilerCodeGen::pushBytecodePCArg() {
  pushArg(ImmPtr(handler.pc()));
}

template <>
void BaselineInterpreterCodeGen::pushBytecodePCArg() {
  if (HasInterpreterPCReg()) {
    pushArg(InterpreterPCReg);
  } else {
    pushArg(frame.addressOfInterpreterPC());
  }
}

static gc::Cell* GetScriptGCThing(JSScript* script, jsbytecode* pc,
                                  ScriptGCThingType type) {
  switch (type) {
    case ScriptGCThingType::Atom:
      return script->getAtom(pc);
    case ScriptGCThingType::String:
      return script->getString(pc);
    case ScriptGCThingType::RegExp:
      return script->getRegExp(pc);
    case ScriptGCThingType::Object:
      return script->getObject(pc);
    case ScriptGCThingType::Function:
      return script->getFunction(pc);
    case ScriptGCThingType::Scope:
      return script->getScope(pc);
    case ScriptGCThingType::BigInt:
      return script->getBigInt(pc);
  }
  MOZ_CRASH("Unexpected GCThing type");
}

template <>
void BaselineCompilerCodeGen::loadScriptGCThing(ScriptGCThingType type,
                                                Register dest,
                                                Register scratch) {
  gc::Cell* thing = GetScriptGCThing(handler.script(), handler.pc(), type);
  masm.movePtr(ImmGCPtr(thing), dest);
}

template <>
void BaselineInterpreterCodeGen::loadScriptGCThing(ScriptGCThingType type,
                                                   Register dest,
                                                   Register scratch) {
  MOZ_ASSERT(dest != scratch);

  // Load the index in |scratch|.
  LoadInt32Operand(masm, scratch);

  // Load the GCCellPtr.
  loadScript(dest);
  masm.loadPtr(Address(dest, JSScript::offsetOfPrivateData()), dest);
  masm.loadPtr(BaseIndex(dest, scratch, ScalePointer,
                         PrivateScriptData::offsetOfGCThings()),
               dest);

  // Clear the tag bits.
  switch (type) {
    case ScriptGCThingType::Atom:
    case ScriptGCThingType::String:
      // Use xorPtr with a 32-bit immediate because it's more efficient than
      // andPtr on 64-bit.
      static_assert(uintptr_t(TraceKind::String) == 2,
                    "Unexpected tag bits for string GCCellPtr");
      masm.xorPtr(Imm32(2), dest);
      break;
    case ScriptGCThingType::RegExp:
    case ScriptGCThingType::Object:
    case ScriptGCThingType::Function:
      // No-op because GCCellPtr tag bits are zero for objects.
      static_assert(uintptr_t(TraceKind::Object) == 0,
                    "Unexpected tag bits for object GCCellPtr");
      break;
    case ScriptGCThingType::BigInt:
      // Use xorPtr with a 32-bit immediate because it's more efficient than
      // andPtr on 64-bit.
      static_assert(uintptr_t(TraceKind::BigInt) == 1,
                    "Unexpected tag bits for BigInt GCCellPtr");
      masm.xorPtr(Imm32(1), dest);
      break;
    case ScriptGCThingType::Scope:
      // Use xorPtr with a 32-bit immediate because it's more efficient than
      // andPtr on 64-bit.
      static_assert(uintptr_t(TraceKind::Scope) >= JS::OutOfLineTraceKindMask,
                    "Expected Scopes to have OutOfLineTraceKindMask tag");
      masm.xorPtr(Imm32(JS::OutOfLineTraceKindMask), dest);
      break;
  }

#ifdef DEBUG
  // Assert low bits are not set.
  Label ok;
  masm.branchTestPtr(Assembler::Zero, dest, Imm32(0b111), &ok);
  masm.assumeUnreachable("GC pointer with tag bits set");
  masm.bind(&ok);
#endif
}

template <>
void BaselineCompilerCodeGen::pushScriptGCThingArg(ScriptGCThingType type,
                                                   Register scratch1,
                                                   Register scratch2) {
  gc::Cell* thing = GetScriptGCThing(handler.script(), handler.pc(), type);
  pushArg(ImmGCPtr(thing));
}

template <>
void BaselineInterpreterCodeGen::pushScriptGCThingArg(ScriptGCThingType type,
                                                      Register scratch1,
                                                      Register scratch2) {
  loadScriptGCThing(type, scratch1, scratch2);
  pushArg(scratch1);
}

template <typename Handler>
void BaselineCodeGen<Handler>::pushScriptNameArg(Register scratch1,
                                                 Register scratch2) {
  pushScriptGCThingArg(ScriptGCThingType::Atom, scratch1, scratch2);
}

template <>
void BaselineCompilerCodeGen::pushUint8BytecodeOperandArg(Register) {
  MOZ_ASSERT(JOF_OPTYPE(JSOp(*handler.pc())) == JOF_UINT8);
  pushArg(Imm32(GET_UINT8(handler.pc())));
}

template <>
void BaselineInterpreterCodeGen::pushUint8BytecodeOperandArg(Register scratch) {
  LoadUint8Operand(masm, scratch);
  pushArg(scratch);
}

template <>
void BaselineCompilerCodeGen::pushUint16BytecodeOperandArg(Register) {
  MOZ_ASSERT(JOF_OPTYPE(JSOp(*handler.pc())) == JOF_UINT16);
  pushArg(Imm32(GET_UINT16(handler.pc())));
}

template <>
void BaselineInterpreterCodeGen::pushUint16BytecodeOperandArg(
    Register scratch) {
  LoadUint16Operand(masm, scratch);
  pushArg(scratch);
}

template <>
void BaselineCompilerCodeGen::loadInt32LengthBytecodeOperand(Register dest) {
  uint32_t length = GET_UINT32(handler.pc());
  MOZ_ASSERT(length <= INT32_MAX,
             "the bytecode emitter must fail to compile code that would "
             "produce a length exceeding int32_t range");
  masm.move32(Imm32(AssertedCast<int32_t>(length)), dest);
}

template <>
void BaselineInterpreterCodeGen::loadInt32LengthBytecodeOperand(Register dest) {
  LoadInt32Operand(masm, dest);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitDebugPrologue() {
  auto ifDebuggee = [this]() {
    // Load pointer to BaselineFrame in R0.
    masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());

    prepareVMCall();
    pushArg(R0.scratchReg());

    const RetAddrEntry::Kind kind = RetAddrEntry::Kind::DebugPrologue;

    using Fn = bool (*)(JSContext*, BaselineFrame*);
    if (!callVM<Fn, jit::DebugPrologue>(kind)) {
      return false;
    }

    return true;
  };
  return emitDebugInstrumentation(ifDebuggee);
}

template <>
void BaselineCompilerCodeGen::emitInitFrameFields(Register nonFunctionEnv) {
  Register scratch = R0.scratchReg();
  Register scratch2 = R2.scratchReg();
  MOZ_ASSERT(nonFunctionEnv != scratch && nonFunctionEnv != scratch2);

  masm.store32(Imm32(0), frame.addressOfFlags());
  if (handler.function()) {
    masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(), scratch);
    masm.unboxObject(Address(scratch, JSFunction::offsetOfEnvironment()),
                     scratch);
    masm.storePtr(scratch, frame.addressOfEnvironmentChain());
  } else {
    masm.storePtr(nonFunctionEnv, frame.addressOfEnvironmentChain());
  }

  // If cx->inlinedICScript contains an inlined ICScript (passed from
  // the caller), take that ICScript and store it in the frame, then
  // overwrite cx->inlinedICScript with nullptr.
  Label notInlined, done;
  masm.movePtr(ImmPtr(cx->addressOfInlinedICScript()), scratch);
  Address inlinedAddr(scratch, 0);
  masm.branchPtr(Assembler::Equal, inlinedAddr, ImmWord(0), &notInlined);
  masm.loadPtr(inlinedAddr, scratch2);
  masm.storePtr(scratch2, frame.addressOfICScript());
  masm.storePtr(ImmPtr(nullptr), inlinedAddr);
  masm.jump(&done);

  // Otherwise, store this script's default ICSCript in the frame.
  masm.bind(&notInlined);
  masm.storePtr(ImmPtr(handler.script()->jitScript()->icScript()),
                frame.addressOfICScript());
  masm.bind(&done);
}

template <>
void BaselineInterpreterCodeGen::emitInitFrameFields(Register nonFunctionEnv) {
  MOZ_ASSERT(nonFunctionEnv == R1.scratchReg(),
             "Don't clobber nonFunctionEnv below");

  // If we have a dedicated PC register we use it as scratch1 to avoid a
  // register move below.
  Register scratch1 =
      HasInterpreterPCReg() ? InterpreterPCReg : R0.scratchReg();
  Register scratch2 = R2.scratchReg();

  masm.store32(Imm32(BaselineFrame::RUNNING_IN_INTERPRETER),
               frame.addressOfFlags());

  // Initialize interpreterScript.
  Label notFunction, done;
  masm.loadPtr(frame.addressOfCalleeToken(), scratch1);
  masm.branchTestPtr(Assembler::NonZero, scratch1, Imm32(CalleeTokenScriptBit),
                     &notFunction);
  {
    // CalleeToken_Function or CalleeToken_FunctionConstructing.
    masm.andPtr(Imm32(uint32_t(CalleeTokenMask)), scratch1);
    masm.unboxObject(Address(scratch1, JSFunction::offsetOfEnvironment()),
                     scratch2);
    masm.storePtr(scratch2, frame.addressOfEnvironmentChain());
    masm.loadPrivate(Address(scratch1, JSFunction::offsetOfJitInfoOrScript()),
                     scratch1);
    masm.jump(&done);
  }
  masm.bind(&notFunction);
  {
    // CalleeToken_Script.
    masm.andPtr(Imm32(uint32_t(CalleeTokenMask)), scratch1);
    masm.storePtr(nonFunctionEnv, frame.addressOfEnvironmentChain());
  }
  masm.bind(&done);
  masm.storePtr(scratch1, frame.addressOfInterpreterScript());

  // Initialize icScript and interpreterICEntry
  masm.loadJitScript(scratch1, scratch2);
  masm.computeEffectiveAddress(Address(scratch2, JitScript::offsetOfICScript()),
                               scratch2);
  masm.storePtr(scratch2, frame.addressOfICScript());
  masm.computeEffectiveAddress(Address(scratch2, ICScript::offsetOfICEntries()),
                               scratch2);
  masm.storePtr(scratch2, frame.addressOfInterpreterICEntry());

  // Initialize interpreter pc.
  masm.loadPtr(Address(scratch1, JSScript::offsetOfSharedData()), scratch1);
  masm.loadPtr(Address(scratch1, SharedImmutableScriptData::offsetOfISD()),
               scratch1);
  masm.addPtr(Imm32(ImmutableScriptData::offsetOfCode()), scratch1);

  if (HasInterpreterPCReg()) {
    MOZ_ASSERT(scratch1 == InterpreterPCReg,
               "pc must be stored in the pc register");
  } else {
    masm.storePtr(scratch1, frame.addressOfInterpreterPC());
  }
}

// Assert we don't need a post write barrier to write sourceObj to a slot of
// destObj. See comments in WarpBuilder::buildNamedLambdaEnv.
static void AssertCanElidePostWriteBarrier(MacroAssembler& masm,
                                           Register destObj, Register sourceObj,
                                           Register temp) {
#ifdef DEBUG
  Label ok;
  masm.branchPtrInNurseryChunk(Assembler::Equal, destObj, temp, &ok);
  masm.branchPtrInNurseryChunk(Assembler::NotEqual, sourceObj, temp, &ok);
  masm.assumeUnreachable("Unexpected missing post write barrier in Baseline");
  masm.bind(&ok);
#endif
}

template <>
bool BaselineCompilerCodeGen::initEnvironmentChain() {
  if (!handler.function()) {
    return true;
  }
  if (!handler.script()->needsFunctionEnvironmentObjects()) {
    return true;
  }

  // Allocate a NamedLambdaObject and/or a CallObject. If the function needs
  // both, the NamedLambdaObject must enclose the CallObject. If one of the
  // allocations fails, we perform the whole operation in C++.

  JSObject* templateEnv = handler.script()->jitScript()->templateEnvironment();
  MOZ_ASSERT(templateEnv);

  CallObject* callObjectTemplate = nullptr;
  if (handler.function()->needsCallObject()) {
    callObjectTemplate = &templateEnv->as<CallObject>();
  }

  NamedLambdaObject* namedLambdaTemplate = nullptr;
  if (handler.function()->needsNamedLambdaEnvironment()) {
    if (callObjectTemplate) {
      templateEnv = templateEnv->enclosingEnvironment();
    }
    namedLambdaTemplate = &templateEnv->as<NamedLambdaObject>();
  }

  MOZ_ASSERT(namedLambdaTemplate || callObjectTemplate);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  Register newEnv = regs.takeAny();
  Register enclosingEnv = regs.takeAny();
  Register callee = regs.takeAny();
  Register temp = regs.takeAny();

  Label fail;
  masm.loadPtr(frame.addressOfEnvironmentChain(), enclosingEnv);
  masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(), callee);

  // Allocate a NamedLambdaObject if needed.
  if (namedLambdaTemplate) {
    TemplateObject templateObject(namedLambdaTemplate);
    masm.createGCObject(newEnv, temp, templateObject, gc::Heap::Default, &fail);

    // Store enclosing environment.
    Address enclosingSlot(newEnv,
                          NamedLambdaObject::offsetOfEnclosingEnvironment());
    masm.storeValue(JSVAL_TYPE_OBJECT, enclosingEnv, enclosingSlot);
    AssertCanElidePostWriteBarrier(masm, newEnv, enclosingEnv, temp);

    // Store callee.
    Address lambdaSlot(newEnv, NamedLambdaObject::offsetOfLambdaSlot());
    masm.storeValue(JSVAL_TYPE_OBJECT, callee, lambdaSlot);
    AssertCanElidePostWriteBarrier(masm, newEnv, callee, temp);

    if (callObjectTemplate) {
      masm.movePtr(newEnv, enclosingEnv);
    }
  }

  // Allocate a CallObject if needed.
  if (callObjectTemplate) {
    TemplateObject templateObject(callObjectTemplate);
    masm.createGCObject(newEnv, temp, templateObject, gc::Heap::Default, &fail);

    // Store enclosing environment.
    Address enclosingSlot(newEnv, CallObject::offsetOfEnclosingEnvironment());
    masm.storeValue(JSVAL_TYPE_OBJECT, enclosingEnv, enclosingSlot);
    AssertCanElidePostWriteBarrier(masm, newEnv, enclosingEnv, temp);

    // Store callee.
    Address calleeSlot(newEnv, CallObject::offsetOfCallee());
    masm.storeValue(JSVAL_TYPE_OBJECT, callee, calleeSlot);
    AssertCanElidePostWriteBarrier(masm, newEnv, callee, temp);
  }

  // Update the frame's environment chain and mark it initialized.
  Label done;
  masm.storePtr(newEnv, frame.addressOfEnvironmentChain());
  masm.or32(Imm32(BaselineFrame::HAS_INITIAL_ENV), frame.addressOfFlags());
  masm.jump(&done);

  masm.bind(&fail);

  prepareVMCall();

  masm.loadBaselineFramePtr(FramePointer, temp);
  pushArg(temp);

  const CallVMPhase phase = CallVMPhase::BeforePushingLocals;

  using Fn = bool (*)(JSContext*, BaselineFrame*);
  if (!callVMNonOp<Fn, jit::InitFunctionEnvironmentObjects>(phase)) {
    return false;
  }

  masm.bind(&done);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::initEnvironmentChain() {
  // For function scripts, call InitFunctionEnvironmentObjects if needed. For
  // non-function scripts this is a no-op.

  Label done;
  masm.branchTestPtr(Assembler::NonZero, frame.addressOfCalleeToken(),
                     Imm32(CalleeTokenScriptBit), &done);
  {
    auto initEnv = [this]() {
      // Call into the VM to create the proper environment objects.
      prepareVMCall();

      masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
      pushArg(R0.scratchReg());

      const CallVMPhase phase = CallVMPhase::BeforePushingLocals;

      using Fn = bool (*)(JSContext*, BaselineFrame*);
      return callVMNonOp<Fn, jit::InitFunctionEnvironmentObjects>(phase);
    };
    if (!emitTestScriptFlag(
            JSScript::ImmutableFlags::NeedsFunctionEnvironmentObjects, true,
            initEnv, R2.scratchReg())) {
      return false;
    }
  }

  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitInterruptCheck() {
  frame.syncStack(0);

  Label done;
  masm.branch32(Assembler::Equal, AbsoluteAddress(cx->addressOfInterruptBits()),
                Imm32(0), &done);

  prepareVMCall();

  // Use a custom RetAddrEntry::Kind so DebugModeOSR can distinguish this call
  // from other callVMs that might happen at this pc.
  const RetAddrEntry::Kind kind = RetAddrEntry::Kind::InterruptCheck;

  using Fn = bool (*)(JSContext*);
  if (!callVM<Fn, InterruptCheck>(kind)) {
    return false;
  }

  masm.bind(&done);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emitWarmUpCounterIncrement() {
  frame.assertSyncedStack();

  // Record native code offset for OSR from Baseline Interpreter into Baseline
  // JIT code. This is right before the warm-up check in the Baseline JIT code,
  // to make sure we can immediately enter Ion if the script is warm enough or
  // if --ion-eager is used.
  JSScript* script = handler.script();
  jsbytecode* pc = handler.pc();
  if (JSOp(*pc) == JSOp::LoopHead) {
    uint32_t pcOffset = script->pcToOffset(pc);
    uint32_t nativeOffset = masm.currentOffset();
    if (!handler.osrEntries().emplaceBack(pcOffset, nativeOffset)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Emit no warm-up counter increments if Ion is not enabled or if the script
  // will never be Ion-compileable.
  if (!handler.maybeIonCompileable()) {
    return true;
  }

  Register scriptReg = R2.scratchReg();
  Register countReg = R0.scratchReg();

  // Load the ICScript* in scriptReg.
  masm.loadPtr(frame.addressOfICScript(), scriptReg);

  // Bump warm-up counter.
  Address warmUpCounterAddr(scriptReg, ICScript::offsetOfWarmUpCount());
  masm.load32(warmUpCounterAddr, countReg);
  masm.add32(Imm32(1), countReg);
  masm.store32(countReg, warmUpCounterAddr);

  if (!JitOptions.disableInlining) {
    // Consider trial inlining.
    // Note: unlike other warmup thresholds, where we try to enter a
    // higher tier whenever we are higher than a given warmup count,
    // trial inlining triggers once when reaching the threshold.
    Label noTrialInlining;
    masm.branch32(Assembler::NotEqual, countReg,
                  Imm32(JitOptions.trialInliningWarmUpThreshold),
                  &noTrialInlining);
    prepareVMCall();

    masm.PushBaselineFramePtr(FramePointer, R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*);
    if (!callVMNonOp<Fn, DoTrialInlining>()) {
      return false;
    }
    // Reload registers potentially clobbered by the call.
    masm.loadPtr(frame.addressOfICScript(), scriptReg);
    masm.load32(warmUpCounterAddr, countReg);
    masm.bind(&noTrialInlining);
  }

  if (JSOp(*pc) == JSOp::LoopHead) {
    // If this is a loop where we can't OSR (for example because it's inside a
    // catch or finally block), increment the warmup counter but don't attempt
    // OSR (Ion/Warp only compiles the try block).
    if (!handler.analysis().info(pc).loopHeadCanOsr) {
      return true;
    }
  }

  Label done;

  const OptimizationInfo* info =
      IonOptimizations.get(OptimizationLevel::Normal);
  uint32_t warmUpThreshold = info->compilerWarmUpThreshold(cx, script, pc);
  masm.branch32(Assembler::LessThan, countReg, Imm32(warmUpThreshold), &done);

  // Don't trigger Warp compilations from trial-inlined scripts.
  Address depthAddr(scriptReg, ICScript::offsetOfDepth());
  masm.branch32(Assembler::NotEqual, depthAddr, Imm32(0), &done);

  // Load the IonScript* in scriptReg. We can load this from the ICScript*
  // because it must be an outer ICScript embedded in the JitScript.
  constexpr int32_t offset = -int32_t(JitScript::offsetOfICScript()) +
                             int32_t(JitScript::offsetOfIonScript());
  masm.loadPtr(Address(scriptReg, offset), scriptReg);

  // Do nothing if Ion is already compiling this script off-thread or if Ion has
  // been disabled for this script.
  masm.branchPtr(Assembler::Equal, scriptReg, ImmPtr(IonCompilingScriptPtr),
                 &done);
  masm.branchPtr(Assembler::Equal, scriptReg, ImmPtr(IonDisabledScriptPtr),
                 &done);

  // Try to compile and/or finish a compilation.
  if (JSOp(*pc) == JSOp::LoopHead) {
    // Try to OSR into Ion.
    computeFrameSize(R0.scratchReg());

    prepareVMCall();

    pushBytecodePCArg();
    pushArg(R0.scratchReg());
    masm.PushBaselineFramePtr(FramePointer, R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, uint32_t, jsbytecode*,
                        IonOsrTempData**);
    if (!callVM<Fn, IonCompileScriptForBaselineOSR>()) {
      return false;
    }

    // The return register holds the IonOsrTempData*. Perform OSR if it's not
    // nullptr.
    static_assert(ReturnReg != OsrFrameReg,
                  "Code below depends on osrDataReg != OsrFrameReg");
    Register osrDataReg = ReturnReg;
    masm.branchTestPtr(Assembler::Zero, osrDataReg, osrDataReg, &done);

    // Success! Switch from Baseline JIT code to Ion JIT code.

    // At this point, stack looks like:
    //
    //  +-> [...Calling-Frame...]
    //  |   [...Actual-Args/ThisV/ArgCount/Callee...]
    //  |   [Descriptor]
    //  |   [Return-Addr]
    //  +---[Saved-FramePtr]
    //      [...Baseline-Frame...]

#ifdef DEBUG
    // Get a scratch register that's not osrDataReg or OsrFrameReg.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    MOZ_ASSERT(!regs.has(FramePointer));
    regs.take(osrDataReg);
    regs.take(OsrFrameReg);

    Register scratchReg = regs.takeAny();

    // If profiler instrumentation is on, ensure that lastProfilingFrame is
    // the frame currently being OSR-ed
    {
      Label checkOk;
      AbsoluteAddress addressOfEnabled(
          cx->runtime()->geckoProfiler().addressOfEnabled());
      masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &checkOk);
      masm.loadPtr(AbsoluteAddress((void*)&cx->jitActivation), scratchReg);
      masm.loadPtr(
          Address(scratchReg, JitActivation::offsetOfLastProfilingFrame()),
          scratchReg);

      // It may be the case that we entered the baseline frame with
      // profiling turned off on, then in a call within a loop (i.e. a
      // callee frame), turn on profiling, then return to this frame,
      // and then OSR with profiling turned on.  In this case, allow for
      // lastProfilingFrame to be null.
      masm.branchPtr(Assembler::Equal, scratchReg, ImmWord(0), &checkOk);

      masm.branchPtr(Assembler::Equal, FramePointer, scratchReg, &checkOk);
      masm.assumeUnreachable("Baseline OSR lastProfilingFrame mismatch.");
      masm.bind(&checkOk);
    }
#endif

    // Restore the stack pointer so that the saved frame pointer is on top of
    // the stack.
    masm.moveToStackPtr(FramePointer);

    // Jump into Ion.
    masm.loadPtr(Address(osrDataReg, IonOsrTempData::offsetOfBaselineFrame()),
                 OsrFrameReg);
    masm.jump(Address(osrDataReg, IonOsrTempData::offsetOfJitCode()));
  } else {
    prepareVMCall();

    masm.PushBaselineFramePtr(FramePointer, R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*);
    if (!callVMNonOp<Fn, IonCompileScriptForBaselineAtEntry>()) {
      return false;
    }
  }

  masm.bind(&done);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emitWarmUpCounterIncrement() {
  Register scriptReg = R2.scratchReg();
  Register countReg = R0.scratchReg();

  // Load the JitScript* in scriptReg.
  loadScript(scriptReg);
  masm.loadJitScript(scriptReg, scriptReg);

  // Bump warm-up counter.
  Address warmUpCounterAddr(scriptReg, JitScript::offsetOfWarmUpCount());
  masm.load32(warmUpCounterAddr, countReg);
  masm.add32(Imm32(1), countReg);
  masm.store32(countReg, warmUpCounterAddr);

  // If the script is warm enough for Baseline compilation, call into the VM to
  // compile it.
  Label done;
  masm.branch32(Assembler::BelowOrEqual, countReg,
                Imm32(JitOptions.baselineJitWarmUpThreshold), &done);
  masm.branchPtr(Assembler::Equal,
                 Address(scriptReg, JitScript::offsetOfBaselineScript()),
                 ImmPtr(BaselineDisabledScriptPtr), &done);
  {
    prepareVMCall();

    masm.PushBaselineFramePtr(FramePointer, R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, uint8_t**);
    if (!callVM<Fn, BaselineCompileFromBaselineInterpreter>()) {
      return false;
    }

    // If the function returned nullptr we either skipped compilation or were
    // unable to compile the script. Continue running in the interpreter.
    masm.branchTestPtr(Assembler::Zero, ReturnReg, ReturnReg, &done);

    // Success! Switch from interpreter to JIT code by jumping to the
    // corresponding code in the BaselineScript.
    //
    // This works because BaselineCompiler uses the same frame layout (stack is
    // synced at OSR points) and BaselineCompileFromBaselineInterpreter has
    // already cleared the RUNNING_IN_INTERPRETER flag for us.
    // See BaselineFrame::prepareForBaselineInterpreterToJitOSR.
    masm.jump(ReturnReg);
  }

  masm.bind(&done);
  return true;
}

bool BaselineCompiler::emitDebugTrap() {
  MOZ_ASSERT(compileDebugInstrumentation());
  MOZ_ASSERT(frame.numUnsyncedSlots() == 0);

  JSScript* script = handler.script();
  bool enabled = DebugAPI::stepModeEnabled(script) ||
                 DebugAPI::hasBreakpointsAt(script, handler.pc());

  // Emit patchable call to debug trap handler.
  JitCode* handlerCode = cx->runtime()->jitRuntime()->debugTrapHandler(
      cx, DebugTrapHandlerKind::Compiler);
  if (!handlerCode) {
    return false;
  }

  CodeOffset nativeOffset = masm.toggledCall(handlerCode, enabled);

  uint32_t pcOffset = script->pcToOffset(handler.pc());
  if (!debugTrapEntries_.emplaceBack(pcOffset, nativeOffset.offset())) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Add a RetAddrEntry for the return offset -> pc mapping.
  return handler.recordCallRetAddr(cx, RetAddrEntry::Kind::DebugTrap,
                                   masm.currentOffset());
}

template <typename Handler>
void BaselineCodeGen<Handler>::emitProfilerEnterFrame() {
  // Store stack position to lastProfilingFrame variable, guarded by a toggled
  // jump. Starts off initially disabled.
  Label noInstrument;
  CodeOffset toggleOffset = masm.toggledJump(&noInstrument);
  masm.profilerEnterFrame(FramePointer, R0.scratchReg());
  masm.bind(&noInstrument);

  // Store the start offset in the appropriate location.
  MOZ_ASSERT(!profilerEnterFrameToggleOffset_.bound());
  profilerEnterFrameToggleOffset_ = toggleOffset;
}

template <typename Handler>
void BaselineCodeGen<Handler>::emitProfilerExitFrame() {
  // Store previous frame to lastProfilingFrame variable, guarded by a toggled
  // jump. Starts off initially disabled.
  Label noInstrument;
  CodeOffset toggleOffset = masm.toggledJump(&noInstrument);
  masm.profilerExitFrame();
  masm.bind(&noInstrument);

  // Store the start offset in the appropriate location.
  MOZ_ASSERT(!profilerExitFrameToggleOffset_.bound());
  profilerExitFrameToggleOffset_ = toggleOffset;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Nop() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_NopDestructuring() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_NopIsAssignOp() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_TryDestructuring() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Pop() {
  frame.pop();
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_PopN() {
  frame.popn(GET_UINT16(handler.pc()));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_PopN() {
  LoadUint16Operand(masm, R0.scratchReg());
  frame.popn(R0.scratchReg());
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_DupAt() {
  frame.syncStack(0);

  // DupAt takes a value on the stack and re-pushes it on top.  It's like
  // GetLocal but it addresses from the top of the stack instead of from the
  // stack frame.

  int depth = -(GET_UINT24(handler.pc()) + 1);
  masm.loadValue(frame.addressOfStackValue(depth), R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_DupAt() {
  LoadUint24Operand(masm, 0, R0.scratchReg());
  masm.loadValue(frame.addressOfStackValue(R0.scratchReg()), R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Dup() {
  // Keep top stack value in R0, sync the rest so that we can use R1. We use
  // separate registers because every register can be used by at most one
  // StackValue.
  frame.popRegsAndSync(1);
  masm.moveValue(R0, R1);

  // inc/dec ops use Dup followed by Inc/Dec. Push R0 last to avoid a move.
  frame.push(R1);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Dup2() {
  frame.syncStack(0);

  masm.loadValue(frame.addressOfStackValue(-2), R0);
  masm.loadValue(frame.addressOfStackValue(-1), R1);

  frame.push(R0);
  frame.push(R1);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Swap() {
  // Keep top stack values in R0 and R1.
  frame.popRegsAndSync(2);

  frame.push(R1);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_Pick() {
  frame.syncStack(0);

  // Pick takes a value on the stack and moves it to the top.
  // For instance, pick 2:
  //     before: A B C D E
  //     after : A B D E C

  // First, move value at -(amount + 1) into R0.
  int32_t depth = -(GET_INT8(handler.pc()) + 1);
  masm.loadValue(frame.addressOfStackValue(depth), R0);

  // Move the other values down.
  depth++;
  for (; depth < 0; depth++) {
    Address source = frame.addressOfStackValue(depth);
    Address dest = frame.addressOfStackValue(depth - 1);
    masm.loadValue(source, R1);
    masm.storeValue(R1, dest);
  }

  // Push R0.
  frame.pop();
  frame.push(R0);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_Pick() {
  // First, move the value to move up into R0.
  Register scratch = R2.scratchReg();
  LoadUint8Operand(masm, scratch);
  masm.loadValue(frame.addressOfStackValue(scratch), R0);

  // Move the other values down.
  Label top, done;
  masm.bind(&top);
  masm.branchSub32(Assembler::Signed, Imm32(1), scratch, &done);
  {
    masm.loadValue(frame.addressOfStackValue(scratch), R1);
    masm.storeValue(R1, frame.addressOfStackValue(scratch, sizeof(Value)));
    masm.jump(&top);
  }

  masm.bind(&done);

  // Replace value on top of the stack with R0.
  masm.storeValue(R0, frame.addressOfStackValue(-1));
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_Unpick() {
  frame.syncStack(0);

  // Pick takes the top of the stack value and moves it under the nth value.
  // For instance, unpick 2:
  //     before: A B C D E
  //     after : A B E C D

  // First, move value at -1 into R0.
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  MOZ_ASSERT(GET_INT8(handler.pc()) > 0,
             "Interpreter code assumes JSOp::Unpick operand > 0");

  // Move the other values up.
  int32_t depth = -(GET_INT8(handler.pc()) + 1);
  for (int32_t i = -1; i > depth; i--) {
    Address source = frame.addressOfStackValue(i - 1);
    Address dest = frame.addressOfStackValue(i);
    masm.loadValue(source, R1);
    masm.storeValue(R1, dest);
  }

  // Store R0 under the nth value.
  Address dest = frame.addressOfStackValue(depth);
  masm.storeValue(R0, dest);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_Unpick() {
  Register scratch = R2.scratchReg();
  LoadUint8Operand(masm, scratch);

  // Move the top value into R0.
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  // Overwrite the nth stack value with R0 but first save the old value in R1.
  masm.loadValue(frame.addressOfStackValue(scratch), R1);
  masm.storeValue(R0, frame.addressOfStackValue(scratch));

  // Now for each slot x in [n-1, 1] do the following:
  //
  // * Store the value in slot x in R0.
  // * Store the value in the previous slot (now in R1) in slot x.
  // * Move R0 to R1.

#ifdef DEBUG
  // Assert the operand > 0 so the branchSub32 below doesn't "underflow" to
  // negative values.
  {
    Label ok;
    masm.branch32(Assembler::GreaterThan, scratch, Imm32(0), &ok);
    masm.assumeUnreachable("JSOp::Unpick with operand <= 0?");
    masm.bind(&ok);
  }
#endif

  Label top, done;
  masm.bind(&top);
  masm.branchSub32(Assembler::Zero, Imm32(1), scratch, &done);
  {
    // Overwrite stack slot x with slot x + 1, saving the old value in R1.
    masm.loadValue(frame.addressOfStackValue(scratch), R0);
    masm.storeValue(R1, frame.addressOfStackValue(scratch));
    masm.moveValue(R0, R1);
    masm.jump(&top);
  }

  // Finally, replace the value on top of the stack (slot 0) with R1. This is
  // the value that used to be in slot 1.
  masm.bind(&done);
  masm.storeValue(R1, frame.addressOfStackValue(-1));
  return true;
}

template <>
void BaselineCompilerCodeGen::emitJump() {
  jsbytecode* pc = handler.pc();
  MOZ_ASSERT(IsJumpOpcode(JSOp(*pc)));
  frame.assertSyncedStack();

  jsbytecode* target = pc + GET_JUMP_OFFSET(pc);
  masm.jump(handler.labelOf(target));
}

template <>
void BaselineInterpreterCodeGen::emitJump() {
  // We have to add the current pc's jump offset to the current pc. We can use
  // R0 and R1 as scratch because we jump to the "next op" label so these
  // registers aren't in use at this point.
  Register scratch1 = R0.scratchReg();
  Register scratch2 = R1.scratchReg();
  Register pc = LoadBytecodePC(masm, scratch1);
  LoadInt32OperandSignExtendToPtr(masm, pc, scratch2);
  if (HasInterpreterPCReg()) {
    masm.addPtr(scratch2, InterpreterPCReg);
  } else {
    masm.addPtr(pc, scratch2);
    masm.storePtr(scratch2, frame.addressOfInterpreterPC());
  }
  masm.jump(handler.interpretOpWithPCRegLabel());
}

template <>
void BaselineCompilerCodeGen::emitTestBooleanTruthy(bool branchIfTrue,
                                                    ValueOperand val) {
  jsbytecode* pc = handler.pc();
  MOZ_ASSERT(IsJumpOpcode(JSOp(*pc)));
  frame.assertSyncedStack();

  jsbytecode* target = pc + GET_JUMP_OFFSET(pc);
  masm.branchTestBooleanTruthy(branchIfTrue, val, handler.labelOf(target));
}

template <>
void BaselineInterpreterCodeGen::emitTestBooleanTruthy(bool branchIfTrue,
                                                       ValueOperand val) {
  Label done;
  masm.branchTestBooleanTruthy(!branchIfTrue, val, &done);
  emitJump();
  masm.bind(&done);
}

template <>
template <typename F1, typename F2>
[[nodiscard]] bool BaselineCompilerCodeGen::emitTestScriptFlag(
    JSScript::ImmutableFlags flag, const F1& ifSet, const F2& ifNotSet,
    Register scratch) {
  if (handler.script()->hasFlag(flag)) {
    return ifSet();
  }
  return ifNotSet();
}

template <>
template <typename F1, typename F2>
[[nodiscard]] bool BaselineInterpreterCodeGen::emitTestScriptFlag(
    JSScript::ImmutableFlags flag, const F1& ifSet, const F2& ifNotSet,
    Register scratch) {
  Label flagNotSet, done;
  loadScript(scratch);
  masm.branchTest32(Assembler::Zero,
                    Address(scratch, JSScript::offsetOfImmutableFlags()),
                    Imm32(uint32_t(flag)), &flagNotSet);
  {
    if (!ifSet()) {
      return false;
    }
    masm.jump(&done);
  }
  masm.bind(&flagNotSet);
  {
    if (!ifNotSet()) {
      return false;
    }
  }

  masm.bind(&done);
  return true;
}

template <>
template <typename F>
[[nodiscard]] bool BaselineCompilerCodeGen::emitTestScriptFlag(
    JSScript::ImmutableFlags flag, bool value, const F& emit,
    Register scratch) {
  if (handler.script()->hasFlag(flag) == value) {
    return emit();
  }
  return true;
}

template <>
template <typename F>
[[nodiscard]] bool BaselineCompilerCodeGen::emitTestScriptFlag(
    JSScript::MutableFlags flag, bool value, const F& emit, Register scratch) {
  if (handler.script()->hasFlag(flag) == value) {
    return emit();
  }
  return true;
}

template <>
template <typename F>
[[nodiscard]] bool BaselineInterpreterCodeGen::emitTestScriptFlag(
    JSScript::ImmutableFlags flag, bool value, const F& emit,
    Register scratch) {
  Label done;
  loadScript(scratch);
  masm.branchTest32(value ? Assembler::Zero : Assembler::NonZero,
                    Address(scratch, JSScript::offsetOfImmutableFlags()),
                    Imm32(uint32_t(flag)), &done);
  {
    if (!emit()) {
      return false;
    }
  }

  masm.bind(&done);
  return true;
}

template <>
template <typename F>
[[nodiscard]] bool BaselineInterpreterCodeGen::emitTestScriptFlag(
    JSScript::MutableFlags flag, bool value, const F& emit, Register scratch) {
  Label done;
  loadScript(scratch);
  masm.branchTest32(value ? Assembler::Zero : Assembler::NonZero,
                    Address(scratch, JSScript::offsetOfMutableFlags()),
                    Imm32(uint32_t(flag)), &done);
  {
    if (!emit()) {
      return false;
    }
  }

  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Goto() {
  frame.syncStack(0);
  emitJump();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitTest(bool branchIfTrue) {
  bool knownBoolean = frame.stackValueHasKnownType(-1, JSVAL_TYPE_BOOLEAN);

  // Keep top stack value in R0.
  frame.popRegsAndSync(1);

  if (!knownBoolean && !emitNextIC()) {
    return false;
  }

  // IC will leave a BooleanValue in R0, just need to branch on it.
  emitTestBooleanTruthy(branchIfTrue, R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JumpIfFalse() {
  return emitTest(false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JumpIfTrue() {
  return emitTest(true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitAndOr(bool branchIfTrue) {
  bool knownBoolean = frame.stackValueHasKnownType(-1, JSVAL_TYPE_BOOLEAN);

  // And and Or leave the original value on the stack.
  frame.syncStack(0);

  masm.loadValue(frame.addressOfStackValue(-1), R0);
  if (!knownBoolean && !emitNextIC()) {
    return false;
  }

  emitTestBooleanTruthy(branchIfTrue, R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_And() {
  return emitAndOr(false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Or() {
  return emitAndOr(true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Coalesce() {
  // Coalesce leaves the original value on the stack.
  frame.syncStack(0);

  masm.loadValue(frame.addressOfStackValue(-1), R0);

  Label undefinedOrNull;

  masm.branchTestUndefined(Assembler::Equal, R0, &undefinedOrNull);
  masm.branchTestNull(Assembler::Equal, R0, &undefinedOrNull);
  emitJump();

  masm.bind(&undefinedOrNull);
  // fall through
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Not() {
  bool knownBoolean = frame.stackValueHasKnownType(-1, JSVAL_TYPE_BOOLEAN);

  // Keep top stack value in R0.
  frame.popRegsAndSync(1);

  if (!knownBoolean && !emitNextIC()) {
    return false;
  }

  masm.notBoolean(R0);

  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Pos() {
  return emitUnaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ToNumeric() {
  return emitUnaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_LoopHead() {
  if (!emit_JumpTarget()) {
    return false;
  }
  if (!emitInterruptCheck()) {
    return false;
  }
  if (!emitWarmUpCounterIncrement()) {
    return false;
  }
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Void() {
  frame.pop();
  frame.push(UndefinedValue());
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Undefined() {
  frame.push(UndefinedValue());
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Hole() {
  frame.push(MagicValue(JS_ELEMENTS_HOLE));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Null() {
  frame.push(NullValue());
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckIsObj() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  Label ok;
  masm.branchTestObject(Assembler::Equal, R0, &ok);

  prepareVMCall();

  pushUint8BytecodeOperandArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, CheckIsObjectKind);
  if (!callVM<Fn, ThrowCheckIsObject>()) {
    return false;
  }

  masm.bind(&ok);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckThis() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  return emitCheckThis(R0);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckThisReinit() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  return emitCheckThis(R0, /* reinit = */ true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitCheckThis(ValueOperand val, bool reinit) {
  Label thisOK;
  if (reinit) {
    masm.branchTestMagic(Assembler::Equal, val, &thisOK);
  } else {
    masm.branchTestMagic(Assembler::NotEqual, val, &thisOK);
  }

  prepareVMCall();

  if (reinit) {
    using Fn = bool (*)(JSContext*);
    if (!callVM<Fn, ThrowInitializedThis>()) {
      return false;
    }
  } else {
    using Fn = bool (*)(JSContext*);
    if (!callVM<Fn, ThrowUninitializedThis>()) {
      return false;
    }
  }

  masm.bind(&thisOK);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckReturn() {
  MOZ_ASSERT_IF(handler.maybeScript(),
                handler.maybeScript()->isDerivedClassConstructor());

  // Load |this| in R0, return value in R1.
  frame.popRegsAndSync(1);
  emitLoadReturnValue(R1);

  Label done, returnBad, checkThis;
  masm.branchTestObject(Assembler::NotEqual, R1, &checkThis);
  {
    masm.moveValue(R1, R0);
    masm.jump(&done);
  }
  masm.bind(&checkThis);
  masm.branchTestUndefined(Assembler::NotEqual, R1, &returnBad);
  masm.branchTestMagic(Assembler::NotEqual, R0, &done);
  masm.bind(&returnBad);

  prepareVMCall();
  pushArg(R1);

  using Fn = bool (*)(JSContext*, HandleValue);
  if (!callVM<Fn, ThrowBadDerivedReturnOrUninitializedThis>()) {
    return false;
  }
  masm.assumeUnreachable("Should throw on bad derived constructor return");

  masm.bind(&done);

  // Push |rval| or |this| onto the stack.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_FunctionThis() {
  MOZ_ASSERT_IF(handler.maybeFunction(), !handler.maybeFunction()->isArrow());

  frame.pushThis();

  auto boxThis = [this]() {
    // Load |thisv| in R0. Skip the call if it's already an object.
    Label skipCall;
    frame.popRegsAndSync(1);
    masm.branchTestObject(Assembler::Equal, R0, &skipCall);

    prepareVMCall();
    masm.loadBaselineFramePtr(FramePointer, R1.scratchReg());

    pushArg(R1.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, MutableHandleValue);
    if (!callVM<Fn, BaselineGetFunctionThis>()) {
      return false;
    }

    masm.bind(&skipCall);
    frame.push(R0);
    return true;
  };

  // In strict mode code, |this| is left alone.
  return emitTestScriptFlag(JSScript::ImmutableFlags::Strict, false, boxThis,
                            R2.scratchReg());
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GlobalThis() {
  frame.syncStack(0);

  loadGlobalThisValue(R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_NonSyntacticGlobalThis() {
  frame.syncStack(0);

  prepareVMCall();

  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());
  pushArg(R0.scratchReg());

  using Fn = void (*)(JSContext*, HandleObject, MutableHandleValue);
  if (!callVM<Fn, GetNonSyntacticGlobalThis>()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_True() {
  frame.push(BooleanValue(true));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_False() {
  frame.push(BooleanValue(false));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Zero() {
  frame.push(Int32Value(0));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_One() {
  frame.push(Int32Value(1));
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_Int8() {
  frame.push(Int32Value(GET_INT8(handler.pc())));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_Int8() {
  LoadInt8Operand(masm, R0.scratchReg());
  masm.tagValue(JSVAL_TYPE_INT32, R0.scratchReg(), R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_Int32() {
  frame.push(Int32Value(GET_INT32(handler.pc())));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_Int32() {
  LoadInt32Operand(masm, R0.scratchReg());
  masm.tagValue(JSVAL_TYPE_INT32, R0.scratchReg(), R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_Uint16() {
  frame.push(Int32Value(GET_UINT16(handler.pc())));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_Uint16() {
  LoadUint16Operand(masm, R0.scratchReg());
  masm.tagValue(JSVAL_TYPE_INT32, R0.scratchReg(), R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_Uint24() {
  frame.push(Int32Value(GET_UINT24(handler.pc())));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_Uint24() {
  LoadUint24Operand(masm, 0, R0.scratchReg());
  masm.tagValue(JSVAL_TYPE_INT32, R0.scratchReg(), R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_Double() {
  frame.push(GET_INLINE_VALUE(handler.pc()));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_Double() {
  LoadInlineValueOperand(masm, R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_BigInt() {
  BigInt* bi = handler.script()->getBigInt(handler.pc());
  frame.push(BigIntValue(bi));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_BigInt() {
  Register scratch1 = R0.scratchReg();
  Register scratch2 = R1.scratchReg();
  loadScriptGCThing(ScriptGCThingType::BigInt, scratch1, scratch2);
  masm.tagValue(JSVAL_TYPE_BIGINT, scratch1, R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_String() {
  frame.push(StringValue(handler.script()->getString(handler.pc())));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_String() {
  Register scratch1 = R0.scratchReg();
  Register scratch2 = R1.scratchReg();
  loadScriptGCThing(ScriptGCThingType::String, scratch1, scratch2);
  masm.tagValue(JSVAL_TYPE_STRING, scratch1, R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_Symbol() {
  unsigned which = GET_UINT8(handler.pc());
  JS::Symbol* sym = cx->runtime()->wellKnownSymbols->get(which);
  frame.push(SymbolValue(sym));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_Symbol() {
  Register scratch1 = R0.scratchReg();
  Register scratch2 = R1.scratchReg();
  LoadUint8Operand(masm, scratch1);

  masm.movePtr(ImmPtr(cx->runtime()->wellKnownSymbols), scratch2);
  masm.loadPtr(BaseIndex(scratch2, scratch1, ScalePointer), scratch1);

  masm.tagValue(JSVAL_TYPE_SYMBOL, scratch1, R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_Object() {
  frame.push(ObjectValue(*handler.script()->getObject(handler.pc())));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_Object() {
  Register scratch1 = R0.scratchReg();
  Register scratch2 = R1.scratchReg();
  loadScriptGCThing(ScriptGCThingType::Object, scratch1, scratch2);
  masm.tagValue(JSVAL_TYPE_OBJECT, scratch1, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CallSiteObj() {
  return emit_Object();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_RegExp() {
  prepareVMCall();
  pushScriptGCThingArg(ScriptGCThingType::RegExp, R0.scratchReg(),
                       R1.scratchReg());

  using Fn = JSObject* (*)(JSContext*, Handle<RegExpObject*>);
  if (!callVM<Fn, CloneRegExpObject>()) {
    return false;
  }

  // Box and push return value.
  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

#ifdef ENABLE_RECORD_TUPLE
#  define UNSUPPORTED_OPCODE(OP)                              \
    template <typename Handler>                               \
    bool BaselineCodeGen<Handler>::emit_##OP() {              \
      MOZ_CRASH("Record and Tuple are not supported by jit"); \
      return false;                                           \
    }

UNSUPPORTED_OPCODE(InitRecord)
UNSUPPORTED_OPCODE(AddRecordProperty)
UNSUPPORTED_OPCODE(AddRecordSpread)
UNSUPPORTED_OPCODE(FinishRecord)
UNSUPPORTED_OPCODE(InitTuple)
UNSUPPORTED_OPCODE(AddTupleElement)
UNSUPPORTED_OPCODE(FinishTuple)

#  undef UNSUPPORTED_OPCODE
#endif

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Lambda() {
  prepareVMCall();
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  pushArg(R0.scratchReg());
  pushScriptGCThingArg(ScriptGCThingType::Function, R0.scratchReg(),
                       R1.scratchReg());

  using Fn = JSObject* (*)(JSContext*, HandleFunction, HandleObject);
  if (!callVM<Fn, js::Lambda>()) {
    return false;
  }

  // Box and push return value.
  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetFunName() {
  frame.popRegsAndSync(2);

  frame.push(R0);
  frame.syncStack(0);

  masm.unboxObject(R0, R0.scratchReg());

  prepareVMCall();

  pushUint8BytecodeOperandArg(R2.scratchReg());
  pushArg(R1);
  pushArg(R0.scratchReg());

  using Fn =
      bool (*)(JSContext*, HandleFunction, HandleValue, FunctionPrefixKind);
  return callVM<Fn, SetFunctionName>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_BitOr() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_BitXor() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_BitAnd() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Lsh() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Rsh() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Ursh() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Add() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Sub() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Mul() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Div() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Mod() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Pow() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitBinaryArith() {
  // Keep top JSStack value in R0 and R2
  frame.popRegsAndSync(2);

  // Call IC
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitUnaryArith() {
  // Keep top stack value in R0.
  frame.popRegsAndSync(1);

  // Call IC
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_BitNot() {
  return emitUnaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Neg() {
  return emitUnaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Inc() {
  return emitUnaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Dec() {
  return emitUnaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Lt() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Le() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Gt() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Ge() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Eq() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Ne() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitCompare() {
  // Keep top JSStack value in R0 and R1.
  frame.popRegsAndSync(2);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictEq() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictNe() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Case() {
  frame.popRegsAndSync(1);

  Label done;
  masm.branchTestBooleanTruthy(/* branchIfTrue */ false, R0, &done);
  {
    // Pop the switch value if the case matches.
    masm.addToStackPtr(Imm32(sizeof(Value)));
    emitJump();
  }
  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Default() {
  frame.pop();
  return emit_Goto();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Lineno() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_NewArray() {
  frame.syncStack(0);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

static void MarkElementsNonPackedIfHoleValue(MacroAssembler& masm,
                                             Register elements,
                                             ValueOperand val) {
  Label notHole;
  masm.branchTestMagic(Assembler::NotEqual, val, &notHole);
  {
    Address elementsFlags(elements, ObjectElements::offsetOfFlags());
    masm.or32(Imm32(ObjectElements::NON_PACKED), elementsFlags);
  }
  masm.bind(&notHole);
}

template <>
bool BaselineInterpreterCodeGen::emit_InitElemArray() {
  // Pop value into R0, keep the object on the stack.
  frame.popRegsAndSync(1);

  // Load object in R2.
  Register obj = R2.scratchReg();
  masm.unboxObject(frame.addressOfStackValue(-1), obj);

  // Load index in R1.
  Register index = R1.scratchReg();
  LoadInt32Operand(masm, index);

  // Store the Value. No pre-barrier because this is an initialization.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), obj);
  masm.storeValue(R0, BaseObjectElementIndex(obj, index));

  // Bump initialized length.
  Address initLength(obj, ObjectElements::offsetOfInitializedLength());
  masm.add32(Imm32(1), index);
  masm.store32(index, initLength);

  // Mark elements as NON_PACKED if we stored the hole value.
  MarkElementsNonPackedIfHoleValue(masm, obj, R0);

  // Post-barrier.
  Label skipBarrier;
  Register scratch = index;
  masm.branchValueIsNurseryCell(Assembler::NotEqual, R0, scratch, &skipBarrier);
  {
    masm.unboxObject(frame.addressOfStackValue(-1), obj);
    masm.branchPtrInNurseryChunk(Assembler::Equal, obj, scratch, &skipBarrier);
    MOZ_ASSERT(obj == R2.scratchReg(), "post barrier expects object in R2");
    masm.call(&postBarrierSlot_);
  }
  masm.bind(&skipBarrier);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_InitElemArray() {
  // Pop value into R0, keep the object on the stack.
  Maybe<Value> knownValue = frame.knownStackValue(-1);
  frame.popRegsAndSync(1);

  // Load object in R2.
  Register obj = R2.scratchReg();
  masm.unboxObject(frame.addressOfStackValue(-1), obj);

  uint32_t index = GET_UINT32(handler.pc());
  MOZ_ASSERT(index <= INT32_MAX,
             "the bytecode emitter must fail to compile code that would "
             "produce an index exceeding int32_t range");

  // Store the Value. No pre-barrier because this is an initialization.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), obj);
  masm.storeValue(R0, Address(obj, index * sizeof(Value)));

  // Bump initialized length.
  Address initLength(obj, ObjectElements::offsetOfInitializedLength());
  masm.store32(Imm32(index + 1), initLength);

  // Mark elements as NON_PACKED if we stored the hole value. We know this
  // statically except when debugger instrumentation is enabled because that
  // forces a stack-sync (which discards constants and known types) for each op.
  if (knownValue && knownValue->isMagic(JS_ELEMENTS_HOLE)) {
    Address elementsFlags(obj, ObjectElements::offsetOfFlags());
    masm.or32(Imm32(ObjectElements::NON_PACKED), elementsFlags);
  } else if (handler.compileDebugInstrumentation()) {
    MarkElementsNonPackedIfHoleValue(masm, obj, R0);
  } else {
#ifdef DEBUG
    Label notHole;
    masm.branchTestMagic(Assembler::NotEqual, R0, &notHole);
    masm.assumeUnreachable("Unexpected hole value");
    masm.bind(&notHole);
#endif
  }

  // Post-barrier.
  if (knownValue) {
    MOZ_ASSERT(JS::GCPolicy<Value>::isTenured(*knownValue));
  } else {
    Label skipBarrier;
    Register scratch = R1.scratchReg();
    masm.branchValueIsNurseryCell(Assembler::NotEqual, R0, scratch,
                                  &skipBarrier);
    {
      masm.unboxObject(frame.addressOfStackValue(-1), obj);
      masm.branchPtrInNurseryChunk(Assembler::Equal, obj, scratch,
                                   &skipBarrier);
      MOZ_ASSERT(obj == R2.scratchReg(), "post barrier expects object in R2");
      masm.call(&postBarrierSlot_);
    }
    masm.bind(&skipBarrier);
  }
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_NewObject() {
  return emitNewObject();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_NewInit() {
  return emitNewObject();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitNewObject() {
  frame.syncStack(0);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitElem() {
  // Store RHS in the scratch slot.
  frame.storeStackValue(-1, frame.addressOfScratchValue(), R2);
  frame.pop();

  // Keep object and index in R0 and R1.
  frame.popRegsAndSync(2);

  // Push the object to store the result of the IC.
  frame.push(R0);
  frame.syncStack(0);

  // Keep RHS on the stack.
  frame.pushScratchValue();

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Pop the rhs, so that the object is on the top of the stack.
  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitHiddenElem() {
  return emit_InitElem();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitLockedElem() {
  return emit_InitElem();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_MutateProto() {
  // Keep values on the stack for the decompiler.
  frame.syncStack(0);

  masm.unboxObject(frame.addressOfStackValue(-2), R0.scratchReg());
  masm.loadValue(frame.addressOfStackValue(-1), R1);

  prepareVMCall();

  pushArg(R1);
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, Handle<PlainObject*>, HandleValue);
  if (!callVM<Fn, MutatePrototype>()) {
    return false;
  }

  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitProp() {
  // Load lhs in R0, rhs in R1.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-2), R0);
  masm.loadValue(frame.addressOfStackValue(-1), R1);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Leave the object on the stack.
  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitLockedProp() {
  return emit_InitProp();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitHiddenProp() {
  return emit_InitProp();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetElem() {
  // Keep top two stack values in R0 and R1.
  frame.popRegsAndSync(2);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetElemSuper() {
  // Store obj in the scratch slot.
  frame.storeStackValue(-1, frame.addressOfScratchValue(), R2);
  frame.pop();

  // Keep receiver and index in R0 and R1.
  frame.popRegsAndSync(2);

  // Keep obj on the stack.
  frame.pushScratchValue();

  if (!emitNextIC()) {
    return false;
  }

  frame.pop();
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetElem() {
  // Store RHS in the scratch slot.
  frame.storeStackValue(-1, frame.addressOfScratchValue(), R2);
  frame.pop();

  // Keep object and index in R0 and R1.
  frame.popRegsAndSync(2);

  // Keep RHS on the stack.
  frame.pushScratchValue();

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictSetElem() {
  return emit_SetElem();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitSetElemSuper(bool strict) {
  // Incoming stack is |receiver, propval, obj, rval|. We need to shuffle
  // stack to leave rval when operation is complete.

  // Pop rval into R0, then load receiver into R1 and replace with rval.
  frame.popRegsAndSync(1);
  masm.loadValue(frame.addressOfStackValue(-3), R1);
  masm.storeValue(R0, frame.addressOfStackValue(-3));

  prepareVMCall();

  pushArg(Imm32(strict));
  pushArg(R0);  // rval
  masm.loadValue(frame.addressOfStackValue(-2), R0);
  pushArg(R0);  // propval
  pushArg(R1);  // receiver
  masm.loadValue(frame.addressOfStackValue(-1), R0);
  pushArg(R0);  // obj

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue, HandleValue,
                      HandleValue, bool);
  if (!callVM<Fn, js::SetElementSuper>()) {
    return false;
  }

  frame.popn(2);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetElemSuper() {
  return emitSetElemSuper(/* strict = */ false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictSetElemSuper() {
  return emitSetElemSuper(/* strict = */ true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitDelElem(bool strict) {
  // Keep values on the stack for the decompiler.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-2), R0);
  masm.loadValue(frame.addressOfStackValue(-1), R1);

  prepareVMCall();

  pushArg(R1);
  pushArg(R0);

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue, bool*);
  if (strict) {
    if (!callVM<Fn, DelElemOperation<true>>()) {
      return false;
    }
  } else {
    if (!callVM<Fn, DelElemOperation<false>>()) {
      return false;
    }
  }

  masm.boxNonDouble(JSVAL_TYPE_BOOLEAN, ReturnReg, R1);
  frame.popn(2);
  frame.push(R1, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_DelElem() {
  return emitDelElem(/* strict = */ false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictDelElem() {
  return emitDelElem(/* strict = */ true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_In() {
  frame.popRegsAndSync(2);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_HasOwn() {
  frame.popRegsAndSync(2);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckPrivateField() {
  // Keep key and val on the stack.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-2), R0);
  masm.loadValue(frame.addressOfStackValue(-1), R1);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_NewPrivateName() {
  prepareVMCall();

  pushScriptNameArg(R0.scratchReg(), R1.scratchReg());

  using Fn = JS::Symbol* (*)(JSContext*, Handle<JSAtom*>);
  if (!callVM<Fn, NewPrivateName>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_SYMBOL, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetGName() {
  frame.syncStack(0);

  loadGlobalLexicalEnvironment(R0.scratchReg());

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::tryOptimizeBindGlobalName() {
  JSScript* script = handler.script();
  MOZ_ASSERT(!script->hasNonSyntacticScope());

  Rooted<GlobalObject*> global(cx, &script->global());
  Rooted<PropertyName*> name(cx, script->getName(handler.pc()));
  if (JSObject* binding = MaybeOptimizeBindGlobalName(cx, global, name)) {
    frame.push(ObjectValue(*binding));
    return true;
  }
  return false;
}

template <>
bool BaselineInterpreterCodeGen::tryOptimizeBindGlobalName() {
  // Interpreter doesn't optimize simple BindGNames.
  return false;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_BindGName() {
  if (tryOptimizeBindGlobalName()) {
    return true;
  }

  frame.syncStack(0);
  loadGlobalLexicalEnvironment(R0.scratchReg());

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_BindVar() {
  frame.syncStack(0);
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  prepareVMCall();
  pushArg(R0.scratchReg());

  using Fn = JSObject* (*)(JSContext*, JSObject*);
  if (!callVM<Fn, BindVarOperation>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetProp() {
  // Keep lhs in R0, rhs in R1.
  frame.popRegsAndSync(2);

  // Keep RHS on the stack.
  frame.push(R1);
  frame.syncStack(0);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictSetProp() {
  return emit_SetProp();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetName() {
  return emit_SetProp();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictSetName() {
  return emit_SetProp();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetGName() {
  return emit_SetProp();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictSetGName() {
  return emit_SetProp();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitSetPropSuper(bool strict) {
  // Incoming stack is |receiver, obj, rval|. We need to shuffle stack to
  // leave rval when operation is complete.

  // Pop rval into R0, then load receiver into R1 and replace with rval.
  frame.popRegsAndSync(1);
  masm.loadValue(frame.addressOfStackValue(-2), R1);
  masm.storeValue(R0, frame.addressOfStackValue(-2));

  prepareVMCall();

  pushArg(Imm32(strict));
  pushArg(R0);  // rval
  pushScriptNameArg(R0.scratchReg(), R2.scratchReg());
  pushArg(R1);  // receiver
  masm.loadValue(frame.addressOfStackValue(-1), R0);
  pushArg(R0);  // obj

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue,
                      Handle<PropertyName*>, HandleValue, bool);
  if (!callVM<Fn, js::SetPropertySuper>()) {
    return false;
  }

  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetPropSuper() {
  return emitSetPropSuper(/* strict = */ false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictSetPropSuper() {
  return emitSetPropSuper(/* strict = */ true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetProp() {
  // Keep object in R0.
  frame.popRegsAndSync(1);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetBoundName() {
  return emit_GetProp();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetPropSuper() {
  // Receiver -> R1, ObjectOrNull -> R0
  frame.popRegsAndSync(1);
  masm.loadValue(frame.addressOfStackValue(-1), R1);
  frame.pop();

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitDelProp(bool strict) {
  // Keep value on the stack for the decompiler.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  prepareVMCall();

  pushScriptNameArg(R1.scratchReg(), R2.scratchReg());
  pushArg(R0);

  using Fn = bool (*)(JSContext*, HandleValue, Handle<PropertyName*>, bool*);
  if (strict) {
    if (!callVM<Fn, DelPropOperation<true>>()) {
      return false;
    }
  } else {
    if (!callVM<Fn, DelPropOperation<false>>()) {
      return false;
    }
  }

  masm.boxNonDouble(JSVAL_TYPE_BOOLEAN, ReturnReg, R1);
  frame.pop();
  frame.push(R1, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_DelProp() {
  return emitDelProp(/* strict = */ false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictDelProp() {
  return emitDelProp(/* strict = */ true);
}

template <>
void BaselineCompilerCodeGen::getEnvironmentCoordinateObject(Register reg) {
  EnvironmentCoordinate ec(handler.pc());

  masm.loadPtr(frame.addressOfEnvironmentChain(), reg);
  for (unsigned i = ec.hops(); i; i--) {
    masm.unboxObject(
        Address(reg, EnvironmentObject::offsetOfEnclosingEnvironment()), reg);
  }
}

template <>
void BaselineInterpreterCodeGen::getEnvironmentCoordinateObject(Register reg) {
  MOZ_CRASH("Shouldn't call this for interpreter");
}

template <>
Address BaselineCompilerCodeGen::getEnvironmentCoordinateAddressFromObject(
    Register objReg, Register reg) {
  EnvironmentCoordinate ec(handler.pc());

  if (EnvironmentObject::nonExtensibleIsFixedSlot(ec)) {
    return Address(objReg, NativeObject::getFixedSlotOffset(ec.slot()));
  }

  uint32_t slot = EnvironmentObject::nonExtensibleDynamicSlotIndex(ec);
  masm.loadPtr(Address(objReg, NativeObject::offsetOfSlots()), reg);
  return Address(reg, slot * sizeof(Value));
}

template <>
Address BaselineInterpreterCodeGen::getEnvironmentCoordinateAddressFromObject(
    Register objReg, Register reg) {
  MOZ_CRASH("Shouldn't call this for interpreter");
}

template <typename Handler>
Address BaselineCodeGen<Handler>::getEnvironmentCoordinateAddress(
    Register reg) {
  getEnvironmentCoordinateObject(reg);
  return getEnvironmentCoordinateAddressFromObject(reg, reg);
}

// For a JOF_ENVCOORD op load the number of hops from the bytecode and skip this
// number of environment objects.
static void LoadAliasedVarEnv(MacroAssembler& masm, Register env,
                              Register scratch) {
  static_assert(ENVCOORD_HOPS_LEN == 1,
                "Code assumes number of hops is stored in uint8 operand");
  LoadUint8Operand(masm, scratch);

  Label top, done;
  masm.branchTest32(Assembler::Zero, scratch, scratch, &done);
  masm.bind(&top);
  {
    Address nextEnv(env, EnvironmentObject::offsetOfEnclosingEnvironment());
    masm.unboxObject(nextEnv, env);
    masm.branchSub32(Assembler::NonZero, Imm32(1), scratch, &top);
  }
  masm.bind(&done);
}

template <>
void BaselineCompilerCodeGen::emitGetAliasedVar(ValueOperand dest) {
  frame.syncStack(0);

  Address address = getEnvironmentCoordinateAddress(R0.scratchReg());
  masm.loadValue(address, dest);
}

template <>
void BaselineInterpreterCodeGen::emitGetAliasedVar(ValueOperand dest) {
  Register env = R0.scratchReg();
  Register scratch = R1.scratchReg();

  // Load the right environment object.
  masm.loadPtr(frame.addressOfEnvironmentChain(), env);
  LoadAliasedVarEnv(masm, env, scratch);

  // Load the slot index.
  static_assert(ENVCOORD_SLOT_LEN == 3,
                "Code assumes slot is stored in uint24 operand");
  LoadUint24Operand(masm, ENVCOORD_HOPS_LEN, scratch);

  // Load the Value from a fixed or dynamic slot.
  // See EnvironmentObject::nonExtensibleIsFixedSlot.
  Label isDynamic, done;
  masm.branch32(Assembler::AboveOrEqual, scratch,
                Imm32(NativeObject::MAX_FIXED_SLOTS), &isDynamic);
  {
    uint32_t offset = NativeObject::getFixedSlotOffset(0);
    masm.loadValue(BaseValueIndex(env, scratch, offset), dest);
    masm.jump(&done);
  }
  masm.bind(&isDynamic);
  {
    masm.loadPtr(Address(env, NativeObject::offsetOfSlots()), env);

    // Use an offset to subtract the number of fixed slots.
    int32_t offset = -int32_t(NativeObject::MAX_FIXED_SLOTS * sizeof(Value));
    masm.loadValue(BaseValueIndex(env, scratch, offset), dest);
  }
  masm.bind(&done);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitGetAliasedDebugVar(ValueOperand dest) {
  frame.syncStack(0);
  Register env = R0.scratchReg();
  // Load the right environment object.
  masm.loadPtr(frame.addressOfEnvironmentChain(), env);

  prepareVMCall();
  pushBytecodePCArg();
  pushArg(env);

  using Fn =
      bool (*)(JSContext*, JSObject* env, jsbytecode*, MutableHandleValue);
  return callVM<Fn, LoadAliasedDebugVar>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetAliasedDebugVar() {
  if (!emitGetAliasedDebugVar(R0)) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetAliasedVar() {
  emitGetAliasedVar(R0);

  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_SetAliasedVar() {
  // Keep rvalue in R0.
  frame.popRegsAndSync(1);
  Register objReg = R2.scratchReg();

  getEnvironmentCoordinateObject(objReg);
  Address address =
      getEnvironmentCoordinateAddressFromObject(objReg, R1.scratchReg());
  masm.guardedCallPreBarrier(address, MIRType::Value);
  masm.storeValue(R0, address);
  frame.push(R0);

  // Only R0 is live at this point.
  // Scope coordinate object is already in R2.scratchReg().
  Register temp = R1.scratchReg();

  Label skipBarrier;
  masm.branchPtrInNurseryChunk(Assembler::Equal, objReg, temp, &skipBarrier);
  masm.branchValueIsNurseryCell(Assembler::NotEqual, R0, temp, &skipBarrier);

  masm.call(&postBarrierSlot_);  // Won't clobber R0

  masm.bind(&skipBarrier);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_SetAliasedVar() {
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  MOZ_ASSERT(!regs.has(FramePointer));
  regs.take(R2);
  if (HasInterpreterPCReg()) {
    regs.take(InterpreterPCReg);
  }

  Register env = regs.takeAny();
  Register scratch1 = regs.takeAny();
  Register scratch2 = regs.takeAny();
  Register scratch3 = regs.takeAny();

  // Load the right environment object.
  masm.loadPtr(frame.addressOfEnvironmentChain(), env);
  LoadAliasedVarEnv(masm, env, scratch1);

  // Load the slot index.
  static_assert(ENVCOORD_SLOT_LEN == 3,
                "Code assumes slot is stored in uint24 operand");
  LoadUint24Operand(masm, ENVCOORD_HOPS_LEN, scratch1);

  // Store the RHS Value in R2.
  masm.loadValue(frame.addressOfStackValue(-1), R2);

  // Load a pointer to the fixed or dynamic slot into scratch2. We want to call
  // guardedCallPreBarrierAnyZone once to avoid code bloat.

  // See EnvironmentObject::nonExtensibleIsFixedSlot.
  Label isDynamic, done;
  masm.branch32(Assembler::AboveOrEqual, scratch1,
                Imm32(NativeObject::MAX_FIXED_SLOTS), &isDynamic);
  {
    uint32_t offset = NativeObject::getFixedSlotOffset(0);
    BaseValueIndex slotAddr(env, scratch1, offset);
    masm.computeEffectiveAddress(slotAddr, scratch2);
    masm.jump(&done);
  }
  masm.bind(&isDynamic);
  {
    masm.loadPtr(Address(env, NativeObject::offsetOfSlots()), scratch2);

    // Use an offset to subtract the number of fixed slots.
    int32_t offset = -int32_t(NativeObject::MAX_FIXED_SLOTS * sizeof(Value));
    BaseValueIndex slotAddr(scratch2, scratch1, offset);
    masm.computeEffectiveAddress(slotAddr, scratch2);
  }
  masm.bind(&done);

  // Pre-barrier and store.
  Address slotAddr(scratch2, 0);
  masm.guardedCallPreBarrierAnyZone(slotAddr, MIRType::Value, scratch3);
  masm.storeValue(R2, slotAddr);

  // Post barrier.
  Label skipBarrier;
  masm.branchPtrInNurseryChunk(Assembler::Equal, env, scratch1, &skipBarrier);
  masm.branchValueIsNurseryCell(Assembler::NotEqual, R2, scratch1,
                                &skipBarrier);
  {
    // Post barrier code expects the object in R2.
    masm.movePtr(env, R2.scratchReg());
    masm.call(&postBarrierSlot_);
  }
  masm.bind(&skipBarrier);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetName() {
  frame.syncStack(0);

  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_BindName() {
  frame.syncStack(0);
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_DelName() {
  frame.syncStack(0);
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  prepareVMCall();

  pushArg(R0.scratchReg());
  pushScriptNameArg(R1.scratchReg(), R2.scratchReg());

  using Fn = bool (*)(JSContext*, Handle<PropertyName*>, HandleObject,
                      MutableHandleValue);
  if (!callVM<Fn, js::DeleteNameOperation>()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_GetImport() {
  JSScript* script = handler.script();
  ModuleEnvironmentObject* env = GetModuleEnvironmentForScript(script);
  MOZ_ASSERT(env);

  jsid id = NameToId(script->getName(handler.pc()));
  ModuleEnvironmentObject* targetEnv;
  Maybe<PropertyInfo> prop;
  MOZ_ALWAYS_TRUE(env->lookupImport(id, &targetEnv, &prop));

  frame.syncStack(0);

  uint32_t slot = prop->slot();
  Register scratch = R0.scratchReg();
  masm.movePtr(ImmGCPtr(targetEnv), scratch);
  if (slot < targetEnv->numFixedSlots()) {
    masm.loadValue(Address(scratch, NativeObject::getFixedSlotOffset(slot)),
                   R0);
  } else {
    masm.loadPtr(Address(scratch, NativeObject::offsetOfSlots()), scratch);
    masm.loadValue(
        Address(scratch, (slot - targetEnv->numFixedSlots()) * sizeof(Value)),
        R0);
  }

  // Imports are initialized by this point except in rare circumstances, so
  // don't emit a check unless we have to.
  if (targetEnv->getSlot(slot).isMagic(JS_UNINITIALIZED_LEXICAL)) {
    if (!emitUninitializedLexicalCheck(R0)) {
      return false;
    }
  }

  frame.push(R0);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_GetImport() {
  frame.syncStack(0);

  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  prepareVMCall();

  pushBytecodePCArg();
  pushScriptArg();
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, HandleObject, HandleScript, jsbytecode*,
                      MutableHandleValue);
  if (!callVM<Fn, GetImportOperation>()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetIntrinsic() {
  frame.syncStack(0);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetIntrinsic() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  prepareVMCall();

  pushArg(R0);
  pushBytecodePCArg();
  pushScriptArg();

  using Fn = bool (*)(JSContext*, JSScript*, jsbytecode*, HandleValue);
  return callVM<Fn, SetIntrinsicOperation>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GlobalOrEvalDeclInstantiation() {
  frame.syncStack(0);

  prepareVMCall();

  loadInt32LengthBytecodeOperand(R0.scratchReg());
  pushArg(R0.scratchReg());
  pushScriptArg();
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, HandleObject, HandleScript, GCThingIndex);
  return callVM<Fn, js::GlobalOrEvalDeclInstantiation>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitInitPropGetterSetter() {
  // Keep values on the stack for the decompiler.
  frame.syncStack(0);

  prepareVMCall();

  masm.unboxObject(frame.addressOfStackValue(-1), R0.scratchReg());
  masm.unboxObject(frame.addressOfStackValue(-2), R1.scratchReg());

  pushArg(R0.scratchReg());
  pushScriptNameArg(R0.scratchReg(), R2.scratchReg());
  pushArg(R1.scratchReg());
  pushBytecodePCArg();

  using Fn = bool (*)(JSContext*, jsbytecode*, HandleObject,
                      Handle<PropertyName*>, HandleObject);
  if (!callVM<Fn, InitPropGetterSetterOperation>()) {
    return false;
  }

  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitPropGetter() {
  return emitInitPropGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitHiddenPropGetter() {
  return emitInitPropGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitPropSetter() {
  return emitInitPropGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitHiddenPropSetter() {
  return emitInitPropGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitInitElemGetterSetter() {
  // Load index and value in R0 and R1, but keep values on the stack for the
  // decompiler.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-2), R0);
  masm.unboxObject(frame.addressOfStackValue(-1), R1.scratchReg());

  prepareVMCall();

  pushArg(R1.scratchReg());
  pushArg(R0);
  masm.unboxObject(frame.addressOfStackValue(-3), R0.scratchReg());
  pushArg(R0.scratchReg());
  pushBytecodePCArg();

  using Fn = bool (*)(JSContext*, jsbytecode*, HandleObject, HandleValue,
                      HandleObject);
  if (!callVM<Fn, InitElemGetterSetterOperation>()) {
    return false;
  }

  frame.popn(2);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitElemGetter() {
  return emitInitElemGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitHiddenElemGetter() {
  return emitInitElemGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitElemSetter() {
  return emitInitElemGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitHiddenElemSetter() {
  return emitInitElemGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitElemInc() {
  // Keep the object and rhs on the stack.
  frame.syncStack(0);

  // Load object in R0, index in R1.
  masm.loadValue(frame.addressOfStackValue(-3), R0);
  masm.loadValue(frame.addressOfStackValue(-2), R1);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Pop the rhs
  frame.pop();

  // Increment index
  Address indexAddr = frame.addressOfStackValue(-1);
#ifdef DEBUG
  Label isInt32;
  masm.branchTestInt32(Assembler::Equal, indexAddr, &isInt32);
  masm.assumeUnreachable("INITELEM_INC index must be Int32");
  masm.bind(&isInt32);
#endif
  masm.incrementInt32Value(indexAddr);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_GetLocal() {
  frame.pushLocal(GET_LOCALNO(handler.pc()));
  return true;
}

static BaseValueIndex ComputeAddressOfLocal(MacroAssembler& masm,
                                            Register indexScratch) {
  // Locals are stored in memory at a negative offset from the frame pointer. We
  // negate the index first to effectively subtract it.
  masm.negPtr(indexScratch);
  return BaseValueIndex(FramePointer, indexScratch,
                        BaselineFrame::reverseOffsetOfLocal(0));
}

template <>
bool BaselineInterpreterCodeGen::emit_GetLocal() {
  Register scratch = R0.scratchReg();
  LoadUint24Operand(masm, 0, scratch);
  BaseValueIndex addr = ComputeAddressOfLocal(masm, scratch);
  masm.loadValue(addr, R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_SetLocal() {
  // Ensure no other StackValue refers to the old value, for instance i + (i =
  // 3). This also allows us to use R0 as scratch below.
  frame.syncStack(1);

  uint32_t local = GET_LOCALNO(handler.pc());
  frame.storeStackValue(-1, frame.addressOfLocal(local), R0);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_SetLocal() {
  Register scratch = R0.scratchReg();
  LoadUint24Operand(masm, 0, scratch);
  BaseValueIndex addr = ComputeAddressOfLocal(masm, scratch);
  masm.loadValue(frame.addressOfStackValue(-1), R1);
  masm.storeValue(R1, addr);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emitFormalArgAccess(JSOp op) {
  MOZ_ASSERT(op == JSOp::GetArg || op == JSOp::SetArg);

  uint32_t arg = GET_ARGNO(handler.pc());

  // Fast path: the script does not use |arguments| or formals don't
  // alias the arguments object.
  if (!handler.script()->argsObjAliasesFormals()) {
    if (op == JSOp::GetArg) {
      frame.pushArg(arg);
    } else {
      // See the comment in emit_SetLocal.
      frame.syncStack(1);
      frame.storeStackValue(-1, frame.addressOfArg(arg), R0);
    }

    return true;
  }

  // Sync so that we can use R0.
  frame.syncStack(0);

  // Load the arguments object data vector.
  Register reg = R2.scratchReg();
  masm.loadPtr(frame.addressOfArgsObj(), reg);
  masm.loadPrivate(Address(reg, ArgumentsObject::getDataSlotOffset()), reg);

  // Load/store the argument.
  Address argAddr(reg, ArgumentsData::offsetOfArgs() + arg * sizeof(Value));
  if (op == JSOp::GetArg) {
    masm.loadValue(argAddr, R0);
    frame.push(R0);
  } else {
    Register temp = R1.scratchReg();
    masm.guardedCallPreBarrierAnyZone(argAddr, MIRType::Value, temp);
    masm.loadValue(frame.addressOfStackValue(-1), R0);
    masm.storeValue(R0, argAddr);

    MOZ_ASSERT(frame.numUnsyncedSlots() == 0);

    // Reload the arguments object.
    Register reg = R2.scratchReg();
    masm.loadPtr(frame.addressOfArgsObj(), reg);

    Label skipBarrier;

    masm.branchPtrInNurseryChunk(Assembler::Equal, reg, temp, &skipBarrier);
    masm.branchValueIsNurseryCell(Assembler::NotEqual, R0, temp, &skipBarrier);

    masm.call(&postBarrierSlot_);

    masm.bind(&skipBarrier);
  }

  return true;
}

template <>
bool BaselineInterpreterCodeGen::emitFormalArgAccess(JSOp op) {
  MOZ_ASSERT(op == JSOp::GetArg || op == JSOp::SetArg);

  // Load the index.
  Register argReg = R1.scratchReg();
  LoadUint16Operand(masm, argReg);

  // If the frame has no arguments object, this must be an unaliased access.
  Label isUnaliased, done;
  masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                    Imm32(BaselineFrame::HAS_ARGS_OBJ), &isUnaliased);
  {
    Register reg = R2.scratchReg();

    // If it's an unmapped arguments object, this is an unaliased access.
    loadScript(reg);
    masm.branchTest32(
        Assembler::Zero, Address(reg, JSScript::offsetOfImmutableFlags()),
        Imm32(uint32_t(JSScript::ImmutableFlags::HasMappedArgsObj)),
        &isUnaliased);

    // Load the arguments object data vector.
    masm.loadPtr(frame.addressOfArgsObj(), reg);
    masm.loadPrivate(Address(reg, ArgumentsObject::getDataSlotOffset()), reg);

    // Load/store the argument.
    BaseValueIndex argAddr(reg, argReg, ArgumentsData::offsetOfArgs());
    if (op == JSOp::GetArg) {
      masm.loadValue(argAddr, R0);
      frame.push(R0);
    } else {
      masm.guardedCallPreBarrierAnyZone(argAddr, MIRType::Value,
                                        R0.scratchReg());
      masm.loadValue(frame.addressOfStackValue(-1), R0);
      masm.storeValue(R0, argAddr);

      // Reload the arguments object.
      masm.loadPtr(frame.addressOfArgsObj(), reg);

      Register temp = R1.scratchReg();
      masm.branchPtrInNurseryChunk(Assembler::Equal, reg, temp, &done);
      masm.branchValueIsNurseryCell(Assembler::NotEqual, R0, temp, &done);

      masm.call(&postBarrierSlot_);
    }
    masm.jump(&done);
  }
  masm.bind(&isUnaliased);
  {
    BaseValueIndex addr(FramePointer, argReg,
                        JitFrameLayout::offsetOfActualArgs());
    if (op == JSOp::GetArg) {
      masm.loadValue(addr, R0);
      frame.push(R0);
    } else {
      masm.loadValue(frame.addressOfStackValue(-1), R0);
      masm.storeValue(R0, addr);
    }
  }

  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetArg() {
  return emitFormalArgAccess(JSOp::GetArg);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetArg() {
  return emitFormalArgAccess(JSOp::SetArg);
}

template <>
bool BaselineInterpreterCodeGen::emit_GetFrameArg() {
  frame.syncStack(0);

  Register argReg = R1.scratchReg();
  LoadUint16Operand(masm, argReg);

  BaseValueIndex addr(FramePointer, argReg,
                      JitFrameLayout::offsetOfActualArgs());
  masm.loadValue(addr, R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_GetFrameArg() {
  uint32_t arg = GET_ARGNO(handler.pc());
  frame.pushArg(arg);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ArgumentsLength() {
  frame.syncStack(0);

  masm.loadNumActualArgs(FramePointer, R0.scratchReg());
  masm.tagValue(JSVAL_TYPE_INT32, R0.scratchReg(), R0);

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetActualArg() {
  frame.popRegsAndSync(1);

#ifdef DEBUG
  {
    Label ok;
    masm.branchTestInt32(Assembler::Equal, R0, &ok);
    masm.assumeUnreachable("GetActualArg unexpected type");
    masm.bind(&ok);
  }
#endif

  Register index = R0.scratchReg();
  masm.unboxInt32(R0, index);

#ifdef DEBUG
  {
    Label ok;
    masm.loadNumActualArgs(FramePointer, R1.scratchReg());
    masm.branch32(Assembler::Above, R1.scratchReg(), index, &ok);
    masm.assumeUnreachable("GetActualArg invalid index");
    masm.bind(&ok);
  }
#endif

  BaseValueIndex addr(FramePointer, index,
                      JitFrameLayout::offsetOfActualArgs());
  masm.loadValue(addr, R0);
  frame.push(R0);
  return true;
}

template <>
void BaselineCompilerCodeGen::loadNumFormalArguments(Register dest) {
  masm.move32(Imm32(handler.function()->nargs()), dest);
}

template <>
void BaselineInterpreterCodeGen::loadNumFormalArguments(Register dest) {
  masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(), dest);
  masm.loadFunctionArgCount(dest, dest);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_NewTarget() {
  MOZ_ASSERT_IF(handler.maybeFunction(), !handler.maybeFunction()->isArrow());

  frame.syncStack(0);

#ifdef DEBUG
  Register scratch1 = R0.scratchReg();
  Register scratch2 = R1.scratchReg();

  Label isFunction;
  masm.loadPtr(frame.addressOfCalleeToken(), scratch1);
  masm.branchTestPtr(Assembler::Zero, scratch1, Imm32(CalleeTokenScriptBit),
                     &isFunction);
  masm.assumeUnreachable("Unexpected non-function script");
  masm.bind(&isFunction);

  Label notArrow;
  masm.andPtr(Imm32(uint32_t(CalleeTokenMask)), scratch1);
  masm.branchFunctionKind(Assembler::NotEqual,
                          FunctionFlags::FunctionKind::Arrow, scratch1,
                          scratch2, &notArrow);
  masm.assumeUnreachable("Unexpected arrow function");
  masm.bind(&notArrow);
#endif

  // if (isConstructing()) push(argv[Max(numActualArgs, numFormalArgs)])
  Label notConstructing, done;
  masm.branchTestPtr(Assembler::Zero, frame.addressOfCalleeToken(),
                     Imm32(CalleeToken_FunctionConstructing), &notConstructing);
  {
    Register argvLen = R0.scratchReg();
    Register nformals = R1.scratchReg();
    masm.loadNumActualArgs(FramePointer, argvLen);

    // If argvLen < nformals, set argvlen := nformals.
    loadNumFormalArguments(nformals);
    masm.cmp32Move32(Assembler::Below, argvLen, nformals, nformals, argvLen);

    BaseValueIndex newTarget(FramePointer, argvLen,
                             JitFrameLayout::offsetOfActualArgs());
    masm.loadValue(newTarget, R0);
    masm.jump(&done);
  }
  // else push(undefined)
  masm.bind(&notConstructing);
  masm.moveValue(UndefinedValue(), R0);

  masm.bind(&done);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ThrowSetConst() {
  prepareVMCall();
  pushArg(Imm32(JSMSG_BAD_CONST_ASSIGN));

  using Fn = bool (*)(JSContext*, unsigned);
  return callVM<Fn, jit::ThrowRuntimeLexicalError>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitUninitializedLexicalCheck(
    const ValueOperand& val) {
  Label done;
  masm.branchTestMagicValue(Assembler::NotEqual, val, JS_UNINITIALIZED_LEXICAL,
                            &done);

  prepareVMCall();
  pushArg(Imm32(JSMSG_UNINITIALIZED_LEXICAL));

  using Fn = bool (*)(JSContext*, unsigned);
  if (!callVM<Fn, jit::ThrowRuntimeLexicalError>()) {
    return false;
  }

  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckLexical() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-1), R0);
  return emitUninitializedLexicalCheck(R0);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckAliasedLexical() {
  return emit_CheckLexical();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitLexical() {
  return emit_SetLocal();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitGLexical() {
  frame.popRegsAndSync(1);
  pushGlobalLexicalEnvironmentValue(R1);
  frame.push(R0);
  return emit_SetProp();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitAliasedLexical() {
  return emit_SetAliasedVar();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Uninitialized() {
  frame.push(MagicValue(JS_UNINITIALIZED_LEXICAL));
  return true;
}

template <>
bool BaselineCompilerCodeGen::emitCall(JSOp op) {
  MOZ_ASSERT(IsInvokeOp(op));

  frame.syncStack(0);

  uint32_t argc = GET_ARGC(handler.pc());
  masm.move32(Imm32(argc), R0.scratchReg());

  // Call IC
  if (!emitNextIC()) {
    return false;
  }

  // Update FrameInfo.
  bool construct = IsConstructOp(op);
  frame.popn(2 + argc + construct);
  frame.push(R0);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emitCall(JSOp op) {
  MOZ_ASSERT(IsInvokeOp(op));

  // The IC expects argc in R0.
  LoadUint16Operand(masm, R0.scratchReg());
  if (!emitNextIC()) {
    return false;
  }

  // Pop the arguments. We have to reload pc/argc because the IC clobbers them.
  // The return value is in R0 so we can't use that.
  Register scratch = R1.scratchReg();
  uint32_t extraValuesToPop = IsConstructOp(op) ? 3 : 2;
  Register spReg = AsRegister(masm.getStackPointer());
  LoadUint16Operand(masm, scratch);
  masm.computeEffectiveAddress(
      BaseValueIndex(spReg, scratch, extraValuesToPop * sizeof(Value)), spReg);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitSpreadCall(JSOp op) {
  MOZ_ASSERT(IsInvokeOp(op));

  frame.syncStack(0);
  masm.move32(Imm32(1), R0.scratchReg());

  // Call IC
  if (!emitNextIC()) {
    return false;
  }

  // Update FrameInfo.
  bool construct = op == JSOp::SpreadNew || op == JSOp::SpreadSuperCall;
  frame.popn(3 + construct);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Call() {
  return emitCall(JSOp::Call);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CallContent() {
  return emitCall(JSOp::CallContent);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CallIgnoresRv() {
  return emitCall(JSOp::CallIgnoresRv);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CallIter() {
  return emitCall(JSOp::CallIter);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CallContentIter() {
  return emitCall(JSOp::CallContentIter);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_New() {
  return emitCall(JSOp::New);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_NewContent() {
  return emitCall(JSOp::NewContent);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SuperCall() {
  return emitCall(JSOp::SuperCall);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Eval() {
  return emitCall(JSOp::Eval);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictEval() {
  return emitCall(JSOp::StrictEval);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SpreadCall() {
  return emitSpreadCall(JSOp::SpreadCall);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SpreadNew() {
  return emitSpreadCall(JSOp::SpreadNew);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SpreadSuperCall() {
  return emitSpreadCall(JSOp::SpreadSuperCall);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SpreadEval() {
  return emitSpreadCall(JSOp::SpreadEval);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_StrictSpreadEval() {
  return emitSpreadCall(JSOp::StrictSpreadEval);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_OptimizeSpreadCall() {
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ImplicitThis() {
  frame.syncStack(0);
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  prepareVMCall();

  pushScriptNameArg(R1.scratchReg(), R2.scratchReg());
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, HandleObject, Handle<PropertyName*>,
                      MutableHandleValue);
  if (!callVM<Fn, ImplicitThisOperation>()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Instanceof() {
  frame.popRegsAndSync(2);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Typeof() {
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_TypeofExpr() {
  return emit_Typeof();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_TypeofEq() {
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ThrowMsg() {
  prepareVMCall();
  pushUint8BytecodeOperandArg(R2.scratchReg());

  using Fn = bool (*)(JSContext*, const unsigned);
  return callVM<Fn, js::ThrowMsgOperation>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Throw() {
  // Keep value to throw in R0.
  frame.popRegsAndSync(1);

  prepareVMCall();
  pushArg(R0);

  using Fn = bool (*)(JSContext*, HandleValue);
  return callVM<Fn, js::ThrowOperation>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ThrowWithStack() {
  // Keep value to throw in R0 and the stack in R1.
  frame.popRegsAndSync(2);

  prepareVMCall();
  pushArg(R1);
  pushArg(R0);

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue);
  return callVM<Fn, js::ThrowWithStackOperation>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Try() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Finally() {
  // To match the interpreter, emit an interrupt check at the start of the
  // finally block.
  return emitInterruptCheck();
}

static void LoadBaselineScriptResumeEntries(MacroAssembler& masm,
                                            JSScript* script, Register dest,
                                            Register scratch) {
  MOZ_ASSERT(dest != scratch);

  masm.movePtr(ImmPtr(script->jitScript()), dest);
  masm.loadPtr(Address(dest, JitScript::offsetOfBaselineScript()), dest);
  masm.load32(Address(dest, BaselineScript::offsetOfResumeEntriesOffset()),
              scratch);
  masm.addPtr(scratch, dest);
}

template <typename Handler>
void BaselineCodeGen<Handler>::emitInterpJumpToResumeEntry(Register script,
                                                           Register resumeIndex,
                                                           Register scratch) {
  // Load JSScript::immutableScriptData() into |script|.
  masm.loadPtr(Address(script, JSScript::offsetOfSharedData()), script);
  masm.loadPtr(Address(script, SharedImmutableScriptData::offsetOfISD()),
               script);

  // Load the resume pcOffset in |resumeIndex|.
  masm.load32(
      Address(script, ImmutableScriptData::offsetOfResumeOffsetsOffset()),
      scratch);
  masm.computeEffectiveAddress(BaseIndex(scratch, resumeIndex, TimesFour),
                               scratch);
  masm.load32(BaseIndex(script, scratch, TimesOne), resumeIndex);

  // Add resume offset to PC, jump to it.
  masm.computeEffectiveAddress(BaseIndex(script, resumeIndex, TimesOne,
                                         ImmutableScriptData::offsetOfCode()),
                               script);
  Address pcAddr(FramePointer, BaselineFrame::reverseOffsetOfInterpreterPC());
  masm.storePtr(script, pcAddr);
  emitJumpToInterpretOpLabel();
}

template <>
void BaselineCompilerCodeGen::jumpToResumeEntry(Register resumeIndex,
                                                Register scratch1,
                                                Register scratch2) {
  LoadBaselineScriptResumeEntries(masm, handler.script(), scratch1, scratch2);
  masm.loadPtr(
      BaseIndex(scratch1, resumeIndex, ScaleFromElemWidth(sizeof(uintptr_t))),
      scratch1);
  masm.jump(scratch1);
}

template <>
void BaselineInterpreterCodeGen::jumpToResumeEntry(Register resumeIndex,
                                                   Register scratch1,
                                                   Register scratch2) {
  loadScript(scratch1);
  emitInterpJumpToResumeEntry(scratch1, resumeIndex, scratch2);
}

template <>
template <typename F1, typename F2>
[[nodiscard]] bool BaselineCompilerCodeGen::emitDebugInstrumentation(
    const F1& ifDebuggee, const Maybe<F2>& ifNotDebuggee) {
  // The JIT calls either ifDebuggee or (if present) ifNotDebuggee, because it
  // knows statically whether we're compiling with debug instrumentation.

  if (handler.compileDebugInstrumentation()) {
    return ifDebuggee();
  }

  if (ifNotDebuggee) {
    return (*ifNotDebuggee)();
  }

  return true;
}

template <>
template <typename F1, typename F2>
[[nodiscard]] bool BaselineInterpreterCodeGen::emitDebugInstrumentation(
    const F1& ifDebuggee, const Maybe<F2>& ifNotDebuggee) {
  // The interpreter emits both ifDebuggee and (if present) ifNotDebuggee
  // paths, with a toggled jump followed by a branch on the frame's DEBUGGEE
  // flag.

  Label isNotDebuggee, done;

  CodeOffset toggleOffset = masm.toggledJump(&isNotDebuggee);
  if (!handler.addDebugInstrumentationOffset(cx, toggleOffset)) {
    return false;
  }

  masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                    Imm32(BaselineFrame::DEBUGGEE), &isNotDebuggee);

  if (!ifDebuggee()) {
    return false;
  }

  if (ifNotDebuggee) {
    masm.jump(&done);
  }

  masm.bind(&isNotDebuggee);

  if (ifNotDebuggee && !(*ifNotDebuggee)()) {
    return false;
  }

  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_PushLexicalEnv() {
  // Call a stub to push the block on the block chain.
  prepareVMCall();
  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());

  pushScriptGCThingArg(ScriptGCThingType::Scope, R1.scratchReg(),
                       R2.scratchReg());
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, Handle<LexicalScope*>);
  return callVM<Fn, jit::PushLexicalEnv>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_PushClassBodyEnv() {
  prepareVMCall();
  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());

  pushScriptGCThingArg(ScriptGCThingType::Scope, R1.scratchReg(),
                       R2.scratchReg());
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, Handle<ClassBodyScope*>);
  return callVM<Fn, jit::PushClassBodyEnv>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_PopLexicalEnv() {
  frame.syncStack(0);

  Register scratch1 = R0.scratchReg();

  auto ifDebuggee = [this, scratch1]() {
    masm.loadBaselineFramePtr(FramePointer, scratch1);

    prepareVMCall();
    pushBytecodePCArg();
    pushArg(scratch1);

    using Fn = bool (*)(JSContext*, BaselineFrame*, const jsbytecode*);
    return callVM<Fn, jit::DebugLeaveThenPopLexicalEnv>();
  };
  auto ifNotDebuggee = [this, scratch1]() {
    Register scratch2 = R1.scratchReg();
    masm.loadPtr(frame.addressOfEnvironmentChain(), scratch1);
    masm.debugAssertObjectHasClass(scratch1, scratch2,
                                   &LexicalEnvironmentObject::class_);
    Address enclosingAddr(scratch1,
                          EnvironmentObject::offsetOfEnclosingEnvironment());
    masm.unboxObject(enclosingAddr, scratch1);
    masm.storePtr(scratch1, frame.addressOfEnvironmentChain());
    return true;
  };
  return emitDebugInstrumentation(ifDebuggee, mozilla::Some(ifNotDebuggee));
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_FreshenLexicalEnv() {
  frame.syncStack(0);

  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());

  auto ifDebuggee = [this]() {
    prepareVMCall();
    pushBytecodePCArg();
    pushArg(R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, const jsbytecode*);
    return callVM<Fn, jit::DebuggeeFreshenLexicalEnv>();
  };
  auto ifNotDebuggee = [this]() {
    prepareVMCall();
    pushArg(R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*);
    return callVM<Fn, jit::FreshenLexicalEnv>();
  };
  return emitDebugInstrumentation(ifDebuggee, mozilla::Some(ifNotDebuggee));
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_RecreateLexicalEnv() {
  frame.syncStack(0);

  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());

  auto ifDebuggee = [this]() {
    prepareVMCall();
    pushBytecodePCArg();
    pushArg(R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, const jsbytecode*);
    return callVM<Fn, jit::DebuggeeRecreateLexicalEnv>();
  };
  auto ifNotDebuggee = [this]() {
    prepareVMCall();
    pushArg(R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*);
    return callVM<Fn, jit::RecreateLexicalEnv>();
  };
  return emitDebugInstrumentation(ifDebuggee, mozilla::Some(ifNotDebuggee));
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_DebugLeaveLexicalEnv() {
  auto ifDebuggee = [this]() {
    prepareVMCall();
    masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
    pushBytecodePCArg();
    pushArg(R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, const jsbytecode*);
    return callVM<Fn, jit::DebugLeaveLexicalEnv>();
  };
  return emitDebugInstrumentation(ifDebuggee);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_PushVarEnv() {
  prepareVMCall();
  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
  pushScriptGCThingArg(ScriptGCThingType::Scope, R1.scratchReg(),
                       R2.scratchReg());
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, Handle<Scope*>);
  return callVM<Fn, jit::PushVarEnv>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_EnterWith() {
  // Pop "with" object to R0.
  frame.popRegsAndSync(1);

  // Call a stub to push the object onto the environment chain.
  prepareVMCall();

  pushScriptGCThingArg(ScriptGCThingType::Scope, R1.scratchReg(),
                       R2.scratchReg());
  pushArg(R0);
  masm.loadBaselineFramePtr(FramePointer, R1.scratchReg());
  pushArg(R1.scratchReg());

  using Fn =
      bool (*)(JSContext*, BaselineFrame*, HandleValue, Handle<WithScope*>);
  return callVM<Fn, jit::EnterWith>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_LeaveWith() {
  // Call a stub to pop the with object from the environment chain.
  prepareVMCall();

  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*);
  return callVM<Fn, jit::LeaveWith>();
}

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
template <typename Handler>
bool BaselineCodeGen<Handler>::emit_AddDisposable() {
  // TODO: AddDisposable to be implemented for Baseline (Bug 1899500)
  MOZ_CRASH("AddDisposable has not been implemented for baseline");
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_DisposeDisposables() {
  // TODO: DisposeDisposables to be implemented for Baseline (Bug 1899500)
  MOZ_CRASH("DisposeDisposables has not been implemented for baseline");
}
#endif

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Exception() {
  prepareVMCall();

  using Fn = bool (*)(JSContext*, MutableHandleValue);
  if (!callVM<Fn, GetAndClearException>()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ExceptionAndStack() {
  // First call into the VM to store the exception stack.
  {
    prepareVMCall();

    using Fn = bool (*)(JSContext*, MutableHandleValue);
    if (!callVM<Fn, GetPendingExceptionStack>()) {
      return false;
    }

    frame.push(R0);
  }

  // Now get the actual exception value and clear the exception state.
  {
    prepareVMCall();

    using Fn = bool (*)(JSContext*, MutableHandleValue);
    if (!callVM<Fn, GetAndClearException>()) {
      return false;
    }

    frame.push(R0);
  }

  // Finally swap the stack and the exception.
  frame.popRegsAndSync(2);
  frame.push(R1);
  frame.push(R0);

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Debugger() {
  prepareVMCall();

  frame.assertSyncedStack();
  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*);
  if (!callVM<Fn, jit::OnDebuggerStatement>()) {
    return false;
  }

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitDebugEpilogue() {
  auto ifDebuggee = [this]() {
    // Move return value into the frame's rval slot.
    masm.storeValue(JSReturnOperand, frame.addressOfReturnValue());
    masm.or32(Imm32(BaselineFrame::HAS_RVAL), frame.addressOfFlags());

    // Load BaselineFrame pointer in R0.
    frame.syncStack(0);
    masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());

    prepareVMCall();
    pushBytecodePCArg();
    pushArg(R0.scratchReg());

    const RetAddrEntry::Kind kind = RetAddrEntry::Kind::DebugEpilogue;

    using Fn = bool (*)(JSContext*, BaselineFrame*, const jsbytecode*);
    if (!callVM<Fn, jit::DebugEpilogueOnBaselineReturn>(kind)) {
      return false;
    }

    masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
    return true;
  };
  return emitDebugInstrumentation(ifDebuggee);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitReturn() {
  if (handler.shouldEmitDebugEpilogueAtReturnOp()) {
    if (!emitDebugEpilogue()) {
      return false;
    }
  }

  // Only emit the jump if this JSOp::RetRval is not the last instruction.
  // Not needed for last instruction, because last instruction flows
  // into return label.
  if (!handler.isDefinitelyLastOp()) {
    masm.jump(&return_);
  }

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Return() {
  frame.assertStackDepth(1);

  frame.popValue(JSReturnOperand);
  return emitReturn();
}

template <typename Handler>
void BaselineCodeGen<Handler>::emitLoadReturnValue(ValueOperand val) {
  Label done, noRval;
  masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                    Imm32(BaselineFrame::HAS_RVAL), &noRval);
  masm.loadValue(frame.addressOfReturnValue(), val);
  masm.jump(&done);

  masm.bind(&noRval);
  masm.moveValue(UndefinedValue(), val);

  masm.bind(&done);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_RetRval() {
  frame.assertStackDepth(0);

  masm.moveValue(UndefinedValue(), JSReturnOperand);

  if (!handler.maybeScript() || !handler.maybeScript()->noScriptRval()) {
    // Return the value in the return value slot, if any.
    Label done;
    Address flags = frame.addressOfFlags();
    masm.branchTest32(Assembler::Zero, flags, Imm32(BaselineFrame::HAS_RVAL),
                      &done);
    masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
    masm.bind(&done);
  }

  return emitReturn();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ToPropertyKey() {
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ToAsyncIter() {
  frame.syncStack(0);
  masm.unboxObject(frame.addressOfStackValue(-2), R0.scratchReg());
  masm.loadValue(frame.addressOfStackValue(-1), R1);

  prepareVMCall();
  pushArg(R1);
  pushArg(R0.scratchReg());

  using Fn = JSObject* (*)(JSContext*, HandleObject, HandleValue);
  if (!callVM<Fn, js::CreateAsyncFromSyncIterator>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.popn(2);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CanSkipAwait() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  prepareVMCall();
  pushArg(R0);

  using Fn = bool (*)(JSContext*, HandleValue, bool* canSkip);
  if (!callVM<Fn, js::CanSkipAwait>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_BOOLEAN, ReturnReg, R0);
  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_MaybeExtractAwaitValue() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-2), R0);

  masm.unboxBoolean(frame.addressOfStackValue(-1), R1.scratchReg());

  Label cantExtract;
  masm.branchIfFalseBool(R1.scratchReg(), &cantExtract);

  prepareVMCall();
  pushArg(R0);

  using Fn = bool (*)(JSContext*, HandleValue, MutableHandleValue);
  if (!callVM<Fn, js::ExtractAwaitValue>()) {
    return false;
  }

  masm.storeValue(R0, frame.addressOfStackValue(-2));
  masm.bind(&cantExtract);

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_AsyncAwait() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-2), R1);
  masm.unboxObject(frame.addressOfStackValue(-1), R0.scratchReg());

  prepareVMCall();
  pushArg(R1);
  pushArg(R0.scratchReg());

  using Fn = JSObject* (*)(JSContext*, Handle<AsyncFunctionGeneratorObject*>,
                           HandleValue);
  if (!callVM<Fn, js::AsyncFunctionAwait>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.popn(2);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_AsyncResolve() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-2), R1);
  masm.unboxObject(frame.addressOfStackValue(-1), R0.scratchReg());

  prepareVMCall();
  pushArg(R1);
  pushArg(R0.scratchReg());

  using Fn = JSObject* (*)(JSContext*, Handle<AsyncFunctionGeneratorObject*>,
                           HandleValue);
  if (!callVM<Fn, js::AsyncFunctionResolve>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.popn(2);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_AsyncReject() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-3), R2);
  masm.loadValue(frame.addressOfStackValue(-2), R1);
  masm.unboxObject(frame.addressOfStackValue(-1), R0.scratchReg());

  prepareVMCall();
  pushArg(R1);
  pushArg(R2);
  pushArg(R0.scratchReg());

  using Fn = JSObject* (*)(JSContext*, Handle<AsyncFunctionGeneratorObject*>,
                           HandleValue, HandleValue);
  if (!callVM<Fn, js::AsyncFunctionReject>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.popn(3);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckObjCoercible() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  Label fail, done;

  masm.branchTestUndefined(Assembler::Equal, R0, &fail);
  masm.branchTestNull(Assembler::NotEqual, R0, &done);

  masm.bind(&fail);
  prepareVMCall();

  pushArg(R0);

  using Fn = bool (*)(JSContext*, HandleValue);
  if (!callVM<Fn, ThrowObjectCoercible>()) {
    return false;
  }

  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ToString() {
  // Keep top stack value in R0.
  frame.popRegsAndSync(1);

  // Inline path for string.
  Label done;
  masm.branchTestString(Assembler::Equal, R0, &done);

  prepareVMCall();

  pushArg(R0);

  // Call ToStringSlow which doesn't handle string inputs.
  using Fn = JSString* (*)(JSContext*, HandleValue);
  if (!callVM<Fn, ToStringSlowForVM<CanGC>>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_STRING, ReturnReg, R0);

  masm.bind(&done);
  frame.push(R0);
  return true;
}

static constexpr uint32_t TableSwitchOpLowOffset = 1 * JUMP_OFFSET_LEN;
static constexpr uint32_t TableSwitchOpHighOffset = 2 * JUMP_OFFSET_LEN;
static constexpr uint32_t TableSwitchOpFirstResumeIndexOffset =
    3 * JUMP_OFFSET_LEN;

template <>
void BaselineCompilerCodeGen::emitGetTableSwitchIndex(ValueOperand val,
                                                      Register dest,
                                                      Register scratch1,
                                                      Register scratch2) {
  jsbytecode* pc = handler.pc();
  jsbytecode* defaultpc = pc + GET_JUMP_OFFSET(pc);
  Label* defaultLabel = handler.labelOf(defaultpc);

  int32_t low = GET_JUMP_OFFSET(pc + TableSwitchOpLowOffset);
  int32_t high = GET_JUMP_OFFSET(pc + TableSwitchOpHighOffset);
  int32_t length = high - low + 1;

  // Jump to the 'default' pc if not int32 (tableswitch is only used when
  // all cases are int32).
  masm.branchTestInt32(Assembler::NotEqual, val, defaultLabel);
  masm.unboxInt32(val, dest);

  // Subtract 'low'. Bounds check.
  if (low != 0) {
    masm.sub32(Imm32(low), dest);
  }
  masm.branch32(Assembler::AboveOrEqual, dest, Imm32(length), defaultLabel);
}

template <>
void BaselineInterpreterCodeGen::emitGetTableSwitchIndex(ValueOperand val,
                                                         Register dest,
                                                         Register scratch1,
                                                         Register scratch2) {
  // Jump to the 'default' pc if not int32 (tableswitch is only used when
  // all cases are int32).
  Label done, jumpToDefault;
  masm.branchTestInt32(Assembler::NotEqual, val, &jumpToDefault);
  masm.unboxInt32(val, dest);

  Register pcReg = LoadBytecodePC(masm, scratch1);
  Address lowAddr(pcReg, sizeof(jsbytecode) + TableSwitchOpLowOffset);
  Address highAddr(pcReg, sizeof(jsbytecode) + TableSwitchOpHighOffset);

  // Jump to default if val > high.
  masm.branch32(Assembler::LessThan, highAddr, dest, &jumpToDefault);

  // Jump to default if val < low.
  masm.load32(lowAddr, scratch2);
  masm.branch32(Assembler::GreaterThan, scratch2, dest, &jumpToDefault);

  // index := val - low.
  masm.sub32(scratch2, dest);
  masm.jump(&done);

  masm.bind(&jumpToDefault);
  emitJump();

  masm.bind(&done);
}

template <>
void BaselineCompilerCodeGen::emitTableSwitchJump(Register key,
                                                  Register scratch1,
                                                  Register scratch2) {
  // Jump to resumeEntries[firstResumeIndex + key].

  // Note: BytecodeEmitter::allocateResumeIndex static_asserts
  // |firstResumeIndex * sizeof(uintptr_t)| fits in int32_t.
  uint32_t firstResumeIndex =
      GET_RESUMEINDEX(handler.pc() + TableSwitchOpFirstResumeIndexOffset);
  LoadBaselineScriptResumeEntries(masm, handler.script(), scratch1, scratch2);
  masm.loadPtr(BaseIndex(scratch1, key, ScaleFromElemWidth(sizeof(uintptr_t)),
                         firstResumeIndex * sizeof(uintptr_t)),
               scratch1);
  masm.jump(scratch1);
}

template <>
void BaselineInterpreterCodeGen::emitTableSwitchJump(Register key,
                                                     Register scratch1,
                                                     Register scratch2) {
  // Load the op's firstResumeIndex in scratch1.
  LoadUint24Operand(masm, TableSwitchOpFirstResumeIndexOffset, scratch1);

  masm.add32(key, scratch1);
  jumpToResumeEntry(scratch1, key, scratch2);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_TableSwitch() {
  frame.popRegsAndSync(1);

  Register key = R0.scratchReg();
  Register scratch1 = R1.scratchReg();
  Register scratch2 = R2.scratchReg();

  // Call a stub to convert R0 from double to int32 if needed.
  // Note: this stub may clobber scratch1.
  masm.call(cx->runtime()->jitRuntime()->getDoubleToInt32ValueStub());

  // Load the index in the jump table in |key|, or branch to default pc if not
  // int32 or out-of-range.
  emitGetTableSwitchIndex(R0, key, scratch1, scratch2);

  // Jump to the target pc.
  emitTableSwitchJump(key, scratch1, scratch2);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Iter() {
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_MoreIter() {
  frame.syncStack(0);

  masm.unboxObject(frame.addressOfStackValue(-1), R1.scratchReg());

  masm.iteratorMore(R1.scratchReg(), R0, R2.scratchReg());
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitIsMagicValue() {
  frame.syncStack(0);

  Label isMagic, done;
  masm.branchTestMagic(Assembler::Equal, frame.addressOfStackValue(-1),
                       &isMagic);
  masm.moveValue(BooleanValue(false), R0);
  masm.jump(&done);

  masm.bind(&isMagic);
  masm.moveValue(BooleanValue(true), R0);

  masm.bind(&done);
  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_IsNoIter() {
  return emitIsMagicValue();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_EndIter() {
  // Pop iterator value.
  frame.pop();

  // Pop the iterator object to close in R0.
  frame.popRegsAndSync(1);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  MOZ_ASSERT(!regs.has(FramePointer));
  if (HasInterpreterPCReg()) {
    regs.take(InterpreterPCReg);
  }

  Register obj = R0.scratchReg();
  regs.take(obj);
  masm.unboxObject(R0, obj);

  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();
  Register temp3 = regs.takeAny();
  masm.iteratorClose(obj, temp1, temp2, temp3);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CloseIter() {
  frame.popRegsAndSync(1);

  Register iter = R0.scratchReg();
  masm.unboxObject(R0, iter);

  return emitNextIC();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_OptimizeGetIterator() {
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_IsGenClosing() {
  return emitIsMagicValue();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_IsNullOrUndefined() {
  frame.syncStack(0);

  Label isNullOrUndefined, done;
  masm.branchTestNull(Assembler::Equal, frame.addressOfStackValue(-1),
                      &isNullOrUndefined);
  masm.branchTestUndefined(Assembler::Equal, frame.addressOfStackValue(-1),
                           &isNullOrUndefined);
  masm.moveValue(BooleanValue(false), R0);
  masm.jump(&done);

  masm.bind(&isNullOrUndefined);
  masm.moveValue(BooleanValue(true), R0);

  masm.bind(&done);
  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_GetRval() {
  frame.syncStack(0);

  emitLoadReturnValue(R0);

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SetRval() {
  // Store to the frame's return value slot.
  frame.storeStackValue(-1, frame.addressOfReturnValue(), R2);
  masm.or32(Imm32(BaselineFrame::HAS_RVAL), frame.addressOfFlags());
  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Callee() {
  MOZ_ASSERT_IF(handler.maybeScript(), handler.maybeScript()->function());
  frame.syncStack(0);
  masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(),
                                   R0.scratchReg());
  masm.tagValue(JSVAL_TYPE_OBJECT, R0.scratchReg(), R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_EnvCallee() {
  frame.syncStack(0);
  uint8_t numHops = GET_UINT8(handler.pc());
  Register scratch = R0.scratchReg();

  masm.loadPtr(frame.addressOfEnvironmentChain(), scratch);
  for (unsigned i = 0; i < numHops; i++) {
    Address nextAddr(scratch,
                     EnvironmentObject::offsetOfEnclosingEnvironment());
    masm.unboxObject(nextAddr, scratch);
  }

  masm.loadValue(Address(scratch, CallObject::offsetOfCallee()), R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_EnvCallee() {
  Register scratch = R0.scratchReg();
  Register env = R1.scratchReg();

  static_assert(JSOpLength_EnvCallee - sizeof(jsbytecode) == ENVCOORD_HOPS_LEN,
                "op must have uint8 operand for LoadAliasedVarEnv");

  // Load the right environment object.
  masm.loadPtr(frame.addressOfEnvironmentChain(), env);
  LoadAliasedVarEnv(masm, env, scratch);

  masm.pushValue(Address(env, CallObject::offsetOfCallee()));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SuperBase() {
  frame.popRegsAndSync(1);

  Register scratch = R0.scratchReg();
  Register proto = R1.scratchReg();

  // Unbox callee.
  masm.unboxObject(R0, scratch);

  // Load [[HomeObject]]
  Address homeObjAddr(scratch,
                      FunctionExtended::offsetOfMethodHomeObjectSlot());

  masm.assertFunctionIsExtended(scratch);
#ifdef DEBUG
  Label isObject;
  masm.branchTestObject(Assembler::Equal, homeObjAddr, &isObject);
  masm.assumeUnreachable("[[HomeObject]] must be Object");
  masm.bind(&isObject);
#endif
  masm.unboxObject(homeObjAddr, scratch);

  // Load prototype from [[HomeObject]]
  masm.loadObjProto(scratch, proto);

#ifdef DEBUG
  // We won't encounter a lazy proto, because the prototype is guaranteed to
  // either be a JSFunction or a PlainObject, and only proxy objects can have a
  // lazy proto.
  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

  Label proxyCheckDone;
  masm.branchPtr(Assembler::NotEqual, proto, ImmWord(1), &proxyCheckDone);
  masm.assumeUnreachable("Unexpected lazy proto in JSOp::SuperBase");
  masm.bind(&proxyCheckDone);
#endif

  Label nullProto, done;
  masm.branchPtr(Assembler::Equal, proto, ImmWord(0), &nullProto);

  // Box prototype and return
  masm.tagValue(JSVAL_TYPE_OBJECT, proto, R1);
  masm.jump(&done);

  masm.bind(&nullProto);
  masm.moveValue(NullValue(), R1);

  masm.bind(&done);
  frame.push(R1);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_SuperFun() {
  frame.popRegsAndSync(1);

  Register callee = R0.scratchReg();
  Register proto = R1.scratchReg();
#ifdef DEBUG
  Register scratch = R2.scratchReg();
#endif

  // Unbox callee.
  masm.unboxObject(R0, callee);

#ifdef DEBUG
  Label classCheckDone;
  masm.branchTestObjIsFunction(Assembler::Equal, callee, scratch, callee,
                               &classCheckDone);
  masm.assumeUnreachable("Unexpected non-JSFunction callee in JSOp::SuperFun");
  masm.bind(&classCheckDone);
#endif

  // Load prototype of callee
  masm.loadObjProto(callee, proto);

#ifdef DEBUG
  // We won't encounter a lazy proto, because |callee| is guaranteed to be a
  // JSFunction and only proxy objects can have a lazy proto.
  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

  Label proxyCheckDone;
  masm.branchPtr(Assembler::NotEqual, proto, ImmWord(1), &proxyCheckDone);
  masm.assumeUnreachable("Unexpected lazy proto in JSOp::SuperFun");
  masm.bind(&proxyCheckDone);
#endif

  Label nullProto, done;
  masm.branchPtr(Assembler::Equal, proto, ImmWord(0), &nullProto);

  // Box prototype and return
  masm.tagValue(JSVAL_TYPE_OBJECT, proto, R1);
  masm.jump(&done);

  masm.bind(&nullProto);
  masm.moveValue(NullValue(), R1);

  masm.bind(&done);
  frame.push(R1);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Arguments() {
  frame.syncStack(0);

  MOZ_ASSERT_IF(handler.maybeScript(), handler.maybeScript()->needsArgsObj());

  prepareVMCall();

  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, MutableHandleValue);
  if (!callVM<Fn, jit::NewArgumentsObject>()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Rest() {
  frame.syncStack(0);

  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Generator() {
  frame.assertStackDepth(0);

  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());

  prepareVMCall();
  pushArg(R0.scratchReg());

  using Fn = JSObject* (*)(JSContext*, BaselineFrame*);
  if (!callVM<Fn, jit::CreateGeneratorFromFrame>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitSuspend(JSOp op) {
  MOZ_ASSERT(op == JSOp::InitialYield || op == JSOp::Yield ||
             op == JSOp::Await);

  // Load the generator object in R2, but leave the return value on the
  // expression stack.
  Register genObj = R2.scratchReg();
  if (op == JSOp::InitialYield) {
    // Generator and return value are one and the same.
    frame.syncStack(0);
    frame.assertStackDepth(1);
    masm.unboxObject(frame.addressOfStackValue(-1), genObj);
  } else {
    frame.popRegsAndSync(1);
    masm.unboxObject(R0, genObj);
  }

  if (frame.hasKnownStackDepth(1) && !handler.canHaveFixedSlots()) {
    // If the expression stack is empty, we can inline the Yield. Note that this
    // branch is never taken for the interpreter because it doesn't know static
    // stack depths.
    MOZ_ASSERT_IF(op == JSOp::InitialYield && handler.maybePC(),
                  GET_RESUMEINDEX(handler.maybePC()) == 0);
    Address resumeIndexSlot(genObj,
                            AbstractGeneratorObject::offsetOfResumeIndexSlot());
    Register temp = R1.scratchReg();
    if (op == JSOp::InitialYield) {
      masm.storeValue(Int32Value(0), resumeIndexSlot);
    } else {
      jsbytecode* pc = handler.maybePC();
      MOZ_ASSERT(pc, "compiler-only code never has a null pc");
      masm.move32(Imm32(GET_RESUMEINDEX(pc)), temp);
      masm.storeValue(JSVAL_TYPE_INT32, temp, resumeIndexSlot);
    }

    Register envObj = R0.scratchReg();
    Address envChainSlot(
        genObj, AbstractGeneratorObject::offsetOfEnvironmentChainSlot());
    masm.loadPtr(frame.addressOfEnvironmentChain(), envObj);
    masm.guardedCallPreBarrierAnyZone(envChainSlot, MIRType::Value, temp);
    masm.storeValue(JSVAL_TYPE_OBJECT, envObj, envChainSlot);

    Label skipBarrier;
    masm.branchPtrInNurseryChunk(Assembler::Equal, genObj, temp, &skipBarrier);
    masm.branchPtrInNurseryChunk(Assembler::NotEqual, envObj, temp,
                                 &skipBarrier);
    MOZ_ASSERT(genObj == R2.scratchReg());
    masm.call(&postBarrierSlot_);
    masm.bind(&skipBarrier);
  } else {
    masm.loadBaselineFramePtr(FramePointer, R1.scratchReg());
    computeFrameSize(R0.scratchReg());

    prepareVMCall();
    pushBytecodePCArg();
    pushArg(R0.scratchReg());
    pushArg(R1.scratchReg());
    pushArg(genObj);

    using Fn = bool (*)(JSContext*, HandleObject, BaselineFrame*, uint32_t,
                        const jsbytecode*);
    if (!callVM<Fn, jit::NormalSuspend>()) {
      return false;
    }
  }

  masm.loadValue(frame.addressOfStackValue(-1), JSReturnOperand);
  if (!emitReturn()) {
    return false;
  }

  // Three values are pushed onto the stack when resuming the generator,
  // replacing the one slot that holds the return value.
  frame.incStackDepth(2);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitialYield() {
  return emitSuspend(JSOp::InitialYield);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Yield() {
  return emitSuspend(JSOp::Yield);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Await() {
  return emitSuspend(JSOp::Await);
}

template <>
template <typename F>
bool BaselineCompilerCodeGen::emitAfterYieldDebugInstrumentation(
    const F& ifDebuggee, Register) {
  if (handler.compileDebugInstrumentation()) {
    return ifDebuggee();
  }
  return true;
}

template <>
template <typename F>
bool BaselineInterpreterCodeGen::emitAfterYieldDebugInstrumentation(
    const F& ifDebuggee, Register scratch) {
  // Note that we can't use emitDebugInstrumentation here because the frame's
  // DEBUGGEE flag hasn't been initialized yet.

  // If the current Realm is not a debuggee we're done.
  Label done;
  CodeOffset toggleOffset = masm.toggledJump(&done);
  if (!handler.addDebugInstrumentationOffset(cx, toggleOffset)) {
    return false;
  }
  masm.loadPtr(AbsoluteAddress(cx->addressOfRealm()), scratch);
  masm.branchTest32(Assembler::Zero,
                    Address(scratch, Realm::offsetOfDebugModeBits()),
                    Imm32(Realm::debugModeIsDebuggeeBit()), &done);

  if (!ifDebuggee()) {
    return false;
  }

  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_AfterYield() {
  if (!emit_JumpTarget()) {
    return false;
  }

  auto ifDebuggee = [this]() {
    frame.assertSyncedStack();
    masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
    prepareVMCall();
    pushArg(R0.scratchReg());

    const RetAddrEntry::Kind kind = RetAddrEntry::Kind::DebugAfterYield;

    using Fn = bool (*)(JSContext*, BaselineFrame*);
    if (!callVM<Fn, jit::DebugAfterYield>(kind)) {
      return false;
    }

    return true;
  };
  return emitAfterYieldDebugInstrumentation(ifDebuggee, R0.scratchReg());
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_FinalYieldRval() {
  // Store generator in R0.
  frame.popRegsAndSync(1);
  masm.unboxObject(R0, R0.scratchReg());

  prepareVMCall();
  pushBytecodePCArg();
  pushArg(R0.scratchReg());

  using Fn = bool (*)(JSContext*, HandleObject, const jsbytecode*);
  if (!callVM<Fn, jit::FinalSuspend>()) {
    return false;
  }

  masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
  return emitReturn();
}

template <>
void BaselineCompilerCodeGen::emitJumpToInterpretOpLabel() {
  TrampolinePtr code =
      cx->runtime()->jitRuntime()->baselineInterpreter().interpretOpAddr();
  masm.jump(code);
}

template <>
void BaselineInterpreterCodeGen::emitJumpToInterpretOpLabel() {
  masm.jump(handler.interpretOpLabel());
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitEnterGeneratorCode(Register script,
                                                      Register resumeIndex,
                                                      Register scratch) {
  // Resume in either the BaselineScript (if present) or Baseline Interpreter.

  static_assert(BaselineDisabledScript == 0x1,
                "Comparison below requires specific sentinel encoding");

  // Initialize the icScript slot in the baseline frame.
  masm.loadJitScript(script, scratch);
  masm.computeEffectiveAddress(Address(scratch, JitScript::offsetOfICScript()),
                               scratch);
  Address icScriptAddr(FramePointer, BaselineFrame::reverseOffsetOfICScript());
  masm.storePtr(scratch, icScriptAddr);

  Label noBaselineScript;
  masm.loadJitScript(script, scratch);
  masm.loadPtr(Address(scratch, JitScript::offsetOfBaselineScript()), scratch);
  masm.branchPtr(Assembler::BelowOrEqual, scratch,
                 ImmPtr(BaselineDisabledScriptPtr), &noBaselineScript);

  masm.load32(Address(scratch, BaselineScript::offsetOfResumeEntriesOffset()),
              script);
  masm.addPtr(scratch, script);
  masm.loadPtr(
      BaseIndex(script, resumeIndex, ScaleFromElemWidth(sizeof(uintptr_t))),
      scratch);
  masm.jump(scratch);

  masm.bind(&noBaselineScript);

  // Initialize interpreter frame fields.
  Address flagsAddr(FramePointer, BaselineFrame::reverseOffsetOfFlags());
  Address scriptAddr(FramePointer,
                     BaselineFrame::reverseOffsetOfInterpreterScript());
  masm.or32(Imm32(BaselineFrame::RUNNING_IN_INTERPRETER), flagsAddr);
  masm.storePtr(script, scriptAddr);

  // Initialize pc and jump to it.
  emitInterpJumpToResumeEntry(script, resumeIndex, scratch);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_Resume() {
  frame.syncStack(0);
  masm.assertStackAlignment(sizeof(Value), 0);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  MOZ_ASSERT(!regs.has(FramePointer));
  if (HasInterpreterPCReg()) {
    regs.take(InterpreterPCReg);
  }

  saveInterpreterPCReg();

  // Load generator object.
  Register genObj = regs.takeAny();
  masm.unboxObject(frame.addressOfStackValue(-3), genObj);

  // Load callee.
  Register callee = regs.takeAny();
  masm.unboxObject(
      Address(genObj, AbstractGeneratorObject::offsetOfCalleeSlot()), callee);

  // Save a pointer to the JSOp::Resume operand stack Values.
  Register callerStackPtr = regs.takeAny();
  masm.computeEffectiveAddress(frame.addressOfStackValue(-1), callerStackPtr);

  // Branch to |interpret| to resume the generator in the C++ interpreter if the
  // script does not have a JitScript.
  Label interpret;
  Register scratch1 = regs.takeAny();
  masm.loadPrivate(Address(callee, JSFunction::offsetOfJitInfoOrScript()),
                   scratch1);
  masm.branchIfScriptHasNoJitScript(scratch1, &interpret);

  // Push |undefined| for all formals.
  Register scratch2 = regs.takeAny();
  Label loop, loopDone;
  masm.loadFunctionArgCount(callee, scratch2);

  static_assert(sizeof(Value) == 8);
#ifndef JS_CODEGEN_NONE
  static_assert(JitStackAlignment == 16 || JitStackAlignment == 8);
#endif
  // If JitStackValueAlignment == 1, then we were already correctly aligned on
  // entry, as guaranteed by the assertStackAlignment at the entry to this
  // function.
  if (JitStackValueAlignment > 1) {
    Register alignment = regs.takeAny();
    masm.moveStackPtrTo(alignment);
    masm.alignJitStackBasedOnNArgs(scratch2, false);

    // Compute alignment adjustment.
    masm.subStackPtrFrom(alignment);

    // Some code, like BaselineFrame::trace, will inspect the whole range of
    // the stack frame. In order to ensure that garbage data left behind from
    // previous activations doesn't confuse other machinery, we zero out the
    // alignment bytes.
    Label alignmentZero;
    masm.branchPtr(Assembler::Equal, alignment, ImmWord(0), &alignmentZero);

    // Since we know prior to the stack alignment that the stack was 8 byte
    // aligned, and JitStackAlignment is 8 or 16 bytes, if we are doing an
    // alignment then we -must- have aligned by subtracting 8 bytes from
    // the stack pointer.
    //
    // So we can freely store a valid double here.
    masm.storeValue(DoubleValue(0), Address(masm.getStackPointer(), 0));
    masm.bind(&alignmentZero);
  }

  masm.branchTest32(Assembler::Zero, scratch2, scratch2, &loopDone);
  masm.bind(&loop);
  {
    masm.pushValue(UndefinedValue());
    masm.branchSub32(Assembler::NonZero, Imm32(1), scratch2, &loop);
  }
  masm.bind(&loopDone);

  // Push |undefined| for |this|.
  masm.pushValue(UndefinedValue());

#ifdef DEBUG
  // Update BaselineFrame debugFrameSize field.
  masm.mov(FramePointer, scratch2);
  masm.subStackPtrFrom(scratch2);
  masm.store32(scratch2, frame.addressOfDebugFrameSize());
#endif

  masm.PushCalleeToken(callee, /* constructing = */ false);
  masm.pushFrameDescriptorForJitCall(FrameType::BaselineJS, /* argc = */ 0);

  // PushCalleeToken bumped framePushed. Reset it.
  MOZ_ASSERT(masm.framePushed() == sizeof(uintptr_t));
  masm.setFramePushed(0);

  regs.add(callee);

  // Push a fake return address on the stack. We will resume here when the
  // generator returns.
  Label genStart, returnTarget;
#ifdef JS_USE_LINK_REGISTER
  masm.call(&genStart);
#else
  masm.callAndPushReturnAddress(&genStart);
#endif

  // Record the return address so the return offset -> pc mapping works.
  if (!handler.recordCallRetAddr(cx, RetAddrEntry::Kind::IC,
                                 masm.currentOffset())) {
    return false;
  }

  masm.jump(&returnTarget);
  masm.bind(&genStart);
#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  // Construct BaselineFrame.
  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  // If profiler instrumentation is on, update lastProfilingFrame on
  // current JitActivation
  {
    Register scratchReg = scratch2;
    Label skip;
    AbsoluteAddress addressOfEnabled(
        cx->runtime()->geckoProfiler().addressOfEnabled());
    masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skip);
    masm.loadJSContext(scratchReg);
    masm.loadPtr(Address(scratchReg, JSContext::offsetOfProfilingActivation()),
                 scratchReg);
    masm.storePtr(
        FramePointer,
        Address(scratchReg, JitActivation::offsetOfLastProfilingFrame()));
    masm.bind(&skip);
  }

  masm.subFromStackPtr(Imm32(BaselineFrame::Size()));
  masm.assertStackAlignment(sizeof(Value), 0);

  // Store flags and env chain.
  masm.store32(Imm32(BaselineFrame::HAS_INITIAL_ENV), frame.addressOfFlags());
  masm.unboxObject(
      Address(genObj, AbstractGeneratorObject::offsetOfEnvironmentChainSlot()),
      scratch2);
  masm.storePtr(scratch2, frame.addressOfEnvironmentChain());

  // Store the arguments object if there is one.
  Label noArgsObj;
  Address argsObjSlot(genObj, AbstractGeneratorObject::offsetOfArgsObjSlot());
  masm.fallibleUnboxObject(argsObjSlot, scratch2, &noArgsObj);
  {
    masm.storePtr(scratch2, frame.addressOfArgsObj());
    masm.or32(Imm32(BaselineFrame::HAS_ARGS_OBJ), frame.addressOfFlags());
  }
  masm.bind(&noArgsObj);

  // Push locals and expression slots if needed.
  Label noStackStorage;
  Address stackStorageSlot(genObj,
                           AbstractGeneratorObject::offsetOfStackStorageSlot());
  masm.fallibleUnboxObject(stackStorageSlot, scratch2, &noStackStorage);
  {
    Register initLength = regs.takeAny();
    masm.loadPtr(Address(scratch2, NativeObject::offsetOfElements()), scratch2);
    masm.load32(Address(scratch2, ObjectElements::offsetOfInitializedLength()),
                initLength);
    masm.store32(
        Imm32(0),
        Address(scratch2, ObjectElements::offsetOfInitializedLength()));

    Label loop, loopDone;
    masm.branchTest32(Assembler::Zero, initLength, initLength, &loopDone);
    masm.bind(&loop);
    {
      masm.pushValue(Address(scratch2, 0));
      masm.guardedCallPreBarrierAnyZone(Address(scratch2, 0), MIRType::Value,
                                        scratch1);
      masm.addPtr(Imm32(sizeof(Value)), scratch2);
      masm.branchSub32(Assembler::NonZero, Imm32(1), initLength, &loop);
    }
    masm.bind(&loopDone);
    regs.add(initLength);
  }

  masm.bind(&noStackStorage);

  // Push arg, generator, resumeKind stack Values, in that order.
  masm.pushValue(Address(callerStackPtr, sizeof(Value)));
  masm.pushValue(JSVAL_TYPE_OBJECT, genObj);
  masm.pushValue(Address(callerStackPtr, 0));

  masm.switchToObjectRealm(genObj, scratch2);

  // Load script in scratch1.
  masm.unboxObject(
      Address(genObj, AbstractGeneratorObject::offsetOfCalleeSlot()), scratch1);
  masm.loadPrivate(Address(scratch1, JSFunction::offsetOfJitInfoOrScript()),
                   scratch1);

  // Load resume index in scratch2 and mark generator as running.
  Address resumeIndexSlot(genObj,
                          AbstractGeneratorObject::offsetOfResumeIndexSlot());
  masm.unboxInt32(resumeIndexSlot, scratch2);
  masm.storeValue(Int32Value(AbstractGeneratorObject::RESUME_INDEX_RUNNING),
                  resumeIndexSlot);

  if (!emitEnterGeneratorCode(scratch1, scratch2, regs.getAny())) {
    return false;
  }

  // Call into the VM to resume the generator in the C++ interpreter if there's
  // no JitScript.
  masm.bind(&interpret);

  prepareVMCall();

  pushArg(callerStackPtr);
  pushArg(genObj);

  using Fn = bool (*)(JSContext*, HandleObject, Value*, MutableHandleValue);
  if (!callVM<Fn, jit::InterpretResume>()) {
    return false;
  }

  masm.bind(&returnTarget);

  // Restore Stack pointer
  masm.computeEffectiveAddress(frame.addressOfStackValue(-1),
                               masm.getStackPointer());

  // After the generator returns, we restore the stack pointer, switch back to
  // the current realm, push the return value, and we're done.
  if (JSScript* script = handler.maybeScript()) {
    masm.switchToRealm(script->realm(), R2.scratchReg());
  } else {
    masm.switchToBaselineFrameRealm(R2.scratchReg());
  }
  restoreInterpreterPCReg();
  frame.popn(3);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckResumeKind() {
  // Load resumeKind in R1, generator in R0.
  frame.popRegsAndSync(2);

#ifdef DEBUG
  Label ok;
  masm.branchTestInt32(Assembler::Equal, R1, &ok);
  masm.assumeUnreachable("Expected int32 resumeKind");
  masm.bind(&ok);
#endif

  // If resumeKind is 'next' we don't have to do anything.
  Label done;
  masm.unboxInt32(R1, R1.scratchReg());
  masm.branch32(Assembler::Equal, R1.scratchReg(),
                Imm32(int32_t(GeneratorResumeKind::Next)), &done);

  prepareVMCall();

  pushArg(R1.scratchReg());  // resumeKind

  masm.loadValue(frame.addressOfStackValue(-1), R2);
  pushArg(R2);  // arg

  masm.unboxObject(R0, R0.scratchReg());
  pushArg(R0.scratchReg());  // genObj

  masm.loadBaselineFramePtr(FramePointer, R2.scratchReg());
  pushArg(R2.scratchReg());  // frame

  using Fn = bool (*)(JSContext*, BaselineFrame*,
                      Handle<AbstractGeneratorObject*>, HandleValue, int32_t);
  if (!callVM<Fn, jit::GeneratorThrowOrReturn>()) {
    return false;
  }

  masm.bind(&done);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_ResumeKind() {
  GeneratorResumeKind resumeKind = ResumeKindFromPC(handler.pc());
  frame.push(Int32Value(int32_t(resumeKind)));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_ResumeKind() {
  LoadUint8Operand(masm, R0.scratchReg());
  masm.tagValue(JSVAL_TYPE_INT32, R0.scratchReg(), R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_DebugCheckSelfHosted() {
#ifdef DEBUG
  frame.syncStack(0);

  masm.loadValue(frame.addressOfStackValue(-1), R0);

  prepareVMCall();
  pushArg(R0);

  using Fn = bool (*)(JSContext*, HandleValue);
  if (!callVM<Fn, js::Debug_CheckSelfHosted>()) {
    return false;
  }
#endif
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_IsConstructing() {
  frame.push(MagicValue(JS_IS_CONSTRUCTING));
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_JumpTarget() {
  MaybeIncrementCodeCoverageCounter(masm, handler.script(), handler.pc());
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_JumpTarget() {
  Register scratch1 = R0.scratchReg();
  Register scratch2 = R1.scratchReg();

  Label skipCoverage;
  CodeOffset toggleOffset = masm.toggledJump(&skipCoverage);
  masm.call(handler.codeCoverageAtPCLabel());
  masm.bind(&skipCoverage);
  if (!handler.codeCoverageOffsets().append(toggleOffset.offset())) {
    return false;
  }

  // Load icIndex in scratch1.
  LoadInt32Operand(masm, scratch1);

  // Compute ICEntry* and store to frame->interpreterICEntry.
  masm.loadPtr(frame.addressOfICScript(), scratch2);
  static_assert(sizeof(ICEntry) == sizeof(uintptr_t));
  masm.computeEffectiveAddress(BaseIndex(scratch2, scratch1, ScalePointer,
                                         ICScript::offsetOfICEntries()),
                               scratch2);
  masm.storePtr(scratch2, frame.addressOfInterpreterICEntry());
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_CheckClassHeritage() {
  frame.syncStack(0);

  // Leave the heritage value on the stack.
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  prepareVMCall();
  pushArg(R0);

  using Fn = bool (*)(JSContext*, HandleValue);
  return callVM<Fn, js::CheckClassHeritageOperation>();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_InitHomeObject() {
  // Load HomeObject in R0.
  frame.popRegsAndSync(1);

  // Load function off stack
  Register func = R2.scratchReg();
  masm.unboxObject(frame.addressOfStackValue(-1), func);

  masm.assertFunctionIsExtended(func);

  // Set HOMEOBJECT_SLOT
  Register temp = R1.scratchReg();
  Address addr(func, FunctionExtended::offsetOfMethodHomeObjectSlot());
  masm.guardedCallPreBarrierAnyZone(addr, MIRType::Value, temp);
  masm.storeValue(R0, addr);

  Label skipBarrier;
  masm.branchPtrInNurseryChunk(Assembler::Equal, func, temp, &skipBarrier);
  masm.branchValueIsNurseryCell(Assembler::NotEqual, R0, temp, &skipBarrier);
  masm.call(&postBarrierSlot_);
  masm.bind(&skipBarrier);

  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_BuiltinObject() {
  // Built-in objects are constants for a given global.
  auto kind = BuiltinObjectKind(GET_UINT8(handler.pc()));
  JSObject* builtin = BuiltinObjectOperation(cx, kind);
  if (!builtin) {
    return false;
  }
  frame.push(ObjectValue(*builtin));
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_BuiltinObject() {
  prepareVMCall();

  pushUint8BytecodeOperandArg(R0.scratchReg());

  using Fn = JSObject* (*)(JSContext*, BuiltinObjectKind);
  if (!callVM<Fn, BuiltinObjectOperation>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_ObjWithProto() {
  frame.syncStack(0);

  // Leave the proto value on the stack for the decompiler
  masm.loadValue(frame.addressOfStackValue(-1), R0);

  prepareVMCall();
  pushArg(R0);

  using Fn = PlainObject* (*)(JSContext*, HandleValue);
  if (!callVM<Fn, js::ObjectWithProtoOperation>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.pop();
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_FunWithProto() {
  frame.popRegsAndSync(1);

  masm.unboxObject(R0, R0.scratchReg());
  masm.loadPtr(frame.addressOfEnvironmentChain(), R1.scratchReg());

  prepareVMCall();
  pushArg(R0.scratchReg());
  pushArg(R1.scratchReg());
  pushScriptGCThingArg(ScriptGCThingType::Function, R0.scratchReg(),
                       R1.scratchReg());

  using Fn =
      JSObject* (*)(JSContext*, HandleFunction, HandleObject, HandleObject);
  if (!callVM<Fn, js::FunWithProtoOperation>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_ImportMeta() {
  // Note: this is like the interpreter implementation, but optimized a bit by
  // calling GetModuleObjectForScript at compile-time.

  Rooted<ModuleObject*> module(cx, GetModuleObjectForScript(handler.script()));

  frame.syncStack(0);

  prepareVMCall();
  pushArg(ImmGCPtr(module));

  using Fn = JSObject* (*)(JSContext*, HandleObject);
  if (!callVM<Fn, js::GetOrCreateModuleMetaObject>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineInterpreterCodeGen::emit_ImportMeta() {
  prepareVMCall();

  pushScriptArg();

  using Fn = JSObject* (*)(JSContext*, HandleScript);
  if (!callVM<Fn, ImportMetaOperation>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_DynamicImport() {
  // Put specifier into R0 and object value into R1
  frame.popRegsAndSync(2);

  prepareVMCall();
  pushArg(R1);
  pushArg(R0);
  pushScriptArg();

  using Fn = JSObject* (*)(JSContext*, HandleScript, HandleValue, HandleValue);
  if (!callVM<Fn, js::StartDynamicModuleImport>()) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <>
bool BaselineCompilerCodeGen::emit_ForceInterpreter() {
  // Caller is responsible for checking script->hasForceInterpreterOp().
  MOZ_CRASH("JSOp::ForceInterpreter in baseline");
}

template <>
bool BaselineInterpreterCodeGen::emit_ForceInterpreter() {
  masm.assumeUnreachable("JSOp::ForceInterpreter");
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitPrologue() {
  AutoCreatedBy acb(masm, "BaselineCodeGen<Handler>::emitPrologue");

#ifdef JS_USE_LINK_REGISTER
  // Push link register from generateEnterJIT()'s BLR.
  masm.pushReturnAddress();
#endif

  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  masm.checkStackAlignment();

  emitProfilerEnterFrame();

  masm.subFromStackPtr(Imm32(BaselineFrame::Size()));

  // Initialize BaselineFrame. Also handles env chain pre-initialization (in
  // case GC gets run during stack check). For global and eval scripts, the env
  // chain is in R1. For function scripts, the env chain is in the callee.
  emitInitFrameFields(R1.scratchReg());

  // When compiling with Debugger instrumentation, set the debuggeeness of
  // the frame before any operation that can call into the VM.
  if (!emitIsDebuggeeCheck()) {
    return false;
  }

  // Initialize the env chain before any operation that may call into the VM and
  // trigger a GC.
  if (!initEnvironmentChain()) {
    return false;
  }

  // Check for overrecursion before initializing locals.
  if (!emitStackCheck()) {
    return false;
  }

  emitInitializeLocals();

  // Ion prologue bailouts will enter here in the Baseline Interpreter.
  masm.bind(&bailoutPrologue_);

  frame.assertSyncedStack();

  if (JSScript* script = handler.maybeScript()) {
    masm.debugAssertContextRealm(script->realm(), R1.scratchReg());
  }

  if (!emitDebugPrologue()) {
    return false;
  }

  if (!emitHandleCodeCoverageAtPrologue()) {
    return false;
  }

  if (!emitWarmUpCounterIncrement()) {
    return false;
  }

  warmUpCheckPrologueOffset_ = CodeOffset(masm.currentOffset());

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitEpilogue() {
  AutoCreatedBy acb(masm, "BaselineCodeGen<Handler>::emitEpilogue");

  masm.bind(&return_);

  if (!handler.shouldEmitDebugEpilogueAtReturnOp()) {
    if (!emitDebugEpilogue()) {
      return false;
    }
  }

  emitProfilerExitFrame();

  masm.moveToStackPtr(FramePointer);
  masm.pop(FramePointer);

  masm.ret();
  return true;
}

MethodStatus BaselineCompiler::emitBody() {
  AutoCreatedBy acb(masm, "BaselineCompiler::emitBody");

  JSScript* script = handler.script();
  MOZ_ASSERT(handler.pc() == script->code());

  mozilla::DebugOnly<jsbytecode*> prevpc = handler.pc();

  while (true) {
    JSOp op = JSOp(*handler.pc());
    JitSpew(JitSpew_BaselineOp, "Compiling op @ %d: %s",
            int(script->pcToOffset(handler.pc())), CodeName(op));

    BytecodeInfo* info = handler.analysis().maybeInfo(handler.pc());

    // Skip unreachable ops.
    if (!info) {
      // Test if last instructions and stop emitting in that case.
      handler.moveToNextPC();
      if (handler.pc() >= script->codeEnd()) {
        break;
      }

      prevpc = handler.pc();
      continue;
    }

    if (info->jumpTarget) {
      // Fully sync the stack if there are incoming jumps.
      frame.syncStack(0);
      frame.setStackDepth(info->stackDepth);
      masm.bind(handler.labelOf(handler.pc()));
    } else if (MOZ_UNLIKELY(compileDebugInstrumentation())) {
      // Also fully sync the stack if the debugger is enabled.
      frame.syncStack(0);
    } else {
      // At the beginning of any op, at most the top 2 stack-values are
      // unsynced.
      if (frame.stackDepth() > 2) {
        frame.syncStack(2);
      }
    }

    frame.assertValidState(*info);

    // If the script has a resume offset for this pc we need to keep track of
    // the native code offset.
    if (info->hasResumeOffset) {
      frame.assertSyncedStack();
      uint32_t pcOffset = script->pcToOffset(handler.pc());
      uint32_t nativeOffset = masm.currentOffset();
      if (!resumeOffsetEntries_.emplaceBack(pcOffset, nativeOffset)) {
        ReportOutOfMemory(cx);
        return Method_Error;
      }
    }

    // Emit traps for breakpoints and step mode.
    if (MOZ_UNLIKELY(compileDebugInstrumentation()) && !emitDebugTrap()) {
      return Method_Error;
    }

    perfSpewer_.recordInstruction(cx, masm, handler.pc(), frame);

#define EMIT_OP(OP, ...)                                       \
  case JSOp::OP: {                                             \
    AutoCreatedBy acb(masm, "op=" #OP);                        \
    if (MOZ_UNLIKELY(!this->emit_##OP())) return Method_Error; \
  } break;

    switch (op) {
      FOR_EACH_OPCODE(EMIT_OP)
      default:
        MOZ_CRASH("Unexpected op");
    }

#undef EMIT_OP

    MOZ_ASSERT(masm.framePushed() == 0);

    // Test if last instructions and stop emitting in that case.
    handler.moveToNextPC();
    if (handler.pc() >= script->codeEnd()) {
      break;
    }

#ifdef DEBUG
    prevpc = handler.pc();
#endif
  }

  MOZ_ASSERT(JSOp(*prevpc) == JSOp::RetRval || JSOp(*prevpc) == JSOp::Return);
  return Method_Compiled;
}

bool BaselineInterpreterGenerator::emitDebugTrap() {
  CodeOffset offset = masm.nopPatchableToCall();
  if (!debugTrapOffsets_.append(offset.offset())) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

// Register holding the bytecode pc during dispatch. This exists so the debug
// trap handler can reload the pc into this register when it's done.
static constexpr Register InterpreterPCRegAtDispatch =
    HasInterpreterPCReg() ? InterpreterPCReg : R0.scratchReg();

bool BaselineInterpreterGenerator::emitInterpreterLoop() {
  AutoCreatedBy acb(masm, "BaselineInterpreterGenerator::emitInterpreterLoop");

  Register scratch1 = R0.scratchReg();
  Register scratch2 = R1.scratchReg();

  // Entry point for interpreting a bytecode op. No registers are live except
  // for InterpreterPCReg.
  masm.bind(handler.interpretOpWithPCRegLabel());

  // Emit a patchable call for debugger breakpoints/stepping.
  if (!emitDebugTrap()) {
    return false;
  }
  Label interpretOpAfterDebugTrap;
  masm.bind(&interpretOpAfterDebugTrap);

  // Load pc, bytecode op.
  Register pcReg = LoadBytecodePC(masm, scratch1);
  masm.load8ZeroExtend(Address(pcReg, 0), scratch1);

  // Jump to table[op].
  {
    CodeOffset label = masm.moveNearAddressWithPatch(scratch2);
    if (!tableLabels_.append(label)) {
      return false;
    }
    BaseIndex pointer(scratch2, scratch1, ScalePointer);
    masm.branchToComputedAddress(pointer);
  }

  // At the end of each op, emit code to bump the pc and jump to the
  // next op (this is also known as a threaded interpreter).
  auto opEpilogue = [&](JSOp op, size_t opLength) -> bool {
    MOZ_ASSERT(masm.framePushed() == 0);

    if (!BytecodeFallsThrough(op)) {
      // Nothing to do.
      masm.assumeUnreachable("unexpected fall through");
      return true;
    }

    // Bump frame->interpreterICEntry if needed.
    if (BytecodeOpHasIC(op)) {
      frame.bumpInterpreterICEntry();
    }

    // Bump bytecode PC.
    if (HasInterpreterPCReg()) {
      MOZ_ASSERT(InterpreterPCRegAtDispatch == InterpreterPCReg);
      masm.addPtr(Imm32(opLength), InterpreterPCReg);
    } else {
      MOZ_ASSERT(InterpreterPCRegAtDispatch == scratch1);
      masm.loadPtr(frame.addressOfInterpreterPC(), InterpreterPCRegAtDispatch);
      masm.addPtr(Imm32(opLength), InterpreterPCRegAtDispatch);
      masm.storePtr(InterpreterPCRegAtDispatch, frame.addressOfInterpreterPC());
    }

    if (!emitDebugTrap()) {
      return false;
    }

    // Load the opcode, jump to table[op].
    masm.load8ZeroExtend(Address(InterpreterPCRegAtDispatch, 0), scratch1);
    CodeOffset label = masm.moveNearAddressWithPatch(scratch2);
    if (!tableLabels_.append(label)) {
      return false;
    }
    BaseIndex pointer(scratch2, scratch1, ScalePointer);
    masm.branchToComputedAddress(pointer);
    return true;
  };

  // Emit code for each bytecode op.
  Label opLabels[JSOP_LIMIT];
#define EMIT_OP(OP, ...)                          \
  {                                               \
    AutoCreatedBy acb(masm, "op=" #OP);           \
    perfSpewer_.recordOffset(masm, JSOp::OP);     \
    masm.bind(&opLabels[uint8_t(JSOp::OP)]);      \
    handler.setCurrentOp(JSOp::OP);               \
    if (!this->emit_##OP()) {                     \
      return false;                               \
    }                                             \
    if (!opEpilogue(JSOp::OP, JSOpLength_##OP)) { \
      return false;                               \
    }                                             \
    handler.resetCurrentOp();                     \
  }
  FOR_EACH_OPCODE(EMIT_OP)
#undef EMIT_OP

  // External entry point to start interpreting bytecode ops. This is used for
  // things like exception handling and OSR. DebugModeOSR patches JIT frames to
  // return here from the DebugTrapHandler.
  masm.bind(handler.interpretOpLabel());
  interpretOpOffset_ = masm.currentOffset();
  restoreInterpreterPCReg();
  masm.jump(handler.interpretOpWithPCRegLabel());

  // Second external entry point: this skips the debug trap for the first op
  // and is used by OSR.
  interpretOpNoDebugTrapOffset_ = masm.currentOffset();
  restoreInterpreterPCReg();
  masm.jump(&interpretOpAfterDebugTrap);

  // External entry point for Ion prologue bailouts.
  bailoutPrologueOffset_ = CodeOffset(masm.currentOffset());
  restoreInterpreterPCReg();
  masm.jump(&bailoutPrologue_);

  // Emit debug trap handler code (target of patchable call instructions). This
  // is just a tail call to the debug trap handler trampoline code.
  {
    JitRuntime* jrt = cx->runtime()->jitRuntime();
    JitCode* handlerCode =
        jrt->debugTrapHandler(cx, DebugTrapHandlerKind::Interpreter);
    if (!handlerCode) {
      return false;
    }

    debugTrapHandlerOffset_ = masm.currentOffset();
    masm.jump(handlerCode);
  }

  // Emit the table.
  masm.haltingAlign(sizeof(void*));

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
  size_t numInstructions = JSOP_LIMIT * (sizeof(uintptr_t) / sizeof(uint32_t));
  AutoForbidPoolsAndNops afp(&masm, numInstructions);
#endif

  tableOffset_ = masm.currentOffset();

  for (size_t i = 0; i < JSOP_LIMIT; i++) {
    const Label& opLabel = opLabels[i];
    MOZ_ASSERT(opLabel.bound());
    CodeLabel cl;
    masm.writeCodePointer(&cl);
    cl.target()->bind(opLabel.offset());
    masm.addCodeLabel(cl);
  }

  return true;
}

void BaselineInterpreterGenerator::emitOutOfLineCodeCoverageInstrumentation() {
  AutoCreatedBy acb(masm,
                    "BaselineInterpreterGenerator::"
                    "emitOutOfLineCodeCoverageInstrumentation");

  masm.bind(handler.codeCoverageAtPrologueLabel());
#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  saveInterpreterPCReg();

  using Fn1 = void (*)(BaselineFrame* frame);
  masm.setupUnalignedABICall(R0.scratchReg());
  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
  masm.passABIArg(R0.scratchReg());
  masm.callWithABI<Fn1, HandleCodeCoverageAtPrologue>();

  restoreInterpreterPCReg();
  masm.ret();

  masm.bind(handler.codeCoverageAtPCLabel());
#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  saveInterpreterPCReg();

  using Fn2 = void (*)(BaselineFrame* frame, jsbytecode* pc);
  masm.setupUnalignedABICall(R0.scratchReg());
  masm.loadBaselineFramePtr(FramePointer, R0.scratchReg());
  masm.passABIArg(R0.scratchReg());
  Register pcReg = LoadBytecodePC(masm, R2.scratchReg());
  masm.passABIArg(pcReg);
  masm.callWithABI<Fn2, HandleCodeCoverageAtPC>();

  restoreInterpreterPCReg();
  masm.ret();
}

bool BaselineInterpreterGenerator::generate(BaselineInterpreter& interpreter) {
  AutoCreatedBy acb(masm, "BaselineInterpreterGenerator::generate");

  perfSpewer_.recordOffset(masm, "Prologue");
  if (!emitPrologue()) {
    return false;
  }

  perfSpewer_.recordOffset(masm, "InterpreterLoop");
  if (!emitInterpreterLoop()) {
    return false;
  }

  perfSpewer_.recordOffset(masm, "Epilogue");
  if (!emitEpilogue()) {
    return false;
  }

  perfSpewer_.recordOffset(masm, "OOLPostBarrierSlot");
  if (!emitOutOfLinePostBarrierSlot()) {
    return false;
  }

  perfSpewer_.recordOffset(masm, "OOLCodeCoverageInstrumentation");
  emitOutOfLineCodeCoverageInstrumentation();

  {
    AutoCreatedBy acb(masm, "everything_else");
    Linker linker(masm);
    if (masm.oom()) {
      ReportOutOfMemory(cx);
      return false;
    }

    JitCode* code = linker.newCode(cx, CodeKind::Other);
    if (!code) {
      return false;
    }

    // Register BaselineInterpreter code with the profiler's JitCode table.
    {
      auto entry = MakeJitcodeGlobalEntry<BaselineInterpreterEntry>(
          cx, code, code->raw(), code->rawEnd());
      if (!entry) {
        return false;
      }

      JitcodeGlobalTable* globalTable =
          cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
      if (!globalTable->addEntry(std::move(entry))) {
        ReportOutOfMemory(cx);
        return false;
      }

      code->setHasBytecodeMap();
    }

    // Patch loads now that we know the tableswitch base address.
    CodeLocationLabel tableLoc(code, CodeOffset(tableOffset_));
    for (CodeOffset off : tableLabels_) {
      MacroAssembler::patchNearAddressMove(CodeLocationLabel(code, off),
                                           tableLoc);
    }

    perfSpewer_.saveProfile(code);

#ifdef MOZ_VTUNE
    vtune::MarkStub(code, "BaselineInterpreter");
#endif

    interpreter.init(
        code, interpretOpOffset_, interpretOpNoDebugTrapOffset_,
        bailoutPrologueOffset_.offset(),
        profilerEnterFrameToggleOffset_.offset(),
        profilerExitFrameToggleOffset_.offset(), debugTrapHandlerOffset_,
        std::move(handler.debugInstrumentationOffsets()),
        std::move(debugTrapOffsets_), std::move(handler.codeCoverageOffsets()),
        std::move(handler.icReturnOffsets()), handler.callVMOffsets());
  }

  if (cx->runtime()->geckoProfiler().enabled()) {
    interpreter.toggleProfilerInstrumentation(true);
  }

  if (coverage::IsLCovEnabled()) {
    interpreter.toggleCodeCoverageInstrumentationUnchecked(true);
  }

  return true;
}

JitCode* JitRuntime::generateDebugTrapHandler(JSContext* cx,
                                              DebugTrapHandlerKind kind) {
  TempAllocator temp(&cx->tempLifoAlloc());
  StackMacroAssembler masm(cx, temp);
  AutoCreatedBy acb(masm, "JitRuntime::generateDebugTrapHandler");

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  MOZ_ASSERT(!regs.has(FramePointer));
  regs.takeUnchecked(ICStubReg);
  if (HasInterpreterPCReg()) {
    regs.takeUnchecked(InterpreterPCReg);
  }
#ifdef JS_CODEGEN_ARM
  regs.takeUnchecked(BaselineSecondScratchReg);
  AutoNonDefaultSecondScratchRegister andssr(masm, BaselineSecondScratchReg);
#endif
  Register scratch1 = regs.takeAny();
  Register scratch2 = regs.takeAny();
  Register scratch3 = regs.takeAny();

  if (kind == DebugTrapHandlerKind::Interpreter) {
    // The interpreter calls this for every script when debugging, so check if
    // the script has any breakpoints or is in step mode before calling into
    // C++.
    Label hasDebugScript;
    Address scriptAddr(FramePointer,
                       BaselineFrame::reverseOffsetOfInterpreterScript());
    masm.loadPtr(scriptAddr, scratch1);
    masm.branchTest32(Assembler::NonZero,
                      Address(scratch1, JSScript::offsetOfMutableFlags()),
                      Imm32(int32_t(JSScript::MutableFlags::HasDebugScript)),
                      &hasDebugScript);
    masm.abiret();
    masm.bind(&hasDebugScript);

    if (HasInterpreterPCReg()) {
      // Update frame's bytecode pc because the debugger depends on it.
      Address pcAddr(FramePointer,
                     BaselineFrame::reverseOffsetOfInterpreterPC());
      masm.storePtr(InterpreterPCReg, pcAddr);
    }
  }

  // Load the return address in scratch1.
  masm.loadAbiReturnAddress(scratch1);

  // Load BaselineFrame pointer in scratch2.
  masm.loadBaselineFramePtr(FramePointer, scratch2);

  // Enter a stub frame and call the HandleDebugTrap VM function. Ensure
  // the stub frame has a nullptr ICStub pointer, since this pointer is marked
  // during GC.
  masm.movePtr(ImmPtr(nullptr), ICStubReg);
  EmitBaselineEnterStubFrame(masm, scratch3);

  using Fn = bool (*)(JSContext*, BaselineFrame*, const uint8_t*);
  VMFunctionId id = VMFunctionToId<Fn, jit::HandleDebugTrap>::id;
  TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(id);

  masm.push(scratch1);
  masm.push(scratch2);
  EmitBaselineCallVM(code, masm);

  EmitBaselineLeaveStubFrame(masm);

  if (kind == DebugTrapHandlerKind::Interpreter) {
    // We have to reload the bytecode pc register.
    Address pcAddr(FramePointer, BaselineFrame::reverseOffsetOfInterpreterPC());
    masm.loadPtr(pcAddr, InterpreterPCRegAtDispatch);
  }
  masm.abiret();

  Linker linker(masm);
  JitCode* handlerCode = linker.newCode(cx, CodeKind::Other);
  if (!handlerCode) {
    return nullptr;
  }

  CollectPerfSpewerJitCodeProfile(handlerCode, "DebugTrapHandler");

#ifdef MOZ_VTUNE
  vtune::MarkStub(handlerCode, "DebugTrapHandler");
#endif

  return handlerCode;
}

}  // namespace jit
}  // namespace js
