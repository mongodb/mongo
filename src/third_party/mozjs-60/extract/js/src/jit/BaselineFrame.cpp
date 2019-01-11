/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineFrame-inl.h"

#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "vm/Debugger.h"
#include "vm/EnvironmentObject.h"

#include "jit/JitFrames-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::jit;

static void
TraceLocals(BaselineFrame* frame, JSTracer* trc, unsigned start, unsigned end)
{
    if (start < end) {
        // Stack grows down.
        Value* last = frame->valueSlot(end - 1);
        TraceRootRange(trc, end - start, last, "baseline-stack");
    }
}

void
BaselineFrame::trace(JSTracer* trc, const JSJitFrameIter& frameIterator)
{
    replaceCalleeToken(TraceCalleeToken(trc, calleeToken()));

    // Trace |this|, actual and formal args.
    if (isFunctionFrame()) {
        TraceRoot(trc, &thisArgument(), "baseline-this");

        unsigned numArgs = js::Max(numActualArgs(), numFormalArgs());
        TraceRootRange(trc, numArgs + isConstructing(), argv(), "baseline-args");
    }

    // Trace environment chain, if it exists.
    if (envChain_)
        TraceRoot(trc, &envChain_, "baseline-envchain");

    // Trace return value.
    if (hasReturnValue())
        TraceRoot(trc, returnValue().address(), "baseline-rval");

    if (isEvalFrame() && script()->isDirectEvalInFunction())
        TraceRoot(trc, evalNewTargetAddress(), "baseline-evalNewTarget");

    if (hasArgsObj())
        TraceRoot(trc, &argsObj_, "baseline-args-obj");

    // Trace locals and stack values.
    JSScript* script = this->script();
    size_t nfixed = script->nfixed();
    jsbytecode* pc;
    frameIterator.baselineScriptAndPc(nullptr, &pc);
    size_t nlivefixed = script->calculateLiveFixed(pc);

    // NB: It is possible that numValueSlots() could be zero, even if nfixed is
    // nonzero.  This is the case if the function has an early stack check.
    if (numValueSlots() == 0)
        return;

    MOZ_ASSERT(nfixed <= numValueSlots());

    if (nfixed == nlivefixed) {
        // All locals are live.
        TraceLocals(this, trc, 0, numValueSlots());
    } else {
        // Trace operand stack.
        TraceLocals(this, trc, nfixed, numValueSlots());

        // Clear dead block-scoped locals.
        while (nfixed > nlivefixed)
            unaliasedLocal(--nfixed).setUndefined();

        // Trace live locals.
        TraceLocals(this, trc, 0, nlivefixed);
    }

    if (script->compartment()->debugEnvs)
        script->compartment()->debugEnvs->traceLiveFrame(trc, this);
}

bool
BaselineFrame::isNonGlobalEvalFrame() const
{
    return isEvalFrame() && script()->enclosingScope()->as<EvalScope>().isNonGlobal();
}

bool
BaselineFrame::initFunctionEnvironmentObjects(JSContext* cx)
{
    return js::InitFunctionEnvironmentObjects(cx, this);
}

bool
BaselineFrame::pushVarEnvironment(JSContext* cx, HandleScope scope)
{
    return js::PushVarEnvironmentObject(cx, scope, this);
}

bool
BaselineFrame::initForOsr(InterpreterFrame* fp, uint32_t numStackValues)
{
    mozilla::PodZero(this);

    envChain_ = fp->environmentChain();

    if (fp->hasInitialEnvironmentUnchecked())
        flags_ |= BaselineFrame::HAS_INITIAL_ENV;

    if (fp->script()->needsArgsObj() && fp->hasArgsObj()) {
        flags_ |= BaselineFrame::HAS_ARGS_OBJ;
        argsObj_ = &fp->argsObj();
    }

    if (fp->hasReturnValue())
        setReturnValue(fp->returnValue());

    frameSize_ = BaselineFrame::FramePointerOffset +
        BaselineFrame::Size() +
        numStackValues * sizeof(Value);

    MOZ_ASSERT(numValueSlots() == numStackValues);

    for (uint32_t i = 0; i < numStackValues; i++)
        *valueSlot(i) = fp->slots()[i];

    if (fp->isDebuggee()) {
        JSContext* cx = TlsContext.get();

        // For debuggee frames, update any Debugger.Frame objects for the
        // InterpreterFrame to point to the BaselineFrame.

        // The caller pushed a fake return address. ScriptFrameIter, used by the
        // debugger, wants a valid return address, but it's okay to just pick one.
        // In debug mode there's always at least 1 ICEntry (since there are always
        // debug prologue/epilogue calls).
        JSJitFrameIter frame(cx->activation()->asJit());
        MOZ_ASSERT(frame.returnAddress() == nullptr);
        BaselineScript* baseline = fp->script()->baselineScript();
        frame.current()->setReturnAddress(baseline->returnAddressForIC(baseline->icEntry(0)));

        if (!Debugger::handleBaselineOsr(cx, fp, this))
            return false;

        setIsDebuggee();
    }

    return true;
}
