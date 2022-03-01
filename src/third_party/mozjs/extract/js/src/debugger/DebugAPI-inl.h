/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_DebugAPI_inl_h
#define debugger_DebugAPI_inl_h

#include "debugger/DebugAPI.h"

#include "vm/GeneratorObject.h"
#include "vm/PromiseObject.h"  // js::PromiseObject

#include "vm/Stack-inl.h"

namespace js {

/* static */
bool DebugAPI::stepModeEnabled(JSScript* script) {
  return script->hasDebugScript() && stepModeEnabledSlow(script);
}

/* static */
bool DebugAPI::hasBreakpointsAt(JSScript* script, jsbytecode* pc) {
  return script->hasDebugScript() && hasBreakpointsAtSlow(script, pc);
}

/* static */
bool DebugAPI::hasAnyBreakpointsOrStepMode(JSScript* script) {
  return script->hasDebugScript();
}

/* static */
void DebugAPI::onNewGlobalObject(JSContext* cx, Handle<GlobalObject*> global) {
  MOZ_ASSERT(!global->realm()->firedOnNewGlobalObject);
#ifdef DEBUG
  global->realm()->firedOnNewGlobalObject = true;
#endif
  if (!cx->runtime()->onNewGlobalObjectWatchers().isEmpty()) {
    slowPathOnNewGlobalObject(cx, global);
  }
}

/* static */
void DebugAPI::notifyParticipatesInGC(GlobalObject* global,
                                      uint64_t majorGCNumber) {
  Realm::DebuggerVector& dbgs = global->getDebuggers();
  if (!dbgs.empty()) {
    slowPathNotifyParticipatesInGC(majorGCNumber, dbgs);
  }
}

/* static */
bool DebugAPI::onLogAllocationSite(JSContext* cx, JSObject* obj,
                                   HandleSavedFrame frame,
                                   mozilla::TimeStamp when) {
  Realm::DebuggerVector& dbgs = cx->global()->getDebuggers();
  if (dbgs.empty()) {
    return true;
  }
  RootedObject hobj(cx, obj);
  return slowPathOnLogAllocationSite(cx, hobj, frame, when, dbgs);
}

/* static */
bool DebugAPI::onLeaveFrame(JSContext* cx, AbstractFramePtr frame,
                            jsbytecode* pc, bool ok) {
  MOZ_ASSERT_IF(frame.isInterpreterFrame(),
                frame.asInterpreterFrame() == cx->interpreterFrame());
  MOZ_ASSERT_IF(frame.hasScript() && frame.script()->isDebuggee(),
                frame.isDebuggee());
  /* Traps must be cleared from eval frames, see slowPathOnLeaveFrame. */
  mozilla::DebugOnly<bool> evalTraps =
      frame.isEvalFrame() && frame.script()->hasDebugScript();
  MOZ_ASSERT_IF(evalTraps, frame.isDebuggee());
  if (frame.isDebuggee()) {
    ok = slowPathOnLeaveFrame(cx, frame, pc, ok);
  }
  MOZ_ASSERT(!inFrameMaps(frame));
  return ok;
}

/* static */
bool DebugAPI::onNewGenerator(JSContext* cx, AbstractFramePtr frame,
                              Handle<AbstractGeneratorObject*> genObj) {
  if (frame.isDebuggee()) {
    return slowPathOnNewGenerator(cx, frame, genObj);
  }
  return true;
}

/* static */
bool DebugAPI::checkNoExecute(JSContext* cx, HandleScript script) {
  if (!cx->realm()->isDebuggee() || !cx->noExecuteDebuggerTop) {
    return true;
  }
  return slowPathCheckNoExecute(cx, script);
}

/* static */
bool DebugAPI::onEnterFrame(JSContext* cx, AbstractFramePtr frame) {
  MOZ_ASSERT_IF(frame.hasScript() && frame.script()->isDebuggee(),
                frame.isDebuggee());
  if (MOZ_UNLIKELY(frame.isDebuggee())) {
    return slowPathOnEnterFrame(cx, frame);
  }
  return true;
}

/* static */
bool DebugAPI::onResumeFrame(JSContext* cx, AbstractFramePtr frame) {
  MOZ_ASSERT_IF(frame.hasScript() && frame.script()->isDebuggee(),
                frame.isDebuggee());
  if (MOZ_UNLIKELY(frame.isDebuggee())) {
    return slowPathOnResumeFrame(cx, frame);
  }
  return true;
}

/* static */
NativeResumeMode DebugAPI::onNativeCall(JSContext* cx, const CallArgs& args,
                                        CallReason reason) {
  if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
    return slowPathOnNativeCall(cx, args, reason);
  }

  return NativeResumeMode::Continue;
}

/* static */
bool DebugAPI::onDebuggerStatement(JSContext* cx, AbstractFramePtr frame) {
  if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
    return slowPathOnDebuggerStatement(cx, frame);
  }

  return true;
}

/* static */
bool DebugAPI::onExceptionUnwind(JSContext* cx, AbstractFramePtr frame) {
  if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
    return slowPathOnExceptionUnwind(cx, frame);
  }
  return true;
}

/* static */
void DebugAPI::onNewWasmInstance(JSContext* cx,
                                 Handle<WasmInstanceObject*> wasmInstance) {
  if (cx->realm()->isDebuggee()) {
    slowPathOnNewWasmInstance(cx, wasmInstance);
  }
}

/* static */
void DebugAPI::onNewPromise(JSContext* cx, Handle<PromiseObject*> promise) {
  if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
    slowPathOnNewPromise(cx, promise);
  }
}

/* static */
void DebugAPI::onPromiseSettled(JSContext* cx, Handle<PromiseObject*> promise) {
  if (MOZ_UNLIKELY(promise->realm()->isDebuggee())) {
    slowPathOnPromiseSettled(cx, promise);
  }
}

/* static */
void DebugAPI::traceGeneratorFrame(JSTracer* tracer,
                                   AbstractGeneratorObject* generator) {
  if (MOZ_UNLIKELY(generator->realm()->isDebuggee())) {
    slowPathTraceGeneratorFrame(tracer, generator);
  }
}

}  // namespace js

#endif /* debugger_DebugAPI_inl_h */
