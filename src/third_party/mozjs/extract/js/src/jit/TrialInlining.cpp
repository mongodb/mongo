/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/TrialInlining.h"

#include "jit/BaselineCacheIRCompiler.h"
#include "jit/BaselineIC.h"
#include "jit/CacheIRHealth.h"
#include "jit/Ion.h"  // TooManyFormalArguments

#include "jit/BaselineFrame-inl.h"
#include "vm/BytecodeIterator-inl.h"
#include "vm/BytecodeLocation-inl.h"

using mozilla::Maybe;

namespace js {
namespace jit {

bool DoTrialInlining(JSContext* cx, BaselineFrame* frame) {
  RootedScript script(cx, frame->script());
  ICScript* icScript = frame->icScript();
  bool isRecursive = icScript->depth() > 0;

#ifdef JS_CACHEIR_SPEW
  if (cx->spewer().enabled(cx, script, SpewChannel::CacheIRHealthReport)) {
    for (uint32_t i = 0; i < icScript->numICEntries(); i++) {
      ICEntry& entry = icScript->icEntry(i);
      ICFallbackStub* fallbackStub = icScript->fallbackStub(i);

      // If the IC is megamorphic or generic, then we have already
      // spewed the IC report on transition.
      if (!(uint8_t(fallbackStub->state().mode()) > 0)) {
        jit::ICStub* stub = entry.firstStub();
        bool sawNonZeroCount = false;
        while (!stub->isFallback()) {
          uint32_t count = stub->enteredCount();
          if (count > 0 && sawNonZeroCount) {
            CacheIRHealth cih;
            cih.healthReportForIC(cx, &entry, fallbackStub, script,
                                  SpewContext::TrialInlining);
            break;
          }

          if (count > 0 && !sawNonZeroCount) {
            sawNonZeroCount = true;
          }

          stub = stub->toCacheIRStub()->next();
        }
      }
    }
  }
#endif

  if (!script->canIonCompile()) {
    return true;
  }

  // Baseline shouldn't attempt trial inlining in scripts that are too large.
  MOZ_ASSERT_IF(JitOptions.limitScriptSize,
                script->length() <= JitOptions.ionMaxScriptSize);

  const uint32_t MAX_INLINING_DEPTH = 4;
  if (icScript->depth() > MAX_INLINING_DEPTH) {
    return true;
  }

  InliningRoot* root =
      isRecursive ? icScript->inliningRoot()
                  : script->jitScript()->getOrCreateInliningRoot(cx, script);
  if (!root) {
    return false;
  }

  JitSpew(JitSpew_WarpTrialInlining,
          "Trial inlining for %s script %s:%u:%u (%p) (inliningRoot=%p)",
          (isRecursive ? "inner" : "outer"), script->filename(),
          script->lineno(), script->column(), frame->script(), root);
  JitSpewIndent spewIndent(JitSpew_WarpTrialInlining);

  TrialInliner inliner(cx, script, icScript, root);
  return inliner.tryInlining();
}

void TrialInliner::cloneSharedPrefix(ICCacheIRStub* stub,
                                     const uint8_t* endOfPrefix,
                                     CacheIRWriter& writer) {
  CacheIRReader reader(stub->stubInfo());
  CacheIRCloner cloner(stub);
  while (reader.currentPosition() < endOfPrefix) {
    CacheOp op = reader.readOp();
    cloner.cloneOp(op, reader, writer);
  }
}

bool TrialInliner::replaceICStub(ICEntry& entry, ICFallbackStub* fallback,
                                 CacheIRWriter& writer, CacheKind kind) {
  MOZ_ASSERT(fallback->trialInliningState() == TrialInliningState::Candidate);

  fallback->discardStubs(cx(), &entry);

  // Note: AttachBaselineCacheIRStub never throws an exception.
  bool attached = false;
  auto* newStub = AttachBaselineCacheIRStub(cx(), writer, kind, script_,
                                            icScript_, fallback, &attached);
  if (!newStub) {
    MOZ_ASSERT(fallback->trialInliningState() == TrialInliningState::Candidate);
    ReportOutOfMemory(cx());
    return false;
  }

  MOZ_ASSERT(attached);
  MOZ_ASSERT(fallback->trialInliningState() == TrialInliningState::Inlined);
  return true;
}

ICCacheIRStub* TrialInliner::maybeSingleStub(const ICEntry& entry) {
  // Look for a single non-fallback stub followed by stubs with entered-count 0.
  // Allow one optimized stub before the fallback stub to support the
  // CallIRGenerator::emitCalleeGuard optimization where we first try a
  // GuardSpecificFunction guard before falling back to GuardFunctionHasScript.
  ICStub* stub = entry.firstStub();
  if (stub->isFallback()) {
    return nullptr;
  }
  ICStub* next = stub->toCacheIRStub()->next();
  if (next->enteredCount() != 0) {
    return nullptr;
  }

  ICFallbackStub* fallback = nullptr;
  if (next->isFallback()) {
    fallback = next->toFallbackStub();
  } else {
    ICStub* nextNext = next->toCacheIRStub()->next();
    if (!nextNext->isFallback() || nextNext->enteredCount() != 0) {
      return nullptr;
    }
    fallback = nextNext->toFallbackStub();
  }

  if (fallback->trialInliningState() != TrialInliningState::Candidate) {
    return nullptr;
  }

  return stub->toCacheIRStub();
}

Maybe<InlinableOpData> FindInlinableOpData(ICCacheIRStub* stub,
                                           BytecodeLocation loc) {
  if (loc.isInvokeOp()) {
    Maybe<InlinableCallData> call = FindInlinableCallData(stub);
    if (call.isSome()) {
      return call;
    }
  }
  if (loc.isGetPropOp()) {
    Maybe<InlinableGetterData> getter = FindInlinableGetterData(stub);
    if (getter.isSome()) {
      return getter;
    }
  }
  if (loc.isSetPropOp()) {
    Maybe<InlinableSetterData> setter = FindInlinableSetterData(stub);
    if (setter.isSome()) {
      return setter;
    }
  }
  return mozilla::Nothing();
}

Maybe<InlinableCallData> FindInlinableCallData(ICCacheIRStub* stub) {
  Maybe<InlinableCallData> data;

  const CacheIRStubInfo* stubInfo = stub->stubInfo();
  const uint8_t* stubData = stub->stubDataStart();

  ObjOperandId calleeGuardOperand;
  CallFlags flags;
  JSFunction* target = nullptr;

  CacheIRReader reader(stubInfo);
  while (reader.more()) {
    const uint8_t* opStart = reader.currentPosition();

    CacheOp op = reader.readOp();
    CacheIROpInfo opInfo = CacheIROpInfos[size_t(op)];
    uint32_t argLength = opInfo.argLength;
    mozilla::DebugOnly<const uint8_t*> argStart = reader.currentPosition();

    switch (op) {
      case CacheOp::GuardSpecificFunction: {
        // If we see a guard, remember which operand we are guarding.
        MOZ_ASSERT(data.isNothing());
        calleeGuardOperand = reader.objOperandId();
        uint32_t targetOffset = reader.stubOffset();
        (void)reader.stubOffset();  // nargsAndFlags
        uintptr_t rawTarget = stubInfo->getStubRawWord(stubData, targetOffset);
        target = reinterpret_cast<JSFunction*>(rawTarget);
        break;
      }
      case CacheOp::GuardFunctionScript: {
        MOZ_ASSERT(data.isNothing());
        calleeGuardOperand = reader.objOperandId();
        uint32_t targetOffset = reader.stubOffset();
        uintptr_t rawTarget = stubInfo->getStubRawWord(stubData, targetOffset);
        target = reinterpret_cast<BaseScript*>(rawTarget)->function();
        (void)reader.stubOffset();  // nargsAndFlags
        break;
      }
      case CacheOp::CallScriptedFunction: {
        // If we see a call, check if `callee` is the previously guarded
        // operand. If it is, we know the target and can inline.
        ObjOperandId calleeOperand = reader.objOperandId();
        mozilla::DebugOnly<Int32OperandId> argcId = reader.int32OperandId();
        flags = reader.callFlags();

        if (calleeOperand == calleeGuardOperand) {
          MOZ_ASSERT(static_cast<OperandId&>(argcId).id() == 0);
          MOZ_ASSERT(data.isNothing());
          data.emplace();
          data->endOfSharedPrefix = opStart;
        }
        break;
      }
      case CacheOp::CallInlinedFunction: {
        ObjOperandId calleeOperand = reader.objOperandId();
        mozilla::DebugOnly<Int32OperandId> argcId = reader.int32OperandId();
        uint32_t icScriptOffset = reader.stubOffset();
        flags = reader.callFlags();

        if (calleeOperand == calleeGuardOperand) {
          MOZ_ASSERT(static_cast<OperandId&>(argcId).id() == 0);
          MOZ_ASSERT(data.isNothing());
          data.emplace();
          data->endOfSharedPrefix = opStart;
          uintptr_t rawICScript =
              stubInfo->getStubRawWord(stubData, icScriptOffset);
          data->icScript = reinterpret_cast<ICScript*>(rawICScript);
        }
        break;
      }
      default:
        if (!opInfo.transpile) {
          return mozilla::Nothing();
        }
        if (data.isSome()) {
          MOZ_ASSERT(op == CacheOp::ReturnFromIC);
        }
        reader.skip(argLength);
        break;
    }
    MOZ_ASSERT(argStart + argLength == reader.currentPosition());
  }

  if (data.isSome()) {
    data->calleeOperand = calleeGuardOperand;
    data->callFlags = flags;
    data->target = target;
  }
  return data;
}

Maybe<InlinableGetterData> FindInlinableGetterData(ICCacheIRStub* stub) {
  Maybe<InlinableGetterData> data;

  const CacheIRStubInfo* stubInfo = stub->stubInfo();
  const uint8_t* stubData = stub->stubDataStart();

  CacheIRReader reader(stubInfo);
  while (reader.more()) {
    const uint8_t* opStart = reader.currentPosition();

    CacheOp op = reader.readOp();
    CacheIROpInfo opInfo = CacheIROpInfos[size_t(op)];
    uint32_t argLength = opInfo.argLength;
    mozilla::DebugOnly<const uint8_t*> argStart = reader.currentPosition();

    switch (op) {
      case CacheOp::CallScriptedGetterResult: {
        data.emplace();
        data->receiverOperand = reader.valOperandId();

        uint32_t getterOffset = reader.stubOffset();
        uintptr_t rawTarget = stubInfo->getStubRawWord(stubData, getterOffset);
        data->target = reinterpret_cast<JSFunction*>(rawTarget);

        data->sameRealm = reader.readBool();
        (void)reader.stubOffset();  // nargsAndFlags

        data->endOfSharedPrefix = opStart;
        break;
      }
      case CacheOp::CallInlinedGetterResult: {
        data.emplace();
        data->receiverOperand = reader.valOperandId();

        uint32_t getterOffset = reader.stubOffset();
        uintptr_t rawTarget = stubInfo->getStubRawWord(stubData, getterOffset);
        data->target = reinterpret_cast<JSFunction*>(rawTarget);

        uint32_t icScriptOffset = reader.stubOffset();
        uintptr_t rawICScript =
            stubInfo->getStubRawWord(stubData, icScriptOffset);
        data->icScript = reinterpret_cast<ICScript*>(rawICScript);

        data->sameRealm = reader.readBool();
        (void)reader.stubOffset();  // nargsAndFlags

        data->endOfSharedPrefix = opStart;
        break;
      }
      default:
        if (!opInfo.transpile) {
          return mozilla::Nothing();
        }
        if (data.isSome()) {
          MOZ_ASSERT(op == CacheOp::ReturnFromIC);
        }
        reader.skip(argLength);
        break;
    }
    MOZ_ASSERT(argStart + argLength == reader.currentPosition());
  }

  return data;
}

Maybe<InlinableSetterData> FindInlinableSetterData(ICCacheIRStub* stub) {
  Maybe<InlinableSetterData> data;

  const CacheIRStubInfo* stubInfo = stub->stubInfo();
  const uint8_t* stubData = stub->stubDataStart();

  CacheIRReader reader(stubInfo);
  while (reader.more()) {
    const uint8_t* opStart = reader.currentPosition();

    CacheOp op = reader.readOp();
    CacheIROpInfo opInfo = CacheIROpInfos[size_t(op)];
    uint32_t argLength = opInfo.argLength;
    mozilla::DebugOnly<const uint8_t*> argStart = reader.currentPosition();

    switch (op) {
      case CacheOp::CallScriptedSetter: {
        data.emplace();
        data->receiverOperand = reader.objOperandId();

        uint32_t setterOffset = reader.stubOffset();
        uintptr_t rawTarget = stubInfo->getStubRawWord(stubData, setterOffset);
        data->target = reinterpret_cast<JSFunction*>(rawTarget);

        data->rhsOperand = reader.valOperandId();
        data->sameRealm = reader.readBool();
        (void)reader.stubOffset();  // nargsAndFlags

        data->endOfSharedPrefix = opStart;
        break;
      }
      case CacheOp::CallInlinedSetter: {
        data.emplace();
        data->receiverOperand = reader.objOperandId();

        uint32_t setterOffset = reader.stubOffset();
        uintptr_t rawTarget = stubInfo->getStubRawWord(stubData, setterOffset);
        data->target = reinterpret_cast<JSFunction*>(rawTarget);

        data->rhsOperand = reader.valOperandId();

        uint32_t icScriptOffset = reader.stubOffset();
        uintptr_t rawICScript =
            stubInfo->getStubRawWord(stubData, icScriptOffset);
        data->icScript = reinterpret_cast<ICScript*>(rawICScript);

        data->sameRealm = reader.readBool();
        (void)reader.stubOffset();  // nargsAndFlags

        data->endOfSharedPrefix = opStart;
        break;
      }
      default:
        if (!opInfo.transpile) {
          return mozilla::Nothing();
        }
        if (data.isSome()) {
          MOZ_ASSERT(op == CacheOp::ReturnFromIC);
        }
        reader.skip(argLength);
        break;
    }
    MOZ_ASSERT(argStart + argLength == reader.currentPosition());
  }

  return data;
}

// Return the number of actual arguments that will be passed to the
// target function.
static uint32_t GetCalleeNumActuals(BytecodeLocation loc) {
  switch (loc.getOp()) {
    case JSOp::GetProp:
      // Getters do not pass arguments.
      return 0;

    case JSOp::SetProp:
    case JSOp::StrictSetProp:
      // Setters pass 1 argument.
      return 1;

    case JSOp::FunCall: {
      // If FunCall is passed arguments, one of them will become |this|.
      uint32_t callArgc = loc.getCallArgc();
      return callArgc > 0 ? callArgc - 1 : 0;
    }

    case JSOp::Call:
    case JSOp::CallIgnoresRv:
    case JSOp::CallIter:
    case JSOp::New:
    case JSOp::SuperCall:
      return loc.getCallArgc();

    default:
      MOZ_CRASH("Unsupported op");
  }
}

/*static*/
bool TrialInliner::canInline(JSFunction* target, HandleScript caller,
                             BytecodeLocation loc) {
  if (!target->hasJitScript()) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: no JIT script");
    return false;
  }
  JSScript* script = target->nonLazyScript();
  if (!script->jitScript()->hasBaselineScript()) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: no BaselineScript");
    return false;
  }
  if (script->uninlineable()) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: uninlineable flag");
    return false;
  }
  if (!script->canIonCompile()) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: can't ion-compile");
    return false;
  }
  if (script->isDebuggee()) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: is debuggee");
    return false;
  }
  // Don't inline cross-realm calls.
  if (target->realm() != caller->realm()) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: cross-realm call");
    return false;
  }

  uint32_t calleeNumActuals = GetCalleeNumActuals(loc);
  if (script->needsArgsObj() &&
      calleeNumActuals > ArgumentsObject::MaxInlinedArgs) {
    JitSpew(JitSpew_WarpTrialInlining,
            "SKIP: needs arguments object with %u actual args (maximum %u)",
            calleeNumActuals, ArgumentsObject::MaxInlinedArgs);
    return false;
  }

  if (TooManyFormalArguments(target->nargs())) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: Too many formal arguments: %u",
            unsigned(target->nargs()));
    return false;
  }

  if (TooManyFormalArguments(calleeNumActuals)) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: argc too large: %u",
            unsigned(loc.getCallArgc()));
    return false;
  }

  return true;
}

bool TrialInliner::shouldInline(JSFunction* target, ICCacheIRStub* stub,
                                BytecodeLocation loc) {
#ifdef JS_JITSPEW
  BaseScript* baseScript =
      target->hasBaseScript() ? target->baseScript() : nullptr;
  JitSpew(JitSpew_WarpTrialInlining,
          "Inlining candidate JSOp::%s: callee script %s:%u:%u",
          CodeName(loc.getOp()),
          baseScript ? baseScript->filename() : "<not-scripted>",
          baseScript ? baseScript->lineno() : 0,
          baseScript ? baseScript->column() : 0);
  JitSpewIndent spewIndent(JitSpew_WarpTrialInlining);
#endif

  if (!canInline(target, script_, loc)) {
    return false;
  }

  // Don't inline (direct) recursive calls. This still allows recursion if
  // called through another function (f => g => f).
  JSScript* targetScript = target->nonLazyScript();
  if (script_ == targetScript) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: recursion");
    return false;
  }

  // Don't inline if the callee has a loop that was hot enough to enter Warp
  // via OSR. This helps prevent getting stuck in Baseline code for a long time.
  if (targetScript->jitScript()->hadIonOSR()) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: had OSR");
    return false;
  }

  // Ensure the total bytecode size does not exceed ionMaxScriptSize.
  size_t newTotalSize = root_->totalBytecodeSize() + targetScript->length();
  if (newTotalSize > JitOptions.ionMaxScriptSize) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: total size too big");
    return false;
  }

  uint32_t entryCount = stub->enteredCount();
  if (entryCount < JitOptions.inliningEntryThreshold) {
    JitSpew(JitSpew_WarpTrialInlining, "SKIP: Entry count is %u (minimum %u)",
            unsigned(entryCount), unsigned(JitOptions.inliningEntryThreshold));
    return false;
  }

  if (!JitOptions.isSmallFunction(targetScript)) {
    if (!targetScript->isInlinableLargeFunction()) {
      JitSpew(JitSpew_WarpTrialInlining, "SKIP: Length is %u (maximum %u)",
              unsigned(targetScript->length()),
              unsigned(JitOptions.smallFunctionMaxBytecodeLength));
      return false;
    }

    JitSpew(JitSpew_WarpTrialInlining,
            "INFO: Ignored length (%u) of InlinableLargeFunction",
            unsigned(targetScript->length()));
  }

  return true;
}

ICScript* TrialInliner::createInlinedICScript(JSFunction* target,
                                              BytecodeLocation loc) {
  MOZ_ASSERT(target->hasJitEntry());
  MOZ_ASSERT(target->hasJitScript());

  JSScript* targetScript = target->baseScript()->asJSScript();

  // We don't have to check for overflow here because we have already
  // successfully allocated an ICScript with this number of entries
  // when creating the JitScript for the target function, and we
  // checked for overflow then.
  uint32_t fallbackStubsOffset =
      sizeof(ICScript) + targetScript->numICEntries() * sizeof(ICEntry);
  uint32_t allocSize = fallbackStubsOffset +
                       targetScript->numICEntries() * sizeof(ICFallbackStub);

  void* raw = cx()->pod_malloc<uint8_t>(allocSize);
  MOZ_ASSERT(uintptr_t(raw) % alignof(ICScript) == 0);
  if (!raw) {
    return nullptr;
  }

  uint32_t initialWarmUpCount = JitOptions.trialInliningInitialWarmUpCount;

  uint32_t depth = icScript_->depth() + 1;
  UniquePtr<ICScript> inlinedICScript(new (raw) ICScript(
      initialWarmUpCount, fallbackStubsOffset, allocSize, depth, root_));

  inlinedICScript->initICEntries(cx(), targetScript);

  uint32_t pcOffset = loc.bytecodeToOffset(script_);
  ICScript* result = inlinedICScript.get();
  if (!icScript_->addInlinedChild(cx(), std::move(inlinedICScript), pcOffset)) {
    return nullptr;
  }
  MOZ_ASSERT(result->numICEntries() == targetScript->numICEntries());

  root_->addToTotalBytecodeSize(targetScript->length());

  JitSpew(JitSpew_WarpTrialInlining,
          "SUCCESS: Outer ICScript: %p Inner ICScript: %p pcOffset: %u",
          icScript_, result, pcOffset);

  return result;
}

bool TrialInliner::maybeInlineCall(ICEntry& entry, ICFallbackStub* fallback,
                                   BytecodeLocation loc) {
  ICCacheIRStub* stub = maybeSingleStub(entry);
  if (!stub) {
    return true;
  }

  MOZ_ASSERT(!icScript_->hasInlinedChild(fallback->pcOffset()));

  // Look for a CallScriptedFunction with a known target.
  Maybe<InlinableCallData> data = FindInlinableCallData(stub);
  if (data.isNothing()) {
    return true;
  }

  MOZ_ASSERT(!data->icScript);

  // Decide whether to inline the target.
  if (!shouldInline(data->target, stub, loc)) {
    return true;
  }

  // We only inline FunCall if we are calling the js::fun_call builtin.
  MOZ_ASSERT_IF(loc.getOp() == JSOp::FunCall,
                data->callFlags.getArgFormat() == CallFlags::FunCall);

  ICScript* newICScript = createInlinedICScript(data->target, loc);
  if (!newICScript) {
    return false;
  }

  CacheIRWriter writer(cx());
  Int32OperandId argcId(writer.setInputOperandId(0));
  cloneSharedPrefix(stub, data->endOfSharedPrefix, writer);

  writer.callInlinedFunction(data->calleeOperand, argcId, newICScript,
                             data->callFlags);
  writer.returnFromIC();

  if (!replaceICStub(entry, fallback, writer, CacheKind::Call)) {
    icScript_->removeInlinedChild(fallback->pcOffset());
    return false;
  }

  return true;
}

bool TrialInliner::maybeInlineGetter(ICEntry& entry, ICFallbackStub* fallback,
                                     BytecodeLocation loc) {
  ICCacheIRStub* stub = maybeSingleStub(entry);
  if (!stub) {
    return true;
  }

  MOZ_ASSERT(!icScript_->hasInlinedChild(fallback->pcOffset()));

  Maybe<InlinableGetterData> data = FindInlinableGetterData(stub);
  if (data.isNothing()) {
    return true;
  }

  MOZ_ASSERT(!data->icScript);

  // Decide whether to inline the target.
  if (!shouldInline(data->target, stub, loc)) {
    return true;
  }

  ICScript* newICScript = createInlinedICScript(data->target, loc);
  if (!newICScript) {
    return false;
  }

  CacheIRWriter writer(cx());
  ValOperandId valId(writer.setInputOperandId(0));
  cloneSharedPrefix(stub, data->endOfSharedPrefix, writer);

  writer.callInlinedGetterResult(data->receiverOperand, data->target,
                                 newICScript, data->sameRealm);
  writer.returnFromIC();

  if (!replaceICStub(entry, fallback, writer, CacheKind::GetProp)) {
    icScript_->removeInlinedChild(fallback->pcOffset());
    return false;
  }

  return true;
}

bool TrialInliner::maybeInlineSetter(ICEntry& entry, ICFallbackStub* fallback,
                                     BytecodeLocation loc) {
  ICCacheIRStub* stub = maybeSingleStub(entry);
  if (!stub) {
    return true;
  }

  MOZ_ASSERT(!icScript_->hasInlinedChild(fallback->pcOffset()));

  Maybe<InlinableSetterData> data = FindInlinableSetterData(stub);
  if (data.isNothing()) {
    return true;
  }

  MOZ_ASSERT(!data->icScript);

  // Decide whether to inline the target.
  if (!shouldInline(data->target, stub, loc)) {
    return true;
  }

  ICScript* newICScript = createInlinedICScript(data->target, loc);
  if (!newICScript) {
    return false;
  }

  CacheIRWriter writer(cx());
  ValOperandId objValId(writer.setInputOperandId(0));
  ValOperandId rhsValId(writer.setInputOperandId(1));
  cloneSharedPrefix(stub, data->endOfSharedPrefix, writer);

  writer.callInlinedSetter(data->receiverOperand, data->target,
                           data->rhsOperand, newICScript, data->sameRealm);
  writer.returnFromIC();

  if (!replaceICStub(entry, fallback, writer, CacheKind::SetProp)) {
    icScript_->removeInlinedChild(fallback->pcOffset());
    return false;
  }

  return true;
}

bool TrialInliner::tryInlining() {
  uint32_t numICEntries = icScript_->numICEntries();
  BytecodeLocation startLoc = script_->location();

  for (uint32_t icIndex = 0; icIndex < numICEntries; icIndex++) {
    ICEntry& entry = icScript_->icEntry(icIndex);
    ICFallbackStub* fallback = icScript_->fallbackStub(icIndex);
    BytecodeLocation loc =
        startLoc + BytecodeLocationOffset(fallback->pcOffset());
    JSOp op = loc.getOp();
    switch (op) {
      case JSOp::Call:
      case JSOp::CallIgnoresRv:
      case JSOp::CallIter:
      case JSOp::FunCall:
      case JSOp::New:
      case JSOp::SuperCall:
        if (!maybeInlineCall(entry, fallback, loc)) {
          return false;
        }
        break;
      case JSOp::GetProp:
        if (!maybeInlineGetter(entry, fallback, loc)) {
          return false;
        }
        break;
      case JSOp::SetProp:
      case JSOp::StrictSetProp:
        if (!maybeInlineSetter(entry, fallback, loc)) {
          return false;
        }
        break;
      default:
        break;
    }
  }

  return true;
}

bool InliningRoot::addInlinedScript(UniquePtr<ICScript> icScript) {
  return inlinedScripts_.append(std::move(icScript));
}

void InliningRoot::trace(JSTracer* trc) {
  TraceEdge(trc, &owningScript_, "inlining-root-owning-script");
  for (auto& inlinedScript : inlinedScripts_) {
    inlinedScript->trace(trc);
  }
}

void InliningRoot::purgeOptimizedStubs(Zone* zone) {
  for (auto& inlinedScript : inlinedScripts_) {
    inlinedScript->purgeOptimizedStubs(zone);
  }
}

void InliningRoot::resetWarmUpCounts(uint32_t count) {
  for (auto& inlinedScript : inlinedScripts_) {
    inlinedScript->resetWarmUpCount(count);
  }
}

}  // namespace jit
}  // namespace js
