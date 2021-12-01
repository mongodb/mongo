/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JitFrames-inl.h"

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
#include "vm/GeckoProfiler.h"
#include "vm/Interpreter.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/TraceLogging.h"
#include "vm/TypeInference.h"
#include "wasm/WasmBuiltins.h"

#include "gc/Nursery-inl.h"
#include "jit/JSJitFrameIter-inl.h"
#include "vm/Debugger-inl.h"
#include "vm/JSScript-inl.h"
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

static uint32_t
NumArgAndLocalSlots(const InlineFrameIterator& frame)
{
    JSScript* script = frame.script();
    return CountArgSlots(script, frame.maybeCalleeTemplate()) + script->nfixed();
}

static void
CloseLiveIteratorIon(JSContext* cx, const InlineFrameIterator& frame, JSTryNote* tn)
{
    MOZ_ASSERT(tn->kind == JSTRY_FOR_IN ||
               tn->kind == JSTRY_DESTRUCTURING_ITERCLOSE);

    bool isDestructuring = tn->kind == JSTRY_DESTRUCTURING_ITERCLOSE;
    MOZ_ASSERT_IF(!isDestructuring, tn->stackDepth > 0);
    MOZ_ASSERT_IF(isDestructuring, tn->stackDepth > 1);

    SnapshotIterator si = frame.snapshotIterator();

    // Skip stack slots until we reach the iterator object on the stack. For
    // the destructuring case, we also need to get the "done" value.
    uint32_t stackSlot = tn->stackDepth;
    uint32_t adjust = isDestructuring ? 2 : 1;
    uint32_t skipSlots = NumArgAndLocalSlots(frame) + stackSlot - adjust;

    for (unsigned i = 0; i < skipSlots; i++)
        si.skip();

    MaybeReadFallback recover(cx, cx->activation()->asJit(), &frame.frame(),
                              MaybeReadFallback::Fallback_DoNothing);
    Value v = si.maybeRead(recover);
    RootedObject iterObject(cx, &v.toObject());

    if (isDestructuring) {
        RootedValue doneValue(cx, si.read());
        bool done = ToBoolean(doneValue);
        // Do not call IteratorClose if the destructuring iterator is already
        // done.
        if (done)
            return;
    }

    if (cx->isExceptionPending()) {
        if (tn->kind == JSTRY_FOR_IN)
            CloseIterator(iterObject);
        else
            IteratorCloseForException(cx, iterObject);
    } else {
        UnwindIteratorForUncatchableException(iterObject);
    }
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
            JitActivation* act = cx->activation()->asJit();
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

    bool inForOfIterClose = false;

    for (TryNoteIterIon tni(cx, frame); !tni.done(); ++tni) {
        JSTryNote* tn = *tni;

        switch (tn->kind) {
          case JSTRY_FOR_IN:
          case JSTRY_DESTRUCTURING_ITERCLOSE:
            // See corresponding comment in ProcessTryNotes.
            if (inForOfIterClose)
                break;

            MOZ_ASSERT_IF(tn->kind == JSTRY_FOR_IN,
                          JSOp(*(script->main() + tn->start + tn->length)) == JSOP_ENDITER);
            CloseLiveIteratorIon(cx, frame, tn);
            break;

          case JSTRY_FOR_OF_ITERCLOSE:
            inForOfIterClose = true;
            break;

          case JSTRY_FOR_OF:
            inForOfIterClose = false;
            break;

          case JSTRY_LOOP:
            break;

          case JSTRY_CATCH:
            if (cx->isExceptionPending()) {
                // See corresponding comment in ProcessTryNotes.
                if (inForOfIterClose)
                    break;

                // Ion can compile try-catch, but bailing out to catch
                // exceptions is slow. Reset the warm-up counter so that if we
                // catch many exceptions we won't Ion-compile the script.
                script->resetWarmUpCounter();

                // Bailout at the start of the catch block.
                jsbytecode* catchPC = script->main() + tn->start + tn->length;
                ExceptionBailoutInfo excInfo(frame.frameNo(), catchPC, tn->stackDepth);
                uint32_t retval = ExceptionHandlerBailout(cx, frame, rfe, excInfo, overrecursed);
                if (retval == BAILOUT_RETURN_OK) {
                    // Record exception locations to allow scope unwinding in
                    // |FinishBailoutToBaseline|
                    MOZ_ASSERT(cx->isExceptionPending());
                    rfe->bailoutInfo->tryPC = UnwindEnvironmentToTryPc(frame.script(), tn);
                    rfe->bailoutInfo->faultPC = frame.pc();
                    return;
                }

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
OnLeaveBaselineFrame(JSContext* cx, const JSJitFrameIter& frame, jsbytecode* pc,
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
ForcedReturn(JSContext* cx, const JSJitFrameIter& frame, jsbytecode* pc,
             ResumeFromException* rfe)
{
    OnLeaveBaselineFrame(cx, frame, pc, rfe, true);
}

static inline void
BaselineFrameAndStackPointersFromTryNote(JSTryNote* tn, const JSJitFrameIter& frame,
                                         uint8_t** framePointer, uint8_t** stackPointer)
{
    JSScript* script = frame.baselineFrame()->script();
    *framePointer = frame.fp() - BaselineFrame::FramePointerOffset;
    *stackPointer = *framePointer - BaselineFrame::Size() -
                    (script->nfixed() + tn->stackDepth) * sizeof(Value);
}

static void
SettleOnTryNote(JSContext* cx, JSTryNote* tn, const JSJitFrameIter& frame,
                EnvironmentIter& ei, ResumeFromException* rfe, jsbytecode** pc)
{
    RootedScript script(cx, frame.baselineFrame()->script());

    // Unwind environment chain (pop block objects).
    if (cx->isExceptionPending())
        UnwindEnvironment(cx, ei, UnwindEnvironmentToTryPc(script, tn));

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
// pc parameter is updated to where the envs have been unwound to.
static void
CloseLiveIteratorsBaselineForUncatchableException(JSContext* cx, const JSJitFrameIter& frame,
                                                  jsbytecode* pc)
{
    bool inForOfIterClose = false;
    for (TryNoteIterBaseline tni(cx, frame.baselineFrame(), pc); !tni.done(); ++tni) {
        JSTryNote* tn = *tni;
        switch (tn->kind) {
          case JSTRY_FOR_IN: {
            // See corresponding comment in ProcessTryNotes.
            if (inForOfIterClose)
                break;

            uint8_t* framePointer;
            uint8_t* stackPointer;
            BaselineFrameAndStackPointersFromTryNote(tn, frame, &framePointer, &stackPointer);
            Value iterValue(*(Value*) stackPointer);
            RootedObject iterObject(cx, &iterValue.toObject());
            UnwindIteratorForUncatchableException(iterObject);
            break;
          }

          case JSTRY_FOR_OF_ITERCLOSE:
            inForOfIterClose = true;
            break;

          case JSTRY_FOR_OF:
            inForOfIterClose = false;
            break;

          default:
            break;
        }
    }
}

static bool
ProcessTryNotesBaseline(JSContext* cx, const JSJitFrameIter& frame, EnvironmentIter& ei,
                        ResumeFromException* rfe, jsbytecode** pc)
{
    RootedScript script(cx, frame.baselineFrame()->script());
    bool inForOfIterClose = false;

    for (TryNoteIterBaseline tni(cx, frame.baselineFrame(), *pc); !tni.done(); ++tni) {
        JSTryNote* tn = *tni;

        MOZ_ASSERT(cx->isExceptionPending());
        switch (tn->kind) {
          case JSTRY_CATCH: {
            // If we're closing a legacy generator, we have to skip catch
            // blocks.
            if (cx->isClosingGenerator())
                break;

            // See corresponding comment in ProcessTryNotes.
            if (inForOfIterClose)
                break;

            SettleOnTryNote(cx, tn, frame, ei, rfe, pc);

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
            // See corresponding comment in ProcessTryNotes.
            if (inForOfIterClose)
                break;

            SettleOnTryNote(cx, tn, frame, ei, rfe, pc);
            rfe->kind = ResumeFromException::RESUME_FINALLY;
            rfe->target = script->baselineScript()->nativeCodeForPC(script, *pc);
            // Drop the exception instead of leaking cross compartment data.
            if (!cx->getPendingException(MutableHandleValue::fromMarkedLocation(&rfe->exception)))
                rfe->exception = UndefinedValue();
            cx->clearPendingException();
            return true;
          }

          case JSTRY_FOR_IN: {
            // See corresponding comment in ProcessTryNotes.
            if (inForOfIterClose)
                break;

            uint8_t* framePointer;
            uint8_t* stackPointer;
            BaselineFrameAndStackPointersFromTryNote(tn, frame, &framePointer, &stackPointer);
            Value iterValue(*reinterpret_cast<Value*>(stackPointer));
            JSObject* iterObject = &iterValue.toObject();
            CloseIterator(iterObject);
            break;
          }

          case JSTRY_DESTRUCTURING_ITERCLOSE: {
            // See corresponding comment in ProcessTryNotes.
            if (inForOfIterClose)
                break;

            uint8_t* framePointer;
            uint8_t* stackPointer;
            BaselineFrameAndStackPointersFromTryNote(tn, frame, &framePointer, &stackPointer);
            RootedValue doneValue(cx, *(reinterpret_cast<Value*>(stackPointer)));
            bool done = ToBoolean(doneValue);
            if (!done) {
                Value iterValue(*(reinterpret_cast<Value*>(stackPointer) + 1));
                RootedObject iterObject(cx, &iterValue.toObject());
                if (!IteratorCloseForException(cx, iterObject)) {
                    SettleOnTryNote(cx, tn, frame, ei, rfe, pc);
                    return false;
                }
            }
            break;
          }

          case JSTRY_FOR_OF_ITERCLOSE:
            inForOfIterClose = true;
            break;

          case JSTRY_FOR_OF:
            inForOfIterClose = false;
            break;

          case JSTRY_LOOP:
            break;

          default:
            MOZ_CRASH("Invalid try note");
        }
    }
    return true;
}

static void
HandleExceptionBaseline(JSContext* cx, const JSJitFrameIter& frame, ResumeFromException* rfe,
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
            EnvironmentIter ei(cx, frame.baselineFrame(), pc);
            if (!ProcessTryNotesBaseline(cx, frame, ei, rfe, &pc))
                goto again;
            if (rfe->kind != ResumeFromException::RESUME_ENTRY_FRAME) {
                // No need to increment the PCCounts number of execution here,
                // as the interpreter increments any PCCounts if present.
                MOZ_ASSERT_IF(script->hasScriptCounts(), script->maybeGetPCCounts(pc));
                return;
            }
        }

        frameOk = HandleClosingGeneratorReturn(cx, frame.baselineFrame(), frameOk);
        frameOk = Debugger::onLeaveFrame(cx, frame.baselineFrame(), pc, frameOk);
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

        MOZ_ASSERT(cx->jitActivation == cx->profilingActivation());

        void* lastProfilingFrame = getLastProfilingFrame();
        cx->jitActivation->setLastProfilingFrame(lastProfilingFrame);
    }

    void* getLastProfilingFrame() {
        switch (rfe->kind) {
          case ResumeFromException::RESUME_ENTRY_FRAME:
          case ResumeFromException::RESUME_WASM:
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
HandleExceptionWasm(JSContext* cx, wasm::WasmFrameIter* iter, ResumeFromException* rfe)
{
    MOZ_ASSERT(cx->activation()->asJit()->hasWasmExitFP());
    rfe->kind = ResumeFromException::RESUME_WASM;
    rfe->framePointer = (uint8_t*) wasm::FailFP;
    rfe->stackPointer = (uint8_t*) wasm::HandleThrow(cx, *iter);
    MOZ_ASSERT(iter->done());
}

void
HandleException(ResumeFromException* rfe)
{
    JSContext* cx = TlsContext.get();
    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);

    AutoResetLastProfilerFrameOnReturnFromException profFrameReset(cx, rfe);

    rfe->kind = ResumeFromException::RESUME_ENTRY_FRAME;

    JitSpew(JitSpew_IonInvalidate, "handling exception");

    // Clear any Ion return override that's been set.
    // This may happen if a callVM function causes an invalidation (setting the
    // override), and then fails, bypassing the bailout handlers that would
    // otherwise clear the return override.
    if (cx->hasIonReturnOverride())
        cx->takeIonReturnOverride();

    JitActivation* activation = cx->activation()->asJit();

#ifdef CHECK_OSIPOINT_REGISTERS
    if (JitOptions.checkOsiPointRegisters)
        activation->setCheckRegs(false);
#endif

    // The Debugger onExceptionUnwind hook (reachable via
    // HandleExceptionBaseline below) may cause on-stack recompilation of
    // baseline scripts, which may patch return addresses on the stack. Since
    // JSJitFrameIter cache the previous frame's return address when
    // iterating, we need a variant here that is automatically updated should
    // on-stack recompilation occur.
    DebugModeOSRVolatileJitFrameIter iter(cx);
    while (!iter.done()) {
        if (iter.isWasm()) {
            HandleExceptionWasm(cx, &iter.asWasm(), rfe);
            if (!iter.done())
                ++iter;
            continue;
        }

        const JSJitFrameIter& frame = iter.asJSJit();

        bool overrecursed = false;
        if (frame.isIonJS()) {
            // Search each inlined frame for live iterator objects, and close
            // them.
            InlineFrameIterator frames(cx, &frame);

            // Invalidation state will be the same for all inlined scripts in the frame.
            IonScript* ionScript = nullptr;
            bool invalidated = frame.checkInvalidation(&ionScript);

#ifdef JS_TRACE_LOGGING
            if (logger && cx->compartment()->isDebuggee() && logger->enabled()) {
                logger->disable(/* force = */ true,
                                "Forcefully disabled tracelogger, due to "
                                "throwing an exception with an active Debugger "
                                "in IonMonkey.");
            }
#endif

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
                                   /* popProfilerFrame = */ false);
                if (!frames.more()) {
                    TraceLogStopEvent(logger, TraceLogger_IonMonkey);
                    TraceLogStopEvent(logger, TraceLogger_Scripts);
                    break;
                }
                ++frames;
            }

            activation->removeIonFrameRecovery(frame.jsFrame());
            if (invalidated)
                ionScript->decrementInvalidationCount(cx->runtime()->defaultFreeOp());

        } else if (frame.isBaselineJS()) {
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
            frame.baselineScriptAndPc(nullptr, &pc);
            AutoBaselineHandlingException handlingException(frame.baselineFrame(), pc);

            HandleExceptionBaseline(cx, frame, rfe, pc);

            // If we are propagating an exception through a frame with
            // on-stack recompile info, we should free the allocated
            // RecompileInfo struct before we leave this block, as we will not
            // be returning to the recompile handler.
            AutoDeleteDebugModeOSRInfo deleteDebugModeOSRInfo(frame.baselineFrame());

            if (rfe->kind != ResumeFromException::RESUME_ENTRY_FRAME &&
                rfe->kind != ResumeFromException::RESUME_FORCED_RETURN)
            {
                return;
            }

            TraceLogStopEvent(logger, TraceLogger_Baseline);
            TraceLogStopEvent(logger, TraceLogger_Scripts);

            // Unwind profiler pseudo-stack
            JSScript* script = frame.script();
            probes::ExitScript(cx, script, script->functionNonDelazifying(),
                               /* popProfilerFrame = */ false);

            if (rfe->kind == ResumeFromException::RESUME_FORCED_RETURN)
                return;
        }

        ++iter;

        if (overrecursed) {
            // We hit an overrecursion error during bailout. Report it now.
            ReportOverRecursed(cx);
        }
    }

    // Wasm sets its own value of SP in HandleExceptionWasm.
    if (iter.isJSJit())
        rfe->stackPointer = iter.asJSJit().fp();
}

// Turns a JitFrameLayout into an ExitFrameLayout. Note that it has to be a
// bare exit frame so it's ignored by TraceJitExitFrame.
void
EnsureBareExitFrame(JitActivation* act, JitFrameLayout* frame)
{
    ExitFrameLayout* exitFrame = reinterpret_cast<ExitFrameLayout*>(frame);

    if (act->jsExitFP() == (uint8_t*)frame) {
        // If we already called this function for the current frame, do
        // nothing.
        MOZ_ASSERT(exitFrame->isBareExit());
        return;
    }

#ifdef DEBUG
    JSJitFrameIter iter(act);
    while (!iter.isScripted())
        ++iter;
    MOZ_ASSERT(iter.current() == frame, "|frame| must be the top JS frame");

    MOZ_ASSERT(!!act->jsExitFP());
    MOZ_ASSERT((uint8_t*)exitFrame->footer() >= act->jsExitFP(),
               "Must have space for ExitFooterFrame before jsExitFP");
#endif

    act->setJSExitFP((uint8_t*)frame);
    exitFrame->footer()->setBareExitFrame();
    MOZ_ASSERT(exitFrame->isBareExit());
}

CalleeToken
TraceCalleeToken(JSTracer* trc, CalleeToken token)
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
ReadAllocation(const JSJitFrameIter& frame, const LAllocation* a)
{
    if (a->isGeneralReg()) {
        Register reg = a->toGeneralReg()->reg();
        return frame.machineState().read(reg);
    }
    return *frame.jsFrame()->slotRef(SafepointSlotEntry(a));
}
#endif

static void
TraceThisAndArguments(JSTracer* trc, const JSJitFrameIter& frame, JitFrameLayout* layout)
{
    // Trace |this| and any extra actual arguments for an Ion frame. Tracing
    // of formal arguments is taken care of by the frame's safepoint/snapshot,
    // except when the script might have lazy arguments or rest, in which case
    // we trace them as well. We also have to trace formals if we have a
    // LazyLink frame or an InterpreterStub frame or a special JSJit to wasm
    // frame (since wasm doesn't use snapshots).

    if (!CalleeTokenIsFunction(layout->calleeToken()))
        return;

    size_t nargs = layout->numActualArgs();
    size_t nformals = 0;

    JSFunction* fun = CalleeTokenToFunction(layout->calleeToken());
    if (frame.type() != JitFrame_JSJitToWasm &&
        !frame.isExitFrameLayout<CalledFromJitExitFrameLayout>() &&
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

    // Always trace the new.target from the frame. It's not in the snapshots.
    // +1 to pass |this|
    if (CalleeTokenIsConstructing(layout->calleeToken()))
        TraceRoot(trc, &argv[1 + newTargetOffset], "ion-newTarget");
}

#ifdef JS_NUNBOX32
static inline void
WriteAllocation(const JSJitFrameIter& frame, const LAllocation* a, uintptr_t value)
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
TraceIonJSFrame(JSTracer* trc, const JSJitFrameIter& frame)
{
    JitFrameLayout* layout = (JitFrameLayout*)frame.fp();

    layout->replaceCalleeToken(TraceCalleeToken(trc, layout->calleeToken()));

    IonScript* ionScript = nullptr;
    if (frame.checkInvalidation(&ionScript)) {
        // This frame has been invalidated, meaning that its IonScript is no
        // longer reachable through the callee token (JSFunction/JSScript->ion
        // is now nullptr or recompiled). Manually trace it here.
        IonScript::Trace(trc, ionScript);
    } else {
        ionScript = frame.ionScriptFromCalleeToken();
    }

    TraceThisAndArguments(trc, frame, frame.jsFrame());

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
    for (GeneralRegisterBackwardIterator iter(safepoint.allGprSpills()); iter.more(); ++iter) {
        --spill;
        if (gcRegs.has(*iter))
            TraceGenericPointerRoot(trc, reinterpret_cast<gc::Cell**>(spill), "ion-gc-spill");
        else if (valueRegs.has(*iter))
            TraceRoot(trc, reinterpret_cast<Value*>(spill), "ion-value-spill");
    }

#ifdef JS_NUNBOX32
    LAllocation type, payload;
    while (safepoint.getNunboxSlot(&type, &payload)) {
        JSValueTag tag = JSValueTag(ReadAllocation(frame, &type));
        uintptr_t rawPayload = ReadAllocation(frame, &payload);

        Value v = Value::fromTagAndPayload(tag, rawPayload);
        TraceRoot(trc, &v, "ion-torn-value");

        if (v != Value::fromTagAndPayload(tag, rawPayload)) {
            // GC moved the value, replace the stored payload.
            rawPayload = *v.payloadUIntPtr();
            WriteAllocation(frame, &payload, rawPayload);
        }
    }
#endif
}

static void
TraceBailoutFrame(JSTracer* trc, const JSJitFrameIter& frame)
{
    JitFrameLayout* layout = (JitFrameLayout*)frame.fp();

    layout->replaceCalleeToken(TraceCalleeToken(trc, layout->calleeToken()));

    // We have to trace the list of actual arguments, as only formal arguments
    // are represented in the Snapshot.
    TraceThisAndArguments(trc, frame, frame.jsFrame());

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
    }

}

static void
UpdateIonJSFrameForMinorGC(const JSJitFrameIter& frame)
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

    Nursery& nursery = ionScript->method()->zone()->group()->nursery();

    const SafepointIndex* si = ionScript->getSafepointIndex(frame.returnAddressToFp());
    SafepointReader safepoint(ionScript, si);

    LiveGeneralRegisterSet slotsRegs = safepoint.slotsOrElementsSpills();
    uintptr_t* spill = frame.spillBase();
    for (GeneralRegisterBackwardIterator iter(safepoint.allGprSpills()); iter.more(); ++iter) {
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
TraceBaselineStubFrame(JSTracer* trc, const JSJitFrameIter& frame)
{
    // Trace the ICStub pointer stored in the stub frame. This is necessary
    // so that we don't destroy the stub code after unlinking the stub.

    MOZ_ASSERT(frame.type() == JitFrame_BaselineStub);
    JitStubFrameLayout* layout = (JitStubFrameLayout*)frame.fp();

    if (ICStub* stub = layout->maybeStubPtr()) {
        MOZ_ASSERT(stub->makesGCCalls());
        stub->trace(trc);
    }
}

static void
TraceIonICCallFrame(JSTracer* trc, const JSJitFrameIter& frame)
{
    MOZ_ASSERT(frame.type() == JitFrame_IonICCall);
    IonICCallFrameLayout* layout = (IonICCallFrameLayout*)frame.fp();
    TraceRoot(trc, layout->stubCode(), "ion-ic-call-code");
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
TraceJitExitFrameCopiedArguments(JSTracer* trc, const VMFunction* f, ExitFooterFrame* footer)
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
TraceJitExitFrameCopiedArguments(JSTracer* trc, const VMFunction* f, ExitFooterFrame* footer)
{
    // This is NO-OP on other platforms.
}
#endif

static void
TraceJitExitFrame(JSTracer* trc, const JSJitFrameIter& frame)
{
    ExitFooterFrame* footer = frame.exitFrame()->footer();

    // This corresponds to the case where we have build a fake exit frame which
    // handles the case of a native function call. We need to trace the argument
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

    if (frame.isExitFrameLayout<CalledFromJitExitFrameLayout>()) {
        auto* layout = frame.exitFrame()->as<CalledFromJitExitFrameLayout>();
        JitFrameLayout* jsLayout = layout->jsFrame();
        jsLayout->replaceCalleeToken(TraceCalleeToken(trc, jsLayout->calleeToken()));
        TraceThisAndArguments(trc, frame, jsLayout);
        return;
    }

    if (frame.isBareExit()) {
        // Nothing to trace. Fake exit frame pushed for VM functions with
        // nothing to trace on the stack.
        return;
    }

    MOZ_ASSERT(frame.exitFrame()->isWrapperExit());

    const VMFunction* f = footer->function();
    MOZ_ASSERT(f);

    // Trace arguments of the VM wrapper.
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
            TraceRoot(trc, reinterpret_cast<JSString**>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootFunction:
            TraceRoot(trc, reinterpret_cast<JSFunction**>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootValue:
            TraceRoot(trc, reinterpret_cast<Value*>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootId:
            TraceRoot(trc, reinterpret_cast<jsid*>(argBase), "ion-vm-args");
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
            TraceRoot(trc, footer->outParam<JSString*>(), "ion-vm-out");
            break;
          case VMFunction::RootFunction:
            TraceRoot(trc, footer->outParam<JSFunction*>(), "ion-vm-out");
            break;
          case VMFunction::RootValue:
            TraceRoot(trc, footer->outParam<Value>(), "ion-vm-outvp");
            break;
          case VMFunction::RootId:
            TraceRoot(trc, footer->outParam<jsid>(), "ion-vm-outvp");
            break;
          case VMFunction::RootCell:
            TraceGenericPointerRoot(trc, footer->outParam<gc::Cell*>(), "ion-vm-out");
            break;
        }
    }

    TraceJitExitFrameCopiedArguments(trc, f, footer);
}

static void
TraceRectifierFrame(JSTracer* trc, const JSJitFrameIter& frame)
{
    // Trace thisv.
    //
    // Baseline JIT code generated as part of the ICCall_Fallback stub may use
    // it if we're calling a constructor that returns a primitive value.
    RectifierFrameLayout* layout = (RectifierFrameLayout*)frame.fp();
    TraceRoot(trc, &layout->argv()[0], "ion-thisv");
}

static void
TraceJSJitToWasmFrame(JSTracer* trc, const JSJitFrameIter& frame)
{
    // This is doing a subset of TraceIonJSFrame, since the callee doesn't
    // have a script.
    JitFrameLayout* layout = (JitFrameLayout*)frame.fp();
    layout->replaceCalleeToken(TraceCalleeToken(trc, layout->calleeToken()));
    TraceThisAndArguments(trc, frame, layout);
}

static void
TraceJitActivation(JSTracer* trc, JitActivation* activation)
{
#ifdef CHECK_OSIPOINT_REGISTERS
    if (JitOptions.checkOsiPointRegisters) {
        // GC can modify spilled registers, breaking our register checks.
        // To handle this, we disable these checks for the current VM call
        // when a GC happens.
        activation->setCheckRegs(false);
    }
#endif

    activation->traceRematerializedFrames(trc);
    activation->traceIonRecovery(trc);

    for (JitFrameIter frames(activation); !frames.done(); ++frames) {
        if (frames.isJSJit()) {
            const JSJitFrameIter& jitFrame = frames.asJSJit();
            switch (jitFrame.type()) {
              case JitFrame_Exit:
                TraceJitExitFrame(trc, jitFrame);
                break;
              case JitFrame_BaselineJS:
                jitFrame.baselineFrame()->trace(trc, jitFrame);
                break;
              case JitFrame_IonJS:
                TraceIonJSFrame(trc, jitFrame);
                break;
              case JitFrame_BaselineStub:
                TraceBaselineStubFrame(trc, jitFrame);
                break;
              case JitFrame_Bailout:
                TraceBailoutFrame(trc, jitFrame);
                break;
              case JitFrame_Rectifier:
                TraceRectifierFrame(trc, jitFrame);
                break;
              case JitFrame_IonICCall:
                TraceIonICCallFrame(trc, jitFrame);
                break;
              case JitFrame_WasmToJSJit:
                // Ignore: this is a special marker used to let the
                // JitFrameIter know the frame above is a wasm frame, handled
                // in the next iteration.
                break;
              case JitFrame_JSJitToWasm:
                TraceJSJitToWasmFrame(trc, jitFrame);
                break;
              default:
                MOZ_CRASH("unexpected frame type");
            }
        } else {
            MOZ_ASSERT(frames.isWasm());
            frames.asWasm().instance()->trace(trc);
        }
    }
}

void
TraceJitActivations(JSContext* cx, const CooperatingContext& target, JSTracer* trc)
{
    for (JitActivationIterator activations(cx, target); !activations.done(); ++activations)
        TraceJitActivation(trc, activations->asJit());
}

void
UpdateJitActivationsForMinorGC(JSRuntime* rt)
{
    MOZ_ASSERT(JS::CurrentThreadIsHeapMinorCollecting());
    JSContext* cx = TlsContext.get();
    for (const CooperatingContext& target : rt->cooperatingContexts()) {
        for (JitActivationIterator activations(cx, target); !activations.done(); ++activations) {
            for (OnlyJSJitFrameIter iter(activations); !iter.done(); ++iter) {
                if (iter.frame().type() == JitFrame_IonJS)
                    UpdateIonJSFrameForMinorGC(iter.frame());
            }
        }
    }
}

void
GetPcScript(JSContext* cx, JSScript** scriptRes, jsbytecode** pcRes)
{
    JitSpew(JitSpew_IonSnapshots, "Recover PC & Script from the last frame.");

    // Recover the return address so that we can look it up in the
    // PcScriptCache, as script/pc computation is expensive.
    JitActivationIterator actIter(cx);
    OnlyJSJitFrameIter it(actIter);
    uint8_t* retAddr;
    if (it.frame().isExitFrame()) {
        ++it;

        // Skip rectifier frames.
        if (it.frame().isRectifier()) {
            ++it;
            MOZ_ASSERT(it.frame().isBaselineStub() ||
                       it.frame().isBaselineJS() ||
                       it.frame().isIonJS());
        }

        // Skip Baseline/Ion stub and IC call frames.
        if (it.frame().isBaselineStub()) {
            ++it;
            MOZ_ASSERT(it.frame().isBaselineJS());
        } else if (it.frame().isIonICCall()) {
            ++it;
            MOZ_ASSERT(it.frame().isIonJS());
        }

        MOZ_ASSERT(it.frame().isBaselineJS() || it.frame().isIonJS());

        // Don't use the return address if the BaselineFrame has an override pc.
        // The override pc is cheap to get, so we won't benefit from the cache,
        // and the override pc could change without the return address changing.
        // Moreover, sometimes when an override pc is present during exception
        // handling, the return address is set to nullptr as a sanity check,
        // since we do not return to the frame that threw the exception.
        if (!it.frame().isBaselineJS() || !it.frame().baselineFrame()->hasOverridePc()) {
            retAddr = it.frame().returnAddressToFp();
            MOZ_ASSERT(retAddr);
        } else {
            retAddr = nullptr;
        }
    } else {
        MOZ_ASSERT(it.frame().isBailoutJS());
        retAddr = it.frame().returnAddress();
    }

    uint32_t hash;
    if (retAddr) {
        hash = PcScriptCache::Hash(retAddr);

        // Lazily initialize the cache. The allocation may safely fail and will not GC.
        if (MOZ_UNLIKELY(cx->ionPcScriptCache == nullptr)) {
            cx->ionPcScriptCache = (PcScriptCache*)js_malloc(sizeof(struct PcScriptCache));
            if (cx->ionPcScriptCache)
                cx->ionPcScriptCache->clear(cx->runtime()->gc.gcNumber());
        }

        if (cx->ionPcScriptCache && cx->ionPcScriptCache->get(cx->runtime(), hash, retAddr, scriptRes, pcRes))
            return;
    }

    // Lookup failed: undertake expensive process to recover the innermost inlined frame.
    jsbytecode* pc = nullptr;
    if (it.frame().isIonJS() || it.frame().isBailoutJS()) {
        InlineFrameIterator ifi(cx, &it.frame());
        *scriptRes = ifi.script();
        pc = ifi.pc();
    } else {
        MOZ_ASSERT(it.frame().isBaselineJS());
        it.frame().baselineScriptAndPc(scriptRes, &pc);
    }

    if (pcRes)
        *pcRes = pc;

    // Add entry to cache.
    if (retAddr && cx->ionPcScriptCache)
        cx->ionPcScriptCache->add(hash, retAddr, pc, *scriptRes);
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

size_t
RInstructionResults::length() const
{
    return results_->length();
}

JitFrameLayout*
RInstructionResults::frame() const
{
    MOZ_ASSERT(fp_);
    return fp_;
}

HeapPtr<Value>&
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


SnapshotIterator::SnapshotIterator(const JSJitFrameIter& iter, const MachineState* machineState)
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
    // Note: Both MIRType::Object and MIRType::ObjectOrNull are encoded in
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
        return Value::fromTagAndPayload(JSValueTag(fromRegister(alloc.reg())),
                                        fromRegister(alloc.reg2()));
      }

      case RValueAllocation::UNTYPED_REG_STACK:
      {
        return Value::fromTagAndPayload(JSValueTag(fromRegister(alloc.reg())),
                                        fromStack(alloc.stackOffset2()));
      }

      case RValueAllocation::UNTYPED_STACK_REG:
      {
        return Value::fromTagAndPayload(JSValueTag(fromStack(alloc.stackOffset())),
                                        fromRegister(alloc.reg2()));
      }

      case RValueAllocation::UNTYPED_STACK_STACK:
      {
        return Value::fromTagAndPayload(JSValueTag(fromStack(alloc.stackOffset())),
                                        fromStack(alloc.stackOffset2()));
      }
#elif defined(JS_PUNBOX64)
      case RValueAllocation::UNTYPED_REG:
      {
        return Value::fromRawBits(fromRegister(alloc.reg()));
      }

      case RValueAllocation::UNTYPED_STACK:
      {
        return Value::fromRawBits(fromStack(alloc.stackOffset()));
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
        // Code paths which are calling maybeRead are not always capable of
        // returning an error code, as these code paths used to be infallible.
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!initInstructionResults(fallback))
            oomUnsafe.crash("js::jit::SnapshotIterator::maybeRead");

        if (allocationReadable(a))
            return allocationValue(a);

        MOZ_ASSERT_UNREACHABLE("All allocations should be readable.");
    }

    return fallback.unreadablePlaceholder();
}

void
SnapshotIterator::writeAllocationValuePayload(const RValueAllocation& alloc, const Value& v)
{
    MOZ_ASSERT(v.isGCThing());

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
        machine_->write(alloc.reg2(), uintptr_t(v.toGCThing()));
        break;

      case RValueAllocation::TYPED_STACK:
        switch (alloc.knownType()) {
          default:
            MOZ_CRASH("Not a GC thing: Unexpected write");
            break;
          case JSVAL_TYPE_STRING:
          case JSVAL_TYPE_SYMBOL:
          case JSVAL_TYPE_OBJECT:
            WriteFrameSlot(fp_, alloc.stackOffset2(), uintptr_t(v.toGCThing()));
            break;
        }
        break;

#if defined(JS_NUNBOX32)
      case RValueAllocation::UNTYPED_REG_REG:
      case RValueAllocation::UNTYPED_STACK_REG:
        machine_->write(alloc.reg2(), uintptr_t(v.toGCThing()));
        break;

      case RValueAllocation::UNTYPED_REG_STACK:
      case RValueAllocation::UNTYPED_STACK_STACK:
        WriteFrameSlot(fp_, alloc.stackOffset2(), uintptr_t(v.toGCThing()));
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
    if (!v.isGCThing())
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
        AutoCompartment ac(cx, fallback.frame->script());

        // We do not have the result yet, which means that an observable stack
        // slot is requested.  As we do not want to bailout every time for the
        // same reason, we need to recompile without optimizing away the
        // observable stack slots.  The script would later be recompiled to have
        // support for Argument objects.
        if (fallback.consequence == MaybeReadFallback::Fallback_Invalidate)
            ionScript_->invalidate(cx, /* resetUses = */ false, "Observe recovered instruction.");

        // Register the list of result on the activation.  We need to do that
        // before we initialize the list such as if any recover instruction
        // cause a GC, we can ensure that the results are properly traced by the
        // activation.
        RInstructionResults tmp(fallback.frame->jsFrame());
        if (!fallback.activation->registerIonFrameRecovery(mozilla::Move(tmp)))
            return false;

        results = fallback.activation->maybeIonFrameRecovery(fp);

        // Start a new snapshot at the beginning of the JSJitFrameIter.  This
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
    MOZ_RELEASE_ASSERT(results->length() == recover_.numInstructions() - 1);
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
SnapshotIterator::storeInstructionResult(const Value& v)
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

InlineFrameIterator::InlineFrameIterator(JSContext* cx, const JSJitFrameIter* iter)
  : calleeTemplate_(cx),
    calleeRVA_(),
    script_(cx)
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
InlineFrameIterator::resetOn(const JSJitFrameIter* iter)
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
        bool skipNewTarget = IsConstructorCallPC(pc_);
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
        script_ = calleeTemplate_->existingScript();
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
InlineFrameIterator::computeEnvironmentChain(const Value& envChainValue,
                                             MaybeReadFallback& fallback,
                                             bool* hasInitialEnv) const
{
    if (envChainValue.isObject()) {
        if (hasInitialEnv) {
            if (fallback.canRecoverResults()) {
                RootedObject obj(fallback.maybeCx, &envChainValue.toObject());
                *hasInitialEnv = isFunctionFrame() &&
                                 callee(fallback)->needsFunctionEnvironmentObjects();
                return obj;
            } else {
                JS::AutoSuppressGCAnalysis nogc; // If we cannot recover then we cannot GC.
                *hasInitialEnv = isFunctionFrame() &&
                                 callee(fallback)->needsFunctionEnvironmentObjects();
            }
        }

        return &envChainValue.toObject();
    }

    // Note we can hit this case even for functions with a CallObject, in case
    // we are walking the frame during the function prologue, before the env
    // chain has been initialized.
    if (isFunctionFrame())
        return callee(fallback)->environment();

    if (isModuleFrame())
        return script()->module()->environment();

    // Ion does not handle non-function scripts that have anything other than
    // the global on their env chain.
    MOZ_ASSERT(!script()->isForEval());
    MOZ_ASSERT(!script()->hasNonSyntacticScope());
    return &script()->global().lexicalEnvironment();
}

bool
InlineFrameIterator::isFunctionFrame() const
{
    return !!calleeTemplate_;
}

bool
InlineFrameIterator::isModuleFrame() const
{
    return script()->module();
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
    for (unsigned i = 0; i < FloatRegisters::TotalPhys; i++) {
        machine.setRegisterLocation(FloatRegister::FromIndex(i, FloatRegister::Double), &fpregs[i]);
        machine.setRegisterLocation(FloatRegister::FromIndex(i, FloatRegister::Single), &fpregs[i]);
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
        InlineFrameIterator parent(TlsContext.get(), this);
        ++parent;

        // Inlined Getters and Setters are never constructing.
        if (IsGetPropPC(parent.pc()) || IsSetPropPC(parent.pc()))
            return false;

        // In the case of a JS frame, look up the pc from the snapshot.
        MOZ_ASSERT(IsCallPC(parent.pc()) && !IsSpreadCallPC(parent.pc()));

        return IsConstructorCallPC(parent.pc());
    }

    return frame_->isConstructing();
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

    fprintf(stderr, "  file %s line %zu\n",
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
                fprintf(stderr, "  env chain: ");
            else if (i == 1)
                fprintf(stderr, "  this: ");
            else if (i - 2 < calleeTemplate()->nargs())
                fprintf(stderr, "  formal (arg %d): ", i - 2);
            else {
                if (i - 2 == calleeTemplate()->nargs() && numActualArgs() > calleeTemplate()->nargs()) {
                    DumpOp d(calleeTemplate()->nargs());
                    unaliasedForEachActual(TlsContext.get(), d, ReadFrame_Overflown, fallback);
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
    for (JitActivationIterator activations(cx); !activations.done(); ++activations) {
        JitFrameIter iter(activations->asJit());
        if (iter.isJSJit()) {
            JSJitFrameIter& frames = iter.asJSJit();
            size_t prevFrameSize = 0;
            size_t frameSize = 0;
            bool isScriptedCallee = false;
            for (; !frames.done(); ++frames) {
                size_t calleeFp = reinterpret_cast<size_t>(frames.fp());
                size_t callerFp = reinterpret_cast<size_t>(frames.prevFp());
                MOZ_ASSERT(callerFp >= calleeFp);
                prevFrameSize = frameSize;
                frameSize = callerFp - calleeFp;

                if (frames.isScripted() && frames.prevType() == JitFrame_Rectifier) {
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

                isScriptedCallee = frames.isScripted() || frames.type() == JitFrame_Rectifier;
            }

            MOZ_RELEASE_ASSERT(JSJitFrameIter::isEntry(frames.type()),
              "The first frame of a Jit activation should be an entry frame");
            MOZ_RELEASE_ASSERT(reinterpret_cast<size_t>(frames.fp()) % JitStackAlignment == 0,
              "The entry frame should be properly aligned");
        } else {
            MOZ_ASSERT(iter.isWasm());
            wasm::WasmFrameIter& frames = iter.asWasm();
            while (!frames.done())
                ++frames;
        }
    }
}

} // namespace jit
} // namespace js
