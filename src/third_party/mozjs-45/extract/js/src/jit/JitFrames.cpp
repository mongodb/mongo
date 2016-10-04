/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JitFrames-inl.h"

#include "mozilla/SizePrintfMacros.h"

#include "jsfun.h"
#include "jsobj.h"
#include "jsscript.h"
#include "jsutil.h"

#include "gc/Marking.h"
#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "jit/JitcodeMap.h"
#include "jit/JitCompartment.h"
#include "jit/JitSpewer.h"
#include "jit/MacroAssembler.h"
#include "jit/PcScriptCache.h"
#include "jit/Recover.h"
#include "jit/Safepoints.h"
#include "jit/Snapshots.h"
#include "jit/VMFunctions.h"
#include "vm/ArgumentsObject.h"
#include "vm/Debugger.h"
#include "vm/Interpreter.h"
#include "vm/SPSProfiler.h"
#include "vm/TraceLogging.h"
#include "vm/TypeInference.h"

#include "jsscriptinlines.h"
#include "gc/Nursery-inl.h"
#include "jit/JitFrameIterator-inl.h"
#include "vm/Debugger-inl.h"
#include "vm/Probes-inl.h"
#include "vm/TypeInference-inl.h"

namespace js {
namespace jit {

// Given a slot index, returns the offset, in bytes, of that slot from an
// JitFrameLayout. Slot distances are uniform across architectures, however,
// the distance does depend on the size of the frame header.
static inline int32_t
OffsetOfFrameSlot(int32_t slot)
{
    return -slot;
}

static inline uint8_t*
AddressOfFrameSlot(JitFrameLayout* fp, int32_t slot)
{
    return (uint8_t*) fp + OffsetOfFrameSlot(slot);
}

static inline uintptr_t
ReadFrameSlot(JitFrameLayout* fp, int32_t slot)
{
    return *(uintptr_t*) AddressOfFrameSlot(fp, slot);
}

static inline void
WriteFrameSlot(JitFrameLayout* fp, int32_t slot, uintptr_t value)
{
    *(uintptr_t*) AddressOfFrameSlot(fp, slot) = value;
}

static inline double
ReadFrameDoubleSlot(JitFrameLayout* fp, int32_t slot)
{
    return *(double*) AddressOfFrameSlot(fp, slot);
}

static inline float
ReadFrameFloat32Slot(JitFrameLayout* fp, int32_t slot)
{
    return *(float*) AddressOfFrameSlot(fp, slot);
}

static inline int32_t
ReadFrameInt32Slot(JitFrameLayout* fp, int32_t slot)
{
    return *(int32_t*) AddressOfFrameSlot(fp, slot);
}

static inline bool
ReadFrameBooleanSlot(JitFrameLayout* fp, int32_t slot)
{
    return *(bool*) AddressOfFrameSlot(fp, slot);
}

JitFrameIterator::JitFrameIterator()
  : current_(nullptr),
    type_(JitFrame_Exit),
    returnAddressToFp_(nullptr),
    frameSize_(0),
    cachedSafepointIndex_(nullptr),
    activation_(nullptr)
{
}

JitFrameIterator::JitFrameIterator(JSContext* cx)
  : current_(cx->runtime()->jitTop),
    type_(JitFrame_Exit),
    returnAddressToFp_(nullptr),
    frameSize_(0),
    cachedSafepointIndex_(nullptr),
    activation_(cx->runtime()->activation()->asJit())
{
    if (activation_->bailoutData()) {
        current_ = activation_->bailoutData()->fp();
        frameSize_ = activation_->bailoutData()->topFrameSize();
        type_ = JitFrame_Bailout;
    } else if (activation_->isLazyLinkExitFrame()) {
        type_ = JitFrame_LazyLink;
        MOZ_ASSERT(isExitFrameLayout<LazyLinkExitFrameLayout>());
    }
}

JitFrameIterator::JitFrameIterator(const ActivationIterator& activations)
  : current_(activations.jitTop()),
    type_(JitFrame_Exit),
    returnAddressToFp_(nullptr),
    frameSize_(0),
    cachedSafepointIndex_(nullptr),
    activation_(activations->asJit())
{
    if (activation_->bailoutData()) {
        current_ = activation_->bailoutData()->fp();
        frameSize_ = activation_->bailoutData()->topFrameSize();
        type_ = JitFrame_Bailout;
    } else if (activation_->isLazyLinkExitFrame()) {
        type_ = JitFrame_LazyLink;
        MOZ_ASSERT(isExitFrameLayout<LazyLinkExitFrameLayout>());
    }
}

bool
JitFrameIterator::checkInvalidation() const
{
    IonScript* dummy;
    return checkInvalidation(&dummy);
}

bool
JitFrameIterator::checkInvalidation(IonScript** ionScriptOut) const
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
JitFrameIterator::calleeToken() const
{
    return ((JitFrameLayout*) current_)->calleeToken();
}

JSFunction*
JitFrameIterator::callee() const
{
    MOZ_ASSERT(isScripted());
    MOZ_ASSERT(isFunctionFrame());
    return CalleeTokenToFunction(calleeToken());
}

JSFunction*
JitFrameIterator::maybeCallee() const
{
    if (isScripted() && (isFunctionFrame()))
        return callee();
    return nullptr;
}

bool
JitFrameIterator::isBareExit() const
{
    if (type_ != JitFrame_Exit)
        return false;
    return exitFrame()->isBareExit();
}

bool
JitFrameIterator::isFunctionFrame() const
{
    return CalleeTokenIsFunction(calleeToken());
}

JSScript*
JitFrameIterator::script() const
{
    MOZ_ASSERT(isScripted());
    if (isBaselineJS())
        return baselineFrame()->script();
    JSScript* script = ScriptFromCalleeToken(calleeToken());
    MOZ_ASSERT(script);
    return script;
}

void
JitFrameIterator::baselineScriptAndPc(JSScript** scriptRes, jsbytecode** pcRes) const
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
JitFrameIterator::actualArgs() const
{
    return jsFrame()->argv() + 1;
}

static inline size_t
SizeOfFramePrefix(FrameType type)
{
    switch (type) {
      case JitFrame_Entry:
        return EntryFrameLayout::Size();
      case JitFrame_BaselineJS:
      case JitFrame_IonJS:
      case JitFrame_Bailout:
      case JitFrame_Unwound_BaselineJS:
      case JitFrame_Unwound_IonJS:
        return JitFrameLayout::Size();
      case JitFrame_BaselineStub:
      case JitFrame_Unwound_BaselineStub:
        return BaselineStubFrameLayout::Size();
      case JitFrame_IonStub:
      case JitFrame_Unwound_IonStub:
        return JitStubFrameLayout::Size();
      case JitFrame_Rectifier:
        return RectifierFrameLayout::Size();
      case JitFrame_Unwound_Rectifier:
        return IonUnwoundRectifierFrameLayout::Size();
      case JitFrame_Exit:
      case JitFrame_LazyLink:
        return ExitFrameLayout::Size();
      case JitFrame_IonAccessorIC:
      case JitFrame_Unwound_IonAccessorIC:
        return IonAccessorICFrameLayout::Size();
    }

    MOZ_CRASH("unknown frame type");
}

uint8_t*
JitFrameIterator::prevFp() const
{
    size_t currentSize = SizeOfFramePrefix(type_);
    // This quick fix must be removed as soon as bug 717297 land.  This is
    // needed because the descriptor size of JS-to-JS frame which is just after
    // a Rectifier frame should not change. (cf EnsureExitFrame function)
    if (isFakeExitFrame()) {
        MOZ_ASSERT(SizeOfFramePrefix(JitFrame_BaselineJS) ==
                   SizeOfFramePrefix(JitFrame_IonJS));
        currentSize = SizeOfFramePrefix(JitFrame_IonJS);
    }
    currentSize += current()->prevFrameLocalSize();
    return current_ + currentSize;
}

JitFrameIterator&
JitFrameIterator::operator++()
{
    MOZ_ASSERT(type_ != JitFrame_Entry);

    frameSize_ = prevFrameLocalSize();
    cachedSafepointIndex_ = nullptr;

    // If the next frame is the entry frame, just exit. Don't update current_,
    // since the entry and first frames overlap.
    if (current()->prevType() == JitFrame_Entry) {
        type_ = JitFrame_Entry;
        return *this;
    }

    // Note: prevFp() needs the current type, so set it after computing the
    // next frame.
    uint8_t* prev = prevFp();
    type_ = current()->prevType();
    if (type_ == JitFrame_Unwound_IonJS)
        type_ = JitFrame_IonJS;
    else if (type_ == JitFrame_Unwound_IonStub)
        type_ = JitFrame_IonStub;
    else if (type_ == JitFrame_Unwound_BaselineJS)
        type_ = JitFrame_BaselineJS;
    else if (type_ == JitFrame_Unwound_BaselineStub)
        type_ = JitFrame_BaselineStub;
    else if (type_ == JitFrame_Unwound_IonAccessorIC)
        type_ = JitFrame_IonAccessorIC;
    returnAddressToFp_ = current()->returnAddress();
    current_ = prev;

    return *this;
}

uintptr_t*
JitFrameIterator::spillBase() const
{
    MOZ_ASSERT(isIonJS());

    // Get the base address to where safepoint registers are spilled.
    // Out-of-line calls do not unwind the extra padding space used to
    // aggregate bailout tables, so we use frameSize instead of frameLocals,
    // which would only account for local stack slots.
    return reinterpret_cast<uintptr_t*>(fp() - ionScript()->frameSize());
}

MachineState
JitFrameIterator::machineState() const
{
    MOZ_ASSERT(isIonScripted());

    // The MachineState is used by GCs for marking call-sites.
    if (MOZ_UNLIKELY(isBailoutJS()))
        return *activation_->bailoutData()->machineState();

    SafepointReader reader(ionScript(), safepoint());
    uintptr_t* spill = spillBase();
    MachineState machine;

    for (GeneralRegisterBackwardIterator iter(reader.allGprSpills()); iter.more(); iter++)
        machine.setRegisterLocation(*iter, --spill);

    uint8_t* spillAlign = alignDoubleSpillWithOffset(reinterpret_cast<uint8_t*>(spill), 0);

    char* floatSpill = reinterpret_cast<char*>(spillAlign);
    FloatRegisterSet fregs = reader.allFloatSpills().set();
    fregs = fregs.reduceSetForPush();
    for (FloatRegisterBackwardIterator iter(fregs); iter.more(); iter++) {
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

static uint32_t
NumArgAndLocalSlots(const InlineFrameIterator& frame)
{
    JSScript* script = frame.script();
    return CountArgSlots(script, frame.maybeCalleeTemplate()) + script->nfixed();
}

static void
CloseLiveIteratorIon(JSContext* cx, const InlineFrameIterator& frame, uint32_t stackSlot)
{
    SnapshotIterator si = frame.snapshotIterator();

    // Skip stack slots until we reach the iterator object.
    uint32_t skipSlots = NumArgAndLocalSlots(frame) + stackSlot - 1;

    for (unsigned i = 0; i < skipSlots; i++)
        si.skip();

    Value v = si.read();
    RootedObject obj(cx, &v.toObject());

    if (cx->isExceptionPending())
        UnwindIteratorForException(cx, obj);
    else
        UnwindIteratorForUncatchableException(cx, obj);
}

class IonFrameStackDepthOp
{
    uint32_t depth_;

  public:
    explicit IonFrameStackDepthOp(const InlineFrameIterator& frame) {
        uint32_t base = NumArgAndLocalSlots(frame);
        SnapshotIterator si = frame.snapshotIterator();
        MOZ_ASSERT(si.numAllocations() >= base);
        depth_ = si.numAllocations() - base;
    }

    uint32_t operator()() { return depth_; }
};

class TryNoteIterIon : public TryNoteIter<IonFrameStackDepthOp>
{
  public:
    TryNoteIterIon(JSContext* cx, const InlineFrameIterator& frame)
      : TryNoteIter(cx, frame.script(), frame.pc(), IonFrameStackDepthOp(frame))
    { }
};

static void
HandleExceptionIon(JSContext* cx, const InlineFrameIterator& frame, ResumeFromException* rfe,
                   bool* overrecursed)
{
    if (cx->compartment()->isDebuggee()) {
        // We need to bail when there is a catchable exception, and we are the
        // debuggee of a Debugger with a live onExceptionUnwind hook, or if a
        // Debugger has observed this frame (e.g., for onPop).
        bool shouldBail = Debugger::hasLiveHook(cx->global(), Debugger::OnExceptionUnwind);
        RematerializedFrame* rematFrame = nullptr;
        if (!shouldBail) {
            JitActivation* act = cx->runtime()->activation()->asJit();
            rematFrame = act->lookupRematerializedFrame(frame.frame().fp(), frame.frameNo());
            shouldBail = rematFrame && rematFrame->isDebuggee();
        }

        if (shouldBail) {
            // If we have an exception from within Ion and the debugger is active,
            // we do the following:
            //
            //   1. Bailout to baseline to reconstruct a baseline frame.
            //   2. Resume immediately into the exception tail afterwards, and
            //      handle the exception again with the top frame now a baseline
            //      frame.
            //
            // An empty exception info denotes that we're propagating an Ion
            // exception due to debug mode, which BailoutIonToBaseline needs to
            // know. This is because we might not be able to fully reconstruct up
            // to the stack depth at the snapshot, as we could've thrown in the
            // middle of a call.
            ExceptionBailoutInfo propagateInfo;
            uint32_t retval = ExceptionHandlerBailout(cx, frame, rfe, propagateInfo, overrecursed);
            if (retval == BAILOUT_RETURN_OK)
                return;
        }

        MOZ_ASSERT_IF(rematFrame, !Debugger::inFrameMaps(rematFrame));
    }

    RootedScript script(cx, frame.script());
    if (!script->hasTrynotes())
        return;

    for (TryNoteIterIon tni(cx, frame); !tni.done(); ++tni) {
        JSTryNote* tn = *tni;

        switch (tn->kind) {
          case JSTRY_FOR_IN: {
            MOZ_ASSERT(JSOp(*(script->main() + tn->start + tn->length)) == JSOP_ENDITER);
            MOZ_ASSERT(tn->stackDepth > 0);

            uint32_t localSlot = tn->stackDepth;
            CloseLiveIteratorIon(cx, frame, localSlot);
            break;
          }

          case JSTRY_FOR_OF:
          case JSTRY_LOOP:
            break;

          case JSTRY_CATCH:
            if (cx->isExceptionPending()) {
                // Ion can compile try-catch, but bailing out to catch
                // exceptions is slow. Reset the warm-up counter so that if we
                // catch many exceptions we won't Ion-compile the script.
                script->resetWarmUpCounter();

                // Bailout at the start of the catch block.
                jsbytecode* catchPC = script->main() + tn->start + tn->length;
                ExceptionBailoutInfo excInfo(frame.frameNo(), catchPC, tn->stackDepth);
                uint32_t retval = ExceptionHandlerBailout(cx, frame, rfe, excInfo, overrecursed);
                if (retval == BAILOUT_RETURN_OK)
                    return;

                // Error on bailout clears pending exception.
                MOZ_ASSERT(!cx->isExceptionPending());
            }
            break;

          default:
            MOZ_CRASH("Unexpected try note");
        }
    }
}

static void
OnLeaveBaselineFrame(JSContext* cx, const JitFrameIterator& frame, jsbytecode* pc,
                     ResumeFromException* rfe, bool frameOk)
{
    BaselineFrame* baselineFrame = frame.baselineFrame();
    if (jit::DebugEpilogue(cx, baselineFrame, pc, frameOk)) {
        rfe->kind = ResumeFromException::RESUME_FORCED_RETURN;
        rfe->framePointer = frame.fp() - BaselineFrame::FramePointerOffset;
        rfe->stackPointer = reinterpret_cast<uint8_t*>(baselineFrame);
    }
}

static inline void
ForcedReturn(JSContext* cx, const JitFrameIterator& frame, jsbytecode* pc,
             ResumeFromException* rfe)
{
    OnLeaveBaselineFrame(cx, frame, pc, rfe, true);
}

static inline void
BaselineFrameAndStackPointersFromTryNote(JSTryNote* tn, const JitFrameIterator& frame,
                                         uint8_t** framePointer, uint8_t** stackPointer)
{
    JSScript* script = frame.baselineFrame()->script();
    *framePointer = frame.fp() - BaselineFrame::FramePointerOffset;
    *stackPointer = *framePointer - BaselineFrame::Size() -
                    (script->nfixed() + tn->stackDepth) * sizeof(Value);
}

static void
SettleOnTryNote(JSContext* cx, JSTryNote* tn, const JitFrameIterator& frame,
                ScopeIter& si, ResumeFromException* rfe, jsbytecode** pc)
{
    RootedScript script(cx, frame.baselineFrame()->script());

    // Unwind scope chain (pop block objects).
    if (cx->isExceptionPending())
        UnwindScope(cx, si, UnwindScopeToTryPc(script, tn));

    // Compute base pointer and stack pointer.
    BaselineFrameAndStackPointersFromTryNote(tn, frame, &rfe->framePointer, &rfe->stackPointer);

    // Compute the pc.
    *pc = script->main() + tn->start + tn->length;
}

struct AutoBaselineHandlingException
{
    BaselineFrame* frame;
    AutoBaselineHandlingException(BaselineFrame* frame, jsbytecode* pc)
      : frame(frame)
    {
        frame->setIsHandlingException();
        frame->setOverridePc(pc);
    }
    ~AutoBaselineHandlingException() {
        frame->unsetIsHandlingException();
        frame->clearOverridePc();
    }
};

class BaselineFrameStackDepthOp
{
    BaselineFrame* frame_;
  public:
    explicit BaselineFrameStackDepthOp(BaselineFrame* frame)
      : frame_(frame)
    { }
    uint32_t operator()() {
        MOZ_ASSERT(frame_->numValueSlots() >= frame_->script()->nfixed());
        return frame_->numValueSlots() - frame_->script()->nfixed();
    }
};

class TryNoteIterBaseline : public TryNoteIter<BaselineFrameStackDepthOp>
{
  public:
    TryNoteIterBaseline(JSContext* cx, BaselineFrame* frame, jsbytecode* pc)
      : TryNoteIter(cx, frame->script(), pc, BaselineFrameStackDepthOp(frame))
    { }
};

// Close all live iterators on a BaselineFrame due to exception unwinding. The
// pc parameter is updated to where the scopes have been unwound to.
static void
CloseLiveIteratorsBaselineForUncatchableException(JSContext* cx, const JitFrameIterator& frame,
                                                  jsbytecode* pc)
{
    for (TryNoteIterBaseline tni(cx, frame.baselineFrame(), pc); !tni.done(); ++tni) {
        JSTryNote* tn = *tni;

        if (tn->kind == JSTRY_FOR_IN) {
            uint8_t* framePointer;
            uint8_t* stackPointer;
            BaselineFrameAndStackPointersFromTryNote(tn, frame, &framePointer, &stackPointer);
            Value iterValue(*(Value*) stackPointer);
            RootedObject iterObject(cx, &iterValue.toObject());
            UnwindIteratorForUncatchableException(cx, iterObject);
        }
    }
}

static bool
ProcessTryNotesBaseline(JSContext* cx, const JitFrameIterator& frame, ScopeIter& si,
                        ResumeFromException* rfe, jsbytecode** pc)
{
    RootedScript script(cx, frame.baselineFrame()->script());

    for (TryNoteIterBaseline tni(cx, frame.baselineFrame(), *pc); !tni.done(); ++tni) {
        JSTryNote* tn = *tni;

        MOZ_ASSERT(cx->isExceptionPending());
        switch (tn->kind) {
          case JSTRY_CATCH: {
            // If we're closing a legacy generator, we have to skip catch
            // blocks.
            if (cx->isClosingGenerator())
                continue;

            SettleOnTryNote(cx, tn, frame, si, rfe, pc);

            // Ion can compile try-catch, but bailing out to catch
            // exceptions is slow. Reset the warm-up counter so that if we
            // catch many exceptions we won't Ion-compile the script.
            script->resetWarmUpCounter();

            // Resume at the start of the catch block.
            rfe->kind = ResumeFromException::RESUME_CATCH;
            rfe->target = script->baselineScript()->nativeCodeForPC(script, *pc);
            return true;
          }

          case JSTRY_FINALLY: {
            SettleOnTryNote(cx, tn, frame, si, rfe, pc);
            rfe->kind = ResumeFromException::RESUME_FINALLY;
            rfe->target = script->baselineScript()->nativeCodeForPC(script, *pc);
            // Drop the exception instead of leaking cross compartment data.
            if (!cx->getPendingException(MutableHandleValue::fromMarkedLocation(&rfe->exception)))
                rfe->exception = UndefinedValue();
            cx->clearPendingException();
            return true;
          }

          case JSTRY_FOR_IN: {
            uint8_t* framePointer;
            uint8_t* stackPointer;
            BaselineFrameAndStackPointersFromTryNote(tn, frame, &framePointer, &stackPointer);
            Value iterValue(*(Value*) stackPointer);
            RootedObject iterObject(cx, &iterValue.toObject());
            if (!UnwindIteratorForException(cx, iterObject)) {
                // See comment in the JSTRY_FOR_IN case in Interpreter.cpp's
                // ProcessTryNotes.
                SettleOnTryNote(cx, tn, frame, si, rfe, pc);
                MOZ_ASSERT(**pc == JSOP_ENDITER);
                return false;
            }
            break;
          }

          case JSTRY_FOR_OF:
          case JSTRY_LOOP:
            break;

          default:
            MOZ_CRASH("Invalid try note");
        }
    }
    return true;
}

static void
HandleExceptionBaseline(JSContext* cx, const JitFrameIterator& frame, ResumeFromException* rfe,
                        jsbytecode* pc)
{
    MOZ_ASSERT(frame.isBaselineJS());

    bool frameOk = false;
    RootedScript script(cx, frame.baselineFrame()->script());

    if (script->hasScriptCounts()) {
        PCCounts* counts = script->getThrowCounts(pc);
        // If we failed to allocate, then skip the increment and continue to
        // handle the exception.
        if (counts)
            counts->numExec()++;
    }

    // We may be propagating a forced return from the interrupt
    // callback, which cannot easily force a return.
    if (cx->isPropagatingForcedReturn()) {
        cx->clearPropagatingForcedReturn();
        ForcedReturn(cx, frame, pc, rfe);
        return;
    }

  again:
    if (cx->isExceptionPending()) {
        if (!cx->isClosingGenerator()) {
            switch (Debugger::onExceptionUnwind(cx, frame.baselineFrame())) {
              case JSTRAP_ERROR:
                // Uncatchable exception.
                MOZ_ASSERT(!cx->isExceptionPending());
                goto again;

              case JSTRAP_CONTINUE:
              case JSTRAP_THROW:
                MOZ_ASSERT(cx->isExceptionPending());
                break;

              case JSTRAP_RETURN:
                if (script->hasTrynotes())
                    CloseLiveIteratorsBaselineForUncatchableException(cx, frame, pc);
                ForcedReturn(cx, frame, pc, rfe);
                return;

              default:
                MOZ_CRASH("Invalid trap status");
            }
        }

        if (script->hasTrynotes()) {
            ScopeIter si(cx, frame.baselineFrame(), pc);
            if (!ProcessTryNotesBaseline(cx, frame, si, rfe, &pc))
                goto again;
            if (rfe->kind != ResumeFromException::RESUME_ENTRY_FRAME) {
                // No need to increment the PCCounts number of execution here,
                // as the interpreter increments any PCCounts if present.
                MOZ_ASSERT_IF(script->hasScriptCounts(), script->maybeGetPCCounts(pc));
                return;
            }
        }

        frameOk = HandleClosingGeneratorReturn(cx, frame.baselineFrame(), frameOk);
        frameOk = Debugger::onLeaveFrame(cx, frame.baselineFrame(), frameOk);
    } else if (script->hasTrynotes()) {
        CloseLiveIteratorsBaselineForUncatchableException(cx, frame, pc);
    }

    OnLeaveBaselineFrame(cx, frame, pc, rfe, frameOk);
}

struct AutoDeleteDebugModeOSRInfo
{
    BaselineFrame* frame;
    explicit AutoDeleteDebugModeOSRInfo(BaselineFrame* frame) : frame(frame) { MOZ_ASSERT(frame); }
    ~AutoDeleteDebugModeOSRInfo() { frame->deleteDebugModeOSRInfo(); }
};

struct AutoResetLastProfilerFrameOnReturnFromException
{
    JSContext* cx;
    ResumeFromException* rfe;

    AutoResetLastProfilerFrameOnReturnFromException(JSContext* cx, ResumeFromException* rfe)
      : cx(cx), rfe(rfe) {}

    ~AutoResetLastProfilerFrameOnReturnFromException() {
        if (!cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(cx->runtime()))
            return;

        MOZ_ASSERT(cx->runtime()->jitActivation == cx->runtime()->profilingActivation());

        void* lastProfilingFrame = getLastProfilingFrame();
        cx->runtime()->jitActivation->setLastProfilingFrame(lastProfilingFrame);
    }

    void* getLastProfilingFrame() {
        switch (rfe->kind) {
          case ResumeFromException::RESUME_ENTRY_FRAME:
            return nullptr;

          // The following all return into baseline frames.
          case ResumeFromException::RESUME_CATCH:
          case ResumeFromException::RESUME_FINALLY:
          case ResumeFromException::RESUME_FORCED_RETURN:
            return rfe->framePointer + BaselineFrame::FramePointerOffset;

          // When resuming into a bailed-out ion frame, use the bailout info to
          // find the frame we are resuming into.
          case ResumeFromException::RESUME_BAILOUT:
            return rfe->bailoutInfo->incomingStack;
        }

        MOZ_CRASH("Invalid ResumeFromException type!");
        return nullptr;
    }
};

void
HandleException(ResumeFromException* rfe)
{
    JSContext* cx = GetJSContextFromJitCode();
    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());

    AutoResetLastProfilerFrameOnReturnFromException profFrameReset(cx, rfe);

    rfe->kind = ResumeFromException::RESUME_ENTRY_FRAME;

    JitSpew(JitSpew_IonInvalidate, "handling exception");

    // Clear any Ion return override that's been set.
    // This may happen if a callVM function causes an invalidation (setting the
    // override), and then fails, bypassing the bailout handlers that would
    // otherwise clear the return override.
    if (cx->runtime()->jitRuntime()->hasIonReturnOverride())
        cx->runtime()->jitRuntime()->takeIonReturnOverride();

    JitActivation* activation = cx->runtime()->activation()->asJit();

#ifdef CHECK_OSIPOINT_REGISTERS
    if (JitOptions.checkOsiPointRegisters)
        activation->setCheckRegs(false);
#endif

    // The Debugger onExceptionUnwind hook (reachable via
    // HandleExceptionBaseline below) may cause on-stack recompilation of
    // baseline scripts, which may patch return addresses on the stack. Since
    // JitFrameIterators cache the previous frame's return address when
    // iterating, we need a variant here that is automatically updated should
    // on-stack recompilation occur.
    DebugModeOSRVolatileJitFrameIterator iter(cx);
    while (!iter.isEntry()) {
        bool overrecursed = false;
        if (iter.isIonJS()) {
            // Search each inlined frame for live iterator objects, and close
            // them.
            InlineFrameIterator frames(cx, &iter);

            // Invalidation state will be the same for all inlined scripts in the frame.
            IonScript* ionScript = nullptr;
            bool invalidated = iter.checkInvalidation(&ionScript);

            for (;;) {
                HandleExceptionIon(cx, frames, rfe, &overrecursed);

                if (rfe->kind == ResumeFromException::RESUME_BAILOUT) {
                    if (invalidated)
                        ionScript->decrementInvalidationCount(cx->runtime()->defaultFreeOp());
                    return;
                }

                MOZ_ASSERT(rfe->kind == ResumeFromException::RESUME_ENTRY_FRAME);

                // When profiling, each frame popped needs a notification that
                // the function has exited, so invoke the probe that a function
                // is exiting.

                JSScript* script = frames.script();
                probes::ExitScript(cx, script, script->functionNonDelazifying(),
                                   /* popSPSFrame = */ false);
                if (!frames.more()) {
                    TraceLogStopEvent(logger, TraceLogger_IonMonkey);
                    TraceLogStopEvent(logger, TraceLogger_Scripts);
                    break;
                }
                ++frames;
            }

            activation->removeIonFrameRecovery(iter.jsFrame());
            if (invalidated)
                ionScript->decrementInvalidationCount(cx->runtime()->defaultFreeOp());

        } else if (iter.isBaselineJS()) {
            // Set a flag on the frame to signal to DebugModeOSR that we're
            // handling an exception. Also ensure the frame has an override
            // pc. We clear the frame's override pc when we leave this block,
            // this is fine because we're either:
            //
            // (1) Going to enter a catch or finally block. We don't want to
            //     keep the old pc when we're executing JIT code.
            // (2) Going to pop the frame, either here or a forced return.
            //     In this case nothing will observe the frame's pc.
            // (3) Performing an exception bailout. In this case
            //     FinishBailoutToBaseline will set the pc to the resume pc
            //     and clear it before it returns to JIT code.
            jsbytecode* pc;
            iter.baselineScriptAndPc(nullptr, &pc);
            AutoBaselineHandlingException handlingException(iter.baselineFrame(), pc);

            HandleExceptionBaseline(cx, iter, rfe, pc);

            // If we are propagating an exception through a frame with
            // on-stack recompile info, we should free the allocated
            // RecompileInfo struct before we leave this block, as we will not
            // be returning to the recompile handler.
            AutoDeleteDebugModeOSRInfo deleteDebugModeOSRInfo(iter.baselineFrame());

            if (rfe->kind != ResumeFromException::RESUME_ENTRY_FRAME &&
                rfe->kind != ResumeFromException::RESUME_FORCED_RETURN)
            {
                return;
            }

            TraceLogStopEvent(logger, TraceLogger_Baseline);
            TraceLogStopEvent(logger, TraceLogger_Scripts);

            // Unwind profiler pseudo-stack
            JSScript* script = iter.script();
            probes::ExitScript(cx, script, script->functionNonDelazifying(),
                               /* popSPSFrame = */ false);

            if (rfe->kind == ResumeFromException::RESUME_FORCED_RETURN)
                return;
        }

        JitFrameLayout* current = iter.isScripted() ? iter.jsFrame() : nullptr;

        ++iter;

        if (current) {
            // Unwind the frame by updating jitTop. This is necessary so that
            // (1) debugger exception unwind and leave frame hooks don't see this
            // frame when they use ScriptFrameIter, and (2) ScriptFrameIter does
            // not crash when accessing an IonScript that's destroyed by the
            // ionScript->decref call.
            EnsureExitFrame(current);
            cx->runtime()->jitTop = (uint8_t*)current;
        }

        if (overrecursed) {
            // We hit an overrecursion error during bailout. Report it now.
            ReportOverRecursed(cx);
        }
    }

    rfe->stackPointer = iter.fp();
}

void
EnsureExitFrame(CommonFrameLayout* frame)
{
    switch (frame->prevType()) {
      case JitFrame_Unwound_IonJS:
      case JitFrame_Unwound_IonStub:
      case JitFrame_Unwound_BaselineJS:
      case JitFrame_Unwound_BaselineStub:
      case JitFrame_Unwound_Rectifier:
      case JitFrame_Unwound_IonAccessorIC:
        // Already an exit frame, nothing to do.
        return;

      case JitFrame_Entry:
        // The previous frame type is the entry frame, so there's no actual
        // need for an exit frame.
        return;

      case JitFrame_Rectifier:
        // The rectifier code uses the frame descriptor to discard its stack,
        // so modifying its descriptor size here would be dangerous. Instead,
        // we change the frame type, and teach the stack walking code how to
        // deal with this edge case. bug 717297 would obviate the need
        frame->changePrevType(JitFrame_Unwound_Rectifier);
        return;

      case JitFrame_BaselineStub:
        frame->changePrevType(JitFrame_Unwound_BaselineStub);
        return;

      case JitFrame_BaselineJS:
        frame->changePrevType(JitFrame_Unwound_BaselineJS);
        return;

      case JitFrame_IonJS:
        frame->changePrevType(JitFrame_Unwound_IonJS);
        return;

      case JitFrame_IonStub:
        frame->changePrevType(JitFrame_Unwound_IonStub);
        return;

      case JitFrame_IonAccessorIC:
        frame->changePrevType(JitFrame_Unwound_IonAccessorIC);
        return;

      case JitFrame_Exit:
      case JitFrame_Bailout:
      case JitFrame_LazyLink:
        // Fall-through to MOZ_CRASH below.
        break;
    }

    MOZ_CRASH("Unexpected frame type");
}

CalleeToken
MarkCalleeToken(JSTracer* trc, CalleeToken token)
{
    switch (CalleeTokenTag tag = GetCalleeTokenTag(token)) {
      case CalleeToken_Function:
      case CalleeToken_FunctionConstructing:
      {
        JSFunction* fun = CalleeTokenToFunction(token);
        TraceRoot(trc, &fun, "jit-callee");
        return CalleeToToken(fun, tag == CalleeToken_FunctionConstructing);
      }
      case CalleeToken_Script:
      {
        JSScript* script = CalleeTokenToScript(token);
        TraceRoot(trc, &script, "jit-script");
        return CalleeToToken(script);
      }
      default:
        MOZ_CRASH("unknown callee token type");
    }
}

uintptr_t*
JitFrameLayout::slotRef(SafepointSlotEntry where)
{
    if (where.stack)
        return (uintptr_t*)((uint8_t*)this - where.slot);
    return (uintptr_t*)((uint8_t*)argv() + where.slot);
}

#ifdef JS_NUNBOX32
static inline uintptr_t
ReadAllocation(const JitFrameIterator& frame, const LAllocation* a)
{
    if (a->isGeneralReg()) {
        Register reg = a->toGeneralReg()->reg();
        return frame.machineState().read(reg);
    }
    return *frame.jsFrame()->slotRef(SafepointSlotEntry(a));
}
#endif

static void
MarkThisAndArguments(JSTracer* trc, const JitFrameIterator& frame)
{
    // Mark |this| and any extra actual arguments for an Ion frame. Marking of
    // formal arguments is taken care of by the frame's safepoint/snapshot,
    // except when the script might have lazy arguments or rest, in which case
    // we mark them as well. We also have to mark formals if we have a LazyLink
    // frame.

    JitFrameLayout* layout = frame.isExitFrameLayout<LazyLinkExitFrameLayout>()
                             ? frame.exitFrame()->as<LazyLinkExitFrameLayout>()->jsFrame()
                             : frame.jsFrame();

    if (!CalleeTokenIsFunction(layout->calleeToken()))
        return;

    size_t nargs = layout->numActualArgs();
    size_t nformals = 0;

    JSFunction* fun = CalleeTokenToFunction(layout->calleeToken());
    if (!frame.isExitFrameLayout<LazyLinkExitFrameLayout>() &&
        !fun->nonLazyScript()->mayReadFrameArgsDirectly())
    {
        nformals = fun->nargs();
    }

    size_t newTargetOffset = Max(nargs, fun->nargs());

    Value* argv = layout->argv();

    // Trace |this|.
    TraceRoot(trc, argv, "ion-thisv");

    // Trace actual arguments beyond the formals. Note + 1 for thisv.
    for (size_t i = nformals + 1; i < nargs + 1; i++)
        TraceRoot(trc, &argv[i], "ion-argv");

    // Always mark the new.target from the frame. It's not in the snapshots.
    // +1 to pass |this|
    if (CalleeTokenIsConstructing(layout->calleeToken()))
        TraceRoot(trc, &argv[1 + newTargetOffset], "ion-newTarget");
}

#ifdef JS_NUNBOX32
static inline void
WriteAllocation(const JitFrameIterator& frame, const LAllocation* a, uintptr_t value)
{
    if (a->isGeneralReg()) {
        Register reg = a->toGeneralReg()->reg();
        frame.machineState().write(reg, value);
    } else {
        *frame.jsFrame()->slotRef(SafepointSlotEntry(a)) = value;
    }
}
#endif

static void
MarkIonJSFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    JitFrameLayout* layout = (JitFrameLayout*)frame.fp();

    layout->replaceCalleeToken(MarkCalleeToken(trc, layout->calleeToken()));

    IonScript* ionScript = nullptr;
    if (frame.checkInvalidation(&ionScript)) {
        // This frame has been invalidated, meaning that its IonScript is no
        // longer reachable through the callee token (JSFunction/JSScript->ion
        // is now nullptr or recompiled). Manually trace it here.
        IonScript::Trace(trc, ionScript);
    } else {
        ionScript = frame.ionScriptFromCalleeToken();
    }

    MarkThisAndArguments(trc, frame);

    const SafepointIndex* si = ionScript->getSafepointIndex(frame.returnAddressToFp());

    SafepointReader safepoint(ionScript, si);

    // Scan through slots which contain pointers (or on punboxing systems,
    // actual values).
    SafepointSlotEntry entry;

    while (safepoint.getGcSlot(&entry)) {
        uintptr_t* ref = layout->slotRef(entry);
        TraceGenericPointerRoot(trc, reinterpret_cast<gc::Cell**>(ref), "ion-gc-slot");
    }

    while (safepoint.getValueSlot(&entry)) {
        Value* v = (Value*)layout->slotRef(entry);
        TraceRoot(trc, v, "ion-gc-slot");
    }

    uintptr_t* spill = frame.spillBase();
    LiveGeneralRegisterSet gcRegs = safepoint.gcSpills();
    LiveGeneralRegisterSet valueRegs = safepoint.valueSpills();
    for (GeneralRegisterBackwardIterator iter(safepoint.allGprSpills()); iter.more(); iter++) {
        --spill;
        if (gcRegs.has(*iter))
            TraceGenericPointerRoot(trc, reinterpret_cast<gc::Cell**>(spill), "ion-gc-spill");
        else if (valueRegs.has(*iter))
            TraceRoot(trc, reinterpret_cast<Value*>(spill), "ion-value-spill");
    }

#ifdef JS_NUNBOX32
    LAllocation type, payload;
    while (safepoint.getNunboxSlot(&type, &payload)) {
        jsval_layout layout;
        layout.s.tag = (JSValueTag)ReadAllocation(frame, &type);
        layout.s.payload.uintptr = ReadAllocation(frame, &payload);

        Value v = IMPL_TO_JSVAL(layout);
        TraceRoot(trc, &v, "ion-torn-value");

        if (v != IMPL_TO_JSVAL(layout)) {
            // GC moved the value, replace the stored payload.
            layout = JSVAL_TO_IMPL(v);
            WriteAllocation(frame, &payload, layout.s.payload.uintptr);
        }
    }
#endif
}

static void
MarkBailoutFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    JitFrameLayout* layout = (JitFrameLayout*)frame.fp();

    layout->replaceCalleeToken(MarkCalleeToken(trc, layout->calleeToken()));

    // We have to mark the list of actual arguments, as only formal arguments
    // are represented in the Snapshot.
    MarkThisAndArguments(trc, frame);

    // Under a bailout, do not have a Safepoint to only iterate over GC-things.
    // Thus we use a SnapshotIterator to trace all the locations which would be
    // used to reconstruct the Baseline frame.
    //
    // Note that at the time where this function is called, we have not yet
    // started to reconstruct baseline frames.

    // The vector of recover instructions is already traced as part of the
    // JitActivation.
    SnapshotIterator snapIter(frame, frame.activation()->bailoutData()->machineState());

    // For each instruction, we read the allocations without evaluating the
    // recover instruction, nor reconstructing the frame. We are only looking at
    // tracing readable allocations.
    while (true) {
        while (snapIter.moreAllocations())
            snapIter.traceAllocation(trc);

        if (!snapIter.moreInstructions())
            break;
        snapIter.nextInstruction();
    };

}

void
UpdateIonJSFrameForMinorGC(JSTracer* trc, const JitFrameIterator& frame)
{
    // Minor GCs may move slots/elements allocated in the nursery. Update
    // any slots/elements pointers stored in this frame.

    JitFrameLayout* layout = (JitFrameLayout*)frame.fp();

    IonScript* ionScript = nullptr;
    if (frame.checkInvalidation(&ionScript)) {
        // This frame has been invalidated, meaning that its IonScript is no
        // longer reachable through the callee token (JSFunction/JSScript->ion
        // is now nullptr or recompiled).
    } else {
        ionScript = frame.ionScriptFromCalleeToken();
    }

    Nursery& nursery = trc->runtime()->gc.nursery;

    const SafepointIndex* si = ionScript->getSafepointIndex(frame.returnAddressToFp());
    SafepointReader safepoint(ionScript, si);

    LiveGeneralRegisterSet slotsRegs = safepoint.slotsOrElementsSpills();
    uintptr_t* spill = frame.spillBase();
    for (GeneralRegisterBackwardIterator iter(safepoint.allGprSpills()); iter.more(); iter++) {
        --spill;
        if (slotsRegs.has(*iter))
            nursery.forwardBufferPointer(reinterpret_cast<HeapSlot**>(spill));
    }

    // Skip to the right place in the safepoint
    SafepointSlotEntry entry;
    while (safepoint.getGcSlot(&entry));
    while (safepoint.getValueSlot(&entry));
#ifdef JS_NUNBOX32
    LAllocation type, payload;
    while (safepoint.getNunboxSlot(&type, &payload));
#endif

    while (safepoint.getSlotsOrElementsSlot(&entry)) {
        HeapSlot** slots = reinterpret_cast<HeapSlot**>(layout->slotRef(entry));
        nursery.forwardBufferPointer(slots);
    }
}

static void
MarkJitStubFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    // Mark the ICStub pointer stored in the stub frame. This is necessary
    // so that we don't destroy the stub code after unlinking the stub.

    MOZ_ASSERT(frame.type() == JitFrame_IonStub || frame.type() == JitFrame_BaselineStub);
    JitStubFrameLayout* layout = (JitStubFrameLayout*)frame.fp();

    if (ICStub* stub = layout->maybeStubPtr()) {
        MOZ_ASSERT(ICStub::CanMakeCalls(stub->kind()));
        stub->trace(trc);
    }
}

static void
MarkIonAccessorICFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    MOZ_ASSERT(frame.type() == JitFrame_IonAccessorIC);
    IonAccessorICFrameLayout* layout = (IonAccessorICFrameLayout*)frame.fp();
    TraceRoot(trc, layout->stubCode(), "ion-ic-accessor-code");
}

void
JitActivationIterator::jitStackRange(uintptr_t*& min, uintptr_t*& end)
{
    JitFrameIterator frames(*this);

    if (frames.isFakeExitFrame()) {
        min = reinterpret_cast<uintptr_t*>(frames.fp());
    } else {
        ExitFrameLayout* exitFrame = frames.exitFrame();
        ExitFooterFrame* footer = exitFrame->footer();
        const VMFunction* f = footer->function();
        if (exitFrame->isWrapperExit() && f->outParam == Type_Handle) {
            switch (f->outParamRootType) {
              case VMFunction::RootNone:
                MOZ_CRASH("Handle outparam must have root type");
              case VMFunction::RootObject:
              case VMFunction::RootString:
              case VMFunction::RootPropertyName:
              case VMFunction::RootFunction:
              case VMFunction::RootCell:
                // These are all handles to GCThing pointers.
                min = reinterpret_cast<uintptr_t*>(footer->outParam<void*>());
                break;
              case VMFunction::RootValue:
                min = reinterpret_cast<uintptr_t*>(footer->outParam<Value>());
                break;
            }
        } else {
            min = reinterpret_cast<uintptr_t*>(footer);
        }
    }

    while (!frames.done())
        ++frames;

    end = reinterpret_cast<uintptr_t*>(frames.prevFp());
}

#ifdef JS_CODEGEN_MIPS32
uint8_t*
alignDoubleSpillWithOffset(uint8_t* pointer, int32_t offset)
{
    uint32_t address = reinterpret_cast<uint32_t>(pointer);
    address = (address - offset) & ~(ABIStackAlignment - 1);
    return reinterpret_cast<uint8_t*>(address);
}

static void
MarkJitExitFrameCopiedArguments(JSTracer* trc, const VMFunction* f, ExitFooterFrame* footer)
{
    uint8_t* doubleArgs = reinterpret_cast<uint8_t*>(footer);
    doubleArgs = alignDoubleSpillWithOffset(doubleArgs, sizeof(intptr_t));
    if (f->outParam == Type_Handle)
        doubleArgs -= sizeof(Value);
    doubleArgs -= f->doubleByRefArgs() * sizeof(double);

    for (uint32_t explicitArg = 0; explicitArg < f->explicitArgs; explicitArg++) {
        if (f->argProperties(explicitArg) == VMFunction::DoubleByRef) {
            // Arguments with double size can only have RootValue type.
            if (f->argRootType(explicitArg) == VMFunction::RootValue)
                TraceRoot(trc, reinterpret_cast<Value*>(doubleArgs), "ion-vm-args");
            else
                MOZ_ASSERT(f->argRootType(explicitArg) == VMFunction::RootNone);
            doubleArgs += sizeof(double);
        }
    }
}
#else
static void
MarkJitExitFrameCopiedArguments(JSTracer* trc, const VMFunction* f, ExitFooterFrame* footer)
{
    // This is NO-OP on other platforms.
}
#endif

static void
MarkJitExitFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    // Ignore fake exit frames created by EnsureExitFrame.
    if (frame.isFakeExitFrame())
        return;

    ExitFooterFrame* footer = frame.exitFrame()->footer();

    // Mark the code of the code handling the exit path.  This is needed because
    // invalidated script are no longer marked because data are erased by the
    // invalidation and relocation data are no longer reliable.  So the VM
    // wrapper or the invalidation code may be GC if no JitCode keep reference
    // on them.
    MOZ_ASSERT(uintptr_t(footer->jitCode()) != uintptr_t(-1));

    // This corresponds to the case where we have build a fake exit frame which
    // handles the case of a native function call. We need to mark the argument
    // vector of the function call, and also new.target if it was a constructing
    // call.
    if (frame.isExitFrameLayout<NativeExitFrameLayout>()) {
        NativeExitFrameLayout* native = frame.exitFrame()->as<NativeExitFrameLayout>();
        size_t len = native->argc() + 2;
        Value* vp = native->vp();
        TraceRootRange(trc, len, vp, "ion-native-args");
        if (frame.isExitFrameLayout<ConstructNativeExitFrameLayout>())
            TraceRoot(trc, vp + len, "ion-native-new-target");
        return;
    }

    if (frame.isExitFrameLayout<IonOOLNativeExitFrameLayout>()) {
        IonOOLNativeExitFrameLayout* oolnative =
            frame.exitFrame()->as<IonOOLNativeExitFrameLayout>();
        TraceRoot(trc, oolnative->stubCode(), "ion-ool-native-code");
        TraceRoot(trc, oolnative->vp(), "iol-ool-native-vp");
        size_t len = oolnative->argc() + 1;
        TraceRootRange(trc, len, oolnative->thisp(), "ion-ool-native-thisargs");
        return;
    }

    if (frame.isExitFrameLayout<IonOOLPropertyOpExitFrameLayout>() ||
        frame.isExitFrameLayout<IonOOLSetterOpExitFrameLayout>())
    {
        // A SetterOp frame is a different size, but that's the only relevant
        // difference between the two. The fields that need marking are all in
        // the common base class.
        IonOOLPropertyOpExitFrameLayout* oolgetter =
            frame.isExitFrameLayout<IonOOLPropertyOpExitFrameLayout>()
            ? frame.exitFrame()->as<IonOOLPropertyOpExitFrameLayout>()
            : frame.exitFrame()->as<IonOOLSetterOpExitFrameLayout>();
        TraceRoot(trc, oolgetter->stubCode(), "ion-ool-property-op-code");
        TraceRoot(trc, oolgetter->vp(), "ion-ool-property-op-vp");
        TraceRoot(trc, oolgetter->id(), "ion-ool-property-op-id");
        TraceRoot(trc, oolgetter->obj(), "ion-ool-property-op-obj");
        return;
    }

    if (frame.isExitFrameLayout<IonOOLProxyExitFrameLayout>()) {
        IonOOLProxyExitFrameLayout* oolproxy = frame.exitFrame()->as<IonOOLProxyExitFrameLayout>();
        TraceRoot(trc, oolproxy->stubCode(), "ion-ool-proxy-code");
        TraceRoot(trc, oolproxy->vp(), "ion-ool-proxy-vp");
        TraceRoot(trc, oolproxy->id(), "ion-ool-proxy-id");
        TraceRoot(trc, oolproxy->proxy(), "ion-ool-proxy-proxy");
        return;
    }

    if (frame.isExitFrameLayout<IonDOMExitFrameLayout>()) {
        IonDOMExitFrameLayout* dom = frame.exitFrame()->as<IonDOMExitFrameLayout>();
        TraceRoot(trc, dom->thisObjAddress(), "ion-dom-args");
        if (dom->isMethodFrame()) {
            IonDOMMethodExitFrameLayout* method =
                reinterpret_cast<IonDOMMethodExitFrameLayout*>(dom);
            size_t len = method->argc() + 2;
            Value* vp = method->vp();
            TraceRootRange(trc, len, vp, "ion-dom-args");
        } else {
            TraceRoot(trc, dom->vp(), "ion-dom-args");
        }
        return;
    }

    if (frame.isExitFrameLayout<LazyLinkExitFrameLayout>()) {
        LazyLinkExitFrameLayout* ll = frame.exitFrame()->as<LazyLinkExitFrameLayout>();
        JitFrameLayout* layout = ll->jsFrame();

        TraceRoot(trc, ll->stubCode(), "lazy-link-code");
        layout->replaceCalleeToken(MarkCalleeToken(trc, layout->calleeToken()));
        MarkThisAndArguments(trc, frame);
        return;
    }

    if (frame.isBareExit()) {
        // Nothing to mark. Fake exit frame pushed for VM functions with
        // nothing to mark on the stack.
        return;
    }

    TraceRoot(trc, footer->addressOfJitCode(), "ion-exit-code");

    const VMFunction* f = footer->function();
    if (f == nullptr)
        return;

    // Mark arguments of the VM wrapper.
    uint8_t* argBase = frame.exitFrame()->argBase();
    for (uint32_t explicitArg = 0; explicitArg < f->explicitArgs; explicitArg++) {
        switch (f->argRootType(explicitArg)) {
          case VMFunction::RootNone:
            break;
          case VMFunction::RootObject: {
            // Sometimes we can bake in HandleObjects to nullptr.
            JSObject** pobj = reinterpret_cast<JSObject**>(argBase);
            if (*pobj)
                TraceRoot(trc, pobj, "ion-vm-args");
            break;
          }
          case VMFunction::RootString:
          case VMFunction::RootPropertyName:
            TraceRoot(trc, reinterpret_cast<JSString**>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootFunction:
            TraceRoot(trc, reinterpret_cast<JSFunction**>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootValue:
            TraceRoot(trc, reinterpret_cast<Value*>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootCell:
            TraceGenericPointerRoot(trc, reinterpret_cast<gc::Cell**>(argBase), "ion-vm-args");
            break;
        }

        switch (f->argProperties(explicitArg)) {
          case VMFunction::WordByValue:
          case VMFunction::WordByRef:
            argBase += sizeof(void*);
            break;
          case VMFunction::DoubleByValue:
          case VMFunction::DoubleByRef:
            argBase += 2 * sizeof(void*);
            break;
        }
    }

    if (f->outParam == Type_Handle) {
        switch (f->outParamRootType) {
          case VMFunction::RootNone:
            MOZ_CRASH("Handle outparam must have root type");
          case VMFunction::RootObject:
            TraceRoot(trc, footer->outParam<JSObject*>(), "ion-vm-out");
            break;
          case VMFunction::RootString:
          case VMFunction::RootPropertyName:
            TraceRoot(trc, footer->outParam<JSString*>(), "ion-vm-out");
            break;
          case VMFunction::RootFunction:
            TraceRoot(trc, footer->outParam<JSFunction*>(), "ion-vm-out");
            break;
          case VMFunction::RootValue:
            TraceRoot(trc, footer->outParam<Value>(), "ion-vm-outvp");
            break;
          case VMFunction::RootCell:
            TraceGenericPointerRoot(trc, footer->outParam<gc::Cell*>(), "ion-vm-out");
            break;
        }
    }

    MarkJitExitFrameCopiedArguments(trc, f, footer);
}

static void
MarkRectifierFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    // Mark thisv.
    //
    // Baseline JIT code generated as part of the ICCall_Fallback stub may use
    // it if we're calling a constructor that returns a primitive value.
    RectifierFrameLayout* layout = (RectifierFrameLayout*)frame.fp();
    TraceRoot(trc, &layout->argv()[0], "ion-thisv");
}

static void
MarkJitActivation(JSTracer* trc, const JitActivationIterator& activations)
{
    JitActivation* activation = activations->asJit();

#ifdef CHECK_OSIPOINT_REGISTERS
    if (JitOptions.checkOsiPointRegisters) {
        // GC can modify spilled registers, breaking our register checks.
        // To handle this, we disable these checks for the current VM call
        // when a GC happens.
        activation->setCheckRegs(false);
    }
#endif

    activation->markRematerializedFrames(trc);
    activation->markIonRecovery(trc);

    for (JitFrameIterator frames(activations); !frames.done(); ++frames) {
        switch (frames.type()) {
          case JitFrame_Exit:
          case JitFrame_LazyLink:
            MarkJitExitFrame(trc, frames);
            break;
          case JitFrame_BaselineJS:
            frames.baselineFrame()->trace(trc, frames);
            break;
          case JitFrame_IonJS:
            MarkIonJSFrame(trc, frames);
            break;
          case JitFrame_BaselineStub:
          case JitFrame_IonStub:
            MarkJitStubFrame(trc, frames);
            break;
          case JitFrame_Bailout:
            MarkBailoutFrame(trc, frames);
            break;
          case JitFrame_Unwound_IonJS:
          case JitFrame_Unwound_BaselineJS:
          case JitFrame_Unwound_BaselineStub:
          case JitFrame_Unwound_IonAccessorIC:
            MOZ_CRASH("invalid");
          case JitFrame_Rectifier:
            MarkRectifierFrame(trc, frames);
            break;
          case JitFrame_Unwound_Rectifier:
            break;
          case JitFrame_IonAccessorIC:
            MarkIonAccessorICFrame(trc, frames);
            break;
          default:
            MOZ_CRASH("unexpected frame type");
        }
    }
}

void
MarkJitActivations(JSRuntime* rt, JSTracer* trc)
{
    for (JitActivationIterator activations(rt); !activations.done(); ++activations)
        MarkJitActivation(trc, activations);
}

JSCompartment*
TopmostIonActivationCompartment(JSRuntime* rt)
{
    for (JitActivationIterator activations(rt); !activations.done(); ++activations) {
        for (JitFrameIterator frames(activations); !frames.done(); ++frames) {
            if (frames.type() == JitFrame_IonJS)
                return activations.activation()->compartment();
        }
    }
    return nullptr;
}

void UpdateJitActivationsForMinorGC(JSRuntime* rt, JSTracer* trc)
{
    MOZ_ASSERT(trc->runtime()->isHeapMinorCollecting());
    for (JitActivationIterator activations(rt); !activations.done(); ++activations) {
        for (JitFrameIterator frames(activations); !frames.done(); ++frames) {
            if (frames.type() == JitFrame_IonJS)
                UpdateIonJSFrameForMinorGC(trc, frames);
        }
    }
}

void
GetPcScript(JSContext* cx, JSScript** scriptRes, jsbytecode** pcRes)
{
    JitSpew(JitSpew_IonSnapshots, "Recover PC & Script from the last frame.");

    // Recover the return address so that we can look it up in the
    // PcScriptCache, as script/pc computation is expensive.
    JSRuntime* rt = cx->runtime();
    JitActivationIterator iter(rt);
    JitFrameIterator it(iter);
    uint8_t* retAddr;
    if (it.isExitFrame()) {
        ++it;

        // Skip rectifier frames.
        if (it.isRectifierMaybeUnwound()) {
            ++it;
            MOZ_ASSERT(it.isBaselineStub() || it.isBaselineJS() || it.isIonJS());
        }

        // Skip Baseline or Ion stub frames.
        if (it.isBaselineStubMaybeUnwound()) {
            ++it;
            MOZ_ASSERT(it.isBaselineJS());
        } else if (it.isIonStubMaybeUnwound() || it.isIonAccessorICMaybeUnwound()) {
            ++it;
            MOZ_ASSERT(it.isIonJS());
        }

        MOZ_ASSERT(it.isBaselineJS() || it.isIonJS());

        // Don't use the return address if the BaselineFrame has an override pc.
        // The override pc is cheap to get, so we won't benefit from the cache,
        // and the override pc could change without the return address changing.
        // Moreover, sometimes when an override pc is present during exception
        // handling, the return address is set to nullptr as a sanity check,
        // since we do not return to the frame that threw the exception.
        if (!it.isBaselineJS() || !it.baselineFrame()->hasOverridePc()) {
            retAddr = it.returnAddressToFp();
            MOZ_ASSERT(retAddr);
        } else {
            retAddr = nullptr;
        }
    } else {
        MOZ_ASSERT(it.isBailoutJS());
        retAddr = it.returnAddress();
    }

    uint32_t hash;
    if (retAddr) {
        hash = PcScriptCache::Hash(retAddr);

        // Lazily initialize the cache. The allocation may safely fail and will not GC.
        if (MOZ_UNLIKELY(rt->ionPcScriptCache == nullptr)) {
            rt->ionPcScriptCache = (PcScriptCache*)js_malloc(sizeof(struct PcScriptCache));
            if (rt->ionPcScriptCache)
                rt->ionPcScriptCache->clear(rt->gc.gcNumber());
        }

        if (rt->ionPcScriptCache && rt->ionPcScriptCache->get(rt, hash, retAddr, scriptRes, pcRes))
            return;
    }

    // Lookup failed: undertake expensive process to recover the innermost inlined frame.
    jsbytecode* pc = nullptr;
    if (it.isIonJS() || it.isBailoutJS()) {
        InlineFrameIterator ifi(cx, &it);
        *scriptRes = ifi.script();
        pc = ifi.pc();
    } else {
        MOZ_ASSERT(it.isBaselineJS());
        it.baselineScriptAndPc(scriptRes, &pc);
    }

    if (pcRes)
        *pcRes = pc;

    // Add entry to cache.
    if (retAddr && rt->ionPcScriptCache)
        rt->ionPcScriptCache->add(hash, retAddr, pc, *scriptRes);
}

uint32_t
OsiIndex::returnPointDisplacement() const
{
    // In general, pointer arithmetic on code is bad, but in this case,
    // getting the return address from a call instruction, stepping over pools
    // would be wrong.
    return callPointDisplacement_ + Assembler::PatchWrite_NearCallSize();
}

RInstructionResults::RInstructionResults(JitFrameLayout* fp)
  : results_(nullptr),
    fp_(fp),
    initialized_(false)
{
}

RInstructionResults::RInstructionResults(RInstructionResults&& src)
  : results_(mozilla::Move(src.results_)),
    fp_(src.fp_),
    initialized_(src.initialized_)
{
    src.initialized_ = false;
}

RInstructionResults&
RInstructionResults::operator=(RInstructionResults&& rhs)
{
    MOZ_ASSERT(&rhs != this, "self-moves are prohibited");
    this->~RInstructionResults();
    new(this) RInstructionResults(mozilla::Move(rhs));
    return *this;
}

RInstructionResults::~RInstructionResults()
{
    // results_ is freed by the UniquePtr.
}

bool
RInstructionResults::init(JSContext* cx, uint32_t numResults)
{
    if (numResults) {
        results_ = cx->make_unique<Values>();
        if (!results_ || !results_->growBy(numResults))
            return false;

        Value guard = MagicValue(JS_ION_BAILOUT);
        for (size_t i = 0; i < numResults; i++)
            (*results_)[i].init(guard);
    }

    initialized_ = true;
    return true;
}

bool
RInstructionResults::isInitialized() const
{
    return initialized_;
}

#ifdef DEBUG
size_t
RInstructionResults::length() const
{
    return results_->length();
}
#endif

JitFrameLayout*
RInstructionResults::frame() const
{
    MOZ_ASSERT(fp_);
    return fp_;
}

RelocatableValue&
RInstructionResults::operator [](size_t index)
{
    return (*results_)[index];
}

void
RInstructionResults::trace(JSTracer* trc)
{
    // Note: The vector necessary exists, otherwise this object would not have
    // been stored on the activation from where the trace function is called.
    TraceRange(trc, results_->length(), results_->begin(), "ion-recover-results");
}


SnapshotIterator::SnapshotIterator(const JitFrameIterator& iter, const MachineState* machineState)
  : snapshot_(iter.ionScript()->snapshots(),
              iter.snapshotOffset(),
              iter.ionScript()->snapshotsRVATableSize(),
              iter.ionScript()->snapshotsListSize()),
    recover_(snapshot_,
             iter.ionScript()->recovers(),
             iter.ionScript()->recoversSize()),
    fp_(iter.jsFrame()),
    machine_(machineState),
    ionScript_(iter.ionScript()),
    instructionResults_(nullptr)
{
}

SnapshotIterator::SnapshotIterator()
  : snapshot_(nullptr, 0, 0, 0),
    recover_(snapshot_, nullptr, 0),
    fp_(nullptr),
    ionScript_(nullptr),
    instructionResults_(nullptr)
{
}

int32_t
SnapshotIterator::readOuterNumActualArgs() const
{
    return fp_->numActualArgs();
}

uintptr_t
SnapshotIterator::fromStack(int32_t offset) const
{
    return ReadFrameSlot(fp_, offset);
}

static Value
FromObjectPayload(uintptr_t payload)
{
    // Note: Both MIRType_Object and MIRType_ObjectOrNull are encoded in
    // snapshots using JSVAL_TYPE_OBJECT.
    return ObjectOrNullValue(reinterpret_cast<JSObject*>(payload));
}

static Value
FromStringPayload(uintptr_t payload)
{
    return StringValue(reinterpret_cast<JSString*>(payload));
}

static Value
FromSymbolPayload(uintptr_t payload)
{
    return SymbolValue(reinterpret_cast<JS::Symbol*>(payload));
}

static Value
FromTypedPayload(JSValueType type, uintptr_t payload)
{
    switch (type) {
      case JSVAL_TYPE_INT32:
        return Int32Value(payload);
      case JSVAL_TYPE_BOOLEAN:
        return BooleanValue(!!payload);
      case JSVAL_TYPE_STRING:
        return FromStringPayload(payload);
      case JSVAL_TYPE_SYMBOL:
        return FromSymbolPayload(payload);
      case JSVAL_TYPE_OBJECT:
        return FromObjectPayload(payload);
      default:
        MOZ_CRASH("unexpected type - needs payload");
    }
}

bool
SnapshotIterator::allocationReadable(const RValueAllocation& alloc, ReadMethod rm)
{
    // If we have to recover stores, and if we are not interested in the
    // default value of the instruction, then we have to check if the recover
    // instruction results are available.
    if (alloc.needSideEffect() && !(rm & RM_AlwaysDefault)) {
        if (!hasInstructionResults())
            return false;
    }

    switch (alloc.mode()) {
      case RValueAllocation::DOUBLE_REG:
        return hasRegister(alloc.fpuReg());

      case RValueAllocation::TYPED_REG:
        return hasRegister(alloc.reg2());

#if defined(JS_NUNBOX32)
      case RValueAllocation::UNTYPED_REG_REG:
        return hasRegister(alloc.reg()) && hasRegister(alloc.reg2());
      case RValueAllocation::UNTYPED_REG_STACK:
        return hasRegister(alloc.reg()) && hasStack(alloc.stackOffset2());
      case RValueAllocation::UNTYPED_STACK_REG:
        return hasStack(alloc.stackOffset()) && hasRegister(alloc.reg2());
      case RValueAllocation::UNTYPED_STACK_STACK:
        return hasStack(alloc.stackOffset()) && hasStack(alloc.stackOffset2());
#elif defined(JS_PUNBOX64)
      case RValueAllocation::UNTYPED_REG:
        return hasRegister(alloc.reg());
      case RValueAllocation::UNTYPED_STACK:
        return hasStack(alloc.stackOffset());
#endif

      case RValueAllocation::RECOVER_INSTRUCTION:
        return hasInstructionResult(alloc.index());
      case RValueAllocation::RI_WITH_DEFAULT_CST:
        return rm & RM_AlwaysDefault || hasInstructionResult(alloc.index());

      default:
        return true;
    }
}

Value
SnapshotIterator::allocationValue(const RValueAllocation& alloc, ReadMethod rm)
{
    switch (alloc.mode()) {
      case RValueAllocation::CONSTANT:
        return ionScript_->getConstant(alloc.index());

      case RValueAllocation::CST_UNDEFINED:
        return UndefinedValue();

      case RValueAllocation::CST_NULL:
        return NullValue();

      case RValueAllocation::DOUBLE_REG:
        return DoubleValue(fromRegister(alloc.fpuReg()));

      case RValueAllocation::ANY_FLOAT_REG:
      {
        union {
            double d;
            float f;
        } pun;
        MOZ_ASSERT(alloc.fpuReg().isSingle());
        pun.d = fromRegister(alloc.fpuReg());
        // The register contains the encoding of a float32. We just read
        // the bits without making any conversion.
        return Float32Value(pun.f);
      }

      case RValueAllocation::ANY_FLOAT_STACK:
        return Float32Value(ReadFrameFloat32Slot(fp_, alloc.stackOffset()));

      case RValueAllocation::TYPED_REG:
        return FromTypedPayload(alloc.knownType(), fromRegister(alloc.reg2()));

      case RValueAllocation::TYPED_STACK:
      {
        switch (alloc.knownType()) {
          case JSVAL_TYPE_DOUBLE:
            return DoubleValue(ReadFrameDoubleSlot(fp_, alloc.stackOffset2()));
          case JSVAL_TYPE_INT32:
            return Int32Value(ReadFrameInt32Slot(fp_, alloc.stackOffset2()));
          case JSVAL_TYPE_BOOLEAN:
            return BooleanValue(ReadFrameBooleanSlot(fp_, alloc.stackOffset2()));
          case JSVAL_TYPE_STRING:
            return FromStringPayload(fromStack(alloc.stackOffset2()));
          case JSVAL_TYPE_SYMBOL:
            return FromSymbolPayload(fromStack(alloc.stackOffset2()));
          case JSVAL_TYPE_OBJECT:
            return FromObjectPayload(fromStack(alloc.stackOffset2()));
          default:
            MOZ_CRASH("Unexpected type");
        }
      }

#if defined(JS_NUNBOX32)
      case RValueAllocation::UNTYPED_REG_REG:
      {
        jsval_layout layout;
        layout.s.tag = (JSValueTag) fromRegister(alloc.reg());
        layout.s.payload.word = fromRegister(alloc.reg2());
        return IMPL_TO_JSVAL(layout);
      }

      case RValueAllocation::UNTYPED_REG_STACK:
      {
        jsval_layout layout;
        layout.s.tag = (JSValueTag) fromRegister(alloc.reg());
        layout.s.payload.word = fromStack(alloc.stackOffset2());
        return IMPL_TO_JSVAL(layout);
      }

      case RValueAllocation::UNTYPED_STACK_REG:
      {
        jsval_layout layout;
        layout.s.tag = (JSValueTag) fromStack(alloc.stackOffset());
        layout.s.payload.word = fromRegister(alloc.reg2());
        return IMPL_TO_JSVAL(layout);
      }

      case RValueAllocation::UNTYPED_STACK_STACK:
      {
        jsval_layout layout;
        layout.s.tag = (JSValueTag) fromStack(alloc.stackOffset());
        layout.s.payload.word = fromStack(alloc.stackOffset2());
        return IMPL_TO_JSVAL(layout);
      }
#elif defined(JS_PUNBOX64)
      case RValueAllocation::UNTYPED_REG:
      {
        jsval_layout layout;
        layout.asBits = fromRegister(alloc.reg());
        return IMPL_TO_JSVAL(layout);
      }

      case RValueAllocation::UNTYPED_STACK:
      {
        jsval_layout layout;
        layout.asBits = fromStack(alloc.stackOffset());
        return IMPL_TO_JSVAL(layout);
      }
#endif

      case RValueAllocation::RECOVER_INSTRUCTION:
        return fromInstructionResult(alloc.index());

      case RValueAllocation::RI_WITH_DEFAULT_CST:
        if (rm & RM_Normal && hasInstructionResult(alloc.index()))
            return fromInstructionResult(alloc.index());
        MOZ_ASSERT(rm & RM_AlwaysDefault);
        return ionScript_->getConstant(alloc.index2());

      default:
        MOZ_CRASH("huh?");
    }
}

const FloatRegisters::RegisterContent*
SnapshotIterator::floatAllocationPointer(const RValueAllocation& alloc) const
{
    switch (alloc.mode()) {
      case RValueAllocation::ANY_FLOAT_REG:
        return machine_->address(alloc.fpuReg());

      case RValueAllocation::ANY_FLOAT_STACK:
        return (FloatRegisters::RegisterContent*) AddressOfFrameSlot(fp_, alloc.stackOffset());

      default:
        MOZ_CRASH("Not a float allocation.");
    }
}

Value
SnapshotIterator::maybeRead(const RValueAllocation& a, MaybeReadFallback& fallback)
{
    if (allocationReadable(a))
        return allocationValue(a);

    if (fallback.canRecoverResults()) {
        if (!initInstructionResults(fallback))
            MOZ_CRASH("Unable to recover allocations.");

        if (allocationReadable(a))
            return allocationValue(a);

        MOZ_ASSERT_UNREACHABLE("All allocations should be readable.");
    }

    return fallback.unreadablePlaceholder();
}

void
SnapshotIterator::writeAllocationValuePayload(const RValueAllocation& alloc, Value v)
{
    uintptr_t payload = *v.payloadUIntPtr();
#if defined(JS_PUNBOX64)
    // Do not write back the tag, as this will trigger an assertion when we will
    // reconstruct the JS Value while marking again or when bailing out.
    payload &= JSVAL_PAYLOAD_MASK;
#endif

    switch (alloc.mode()) {
      case RValueAllocation::CONSTANT:
        ionScript_->getConstant(alloc.index()) = v;
        break;

      case RValueAllocation::CST_UNDEFINED:
      case RValueAllocation::CST_NULL:
      case RValueAllocation::DOUBLE_REG:
      case RValueAllocation::ANY_FLOAT_REG:
      case RValueAllocation::ANY_FLOAT_STACK:
        MOZ_CRASH("Not a GC thing: Unexpected write");
        break;

      case RValueAllocation::TYPED_REG:
        machine_->write(alloc.reg2(), payload);
        break;

      case RValueAllocation::TYPED_STACK:
        switch (alloc.knownType()) {
          default:
            MOZ_CRASH("Not a GC thing: Unexpected write");
            break;
          case JSVAL_TYPE_STRING:
          case JSVAL_TYPE_SYMBOL:
          case JSVAL_TYPE_OBJECT:
            WriteFrameSlot(fp_, alloc.stackOffset2(), payload);
            break;
        }
        break;

#if defined(JS_NUNBOX32)
      case RValueAllocation::UNTYPED_REG_REG:
      case RValueAllocation::UNTYPED_STACK_REG:
        machine_->write(alloc.reg2(), payload);
        break;

      case RValueAllocation::UNTYPED_REG_STACK:
      case RValueAllocation::UNTYPED_STACK_STACK:
        WriteFrameSlot(fp_, alloc.stackOffset2(), payload);
        break;
#elif defined(JS_PUNBOX64)
      case RValueAllocation::UNTYPED_REG:
        machine_->write(alloc.reg(), v.asRawBits());
        break;

      case RValueAllocation::UNTYPED_STACK:
        WriteFrameSlot(fp_, alloc.stackOffset(), v.asRawBits());
        break;
#endif

      case RValueAllocation::RECOVER_INSTRUCTION:
        MOZ_CRASH("Recover instructions are handled by the JitActivation.");
        break;

      case RValueAllocation::RI_WITH_DEFAULT_CST:
        // Assume that we are always going to be writing on the default value
        // while tracing.
        ionScript_->getConstant(alloc.index2()) = v;
        break;

      default:
        MOZ_CRASH("huh?");
    }
}

void
SnapshotIterator::traceAllocation(JSTracer* trc)
{
    RValueAllocation alloc = readAllocation();
    if (!allocationReadable(alloc, RM_AlwaysDefault))
        return;

    Value v = allocationValue(alloc, RM_AlwaysDefault);
    if (!v.isMarkable())
        return;

    Value copy = v;
    TraceRoot(trc, &v, "ion-typed-reg");
    if (v != copy) {
        MOZ_ASSERT(SameType(v, copy));
        writeAllocationValuePayload(alloc, v);
    }
}

const RResumePoint*
SnapshotIterator::resumePoint() const
{
    return instruction()->toResumePoint();
}

uint32_t
SnapshotIterator::numAllocations() const
{
    return instruction()->numOperands();
}

uint32_t
SnapshotIterator::pcOffset() const
{
    return resumePoint()->pcOffset();
}

void
SnapshotIterator::skipInstruction()
{
    MOZ_ASSERT(snapshot_.numAllocationsRead() == 0);
    size_t numOperands = instruction()->numOperands();
    for (size_t i = 0; i < numOperands; i++)
        skip();
    nextInstruction();
}

bool
SnapshotIterator::initInstructionResults(MaybeReadFallback& fallback)
{
    MOZ_ASSERT(fallback.canRecoverResults());
    JSContext* cx = fallback.maybeCx;

    // If there is only one resume point in the list of instructions, then there
    // is no instruction to recover, and thus no need to register any results.
    if (recover_.numInstructions() == 1)
        return true;

    JitFrameLayout* fp = fallback.frame->jsFrame();
    RInstructionResults* results = fallback.activation->maybeIonFrameRecovery(fp);
    if (!results) {
        AutoCompartment ac(cx, fallback.frame->script()->compartment());

        // We do not have the result yet, which means that an observable stack
        // slot is requested.  As we do not want to bailout every time for the
        // same reason, we need to recompile without optimizing away the
        // observable stack slots.  The script would later be recompiled to have
        // support for Argument objects.
        if (fallback.consequence == MaybeReadFallback::Fallback_Invalidate &&
            !ionScript_->invalidate(cx, /* resetUses = */ false, "Observe recovered instruction."))
        {
            return false;
        }

        // Register the list of result on the activation.  We need to do that
        // before we initialize the list such as if any recover instruction
        // cause a GC, we can ensure that the results are properly traced by the
        // activation.
        RInstructionResults tmp(fallback.frame->jsFrame());
        if (!fallback.activation->registerIonFrameRecovery(mozilla::Move(tmp)))
            return false;

        results = fallback.activation->maybeIonFrameRecovery(fp);

        // Start a new snapshot at the beginning of the JitFrameIterator.  This
        // SnapshotIterator is used for evaluating the content of all recover
        // instructions.  The result is then saved on the JitActivation.
        MachineState machine = fallback.frame->machineState();
        SnapshotIterator s(*fallback.frame, &machine);
        if (!s.computeInstructionResults(cx, results)) {

            // If the evaluation failed because of OOMs, then we discard the
            // current set of result that we collected so far.
            fallback.activation->removeIonFrameRecovery(fp);
            return false;
        }
    }

    MOZ_ASSERT(results->isInitialized());
    MOZ_ASSERT(results->length() == recover_.numInstructions() - 1);
    instructionResults_ = results;
    return true;
}

bool
SnapshotIterator::computeInstructionResults(JSContext* cx, RInstructionResults* results) const
{
    MOZ_ASSERT(!results->isInitialized());
    MOZ_ASSERT(recover_.numInstructionsRead() == 1);

    // The last instruction will always be a resume point.
    size_t numResults = recover_.numInstructions() - 1;
    if (!results->isInitialized()) {
        if (!results->init(cx, numResults))
            return false;

        // No need to iterate over the only resume point.
        if (!numResults) {
            MOZ_ASSERT(results->isInitialized());
            return true;
        }

        // Use AutoEnterAnalysis to avoid invoking the object metadata callback,
        // which could try to walk the stack while bailing out.
        AutoEnterAnalysis enter(cx);

        // Fill with the results of recover instructions.
        SnapshotIterator s(*this);
        s.instructionResults_ = results;
        while (s.moreInstructions()) {
            // Skip resume point and only interpret recover instructions.
            if (s.instruction()->isResumePoint()) {
                s.skipInstruction();
                continue;
            }

            if (!s.instruction()->recover(cx, s))
                return false;
            s.nextInstruction();
        }
    }

    MOZ_ASSERT(results->isInitialized());
    return true;
}

void
SnapshotIterator::storeInstructionResult(Value v)
{
    uint32_t currIns = recover_.numInstructionsRead() - 1;
    MOZ_ASSERT((*instructionResults_)[currIns].isMagic(JS_ION_BAILOUT));
    (*instructionResults_)[currIns] = v;
}

Value
SnapshotIterator::fromInstructionResult(uint32_t index) const
{
    MOZ_ASSERT(!(*instructionResults_)[index].isMagic(JS_ION_BAILOUT));
    return (*instructionResults_)[index];
}

void
SnapshotIterator::settleOnFrame()
{
    // Check that the current instruction can still be use.
    MOZ_ASSERT(snapshot_.numAllocationsRead() == 0);
    while (!instruction()->isResumePoint())
        skipInstruction();
}

void
SnapshotIterator::nextFrame()
{
    nextInstruction();
    settleOnFrame();
}

Value
SnapshotIterator::maybeReadAllocByIndex(size_t index)
{
    while (index--) {
        MOZ_ASSERT(moreAllocations());
        skip();
    }

    Value s;
    {
        // This MaybeReadFallback method cannot GC.
        JS::AutoSuppressGCAnalysis nogc;
        MaybeReadFallback fallback(UndefinedValue());
        s = maybeRead(fallback);
    }

    while (moreAllocations())
        skip();

    return s;
}

JitFrameLayout*
JitFrameIterator::jsFrame() const
{
    MOZ_ASSERT(isScripted());
    if (isBailoutJS())
        return (JitFrameLayout*) activation_->bailoutData()->fp();

    return (JitFrameLayout*) fp();
}

IonScript*
JitFrameIterator::ionScript() const
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
JitFrameIterator::ionScriptFromCalleeToken() const
{
    MOZ_ASSERT(isIonJS());
    MOZ_ASSERT(!checkInvalidation());
    return script()->ionScript();
}

const SafepointIndex*
JitFrameIterator::safepoint() const
{
    MOZ_ASSERT(isIonJS());
    if (!cachedSafepointIndex_)
        cachedSafepointIndex_ = ionScript()->getSafepointIndex(returnAddressToFp());
    return cachedSafepointIndex_;
}

SnapshotOffset
JitFrameIterator::snapshotOffset() const
{
    MOZ_ASSERT(isIonScripted());
    if (isBailoutJS())
        return activation_->bailoutData()->snapshotOffset();
    return osiIndex()->snapshotOffset();
}

const OsiIndex*
JitFrameIterator::osiIndex() const
{
    MOZ_ASSERT(isIonJS());
    SafepointReader reader(ionScript(), safepoint());
    return ionScript()->getOsiIndex(reader.osiReturnPointOffset());
}

InlineFrameIterator::InlineFrameIterator(JSContext* cx, const JitFrameIterator* iter)
  : calleeTemplate_(cx),
    calleeRVA_(),
    script_(cx)
{
    resetOn(iter);
}

InlineFrameIterator::InlineFrameIterator(JSRuntime* rt, const JitFrameIterator* iter)
  : calleeTemplate_(rt),
    calleeRVA_(),
    script_(rt)
{
    resetOn(iter);
}

InlineFrameIterator::InlineFrameIterator(JSContext* cx, const InlineFrameIterator* iter)
  : frame_(iter ? iter->frame_ : nullptr),
    framesRead_(0),
    frameCount_(iter ? iter->frameCount_ : UINT32_MAX),
    calleeTemplate_(cx),
    calleeRVA_(),
    script_(cx)
{
    if (frame_) {
        machine_ = iter->machine_;
        start_ = SnapshotIterator(*frame_, &machine_);

        // findNextFrame will iterate to the next frame and init. everything.
        // Therefore to settle on the same frame, we report one frame less readed.
        framesRead_ = iter->framesRead_ - 1;
        findNextFrame();
    }
}

void
InlineFrameIterator::resetOn(const JitFrameIterator* iter)
{
    frame_ = iter;
    framesRead_ = 0;
    frameCount_ = UINT32_MAX;

    if (iter) {
        machine_ = iter->machineState();
        start_ = SnapshotIterator(*iter, &machine_);
        findNextFrame();
    }
}

void
InlineFrameIterator::findNextFrame()
{
    MOZ_ASSERT(more());

    si_ = start_;

    // Read the initial frame out of the C stack.
    calleeTemplate_ = frame_->maybeCallee();
    calleeRVA_ = RValueAllocation();
    script_ = frame_->script();
    MOZ_ASSERT(script_->hasBaselineScript());

    // Settle on the outermost frame without evaluating any instructions before
    // looking for a pc.
    si_.settleOnFrame();

    pc_ = script_->offsetToPC(si_.pcOffset());
    numActualArgs_ = 0xbadbad;

    // This unfortunately is O(n*m), because we must skip over outer frames
    // before reading inner ones.

    // The first time (frameCount_ == UINT32_MAX) we do not know the number of
    // frames that we are going to inspect.  So we are iterating until there is
    // no more frames, to settle on the inner most frame and to count the number
    // of frames.
    size_t remaining = (frameCount_ != UINT32_MAX) ? frameNo() - 1 : SIZE_MAX;

    size_t i = 1;
    for (; i <= remaining && si_.moreFrames(); i++) {
        MOZ_ASSERT(IsIonInlinablePC(pc_));

        // Recover the number of actual arguments from the script.
        if (JSOp(*pc_) != JSOP_FUNAPPLY)
            numActualArgs_ = GET_ARGC(pc_);
        if (JSOp(*pc_) == JSOP_FUNCALL) {
            MOZ_ASSERT(GET_ARGC(pc_) > 0);
            numActualArgs_ = GET_ARGC(pc_) - 1;
        } else if (IsGetPropPC(pc_)) {
            numActualArgs_ = 0;
        } else if (IsSetPropPC(pc_)) {
            numActualArgs_ = 1;
        }

        if (numActualArgs_ == 0xbadbad)
            MOZ_CRASH("Couldn't deduce the number of arguments of an ionmonkey frame");

        // Skip over non-argument slots, as well as |this|.
        bool skipNewTarget = JSOp(*pc_) == JSOP_NEW;
        unsigned skipCount = (si_.numAllocations() - 1) - numActualArgs_ - 1 - skipNewTarget;
        for (unsigned j = 0; j < skipCount; j++)
            si_.skip();

        // This value should correspond to the function which is being inlined.
        // The value must be readable to iterate over the inline frame. Most of
        // the time, these functions are stored as JSFunction constants,
        // register which are holding the JSFunction pointer, or recover
        // instruction with Default value.
        Value funval = si_.readWithDefault(&calleeRVA_);

        // Skip extra value allocations.
        while (si_.moreAllocations())
            si_.skip();

        si_.nextFrame();

        calleeTemplate_ = &funval.toObject().as<JSFunction>();

        // Inlined functions may be clones that still point to the lazy script
        // for the executed script, if they are clones. The actual script
        // exists though, just make sure the function points to it.
        script_ = calleeTemplate_->existingScriptForInlinedFunction();
        MOZ_ASSERT(script_->hasBaselineScript());

        pc_ = script_->offsetToPC(si_.pcOffset());
    }

    // The first time we do not know the number of frames, we only settle on the
    // last frame, and update the number of frames based on the number of
    // iteration that we have done.
    if (frameCount_ == UINT32_MAX) {
        MOZ_ASSERT(!si_.moreFrames());
        frameCount_ = i;
    }

    framesRead_++;
}

JSFunction*
InlineFrameIterator::callee(MaybeReadFallback& fallback) const
{
    MOZ_ASSERT(isFunctionFrame());
    if (calleeRVA_.mode() == RValueAllocation::INVALID || !fallback.canRecoverResults())
        return calleeTemplate_;

    SnapshotIterator s(si_);
    // :TODO: Handle allocation failures from recover instruction.
    Value funval = s.maybeRead(calleeRVA_, fallback);
    return &funval.toObject().as<JSFunction>();
}

JSObject*
InlineFrameIterator::computeScopeChain(Value scopeChainValue, MaybeReadFallback& fallback,
                                       bool* hasCallObj) const
{
    if (scopeChainValue.isObject()) {
        if (hasCallObj) {
            if (fallback.canRecoverResults()) {
                RootedObject obj(fallback.maybeCx, &scopeChainValue.toObject());
                *hasCallObj = isFunctionFrame() && callee(fallback)->needsCallObject();
                return obj;
            } else {
                JS::AutoSuppressGCAnalysis nogc; // If we cannot recover then we cannot GC.
                *hasCallObj = isFunctionFrame() && callee(fallback)->needsCallObject();
            }
        }

        return &scopeChainValue.toObject();
    }

    // Note we can hit this case even for functions with a CallObject, in case
    // we are walking the frame during the function prologue, before the scope
    // chain has been initialized.
    if (isFunctionFrame())
        return callee(fallback)->environment();

    // Ion does not handle non-function scripts that have anything other than
    // the global on their scope chain.
    MOZ_ASSERT(!script()->isForEval());
    MOZ_ASSERT(!script()->hasNonSyntacticScope());
    return &script()->global().lexicalScope();
}

bool
InlineFrameIterator::isFunctionFrame() const
{
    return !!calleeTemplate_;
}

MachineState
MachineState::FromBailout(RegisterDump::GPRArray& regs, RegisterDump::FPUArray& fpregs)
{
    MachineState machine;

    for (unsigned i = 0; i < Registers::Total; i++)
        machine.setRegisterLocation(Register::FromCode(i), &regs[i].r);
#ifdef JS_CODEGEN_ARM
    float* fbase = (float*)&fpregs[0];
    for (unsigned i = 0; i < FloatRegisters::TotalDouble; i++)
        machine.setRegisterLocation(FloatRegister(i, FloatRegister::Double), &fpregs[i].d);
    for (unsigned i = 0; i < FloatRegisters::TotalSingle; i++)
        machine.setRegisterLocation(FloatRegister(i, FloatRegister::Single), (double*)&fbase[i]);
#elif defined(JS_CODEGEN_MIPS32)
    float* fbase = (float*)&fpregs[0];
    for (unsigned i = 0; i < FloatRegisters::TotalDouble; i++) {
        machine.setRegisterLocation(FloatRegister::FromIndex(i, FloatRegister::Double),
                                    &fpregs[i].d);
    }
    for (unsigned i = 0; i < FloatRegisters::TotalSingle; i++) {
        machine.setRegisterLocation(FloatRegister::FromIndex(i, FloatRegister::Single),
                                    (double*)&fbase[i]);
    }
#elif defined(JS_CODEGEN_MIPS64)
    for (unsigned i = 0; i < FloatRegisters::TotalPhys; i++) {
        machine.setRegisterLocation(FloatRegister(i, FloatRegisters::Double), &fpregs[i]);
        machine.setRegisterLocation(FloatRegister(i, FloatRegisters::Single), &fpregs[i]);
    }
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    for (unsigned i = 0; i < FloatRegisters::TotalPhys; i++) {
        machine.setRegisterLocation(FloatRegister(i, FloatRegisters::Single), &fpregs[i]);
        machine.setRegisterLocation(FloatRegister(i, FloatRegisters::Double), &fpregs[i]);
        machine.setRegisterLocation(FloatRegister(i, FloatRegisters::Simd128), &fpregs[i]);
    }
#elif defined(JS_CODEGEN_ARM64)
    for (unsigned i = 0; i < FloatRegisters::TotalPhys; i++) {
        machine.setRegisterLocation(FloatRegister(i, FloatRegisters::Single), &fpregs[i]);
        machine.setRegisterLocation(FloatRegister(i, FloatRegisters::Double), &fpregs[i]);
    }

#elif defined(JS_CODEGEN_NONE)
    MOZ_CRASH();
#else
# error "Unknown architecture!"
#endif
    return machine;
}

bool
InlineFrameIterator::isConstructing() const
{
    // Skip the current frame and look at the caller's.
    if (more()) {
        InlineFrameIterator parent(GetJSContextFromJitCode(), this);
        ++parent;

        // Inlined Getters and Setters are never constructing.
        if (IsGetPropPC(parent.pc()) || IsSetPropPC(parent.pc()))
            return false;

        // In the case of a JS frame, look up the pc from the snapshot.
        MOZ_ASSERT(IsCallPC(parent.pc()));

        return (JSOp)*parent.pc() == JSOP_NEW;
    }

    return frame_->isConstructing();
}

bool
JitFrameIterator::isConstructing() const
{
    return CalleeTokenIsConstructing(calleeToken());
}

unsigned
JitFrameIterator::numActualArgs() const
{
    if (isScripted())
        return jsFrame()->numActualArgs();

    MOZ_ASSERT(isExitFrameLayout<NativeExitFrameLayout>());
    return exitFrame()->as<NativeExitFrameLayout>()->argc();
}

void
SnapshotIterator::warnUnreadableAllocation()
{
    fprintf(stderr, "Warning! Tried to access unreadable value allocation (possible f.arguments).\n");
}

struct DumpOp {
    explicit DumpOp(unsigned int i) : i_(i) {}

    unsigned int i_;
    void operator()(const Value& v) {
        fprintf(stderr, "  actual (arg %d): ", i_);
#ifdef DEBUG
        DumpValue(v);
#else
        fprintf(stderr, "?\n");
#endif
        i_++;
    }
};

void
JitFrameIterator::dumpBaseline() const
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

    fprintf(stderr, "  file %s line %" PRIuSIZE "\n",
            script()->filename(), script()->lineno());

    JSContext* cx = GetJSContextFromJitCode();
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
InlineFrameIterator::dump() const
{
    MaybeReadFallback fallback(UndefinedValue());

    if (more())
        fprintf(stderr, " JS frame (inlined)\n");
    else
        fprintf(stderr, " JS frame\n");

    bool isFunction = false;
    if (isFunctionFrame()) {
        isFunction = true;
        fprintf(stderr, "  callee fun: ");
#ifdef DEBUG
        DumpObject(callee(fallback));
#else
        fprintf(stderr, "?\n");
#endif
    } else {
        fprintf(stderr, "  global frame, no callee\n");
    }

    fprintf(stderr, "  file %s line %" PRIuSIZE "\n",
            script()->filename(), script()->lineno());

    fprintf(stderr, "  script = %p, pc = %p\n", (void*) script(), pc());
    fprintf(stderr, "  current op: %s\n", CodeName[*pc()]);

    if (!more()) {
        numActualArgs();
    }

    SnapshotIterator si = snapshotIterator();
    fprintf(stderr, "  slots: %u\n", si.numAllocations() - 1);
    for (unsigned i = 0; i < si.numAllocations() - 1; i++) {
        if (isFunction) {
            if (i == 0)
                fprintf(stderr, "  scope chain: ");
            else if (i == 1)
                fprintf(stderr, "  this: ");
            else if (i - 2 < calleeTemplate()->nargs())
                fprintf(stderr, "  formal (arg %d): ", i - 2);
            else {
                if (i - 2 == calleeTemplate()->nargs() && numActualArgs() > calleeTemplate()->nargs()) {
                    DumpOp d(calleeTemplate()->nargs());
                    unaliasedForEachActual(GetJSContextFromJitCode(), d, ReadFrame_Overflown, fallback);
                }

                fprintf(stderr, "  slot %d: ", int(i - 2 - calleeTemplate()->nargs()));
            }
        } else
            fprintf(stderr, "  slot %u: ", i);
#ifdef DEBUG
        DumpValue(si.maybeRead(fallback));
#else
        fprintf(stderr, "?\n");
#endif
    }

    fputc('\n', stderr);
}

void
JitFrameIterator::dump() const
{
    switch (type_) {
      case JitFrame_Entry:
        fprintf(stderr, " Entry frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_BaselineJS:
        dumpBaseline();
        break;
      case JitFrame_BaselineStub:
      case JitFrame_Unwound_BaselineStub:
        fprintf(stderr, " Baseline stub frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_Bailout:
      case JitFrame_IonJS:
      {
        InlineFrameIterator frames(GetJSContextFromJitCode(), this);
        for (;;) {
            frames.dump();
            if (!frames.more())
                break;
            ++frames;
        }
        break;
      }
      case JitFrame_IonStub:
      case JitFrame_Unwound_IonStub:
        fprintf(stderr, " Ion stub frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_Rectifier:
      case JitFrame_Unwound_Rectifier:
        fprintf(stderr, " Rectifier frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_IonAccessorIC:
      case JitFrame_Unwound_IonAccessorIC:
        fprintf(stderr, " Ion scripted accessor IC\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_Unwound_IonJS:
      case JitFrame_Unwound_BaselineJS:
        fprintf(stderr, "Warning! Unwound JS frames are not observable.\n");
        break;
      case JitFrame_Exit:
        fprintf(stderr, " Exit frame\n");
        break;
      case JitFrame_LazyLink:
        fprintf(stderr, " Lazy link frame\n");
        break;
    };
    fputc('\n', stderr);
}

#ifdef DEBUG
bool
JitFrameIterator::verifyReturnAddressUsingNativeToBytecodeMap()
{
    MOZ_ASSERT(returnAddressToFp_ != nullptr);

    // Only handle Ion frames for now.
    if (type_ != JitFrame_IonJS && type_ != JitFrame_BaselineJS)
        return true;

    JSRuntime* rt = js::TlsPerThreadData.get()->runtimeIfOnOwnerThread();

    // Don't verify on non-main-thread.
    if (!rt)
        return true;

    // Don't verify if sampling is being suppressed.
    if (!rt->isProfilerSamplingEnabled())
        return true;

    if (rt->isHeapMinorCollecting())
        return true;

    JitRuntime* jitrt = rt->jitRuntime();

    // Look up and print bytecode info for the native address.
    JitcodeGlobalEntry entry;
    if (!jitrt->getJitcodeGlobalTable()->lookup(returnAddressToFp_, &entry, rt))
        return true;

    JitSpew(JitSpew_Profiling, "Found nativeToBytecode entry for %p: %p - %p",
            returnAddressToFp_, entry.nativeStartAddr(), entry.nativeEndAddr());

    JitcodeGlobalEntry::BytecodeLocationVector location;
    uint32_t depth = UINT32_MAX;
    if (!entry.callStackAtAddr(rt, returnAddressToFp_, location, &depth))
        return false;
    MOZ_ASSERT(depth > 0 && depth != UINT32_MAX);
    MOZ_ASSERT(location.length() == depth);

    JitSpew(JitSpew_Profiling, "Found bytecode location of depth %d:", depth);
    for (size_t i = 0; i < location.length(); i++) {
        JitSpew(JitSpew_Profiling, "   %s:%" PRIuSIZE " - %" PRIuSIZE,
                location[i].script->filename(), location[i].script->lineno(),
                size_t(location[i].pc - location[i].script->code()));
    }

    if (type_ == JitFrame_IonJS) {
        // Create an InlineFrameIterator here and verify the mapped info against the iterator info.
        InlineFrameIterator inlineFrames(GetJSContextFromJitCode(), this);
        for (size_t idx = 0; idx < location.length(); idx++) {
            MOZ_ASSERT(idx < location.length());
            MOZ_ASSERT_IF(idx < location.length() - 1, inlineFrames.more());

            JitSpew(JitSpew_Profiling,
                    "Match %d: ION %s:%" PRIuSIZE "(%" PRIuSIZE ") vs N2B %s:%" PRIuSIZE "(%" PRIuSIZE ")",
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

JitProfilingFrameIterator::JitProfilingFrameIterator(
        JSRuntime* rt, const JS::ProfilingFrameIterator::RegisterState& state)
{
    // If no profilingActivation is live, initialize directly to
    // end-of-iteration state.
    if (!rt->profilingActivation()) {
        type_ = JitFrame_Entry;
        fp_ = nullptr;
        returnAddressToFp_ = nullptr;
        return;
    }

    MOZ_ASSERT(rt->profilingActivation()->isJit());

    JitActivation* act = rt->profilingActivation()->asJit();

    // If the top JitActivation has a null lastProfilingFrame, assume that
    // it's a trivially empty activation, and initialize directly
    // to end-of-iteration state.
    if (!act->lastProfilingFrame()) {
        type_ = JitFrame_Entry;
        fp_ = nullptr;
        returnAddressToFp_ = nullptr;
        return;
    }

    // Get the fp from the current profilingActivation
    fp_ = (uint8_t*) act->lastProfilingFrame();
    void* lastCallSite = act->lastProfilingCallSite();

    JitcodeGlobalTable* table = rt->jitRuntime()->getJitcodeGlobalTable();

    // Profiler sampling must NOT be suppressed if we are here.
    MOZ_ASSERT(rt->isProfilerSamplingEnabled());

    // Try initializing with sampler pc
    if (tryInitWithPC(state.pc))
        return;

    // Try initializing with sampler pc using native=>bytecode table.
    if (tryInitWithTable(table, state.pc, rt, /* forLastCallSite = */ false))
        return;

    // Try initializing with lastProfilingCallSite pc
    if (lastCallSite) {
        if (tryInitWithPC(lastCallSite))
            return;

        // Try initializing with lastProfilingCallSite pc using native=>bytecode table.
        if (tryInitWithTable(table, lastCallSite, rt, /* forLastCallSite = */ true))
            return;
    }

    // In some rare cases (e.g. baseline eval frame), the callee script may
    // not have a baselineScript.  Treat this is an empty frame-sequence and
    // move on.
    if (!frameScript()->hasBaselineScript()) {
        type_ = JitFrame_Entry;
        fp_ = nullptr;
        returnAddressToFp_ = nullptr;
        return;
    }

    // If nothing matches, for now just assume we are at the start of the last frame's
    // baseline jit code.
    type_ = JitFrame_BaselineJS;
    returnAddressToFp_ = frameScript()->baselineScript()->method()->raw();
}

template <typename FrameType, typename ReturnType=CommonFrameLayout*>
inline ReturnType
GetPreviousRawFrame(FrameType* frame)
{
    size_t prevSize = frame->prevFrameLocalSize() + FrameType::Size();
    return ReturnType(((uint8_t*) frame) + prevSize);
}

template <typename ReturnType=CommonFrameLayout*>
inline ReturnType
GetPreviousRawFrameOfExitFrame(ExitFrameLayout* frame)
{
    // Unwound exit frames are fake exit frames, and have the size of a
    // JitFrameLayout instead of ExitFrameLayout. See
    // JitFrameIterator::prevFp.
    size_t frameSize = IsUnwoundFrame(frame->prevType())
                       ? JitFrameLayout::Size()
                       : ExitFrameLayout::Size();
    size_t prevSize = frame->prevFrameLocalSize() + frameSize;
    return ReturnType(((uint8_t*) frame) + prevSize);
}

JitProfilingFrameIterator::JitProfilingFrameIterator(void* exitFrame)
{
    ExitFrameLayout* frame = (ExitFrameLayout*) exitFrame;
    FrameType prevType = frame->prevType();

    if (prevType == JitFrame_IonJS || prevType == JitFrame_Unwound_IonJS) {
        returnAddressToFp_ = frame->returnAddress();
        fp_ = GetPreviousRawFrameOfExitFrame<uint8_t*>(frame);
        type_ = JitFrame_IonJS;
        return;
    }

    if (prevType == JitFrame_BaselineJS || prevType == JitFrame_Unwound_BaselineJS) {
        returnAddressToFp_ = frame->returnAddress();
        fp_ = GetPreviousRawFrameOfExitFrame<uint8_t*>(frame);
        type_ = JitFrame_BaselineJS;
        fixBaselineDebugModeOSRReturnAddress();
        return;
    }

    if (prevType == JitFrame_BaselineStub || prevType == JitFrame_Unwound_BaselineStub) {
        BaselineStubFrameLayout* stubFrame =
            GetPreviousRawFrameOfExitFrame<BaselineStubFrameLayout*>(frame);
        MOZ_ASSERT_IF(prevType == JitFrame_BaselineStub,
                      stubFrame->prevType() == JitFrame_BaselineJS);
        MOZ_ASSERT_IF(prevType == JitFrame_Unwound_BaselineStub,
                      stubFrame->prevType() == JitFrame_BaselineJS ||
                      stubFrame->prevType() == JitFrame_IonJS);
        returnAddressToFp_ = stubFrame->returnAddress();
        fp_ = ((uint8_t*) stubFrame->reverseSavedFramePtr())
                + jit::BaselineFrame::FramePointerOffset;
        type_ = JitFrame_BaselineJS;
        return;
    }

    if (prevType == JitFrame_Unwound_Rectifier) {
        // Unwound rectifier exit frames still keep their 'JS' format (with
        // the target function and actual-args included in the frame and not
        // counted in the frame size).
        RectifierFrameLayout* rectFrame =
            GetPreviousRawFrame<JitFrameLayout, RectifierFrameLayout*>((JitFrameLayout*) frame);

        MOZ_ASSERT(rectFrame->prevType() == JitFrame_BaselineStub ||
                   rectFrame->prevType() == JitFrame_IonJS);

        if (rectFrame->prevType() == JitFrame_BaselineStub) {
            // Unwind past stub frame.
            BaselineStubFrameLayout* stubFrame =
                GetPreviousRawFrame<RectifierFrameLayout, BaselineStubFrameLayout*>(rectFrame);
            MOZ_ASSERT(stubFrame->prevType() == JitFrame_BaselineJS);
            returnAddressToFp_ = stubFrame->returnAddress();
            fp_ = ((uint8_t*) stubFrame->reverseSavedFramePtr())
                    + jit::BaselineFrame::FramePointerOffset;
            type_ = JitFrame_BaselineJS;
            return;
        }

        // else, prior frame was ion frame.
        returnAddressToFp_ = rectFrame->returnAddress();
        fp_ = GetPreviousRawFrame<RectifierFrameLayout, uint8_t*>(rectFrame);
        type_ = JitFrame_IonJS;
        return;
    }

    MOZ_CRASH("Invalid frame type prior to exit frame.");
}

bool
JitProfilingFrameIterator::tryInitWithPC(void* pc)
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
JitProfilingFrameIterator::tryInitWithTable(JitcodeGlobalTable* table, void* pc, JSRuntime* rt,
                                            bool forLastCallSite)
{
    if (!pc)
        return false;

    JitcodeGlobalEntry entry;
    if (!table->lookup(pc, &entry, rt))
        return false;

    JSScript* callee = frameScript();

    MOZ_ASSERT(entry.isIon() || entry.isBaseline() || entry.isIonCache() || entry.isDummy());

    // Treat dummy lookups as an empty frame sequence.
    if (entry.isDummy()) {
        type_ = JitFrame_Entry;
        fp_ = nullptr;
        returnAddressToFp_ = nullptr;
        return true;
    }

    if (entry.isIon()) {
        // If looked-up callee doesn't match frame callee, don't accept lastProfilingCallSite
        if (entry.ionEntry().getScript(0) != callee)
            return false;

        type_ = JitFrame_IonJS;
        returnAddressToFp_ = pc;
        return true;
    }

    if (entry.isBaseline()) {
        // If looked-up callee doesn't match frame callee, don't accept lastProfilingCallSite
        if (forLastCallSite && entry.baselineEntry().script() != callee)
            return false;

        type_ = JitFrame_BaselineJS;
        returnAddressToFp_ = pc;
        return true;
    }

    if (entry.isIonCache()) {
        JitcodeGlobalEntry ionEntry;
        table->lookupInfallible(entry.ionCacheEntry().rejoinAddr(), &ionEntry, rt);
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
JitProfilingFrameIterator::fixBaselineDebugModeOSRReturnAddress()
{
    MOZ_ASSERT(type_ == JitFrame_BaselineJS);
    BaselineFrame* bl = (BaselineFrame*)(fp_ - BaselineFrame::FramePointerOffset -
                                         BaselineFrame::Size());
    if (BaselineDebugModeOSRInfo* info = bl->getDebugModeOSRInfo())
        returnAddressToFp_ = info->resumeAddr;
}

void
JitProfilingFrameIterator::operator++()
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
     * ^--- Argument Rectifier
     * |    ^
     * |    |
     * |    ^--- Ion
     * |    |
     * |    ^--- Baseline Stub <---- Baseline
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
    JitFrameLayout* frame = framePtr();
    FrameType prevType = frame->prevType();

    if (prevType == JitFrame_IonJS || prevType == JitFrame_Unwound_IonJS) {
        returnAddressToFp_ = frame->returnAddress();
        fp_ = GetPreviousRawFrame<JitFrameLayout, uint8_t*>(frame);
        type_ = JitFrame_IonJS;
        return;
    }

    if (prevType == JitFrame_BaselineJS || prevType == JitFrame_Unwound_BaselineJS) {
        returnAddressToFp_ = frame->returnAddress();
        fp_ = GetPreviousRawFrame<JitFrameLayout, uint8_t*>(frame);
        type_ = JitFrame_BaselineJS;
        fixBaselineDebugModeOSRReturnAddress();
        return;
    }

    if (prevType == JitFrame_BaselineStub || prevType == JitFrame_Unwound_BaselineStub) {
        BaselineStubFrameLayout* stubFrame =
            GetPreviousRawFrame<JitFrameLayout, BaselineStubFrameLayout*>(frame);
        MOZ_ASSERT(stubFrame->prevType() == JitFrame_BaselineJS);

        returnAddressToFp_ = stubFrame->returnAddress();
        fp_ = ((uint8_t*) stubFrame->reverseSavedFramePtr())
                + jit::BaselineFrame::FramePointerOffset;
        type_ = JitFrame_BaselineJS;
        return;
    }

    if (prevType == JitFrame_Rectifier || prevType == JitFrame_Unwound_Rectifier) {
        RectifierFrameLayout* rectFrame =
            GetPreviousRawFrame<JitFrameLayout, RectifierFrameLayout*>(frame);
        FrameType rectPrevType = rectFrame->prevType();

        if (rectPrevType == JitFrame_IonJS) {
            returnAddressToFp_ = rectFrame->returnAddress();
            fp_ = GetPreviousRawFrame<JitFrameLayout, uint8_t*>(rectFrame);
            type_ = JitFrame_IonJS;
            return;
        }

        if (rectPrevType == JitFrame_BaselineStub) {
            BaselineStubFrameLayout* stubFrame =
                GetPreviousRawFrame<JitFrameLayout, BaselineStubFrameLayout*>(rectFrame);
            returnAddressToFp_ = stubFrame->returnAddress();
            fp_ = ((uint8_t*) stubFrame->reverseSavedFramePtr())
                    + jit::BaselineFrame::FramePointerOffset;
            type_ = JitFrame_BaselineJS;
            return;
        }

        MOZ_CRASH("Bad frame type prior to rectifier frame.");
    }

    if (prevType == JitFrame_IonAccessorIC || prevType == JitFrame_Unwound_IonAccessorIC) {
        IonAccessorICFrameLayout* accessorFrame =
            GetPreviousRawFrame<JitFrameLayout, IonAccessorICFrameLayout*>(frame);

        MOZ_ASSERT(accessorFrame->prevType() == JitFrame_IonJS);

        returnAddressToFp_ = accessorFrame->returnAddress();
        fp_ = GetPreviousRawFrame<IonAccessorICFrameLayout, uint8_t*>(accessorFrame);
        type_ = JitFrame_IonJS;
        return;
    }

    if (prevType == JitFrame_Entry) {
        // No previous frame, set to null to indicate that JitFrameIterator is done()
        returnAddressToFp_ = nullptr;
        fp_ = nullptr;
        type_ = JitFrame_Entry;
        return;
    }

    MOZ_CRASH("Bad frame type.");
}

JitFrameLayout*
InvalidationBailoutStack::fp() const
{
    return (JitFrameLayout*) (sp() + ionScript_->frameSize());
}

void
InvalidationBailoutStack::checkInvariants() const
{
#ifdef DEBUG
    JitFrameLayout* frame = fp();
    CalleeToken token = frame->calleeToken();
    MOZ_ASSERT(token);

    uint8_t* rawBase = ionScript()->method()->raw();
    uint8_t* rawLimit = rawBase + ionScript()->method()->instructionsSize();
    uint8_t* osiPoint = osiPointReturnAddress();
    MOZ_ASSERT(rawBase <= osiPoint && osiPoint <= rawLimit);
#endif
}

void
AssertJitStackInvariants(JSContext* cx)
{
    for (JitActivationIterator activations(cx->runtime()); !activations.done(); ++activations) {
        JitFrameIterator frames(activations);
        size_t prevFrameSize = 0;
        size_t frameSize = 0;
        bool isScriptedCallee = false;
        for (; !frames.done(); ++frames) {
            size_t calleeFp = reinterpret_cast<size_t>(frames.fp());
            size_t callerFp = reinterpret_cast<size_t>(frames.prevFp());
            MOZ_ASSERT(callerFp >= calleeFp);
            prevFrameSize = frameSize;
            frameSize = callerFp - calleeFp;

            if (frames.prevType() == JitFrame_Rectifier) {
                MOZ_RELEASE_ASSERT(frameSize % JitStackAlignment == 0,
                  "The rectifier frame should keep the alignment");

                size_t expectedFrameSize = 0
#if defined(JS_CODEGEN_X86)
                    + sizeof(void*) /* frame pointer */
#endif
                    + sizeof(Value) * (frames.callee()->nargs() +
                                       1 /* |this| argument */ +
                                       frames.isConstructing() /* new.target */)
                    + sizeof(JitFrameLayout);
                MOZ_RELEASE_ASSERT(frameSize >= expectedFrameSize,
                  "The frame is large enough to hold all arguments");
                MOZ_RELEASE_ASSERT(expectedFrameSize + JitStackAlignment > frameSize,
                  "The frame size is optimal");
            }

            if (frames.isExitFrame()) {
                // For the moment, we do not keep the JitStackAlignment
                // alignment for exit frames.
                frameSize -= ExitFrameLayout::Size();
            }

            if (frames.isIonJS()) {
                // Ideally, we should not have such requirement, but keep the
                // alignment-delta as part of the Safepoint such that we can pad
                // accordingly when making out-of-line calls.  In the mean time,
                // let us have check-points where we can garantee that
                // everything can properly be aligned before adding complexity.
                MOZ_RELEASE_ASSERT(frames.ionScript()->frameSize() % JitStackAlignment == 0,
                  "Ensure that if the Ion frame is aligned, then the spill base is also aligned");

                if (isScriptedCallee) {
                    MOZ_RELEASE_ASSERT(prevFrameSize % JitStackAlignment == 0,
                      "The ion frame should keep the alignment");
                }
            }

            // The stack is dynamically aligned by baseline stubs before calling
            // any jitted code.
            if (frames.prevType() == JitFrame_BaselineStub && isScriptedCallee) {
                MOZ_RELEASE_ASSERT(calleeFp % JitStackAlignment == 0,
                    "The baseline stub restores the stack alignment");
            }

            isScriptedCallee = false
                || frames.isScripted()
                || frames.type() == JitFrame_Rectifier;
        }

        MOZ_RELEASE_ASSERT(frames.type() == JitFrame_Entry,
          "The first frame of a Jit activation should be an entry frame");
        MOZ_RELEASE_ASSERT(reinterpret_cast<size_t>(frames.fp()) % JitStackAlignment == 0,
          "The entry frame should be properly aligned");
    }
}

} // namespace jit
} // namespace js
