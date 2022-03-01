/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineFrame-inl.h"

#include <algorithm>

#include "debugger/DebugAPI.h"
#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "vm/EnvironmentObject.h"
#include "vm/JSContext.h"

#include "jit/JSJitFrameIter-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::jit;

static void TraceLocals(BaselineFrame* frame, JSTracer* trc, unsigned start,
                        unsigned end) {
  if (start < end) {
    // Stack grows down.
    Value* last = frame->valueSlot(end - 1);
    TraceRootRange(trc, end - start, last, "baseline-stack");
  }
}

void BaselineFrame::trace(JSTracer* trc, const JSJitFrameIter& frameIterator) {
  replaceCalleeToken(TraceCalleeToken(trc, calleeToken()));

  // Trace |this|, actual and formal args.
  if (isFunctionFrame()) {
    TraceRoot(trc, &thisArgument(), "baseline-this");

    unsigned numArgs = std::max(numActualArgs(), numFormalArgs());
    TraceRootRange(trc, numArgs + isConstructing(), argv(), "baseline-args");
  }

  // Trace environment chain, if it exists.
  if (envChain_) {
    TraceRoot(trc, &envChain_, "baseline-envchain");
  }

  // Trace return value.
  if (hasReturnValue()) {
    TraceRoot(trc, returnValue().address(), "baseline-rval");
  }

  if (isEvalFrame() && script()->isDirectEvalInFunction()) {
    TraceRoot(trc, evalNewTargetAddress(), "baseline-evalNewTarget");
  }

  if (hasArgsObj()) {
    TraceRoot(trc, &argsObj_, "baseline-args-obj");
  }

  if (runningInInterpreter()) {
    TraceRoot(trc, &interpreterScript_, "baseline-interpreterScript");
  }

  // Trace locals and stack values.
  JSScript* script = this->script();
  size_t nfixed = script->nfixed();
  jsbytecode* pc;
  frameIterator.baselineScriptAndPc(nullptr, &pc);
  size_t nlivefixed = script->calculateLiveFixed(pc);

  uint32_t numValueSlots = frameIterator.baselineFrameNumValueSlots();

  // NB: It is possible that numValueSlots could be zero, even if nfixed is
  // nonzero.  This is the case when we're initializing the environment chain or
  // failed the prologue stack check.
  if (numValueSlots > 0) {
    MOZ_ASSERT(nfixed <= numValueSlots);

    if (nfixed == nlivefixed) {
      // All locals are live.
      TraceLocals(this, trc, 0, numValueSlots);
    } else {
      // Trace operand stack.
      TraceLocals(this, trc, nfixed, numValueSlots);

      // Clear dead block-scoped locals.
      while (nfixed > nlivefixed) {
        unaliasedLocal(--nfixed).setUndefined();
      }

      // Trace live locals.
      TraceLocals(this, trc, 0, nlivefixed);
    }
  }

  if (auto* debugEnvs = script->realm()->debugEnvs()) {
    debugEnvs->traceLiveFrame(trc, this);
  }
}

bool BaselineFrame::uninlineIsProfilerSamplingEnabled(JSContext* cx) {
  return cx->isProfilerSamplingEnabled();
}

bool BaselineFrame::initFunctionEnvironmentObjects(JSContext* cx) {
  return js::InitFunctionEnvironmentObjects(cx, this);
}

bool BaselineFrame::pushVarEnvironment(JSContext* cx, HandleScope scope) {
  return js::PushVarEnvironmentObject(cx, scope, this);
}

void BaselineFrame::setInterpreterFields(JSScript* script, jsbytecode* pc) {
  uint32_t pcOffset = script->pcToOffset(pc);
  interpreterScript_ = script;
  interpreterPC_ = pc;
  MOZ_ASSERT(icScript_);
  interpreterICEntry_ = icScript_->interpreterICEntryFromPCOffset(pcOffset);
}

void BaselineFrame::setInterpreterFieldsForPrologue(JSScript* script) {
  interpreterScript_ = script;
  interpreterPC_ = script->code();
  if (icScript_->numICEntries() > 0) {
    interpreterICEntry_ = &icScript_->icEntry(0);
  } else {
    // If the script does not have any ICEntries (possible for non-function
    // scripts) the interpreterICEntry_ field won't be used. Just set it to
    // nullptr.
    interpreterICEntry_ = nullptr;
  }
}

bool BaselineFrame::initForOsr(InterpreterFrame* fp, uint32_t numStackValues) {
  mozilla::PodZero(this);

  envChain_ = fp->environmentChain();

  if (fp->hasInitialEnvironmentUnchecked()) {
    flags_ |= BaselineFrame::HAS_INITIAL_ENV;
  }

  if (fp->script()->needsArgsObj() && fp->hasArgsObj()) {
    flags_ |= BaselineFrame::HAS_ARGS_OBJ;
    argsObj_ = &fp->argsObj();
  }

  if (fp->hasReturnValue()) {
    setReturnValue(fp->returnValue());
  }

  icScript_ = fp->script()->jitScript()->icScript();

  JSContext* cx =
      fp->script()->runtimeFromMainThread()->mainContextFromOwnThread();

  Activation* interpActivation = cx->activation()->prev();
  jsbytecode* pc = interpActivation->asInterpreter()->regs().pc;
  MOZ_ASSERT(fp->script()->containsPC(pc));

  // We are doing OSR into the Baseline Interpreter. We can get the pc from the
  // C++ interpreter's activation, we just have to skip the JitActivation.
  flags_ |= BaselineFrame::RUNNING_IN_INTERPRETER;
  setInterpreterFields(pc);

#ifdef DEBUG
  debugFrameSize_ = frameSizeForNumValueSlots(numStackValues);
  MOZ_ASSERT(debugNumValueSlots() == numStackValues);
#endif

  for (uint32_t i = 0; i < numStackValues; i++) {
    *valueSlot(i) = fp->slots()[i];
  }

  if (fp->isDebuggee()) {
    // For debuggee frames, update any Debugger.Frame objects for the
    // InterpreterFrame to point to the BaselineFrame.
    if (!DebugAPI::handleBaselineOsr(cx, fp, this)) {
      return false;
    }
    setIsDebuggee();
  }

  return true;
}
