/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/SharedIC.h"

#include "mozilla/Casting.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Sprintf.h"

#include "jslibmath.h"
#include "jstypes.h"

#include "gc/Policy.h"
#include "jit/BaselineCacheIRCompiler.h"
#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineIC.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "jit/SharedICHelpers.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/VMFunctions.h"
#include "vm/Interpreter.h"
#include "vm/StringType.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/SharedICHelpers-inl.h"
#include "vm/Interpreter-inl.h"

using mozilla::BitwiseCast;

namespace js {
namespace jit {

#ifdef JS_JITSPEW
void
FallbackICSpew(JSContext* cx, ICFallbackStub* stub, const char* fmt, ...)
{
    if (JitSpewEnabled(JitSpew_BaselineICFallback)) {
        RootedScript script(cx, GetTopJitJSScript(cx));
        jsbytecode* pc = stub->icEntry()->pc(script);

        char fmtbuf[100];
        va_list args;
        va_start(args, fmt);
        (void) VsprintfLiteral(fmtbuf, fmt, args);
        va_end(args);

        JitSpew(JitSpew_BaselineICFallback,
                "Fallback hit for (%s:%zu) (pc=%zu,line=%d,uses=%d,stubs=%zu): %s",
                script->filename(),
                script->lineno(),
                script->pcToOffset(pc),
                PCToLineNumber(script, pc),
                script->getWarmUpCount(),
                stub->numOptimizedStubs(),
                fmtbuf);
    }
}

void
TypeFallbackICSpew(JSContext* cx, ICTypeMonitor_Fallback* stub, const char* fmt, ...)
{
    if (JitSpewEnabled(JitSpew_BaselineICFallback)) {
        RootedScript script(cx, GetTopJitJSScript(cx));
        jsbytecode* pc = stub->icEntry()->pc(script);

        char fmtbuf[100];
        va_list args;
        va_start(args, fmt);
        (void) VsprintfLiteral(fmtbuf, fmt, args);
        va_end(args);

        JitSpew(JitSpew_BaselineICFallback,
                "Type monitor fallback hit for (%s:%zu) (pc=%zu,line=%d,uses=%d,stubs=%d): %s",
                script->filename(),
                script->lineno(),
                script->pcToOffset(pc),
                PCToLineNumber(script, pc),
                script->getWarmUpCount(),
                (int) stub->numOptimizedMonitorStubs(),
                fmtbuf);
    }
}
#endif // JS_JITSPEW

ICFallbackStub*
ICEntry::fallbackStub() const
{
    return firstStub()->getChainFallback();
}

void
IonICEntry::trace(JSTracer* trc)
{
    TraceManuallyBarrieredEdge(trc, &script_, "IonICEntry::script_");
    traceEntry(trc);
}

void
BaselineICEntry::trace(JSTracer* trc)
{
    traceEntry(trc);
}

void
ICEntry::traceEntry(JSTracer* trc)
{
    if (!hasStub())
        return;
    for (ICStub* stub = firstStub(); stub; stub = stub->next())
        stub->trace(trc);
}

ICStubConstIterator&
ICStubConstIterator::operator++()
{
    MOZ_ASSERT(currentStub_ != nullptr);
    currentStub_ = currentStub_->next();
    return *this;
}


ICStubIterator::ICStubIterator(ICFallbackStub* fallbackStub, bool end)
  : icEntry_(fallbackStub->icEntry()),
    fallbackStub_(fallbackStub),
    previousStub_(nullptr),
    currentStub_(end ? fallbackStub : icEntry_->firstStub()),
    unlinked_(false)
{ }

ICStubIterator&
ICStubIterator::operator++()
{
    MOZ_ASSERT(currentStub_->next() != nullptr);
    if (!unlinked_)
        previousStub_ = currentStub_;
    currentStub_ = currentStub_->next();
    unlinked_ = false;
    return *this;
}

void
ICStubIterator::unlink(JSContext* cx)
{
    MOZ_ASSERT(currentStub_->next() != nullptr);
    MOZ_ASSERT(currentStub_ != fallbackStub_);
    MOZ_ASSERT(!unlinked_);

    fallbackStub_->unlinkStub(cx->zone(), previousStub_, currentStub_);

    // Mark the current iterator position as unlinked, so operator++ works properly.
    unlinked_ = true;
}

/* static */ bool
ICStub::NonCacheIRStubMakesGCCalls(Kind kind)
{
    MOZ_ASSERT(IsValidKind(kind));
    MOZ_ASSERT(!IsCacheIRKind(kind));

    switch (kind) {
      case Call_Fallback:
      case Call_Scripted:
      case Call_AnyScripted:
      case Call_Native:
      case Call_ClassHook:
      case Call_ScriptedApplyArray:
      case Call_ScriptedApplyArguments:
      case Call_ScriptedFunCall:
      case Call_ConstStringSplit:
      case WarmUpCounter_Fallback:
      case RetSub_Fallback:
      // These two fallback stubs don't actually make non-tail calls,
      // but the fallback code for the bailout path needs to pop the stub frame
      // pushed during the bailout.
      case GetProp_Fallback:
      case SetProp_Fallback:
        return true;
      default:
        return false;
    }
}

bool
ICStub::makesGCCalls() const
{
    switch (kind()) {
      case CacheIR_Regular:
        return toCacheIR_Regular()->stubInfo()->makesGCCalls();
      case CacheIR_Monitored:
        return toCacheIR_Monitored()->stubInfo()->makesGCCalls();
      case CacheIR_Updated:
        return toCacheIR_Updated()->stubInfo()->makesGCCalls();
      default:
        return NonCacheIRStubMakesGCCalls(kind());
    }
}

void
ICStub::traceCode(JSTracer* trc, const char* name)
{
    JitCode* stubJitCode = jitCode();
    TraceManuallyBarrieredEdge(trc, &stubJitCode, name);
}

void
ICStub::updateCode(JitCode* code)
{
    // Write barrier on the old code.
    JitCode::writeBarrierPre(jitCode());
    stubCode_ = code->raw();
}

/* static */ void
ICStub::trace(JSTracer* trc)
{
    traceCode(trc, "shared-stub-jitcode");

    // If the stub is a monitored fallback stub, then trace the monitor ICs hanging
    // off of that stub.  We don't need to worry about the regular monitored stubs,
    // because the regular monitored stubs will always have a monitored fallback stub
    // that references the same stub chain.
    if (isMonitoredFallback()) {
        ICTypeMonitor_Fallback* lastMonStub =
            toMonitoredFallbackStub()->maybeFallbackMonitorStub();
        if (lastMonStub) {
            for (ICStubConstIterator iter(lastMonStub->firstMonitorStub());
                 !iter.atEnd();
                 iter++)
            {
                MOZ_ASSERT_IF(iter->next() == nullptr, *iter == lastMonStub);
                iter->trace(trc);
            }
        }
    }

    if (isUpdated()) {
        for (ICStubConstIterator iter(toUpdatedStub()->firstUpdateStub()); !iter.atEnd(); iter++) {
            MOZ_ASSERT_IF(iter->next() == nullptr, iter->isTypeUpdate_Fallback());
            iter->trace(trc);
        }
    }

    switch (kind()) {
      case ICStub::Call_Scripted: {
        ICCall_Scripted* callStub = toCall_Scripted();
        TraceEdge(trc, &callStub->callee(), "baseline-callscripted-callee");
        TraceNullableEdge(trc, &callStub->templateObject(), "baseline-callscripted-template");
        break;
      }
      case ICStub::Call_Native: {
        ICCall_Native* callStub = toCall_Native();
        TraceEdge(trc, &callStub->callee(), "baseline-callnative-callee");
        TraceNullableEdge(trc, &callStub->templateObject(), "baseline-callnative-template");
        break;
      }
      case ICStub::Call_ClassHook: {
        ICCall_ClassHook* callStub = toCall_ClassHook();
        TraceNullableEdge(trc, &callStub->templateObject(), "baseline-callclasshook-template");
        break;
      }
      case ICStub::Call_ConstStringSplit: {
        ICCall_ConstStringSplit* callStub = toCall_ConstStringSplit();
        TraceEdge(trc, &callStub->templateObject(), "baseline-callstringsplit-template");
        TraceEdge(trc, &callStub->expectedSep(), "baseline-callstringsplit-sep");
        TraceEdge(trc, &callStub->expectedStr(), "baseline-callstringsplit-str");
        break;
      }
      case ICStub::TypeMonitor_SingleObject: {
        ICTypeMonitor_SingleObject* monitorStub = toTypeMonitor_SingleObject();
        TraceEdge(trc, &monitorStub->object(), "baseline-monitor-singleton");
        break;
      }
      case ICStub::TypeMonitor_ObjectGroup: {
        ICTypeMonitor_ObjectGroup* monitorStub = toTypeMonitor_ObjectGroup();
        TraceEdge(trc, &monitorStub->group(), "baseline-monitor-group");
        break;
      }
      case ICStub::TypeUpdate_SingleObject: {
        ICTypeUpdate_SingleObject* updateStub = toTypeUpdate_SingleObject();
        TraceEdge(trc, &updateStub->object(), "baseline-update-singleton");
        break;
      }
      case ICStub::TypeUpdate_ObjectGroup: {
        ICTypeUpdate_ObjectGroup* updateStub = toTypeUpdate_ObjectGroup();
        TraceEdge(trc, &updateStub->group(), "baseline-update-group");
        break;
      }
      case ICStub::NewArray_Fallback: {
        ICNewArray_Fallback* stub = toNewArray_Fallback();
        TraceNullableEdge(trc, &stub->templateObject(), "baseline-newarray-template");
        TraceEdge(trc, &stub->templateGroup(), "baseline-newarray-template-group");
        break;
      }
      case ICStub::NewObject_Fallback: {
        ICNewObject_Fallback* stub = toNewObject_Fallback();
        TraceNullableEdge(trc, &stub->templateObject(), "baseline-newobject-template");
        break;
      }
      case ICStub::Rest_Fallback: {
        ICRest_Fallback* stub = toRest_Fallback();
        TraceEdge(trc, &stub->templateObject(), "baseline-rest-template");
        break;
      }
      case ICStub::CacheIR_Regular:
        TraceCacheIRStub(trc, this, toCacheIR_Regular()->stubInfo());
        break;
      case ICStub::CacheIR_Monitored:
        TraceCacheIRStub(trc, this, toCacheIR_Monitored()->stubInfo());
        break;
      case ICStub::CacheIR_Updated: {
        ICCacheIR_Updated* stub = toCacheIR_Updated();
        TraceNullableEdge(trc, &stub->updateStubGroup(), "baseline-update-stub-group");
        TraceEdge(trc, &stub->updateStubId(), "baseline-update-stub-id");
        TraceCacheIRStub(trc, this, stub->stubInfo());
        break;
      }
      default:
        break;
    }
}

void
ICFallbackStub::unlinkStub(Zone* zone, ICStub* prev, ICStub* stub)
{
    MOZ_ASSERT(stub->next());

    // If stub is the last optimized stub, update lastStubPtrAddr.
    if (stub->next() == this) {
        MOZ_ASSERT(lastStubPtrAddr_ == stub->addressOfNext());
        if (prev)
            lastStubPtrAddr_ = prev->addressOfNext();
        else
            lastStubPtrAddr_ = icEntry()->addressOfFirstStub();
        *lastStubPtrAddr_ = this;
    } else {
        if (prev) {
            MOZ_ASSERT(prev->next() == stub);
            prev->setNext(stub->next());
        } else {
            MOZ_ASSERT(icEntry()->firstStub() == stub);
            icEntry()->setFirstStub(stub->next());
        }
    }

    state_.trackUnlinkedStub();

    if (zone->needsIncrementalBarrier()) {
        // We are removing edges from ICStub to gcthings. Perform one final trace
        // of the stub for incremental GC, as it must know about those edges.
        stub->trace(zone->barrierTracer());
    }

    if (stub->makesGCCalls() && stub->isMonitored()) {
        // This stub can make calls so we can return to it if it's on the stack.
        // We just have to reset its firstMonitorStub_ field to avoid a stale
        // pointer when purgeOptimizedStubs destroys all optimized monitor
        // stubs (unlinked stubs won't be updated).
        ICTypeMonitor_Fallback* monitorFallback =
            toMonitoredFallbackStub()->maybeFallbackMonitorStub();
        MOZ_ASSERT(monitorFallback);
        stub->toMonitoredStub()->resetFirstMonitorStub(monitorFallback);
    }

#ifdef DEBUG
    // Poison stub code to ensure we don't call this stub again. However, if
    // this stub can make calls, a pointer to it may be stored in a stub frame
    // on the stack, so we can't touch the stubCode_ or GC will crash when
    // tracing this pointer.
    if (!stub->makesGCCalls())
        stub->stubCode_ = (uint8_t*)0xbad;
#endif
}

void
ICFallbackStub::unlinkStubsWithKind(JSContext* cx, ICStub::Kind kind)
{
    for (ICStubIterator iter = beginChain(); !iter.atEnd(); iter++) {
        if (iter->kind() == kind)
            iter.unlink(cx);
    }
}

void
ICFallbackStub::discardStubs(JSContext* cx)
{
    for (ICStubIterator iter = beginChain(); !iter.atEnd(); iter++)
        iter.unlink(cx);
}

void
ICTypeMonitor_Fallback::resetMonitorStubChain(Zone* zone)
{
    if (zone->needsIncrementalBarrier()) {
        // We are removing edges from monitored stubs to gcthings (JitCode).
        // Perform one final trace of all monitor stubs for incremental GC,
        // as it must know about those edges.
        for (ICStub* s = firstMonitorStub_; !s->isTypeMonitor_Fallback(); s = s->next())
            s->trace(zone->barrierTracer());
    }

    firstMonitorStub_ = this;
    numOptimizedMonitorStubs_ = 0;

    if (hasFallbackStub_) {
        lastMonitorStubPtrAddr_ = nullptr;

        // Reset firstMonitorStub_ field of all monitored stubs.
        for (ICStubConstIterator iter = mainFallbackStub_->beginChainConst();
             !iter.atEnd(); iter++)
        {
            if (!iter->isMonitored())
                continue;
            iter->toMonitoredStub()->resetFirstMonitorStub(this);
        }
    } else {
        icEntry_->setFirstStub(this);
        lastMonitorStubPtrAddr_ = icEntry_->addressOfFirstStub();
    }
}

void
ICUpdatedStub::resetUpdateStubChain(Zone* zone)
{
    while (!firstUpdateStub_->isTypeUpdate_Fallback()) {
        if (zone->needsIncrementalBarrier()) {
            // We are removing edges from update stubs to gcthings (JitCode).
            // Perform one final trace of all update stubs for incremental GC,
            // as it must know about those edges.
            firstUpdateStub_->trace(zone->barrierTracer());
        }
        firstUpdateStub_ = firstUpdateStub_->next();
    }

    numOptimizedStubs_ = 0;
}

ICMonitoredStub::ICMonitoredStub(Kind kind, JitCode* stubCode, ICStub* firstMonitorStub)
  : ICStub(kind, ICStub::Monitored, stubCode),
    firstMonitorStub_(firstMonitorStub)
{
    // In order to silence Coverity - null pointer dereference checker
    MOZ_ASSERT(firstMonitorStub_);
    // If the first monitored stub is a ICTypeMonitor_Fallback stub, then
    // double check that _its_ firstMonitorStub is the same as this one.
    MOZ_ASSERT_IF(firstMonitorStub_->isTypeMonitor_Fallback(),
                  firstMonitorStub_->toTypeMonitor_Fallback()->firstMonitorStub() ==
                     firstMonitorStub_);
}

bool
ICMonitoredFallbackStub::initMonitoringChain(JSContext* cx, JSScript* script)
{
    MOZ_ASSERT(fallbackMonitorStub_ == nullptr);

    ICTypeMonitor_Fallback::Compiler compiler(cx, this);
    ICStubSpace* space = script->baselineScript()->fallbackStubSpace();
    ICTypeMonitor_Fallback* stub = compiler.getStub(space);
    if (!stub)
        return false;
    fallbackMonitorStub_ = stub;
    return true;
}

bool
ICMonitoredFallbackStub::addMonitorStubForValue(JSContext* cx, BaselineFrame* frame,
                                                StackTypeSet* types, HandleValue val)
{
    ICTypeMonitor_Fallback* typeMonitorFallback = getFallbackMonitorStub(cx, frame->script());
    if (!typeMonitorFallback)
        return false;
    return typeMonitorFallback->addMonitorStubForValue(cx, frame, types, val);
}

bool
ICUpdatedStub::initUpdatingChain(JSContext* cx, ICStubSpace* space)
{
    MOZ_ASSERT(firstUpdateStub_ == nullptr);

    ICTypeUpdate_Fallback::Compiler compiler(cx);
    ICTypeUpdate_Fallback* stub = compiler.getStub(space);
    if (!stub)
        return false;

    firstUpdateStub_ = stub;
    return true;
}

JitCode*
ICStubCompiler::getStubCode()
{
    JitCompartment* comp = cx->compartment()->jitCompartment();

    // Check for existing cached stubcode.
    uint32_t stubKey = getKey();
    JitCode* stubCode = comp->getStubCode(stubKey);
    if (stubCode)
        return stubCode;

    // Compile new stubcode.
    JitContext jctx(cx, nullptr);
    MacroAssembler masm;
#ifndef JS_USE_LINK_REGISTER
    // The first value contains the return addres,
    // which we pull into ICTailCallReg for tail calls.
    masm.adjustFrame(sizeof(intptr_t));
#endif
#ifdef JS_CODEGEN_ARM
    masm.setSecondScratchReg(BaselineSecondScratchReg);
#endif

    if (!generateStubCode(masm))
        return nullptr;
    Linker linker(masm);
    AutoFlushICache afc("getStubCode");
    Rooted<JitCode*> newStubCode(cx, linker.newCode(cx, CodeKind::Baseline));
    if (!newStubCode)
        return nullptr;

    // Cache newly compiled stubcode.
    if (!comp->putStubCode(cx, stubKey, newStubCode))
        return nullptr;

    // After generating code, run postGenerateStubCode().  We must not fail
    // after this point.
    postGenerateStubCode(masm, newStubCode);

    MOZ_ASSERT(entersStubFrame_ == ICStub::NonCacheIRStubMakesGCCalls(kind));
    MOZ_ASSERT(!inStubFrame_);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(newStubCode, "BaselineIC");
#endif

    return newStubCode;
}

bool
ICStubCompiler::tailCallVM(const VMFunction& fun, MacroAssembler& masm)
{
    TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(fun);
    MOZ_ASSERT(fun.expectTailCall == TailCall);
    uint32_t argSize = fun.explicitStackSlots() * sizeof(void*);
    if (engine_ == Engine::Baseline) {
        EmitBaselineTailCallVM(code, masm, argSize);
    } else {
        uint32_t stackSize = argSize + fun.extraValuesToPop * sizeof(Value);
        EmitIonTailCallVM(code, masm, stackSize);
    }
    return true;
}

bool
ICStubCompiler::callVM(const VMFunction& fun, MacroAssembler& masm)
{
    MOZ_ASSERT(inStubFrame_);

    TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(fun);
    MOZ_ASSERT(fun.expectTailCall == NonTailCall);
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitBaselineCallVM(code, masm);
    return true;
}

void
ICStubCompiler::enterStubFrame(MacroAssembler& masm, Register scratch)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    EmitBaselineEnterStubFrame(masm, scratch);
#ifdef DEBUG
    framePushedAtEnterStubFrame_ = masm.framePushed();
#endif

    MOZ_ASSERT(!inStubFrame_);
    inStubFrame_ = true;

#ifdef DEBUG
    entersStubFrame_ = true;
#endif
}

void
ICStubCompiler::assumeStubFrame()
{
    MOZ_ASSERT(!inStubFrame_);
    inStubFrame_ = true;

#ifdef DEBUG
    entersStubFrame_ = true;

    // |framePushed| isn't tracked precisely in ICStubs, so simply assume it to
    // be STUB_FRAME_SIZE so that assertions don't fail in leaveStubFrame.
    framePushedAtEnterStubFrame_ = STUB_FRAME_SIZE;
#endif
}

void
ICStubCompiler::leaveStubFrame(MacroAssembler& masm, bool calledIntoIon)
{
    MOZ_ASSERT(entersStubFrame_ && inStubFrame_);
    inStubFrame_ = false;

    MOZ_ASSERT(engine_ == Engine::Baseline);
#ifdef DEBUG
    masm.setFramePushed(framePushedAtEnterStubFrame_);
    if (calledIntoIon)
        masm.adjustFrame(sizeof(intptr_t)); // Calls into ion have this extra.
#endif
    EmitBaselineLeaveStubFrame(masm, calledIntoIon);
}

void
ICStubCompiler::pushStubPayload(MacroAssembler& masm, Register scratch)
{
    if (engine_ == Engine::IonSharedIC) {
        masm.push(Imm32(0));
        return;
    }

    if (inStubFrame_) {
        masm.loadPtr(Address(BaselineFrameReg, 0), scratch);
        masm.pushBaselineFramePtr(scratch, scratch);
    } else {
        masm.pushBaselineFramePtr(BaselineFrameReg, scratch);
    }
}

void
ICStubCompiler::PushStubPayload(MacroAssembler& masm, Register scratch)
{
    pushStubPayload(masm, scratch);
    masm.adjustFrame(sizeof(intptr_t));
}

SharedStubInfo::SharedStubInfo(JSContext* cx, void* payload, ICEntry* icEntry)
  : maybeFrame_(nullptr),
    outerScript_(cx),
    innerScript_(cx),
    icEntry_(icEntry)
{
    if (payload) {
        maybeFrame_ = (BaselineFrame*) payload;
        outerScript_ = maybeFrame_->script();
        innerScript_ = maybeFrame_->script();
    } else {
        IonICEntry* entry = (IonICEntry*) icEntry;
        innerScript_ = entry->script();
        // outerScript_ is initialized lazily.
    }
}

HandleScript
SharedStubInfo::outerScript(JSContext* cx)
{
    if (!outerScript_) {
        js::jit::JitActivationIterator actIter(cx);
        JSJitFrameIter it(actIter->asJit());
        MOZ_ASSERT(it.isExitFrame());
        ++it;
        MOZ_ASSERT(it.isIonJS());
        outerScript_ = it.script();
        MOZ_ASSERT(!it.ionScript()->invalidated());
    }
    return outerScript_;
}

//
// BinaryArith_Fallback
//

static bool
DoBinaryArithFallback(JSContext* cx, void* payload, ICBinaryArith_Fallback* stub_,
                      HandleValue lhs, HandleValue rhs, MutableHandleValue ret)
{
    SharedStubInfo info(cx, payload, stub_->icEntry());
    ICStubCompiler::Engine engine = info.engine();

    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICBinaryArith_Fallback*> stub(engine, info.maybeFrame(), stub_);

    jsbytecode* pc = info.pc();
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "BinaryArith(%s,%d,%d)", CodeName[op],
            int(lhs.isDouble() ? JSVAL_TYPE_DOUBLE : lhs.extractNonDoubleType()),
            int(rhs.isDouble() ? JSVAL_TYPE_DOUBLE : rhs.extractNonDoubleType()));

    // Don't pass lhs/rhs directly, we need the original values when
    // generating stubs.
    RootedValue lhsCopy(cx, lhs);
    RootedValue rhsCopy(cx, rhs);

    // Perform the compare operation.
    switch(op) {
      case JSOP_ADD:
        // Do an add.
        if (!AddValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_SUB:
        if (!SubValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_MUL:
        if (!MulValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_DIV:
        if (!DivValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_MOD:
        if (!ModValues(cx, &lhsCopy, &rhsCopy, ret))
            return false;
        break;
      case JSOP_POW:
        if (!math_pow_handle(cx, lhsCopy, rhsCopy, ret))
            return false;
        break;
      case JSOP_BITOR: {
        int32_t result;
        if (!BitOr(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_BITXOR: {
        int32_t result;
        if (!BitXor(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_BITAND: {
        int32_t result;
        if (!BitAnd(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_LSH: {
        int32_t result;
        if (!BitLsh(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_RSH: {
        int32_t result;
        if (!BitRsh(cx, lhs, rhs, &result))
            return false;
        ret.setInt32(result);
        break;
      }
      case JSOP_URSH: {
        if (!UrshOperation(cx, lhs, rhs, ret))
            return false;
        break;
      }
      default:
        MOZ_CRASH("Unhandled baseline arith op");
    }

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (ret.isDouble())
        stub->setSawDoubleResult();

    // Check to see if a new stub should be generated.
    if (stub->numOptimizedStubs() >= ICBinaryArith_Fallback::MAX_OPTIMIZED_STUBS) {
        stub->noteUnoptimizableOperands();
        return true;
    }

    // Handle string concat.
    if (op == JSOP_ADD) {
        if (lhs.isString() && rhs.isString()) {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(String, String) stub", CodeName[op]);
            MOZ_ASSERT(ret.isString());
            ICBinaryArith_StringConcat::Compiler compiler(cx, engine);
            ICStub* strcatStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
            if (!strcatStub)
                return false;
            stub->addNewStub(strcatStub);
            return true;
        }

        if ((lhs.isString() && rhs.isObject()) || (lhs.isObject() && rhs.isString())) {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", CodeName[op],
                    lhs.isString() ? "String" : "Object",
                    lhs.isString() ? "Object" : "String");
            MOZ_ASSERT(ret.isString());
            ICBinaryArith_StringObjectConcat::Compiler compiler(cx, engine, lhs.isString());
            ICStub* strcatStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
            if (!strcatStub)
                return false;
            stub->addNewStub(strcatStub);
            return true;
        }
    }

    if (((lhs.isBoolean() && (rhs.isBoolean() || rhs.isInt32())) ||
         (rhs.isBoolean() && (lhs.isBoolean() || lhs.isInt32()))) &&
        (op == JSOP_ADD || op == JSOP_SUB || op == JSOP_BITOR || op == JSOP_BITAND ||
         op == JSOP_BITXOR))
    {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", CodeName[op],
                lhs.isBoolean() ? "Boolean" : "Int32", rhs.isBoolean() ? "Boolean" : "Int32");
        ICBinaryArith_BooleanWithInt32::Compiler compiler(cx, op, engine,
                                                          lhs.isBoolean(), rhs.isBoolean());
        ICStub* arithStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
        if (!arithStub)
            return false;
        stub->addNewStub(arithStub);
        return true;
    }

    // Handle only int32 or double.
    if (!lhs.isNumber() || !rhs.isNumber()) {
        stub->noteUnoptimizableOperands();
        return true;
    }

    MOZ_ASSERT(ret.isNumber());

    if (lhs.isDouble() || rhs.isDouble() || ret.isDouble()) {
        if (!cx->runtime()->jitSupportsFloatingPoint)
            return true;

        switch (op) {
          case JSOP_ADD:
          case JSOP_SUB:
          case JSOP_MUL:
          case JSOP_DIV:
          case JSOP_MOD: {
            // Unlink int32 stubs, it's faster to always use the double stub.
            stub->unlinkStubsWithKind(cx, ICStub::BinaryArith_Int32);
            JitSpew(JitSpew_BaselineIC, "  Generating %s(Double, Double) stub", CodeName[op]);

            ICBinaryArith_Double::Compiler compiler(cx, op, engine);
            ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
            if (!doubleStub)
                return false;
            stub->addNewStub(doubleStub);
            return true;
          }
          default:
            break;
        }
    }

    if (lhs.isInt32() && rhs.isInt32() && op != JSOP_POW) {
        bool allowDouble = ret.isDouble();
        if (allowDouble)
            stub->unlinkStubsWithKind(cx, ICStub::BinaryArith_Int32);
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Int32, Int32%s) stub", CodeName[op],
                allowDouble ? " => Double" : "");
        ICBinaryArith_Int32::Compiler compilerInt32(cx, op, engine, allowDouble);
        ICStub* int32Stub = compilerInt32.getStub(compilerInt32.getStubSpace(info.outerScript(cx)));
        if (!int32Stub)
            return false;
        stub->addNewStub(int32Stub);
        return true;
    }

    // Handle Double <BITOP> Int32 or Int32 <BITOP> Double case.
    if (((lhs.isDouble() && rhs.isInt32()) || (lhs.isInt32() && rhs.isDouble())) &&
        ret.isInt32())
    {
        switch(op) {
          case JSOP_BITOR:
          case JSOP_BITXOR:
          case JSOP_BITAND: {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", CodeName[op],
                        lhs.isDouble() ? "Double" : "Int32",
                        lhs.isDouble() ? "Int32" : "Double");
            ICBinaryArith_DoubleWithInt32::Compiler compiler(cx, op, engine, lhs.isDouble());
            ICStub* optStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
            if (!optStub)
                return false;
            stub->addNewStub(optStub);
            return true;
          }
          default:
            break;
        }
    }

    stub->noteUnoptimizableOperands();
    return true;
}

typedef bool (*DoBinaryArithFallbackFn)(JSContext*, void*, ICBinaryArith_Fallback*,
                                        HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoBinaryArithFallbackInfo =
    FunctionInfo<DoBinaryArithFallbackFn>(DoBinaryArithFallback, "DoBinaryArithFallback",
                                          TailCall, PopValues(2));

bool
ICBinaryArith_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoBinaryArithFallbackInfo, masm);
}

static bool
DoConcatStrings(JSContext* cx, HandleString lhs, HandleString rhs, MutableHandleValue res)
{
    JSString* result = ConcatStrings<CanGC>(cx, lhs, rhs);
    if (!result)
        return false;

    res.setString(result);
    return true;
}

typedef bool (*DoConcatStringsFn)(JSContext*, HandleString, HandleString, MutableHandleValue);
static const VMFunction DoConcatStringsInfo =
    FunctionInfo<DoConcatStringsFn>(DoConcatStrings, "DoConcatStrings", TailCall);

bool
ICBinaryArith_StringConcat::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestString(Assembler::NotEqual, R0, &failure);
    masm.branchTestString(Assembler::NotEqual, R1, &failure);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    masm.unboxString(R0, R0.scratchReg());
    masm.unboxString(R1, R1.scratchReg());

    masm.push(R1.scratchReg());
    masm.push(R0.scratchReg());
    if (!tailCallVM(DoConcatStringsInfo, masm))
        return false;

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

static JSString*
ConvertObjectToStringForConcat(JSContext* cx, HandleValue obj)
{
    MOZ_ASSERT(obj.isObject());
    RootedValue rootedObj(cx, obj);
    if (!ToPrimitive(cx, &rootedObj))
        return nullptr;
    return ToString<CanGC>(cx, rootedObj);
}

static bool
DoConcatStringObject(JSContext* cx, bool lhsIsString, HandleValue lhs, HandleValue rhs,
                     MutableHandleValue res)
{
    JSString* lstr = nullptr;
    JSString* rstr = nullptr;
    if (lhsIsString) {
        // Convert rhs first.
        MOZ_ASSERT(lhs.isString() && rhs.isObject());
        rstr = ConvertObjectToStringForConcat(cx, rhs);
        if (!rstr)
            return false;

        // lhs is already string.
        lstr = lhs.toString();
    } else {
        MOZ_ASSERT(rhs.isString() && lhs.isObject());
        // Convert lhs first.
        lstr = ConvertObjectToStringForConcat(cx, lhs);
        if (!lstr)
            return false;

        // rhs is already string.
        rstr = rhs.toString();
    }

    JSString* str = ConcatStrings<NoGC>(cx, lstr, rstr);
    if (!str) {
        RootedString nlstr(cx, lstr), nrstr(cx, rstr);
        str = ConcatStrings<CanGC>(cx, nlstr, nrstr);
        if (!str)
            return false;
    }

    // Technically, we need to call TypeScript::MonitorString for this PC, however
    // it was called when this stub was attached so it's OK.

    res.setString(str);
    return true;
}

typedef bool (*DoConcatStringObjectFn)(JSContext*, bool lhsIsString, HandleValue, HandleValue,
                                       MutableHandleValue);
static const VMFunction DoConcatStringObjectInfo =
    FunctionInfo<DoConcatStringObjectFn>(DoConcatStringObject, "DoConcatStringObject", TailCall,
                                         PopValues(2));

bool
ICBinaryArith_StringObjectConcat::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    if (lhsIsString_) {
        masm.branchTestString(Assembler::NotEqual, R0, &failure);
        masm.branchTestObject(Assembler::NotEqual, R1, &failure);
    } else {
        masm.branchTestObject(Assembler::NotEqual, R0, &failure);
        masm.branchTestString(Assembler::NotEqual, R1, &failure);
    }

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Sync for the decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(Imm32(lhsIsString_));
    if (!tailCallVM(DoConcatStringObjectInfo, masm))
        return false;

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICBinaryArith_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.ensureDouble(R0, FloatReg0, &failure);
    masm.ensureDouble(R1, FloatReg1, &failure);

    switch (op) {
      case JSOP_ADD:
        masm.addDouble(FloatReg1, FloatReg0);
        break;
      case JSOP_SUB:
        masm.subDouble(FloatReg1, FloatReg0);
        break;
      case JSOP_MUL:
        masm.mulDouble(FloatReg1, FloatReg0);
        break;
      case JSOP_DIV:
        masm.divDouble(FloatReg1, FloatReg0);
        break;
      case JSOP_MOD:
        masm.setupUnalignedABICall(R0.scratchReg());
        masm.passABIArg(FloatReg0, MoveOp::DOUBLE);
        masm.passABIArg(FloatReg1, MoveOp::DOUBLE);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, NumberMod), MoveOp::DOUBLE);
        MOZ_ASSERT(ReturnDoubleReg == FloatReg0);
        break;
      default:
        MOZ_CRASH("Unexpected op");
    }

    masm.boxDouble(FloatReg0, R0, FloatReg0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICBinaryArith_BooleanWithInt32::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    if (lhsIsBool_)
        masm.branchTestBoolean(Assembler::NotEqual, R0, &failure);
    else
        masm.branchTestInt32(Assembler::NotEqual, R0, &failure);

    if (rhsIsBool_)
        masm.branchTestBoolean(Assembler::NotEqual, R1, &failure);
    else
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    Register lhsReg = lhsIsBool_ ? masm.extractBoolean(R0, ExtractTemp0)
                                 : masm.extractInt32(R0, ExtractTemp0);
    Register rhsReg = rhsIsBool_ ? masm.extractBoolean(R1, ExtractTemp1)
                                 : masm.extractInt32(R1, ExtractTemp1);

    MOZ_ASSERT(op_ == JSOP_ADD || op_ == JSOP_SUB ||
               op_ == JSOP_BITOR || op_ == JSOP_BITXOR || op_ == JSOP_BITAND);

    switch(op_) {
      case JSOP_ADD: {
        Label fixOverflow;

        masm.branchAdd32(Assembler::Overflow, rhsReg, lhsReg, &fixOverflow);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);

        masm.bind(&fixOverflow);
        masm.sub32(rhsReg, lhsReg);
        // Proceed to failure below.
        break;
      }
      case JSOP_SUB: {
        Label fixOverflow;

        masm.branchSub32(Assembler::Overflow, rhsReg, lhsReg, &fixOverflow);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);

        masm.bind(&fixOverflow);
        masm.add32(rhsReg, lhsReg);
        // Proceed to failure below.
        break;
      }
      case JSOP_BITOR: {
        masm.or32(rhsReg, lhsReg);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);
        break;
      }
      case JSOP_BITXOR: {
        masm.xor32(rhsReg, lhsReg);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);
        break;
      }
      case JSOP_BITAND: {
        masm.and32(rhsReg, lhsReg);
        masm.tagValue(JSVAL_TYPE_INT32, lhsReg, R0);
        EmitReturnFromIC(masm);
        break;
      }
      default:
       MOZ_CRASH("Unhandled op for BinaryArith_BooleanWithInt32.");
    }

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICBinaryArith_DoubleWithInt32::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(op == JSOP_BITOR || op == JSOP_BITAND || op == JSOP_BITXOR);

    Label failure;
    Register intReg;
    Register scratchReg;
    if (lhsIsDouble_) {
        masm.branchTestDouble(Assembler::NotEqual, R0, &failure);
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
        intReg = masm.extractInt32(R1, ExtractTemp0);
        masm.unboxDouble(R0, FloatReg0);
        scratchReg = R0.scratchReg();
    } else {
        masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
        masm.branchTestDouble(Assembler::NotEqual, R1, &failure);
        intReg = masm.extractInt32(R0, ExtractTemp0);
        masm.unboxDouble(R1, FloatReg0);
        scratchReg = R1.scratchReg();
    }

    // Truncate the double to an int32.
    {
        Label doneTruncate;
        Label truncateABICall;
        masm.branchTruncateDoubleMaybeModUint32(FloatReg0, scratchReg, &truncateABICall);
        masm.jump(&doneTruncate);

        masm.bind(&truncateABICall);
        masm.push(intReg);
        masm.setupUnalignedABICall(scratchReg);
        masm.passABIArg(FloatReg0, MoveOp::DOUBLE);
        masm.callWithABI(mozilla::BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32),
                         MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);
        masm.storeCallInt32Result(scratchReg);
        masm.pop(intReg);

        masm.bind(&doneTruncate);
    }

    Register intReg2 = scratchReg;
    // All handled ops commute, so no need to worry about ordering.
    switch(op) {
      case JSOP_BITOR:
        masm.or32(intReg, intReg2);
        break;
      case JSOP_BITXOR:
        masm.xor32(intReg, intReg2);
        break;
      case JSOP_BITAND:
        masm.and32(intReg, intReg2);
        break;
      default:
       MOZ_CRASH("Unhandled op for BinaryArith_DoubleWithInt32.");
    }
    masm.tagValue(JSVAL_TYPE_INT32, intReg2, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// UnaryArith_Fallback
//

static bool
DoUnaryArithFallback(JSContext* cx, void* payload, ICUnaryArith_Fallback* stub_,
                     HandleValue val, MutableHandleValue res)
{
    SharedStubInfo info(cx, payload, stub_->icEntry());
    ICStubCompiler::Engine engine = info.engine();

    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICUnaryArith_Fallback*> stub(engine, info.maybeFrame(), stub_);

    jsbytecode* pc = info.pc();
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "UnaryArith(%s)", CodeName[op]);

    switch (op) {
      case JSOP_BITNOT: {
        int32_t result;
        if (!BitNot(cx, val, &result))
            return false;
        res.setInt32(result);
        break;
      }
      case JSOP_NEG:
        if (!NegOperation(cx, val, res))
            return false;
        break;
      default:
        MOZ_CRASH("Unexpected op");
    }

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (res.isDouble())
        stub->setSawDoubleResult();

    if (stub->numOptimizedStubs() >= ICUnaryArith_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard/replace stubs.
        return true;
    }

    if (val.isInt32() && res.isInt32()) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Int32 => Int32) stub", CodeName[op]);
        ICUnaryArith_Int32::Compiler compiler(cx, op, engine);
        ICStub* int32Stub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
        if (!int32Stub)
            return false;
        stub->addNewStub(int32Stub);
        return true;
    }

    if (val.isNumber() && res.isNumber() && cx->runtime()->jitSupportsFloatingPoint) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Number => Number) stub", CodeName[op]);

        // Unlink int32 stubs, the double stub handles both cases and TI specializes for both.
        stub->unlinkStubsWithKind(cx, ICStub::UnaryArith_Int32);

        ICUnaryArith_Double::Compiler compiler(cx, op, engine);
        ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
        if (!doubleStub)
            return false;
        stub->addNewStub(doubleStub);
        return true;
    }

    return true;
}

typedef bool (*DoUnaryArithFallbackFn)(JSContext*, void*, ICUnaryArith_Fallback*,
                                       HandleValue, MutableHandleValue);
static const VMFunction DoUnaryArithFallbackInfo =
    FunctionInfo<DoUnaryArithFallbackFn>(DoUnaryArithFallback, "DoUnaryArithFallback", TailCall,
                                         PopValues(1));

bool
ICUnaryArith_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);

    // Push arguments.
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoUnaryArithFallbackInfo, masm);
}

bool
ICUnaryArith_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.ensureDouble(R0, FloatReg0, &failure);

    MOZ_ASSERT(op == JSOP_NEG || op == JSOP_BITNOT);

    if (op == JSOP_NEG) {
        masm.negateDouble(FloatReg0);
        masm.boxDouble(FloatReg0, R0, FloatReg0);
    } else {
        // Truncate the double to an int32.
        Register scratchReg = R1.scratchReg();

        Label doneTruncate;
        Label truncateABICall;
        masm.branchTruncateDoubleMaybeModUint32(FloatReg0, scratchReg, &truncateABICall);
        masm.jump(&doneTruncate);

        masm.bind(&truncateABICall);
        masm.setupUnalignedABICall(scratchReg);
        masm.passABIArg(FloatReg0, MoveOp::DOUBLE);
        masm.callWithABI(BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32),
                         MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);
        masm.storeCallInt32Result(scratchReg);

        masm.bind(&doneTruncate);
        masm.not32(scratchReg);
        masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
    }

    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_Fallback
//

static bool
DoCompareFallback(JSContext* cx, void* payload, ICCompare_Fallback* stub_, HandleValue lhs,
                  HandleValue rhs, MutableHandleValue ret)
{
    SharedStubInfo info(cx, payload, stub_->icEntry());
    ICStubCompiler::Engine engine = info.engine();

    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICCompare_Fallback*> stub(engine, info.maybeFrame(), stub_);

    jsbytecode* pc = info.pc();
    JSOp op = JSOp(*pc);

    FallbackICSpew(cx, stub, "Compare(%s)", CodeName[op]);

    // Case operations in a CONDSWITCH are performing strict equality.
    if (op == JSOP_CASE)
        op = JSOP_STRICTEQ;

    // Don't pass lhs/rhs directly, we need the original values when
    // generating stubs.
    RootedValue lhsCopy(cx, lhs);
    RootedValue rhsCopy(cx, rhs);

    // Perform the compare operation.
    bool out;
    switch (op) {
      case JSOP_LT:
        if (!LessThan(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_LE:
        if (!LessThanOrEqual(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_GT:
        if (!GreaterThan(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_GE:
        if (!GreaterThanOrEqual(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_EQ:
        if (!LooselyEqual<true>(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_NE:
        if (!LooselyEqual<false>(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_STRICTEQ:
        if (!StrictlyEqual<true>(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      case JSOP_STRICTNE:
        if (!StrictlyEqual<false>(cx, &lhsCopy, &rhsCopy, &out))
            return false;
        break;
      default:
        MOZ_ASSERT_UNREACHABLE("Unhandled baseline compare op");
        return false;
    }

    ret.setBoolean(out);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Check to see if a new stub should be generated.
    if (stub->numOptimizedStubs() >= ICCompare_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        return true;
    }

    if (engine ==  ICStubEngine::Baseline) {
        RootedScript script(cx, info.outerScript(cx));
        CompareIRGenerator gen(cx, script, pc, stub->state().mode(), op, lhs, rhs);
        bool attached = false;
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Regular,
                                                        engine, script, stub, &attached);
            if (newStub)
                 JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
            return true;
        }
    }

    // Try to generate new stubs.
    if (lhs.isInt32() && rhs.isInt32()) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Int32, Int32) stub", CodeName[op]);
        ICCompare_Int32::Compiler compiler(cx, op, engine);
        ICStub* int32Stub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
        if (!int32Stub)
            return false;

        stub->addNewStub(int32Stub);
        return true;
    }

    if (!cx->runtime()->jitSupportsFloatingPoint && (lhs.isNumber() || rhs.isNumber()))
        return true;

    if (lhs.isNumber() && rhs.isNumber()) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Number, Number) stub", CodeName[op]);

        // Unlink int32 stubs, it's faster to always use the double stub.
        stub->unlinkStubsWithKind(cx, ICStub::Compare_Int32);

        ICCompare_Double::Compiler compiler(cx, op, engine);
        ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
        if (!doubleStub)
            return false;

        stub->addNewStub(doubleStub);
        return true;
    }

    if ((lhs.isNumber() && rhs.isUndefined()) ||
        (lhs.isUndefined() && rhs.isNumber()))
    {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", CodeName[op],
                    rhs.isUndefined() ? "Number" : "Undefined",
                    rhs.isUndefined() ? "Undefined" : "Number");
        ICCompare_NumberWithUndefined::Compiler compiler(cx, op, engine, lhs.isUndefined());
        ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
        if (!doubleStub)
            return false;

        stub->addNewStub(doubleStub);
        return true;
    }

    if (lhs.isBoolean() && rhs.isBoolean()) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(Boolean, Boolean) stub", CodeName[op]);
        ICCompare_Boolean::Compiler compiler(cx, op, engine);
        ICStub* booleanStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
        if (!booleanStub)
            return false;

        stub->addNewStub(booleanStub);
        return true;
    }

    if ((lhs.isBoolean() && rhs.isInt32()) || (lhs.isInt32() && rhs.isBoolean())) {
        JitSpew(JitSpew_BaselineIC, "  Generating %s(%s, %s) stub", CodeName[op],
                    rhs.isInt32() ? "Boolean" : "Int32",
                    rhs.isInt32() ? "Int32" : "Boolean");
        ICCompare_Int32WithBoolean::Compiler compiler(cx, op, engine, lhs.isInt32());
        ICStub* optStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
        if (!optStub)
            return false;

        stub->addNewStub(optStub);
        return true;
    }

    if (IsEqualityOp(op)) {
        if (lhs.isString() && rhs.isString() && !stub->hasStub(ICStub::Compare_String)) {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(String, String) stub", CodeName[op]);
            ICCompare_String::Compiler compiler(cx, op, engine);
            ICStub* stringStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
            if (!stringStub)
                return false;

            stub->addNewStub(stringStub);
            return true;
        }

        if (lhs.isSymbol() && rhs.isSymbol() && !stub->hasStub(ICStub::Compare_Symbol)) {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(Symbol, Symbol) stub", CodeName[op]);
            ICCompare_Symbol::Compiler compiler(cx, op, engine);
            ICStub* symbolStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
            if (!symbolStub)
                return false;

            stub->addNewStub(symbolStub);
            return true;
        }

        if (lhs.isObject() && rhs.isObject()) {
            MOZ_ASSERT(!stub->hasStub(ICStub::Compare_Object));
            JitSpew(JitSpew_BaselineIC, "  Generating %s(Object, Object) stub", CodeName[op]);
            ICCompare_Object::Compiler compiler(cx, op, engine);
            ICStub* objectStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
            if (!objectStub)
                return false;

            stub->addNewStub(objectStub);
            return true;
        }

        if (lhs.isNullOrUndefined() || rhs.isNullOrUndefined()) {
            JitSpew(JitSpew_BaselineIC, "  Generating %s(Null/Undef or X, Null/Undef or X) stub",
                    CodeName[op]);
            bool lhsIsUndefined = lhs.isNullOrUndefined();
            bool compareWithNull = lhs.isNull() || rhs.isNull();
            ICCompare_ObjectWithUndefined::Compiler compiler(cx, op, engine,
                                                             lhsIsUndefined, compareWithNull);
            ICStub* objectStub = compiler.getStub(compiler.getStubSpace(info.outerScript(cx)));
            if (!objectStub)
                return false;

            stub->addNewStub(objectStub);
            return true;
        }
    }

    stub->noteUnoptimizableAccess();

    return true;
}

typedef bool (*DoCompareFallbackFn)(JSContext*, void*, ICCompare_Fallback*,
                                    HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoCompareFallbackInfo =
    FunctionInfo<DoCompareFallbackFn>(DoCompareFallback, "DoCompareFallback", TailCall,
                                      PopValues(2));

bool
ICCompare_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());
    return tailCallVM(DoCompareFallbackInfo, masm);
}

//
// Compare_String
//

bool
ICCompare_String::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestString(Assembler::NotEqual, R0, &failure);
    masm.branchTestString(Assembler::NotEqual, R1, &failure);

    MOZ_ASSERT(IsEqualityOp(op));

    Register left = masm.extractString(R0, ExtractTemp0);
    Register right = masm.extractString(R1, ExtractTemp1);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    masm.compareStrings(op, left, right, scratchReg, &failure);
    masm.tagValue(JSVAL_TYPE_BOOLEAN, scratchReg, R0);
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_Symbol
//

bool
ICCompare_Symbol::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestSymbol(Assembler::NotEqual, R0, &failure);
    masm.branchTestSymbol(Assembler::NotEqual, R1, &failure);

    MOZ_ASSERT(IsEqualityOp(op));

    Register left = masm.extractSymbol(R0, ExtractTemp0);
    Register right = masm.extractSymbol(R1, ExtractTemp1);

    Label ifTrue;
    masm.branchPtr(JSOpToCondition(op, /* signed = */true), left, right, &ifTrue);

    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifTrue);
    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_Boolean
//

bool
ICCompare_Boolean::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestBoolean(Assembler::NotEqual, R0, &failure);
    masm.branchTestBoolean(Assembler::NotEqual, R1, &failure);

    Register left = masm.extractInt32(R0, ExtractTemp0);
    Register right = masm.extractInt32(R1, ExtractTemp1);

    // Compare payload regs of R0 and R1.
    Assembler::Condition cond = JSOpToCondition(op, /* signed = */true);
    masm.cmp32Set(cond, left, right, left);

    // Box the result and return
    masm.tagValue(JSVAL_TYPE_BOOLEAN, left, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_NumberWithUndefined
//

bool
ICCompare_NumberWithUndefined::Compiler::generateStubCode(MacroAssembler& masm)
{
    ValueOperand numberOperand, undefinedOperand;
    if (lhsIsUndefined) {
        numberOperand = R1;
        undefinedOperand = R0;
    } else {
        numberOperand = R0;
        undefinedOperand = R1;
    }

    Label failure;
    masm.branchTestNumber(Assembler::NotEqual, numberOperand, &failure);
    masm.branchTestUndefined(Assembler::NotEqual, undefinedOperand, &failure);

    // Comparing a number with undefined will always be true for NE/STRICTNE,
    // and always be false for other compare ops.
    masm.moveValue(BooleanValue(op == JSOP_NE || op == JSOP_STRICTNE), R0);

    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_Object
//

bool
ICCompare_Object::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    MOZ_ASSERT(IsEqualityOp(op));

    Register left = masm.extractObject(R0, ExtractTemp0);
    Register right = masm.extractObject(R1, ExtractTemp1);

    Label ifTrue;
    masm.branchPtr(JSOpToCondition(op, /* signed = */true), left, right, &ifTrue);

    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifTrue);
    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_ObjectWithUndefined
//

bool
ICCompare_ObjectWithUndefined::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(IsEqualityOp(op));

    ValueOperand objectOperand, undefinedOperand;
    if (lhsIsUndefined) {
        objectOperand = R1;
        undefinedOperand = R0;
    } else {
        objectOperand = R0;
        undefinedOperand = R1;
    }

    Label failure;
    if (compareWithNull)
        masm.branchTestNull(Assembler::NotEqual, undefinedOperand, &failure);
    else
        masm.branchTestUndefined(Assembler::NotEqual, undefinedOperand, &failure);

    Label notObject;
    masm.branchTestObject(Assembler::NotEqual, objectOperand, &notObject);

    if (op == JSOP_STRICTEQ || op == JSOP_STRICTNE) {
        // obj !== undefined for all objects.
        masm.moveValue(BooleanValue(op == JSOP_STRICTNE), R0);
        EmitReturnFromIC(masm);
    } else {
        // obj != undefined only where !obj->getClass()->emulatesUndefined()
        Register obj = masm.extractObject(objectOperand, ExtractTemp0);

        // We need a scratch register.
        masm.push(obj);
        Label slow, emulatesUndefined;
        masm.branchIfObjectEmulatesUndefined(obj, obj, &slow, &emulatesUndefined);

        masm.pop(obj);
        masm.moveValue(BooleanValue(op == JSOP_NE), R0);
        EmitReturnFromIC(masm);

        masm.bind(&emulatesUndefined);
        masm.pop(obj);
        masm.moveValue(BooleanValue(op == JSOP_EQ), R0);
        EmitReturnFromIC(masm);

        masm.bind(&slow);
        masm.pop(obj);
        masm.jump(&failure);
    }

    masm.bind(&notObject);

    // Also support null == null or undefined == undefined comparisons.
    Label differentTypes;
    if (compareWithNull)
        masm.branchTestNull(Assembler::NotEqual, objectOperand, &differentTypes);
    else
        masm.branchTestUndefined(Assembler::NotEqual, objectOperand, &differentTypes);

    masm.moveValue(BooleanValue(op == JSOP_STRICTEQ || op == JSOP_EQ), R0);
    EmitReturnFromIC(masm);

    masm.bind(&differentTypes);
    // Also support null == undefined or undefined == null.
    Label neverEqual;
    if (compareWithNull)
        masm.branchTestUndefined(Assembler::NotEqual, objectOperand, &neverEqual);
    else
        masm.branchTestNull(Assembler::NotEqual, objectOperand, &neverEqual);

    masm.moveValue(BooleanValue(op == JSOP_EQ || op == JSOP_STRICTNE), R0);
    EmitReturnFromIC(masm);

    // null/undefined can only be equal to null/undefined or emulatesUndefined.
    masm.bind(&neverEqual);
    masm.moveValue(BooleanValue(op == JSOP_NE || op == JSOP_STRICTNE), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Compare_Int32WithBoolean
//

bool
ICCompare_Int32WithBoolean::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    ValueOperand int32Val;
    ValueOperand boolVal;
    if (lhsIsInt32_) {
        int32Val = R0;
        boolVal = R1;
    } else {
        boolVal = R0;
        int32Val = R1;
    }
    masm.branchTestBoolean(Assembler::NotEqual, boolVal, &failure);
    masm.branchTestInt32(Assembler::NotEqual, int32Val, &failure);

    if (op_ == JSOP_STRICTEQ || op_ == JSOP_STRICTNE) {
        // Ints and booleans are never strictly equal, always strictly not equal.
        masm.moveValue(BooleanValue(op_ == JSOP_STRICTNE), R0);
        EmitReturnFromIC(masm);
    } else {
        Register boolReg = masm.extractBoolean(boolVal, ExtractTemp0);
        Register int32Reg = masm.extractInt32(int32Val, ExtractTemp1);

        // Compare payload regs of R0 and R1.
        Assembler::Condition cond = JSOpToCondition(op_, /* signed = */true);
        masm.cmp32Set(cond, (lhsIsInt32_ ? int32Reg : boolReg),
                      (lhsIsInt32_ ? boolReg : int32Reg), R0.scratchReg());

        // Box the result and return
        masm.tagValue(JSVAL_TYPE_BOOLEAN, R0.scratchReg(), R0);
        EmitReturnFromIC(masm);
    }

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// GetProp_Fallback
//

void
StripPreliminaryObjectStubs(JSContext* cx, ICFallbackStub* stub)
{
    // Before the new script properties analysis has been performed on a type,
    // all instances of that type have the maximum number of fixed slots.
    // Afterwards, the objects (even the preliminary ones) might be changed
    // to reduce the number of fixed slots they have. If we generate stubs for
    // both the old and new number of fixed slots, the stub will look
    // polymorphic to IonBuilder when it is actually monomorphic. To avoid
    // this, strip out any stubs for preliminary objects before attaching a new
    // stub which isn't on a preliminary object.

    for (ICStubIterator iter = stub->beginChain(); !iter.atEnd(); iter++) {
        if (iter->isCacheIR_Regular() && iter->toCacheIR_Regular()->hasPreliminaryObject())
            iter.unlink(cx);
        else if (iter->isCacheIR_Monitored() && iter->toCacheIR_Monitored()->hasPreliminaryObject())
            iter.unlink(cx);
        else if (iter->isCacheIR_Updated() && iter->toCacheIR_Updated()->hasPreliminaryObject())
            iter.unlink(cx);
    }
}

static bool
ComputeGetPropResult(JSContext* cx, BaselineFrame* frame, JSOp op, HandlePropertyName name,
                     MutableHandleValue val, MutableHandleValue res)
{
    // Handle arguments.length and arguments.callee on optimized arguments, as
    // it is not an object.
    if (val.isMagic(JS_OPTIMIZED_ARGUMENTS) && IsOptimizedArguments(frame, val)) {
        if (op == JSOP_LENGTH) {
            res.setInt32(frame->numActualArgs());
        } else {
            MOZ_ASSERT(name == cx->names().callee);
            MOZ_ASSERT(frame->script()->hasMappedArgsObj());
            res.setObject(*frame->callee());
        }
    } else {
        if (op == JSOP_GETBOUNDNAME) {
            RootedObject env(cx, &val.toObject());
            RootedId id(cx, NameToId(name));
            if (!GetNameBoundInEnvironment(cx, env, id, res))
                return false;
        } else {
            MOZ_ASSERT(op == JSOP_GETPROP || op == JSOP_CALLPROP || op == JSOP_LENGTH);
            if (!GetProperty(cx, val, name, res))
                return false;
        }
    }

    return true;
}

static bool
DoGetPropFallback(JSContext* cx, BaselineFrame* frame, ICGetProp_Fallback* stub_,
                  MutableHandleValue val, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetProp_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub_->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "GetProp(%s)", CodeName[op]);

    MOZ_ASSERT(op == JSOP_GETPROP ||
               op == JSOP_CALLPROP ||
               op == JSOP_LENGTH ||
               op == JSOP_GETBOUNDNAME);

    RootedPropertyName name(cx, script->getName(pc));

    // There are some reasons we can fail to attach a stub that are temporary.
    // We want to avoid calling noteUnoptimizableAccess() if the reason we
    // failed to attach a stub is one of those temporary reasons, since we might
    // end up attaching a stub for the exact same access later.
    bool isTemporarilyUnoptimizable = false;

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    bool attached = false;
    if (stub->state().canAttachStub()) {
        RootedValue idVal(cx, StringValue(name));
        GetPropIRGenerator gen(cx, script, pc, CacheKind::GetProp, stub->state().mode(),
                               &isTemporarilyUnoptimizable, val, idVal, val,
                               GetPropertyResultFlags::All);
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Monitored,
                                                        ICStubEngine::Baseline, script,
                                                        stub, &attached);
            if (newStub) {
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
                if (gen.shouldNotePreliminaryObjectStub())
                    newStub->toCacheIR_Monitored()->notePreliminaryObject();
                else if (gen.shouldUnlinkPreliminaryObjectStubs())
                    StripPreliminaryObjectStubs(cx, stub);
            }
        }
        if (!attached && !isTemporarilyUnoptimizable)
            stub->state().trackNotAttached();
    }

    if (!ComputeGetPropResult(cx, frame, op, name, val, res))
        return false;

    StackTypeSet* types = TypeScript::BytecodeTypes(script, pc);
    TypeScript::Monitor(cx, script, pc, types, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, frame, types, res))
        return false;

    if (attached)
        return true;

    MOZ_ASSERT(!attached);
    if (!isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();

    return true;
}

static bool
DoGetPropSuperFallback(JSContext* cx, BaselineFrame* frame, ICGetProp_Fallback* stub_,
                       HandleValue receiver, MutableHandleValue val, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetProp_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub_->icEntry()->pc(script);
    FallbackICSpew(cx, stub, "GetPropSuper(%s)", CodeName[JSOp(*pc)]);

    MOZ_ASSERT(JSOp(*pc) == JSOP_GETPROP_SUPER);

    RootedPropertyName name(cx, script->getName(pc));

    // There are some reasons we can fail to attach a stub that are temporary.
    // We want to avoid calling noteUnoptimizableAccess() if the reason we
    // failed to attach a stub is one of those temporary reasons, since we might
    // end up attaching a stub for the exact same access later.
    bool isTemporarilyUnoptimizable = false;

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    bool attached = false;
    if (stub->state().canAttachStub()) {
        RootedValue idVal(cx, StringValue(name));
        GetPropIRGenerator gen(cx, script, pc, CacheKind::GetPropSuper, stub->state().mode(),
                               &isTemporarilyUnoptimizable, val, idVal, receiver,
                               GetPropertyResultFlags::All);
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Monitored,
                                                        ICStubEngine::Baseline, script,
                                                        stub, &attached);
            if (newStub) {
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
                if (gen.shouldNotePreliminaryObjectStub())
                    newStub->toCacheIR_Monitored()->notePreliminaryObject();
                else if (gen.shouldUnlinkPreliminaryObjectStubs())
                    StripPreliminaryObjectStubs(cx, stub);
            }
        }
        if (!attached && !isTemporarilyUnoptimizable)
            stub->state().trackNotAttached();
    }

    // |val| is [[HomeObject]].[[Prototype]] which must be Object
    RootedObject valObj(cx, &val.toObject());
    if (!GetProperty(cx, valObj, receiver, name, res))
        return false;

    StackTypeSet* types = TypeScript::BytecodeTypes(script, pc);
    TypeScript::Monitor(cx, script, pc, types, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, frame, types, res))
        return false;

    if (attached)
        return true;

    MOZ_ASSERT(!attached);
    if (!isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();

    return true;
}

typedef bool (*DoGetPropFallbackFn)(JSContext*, BaselineFrame*, ICGetProp_Fallback*,
                                    MutableHandleValue, MutableHandleValue);
static const VMFunction DoGetPropFallbackInfo =
    FunctionInfo<DoGetPropFallbackFn>(DoGetPropFallback, "DoGetPropFallback", TailCall,
                                      PopValues(1));

typedef bool (*DoGetPropSuperFallbackFn)(JSContext*, BaselineFrame*, ICGetProp_Fallback*,
                                         HandleValue, MutableHandleValue, MutableHandleValue);
static const VMFunction DoGetPropSuperFallbackInfo =
    FunctionInfo<DoGetPropSuperFallbackFn>(DoGetPropSuperFallback, "DoGetPropSuperFallback",
                                           TailCall);

bool
ICGetProp_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    // Super property getters use a |this| that differs from base object
    if (hasReceiver_) {
        // Push arguments.
        masm.pushValue(R0);
        masm.pushValue(R1);
        masm.push(ICStubReg);
        masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

        if (!tailCallVM(DoGetPropSuperFallbackInfo, masm))
            return false;
    } else {
        // Ensure stack is fully synced for the expression decompiler.
        masm.pushValue(R0);

        // Push arguments.
        masm.pushValue(R0);
        masm.push(ICStubReg);
        masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

        if (!tailCallVM(DoGetPropFallbackInfo, masm))
            return false;
    }

    // This is the resume point used when bailout rewrites call stack to undo
    // Ion inlined frames. The return address pushed onto reconstructed stack
    // will point here.
    assumeStubFrame();
    bailoutReturnOffset_.bind(masm.currentOffset());

    leaveStubFrame(masm, true);

    // When we get here, ICStubReg contains the ICGetProp_Fallback stub,
    // which we can't use to enter the TypeMonitor IC, because it's a MonitoredFallbackStub
    // instead of a MonitoredStub. So, we cheat. Note that we must have a
    // non-null fallbackMonitorStub here because InitFromBailout delazifies.
    masm.loadPtr(Address(ICStubReg, ICMonitoredFallbackStub::offsetOfFallbackMonitorStub()),
                 ICStubReg);
    EmitEnterTypeMonitorIC(masm, ICTypeMonitor_Fallback::offsetOfFirstMonitorStub());

    return true;
}

void
ICGetProp_Fallback::Compiler::postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code)
{
    if (engine_ == Engine::Baseline) {
        BailoutReturnStub kind = hasReceiver_ ? BailoutReturnStub::GetPropSuper
                                              : BailoutReturnStub::GetProp;
        void* address = code->raw() + bailoutReturnOffset_.offset();
        cx->compartment()->jitCompartment()->initBailoutReturnAddr(address, getKey(), kind);
    }
}

void
LoadTypedThingData(MacroAssembler& masm, TypedThingLayout layout, Register obj, Register result)
{
    switch (layout) {
      case Layout_TypedArray:
        masm.loadPtr(Address(obj, TypedArrayObject::dataOffset()), result);
        break;
      case Layout_OutlineTypedObject:
        masm.loadPtr(Address(obj, OutlineTypedObject::offsetOfData()), result);
        break;
      case Layout_InlineTypedObject:
        masm.computeEffectiveAddress(Address(obj, InlineTypedObject::offsetOfDataStart()), result);
        break;
      default:
        MOZ_CRASH();
    }
}

void
BaselineScript::noteAccessedGetter(uint32_t pcOffset)
{
    ICEntry& entry = icEntryFromPCOffset(pcOffset);
    ICFallbackStub* stub = entry.fallbackStub();

    if (stub->isGetProp_Fallback())
        stub->toGetProp_Fallback()->noteAccessedGetter();
}

// TypeMonitor_Fallback
//

bool
ICTypeMonitor_Fallback::addMonitorStubForValue(JSContext* cx, BaselineFrame* frame,
                                               StackTypeSet* types, HandleValue val)
{
    MOZ_ASSERT(types);

    // Don't attach too many SingleObject/ObjectGroup stubs. If the value is a
    // primitive or if we will attach an any-object stub, we can handle this
    // with a single PrimitiveSet or AnyValue stub so we always optimize.
    if (numOptimizedMonitorStubs_ >= MAX_OPTIMIZED_STUBS &&
        val.isObject() &&
        !types->unknownObject())
    {
        return true;
    }

    bool wasDetachedMonitorChain = lastMonitorStubPtrAddr_ == nullptr;
    MOZ_ASSERT_IF(wasDetachedMonitorChain, numOptimizedMonitorStubs_ == 0);

    if (types->unknown()) {
        // The TypeSet got marked as unknown so attach a stub that always
        // succeeds.

        // Check for existing TypeMonitor_AnyValue stubs.
        for (ICStubConstIterator iter(firstMonitorStub()); !iter.atEnd(); iter++) {
            if (iter->isTypeMonitor_AnyValue())
                return true;
        }

        // Discard existing stubs.
        resetMonitorStubChain(cx->zone());
        wasDetachedMonitorChain = (lastMonitorStubPtrAddr_ == nullptr);

        ICTypeMonitor_AnyValue::Compiler compiler(cx);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(frame->script()));
        if (!stub) {
            ReportOutOfMemory(cx);
            return false;
        }

        JitSpew(JitSpew_BaselineIC, "  Added TypeMonitor stub %p for any value", stub);
        addOptimizedMonitorStub(stub);

    } else if (val.isPrimitive() || types->unknownObject()) {
        if (val.isMagic(JS_UNINITIALIZED_LEXICAL))
            return true;
        MOZ_ASSERT(!val.isMagic());
        JSValueType type = val.isDouble() ? JSVAL_TYPE_DOUBLE : val.extractNonDoubleType();

        // Check for existing TypeMonitor stub.
        ICTypeMonitor_PrimitiveSet* existingStub = nullptr;
        for (ICStubConstIterator iter(firstMonitorStub()); !iter.atEnd(); iter++) {
            if (iter->isTypeMonitor_PrimitiveSet()) {
                existingStub = iter->toTypeMonitor_PrimitiveSet();
                if (existingStub->containsType(type))
                    return true;
            }
        }

        if (val.isObject()) {
            // Check for existing SingleObject/ObjectGroup stubs and discard
            // stubs if we find one. Ideally we would discard just these stubs,
            // but unlinking individual type monitor stubs is somewhat
            // complicated.
            MOZ_ASSERT(types->unknownObject());
            bool hasObjectStubs = false;
            for (ICStubConstIterator iter(firstMonitorStub()); !iter.atEnd(); iter++) {
                if (iter->isTypeMonitor_SingleObject() || iter->isTypeMonitor_ObjectGroup()) {
                    hasObjectStubs = true;
                    break;
                }
            }
            if (hasObjectStubs) {
                resetMonitorStubChain(cx->zone());
                wasDetachedMonitorChain = (lastMonitorStubPtrAddr_ == nullptr);
                existingStub = nullptr;
            }
        }

        ICTypeMonitor_PrimitiveSet::Compiler compiler(cx, existingStub, type);
        ICStub* stub = existingStub
                       ? compiler.updateStub()
                       : compiler.getStub(compiler.getStubSpace(frame->script()));
        if (!stub) {
            ReportOutOfMemory(cx);
            return false;
        }

        JitSpew(JitSpew_BaselineIC, "  %s TypeMonitor stub %p for primitive type %d",
                existingStub ? "Modified existing" : "Created new", stub, type);

        if (!existingStub) {
            MOZ_ASSERT(!hasStub(TypeMonitor_PrimitiveSet));
            addOptimizedMonitorStub(stub);
        }

    } else if (val.toObject().isSingleton()) {
        RootedObject obj(cx, &val.toObject());

        // Check for existing TypeMonitor stub.
        for (ICStubConstIterator iter(firstMonitorStub()); !iter.atEnd(); iter++) {
            if (iter->isTypeMonitor_SingleObject() &&
                iter->toTypeMonitor_SingleObject()->object() == obj)
            {
                return true;
            }
        }

        ICTypeMonitor_SingleObject::Compiler compiler(cx, obj);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(frame->script()));
        if (!stub) {
            ReportOutOfMemory(cx);
            return false;
        }

        JitSpew(JitSpew_BaselineIC, "  Added TypeMonitor stub %p for singleton %p",
                stub, obj.get());

        addOptimizedMonitorStub(stub);

    } else {
        RootedObjectGroup group(cx, val.toObject().group());

        // Check for existing TypeMonitor stub.
        for (ICStubConstIterator iter(firstMonitorStub()); !iter.atEnd(); iter++) {
            if (iter->isTypeMonitor_ObjectGroup() &&
                iter->toTypeMonitor_ObjectGroup()->group() == group)
            {
                return true;
            }
        }

        ICTypeMonitor_ObjectGroup::Compiler compiler(cx, group);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(frame->script()));
        if (!stub) {
            ReportOutOfMemory(cx);
            return false;
        }

        JitSpew(JitSpew_BaselineIC, "  Added TypeMonitor stub %p for ObjectGroup %p",
                stub, group.get());

        addOptimizedMonitorStub(stub);
    }

    bool firstMonitorStubAdded = wasDetachedMonitorChain && (numOptimizedMonitorStubs_ > 0);

    if (firstMonitorStubAdded) {
        // Was an empty monitor chain before, but a new stub was added.  This is the
        // only time that any main stubs' firstMonitorStub fields need to be updated to
        // refer to the newly added monitor stub.
        ICStub* firstStub = mainFallbackStub_->icEntry()->firstStub();
        for (ICStubConstIterator iter(firstStub); !iter.atEnd(); iter++) {
            // Non-monitored stubs are used if the result has always the same type,
            // e.g. a StringLength stub will always return int32.
            if (!iter->isMonitored())
                continue;

            // Since we just added the first optimized monitoring stub, any
            // existing main stub's |firstMonitorStub| MUST be pointing to the fallback
            // monitor stub (i.e. this stub).
            MOZ_ASSERT(iter->toMonitoredStub()->firstMonitorStub() == this);
            iter->toMonitoredStub()->updateFirstMonitorStub(firstMonitorStub_);
        }
    }

    return true;
}

static bool
DoTypeMonitorFallback(JSContext* cx, BaselineFrame* frame, ICTypeMonitor_Fallback* stub,
                      HandleValue value, MutableHandleValue res)
{
    JSScript* script = frame->script();
    jsbytecode* pc = stub->icEntry()->pc(script);
    TypeFallbackICSpew(cx, stub, "TypeMonitor");

    // Copy input value to res.
    res.set(value);

    if (MOZ_UNLIKELY(value.isMagic())) {
        // It's possible that we arrived here from bailing out of Ion, and that
        // Ion proved that the value is dead and optimized out. In such cases,
        // do nothing. However, it's also possible that we have an uninitialized
        // this, in which case we should not look for other magic values.

        if (value.whyMagic() == JS_OPTIMIZED_OUT) {
            MOZ_ASSERT(!stub->monitorsThis());
            return true;
        }

        // In derived class constructors (including nested arrows/eval), the
        // |this| argument or GETALIASEDVAR can return the magic TDZ value.
        MOZ_ASSERT(value.isMagic(JS_UNINITIALIZED_LEXICAL));
        MOZ_ASSERT(frame->isFunctionFrame() || frame->isEvalFrame());
        MOZ_ASSERT(stub->monitorsThis() ||
                   *GetNextPc(pc) == JSOP_CHECKTHIS ||
                   *GetNextPc(pc) == JSOP_CHECKTHISREINIT ||
                   *GetNextPc(pc) == JSOP_CHECKRETURN);
        if (stub->monitorsThis())
            TypeScript::SetThis(cx, script, TypeSet::UnknownType());
        else
            TypeScript::Monitor(cx, script, pc, TypeSet::UnknownType());
        return true;
    }

    // Note: ideally we would merge this if-else statement with the one below,
    // but that triggers an MSVC 2015 compiler bug. See bug 1363054.
    StackTypeSet* types;
    uint32_t argument;
    if (stub->monitorsArgument(&argument))
        types = TypeScript::ArgTypes(script, argument);
    else if (stub->monitorsThis())
        types = TypeScript::ThisTypes(script);
    else
        types = TypeScript::BytecodeTypes(script, pc);

    if (stub->monitorsArgument(&argument)) {
        MOZ_ASSERT(pc == script->code());
        TypeScript::SetArgument(cx, script, argument, value);
    } else if (stub->monitorsThis()) {
        MOZ_ASSERT(pc == script->code());
        TypeScript::SetThis(cx, script, value);
    } else {
        TypeScript::Monitor(cx, script, pc, types, value);
    }

    if (MOZ_UNLIKELY(stub->invalid()))
        return true;

    return stub->addMonitorStubForValue(cx, frame, types, value);
}

typedef bool (*DoTypeMonitorFallbackFn)(JSContext*, BaselineFrame*, ICTypeMonitor_Fallback*,
                                        HandleValue, MutableHandleValue);
static const VMFunction DoTypeMonitorFallbackInfo =
    FunctionInfo<DoTypeMonitorFallbackFn>(DoTypeMonitorFallback, "DoTypeMonitorFallback",
                                          TailCall);

bool
ICTypeMonitor_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    masm.pushValue(R0);
    masm.push(ICStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    return tailCallVM(DoTypeMonitorFallbackInfo, masm);
}

bool
ICTypeMonitor_PrimitiveSet::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label success;
    if ((flags_ & TypeToFlag(JSVAL_TYPE_INT32)) && !(flags_ & TypeToFlag(JSVAL_TYPE_DOUBLE)))
        masm.branchTestInt32(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_DOUBLE))
        masm.branchTestNumber(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_UNDEFINED))
        masm.branchTestUndefined(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_BOOLEAN))
        masm.branchTestBoolean(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_STRING))
        masm.branchTestString(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_SYMBOL))
        masm.branchTestSymbol(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_OBJECT))
        masm.branchTestObject(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_NULL))
        masm.branchTestNull(Assembler::Equal, R0, &success);

    EmitStubGuardFailure(masm);

    masm.bind(&success);
    EmitReturnFromIC(masm);
    return true;
}

static void
MaybeWorkAroundAmdBug(MacroAssembler& masm)
{
    // Attempt to work around an AMD bug (see bug 1034706 and bug 1281759), by
    // inserting 32-bytes of NOPs.
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    if (CPUInfo::NeedAmdBugWorkaround()) {
        masm.nop(9);
        masm.nop(9);
        masm.nop(9);
        masm.nop(5);
    }
#endif
}

bool
ICTypeMonitor_SingleObject::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    MaybeWorkAroundAmdBug(masm);

    // Guard on the object's identity.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    Address expectedObject(ICStubReg, ICTypeMonitor_SingleObject::offsetOfObject());
    masm.branchPtr(Assembler::NotEqual, expectedObject, obj, &failure);
    MaybeWorkAroundAmdBug(masm);

    EmitReturnFromIC(masm);
    MaybeWorkAroundAmdBug(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICTypeMonitor_ObjectGroup::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    MaybeWorkAroundAmdBug(masm);

    // Guard on the object's ObjectGroup. No Spectre mitigations are needed
    // here: we're just recording type information for Ion compilation and
    // it's safe to speculatively return.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    Address expectedGroup(ICStubReg, ICTypeMonitor_ObjectGroup::offsetOfGroup());
    masm.branchTestObjGroupNoSpectreMitigations(Assembler::NotEqual, obj, expectedGroup,
                                                R1.scratchReg(), &failure);
    MaybeWorkAroundAmdBug(masm);

    EmitReturnFromIC(masm);
    MaybeWorkAroundAmdBug(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICTypeMonitor_AnyValue::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitReturnFromIC(masm);
    return true;
}

bool
ICUpdatedStub::addUpdateStubForValue(JSContext* cx, HandleScript outerScript, HandleObject obj,
                                     HandleObjectGroup group, HandleId id, HandleValue val)
{
    EnsureTrackPropertyTypes(cx, obj, id);

    // Make sure that undefined values are explicitly included in the property
    // types for an object if generating a stub to write an undefined value.
    if (val.isUndefined() && CanHaveEmptyPropertyTypesForOwnProperty(obj)) {
        MOZ_ASSERT(obj->group() == group);
        AddTypePropertyId(cx, obj, id, val);
    }

    bool unknown = false, unknownObject = false;
    if (group->unknownProperties()) {
        unknown = unknownObject = true;
    } else {
        if (HeapTypeSet* types = group->maybeGetProperty(id)) {
            unknown = types->unknown();
            unknownObject = types->unknownObject();
        } else {
            // We don't record null/undefined types for certain TypedObject
            // properties. In these cases |types| is allowed to be nullptr
            // without implying unknown types. See DoTypeUpdateFallback.
            MOZ_ASSERT(obj->is<TypedObject>());
            MOZ_ASSERT(val.isNullOrUndefined());
        }
    }
    MOZ_ASSERT_IF(unknown, unknownObject);

    // Don't attach too many SingleObject/ObjectGroup stubs unless we can
    // replace them with a single PrimitiveSet or AnyValue stub.
    if (numOptimizedStubs_ >= MAX_OPTIMIZED_STUBS &&
        val.isObject() &&
        !unknownObject)
    {
        return true;
    }

    if (unknown) {
        // Attach a stub that always succeeds. We should not have a
        // TypeUpdate_AnyValue stub yet.
        MOZ_ASSERT(!hasTypeUpdateStub(TypeUpdate_AnyValue));

        // Discard existing stubs.
        resetUpdateStubChain(cx->zone());

        ICTypeUpdate_AnyValue::Compiler compiler(cx);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(outerScript));
        if (!stub)
            return false;

        JitSpew(JitSpew_BaselineIC, "  Added TypeUpdate stub %p for any value", stub);
        addOptimizedUpdateStub(stub);

    } else if (val.isPrimitive() || unknownObject) {
        JSValueType type = val.isDouble() ? JSVAL_TYPE_DOUBLE : val.extractNonDoubleType();

        // Check for existing TypeUpdate stub.
        ICTypeUpdate_PrimitiveSet* existingStub = nullptr;
        for (ICStubConstIterator iter(firstUpdateStub_); !iter.atEnd(); iter++) {
            if (iter->isTypeUpdate_PrimitiveSet()) {
                existingStub = iter->toTypeUpdate_PrimitiveSet();
                MOZ_ASSERT(!existingStub->containsType(type));
            }
        }

        if (val.isObject()) {
            // Discard existing ObjectGroup/SingleObject stubs.
            resetUpdateStubChain(cx->zone());
            if (existingStub)
                addOptimizedUpdateStub(existingStub);
        }

        ICTypeUpdate_PrimitiveSet::Compiler compiler(cx, existingStub, type);
        ICStub* stub = existingStub ? compiler.updateStub()
                                    : compiler.getStub(compiler.getStubSpace(outerScript));
        if (!stub)
            return false;
        if (!existingStub) {
            MOZ_ASSERT(!hasTypeUpdateStub(TypeUpdate_PrimitiveSet));
            addOptimizedUpdateStub(stub);
        }

        JitSpew(JitSpew_BaselineIC, "  %s TypeUpdate stub %p for primitive type %d",
                existingStub ? "Modified existing" : "Created new", stub, type);

    } else if (val.toObject().isSingleton()) {
        RootedObject obj(cx, &val.toObject());

#ifdef DEBUG
        // We should not have a stub for this object.
        for (ICStubConstIterator iter(firstUpdateStub_); !iter.atEnd(); iter++) {
            MOZ_ASSERT_IF(iter->isTypeUpdate_SingleObject(),
                          iter->toTypeUpdate_SingleObject()->object() != obj);
        }
#endif

        ICTypeUpdate_SingleObject::Compiler compiler(cx, obj);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(outerScript));
        if (!stub)
            return false;

        JitSpew(JitSpew_BaselineIC, "  Added TypeUpdate stub %p for singleton %p", stub, obj.get());

        addOptimizedUpdateStub(stub);

    } else {
        RootedObjectGroup group(cx, val.toObject().group());

#ifdef DEBUG
        // We should not have a stub for this group.
        for (ICStubConstIterator iter(firstUpdateStub_); !iter.atEnd(); iter++) {
            MOZ_ASSERT_IF(iter->isTypeUpdate_ObjectGroup(),
                          iter->toTypeUpdate_ObjectGroup()->group() != group);
        }
#endif

        ICTypeUpdate_ObjectGroup::Compiler compiler(cx, group);
        ICStub* stub = compiler.getStub(compiler.getStubSpace(outerScript));
        if (!stub)
            return false;

        JitSpew(JitSpew_BaselineIC, "  Added TypeUpdate stub %p for ObjectGroup %p",
                stub, group.get());

        addOptimizedUpdateStub(stub);
    }

    return true;
}

//
// NewArray_Fallback
//

static bool
DoNewArray(JSContext* cx, void* payload, ICNewArray_Fallback* stub, uint32_t length,
           MutableHandleValue res)
{
    SharedStubInfo info(cx, payload, stub->icEntry());

    FallbackICSpew(cx, stub, "NewArray");

    RootedObject obj(cx);
    if (stub->templateObject()) {
        RootedObject templateObject(cx, stub->templateObject());
        obj = NewArrayOperationWithTemplate(cx, templateObject);
        if (!obj)
            return false;
    } else {
        HandleScript script = info.script();
        jsbytecode* pc = info.pc();
        obj = NewArrayOperation(cx, script, pc, length);
        if (!obj)
            return false;

        if (obj && !obj->isSingleton() && !obj->group()->maybePreliminaryObjects()) {
            JSObject* templateObject = NewArrayOperation(cx, script, pc, length, TenuredObject);
            if (!templateObject)
                return false;
            stub->setTemplateObject(templateObject);
        }
    }

    res.setObject(*obj);
    return true;
}

typedef bool(*DoNewArrayFn)(JSContext*, void*, ICNewArray_Fallback*, uint32_t,
                            MutableHandleValue);
static const VMFunction DoNewArrayInfo =
    FunctionInfo<DoNewArrayFn>(DoNewArray, "DoNewArray", TailCall);

bool
ICNewArray_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    masm.push(R0.scratchReg()); // length
    masm.push(ICStubReg); // stub.
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoNewArrayInfo, masm);
}

//
// NewObject_Fallback
//

// Unlike typical baseline IC stubs, the code for NewObject_WithTemplate is
// specialized for the template object being allocated.
static JitCode*
GenerateNewObjectWithTemplateCode(JSContext* cx, JSObject* templateObject)
{
    JitContext jctx(cx, nullptr);
    MacroAssembler masm;
#ifdef JS_CODEGEN_ARM
    masm.setSecondScratchReg(BaselineSecondScratchReg);
#endif

    Label failure;
    Register objReg = R0.scratchReg();
    Register tempReg = R1.scratchReg();
    masm.branchIfPretenuredGroup(templateObject->group(), tempReg, &failure);
    masm.branchPtr(Assembler::NotEqual, AbsoluteAddress(cx->compartment()->addressOfMetadataBuilder()),
                   ImmWord(0), &failure);
    masm.createGCObject(objReg, tempReg, templateObject, gc::DefaultHeap, &failure);
    masm.tagValue(JSVAL_TYPE_OBJECT, objReg, R0);

    EmitReturnFromIC(masm);
    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    Linker linker(masm);
    AutoFlushICache afc("GenerateNewObjectWithTemplateCode");
    return linker.newCode(cx, CodeKind::Baseline);
}

static bool
DoNewObject(JSContext* cx, void* payload, ICNewObject_Fallback* stub, MutableHandleValue res)
{
    SharedStubInfo info(cx, payload, stub->icEntry());

    FallbackICSpew(cx, stub, "NewObject");

    RootedObject obj(cx);

    RootedObject templateObject(cx, stub->templateObject());
    if (templateObject) {
        MOZ_ASSERT(!templateObject->group()->maybePreliminaryObjects());
        obj = NewObjectOperationWithTemplate(cx, templateObject);
    } else {
        HandleScript script = info.script();
        jsbytecode* pc = info.pc();
        obj = NewObjectOperation(cx, script, pc);

        if (obj && !obj->isSingleton() && !obj->group()->maybePreliminaryObjects()) {
            JSObject* templateObject = NewObjectOperation(cx, script, pc, TenuredObject);
            if (!templateObject)
                return false;

            if (!stub->invalid() &&
                (templateObject->is<UnboxedPlainObject>() ||
                 !templateObject->as<PlainObject>().hasDynamicSlots()))
            {
                JitCode* code = GenerateNewObjectWithTemplateCode(cx, templateObject);
                if (!code)
                    return false;

                ICStubSpace* space =
                    ICStubCompiler::StubSpaceForStub(/* makesGCCalls = */ false, script,
                                                     ICStubCompiler::Engine::Baseline);
                ICStub* templateStub = ICStub::New<ICNewObject_WithTemplate>(cx, space, code);
                if (!templateStub)
                    return false;

                stub->addNewStub(templateStub);
            }

            stub->setTemplateObject(templateObject);
        }
    }

    if (!obj)
        return false;

    res.setObject(*obj);
    return true;
}

typedef bool(*DoNewObjectFn)(JSContext*, void*, ICNewObject_Fallback*, MutableHandleValue);
static const VMFunction DoNewObjectInfo =
    FunctionInfo<DoNewObjectFn>(DoNewObject, "DoNewObject", TailCall);

bool
ICNewObject_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    EmitRestoreTailCallReg(masm);

    masm.push(ICStubReg); // stub.
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoNewObjectInfo, masm);
}

} // namespace jit
} // namespace js
