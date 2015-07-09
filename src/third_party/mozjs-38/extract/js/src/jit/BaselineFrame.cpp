/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineFrame-inl.h"

#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "vm/Debugger.h"
#include "vm/ScopeObject.h"

#include "jit/JitFrames-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::jit;

static void
MarkLocals(BaselineFrame* frame, JSTracer* trc, unsigned start, unsigned end)
{
    if (start < end) {
        // Stack grows down.
        Value* last = frame->valueSlot(end - 1);
        gc::MarkValueRootRange(trc, end - start, last, "baseline-stack");
    }
}

void
BaselineFrame::trace(JSTracer* trc, JitFrameIterator& frameIterator)
{
    replaceCalleeToken(MarkCalleeToken(trc, calleeToken()));

    gc::MarkValueRoot(trc, &thisValue(), "baseline-this");

    // Mark actual and formal args.
    if (isNonEvalFunctionFrame()) {
        unsigned numArgs = js::Max(numActualArgs(), numFormalArgs());
        gc::MarkValueRootRange(trc, numArgs, argv(), "baseline-args");
    }

    // Mark scope chain, if it exists.
    if (scopeChain_)
        gc::MarkObjectRoot(trc, &scopeChain_, "baseline-scopechain");

    // Mark return value.
    if (hasReturnValue())
        gc::MarkValueRoot(trc, returnValue().address(), "baseline-rval");

    if (isEvalFrame())
        gc::MarkScriptRoot(trc, &evalScript_, "baseline-evalscript");

    if (hasArgsObj())
        gc::MarkObjectRoot(trc, &argsObj_, "baseline-args-obj");

    // Mark locals and stack values.
    JSScript* script = this->script();
    size_t nfixed = script->nfixed();
    size_t nlivefixed = script->nbodyfixed();

    if (nfixed != nlivefixed) {
        jsbytecode* pc;
        frameIterator.baselineScriptAndPc(nullptr, &pc);

        NestedScopeObject* staticScope = script->getStaticBlockScope(pc);
        while (staticScope && !staticScope->is<StaticBlockObject>())
            staticScope = staticScope->enclosingNestedScope();

        if (staticScope) {
            StaticBlockObject& blockObj = staticScope->as<StaticBlockObject>();
            nlivefixed = blockObj.localOffset() + blockObj.numVariables();
        }
    }

    MOZ_ASSERT(nlivefixed <= nfixed);
    MOZ_ASSERT(nlivefixed >= script->nbodyfixed());

    // NB: It is possible that numValueSlots() could be zero, even if nfixed is
    // nonzero.  This is the case if the function has an early stack check.
    if (numValueSlots() == 0)
        return;

    MOZ_ASSERT(nfixed <= numValueSlots());

    if (nfixed == nlivefixed) {
        // All locals are live.
        MarkLocals(this, trc, 0, numValueSlots());
    } else {
        // Mark operand stack.
        MarkLocals(this, trc, nfixed, numValueSlots());

        // Clear dead block-scoped locals.
        while (nfixed > nlivefixed)
            unaliasedLocal(--nfixed).setMagic(JS_UNINITIALIZED_LEXICAL);

        // Mark live locals.
        MarkLocals(this, trc, 0, nlivefixed);
    }
}

bool
BaselineFrame::copyRawFrameSlots(AutoValueVector* vec) const
{
    unsigned nfixed = script()->nfixed();
    unsigned nformals = numFormalArgs();

    if (!vec->resize(nformals + nfixed))
        return false;

    mozilla::PodCopy(vec->begin(), argv(), nformals);
    for (unsigned i = 0; i < nfixed; i++)
        (*vec)[nformals + i].set(*valueSlot(i));
    return true;
}

bool
BaselineFrame::strictEvalPrologue(JSContext* cx)
{
    MOZ_ASSERT(isStrictEvalFrame());

    CallObject* callobj = CallObject::createForStrictEval(cx, this);
    if (!callobj)
        return false;

    pushOnScopeChain(*callobj);
    flags_ |= HAS_CALL_OBJ;
    return true;
}

bool
BaselineFrame::heavyweightFunPrologue(JSContext* cx)
{
    return initFunctionScopeObjects(cx);
}

bool
BaselineFrame::initFunctionScopeObjects(JSContext* cx)
{
    MOZ_ASSERT(isNonEvalFunctionFrame());
    MOZ_ASSERT(fun()->isHeavyweight());

    CallObject* callobj = CallObject::createForFunction(cx, this);
    if (!callobj)
        return false;

    pushOnScopeChain(*callobj);
    flags_ |= HAS_CALL_OBJ;
    return true;
}

bool
BaselineFrame::initForOsr(InterpreterFrame* fp, uint32_t numStackValues)
{
    mozilla::PodZero(this);

    scopeChain_ = fp->scopeChain();

    if (fp->hasCallObjUnchecked())
        flags_ |= BaselineFrame::HAS_CALL_OBJ;

    if (fp->isEvalFrame()) {
        flags_ |= BaselineFrame::EVAL;
        evalScript_ = fp->script();
    }

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
        JSContext* cx = GetJSContextFromJitCode();

        // For debuggee frames, update any Debugger.Frame objects for the
        // InterpreterFrame to point to the BaselineFrame.

        // The caller pushed a fake return address. ScriptFrameIter, used by the
        // debugger, wants a valid return address, but it's okay to just pick one.
        // In debug mode there's always at least 1 ICEntry (since there are always
        // debug prologue/epilogue calls).
        JitFrameIterator iter(cx);
        MOZ_ASSERT(iter.returnAddress() == nullptr);
        BaselineScript* baseline = fp->script()->baselineScript();
        iter.current()->setReturnAddress(baseline->returnAddressForIC(baseline->icEntry(0)));

        if (!Debugger::handleBaselineOsr(cx, fp, this))
            return false;

        setIsDebuggee();
    }

    return true;
}
