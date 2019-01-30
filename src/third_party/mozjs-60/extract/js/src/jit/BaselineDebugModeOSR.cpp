/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineDebugModeOSR.h"

#include "jit/BaselineIC.h"
#include "jit/JitcodeMap.h"
#include "jit/Linker.h"
#include "jit/PerfSpewer.h"

#include "jit/JitFrames-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "vm/Stack-inl.h"
#include "vm/TypeInference-inl.h"

using namespace js;
using namespace js::jit;

struct DebugModeOSREntry
{
    JSScript* script;
    BaselineScript* oldBaselineScript;
    ICStub* oldStub;
    ICStub* newStub;
    BaselineDebugModeOSRInfo* recompInfo;
    uint32_t pcOffset;
    ICEntry::Kind frameKind;

    explicit DebugModeOSREntry(JSScript* script)
      : script(script),
        oldBaselineScript(script->baselineScript()),
        oldStub(nullptr),
        newStub(nullptr),
        recompInfo(nullptr),
        pcOffset(uint32_t(-1)),
        frameKind(ICEntry::Kind_Invalid)
    { }

    DebugModeOSREntry(JSScript* script, uint32_t pcOffset)
      : script(script),
        oldBaselineScript(script->baselineScript()),
        oldStub(nullptr),
        newStub(nullptr),
        recompInfo(nullptr),
        pcOffset(pcOffset),
        frameKind(ICEntry::Kind_Invalid)
    { }

    DebugModeOSREntry(JSScript* script, const ICEntry& icEntry)
      : script(script),
        oldBaselineScript(script->baselineScript()),
        oldStub(nullptr),
        newStub(nullptr),
        recompInfo(nullptr),
        pcOffset(icEntry.pcOffset()),
        frameKind(icEntry.kind())
    {
#ifdef DEBUG
        MOZ_ASSERT(pcOffset == icEntry.pcOffset());
        MOZ_ASSERT(frameKind == icEntry.kind());
#endif
    }

    DebugModeOSREntry(JSScript* script, BaselineDebugModeOSRInfo* info)
      : script(script),
        oldBaselineScript(script->baselineScript()),
        oldStub(nullptr),
        newStub(nullptr),
        recompInfo(nullptr),
        pcOffset(script->pcToOffset(info->pc)),
        frameKind(info->frameKind)
    {
#ifdef DEBUG
        MOZ_ASSERT(pcOffset == script->pcToOffset(info->pc));
        MOZ_ASSERT(frameKind == info->frameKind);
#endif
    }

    DebugModeOSREntry(DebugModeOSREntry&& other)
      : script(other.script),
        oldBaselineScript(other.oldBaselineScript),
        oldStub(other.oldStub),
        newStub(other.newStub),
        recompInfo(other.recompInfo ? other.takeRecompInfo() : nullptr),
        pcOffset(other.pcOffset),
        frameKind(other.frameKind)
    { }

    ~DebugModeOSREntry() {
        // Note that this is nulled out when the recompInfo is taken by the
        // frame. The frame then has the responsibility of freeing the
        // recompInfo.
        js_delete(recompInfo);
    }

    bool needsRecompileInfo() const {
        return frameKind == ICEntry::Kind_CallVM ||
               frameKind == ICEntry::Kind_WarmupCounter ||
               frameKind == ICEntry::Kind_StackCheck ||
               frameKind == ICEntry::Kind_EarlyStackCheck ||
               frameKind == ICEntry::Kind_DebugTrap ||
               frameKind == ICEntry::Kind_DebugPrologue ||
               frameKind == ICEntry::Kind_DebugEpilogue;
    }

    bool recompiled() const {
        return oldBaselineScript != script->baselineScript();
    }

    BaselineDebugModeOSRInfo* takeRecompInfo() {
        MOZ_ASSERT(needsRecompileInfo() && recompInfo);
        BaselineDebugModeOSRInfo* tmp = recompInfo;
        recompInfo = nullptr;
        return tmp;
    }

    bool allocateRecompileInfo(JSContext* cx) {
        MOZ_ASSERT(script);
        MOZ_ASSERT(needsRecompileInfo());

        // If we are returning to a frame which needs a continuation fixer,
        // allocate the recompile info up front so that the patching function
        // is infallible.
        jsbytecode* pc = script->offsetToPC(pcOffset);

        // XXX: Work around compiler error disallowing using bitfields
        // with the template magic of new_.
        ICEntry::Kind kind = frameKind;
        recompInfo = cx->new_<BaselineDebugModeOSRInfo>(pc, kind);
        return !!recompInfo;
    }

    ICFallbackStub* fallbackStub() const {
        MOZ_ASSERT(script);
        MOZ_ASSERT(oldStub);
        return script->baselineScript()->icEntryFromPCOffset(pcOffset).fallbackStub();
    }
};

typedef Vector<DebugModeOSREntry> DebugModeOSREntryVector;

class UniqueScriptOSREntryIter
{
    const DebugModeOSREntryVector& entries_;
    size_t index_;

  public:
    explicit UniqueScriptOSREntryIter(const DebugModeOSREntryVector& entries)
      : entries_(entries),
        index_(0)
    { }

    bool done() {
        return index_ == entries_.length();
    }

    const DebugModeOSREntry& entry() {
        MOZ_ASSERT(!done());
        return entries_[index_];
    }

    UniqueScriptOSREntryIter& operator++() {
        MOZ_ASSERT(!done());
        while (++index_ < entries_.length()) {
            bool unique = true;
            for (size_t i = 0; i < index_; i++) {
                if (entries_[i].script == entries_[index_].script) {
                    unique = false;
                    break;
                }
            }
            if (unique)
                break;
        }
        return *this;
    }
};

static bool
CollectJitStackScripts(JSContext* cx, const Debugger::ExecutionObservableSet& obs,
                       const ActivationIterator& activation, DebugModeOSREntryVector& entries)
{
    ICStub* prevFrameStubPtr = nullptr;
    bool needsRecompileHandler = false;
    for (OnlyJSJitFrameIter iter(activation); !iter.done(); ++iter) {
        const JSJitFrameIter& frame = iter.frame();
        switch (frame.type()) {
          case JitFrame_BaselineJS: {
            JSScript* script = frame.script();

            if (!obs.shouldRecompileOrInvalidate(script)) {
                prevFrameStubPtr = nullptr;
                break;
            }

            BaselineFrame* baselineFrame = frame.baselineFrame();

            if (BaselineDebugModeOSRInfo* info = baselineFrame->getDebugModeOSRInfo()) {
                // If patching a previously patched yet unpopped frame, we can
                // use the BaselineDebugModeOSRInfo on the frame directly to
                // patch. Indeed, we cannot use frame.returnAddressToFp(), as
                // it points into the debug mode OSR handler and cannot be
                // used to look up a corresponding ICEntry.
                //
                // See cases F and G in PatchBaselineFramesForDebugMode.
                if (!entries.append(DebugModeOSREntry(script, info)))
                    return false;
            } else if (baselineFrame->isHandlingException()) {
                // We are in the middle of handling an exception and the frame
                // must have an override pc.
                uint32_t offset = script->pcToOffset(baselineFrame->overridePc());
                if (!entries.append(DebugModeOSREntry(script, offset)))
                    return false;
            } else {
                // The frame must be settled on a pc with an ICEntry.
                uint8_t* retAddr = frame.returnAddressToFp();
                BaselineICEntry& icEntry = script->baselineScript()->icEntryFromReturnAddress(retAddr);
                if (!entries.append(DebugModeOSREntry(script, icEntry)))
                    return false;
            }

            if (entries.back().needsRecompileInfo()) {
                if (!entries.back().allocateRecompileInfo(cx))
                    return false;

                needsRecompileHandler |= true;
            }
            entries.back().oldStub = prevFrameStubPtr;
            prevFrameStubPtr = nullptr;
            break;
          }

          case JitFrame_BaselineStub:
            prevFrameStubPtr =
                reinterpret_cast<BaselineStubFrameLayout*>(frame.fp())->maybeStubPtr();
            break;

          case JitFrame_IonJS: {
            InlineFrameIterator inlineIter(cx, &frame);
            while (true) {
                if (obs.shouldRecompileOrInvalidate(inlineIter.script())) {
                    if (!entries.append(DebugModeOSREntry(inlineIter.script())))
                        return false;
                }
                if (!inlineIter.more())
                    break;
                ++inlineIter;
            }
            break;
          }

          default:;
        }
    }

    // Initialize the on-stack recompile handler, which may fail, so that
    // patching the stack is infallible.
    if (needsRecompileHandler) {
        JitRuntime* rt = cx->runtime()->jitRuntime();
        if (!rt->getBaselineDebugModeOSRHandlerAddress(cx, true))
            return false;
    }

    return true;
}

static bool
CollectInterpreterStackScripts(JSContext* cx, const Debugger::ExecutionObservableSet& obs,
                               const ActivationIterator& activation,
                               DebugModeOSREntryVector& entries)
{
    // Collect interpreter frame stacks with IonScript or BaselineScript as
    // well. These do not need to be patched, but do need to be invalidated
    // and recompiled.
    InterpreterActivation* act = activation.activation()->asInterpreter();
    for (InterpreterFrameIterator iter(act); !iter.done(); ++iter) {
        JSScript* script = iter.frame()->script();
        if (obs.shouldRecompileOrInvalidate(script)) {
            if (!entries.append(DebugModeOSREntry(iter.frame()->script())))
                return false;
        }
    }
    return true;
}

#ifdef JS_JITSPEW
static const char*
ICEntryKindToString(ICEntry::Kind kind)
{
    switch (kind) {
      case ICEntry::Kind_Op:
        return "IC";
      case ICEntry::Kind_NonOp:
        return "non-op IC";
      case ICEntry::Kind_CallVM:
        return "callVM";
      case ICEntry::Kind_WarmupCounter:
        return "warmup counter";
      case ICEntry::Kind_StackCheck:
        return "stack check";
      case ICEntry::Kind_EarlyStackCheck:
        return "early stack check";
      case ICEntry::Kind_DebugTrap:
        return "debug trap";
      case ICEntry::Kind_DebugPrologue:
        return "debug prologue";
      case ICEntry::Kind_DebugEpilogue:
        return "debug epilogue";
      default:
        MOZ_CRASH("bad ICEntry kind");
    }
}
#endif // JS_JITSPEW

static void
SpewPatchBaselineFrame(uint8_t* oldReturnAddress, uint8_t* newReturnAddress,
                       JSScript* script, ICEntry::Kind frameKind, jsbytecode* pc)
{
    JitSpew(JitSpew_BaselineDebugModeOSR,
            "Patch return %p -> %p on BaselineJS frame (%s:%zu) from %s at %s",
            oldReturnAddress, newReturnAddress, script->filename(), script->lineno(),
            ICEntryKindToString(frameKind), CodeName[(JSOp)*pc]);
}

static void
SpewPatchBaselineFrameFromExceptionHandler(uint8_t* oldReturnAddress, uint8_t* newReturnAddress,
                                           JSScript* script, jsbytecode* pc)
{
    JitSpew(JitSpew_BaselineDebugModeOSR,
            "Patch return %p -> %p on BaselineJS frame (%s:%zu) from exception handler at %s",
            oldReturnAddress, newReturnAddress, script->filename(), script->lineno(),
            CodeName[(JSOp)*pc]);
}

static void
SpewPatchStubFrame(ICStub* oldStub, ICStub* newStub)
{
    JitSpew(JitSpew_BaselineDebugModeOSR,
            "Patch   stub %p -> %p on BaselineStub frame (%s)",
            oldStub, newStub, newStub ? ICStub::KindString(newStub->kind()) : "exception handler");
}

static void
PatchBaselineFramesForDebugMode(JSContext* cx, const CooperatingContext& target,
                                const Debugger::ExecutionObservableSet& obs,
                                const ActivationIterator& activation,
                                DebugModeOSREntryVector& entries, size_t* start)
{
    //
    // Recompile Patching Overview
    //
    // When toggling debug mode with live baseline scripts on the stack, we
    // could have entered the VM via the following ways from the baseline
    // script.
    //
    // Off to On:
    //  A. From a "can call" stub.
    //  B. From a VM call.
    //  H. From inside HandleExceptionBaseline.
    //  I. From inside the interrupt handler via the prologue stack check.
    //  J. From the warmup counter in the prologue.
    //
    // On to Off:
    //  - All the ways above.
    //  C. From the debug trap handler.
    //  D. From the debug prologue.
    //  E. From the debug epilogue.
    //
    // Cycles (On to Off to On)+ or (Off to On to Off)+:
    //  F. Undo cases B, C, D, E, I or J above on previously patched yet unpopped
    //     frames.
    //
    // In general, we patch the return address from the VM call to return to a
    // "continuation fixer" to fix up machine state (registers and stack
    // state). Specifics on what needs to be done are documented below.
    //

    CommonFrameLayout* prev = nullptr;
    size_t entryIndex = *start;

    for (OnlyJSJitFrameIter iter(activation); !iter.done(); ++iter) {
        const JSJitFrameIter& frame = iter.frame();
        switch (frame.type()) {
          case JitFrame_BaselineJS: {
            // If the script wasn't recompiled or is not observed, there's
            // nothing to patch.
            if (!obs.shouldRecompileOrInvalidate(frame.script()))
                break;

            DebugModeOSREntry& entry = entries[entryIndex];

            if (!entry.recompiled()) {
                entryIndex++;
                break;
            }

            JSScript* script = entry.script;
            uint32_t pcOffset = entry.pcOffset;
            jsbytecode* pc = script->offsetToPC(pcOffset);

            MOZ_ASSERT(script == frame.script());
            MOZ_ASSERT(pcOffset < script->length());

            BaselineScript* bl = script->baselineScript();
            ICEntry::Kind kind = entry.frameKind;

            if (kind == ICEntry::Kind_Op) {
                // Case A above.
                //
                // Patching these cases needs to patch both the stub frame and
                // the baseline frame. The stub frame is patched below. For
                // the baseline frame here, we resume right after the IC
                // returns.
                //
                // Since we're using the same IC stub code, we can resume
                // directly to the IC resume address.
                uint8_t* retAddr = bl->returnAddressForIC(bl->icEntryFromPCOffset(pcOffset));
                SpewPatchBaselineFrame(prev->returnAddress(), retAddr, script, kind, pc);
                DebugModeOSRVolatileJitFrameIter::forwardLiveIterators(
                    target, prev->returnAddress(), retAddr);
                prev->setReturnAddress(retAddr);
                entryIndex++;
                break;
            }

            if (kind == ICEntry::Kind_Invalid) {
                // Case H above.
                //
                // We are recompiling on-stack scripts from inside the
                // exception handler, by way of an onExceptionUnwind
                // invocation, on a pc without an ICEntry. This means the
                // frame must have an override pc.
                //
                // If profiling is off, patch the resume address to nullptr,
                // to ensure the old address is not used anywhere.
                //
                // If profiling is on, JSJitProfilingFrameIterator requires a
                // valid return address.
                MOZ_ASSERT(frame.baselineFrame()->isHandlingException());
                MOZ_ASSERT(frame.baselineFrame()->overridePc() == pc);
                uint8_t* retAddr;
                if (cx->runtime()->geckoProfiler().enabled())
                    retAddr = bl->nativeCodeForPC(script, pc);
                else
                    retAddr = nullptr;
                SpewPatchBaselineFrameFromExceptionHandler(prev->returnAddress(), retAddr,
                                                           script, pc);
                DebugModeOSRVolatileJitFrameIter::forwardLiveIterators(
                    target, prev->returnAddress(), retAddr);
                prev->setReturnAddress(retAddr);
                entryIndex++;
                break;
            }

            // Case F above.
            //
            // We undo a previous recompile by handling cases B, C, D, E, I or J
            // like normal, except that we retrieve the pc information via
            // the previous OSR debug info stashed on the frame.
            BaselineDebugModeOSRInfo* info = frame.baselineFrame()->getDebugModeOSRInfo();
            if (info) {
                MOZ_ASSERT(info->pc == pc);
                MOZ_ASSERT(info->frameKind == kind);
                MOZ_ASSERT(kind == ICEntry::Kind_CallVM ||
                           kind == ICEntry::Kind_WarmupCounter ||
                           kind == ICEntry::Kind_StackCheck ||
                           kind == ICEntry::Kind_EarlyStackCheck ||
                           kind == ICEntry::Kind_DebugTrap ||
                           kind == ICEntry::Kind_DebugPrologue ||
                           kind == ICEntry::Kind_DebugEpilogue);

                // We will have allocated a new recompile info, so delete the
                // existing one.
                frame.baselineFrame()->deleteDebugModeOSRInfo();
            }

            // The RecompileInfo must already be allocated so that this
            // function may be infallible.
            BaselineDebugModeOSRInfo* recompInfo = entry.takeRecompInfo();

            bool popFrameReg;
            switch (kind) {
              case ICEntry::Kind_CallVM: {
                // Case B above.
                //
                // Patching returns from a VM call. After fixing up the the
                // continuation for unsynced values (the frame register is
                // popped by the callVM trampoline), we resume at the
                // return-from-callVM address. The assumption here is that all
                // callVMs which can trigger debug mode OSR are the *only*
                // callVMs generated for their respective pc locations in the
                // baseline JIT code.
                BaselineICEntry& callVMEntry = bl->callVMEntryFromPCOffset(pcOffset);
                recompInfo->resumeAddr = bl->returnAddressForIC(callVMEntry);
                popFrameReg = false;
                break;
              }

              case ICEntry::Kind_WarmupCounter: {
                // Case J above.
                //
                // Patching mechanism is identical to a CallVM. This is
                // handled especially only because the warmup counter VM call is
                // part of the prologue, and not tied an opcode.
                BaselineICEntry& warmupCountEntry = bl->warmupCountICEntry();
                recompInfo->resumeAddr = bl->returnAddressForIC(warmupCountEntry);
                popFrameReg = false;
                break;
              }

              case ICEntry::Kind_StackCheck:
              case ICEntry::Kind_EarlyStackCheck: {
                // Case I above.
                //
                // Patching mechanism is identical to a CallVM. This is
                // handled especially only because the stack check VM call is
                // part of the prologue, and not tied an opcode.
                bool earlyCheck = kind == ICEntry::Kind_EarlyStackCheck;
                BaselineICEntry& stackCheckEntry = bl->stackCheckICEntry(earlyCheck);
                recompInfo->resumeAddr = bl->returnAddressForIC(stackCheckEntry);
                popFrameReg = false;
                break;
              }

              case ICEntry::Kind_DebugTrap:
                // Case C above.
                //
                // Debug traps are emitted before each op, so we resume at the
                // same op. Calling debug trap handlers is done via a toggled
                // call to a thunk (DebugTrapHandler) that takes care tearing
                // down its own stub frame so we don't need to worry about
                // popping the frame reg.
                recompInfo->resumeAddr = bl->nativeCodeForPC(script, pc, &recompInfo->slotInfo);
                popFrameReg = false;
                break;

              case ICEntry::Kind_DebugPrologue:
                // Case D above.
                //
                // We patch a jump directly to the right place in the prologue
                // after popping the frame reg and checking for forced return.
                recompInfo->resumeAddr = bl->postDebugPrologueAddr();
                popFrameReg = true;
                break;

              default:
                // Case E above.
                //
                // We patch a jump directly to the epilogue after popping the
                // frame reg and checking for forced return.
                MOZ_ASSERT(kind == ICEntry::Kind_DebugEpilogue);
                recompInfo->resumeAddr = bl->epilogueEntryAddr();
                popFrameReg = true;
                break;
            }

            SpewPatchBaselineFrame(prev->returnAddress(), recompInfo->resumeAddr,
                                   script, kind, recompInfo->pc);

            // The recompile handler must already be created so that this
            // function may be infallible.
            JitRuntime* rt = cx->runtime()->jitRuntime();
            void* handlerAddr = rt->getBaselineDebugModeOSRHandlerAddress(cx, popFrameReg);
            MOZ_ASSERT(handlerAddr);

            prev->setReturnAddress(reinterpret_cast<uint8_t*>(handlerAddr));
            frame.baselineFrame()->setDebugModeOSRInfo(recompInfo);
            frame.baselineFrame()->setOverridePc(recompInfo->pc);

            entryIndex++;
            break;
          }

          case JitFrame_BaselineStub: {
            JSJitFrameIter prev(iter.frame());
            ++prev;
            BaselineFrame* prevFrame = prev.baselineFrame();
            if (!obs.shouldRecompileOrInvalidate(prevFrame->script()))
                break;

            DebugModeOSREntry& entry = entries[entryIndex];

            // If the script wasn't recompiled, there's nothing to patch.
            if (!entry.recompiled())
                break;

            BaselineStubFrameLayout* layout =
                reinterpret_cast<BaselineStubFrameLayout*>(frame.fp());
            MOZ_ASSERT(layout->maybeStubPtr() == entry.oldStub);

            // Patch baseline stub frames for case A above.
            //
            // We need to patch the stub frame to point to an ICStub belonging
            // to the recompiled baseline script. These stubs are allocated up
            // front in CloneOldBaselineStub. They share the same JitCode as
            // the old baseline script's stubs, so we don't need to patch the
            // exit frame's return address.
            //
            // Subtlety here: the debug trap handler of case C above pushes a
            // stub frame with a null stub pointer. This handler will exist
            // across recompiling the script, so we don't patch anything for
            // such stub frames. We will return to that handler, which takes
            // care of cleaning up the stub frame.
            //
            // Note that for stub pointers that are already on the C stack
            // (i.e. fallback calls), we need to check for recompilation using
            // DebugModeOSRVolatileStub.
            if (layout->maybeStubPtr()) {
                MOZ_ASSERT(entry.newStub || prevFrame->isHandlingException());
                SpewPatchStubFrame(entry.oldStub, entry.newStub);
                layout->setStubPtr(entry.newStub);
            }

            break;
          }

          case JitFrame_IonJS: {
            // Nothing to patch.
            InlineFrameIterator inlineIter(cx, &frame);
            while (true) {
                if (obs.shouldRecompileOrInvalidate(inlineIter.script()))
                    entryIndex++;
                if (!inlineIter.more())
                    break;
                ++inlineIter;
            }
            break;
          }

          default:;
        }

        prev = frame.current();
    }

    *start = entryIndex;
}

static void
SkipInterpreterFrameEntries(const Debugger::ExecutionObservableSet& obs,
                            const ActivationIterator& activation,
                            size_t* start)
{
    size_t entryIndex = *start;

    // Skip interpreter frames, which do not need patching.
    InterpreterActivation* act = activation.activation()->asInterpreter();
    for (InterpreterFrameIterator iter(act); !iter.done(); ++iter) {
        if (obs.shouldRecompileOrInvalidate(iter.frame()->script()))
            entryIndex++;
    }

    *start = entryIndex;
}

static bool
RecompileBaselineScriptForDebugMode(JSContext* cx, JSScript* script,
                                    Debugger::IsObserving observing)
{
    BaselineScript* oldBaselineScript = script->baselineScript();

    // If a script is on the stack multiple times, it may have already
    // been recompiled.
    if (oldBaselineScript->hasDebugInstrumentation() == observing)
        return true;

    JitSpew(JitSpew_BaselineDebugModeOSR, "Recompiling (%s:%zu) for %s",
            script->filename(), script->lineno(), observing ? "DEBUGGING" : "NORMAL EXECUTION");

    AutoKeepTypeScripts keepTypes(cx);
    script->setBaselineScript(cx->runtime(), nullptr);

    MethodStatus status = BaselineCompile(cx, script, /* forceDebugMode = */ observing);
    if (status != Method_Compiled) {
        // We will only fail to recompile for debug mode due to OOM. Restore
        // the old baseline script in case something doesn't properly
        // propagate OOM.
        MOZ_ASSERT(status == Method_Error);
        script->setBaselineScript(cx->runtime(), oldBaselineScript);
        return false;
    }

    // Don't destroy the old baseline script yet, since if we fail any of the
    // recompiles we need to rollback all the old baseline scripts.
    MOZ_ASSERT(script->baselineScript()->hasDebugInstrumentation() == observing);
    return true;
}

#define PATCHABLE_ICSTUB_KIND_LIST(_)           \
    _(CacheIR_Monitored)                        \
    _(CacheIR_Regular)                          \
    _(CacheIR_Updated)                          \
    _(Call_Scripted)                            \
    _(Call_AnyScripted)                         \
    _(Call_Native)                              \
    _(Call_ClassHook)                           \
    _(Call_ScriptedApplyArray)                  \
    _(Call_ScriptedApplyArguments)              \
    _(Call_ScriptedFunCall)

static bool
CloneOldBaselineStub(JSContext* cx, DebugModeOSREntryVector& entries, size_t entryIndex)
{
    DebugModeOSREntry& entry = entries[entryIndex];
    if (!entry.oldStub)
        return true;

    ICStub* oldStub = entry.oldStub;
    MOZ_ASSERT(oldStub->makesGCCalls());

    // If this script was not recompiled (because it already had the correct
    // debug instrumentation), don't clone to avoid attaching duplicate stubs.
    if (!entry.recompiled()) {
        entry.newStub = nullptr;
        return true;
    }

    if (entry.frameKind == ICEntry::Kind_Invalid) {
        // The exception handler can modify the frame's override pc while
        // unwinding scopes. This is fine, but if we have a stub frame, the code
        // code below will get confused: the entry's pcOffset doesn't match the
        // stub that's still on the stack. To prevent that, we just set the new
        // stub to nullptr as we will never return to this stub frame anyway.
        entry.newStub = nullptr;
        return true;
    }

    // Get the new fallback stub from the recompiled baseline script.
    ICFallbackStub* fallbackStub = entry.fallbackStub();

    // Some stubs are monitored, get the first stub in the monitor chain from
    // the new fallback stub if so. We do this before checking for fallback
    // stubs below, to ensure monitored fallback stubs have a type monitor
    // chain.
    ICStub* firstMonitorStub;
    if (fallbackStub->isMonitoredFallback()) {
        ICMonitoredFallbackStub* monitored = fallbackStub->toMonitoredFallbackStub();
        ICTypeMonitor_Fallback* fallback = monitored->getFallbackMonitorStub(cx, entry.script);
        if (!fallback)
            return false;
        firstMonitorStub = fallback->firstMonitorStub();
    } else {
        firstMonitorStub = nullptr;
    }

    // We don't need to clone fallback stubs, as they are guaranteed to
    // exist. Furthermore, their JitCode is cached and should be the same even
    // across the recompile.
    if (oldStub->isFallback()) {
        MOZ_ASSERT(oldStub->jitCode() == fallbackStub->jitCode());
        entry.newStub = fallbackStub;
        return true;
    }

    // Check if we have already cloned the stub on a younger frame. Ignore
    // frames that entered the exception handler (entries[i].newStub is nullptr
    // in that case, see above).
    for (size_t i = 0; i < entryIndex; i++) {
        if (oldStub == entries[i].oldStub && entries[i].frameKind != ICEntry::Kind_Invalid) {
            MOZ_ASSERT(entries[i].newStub);
            entry.newStub = entries[i].newStub;
            return true;
        }
    }

    ICStubSpace* stubSpace = ICStubCompiler::StubSpaceForStub(oldStub->makesGCCalls(), entry.script,
                                                              ICStubCompiler::Engine::Baseline);

    // Clone the existing stub into the recompiled IC.
    //
    // Note that since JitCode is a GC thing, cloning an ICStub with the same
    // JitCode ensures it won't be collected.
    switch (oldStub->kind()) {
#define CASE_KIND(kindName)                                                  \
      case ICStub::kindName:                                                 \
        entry.newStub = IC##kindName::Clone(cx, stubSpace, firstMonitorStub, \
                                            *oldStub->to##kindName());       \
        break;
        PATCHABLE_ICSTUB_KIND_LIST(CASE_KIND)
#undef CASE_KIND

      default:
        MOZ_CRASH("Bad stub kind");
    }

    if (!entry.newStub)
        return false;

    fallbackStub->addNewStub(entry.newStub);
    return true;
}

static bool
InvalidateScriptsInZone(JSContext* cx, Zone* zone, const Vector<DebugModeOSREntry>& entries)
{
    RecompileInfoVector invalid;
    for (UniqueScriptOSREntryIter iter(entries); !iter.done(); ++iter) {
        JSScript* script = iter.entry().script;
        if (script->compartment()->zone() != zone)
            continue;

        if (script->hasIonScript()) {
            if (!invalid.append(script->ionScript()->recompileInfo())) {
                ReportOutOfMemory(cx);
                return false;
            }
        }

        // Cancel off-thread Ion compile for anything that has a
        // BaselineScript. If we relied on the call to Invalidate below to
        // cancel off-thread Ion compiles, only those with existing IonScripts
        // would be cancelled.
        if (script->hasBaselineScript())
            CancelOffThreadIonCompile(script);
    }

    // No need to cancel off-thread Ion compiles again, we already did it
    // above.
    Invalidate(zone->types, cx->runtime()->defaultFreeOp(), invalid,
               /* resetUses = */ true, /* cancelOffThread = */ false);
    return true;
}

static void
UndoRecompileBaselineScriptsForDebugMode(JSContext* cx,
                                         const DebugModeOSREntryVector& entries)
{
    // In case of failure, roll back the entire set of active scripts so that
    // we don't have to patch return addresses on the stack.
    for (UniqueScriptOSREntryIter iter(entries); !iter.done(); ++iter) {
        const DebugModeOSREntry& entry = iter.entry();
        JSScript* script = entry.script;
        BaselineScript* baselineScript = script->baselineScript();
        if (entry.recompiled()) {
            script->setBaselineScript(cx->runtime(), entry.oldBaselineScript);
            BaselineScript::Destroy(cx->runtime()->defaultFreeOp(), baselineScript);
        }
    }
}

bool
jit::RecompileOnStackBaselineScriptsForDebugMode(JSContext* cx,
                                                 const Debugger::ExecutionObservableSet& obs,
                                                 Debugger::IsObserving observing)
{
    // First recompile the active scripts on the stack and patch the live
    // frames.
    Vector<DebugModeOSREntry> entries(cx);

    for (const CooperatingContext& target : cx->runtime()->cooperatingContexts()) {
        for (ActivationIterator iter(cx, target); !iter.done(); ++iter) {
            if (iter->isJit()) {
                if (!CollectJitStackScripts(cx, obs, iter, entries))
                    return false;
            } else if (iter->isInterpreter()) {
                if (!CollectInterpreterStackScripts(cx, obs, iter, entries))
                    return false;
            }
        }
    }

    if (entries.empty())
        return true;

    // When the profiler is enabled, we need to have suppressed sampling,
    // since the basline jit scripts are in a state of flux.
    MOZ_ASSERT(!cx->isProfilerSamplingEnabled());

    // Invalidate all scripts we are recompiling.
    if (Zone* zone = obs.singleZone()) {
        if (!InvalidateScriptsInZone(cx, zone, entries))
            return false;
    } else {
        typedef Debugger::ExecutionObservableSet::ZoneRange ZoneRange;
        for (ZoneRange r = obs.zones()->all(); !r.empty(); r.popFront()) {
            if (!InvalidateScriptsInZone(cx, r.front(), entries))
                return false;
        }
    }

    // Try to recompile all the scripts. If we encounter an error, we need to
    // roll back as if none of the compilations happened, so that we don't
    // crash.
    for (size_t i = 0; i < entries.length(); i++) {
        JSScript* script = entries[i].script;
        AutoCompartment ac(cx, script);
        if (!RecompileBaselineScriptForDebugMode(cx, script, observing) ||
            !CloneOldBaselineStub(cx, entries, i))
        {
            UndoRecompileBaselineScriptsForDebugMode(cx, entries);
            return false;
        }
    }

    // If all recompiles succeeded, destroy the old baseline scripts and patch
    // the live frames.
    //
    // After this point the function must be infallible.

    for (UniqueScriptOSREntryIter iter(entries); !iter.done(); ++iter) {
        const DebugModeOSREntry& entry = iter.entry();
        if (entry.recompiled())
            BaselineScript::Destroy(cx->runtime()->defaultFreeOp(), entry.oldBaselineScript);
    }

    size_t processed = 0;
    for (const CooperatingContext& target : cx->runtime()->cooperatingContexts()) {
        for (ActivationIterator iter(cx, target); !iter.done(); ++iter) {
            if (iter->isJit())
                PatchBaselineFramesForDebugMode(cx, target, obs, iter, entries, &processed);
            else if (iter->isInterpreter())
                SkipInterpreterFrameEntries(obs, iter, &processed);
        }
    }
    MOZ_ASSERT(processed == entries.length());

    return true;
}

void
BaselineDebugModeOSRInfo::popValueInto(PCMappingSlotInfo::SlotLocation loc, Value* vp)
{
    switch (loc) {
      case PCMappingSlotInfo::SlotInR0:
        valueR0 = vp[stackAdjust];
        break;
      case PCMappingSlotInfo::SlotInR1:
        valueR1 = vp[stackAdjust];
        break;
      case PCMappingSlotInfo::SlotIgnore:
        break;
      default:
        MOZ_CRASH("Bad slot location");
    }

    stackAdjust++;
}

static inline bool
HasForcedReturn(BaselineDebugModeOSRInfo* info, bool rv)
{
    ICEntry::Kind kind = info->frameKind;

    // The debug epilogue always checks its resumption value, so we don't need
    // to check rv.
    if (kind == ICEntry::Kind_DebugEpilogue)
        return true;

    // |rv| is the value in ReturnReg. If true, in the case of the prologue,
    // it means a forced return.
    if (kind == ICEntry::Kind_DebugPrologue)
        return rv;

    // N.B. The debug trap handler handles its own forced return, so no
    // need to deal with it here.
    return false;
}

static inline bool
IsReturningFromCallVM(BaselineDebugModeOSRInfo* info)
{
    // Keep this in sync with EmitBranchIsReturningFromCallVM.
    //
    // The stack check entries are returns from a callVM, but have a special
    // kind because they do not exist in a 1-1 relationship with a pc offset.
    return info->frameKind == ICEntry::Kind_CallVM ||
           info->frameKind == ICEntry::Kind_WarmupCounter ||
           info->frameKind == ICEntry::Kind_StackCheck ||
           info->frameKind == ICEntry::Kind_EarlyStackCheck;
}

static void
EmitBranchICEntryKind(MacroAssembler& masm, Register entry, ICEntry::Kind kind, Label* label)
{
    masm.branch32(MacroAssembler::Equal,
                  Address(entry, offsetof(BaselineDebugModeOSRInfo, frameKind)),
                  Imm32(kind), label);
}

static void
EmitBranchIsReturningFromCallVM(MacroAssembler& masm, Register entry, Label* label)
{
    // Keep this in sync with IsReturningFromCallVM.
    EmitBranchICEntryKind(masm, entry, ICEntry::Kind_CallVM, label);
    EmitBranchICEntryKind(masm, entry, ICEntry::Kind_WarmupCounter, label);
    EmitBranchICEntryKind(masm, entry, ICEntry::Kind_StackCheck, label);
    EmitBranchICEntryKind(masm, entry, ICEntry::Kind_EarlyStackCheck, label);
}

static void
SyncBaselineDebugModeOSRInfo(BaselineFrame* frame, Value* vp, bool rv)
{
    AutoUnsafeCallWithABI unsafe;
    BaselineDebugModeOSRInfo* info = frame->debugModeOSRInfo();
    MOZ_ASSERT(info);
    MOZ_ASSERT(frame->script()->baselineScript()->containsCodeAddress(info->resumeAddr));

    if (HasForcedReturn(info, rv)) {
        // Load the frame's rval and overwrite the resume address to go to the
        // epilogue.
        MOZ_ASSERT(R0 == JSReturnOperand);
        info->valueR0 = frame->returnValue();
        info->resumeAddr = frame->script()->baselineScript()->epilogueEntryAddr();
        return;
    }

    // Read stack values and make sure R0 and R1 have the right values if we
    // aren't returning from a callVM.
    //
    // In the case of returning from a callVM, we don't need to restore R0 and
    // R1 ourself since we'll return into code that does it if needed.
    if (!IsReturningFromCallVM(info)) {
        unsigned numUnsynced = info->slotInfo.numUnsynced();
        MOZ_ASSERT(numUnsynced <= 2);
        if (numUnsynced > 0)
            info->popValueInto(info->slotInfo.topSlotLocation(), vp);
        if (numUnsynced > 1)
            info->popValueInto(info->slotInfo.nextSlotLocation(), vp);
    }

    // Scale stackAdjust.
    info->stackAdjust *= sizeof(Value);
}

static void
FinishBaselineDebugModeOSR(BaselineFrame* frame)
{
    AutoUnsafeCallWithABI unsafe;
    frame->deleteDebugModeOSRInfo();

    // We will return to JIT code now so we have to clear the override pc.
    frame->clearOverridePc();
}

void
BaselineFrame::deleteDebugModeOSRInfo()
{
    js_delete(getDebugModeOSRInfo());
    flags_ &= ~HAS_DEBUG_MODE_OSR_INFO;
}

JitCode*
JitRuntime::getBaselineDebugModeOSRHandler(JSContext* cx)
{
    if (!baselineDebugModeOSRHandler_) {
        AutoLockForExclusiveAccess lock(cx);
        AutoAtomsCompartment ac(cx, lock);
        uint32_t offset;
        if (JitCode* code = generateBaselineDebugModeOSRHandler(cx, &offset)) {
            baselineDebugModeOSRHandler_ = code;
            baselineDebugModeOSRHandlerNoFrameRegPopAddr_ = code->raw() + offset;
        }
    }

    return baselineDebugModeOSRHandler_;
}

void*
JitRuntime::getBaselineDebugModeOSRHandlerAddress(JSContext* cx, bool popFrameReg)
{
    if (!getBaselineDebugModeOSRHandler(cx))
        return nullptr;
    return popFrameReg
           ? baselineDebugModeOSRHandler_->raw()
           : baselineDebugModeOSRHandlerNoFrameRegPopAddr_.ref();
}

static void
EmitBaselineDebugModeOSRHandlerTail(MacroAssembler& masm, Register temp, bool returnFromCallVM)
{
    // Save real return address on the stack temporarily.
    //
    // If we're returning from a callVM, we don't need to worry about R0 and
    // R1 but do need to propagate the original ReturnReg value. Otherwise we
    // need to worry about R0 and R1 but can clobber ReturnReg. Indeed, on
    // x86, R1 contains ReturnReg.
    if (returnFromCallVM) {
        masm.push(ReturnReg);
    } else {
        masm.pushValue(Address(temp, offsetof(BaselineDebugModeOSRInfo, valueR0)));
        masm.pushValue(Address(temp, offsetof(BaselineDebugModeOSRInfo, valueR1)));
    }
    masm.push(BaselineFrameReg);
    masm.push(Address(temp, offsetof(BaselineDebugModeOSRInfo, resumeAddr)));

    // Call a stub to free the allocated info.
    masm.setupUnalignedABICall(temp);
    masm.loadBaselineFramePtr(BaselineFrameReg, temp);
    masm.passABIArg(temp);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, FinishBaselineDebugModeOSR));

    // Restore saved values.
    AllocatableGeneralRegisterSet jumpRegs(GeneralRegisterSet::All());
    if (returnFromCallVM) {
        jumpRegs.take(ReturnReg);
    } else {
        jumpRegs.take(R0);
        jumpRegs.take(R1);
    }
    jumpRegs.take(BaselineFrameReg);
    Register target = jumpRegs.takeAny();

    masm.pop(target);
    masm.pop(BaselineFrameReg);
    if (returnFromCallVM) {
        masm.pop(ReturnReg);
    } else {
        masm.popValue(R1);
        masm.popValue(R0);
    }

    masm.jump(target);
}

JitCode*
JitRuntime::generateBaselineDebugModeOSRHandler(JSContext* cx, uint32_t* noFrameRegPopOffsetOut)
{
    MacroAssembler masm(cx);

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(BaselineFrameReg);
    regs.take(ReturnReg);
    Register temp = regs.takeAny();
    Register syncedStackStart = regs.takeAny();

    // Pop the frame reg.
    masm.pop(BaselineFrameReg);

    // Not all patched baseline frames are returning from a situation where
    // the frame reg is already fixed up.
    CodeOffset noFrameRegPopOffset(masm.currentOffset());

    // Record the stack pointer for syncing.
    masm.moveStackPtrTo(syncedStackStart);
    masm.push(ReturnReg);
    masm.push(BaselineFrameReg);

    // Call a stub to fully initialize the info.
    masm.setupUnalignedABICall(temp);
    masm.loadBaselineFramePtr(BaselineFrameReg, temp);
    masm.passABIArg(temp);
    masm.passABIArg(syncedStackStart);
    masm.passABIArg(ReturnReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, SyncBaselineDebugModeOSRInfo));

    // Discard stack values depending on how many were unsynced, as we always
    // have a fully synced stack in the recompile handler. We arrive here via
    // a callVM, and prepareCallVM in BaselineCompiler always fully syncs the
    // stack.
    masm.pop(BaselineFrameReg);
    masm.pop(ReturnReg);
    masm.loadPtr(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfScratchValue()), temp);
    masm.addToStackPtr(Address(temp, offsetof(BaselineDebugModeOSRInfo, stackAdjust)));

    // Emit two tails for the case of returning from a callVM and all other
    // cases, as the state we need to restore differs depending on the case.
    Label returnFromCallVM, end;
    EmitBranchIsReturningFromCallVM(masm, temp, &returnFromCallVM);

    EmitBaselineDebugModeOSRHandlerTail(masm, temp, /* returnFromCallVM = */ false);
    masm.jump(&end);
    masm.bind(&returnFromCallVM);
    EmitBaselineDebugModeOSRHandlerTail(masm, temp, /* returnFromCallVM = */ true);
    masm.bind(&end);

    Linker linker(masm);
    AutoFlushICache afc("BaselineDebugModeOSRHandler");
    JitCode* code = linker.newCode(cx, CodeKind::Other);
    if (!code)
        return nullptr;

    *noFrameRegPopOffsetOut = noFrameRegPopOffset.offset();

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "BaselineDebugModeOSRHandler");
#endif

    return code;
}

/* static */ void
DebugModeOSRVolatileJitFrameIter::forwardLiveIterators(const CooperatingContext& cx,
                                                       uint8_t* oldAddr, uint8_t* newAddr)
{
    DebugModeOSRVolatileJitFrameIter* iter;
    for (iter = cx.context()->liveVolatileJitFrameIter_; iter; iter = iter->prev)
        iter->asJSJit().exchangeReturnAddressIfMatch(oldAddr, newAddr);
}
