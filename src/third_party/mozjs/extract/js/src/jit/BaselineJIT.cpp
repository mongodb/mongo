/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineJIT.h"

#include "mozilla/BinarySearch.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"

#include <algorithm>

#include "debugger/DebugAPI.h"
#include "gc/GCContext.h"
#include "gc/PublicIterators.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/BaselineCodeGen.h"
#include "jit/BaselineIC.h"
#include "jit/CalleeToken.h"
#include "jit/JitCommon.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/MacroAssembler.h"
#include "js/friend/StackLimits.h"  // js::AutoCheckRecursionLimit
#include "vm/Interpreter.h"

#include "debugger/DebugAPI-inl.h"
#include "gc/GC-inl.h"
#include "jit/JitHints-inl.h"
#include "jit/JitScript-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Stack-inl.h"

using mozilla::BinarySearchIf;
using mozilla::CheckedInt;
using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

void ICStubSpace::freeAllAfterMinorGC(Zone* zone) {
  if (zone->isAtomsZone()) {
    MOZ_ASSERT(allocator_.isEmpty());
  } else {
    JSRuntime* rt = zone->runtimeFromMainThread();
    rt->gc.queueAllLifoBlocksForFreeAfterMinorGC(&allocator_);
  }
}

static bool CheckFrame(InterpreterFrame* fp) {
  if (fp->isDebuggerEvalFrame()) {
    // Debugger eval-in-frame. These are likely short-running scripts so
    // don't bother compiling them for now.
    JitSpew(JitSpew_BaselineAbort, "debugger frame");
    return false;
  }

  if (fp->isFunctionFrame() && TooManyActualArguments(fp->numActualArgs())) {
    // Fall back to the interpreter to avoid running out of stack space.
    JitSpew(JitSpew_BaselineAbort, "Too many arguments (%u)",
            fp->numActualArgs());
    return false;
  }

  return true;
}

struct EnterJitData {
  explicit EnterJitData(JSContext* cx)
      : jitcode(nullptr),
        osrFrame(nullptr),
        calleeToken(nullptr),
        maxArgv(nullptr),
        maxArgc(0),
        numActualArgs(0),
        osrNumStackValues(0),
        envChain(cx),
        result(cx),
        constructing(false) {}

  uint8_t* jitcode;
  InterpreterFrame* osrFrame;

  void* calleeToken;

  Value* maxArgv;
  unsigned maxArgc;
  unsigned numActualArgs;
  unsigned osrNumStackValues;

  RootedObject envChain;
  RootedValue result;

  bool constructing;
};

static JitExecStatus EnterBaseline(JSContext* cx, EnterJitData& data) {
  MOZ_ASSERT(data.osrFrame);

  // Check for potential stack overflow before OSR-ing.
  uint32_t extra =
      BaselineFrame::Size() + (data.osrNumStackValues * sizeof(Value));
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.checkWithExtra(cx, extra)) {
    return JitExec_Aborted;
  }

#ifdef DEBUG
  // Assert we don't GC before entering JIT code. A GC could discard JIT code
  // or move the function stored in the CalleeToken (it won't be traced at
  // this point). We use Maybe<> here so we can call reset() to call the
  // AutoAssertNoGC destructor before we enter JIT code.
  mozilla::Maybe<JS::AutoAssertNoGC> nogc;
  nogc.emplace(cx);
#endif

  MOZ_ASSERT(IsBaselineInterpreterEnabled());
  MOZ_ASSERT(CheckFrame(data.osrFrame));

  EnterJitCode enter = cx->runtime()->jitRuntime()->enterJit();

  // Caller must construct |this| before invoking the function.
  MOZ_ASSERT_IF(data.constructing,
                data.maxArgv[0].isObject() ||
                    data.maxArgv[0].isMagic(JS_UNINITIALIZED_LEXICAL));

  data.result.setInt32(data.numActualArgs);
  {
    AssertRealmUnchanged aru(cx);
    ActivationEntryMonitor entryMonitor(cx, data.calleeToken);
    JitActivation activation(cx);

    data.osrFrame->setRunningInJit();

#ifdef DEBUG
    nogc.reset();
#endif
    // Single transition point from Interpreter to Baseline.
    CALL_GENERATED_CODE(enter, data.jitcode, data.maxArgc, data.maxArgv,
                        data.osrFrame, data.calleeToken, data.envChain.get(),
                        data.osrNumStackValues, data.result.address());

    data.osrFrame->clearRunningInJit();
  }

  // Jit callers wrap primitive constructor return, except for derived
  // class constructors, which are forced to do it themselves.
  if (!data.result.isMagic() && data.constructing &&
      data.result.isPrimitive()) {
    MOZ_ASSERT(data.maxArgv[0].isObject());
    data.result = data.maxArgv[0];
  }

  // Release temporary buffer used for OSR into Ion.
  cx->runtime()->jitRuntime()->freeIonOsrTempData();

  MOZ_ASSERT_IF(data.result.isMagic(), data.result.isMagic(JS_ION_ERROR));
  return data.result.isMagic() ? JitExec_Error : JitExec_Ok;
}

JitExecStatus jit::EnterBaselineInterpreterAtBranch(JSContext* cx,
                                                    InterpreterFrame* fp,
                                                    jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::LoopHead);

  EnterJitData data(cx);

  // Use the entry point that skips the debug trap because the C++ interpreter
  // already handled this for the current op.
  const BaselineInterpreter& interp =
      cx->runtime()->jitRuntime()->baselineInterpreter();
  data.jitcode = interp.interpretOpNoDebugTrapAddr().value;

  data.osrFrame = fp;
  data.osrNumStackValues =
      fp->script()->nfixed() + cx->interpreterRegs().stackDepth();

  if (fp->isFunctionFrame()) {
    data.constructing = fp->isConstructing();
    data.numActualArgs = fp->numActualArgs();
    data.maxArgc = std::max(fp->numActualArgs(), fp->numFormalArgs()) +
                   1;               // +1 = include |this|
    data.maxArgv = fp->argv() - 1;  // -1 = include |this|
    data.envChain = nullptr;
    data.calleeToken = CalleeToToken(&fp->callee(), data.constructing);
  } else {
    data.constructing = false;
    data.numActualArgs = 0;
    data.maxArgc = 0;
    data.maxArgv = nullptr;
    data.envChain = fp->environmentChain();
    data.calleeToken = CalleeToToken(fp->script());
  }

  JitExecStatus status = EnterBaseline(cx, data);
  if (status != JitExec_Ok) {
    return status;
  }

  fp->setReturnValue(data.result);
  return JitExec_Ok;
}

MethodStatus jit::BaselineCompile(JSContext* cx, JSScript* script,
                                  bool forceDebugInstrumentation) {
  cx->check(script);
  MOZ_ASSERT(!script->hasBaselineScript());
  MOZ_ASSERT(script->canBaselineCompile());
  MOZ_ASSERT(IsBaselineJitEnabled(cx));
  AutoGeckoProfilerEntry pseudoFrame(
      cx, "Baseline script compilation",
      JS::ProfilingCategoryPair::JS_BaselineCompilation);

  TempAllocator temp(&cx->tempLifoAlloc());
  JitContext jctx(cx);

  BaselineCompiler compiler(cx, temp, script);
  if (!compiler.init()) {
    ReportOutOfMemory(cx);
    return Method_Error;
  }

  if (forceDebugInstrumentation) {
    compiler.setCompileDebugInstrumentation();
  }

  MethodStatus status = compiler.compile();

  MOZ_ASSERT_IF(status == Method_Compiled, script->hasBaselineScript());
  MOZ_ASSERT_IF(status != Method_Compiled, !script->hasBaselineScript());

  if (status == Method_CantCompile) {
    script->disableBaselineCompile();
  }

  return status;
}

static MethodStatus CanEnterBaselineJIT(JSContext* cx, HandleScript script,
                                        AbstractFramePtr osrSourceFrame) {
  // Skip if the script has been disabled.
  if (!script->canBaselineCompile()) {
    return Method_Skipped;
  }

  if (!IsBaselineJitEnabled(cx)) {
    script->disableBaselineCompile();
    return Method_CantCompile;
  }

  // This check is needed in the following corner case. Consider a function h,
  //
  //   function h(x) {
  //      if (!x)
  //        return;
  //      h(false);
  //      for (var i = 0; i < N; i++)
  //         /* do stuff */
  //   }
  //
  // Suppose h is not yet compiled in baseline and is executing in the
  // interpreter. Let this interpreter frame be f_older. The debugger marks
  // f_older as isDebuggee. At the point of the recursive call h(false), h is
  // compiled in baseline without debug instrumentation, pushing a baseline
  // frame f_newer. The debugger never flags f_newer as isDebuggee, and never
  // recompiles h. When the recursive call returns and execution proceeds to
  // the loop, the interpreter attempts to OSR into baseline. Since h is
  // already compiled in baseline, execution jumps directly into baseline
  // code. This is incorrect as h's baseline script does not have debug
  // instrumentation.
  if (osrSourceFrame && osrSourceFrame.isDebuggee() &&
      !DebugAPI::ensureExecutionObservabilityOfOsrFrame(cx, osrSourceFrame)) {
    return Method_Error;
  }

  if (script->length() > BaselineMaxScriptLength) {
    script->disableBaselineCompile();
    return Method_CantCompile;
  }

  if (script->nslots() > BaselineMaxScriptSlots) {
    script->disableBaselineCompile();
    return Method_CantCompile;
  }

  if (script->hasBaselineScript()) {
    return Method_Compiled;
  }

  // If a hint is available, skip the warmup count threshold.
  bool mightHaveEagerBaselineHint = false;
  if (!JitOptions.disableJitHints && !script->noEagerBaselineHint() &&
      cx->runtime()->jitRuntime()->hasJitHintsMap()) {
    JitHintsMap* jitHints = cx->runtime()->jitRuntime()->getJitHintsMap();
    // If this lookup fails, the NoEagerBaselineHint script flag is set
    // to true to prevent any further lookups for this script.
    if (jitHints->mightHaveEagerBaselineHint(script)) {
      mightHaveEagerBaselineHint = true;
    }
  }
  // Check script warm-up counter if no hint.
  if (!mightHaveEagerBaselineHint) {
    if (script->getWarmUpCount() <= JitOptions.baselineJitWarmUpThreshold) {
      return Method_Skipped;
    }
  }

  // Check this before calling ensureJitZoneExists, so we're less
  // likely to report OOM in JSRuntime::createJitRuntime.
  if (!CanLikelyAllocateMoreExecutableMemory()) {
    return Method_Skipped;
  }

  if (!cx->zone()->ensureJitZoneExists(cx)) {
    return Method_Error;
  }

  if (script->hasForceInterpreterOp()) {
    script->disableBaselineCompile();
    return Method_CantCompile;
  }

  // Frames can be marked as debuggee frames independently of its underlying
  // script being a debuggee script, e.g., when performing
  // Debugger.Frame.prototype.eval.
  bool forceDebugInstrumentation =
      osrSourceFrame && osrSourceFrame.isDebuggee();
  return BaselineCompile(cx, script, forceDebugInstrumentation);
}

bool jit::CanBaselineInterpretScript(JSScript* script) {
  MOZ_ASSERT(IsBaselineInterpreterEnabled());

  if (script->hasForceInterpreterOp()) {
    return false;
  }

  if (script->nslots() > BaselineMaxScriptSlots) {
    // Avoid overrecursion exceptions when the script has a ton of stack slots
    // by forcing such scripts to run in the C++ interpreter with heap-allocated
    // stack frames.
    return false;
  }

  return true;
}

static bool MaybeCreateBaselineInterpreterEntryScript(JSContext* cx,
                                                      JSScript* script) {
  MOZ_ASSERT(script->hasJitScript());

  JitRuntime* jitRuntime = cx->runtime()->jitRuntime();
  if (script->jitCodeRaw() != jitRuntime->baselineInterpreter().codeRaw()) {
    // script already has an updated interpreter trampoline.
#ifdef DEBUG
    auto p = jitRuntime->getInterpreterEntryMap()->lookup(script);
    MOZ_ASSERT(p);
    MOZ_ASSERT(p->value().raw() == script->jitCodeRaw());
#endif
    return true;
  }

  auto p = jitRuntime->getInterpreterEntryMap()->lookupForAdd(script);
  if (!p) {
    Rooted<JitCode*> code(
        cx, jitRuntime->generateEntryTrampolineForScript(cx, script));
    if (!code) {
      return false;
    }

    EntryTrampoline entry(cx, code);
    if (!jitRuntime->getInterpreterEntryMap()->add(p, script, entry)) {
      return false;
    }
  }

  script->updateJitCodeRaw(cx->runtime());
  return true;
}

static MethodStatus CanEnterBaselineInterpreter(JSContext* cx,
                                                JSScript* script) {
  MOZ_ASSERT(IsBaselineInterpreterEnabled());

  if (script->hasJitScript()) {
    return Method_Compiled;
  }

  if (!CanBaselineInterpretScript(script)) {
    return Method_CantCompile;
  }

  // Check script warm-up counter.
  if (script->getWarmUpCount() <=
      JitOptions.baselineInterpreterWarmUpThreshold) {
    return Method_Skipped;
  }

  if (!cx->zone()->ensureJitZoneExists(cx)) {
    return Method_Error;
  }

  AutoKeepJitScripts keepJitScript(cx);
  if (!script->ensureHasJitScript(cx, keepJitScript)) {
    return Method_Error;
  }

  if (JitOptions.emitInterpreterEntryTrampoline) {
    if (!MaybeCreateBaselineInterpreterEntryScript(cx, script)) {
      return Method_Error;
    }
  }
  return Method_Compiled;
}

MethodStatus jit::CanEnterBaselineInterpreterAtBranch(JSContext* cx,
                                                      InterpreterFrame* fp) {
  if (!CheckFrame(fp)) {
    return Method_CantCompile;
  }

  // JITs do not respect the debugger's OnNativeCall hook, so JIT execution is
  // disabled if this hook might need to be called.
  if (cx->realm()->debuggerObservesNativeCall()) {
    return Method_CantCompile;
  }

  return CanEnterBaselineInterpreter(cx, fp->script());
}

template <BaselineTier Tier>
MethodStatus jit::CanEnterBaselineMethod(JSContext* cx, RunState& state) {
  if (state.isInvoke()) {
    InvokeState& invoke = *state.asInvoke();
    if (TooManyActualArguments(invoke.args().length())) {
      JitSpew(JitSpew_BaselineAbort, "Too many arguments (%u)",
              invoke.args().length());
      return Method_CantCompile;
    }
  } else {
    if (state.asExecute()->isDebuggerEval()) {
      JitSpew(JitSpew_BaselineAbort, "debugger frame");
      return Method_CantCompile;
    }
  }

  RootedScript script(cx, state.script());
  switch (Tier) {
    case BaselineTier::Interpreter:
      return CanEnterBaselineInterpreter(cx, script);

    case BaselineTier::Compiler:
      return CanEnterBaselineJIT(cx, script,
                                 /* osrSourceFrame = */ NullFramePtr());
  }

  MOZ_CRASH("Unexpected tier");
}

template MethodStatus jit::CanEnterBaselineMethod<BaselineTier::Interpreter>(
    JSContext* cx, RunState& state);
template MethodStatus jit::CanEnterBaselineMethod<BaselineTier::Compiler>(
    JSContext* cx, RunState& state);

bool jit::BaselineCompileFromBaselineInterpreter(JSContext* cx,
                                                 BaselineFrame* frame,
                                                 uint8_t** res) {
  MOZ_ASSERT(frame->runningInInterpreter());

  RootedScript script(cx, frame->script());
  jsbytecode* pc = frame->interpreterPC();
  MOZ_ASSERT(pc == script->code() || JSOp(*pc) == JSOp::LoopHead);

  MethodStatus status = CanEnterBaselineJIT(cx, script,
                                            /* osrSourceFrame = */ frame);
  switch (status) {
    case Method_Error:
      return false;

    case Method_CantCompile:
    case Method_Skipped:
      *res = nullptr;
      return true;

    case Method_Compiled: {
      if (JSOp(*pc) == JSOp::LoopHead) {
        MOZ_ASSERT(pc > script->code(),
                   "Prologue vs OSR cases must not be ambiguous");
        BaselineScript* baselineScript = script->baselineScript();
        uint32_t pcOffset = script->pcToOffset(pc);
        *res = baselineScript->nativeCodeForOSREntry(pcOffset);
      } else {
        *res = script->baselineScript()->warmUpCheckPrologueAddr();
      }
      frame->prepareForBaselineInterpreterToJitOSR();
      return true;
    }
  }

  MOZ_CRASH("Unexpected status");
}

BaselineScript* BaselineScript::New(JSContext* cx,
                                    uint32_t warmUpCheckPrologueOffset,
                                    uint32_t profilerEnterToggleOffset,
                                    uint32_t profilerExitToggleOffset,
                                    size_t retAddrEntries, size_t osrEntries,
                                    size_t debugTrapEntries,
                                    size_t resumeEntries) {
  // Compute size including trailing arrays.
  CheckedInt<Offset> size = sizeof(BaselineScript);
  size += CheckedInt<Offset>(resumeEntries) * sizeof(uintptr_t);
  size += CheckedInt<Offset>(retAddrEntries) * sizeof(RetAddrEntry);
  size += CheckedInt<Offset>(osrEntries) * sizeof(OSREntry);
  size += CheckedInt<Offset>(debugTrapEntries) * sizeof(DebugTrapEntry);

  if (!size.isValid()) {
    ReportAllocationOverflow(cx);
    return nullptr;
  }

  // Allocate contiguous raw buffer.
  void* raw = cx->pod_malloc<uint8_t>(size.value());
  MOZ_ASSERT(uintptr_t(raw) % alignof(BaselineScript) == 0);
  if (!raw) {
    return nullptr;
  }
  BaselineScript* script = new (raw)
      BaselineScript(warmUpCheckPrologueOffset, profilerEnterToggleOffset,
                     profilerExitToggleOffset);

  Offset cursor = sizeof(BaselineScript);

  MOZ_ASSERT(isAlignedOffset<uintptr_t>(cursor));
  script->resumeEntriesOffset_ = cursor;
  cursor += resumeEntries * sizeof(uintptr_t);

  MOZ_ASSERT(isAlignedOffset<RetAddrEntry>(cursor));
  script->retAddrEntriesOffset_ = cursor;
  cursor += retAddrEntries * sizeof(RetAddrEntry);

  MOZ_ASSERT(isAlignedOffset<OSREntry>(cursor));
  script->osrEntriesOffset_ = cursor;
  cursor += osrEntries * sizeof(OSREntry);

  MOZ_ASSERT(isAlignedOffset<DebugTrapEntry>(cursor));
  script->debugTrapEntriesOffset_ = cursor;
  cursor += debugTrapEntries * sizeof(DebugTrapEntry);

  MOZ_ASSERT(isAlignedOffset<uint32_t>(cursor));

  script->allocBytes_ = cursor;

  MOZ_ASSERT(script->endOffset() == size.value());

  return script;
}

void BaselineScript::trace(JSTracer* trc) {
  TraceEdge(trc, &method_, "baseline-method");
}

void BaselineScript::Destroy(JS::GCContext* gcx, BaselineScript* script) {
  MOZ_ASSERT(!script->hasPendingIonCompileTask());

  // This allocation is tracked by JSScript::setBaselineScriptImpl.
  gcx->deleteUntracked(script);
}

void JS::DeletePolicy<js::jit::BaselineScript>::operator()(
    const js::jit::BaselineScript* script) {
  BaselineScript::Destroy(rt_->gcContext(),
                          const_cast<BaselineScript*>(script));
}

const RetAddrEntry& BaselineScript::retAddrEntryFromReturnOffset(
    CodeOffset returnOffset) {
  mozilla::Span<RetAddrEntry> entries = retAddrEntries();
  size_t loc;
#ifdef DEBUG
  bool found =
#endif
      BinarySearchIf(
          entries.data(), 0, entries.size(),
          [&returnOffset](const RetAddrEntry& entry) {
            size_t roffset = returnOffset.offset();
            size_t entryRoffset = entry.returnOffset().offset();
            if (roffset < entryRoffset) {
              return -1;
            }
            if (entryRoffset < roffset) {
              return 1;
            }
            return 0;
          },
          &loc);

  MOZ_ASSERT(found);
  MOZ_ASSERT(entries[loc].returnOffset().offset() == returnOffset.offset());
  return entries[loc];
}

template <typename Entry>
static bool ComputeBinarySearchMid(mozilla::Span<Entry> entries,
                                   uint32_t pcOffset, size_t* loc) {
  return BinarySearchIf(
      entries.data(), 0, entries.size(),
      [pcOffset](const Entry& entry) {
        uint32_t entryOffset = entry.pcOffset();
        if (pcOffset < entryOffset) {
          return -1;
        }
        if (entryOffset < pcOffset) {
          return 1;
        }
        return 0;
      },
      loc);
}

uint8_t* BaselineScript::returnAddressForEntry(const RetAddrEntry& ent) {
  return method()->raw() + ent.returnOffset().offset();
}

const RetAddrEntry& BaselineScript::retAddrEntryFromPCOffset(
    uint32_t pcOffset, RetAddrEntry::Kind kind) {
  mozilla::Span<RetAddrEntry> entries = retAddrEntries();
  size_t mid;
  MOZ_ALWAYS_TRUE(ComputeBinarySearchMid(entries, pcOffset, &mid));
  MOZ_ASSERT(mid < entries.size());

  // Search for the first entry for this pc.
  size_t first = mid;
  while (first > 0 && entries[first - 1].pcOffset() == pcOffset) {
    first--;
  }

  // Search for the last entry for this pc.
  size_t last = mid;
  while (last + 1 < entries.size() &&
         entries[last + 1].pcOffset() == pcOffset) {
    last++;
  }

  MOZ_ASSERT(first <= last);
  MOZ_ASSERT(entries[first].pcOffset() == pcOffset);
  MOZ_ASSERT(entries[last].pcOffset() == pcOffset);

  for (size_t i = first; i <= last; i++) {
    const RetAddrEntry& entry = entries[i];
    if (entry.kind() != kind) {
      continue;
    }

#ifdef DEBUG
    // There must be a unique entry for this pcOffset and Kind to ensure our
    // return value is well-defined.
    for (size_t j = i + 1; j <= last; j++) {
      MOZ_ASSERT(entries[j].kind() != kind);
    }
#endif

    return entry;
  }

  MOZ_CRASH("Didn't find RetAddrEntry.");
}

const RetAddrEntry& BaselineScript::prologueRetAddrEntry(
    RetAddrEntry::Kind kind) {
  MOZ_ASSERT(kind == RetAddrEntry::Kind::StackCheck);

  // The prologue entries will always be at a very low offset, so just do a
  // linear search from the beginning.
  for (const RetAddrEntry& entry : retAddrEntries()) {
    if (entry.pcOffset() != 0) {
      break;
    }
    if (entry.kind() == kind) {
      return entry;
    }
  }
  MOZ_CRASH("Didn't find prologue RetAddrEntry.");
}

const RetAddrEntry& BaselineScript::retAddrEntryFromReturnAddress(
    const uint8_t* returnAddr) {
  MOZ_ASSERT(returnAddr > method_->raw());
  MOZ_ASSERT(returnAddr < method_->raw() + method_->instructionsSize());
  CodeOffset offset(returnAddr - method_->raw());
  return retAddrEntryFromReturnOffset(offset);
}

uint8_t* BaselineScript::nativeCodeForOSREntry(uint32_t pcOffset) {
  mozilla::Span<OSREntry> entries = osrEntries();
  size_t mid;
  if (!ComputeBinarySearchMid(entries, pcOffset, &mid)) {
    return nullptr;
  }

  uint32_t nativeOffset = entries[mid].nativeOffset();
  return method_->raw() + nativeOffset;
}

void BaselineScript::computeResumeNativeOffsets(
    JSScript* script, const ResumeOffsetEntryVector& entries) {
  // Translate pcOffset to BaselineScript native address. This may return
  // nullptr if compiler decided code was unreachable.
  auto computeNative = [this, &entries](uint32_t pcOffset) -> uint8_t* {
    mozilla::Span<const ResumeOffsetEntry> entriesSpan =
        mozilla::Span(entries.begin(), entries.length());
    size_t mid;
    if (!ComputeBinarySearchMid(entriesSpan, pcOffset, &mid)) {
      return nullptr;
    }

    uint32_t nativeOffset = entries[mid].nativeOffset();
    return method_->raw() + nativeOffset;
  };

  mozilla::Span<const uint32_t> pcOffsets = script->resumeOffsets();
  mozilla::Span<uint8_t*> nativeOffsets = resumeEntryList();
  std::transform(pcOffsets.begin(), pcOffsets.end(), nativeOffsets.begin(),
                 computeNative);
}

void BaselineScript::copyRetAddrEntries(const RetAddrEntry* entries) {
  std::copy_n(entries, retAddrEntries().size(), retAddrEntries().data());
}

void BaselineScript::copyOSREntries(const OSREntry* entries) {
  std::copy_n(entries, osrEntries().size(), osrEntries().data());
}

void BaselineScript::copyDebugTrapEntries(const DebugTrapEntry* entries) {
  std::copy_n(entries, debugTrapEntries().size(), debugTrapEntries().data());
}

jsbytecode* BaselineScript::approximatePcForNativeAddress(
    JSScript* script, uint8_t* nativeAddress) {
  MOZ_ASSERT(script->baselineScript() == this);
  MOZ_ASSERT(containsCodeAddress(nativeAddress));

  uint32_t nativeOffset = nativeAddress - method_->raw();

  // Use the RetAddrEntry list (sorted on pc and return address) to look for the
  // first pc that has a return address >= nativeOffset. This isn't perfect but
  // it's a reasonable approximation for the profiler because most non-trivial
  // bytecode ops have a RetAddrEntry.

  for (const RetAddrEntry& entry : retAddrEntries()) {
    uint32_t retOffset = entry.returnOffset().offset();
    if (retOffset >= nativeOffset) {
      return script->offsetToPC(entry.pcOffset());
    }
  }

  // Return the last entry's pc. Every BaselineScript has at least one
  // RetAddrEntry for the prologue stack overflow check.
  MOZ_ASSERT(!retAddrEntries().empty());
  const RetAddrEntry& lastEntry = retAddrEntries()[retAddrEntries().size() - 1];
  return script->offsetToPC(lastEntry.pcOffset());
}

void BaselineScript::toggleDebugTraps(JSScript* script, jsbytecode* pc) {
  MOZ_ASSERT(script->baselineScript() == this);

  // Only scripts compiled for debug mode have toggled calls.
  if (!hasDebugInstrumentation()) {
    return;
  }

  AutoWritableJitCode awjc(method());

  for (const DebugTrapEntry& entry : debugTrapEntries()) {
    jsbytecode* entryPC = script->offsetToPC(entry.pcOffset());

    // If the |pc| argument is non-null we can skip all other bytecode ops.
    if (pc && pc != entryPC) {
      continue;
    }

    bool enabled = DebugAPI::stepModeEnabled(script) ||
                   DebugAPI::hasBreakpointsAt(script, entryPC);

    // Patch the trap.
    CodeLocationLabel label(method(), CodeOffset(entry.nativeOffset()));
    Assembler::ToggleCall(label, enabled);
  }
}

void BaselineScript::setPendingIonCompileTask(JSRuntime* rt, JSScript* script,
                                              IonCompileTask* task) {
  MOZ_ASSERT(script->baselineScript() == this);
  MOZ_ASSERT(task);
  MOZ_ASSERT(!hasPendingIonCompileTask());

  if (script->isIonCompilingOffThread()) {
    script->jitScript()->clearIsIonCompilingOffThread(script);
  }

  pendingIonCompileTask_ = task;
  script->updateJitCodeRaw(rt);
}

void BaselineScript::removePendingIonCompileTask(JSRuntime* rt,
                                                 JSScript* script) {
  MOZ_ASSERT(script->baselineScript() == this);
  MOZ_ASSERT(hasPendingIonCompileTask());

  pendingIonCompileTask_ = nullptr;
  script->updateJitCodeRaw(rt);
}

static void ToggleProfilerInstrumentation(JitCode* code,
                                          uint32_t profilerEnterToggleOffset,
                                          uint32_t profilerExitToggleOffset,
                                          bool enable) {
  CodeLocationLabel enterToggleLocation(code,
                                        CodeOffset(profilerEnterToggleOffset));
  CodeLocationLabel exitToggleLocation(code,
                                       CodeOffset(profilerExitToggleOffset));
  if (enable) {
    Assembler::ToggleToCmp(enterToggleLocation);
    Assembler::ToggleToCmp(exitToggleLocation);
  } else {
    Assembler::ToggleToJmp(enterToggleLocation);
    Assembler::ToggleToJmp(exitToggleLocation);
  }
}

void BaselineScript::toggleProfilerInstrumentation(bool enable) {
  if (enable == isProfilerInstrumentationOn()) {
    return;
  }

  JitSpew(JitSpew_BaselineIC, "  toggling profiling %s for BaselineScript %p",
          enable ? "on" : "off", this);

  ToggleProfilerInstrumentation(method_, profilerEnterToggleOffset_,
                                profilerExitToggleOffset_, enable);

  if (enable) {
    flags_ |= uint32_t(PROFILER_INSTRUMENTATION_ON);
  } else {
    flags_ &= ~uint32_t(PROFILER_INSTRUMENTATION_ON);
  }
}

void BaselineInterpreter::toggleProfilerInstrumentation(bool enable) {
  if (!IsBaselineInterpreterEnabled()) {
    return;
  }

  AutoWritableJitCode awjc(code_);
  ToggleProfilerInstrumentation(code_, profilerEnterToggleOffset_,
                                profilerExitToggleOffset_, enable);
}

void BaselineInterpreter::toggleDebuggerInstrumentation(bool enable) {
  if (!IsBaselineInterpreterEnabled()) {
    return;
  }

  AutoWritableJitCode awjc(code_);

  // Toggle jumps for debugger instrumentation.
  for (uint32_t offset : debugInstrumentationOffsets_) {
    CodeLocationLabel label(code_, CodeOffset(offset));
    if (enable) {
      Assembler::ToggleToCmp(label);
    } else {
      Assembler::ToggleToJmp(label);
    }
  }

  // Toggle DebugTrapHandler calls.

  uint8_t* debugTrapHandler = codeAtOffset(debugTrapHandlerOffset_);

  for (uint32_t offset : debugTrapOffsets_) {
    uint8_t* trap = codeAtOffset(offset);
    if (enable) {
      MacroAssembler::patchNopToCall(trap, debugTrapHandler);
    } else {
      MacroAssembler::patchCallToNop(trap);
    }
  }
}

void BaselineInterpreter::toggleCodeCoverageInstrumentationUnchecked(
    bool enable) {
  if (!IsBaselineInterpreterEnabled()) {
    return;
  }

  AutoWritableJitCode awjc(code_);

  for (uint32_t offset : codeCoverageOffsets_) {
    CodeLocationLabel label(code_, CodeOffset(offset));
    if (enable) {
      Assembler::ToggleToCmp(label);
    } else {
      Assembler::ToggleToJmp(label);
    }
  }
}

void BaselineInterpreter::toggleCodeCoverageInstrumentation(bool enable) {
  if (coverage::IsLCovEnabled()) {
    // Instrumentation is enabled no matter what.
    return;
  }

  toggleCodeCoverageInstrumentationUnchecked(enable);
}

void jit::FinishDiscardBaselineScript(JS::GCContext* gcx, JSScript* script) {
  MOZ_ASSERT(script->hasBaselineScript());
  MOZ_ASSERT(!script->jitScript()->icScript()->active());

  BaselineScript* baseline =
      script->jitScript()->clearBaselineScript(gcx, script);
  BaselineScript::Destroy(gcx, baseline);
}

void jit::AddSizeOfBaselineData(JSScript* script,
                                mozilla::MallocSizeOf mallocSizeOf,
                                size_t* data) {
  if (script->hasBaselineScript()) {
    script->baselineScript()->addSizeOfIncludingThis(mallocSizeOf, data);
  }
}

void jit::ToggleBaselineProfiling(JSContext* cx, bool enable) {
  JitRuntime* jrt = cx->runtime()->jitRuntime();
  if (!jrt) {
    return;
  }

  jrt->baselineInterpreter().toggleProfilerInstrumentation(enable);

  for (ZonesIter zone(cx->runtime(), SkipAtoms); !zone.done(); zone.next()) {
    if (!zone->jitZone()) {
      continue;
    }
    zone->jitZone()->forEachJitScript([&](jit::JitScript* jitScript) {
      JSScript* script = jitScript->owningScript();
      if (enable) {
        jitScript->ensureProfileString(cx, script);
      }
      if (script->hasBaselineScript()) {
        AutoWritableJitCode awjc(script->baselineScript()->method());
        script->baselineScript()->toggleProfilerInstrumentation(enable);
      }
    });
  }
}

void BaselineInterpreter::init(JitCode* code, uint32_t interpretOpOffset,
                               uint32_t interpretOpNoDebugTrapOffset,
                               uint32_t bailoutPrologueOffset,
                               uint32_t profilerEnterToggleOffset,
                               uint32_t profilerExitToggleOffset,
                               uint32_t debugTrapHandlerOffset,
                               CodeOffsetVector&& debugInstrumentationOffsets,
                               CodeOffsetVector&& debugTrapOffsets,
                               CodeOffsetVector&& codeCoverageOffsets,
                               ICReturnOffsetVector&& icReturnOffsets,
                               const CallVMOffsets& callVMOffsets) {
  code_ = code;
  interpretOpOffset_ = interpretOpOffset;
  interpretOpNoDebugTrapOffset_ = interpretOpNoDebugTrapOffset;
  bailoutPrologueOffset_ = bailoutPrologueOffset;
  profilerEnterToggleOffset_ = profilerEnterToggleOffset;
  profilerExitToggleOffset_ = profilerExitToggleOffset;
  debugTrapHandlerOffset_ = debugTrapHandlerOffset;
  debugInstrumentationOffsets_ = std::move(debugInstrumentationOffsets);
  debugTrapOffsets_ = std::move(debugTrapOffsets);
  codeCoverageOffsets_ = std::move(codeCoverageOffsets);
  icReturnOffsets_ = std::move(icReturnOffsets);
  callVMOffsets_ = callVMOffsets;
}

uint8_t* BaselineInterpreter::retAddrForIC(JSOp op) const {
  for (const ICReturnOffset& entry : icReturnOffsets_) {
    if (entry.op == op) {
      return codeAtOffset(entry.offset);
    }
  }
  MOZ_CRASH("Unexpected op");
}

bool jit::GenerateBaselineInterpreter(JSContext* cx,
                                      BaselineInterpreter& interpreter) {
  if (IsBaselineInterpreterEnabled()) {
    TempAllocator temp(&cx->tempLifoAlloc());
    BaselineInterpreterGenerator generator(cx, temp);
    return generator.generate(interpreter);
  }

  return true;
}
