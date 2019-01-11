/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Debugger_inl_h
#define vm_Debugger_inl_h

#include "vm/Debugger.h"

#include "vm/Stack-inl.h"

/* static */ inline bool
js::Debugger::onLeaveFrame(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc, bool ok)
{
    MOZ_ASSERT_IF(frame.isInterpreterFrame(), frame.asInterpreterFrame() == cx->interpreterFrame());
    MOZ_ASSERT_IF(frame.hasScript() && frame.script()->isDebuggee(), frame.isDebuggee());
    /* Traps must be cleared from eval frames, see slowPathOnLeaveFrame. */
    mozilla::DebugOnly<bool> evalTraps = frame.isEvalFrame() &&
                                         frame.script()->hasAnyBreakpointsOrStepMode();
    MOZ_ASSERT_IF(evalTraps, frame.isDebuggee());
    if (frame.isDebuggee())
        ok = slowPathOnLeaveFrame(cx, frame, pc, ok);
    MOZ_ASSERT(!inFrameMaps(frame));
    return ok;
}

/* static */ inline js::Debugger*
js::Debugger::fromJSObject(const JSObject* obj)
{
    MOZ_ASSERT(js::GetObjectClass(obj) == &class_);
    return (Debugger*) obj->as<NativeObject>().getPrivate();
}

/* static */ inline bool
js::Debugger::checkNoExecute(JSContext* cx, HandleScript script)
{
    if (!cx->compartment()->isDebuggee() || !cx->noExecuteDebuggerTop)
        return true;
    return slowPathCheckNoExecute(cx, script);
}

/* static */ JSTrapStatus
js::Debugger::onEnterFrame(JSContext* cx, AbstractFramePtr frame)
{
    MOZ_ASSERT_IF(frame.hasScript() && frame.script()->isDebuggee(), frame.isDebuggee());
    if (!frame.isDebuggee())
        return JSTRAP_CONTINUE;
    return slowPathOnEnterFrame(cx, frame);
}

/* static */ JSTrapStatus
js::Debugger::onDebuggerStatement(JSContext* cx, AbstractFramePtr frame)
{
    if (!cx->compartment()->isDebuggee())
        return JSTRAP_CONTINUE;
    return slowPathOnDebuggerStatement(cx, frame);
}

/* static */ JSTrapStatus
js::Debugger::onExceptionUnwind(JSContext* cx, AbstractFramePtr frame)
{
    if (!cx->compartment()->isDebuggee())
        return JSTRAP_CONTINUE;
    return slowPathOnExceptionUnwind(cx, frame);
}

/* static */ void
js::Debugger::onNewWasmInstance(JSContext* cx, Handle<WasmInstanceObject*> wasmInstance)
{
    if (cx->compartment()->isDebuggee())
        slowPathOnNewWasmInstance(cx, wasmInstance);
}

/* static */ void
js::Debugger::onNewPromise(JSContext* cx, Handle<PromiseObject*> promise)
{
    if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
        slowPathPromiseHook(cx, Debugger::OnNewPromise, promise);
}

/* static */ void
js::Debugger::onPromiseSettled(JSContext* cx, Handle<PromiseObject*> promise)
{
    if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
        slowPathPromiseHook(cx, Debugger::OnPromiseSettled, promise);
}

inline bool
js::Debugger::getScriptFrame(JSContext* cx, const FrameIter& iter,
                             MutableHandle<DebuggerFrame*> result)
{
    return getScriptFrameWithIter(cx, iter.abstractFramePtr(), &iter, result);
}

inline js::Debugger*
js::DebuggerEnvironment::owner() const
{
    JSObject* dbgobj = &getReservedSlot(OWNER_SLOT).toObject();
    return Debugger::fromJSObject(dbgobj);
}

inline js::Debugger*
js::DebuggerFrame::owner() const
{
    JSObject* dbgobj = &getReservedSlot(OWNER_SLOT).toObject();
    return Debugger::fromJSObject(dbgobj);
}

inline js::Debugger*
js::DebuggerObject::owner() const
{
    JSObject* dbgobj = &getReservedSlot(OWNER_SLOT).toObject();
    return Debugger::fromJSObject(dbgobj);
}

inline js::PromiseObject*
js::DebuggerObject::promise() const
{
    MOZ_ASSERT(isPromise());

    JSObject* referent = this->referent();
    if (IsCrossCompartmentWrapper(referent)) {
        referent = CheckedUnwrap(referent);
        MOZ_ASSERT(referent);
    }

    return &referent->as<PromiseObject>();
}

#endif /* vm_Debugger_inl_h */
