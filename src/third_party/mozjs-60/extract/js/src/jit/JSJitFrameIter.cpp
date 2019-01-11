/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JSJitFrameIter-inl.h"

#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineIC.h"
#include "jit/JitcodeMap.h"
#include "jit/JitFrames.h"

using namespace js;
using namespace js::jit;

JSJitFrameIter::JSJitFrameIter(const JitActivation* activation)
  : current_(activation->jsExitFP()),
    type_(JitFrame_Exit),
    returnAddressToFp_(nullptr),
    frameSize_(0),
    cachedSafepointIndex_(nullptr),
    activation_(activation)
{
    if (activation_->bailoutData()) {
        current_ = activation_->bailoutData()->fp();
        frameSize_ = activation_->bailoutData()->topFrameSize();
        type_ = JitFrame_Bailout;
    } else {
        MOZ_ASSERT(!TlsContext.get()->inUnsafeCallWithABI);
    }
}

JSJitFrameIter::JSJitFrameIter(const JitActivation* activation, uint8_t* fp)
  : current_(fp),
    type_(JitFrame_JSJitToWasm),
    returnAddressToFp_(nullptr),
    frameSize_(0),
    cachedSafepointIndex_(nullptr),
    activation_(activation)
{
    MOZ_ASSERT(!activation_->bailoutData());
    MOZ_ASSERT(!TlsContext.get()->inUnsafeCallWithABI);
}

bool
JSJitFrameIter::checkInvalidation() const
{
    IonScript* dummy;
    return checkInvalidation(&dummy);
}

bool
JSJitFrameIter::checkInvalidation(IonScript** ionScriptOut) const
{
    JSScript* script = this->script();
    if (isBailoutJS()) {
        *ionScriptOut = activation_->bailoutData()->ionScript();
        return !script->hasIonScript() || script->ionScript() != *ionScriptOut;
    }

    uint8_t* returnAddr = returnAddressToFp();
    // N.B. the current IonScript is not the same as the frame's
    // IonScript if the frame has since been invalidated.
    bool invalidated = !script->hasIonScript() ||
                       !script->ionScript()->containsReturnAddress(returnAddr);
    if (!invalidated)
        return false;

    int32_t invalidationDataOffset = ((int32_t*) returnAddr)[-1];
    uint8_t* ionScriptDataOffset = returnAddr + invalidationDataOffset;
    IonScript* ionScript = (IonScript*) Assembler::GetPointer(ionScriptDataOffset);
    MOZ_ASSERT(ionScript->containsReturnAddress(returnAddr));
    *ionScriptOut = ionScript;
    return true;
}

CalleeToken
JSJitFrameIter::calleeToken() const
{
    return ((JitFrameLayout*) current_)->calleeToken();
}

JSFunction*
JSJitFrameIter::callee() const
{
    MOZ_ASSERT(isScripted());
    MOZ_ASSERT(isFunctionFrame());
    return CalleeTokenToFunction(calleeToken());
}

JSFunction*
JSJitFrameIter::maybeCallee() const
{
    if (isScripted() && (isFunctionFrame()))
        return callee();
    return nullptr;
}

bool
JSJitFrameIter::isBareExit() const
{
    if (type_ != JitFrame_Exit)
        return false;
    return exitFrame()->isBareExit();
}

bool
JSJitFrameIter::isFunctionFrame() const
{
    return CalleeTokenIsFunction(calleeToken());
}

JSScript*
JSJitFrameIter::script() const
{
    MOZ_ASSERT(isScripted());
    if (isBaselineJS())
        return baselineFrame()->script();
    JSScript* script = ScriptFromCalleeToken(calleeToken());
    MOZ_ASSERT(script);
    return script;
}

void
JSJitFrameIter::baselineScriptAndPc(JSScript** scriptRes, jsbytecode** pcRes) const
{
    MOZ_ASSERT(isBaselineJS());
    JSScript* script = this->script();
    if (scriptRes)
        *scriptRes = script;

    MOZ_ASSERT(pcRes);

    // Use the frame's override pc, if we have one. This should only happen
    // when we're in FinishBailoutToBaseline, handling an exception or toggling
    // debug mode.
    if (jsbytecode* overridePc = baselineFrame()->maybeOverridePc()) {
        *pcRes = overridePc;
        return;
    }

    // Else, there must be an ICEntry for the current return address.
    uint8_t* retAddr = returnAddressToFp();
    ICEntry& icEntry = script->baselineScript()->icEntryFromReturnAddress(retAddr);
    *pcRes = icEntry.pc(script);
}

Value*
JSJitFrameIter::actualArgs() const
{
    return jsFrame()->argv() + 1;
}

uint8_t*
JSJitFrameIter::prevFp() const
{
    return current_ + current()->prevFrameLocalSize() + current()->headerSize();
}

void
JSJitFrameIter::operator++()
{
    MOZ_ASSERT(!isEntry());

    frameSize_ = prevFrameLocalSize();
    cachedSafepointIndex_ = nullptr;

    // If the next frame is the entry frame, just exit. Don't update current_,
    // since the entry and first frames overlap.
    if (isEntry(current()->prevType())) {
        type_ = current()->prevType();
        return;
    }

    type_ = current()->prevType();
    returnAddressToFp_ = current()->returnAddress();
    current_ = prevFp();
}

uintptr_t*
JSJitFrameIter::spillBase() const
{
    MOZ_ASSERT(isIonJS());

    // Get the base address to where safepoint registers are spilled.
    // Out-of-line calls do not unwind the extra padding space used to
    // aggregate bailout tables, so we use frameSize instead of frameLocals,
    // which would only account for local stack slots.
    return reinterpret_cast<uintptr_t*>(fp() - ionScript()->frameSize());
}

MachineState
JSJitFrameIter::machineState() const
{
    MOZ_ASSERT(isIonScripted());

    // The MachineState is used by GCs for tracing call-sites.
    if (MOZ_UNLIKELY(isBailoutJS()))
        return *activation_->bailoutData()->machineState();

    SafepointReader reader(ionScript(), safepoint());
    uintptr_t* spill = spillBase();
    MachineState machine;

    for (GeneralRegisterBackwardIterator iter(reader.allGprSpills()); iter.more(); ++iter)
        machine.setRegisterLocation(*iter, --spill);

    uint8_t* spillAlign = alignDoubleSpillWithOffset(reinterpret_cast<uint8_t*>(spill), 0);

    char* floatSpill = reinterpret_cast<char*>(spillAlign);
    FloatRegisterSet fregs = reader.allFloatSpills().set();
    fregs = fregs.reduceSetForPush();
    for (FloatRegisterBackwardIterator iter(fregs); iter.more(); ++iter) {
        floatSpill -= (*iter).size();
        for (uint32_t a = 0; a < (*iter).numAlignedAliased(); a++) {
            // Only say that registers that actually start here start here.
            // e.g. d0 should not start at s1, only at s0.
            FloatRegister ftmp;
            (*iter).alignedAliased(a, &ftmp);
            machine.setRegisterLocation(ftmp, (double*)floatSpill);
        }
    }

    return machine;
}

JitFrameLayout*
JSJitFrameIter::jsFrame() const
{
    MOZ_ASSERT(isScripted());
    if (isBailoutJS())
        return (JitFrameLayout*) activation_->bailoutData()->fp();

    return (JitFrameLayout*) fp();
}

IonScript*
JSJitFrameIter::ionScript() const
{
    MOZ_ASSERT(isIonScripted());
    if (isBailoutJS())
        return activation_->bailoutData()->ionScript();

    IonScript* ionScript = nullptr;
    if (checkInvalidation(&ionScript))
        return ionScript;
    return ionScriptFromCalleeToken();
}

IonScript*
JSJitFrameIter::ionScriptFromCalleeToken() const
{
    MOZ_ASSERT(isIonJS());
    MOZ_ASSERT(!checkInvalidation());
    return script()->ionScript();
}

const SafepointIndex*
JSJitFrameIter::safepoint() const
{
    MOZ_ASSERT(isIonJS());
    if (!cachedSafepointIndex_)
        cachedSafepointIndex_ = ionScript()->getSafepointIndex(returnAddressToFp());
    return cachedSafepointIndex_;
}

SnapshotOffset
JSJitFrameIter::snapshotOffset() const
{
    MOZ_ASSERT(isIonScripted());
    if (isBailoutJS())
        return activation_->bailoutData()->snapshotOffset();
    return osiIndex()->snapshotOffset();
}

const OsiIndex*
JSJitFrameIter::osiIndex() const
{
    MOZ_ASSERT(isIonJS());
    SafepointReader reader(ionScript(), safepoint());
    return ionScript()->getOsiIndex(reader.osiReturnPointOffset());
}

bool
JSJitFrameIter::isConstructing() const
{
    return CalleeTokenIsConstructing(calleeToken());
}

unsigned
JSJitFrameIter::numActualArgs() const
{
    if (isScripted())
        return jsFrame()->numActualArgs();

    MOZ_ASSERT(isExitFrameLayout<NativeExitFrameLayout>());
    return exitFrame()->as<NativeExitFrameLayout>()->argc();
}

void
JSJitFrameIter::dumpBaseline() const
{
    MOZ_ASSERT(isBaselineJS());

    fprintf(stderr, " JS Baseline frame\n");
    if (isFunctionFrame()) {
        fprintf(stderr, "  callee fun: ");
#ifdef DEBUG
        DumpObject(callee());
#else
        fprintf(stderr, "?\n");
#endif
    } else {
        fprintf(stderr, "  global frame, no callee\n");
    }

    fprintf(stderr, "  file %s line %zu\n",
            script()->filename(), script()->lineno());

    JSContext* cx = TlsContext.get();
    RootedScript script(cx);
    jsbytecode* pc;
    baselineScriptAndPc(script.address(), &pc);

    fprintf(stderr, "  script = %p, pc = %p (offset %u)\n", (void*)script, pc, uint32_t(script->pcToOffset(pc)));
    fprintf(stderr, "  current op: %s\n", CodeName[*pc]);

    fprintf(stderr, "  actual args: %d\n", numActualArgs());

    BaselineFrame* frame = baselineFrame();

    for (unsigned i = 0; i < frame->numValueSlots(); i++) {
        fprintf(stderr, "  slot %u: ", i);
#ifdef DEBUG
        Value* v = frame->valueSlot(i);
        DumpValue(*v);
#else
        fprintf(stderr, "?\n");
#endif
    }
}

void
JSJitFrameIter::dump() const
{
    switch (type_) {
      case JitFrame_CppToJSJit:
        fprintf(stderr, " Entry frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_BaselineJS:
        dumpBaseline();
        break;
      case JitFrame_BaselineStub:
        fprintf(stderr, " Baseline stub frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_Bailout:
      case JitFrame_IonJS:
      {
        InlineFrameIterator frames(TlsContext.get(), this);
        for (;;) {
            frames.dump();
            if (!frames.more())
                break;
            ++frames;
        }
        break;
      }
      case JitFrame_Rectifier:
        fprintf(stderr, " Rectifier frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_IonICCall:
        fprintf(stderr, " Ion IC call\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_WasmToJSJit:
        fprintf(stderr, " Fast wasm-to-JS entry frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_Exit:
        fprintf(stderr, " Exit frame\n");
        break;
      case JitFrame_JSJitToWasm:
        fprintf(stderr, " Wasm exit frame\n");
        break;
    };
    fputc('\n', stderr);
}

#ifdef DEBUG
bool
JSJitFrameIter::verifyReturnAddressUsingNativeToBytecodeMap()
{
    MOZ_ASSERT(returnAddressToFp_ != nullptr);

    // Only handle Ion frames for now.
    if (type_ != JitFrame_IonJS && type_ != JitFrame_BaselineJS)
        return true;

    JSRuntime* rt = TlsContext.get()->runtime();

    // Don't verify while off thread.
    if (!CurrentThreadCanAccessRuntime(rt))
        return true;

    // Don't verify if sampling is being suppressed.
    if (!TlsContext.get()->isProfilerSamplingEnabled())
        return true;

    if (JS::CurrentThreadIsHeapMinorCollecting())
        return true;

    JitRuntime* jitrt = rt->jitRuntime();

    // Look up and print bytecode info for the native address.
    const JitcodeGlobalEntry* entry = jitrt->getJitcodeGlobalTable()->lookup(returnAddressToFp_);
    if (!entry)
        return true;

    JitSpew(JitSpew_Profiling, "Found nativeToBytecode entry for %p: %p - %p",
            returnAddressToFp_, entry->nativeStartAddr(), entry->nativeEndAddr());

    JitcodeGlobalEntry::BytecodeLocationVector location;
    uint32_t depth = UINT32_MAX;
    if (!entry->callStackAtAddr(rt, returnAddressToFp_, location, &depth))
        return false;
    MOZ_ASSERT(depth > 0 && depth != UINT32_MAX);
    MOZ_ASSERT(location.length() == depth);

    JitSpew(JitSpew_Profiling, "Found bytecode location of depth %d:", depth);
    for (size_t i = 0; i < location.length(); i++) {
        JitSpew(JitSpew_Profiling, "   %s:%zu - %zu",
                location[i].script->filename(), location[i].script->lineno(),
                size_t(location[i].pc - location[i].script->code()));
    }

    if (type_ == JitFrame_IonJS) {
        // Create an InlineFrameIterator here and verify the mapped info against the iterator info.
        InlineFrameIterator inlineFrames(TlsContext.get(), this);
        for (size_t idx = 0; idx < location.length(); idx++) {
            MOZ_ASSERT(idx < location.length());
            MOZ_ASSERT_IF(idx < location.length() - 1, inlineFrames.more());

            JitSpew(JitSpew_Profiling,
                    "Match %d: ION %s:%zu(%zu) vs N2B %s:%zu(%zu)",
                    (int)idx,
                    inlineFrames.script()->filename(),
                    inlineFrames.script()->lineno(),
                    size_t(inlineFrames.pc() - inlineFrames.script()->code()),
                    location[idx].script->filename(),
                    location[idx].script->lineno(),
                    size_t(location[idx].pc - location[idx].script->code()));

            MOZ_ASSERT(inlineFrames.script() == location[idx].script);

            if (inlineFrames.more())
                ++inlineFrames;
        }
    }

    return true;
}
#endif // DEBUG

JSJitProfilingFrameIterator::JSJitProfilingFrameIterator(JSContext* cx, void* pc)
{
    // If no profilingActivation is live, initialize directly to
    // end-of-iteration state.
    if (!cx->profilingActivation()) {
        type_ = JitFrame_CppToJSJit;
        fp_ = nullptr;
        returnAddressToFp_ = nullptr;
        return;
    }

    MOZ_ASSERT(cx->profilingActivation()->isJit());

    JitActivation* act = cx->profilingActivation()->asJit();

    // If the top JitActivation has a null lastProfilingFrame, assume that
    // it's a trivially empty activation, and initialize directly
    // to end-of-iteration state.
    if (!act->lastProfilingFrame()) {
        type_ = JitFrame_CppToJSJit;
        fp_ = nullptr;
        returnAddressToFp_ = nullptr;
        return;
    }

    // Get the fp from the current profilingActivation
    fp_ = (uint8_t*) act->lastProfilingFrame();

    // Profiler sampling must NOT be suppressed if we are here.
    MOZ_ASSERT(cx->isProfilerSamplingEnabled());

    // Try initializing with sampler pc
    if (tryInitWithPC(pc))
        return;

    // Try initializing with sampler pc using native=>bytecode table.
    JitcodeGlobalTable* table = cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (tryInitWithTable(table, pc, /* forLastCallSite = */ false))
        return;

    // Try initializing with lastProfilingCallSite pc
    void* lastCallSite = act->lastProfilingCallSite();
    if (lastCallSite) {
        if (tryInitWithPC(lastCallSite))
            return;

        // Try initializing with lastProfilingCallSite pc using native=>bytecode table.
        if (tryInitWithTable(table, lastCallSite, /* forLastCallSite = */ true))
            return;
    }

    MOZ_ASSERT(frameScript()->hasBaselineScript());

    // If nothing matches, for now just assume we are at the start of the last frame's
    // baseline jit code.
    type_ = JitFrame_BaselineJS;
    returnAddressToFp_ = frameScript()->baselineScript()->method()->raw();
}

template <typename ReturnType = CommonFrameLayout*>
static inline ReturnType
GetPreviousRawFrame(CommonFrameLayout* frame)
{
    size_t prevSize = frame->prevFrameLocalSize() + frame->headerSize();
    return ReturnType((uint8_t*)frame + prevSize);
}

JSJitProfilingFrameIterator::JSJitProfilingFrameIterator(CommonFrameLayout* fp)
{
    moveToNextFrame(fp);
}

bool
JSJitProfilingFrameIterator::tryInitWithPC(void* pc)
{
    JSScript* callee = frameScript();

    // Check for Ion first, since it's more likely for hot code.
    if (callee->hasIonScript() && callee->ionScript()->method()->containsNativePC(pc)) {
        type_ = JitFrame_IonJS;
        returnAddressToFp_ = pc;
        return true;
    }

    // Check for containment in Baseline jitcode second.
    if (callee->hasBaselineScript() && callee->baselineScript()->method()->containsNativePC(pc)) {
        type_ = JitFrame_BaselineJS;
        returnAddressToFp_ = pc;
        return true;
    }

    return false;
}

bool
JSJitProfilingFrameIterator::tryInitWithTable(JitcodeGlobalTable* table, void* pc,
                                            bool forLastCallSite)
{
    if (!pc)
        return false;

    const JitcodeGlobalEntry* entry = table->lookup(pc);
    if (!entry)
        return false;

    JSScript* callee = frameScript();

    MOZ_ASSERT(entry->isIon() || entry->isBaseline() || entry->isIonCache() || entry->isDummy());

    // Treat dummy lookups as an empty frame sequence.
    if (entry->isDummy()) {
        type_ = JitFrame_CppToJSJit;
        fp_ = nullptr;
        returnAddressToFp_ = nullptr;
        return true;
    }

    if (entry->isIon()) {
        // If looked-up callee doesn't match frame callee, don't accept lastProfilingCallSite
        if (entry->ionEntry().getScript(0) != callee)
            return false;

        type_ = JitFrame_IonJS;
        returnAddressToFp_ = pc;
        return true;
    }

    if (entry->isBaseline()) {
        // If looked-up callee doesn't match frame callee, don't accept lastProfilingCallSite
        if (forLastCallSite && entry->baselineEntry().script() != callee)
            return false;

        type_ = JitFrame_BaselineJS;
        returnAddressToFp_ = pc;
        return true;
    }

    if (entry->isIonCache()) {
        void* ptr = entry->ionCacheEntry().rejoinAddr();
        const JitcodeGlobalEntry& ionEntry = table->lookupInfallible(ptr);
        MOZ_ASSERT(ionEntry.isIon());

        if (ionEntry.ionEntry().getScript(0) != callee)
            return false;

        type_ = JitFrame_IonJS;
        returnAddressToFp_ = pc;
        return true;
    }

    return false;
}

void
JSJitProfilingFrameIterator::fixBaselineReturnAddress()
{
    MOZ_ASSERT(type_ == JitFrame_BaselineJS);
    BaselineFrame* bl = (BaselineFrame*)(fp_ - BaselineFrame::FramePointerOffset -
                                         BaselineFrame::Size());

    // Debug mode OSR for Baseline uses a "continuation fixer" and stashes the
    // actual return address in an auxiliary structure.
    if (BaselineDebugModeOSRInfo* info = bl->getDebugModeOSRInfo()) {
        returnAddressToFp_ = info->resumeAddr;
        return;
    }

    // Resuming a generator via .throw() pushes a bogus return address onto
    // the stack. We have the actual jsbytecode* stashed on the frame itself;
    // translate that into the Baseline code address.
    if (jsbytecode* override = bl->maybeOverridePc()) {
        JSScript* script = bl->script();
        returnAddressToFp_ = script->baselineScript()->nativeCodeForPC(script, override);
        return;
    }
}

void
JSJitProfilingFrameIterator::operator++()
{
    JitFrameLayout* frame = framePtr();
    moveToNextFrame(frame);
}

void
JSJitProfilingFrameIterator::moveToWasmFrame(CommonFrameLayout* frame)
{
    // No previous js jit frame, this is a transition frame, used to
    // pass a wasm iterator the correct value of FP.
    returnAddressToFp_ = nullptr;
    fp_ = GetPreviousRawFrame<uint8_t*>(frame);
    type_ = JitFrame_WasmToJSJit;
    MOZ_ASSERT(!done());
}

void
JSJitProfilingFrameIterator::moveToCppEntryFrame()
{
    // No previous frame, set to nullptr to indicate that
    // JSJitProfilingFrameIterator is done().
    returnAddressToFp_ = nullptr;
    fp_ = nullptr;
    type_ = JitFrame_CppToJSJit;
}

void
JSJitProfilingFrameIterator::moveToNextFrame(CommonFrameLayout* frame)
{
    /*
     * fp_ points to a Baseline or Ion frame.  The possible call-stacks
     * patterns occurring between this frame and a previous Ion or Baseline
     * frame are as follows:
     *
     * <Baseline-Or-Ion>
     * ^
     * |
     * ^--- Ion
     * |
     * ^--- Baseline Stub <---- Baseline
     * |
     * ^--- WasmToJSJit <---- (other wasm frames, not handled by this iterator)
     * |
     * ^--- Argument Rectifier
     * |    ^
     * |    |
     * |    ^--- Ion
     * |    |
     * |    ^--- Baseline Stub <---- Baseline
     * |    |
     * |    ^--- WasmToJSJit <--- (other wasm frames)
     * |    |
     * |    ^--- CppToJSJit
     * |
     * ^--- Entry Frame (From C++)
     *      Exit Frame (From previous JitActivation)
     *      ^
     *      |
     *      ^--- Ion
     *      |
     *      ^--- Baseline
     *      |
     *      ^--- Baseline Stub <---- Baseline
     */
    FrameType prevType = frame->prevType();

    if (prevType == JitFrame_IonJS) {
        returnAddressToFp_ = frame->returnAddress();
        fp_ = GetPreviousRawFrame<uint8_t*>(frame);
        type_ = JitFrame_IonJS;
        return;
    }

    if (prevType == JitFrame_BaselineJS) {
        returnAddressToFp_ = frame->returnAddress();
        fp_ = GetPreviousRawFrame<uint8_t*>(frame);
        type_ = JitFrame_BaselineJS;
        fixBaselineReturnAddress();
        return;
    }

    if (prevType == JitFrame_BaselineStub) {
        BaselineStubFrameLayout* stubFrame = GetPreviousRawFrame<BaselineStubFrameLayout*>(frame);
        MOZ_ASSERT(stubFrame->prevType() == JitFrame_BaselineJS);

        returnAddressToFp_ = stubFrame->returnAddress();
        fp_ = ((uint8_t*) stubFrame->reverseSavedFramePtr())
                + jit::BaselineFrame::FramePointerOffset;
        type_ = JitFrame_BaselineJS;
        return;
    }

    if (prevType == JitFrame_Rectifier) {
        RectifierFrameLayout* rectFrame = GetPreviousRawFrame<RectifierFrameLayout*>(frame);
        FrameType rectPrevType = rectFrame->prevType();

        if (rectPrevType == JitFrame_IonJS) {
            returnAddressToFp_ = rectFrame->returnAddress();
            fp_ = GetPreviousRawFrame<uint8_t*>(rectFrame);
            type_ = JitFrame_IonJS;
            return;
        }

        if (rectPrevType == JitFrame_BaselineStub) {
            BaselineStubFrameLayout* stubFrame =
                GetPreviousRawFrame<BaselineStubFrameLayout*>(rectFrame);
            returnAddressToFp_ = stubFrame->returnAddress();
            fp_ = ((uint8_t*) stubFrame->reverseSavedFramePtr())
                    + jit::BaselineFrame::FramePointerOffset;
            type_ = JitFrame_BaselineJS;
            return;
        }

        if (rectPrevType == JitFrame_WasmToJSJit) {
            moveToWasmFrame(rectFrame);
            return;
        }

        if (rectPrevType == JitFrame_CppToJSJit) {
            moveToCppEntryFrame();
            return;
        }

        MOZ_CRASH("Bad frame type prior to rectifier frame.");
    }

    if (prevType == JitFrame_IonICCall) {
        IonICCallFrameLayout* callFrame =
            GetPreviousRawFrame<IonICCallFrameLayout*>(frame);

        MOZ_ASSERT(callFrame->prevType() == JitFrame_IonJS);

        returnAddressToFp_ = callFrame->returnAddress();
        fp_ = GetPreviousRawFrame<uint8_t*>(callFrame);
        type_ = JitFrame_IonJS;
        return;
    }

    if (prevType == JitFrame_WasmToJSJit) {
        moveToWasmFrame(frame);
        return;
    }

    if (prevType == JitFrame_CppToJSJit) {
        moveToCppEntryFrame();
        return;
    }

    MOZ_CRASH("Bad frame type.");
}
