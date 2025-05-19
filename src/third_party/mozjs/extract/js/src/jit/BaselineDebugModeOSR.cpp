/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineDebugModeOSR.h"

#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/Invalidation.h"
#include "jit/IonScript.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JSJitFrameIter.h"

#include "jit/JitScript-inl.h"
#include "jit/JSJitFrameIter-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Realm-inl.h"

using namespace js;
using namespace js::jit;

struct DebugModeOSREntry {
  JSScript* script;
  BaselineScript* oldBaselineScript;
  uint32_t pcOffset;
  RetAddrEntry::Kind frameKind;

  explicit DebugModeOSREntry(JSScript* script)
      : script(script),
        oldBaselineScript(script->baselineScript()),
        pcOffset(uint32_t(-1)),
        frameKind(RetAddrEntry::Kind::Invalid) {}

  DebugModeOSREntry(JSScript* script, const RetAddrEntry& retAddrEntry)
      : script(script),
        oldBaselineScript(script->baselineScript()),
        pcOffset(retAddrEntry.pcOffset()),
        frameKind(retAddrEntry.kind()) {
#ifdef DEBUG
    MOZ_ASSERT(pcOffset == retAddrEntry.pcOffset());
    MOZ_ASSERT(frameKind == retAddrEntry.kind());
#endif
  }

  DebugModeOSREntry(DebugModeOSREntry&& other)
      : script(other.script),
        oldBaselineScript(other.oldBaselineScript),
        pcOffset(other.pcOffset),
        frameKind(other.frameKind) {}

  bool recompiled() const {
    return oldBaselineScript != script->baselineScript();
  }
};

using DebugModeOSREntryVector = Vector<DebugModeOSREntry>;

class UniqueScriptOSREntryIter {
  const DebugModeOSREntryVector& entries_;
  size_t index_;

 public:
  explicit UniqueScriptOSREntryIter(const DebugModeOSREntryVector& entries)
      : entries_(entries), index_(0) {}

  bool done() { return index_ == entries_.length(); }

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
      if (unique) {
        break;
      }
    }
    return *this;
  }
};

static bool CollectJitStackScripts(JSContext* cx,
                                   const DebugAPI::ExecutionObservableSet& obs,
                                   const ActivationIterator& activation,
                                   DebugModeOSREntryVector& entries) {
  for (OnlyJSJitFrameIter iter(activation); !iter.done(); ++iter) {
    const JSJitFrameIter& frame = iter.frame();
    switch (frame.type()) {
      case FrameType::BaselineJS: {
        JSScript* script = frame.script();

        if (!obs.shouldRecompileOrInvalidate(script)) {
          break;
        }

        BaselineFrame* baselineFrame = frame.baselineFrame();

        if (baselineFrame->runningInInterpreter()) {
          // Baseline Interpreter frames for scripts that have a BaselineScript
          // or IonScript don't need to be patched but they do need to be
          // invalidated and recompiled. See also CollectInterpreterStackScripts
          // for C++ interpreter frames.
          if (!entries.append(DebugModeOSREntry(script))) {
            return false;
          }
        } else {
          // The frame must be settled on a pc with a RetAddrEntry.
          uint8_t* retAddr = frame.resumePCinCurrentFrame();
          const RetAddrEntry& retAddrEntry =
              script->baselineScript()->retAddrEntryFromReturnAddress(retAddr);
          if (!entries.append(DebugModeOSREntry(script, retAddrEntry))) {
            return false;
          }
        }

        break;
      }

      case FrameType::BaselineStub:
        break;

      case FrameType::IonJS: {
        InlineFrameIterator inlineIter(cx, &frame);
        while (true) {
          if (obs.shouldRecompileOrInvalidate(inlineIter.script())) {
            if (!entries.append(DebugModeOSREntry(inlineIter.script()))) {
              return false;
            }
          }
          if (!inlineIter.more()) {
            break;
          }
          ++inlineIter;
        }
        break;
      }

      default:;
    }
  }

  return true;
}

static bool CollectInterpreterStackScripts(
    JSContext* cx, const DebugAPI::ExecutionObservableSet& obs,
    const ActivationIterator& activation, DebugModeOSREntryVector& entries) {
  // Collect interpreter frame stacks with IonScript or BaselineScript as
  // well. These do not need to be patched, but do need to be invalidated
  // and recompiled.
  InterpreterActivation* act = activation.activation()->asInterpreter();
  for (InterpreterFrameIterator iter(act); !iter.done(); ++iter) {
    JSScript* script = iter.frame()->script();
    if (obs.shouldRecompileOrInvalidate(script)) {
      if (!entries.append(DebugModeOSREntry(iter.frame()->script()))) {
        return false;
      }
    }
  }
  return true;
}

#ifdef JS_JITSPEW
static const char* RetAddrEntryKindToString(RetAddrEntry::Kind kind) {
  switch (kind) {
    case RetAddrEntry::Kind::IC:
      return "IC";
    case RetAddrEntry::Kind::CallVM:
      return "callVM";
    case RetAddrEntry::Kind::StackCheck:
      return "stack check";
    case RetAddrEntry::Kind::InterruptCheck:
      return "interrupt check";
    case RetAddrEntry::Kind::DebugTrap:
      return "debug trap";
    case RetAddrEntry::Kind::DebugPrologue:
      return "debug prologue";
    case RetAddrEntry::Kind::DebugAfterYield:
      return "debug after yield";
    case RetAddrEntry::Kind::DebugEpilogue:
      return "debug epilogue";
    default:
      MOZ_CRASH("bad RetAddrEntry kind");
  }
}
#endif  // JS_JITSPEW

static void SpewPatchBaselineFrame(const uint8_t* oldReturnAddress,
                                   const uint8_t* newReturnAddress,
                                   JSScript* script,
                                   RetAddrEntry::Kind frameKind,
                                   const jsbytecode* pc) {
  JitSpew(JitSpew_BaselineDebugModeOSR,
          "Patch return %p -> %p on BaselineJS frame (%s:%u:%u) from %s at %s",
          oldReturnAddress, newReturnAddress, script->filename(),
          script->lineno(), script->column().oneOriginValue(),
          RetAddrEntryKindToString(frameKind), CodeName(JSOp(*pc)));
}

static void PatchBaselineFramesForDebugMode(
    JSContext* cx, const DebugAPI::ExecutionObservableSet& obs,
    const ActivationIterator& activation, DebugModeOSREntryVector& entries,
    size_t* start) {
  //
  // Recompile Patching Overview
  //
  // When toggling debug mode with live baseline scripts on the stack, we
  // could have entered the VM via the following ways from the baseline
  // script.
  //
  // Off to On:
  //  A. From a non-prologue IC (fallback stub or "can call" stub).
  //  B. From a VM call.
  //  C. From inside the interrupt handler via the prologue stack check.
  //
  // On to Off:
  //  - All the ways above.
  //  D. From the debug trap handler.
  //  E. From the debug prologue.
  //  F. From the debug epilogue.
  //  G. From a JSOp::AfterYield instruction.
  //
  // In general, we patch the return address from VM calls and ICs to the
  // corresponding entry in the recompiled BaselineScript. For entries that are
  // not present in the recompiled script (cases D to G above) we switch the
  // frame to interpreter mode and resume in the Baseline Interpreter.
  //
  // Specifics on what needs to be done are documented below.
  //

  const BaselineInterpreter& baselineInterp =
      cx->runtime()->jitRuntime()->baselineInterpreter();

  CommonFrameLayout* prev = nullptr;
  size_t entryIndex = *start;

  for (OnlyJSJitFrameIter iter(activation); !iter.done(); ++iter) {
    const JSJitFrameIter& frame = iter.frame();
    switch (frame.type()) {
      case FrameType::BaselineJS: {
        // If the script wasn't recompiled or is not observed, there's
        // nothing to patch.
        if (!obs.shouldRecompileOrInvalidate(frame.script())) {
          break;
        }

        DebugModeOSREntry& entry = entries[entryIndex];

        if (!entry.recompiled()) {
          entryIndex++;
          break;
        }

        BaselineFrame* baselineFrame = frame.baselineFrame();
        if (baselineFrame->runningInInterpreter()) {
          // We recompiled the script's BaselineScript but Baseline Interpreter
          // frames don't need to be patched.
          entryIndex++;
          break;
        }

        JSScript* script = entry.script;
        uint32_t pcOffset = entry.pcOffset;
        jsbytecode* pc = script->offsetToPC(pcOffset);

        MOZ_ASSERT(script == frame.script());
        MOZ_ASSERT(pcOffset < script->length());

        BaselineScript* bl = script->baselineScript();
        RetAddrEntry::Kind kind = entry.frameKind;
        uint8_t* retAddr = nullptr;
        switch (kind) {
          case RetAddrEntry::Kind::IC:
          case RetAddrEntry::Kind::CallVM:
          case RetAddrEntry::Kind::InterruptCheck:
          case RetAddrEntry::Kind::StackCheck: {
            // Cases A, B, C above.
            //
            // For the baseline frame here, we resume right after the CallVM or
            // IC returns.
            //
            // For CallVM (case B) the assumption is that all callVMs which can
            // trigger debug mode OSR are the *only* callVMs generated for their
            // respective pc locations in the Baseline JIT code.
            const RetAddrEntry* retAddrEntry = nullptr;
            switch (kind) {
              case RetAddrEntry::Kind::IC:
              case RetAddrEntry::Kind::CallVM:
              case RetAddrEntry::Kind::InterruptCheck:
                retAddrEntry = &bl->retAddrEntryFromPCOffset(pcOffset, kind);
                break;
              case RetAddrEntry::Kind::StackCheck:
                retAddrEntry = &bl->prologueRetAddrEntry(kind);
                break;
              default:
                MOZ_CRASH("Unexpected kind");
            }
            retAddr = bl->returnAddressForEntry(*retAddrEntry);
            SpewPatchBaselineFrame(prev->returnAddress(), retAddr, script, kind,
                                   pc);
            break;
          }
          case RetAddrEntry::Kind::DebugPrologue:
          case RetAddrEntry::Kind::DebugEpilogue:
          case RetAddrEntry::Kind::DebugTrap:
          case RetAddrEntry::Kind::DebugAfterYield: {
            // Cases D, E, F, G above.
            //
            // Resume in the Baseline Interpreter because these callVMs are not
            // present in the new BaselineScript if we recompiled without debug
            // instrumentation.
            if (kind == RetAddrEntry::Kind::DebugPrologue) {
              frame.baselineFrame()->switchFromJitToInterpreterAtPrologue(cx);
            } else {
              frame.baselineFrame()->switchFromJitToInterpreter(cx, pc);
            }
            switch (kind) {
              case RetAddrEntry::Kind::DebugTrap:
                // DebugTrap handling is different from the ones below because
                // it's not a callVM but a trampoline call at the start of the
                // bytecode op. When we return to the frame we can resume at the
                // interpretOp label.
                retAddr = baselineInterp.interpretOpAddr().value;
                break;
              case RetAddrEntry::Kind::DebugPrologue:
                retAddr = baselineInterp.retAddrForDebugPrologueCallVM();
                break;
              case RetAddrEntry::Kind::DebugEpilogue:
                retAddr = baselineInterp.retAddrForDebugEpilogueCallVM();
                break;
              case RetAddrEntry::Kind::DebugAfterYield:
                retAddr = baselineInterp.retAddrForDebugAfterYieldCallVM();
                break;
              default:
                MOZ_CRASH("Unexpected kind");
            }
            SpewPatchBaselineFrame(prev->returnAddress(), retAddr, script, kind,
                                   pc);
            break;
          }
          case RetAddrEntry::Kind::NonOpCallVM:
          case RetAddrEntry::Kind::Invalid:
            // These cannot trigger BaselineDebugModeOSR.
            MOZ_CRASH("Unexpected RetAddrEntry Kind");
        }

        prev->setReturnAddress(retAddr);
        entryIndex++;
        break;
      }

      case FrameType::IonJS: {
        // Nothing to patch.
        InlineFrameIterator inlineIter(cx, &frame);
        while (true) {
          if (obs.shouldRecompileOrInvalidate(inlineIter.script())) {
            entryIndex++;
          }
          if (!inlineIter.more()) {
            break;
          }
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

static void SkipInterpreterFrameEntries(
    const DebugAPI::ExecutionObservableSet& obs,
    const ActivationIterator& activation, size_t* start) {
  size_t entryIndex = *start;

  // Skip interpreter frames, which do not need patching.
  InterpreterActivation* act = activation.activation()->asInterpreter();
  for (InterpreterFrameIterator iter(act); !iter.done(); ++iter) {
    if (obs.shouldRecompileOrInvalidate(iter.frame()->script())) {
      entryIndex++;
    }
  }

  *start = entryIndex;
}

static bool RecompileBaselineScriptForDebugMode(
    JSContext* cx, JSScript* script, DebugAPI::IsObserving observing) {
  // If a script is on the stack multiple times, it may have already
  // been recompiled.
  if (script->baselineScript()->hasDebugInstrumentation() == observing) {
    return true;
  }

  JitSpew(JitSpew_BaselineDebugModeOSR, "Recompiling (%s:%u:%u) for %s",
          script->filename(), script->lineno(),
          script->column().oneOriginValue(),
          observing ? "DEBUGGING" : "NORMAL EXECUTION");

  AutoKeepJitScripts keepJitScripts(cx);
  BaselineScript* oldBaselineScript =
      script->jitScript()->clearBaselineScript(cx->gcContext(), script);

  MethodStatus status =
      BaselineCompile(cx, script, /* forceDebugMode = */ observing);
  if (status != Method_Compiled) {
    // We will only fail to recompile for debug mode due to OOM. Restore
    // the old baseline script in case something doesn't properly
    // propagate OOM.
    MOZ_ASSERT(status == Method_Error);
    script->jitScript()->setBaselineScript(script, oldBaselineScript);
    return false;
  }

  // Don't destroy the old baseline script yet, since if we fail any of the
  // recompiles we need to rollback all the old baseline scripts.
  MOZ_ASSERT(script->baselineScript()->hasDebugInstrumentation() == observing);
  return true;
}

static bool InvalidateScriptsInZone(JSContext* cx, Zone* zone,
                                    const Vector<DebugModeOSREntry>& entries) {
  RecompileInfoVector invalid;
  for (UniqueScriptOSREntryIter iter(entries); !iter.done(); ++iter) {
    JSScript* script = iter.entry().script;
    if (script->zone() != zone) {
      continue;
    }

    if (script->hasIonScript()) {
      if (!invalid.emplaceBack(script, script->ionScript()->compilationId())) {
        ReportOutOfMemory(cx);
        return false;
      }
    }

    // Cancel off-thread Ion compile for anything that has a
    // BaselineScript. If we relied on the call to Invalidate below to
    // cancel off-thread Ion compiles, only those with existing IonScripts
    // would be cancelled.
    if (script->hasBaselineScript()) {
      CancelOffThreadIonCompile(script);
    }
  }

  // No need to cancel off-thread Ion compiles again, we already did it
  // above.
  Invalidate(cx, invalid,
             /* resetUses = */ true, /* cancelOffThread = */ false);
  return true;
}

static void UndoRecompileBaselineScriptsForDebugMode(
    JSContext* cx, const DebugModeOSREntryVector& entries) {
  // In case of failure, roll back the entire set of active scripts so that
  // we don't have to patch return addresses on the stack.
  for (UniqueScriptOSREntryIter iter(entries); !iter.done(); ++iter) {
    const DebugModeOSREntry& entry = iter.entry();
    JSScript* script = entry.script;
    if (entry.recompiled()) {
      BaselineScript* baselineScript =
          script->jitScript()->clearBaselineScript(cx->gcContext(), script);
      script->jitScript()->setBaselineScript(script, entry.oldBaselineScript);
      BaselineScript::Destroy(cx->gcContext(), baselineScript);
    }
  }
}

bool jit::RecompileOnStackBaselineScriptsForDebugMode(
    JSContext* cx, const DebugAPI::ExecutionObservableSet& obs,
    DebugAPI::IsObserving observing) {
  // First recompile the active scripts on the stack and patch the live
  // frames.
  Vector<DebugModeOSREntry> entries(cx);

  for (ActivationIterator iter(cx); !iter.done(); ++iter) {
    if (iter->isJit()) {
      if (!CollectJitStackScripts(cx, obs, iter, entries)) {
        return false;
      }
    } else if (iter->isInterpreter()) {
      if (!CollectInterpreterStackScripts(cx, obs, iter, entries)) {
        return false;
      }
    }
  }

  if (entries.empty()) {
    return true;
  }

  // When the profiler is enabled, we need to have suppressed sampling,
  // since the basline jit scripts are in a state of flux.
  MOZ_ASSERT(!cx->isProfilerSamplingEnabled());

  // Invalidate all scripts we are recompiling.
  if (Zone* zone = obs.singleZone()) {
    if (!InvalidateScriptsInZone(cx, zone, entries)) {
      return false;
    }
  } else {
    using ZoneRange = DebugAPI::ExecutionObservableSet::ZoneRange;
    for (ZoneRange r = obs.zones()->all(); !r.empty(); r.popFront()) {
      if (!InvalidateScriptsInZone(cx, r.front(), entries)) {
        return false;
      }
    }
  }

  // Try to recompile all the scripts. If we encounter an error, we need to
  // roll back as if none of the compilations happened, so that we don't
  // crash.
  for (size_t i = 0; i < entries.length(); i++) {
    JSScript* script = entries[i].script;
    AutoRealm ar(cx, script);
    if (!RecompileBaselineScriptForDebugMode(cx, script, observing)) {
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
    if (entry.recompiled()) {
      BaselineScript::Destroy(cx->gcContext(), entry.oldBaselineScript);
    }
  }

  size_t processed = 0;
  for (ActivationIterator iter(cx); !iter.done(); ++iter) {
    if (iter->isJit()) {
      PatchBaselineFramesForDebugMode(cx, obs, iter, entries, &processed);
    } else if (iter->isInterpreter()) {
      SkipInterpreterFrameEntries(obs, iter, &processed);
    }
  }
  MOZ_ASSERT(processed == entries.length());

  return true;
}
