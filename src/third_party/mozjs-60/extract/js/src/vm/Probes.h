/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Probes_h
#define vm_Probes_h

#ifdef INCLUDE_MOZILLA_DTRACE
#include "javascript-trace.h"
#endif

#include "vm/Stack.h"

namespace js {

namespace probes {

/*
 * Static probes
 *
 * The probe points defined in this file are scattered around the SpiderMonkey
 * source tree. The presence of probes::SomeEvent() means that someEvent is
 * about to happen or has happened. To the extent possible, probes should be
 * inserted in all paths associated with a given event, regardless of the
 * active runmode (interpreter/traceJIT/methodJIT/ionJIT).
 *
 * When a probe fires, it is handled by any probe handling backends that have
 * been compiled in. By default, most probes do nothing or at least do nothing
 * expensive, so the presence of the probe should have negligible effect on
 * running time. (Probes in slow paths may do something by default, as long as
 * there is no noticeable slowdown.)
 *
 * For some probes, the mere existence of the probe is too expensive even if it
 * does nothing when called. For example, just having consistent information
 * available for a function call entry/exit probe causes the JITs to
 * de-optimize function calls. In those cases, the JITs may query at compile
 * time whether a probe is desired, and omit the probe invocation if not. If a
 * probe is runtime-disabled at compilation time, it is not guaranteed to fire
 * within a compiled function if it is later enabled.
 *
 * Not all backends handle all of the probes listed here.
 */

/*
 * Internal use only: remember whether "profiling", whatever that means, is
 * currently active. Used for state management.
 */
extern bool ProfilingActive;

extern const char nullName[];
extern const char anonymousName[];

/*
 * Test whether we are tracking JS function call enter/exit. The JITs use this
 * to decide whether they can optimize in a way that would prevent probes from
 * firing.
 */
bool CallTrackingActive(JSContext*);

/* Entering a JS function */
bool EnterScript(JSContext*, JSScript*, JSFunction*, InterpreterFrame*);

/* About to leave a JS function */
void ExitScript(JSContext*, JSScript*, JSFunction*, bool popProfilerFrame);

/* Executing a script */
bool StartExecution(JSScript* script);

/* Script has completed execution */
bool StopExecution(JSScript* script);

/*
 * Object has been created. |obj| must exist (its class and size are read)
 */
bool CreateObject(JSContext* cx, JSObject* obj);

/*
 * Object is about to be finalized. |obj| must still exist (its class is
 * read)
 */
bool FinalizeObject(JSObject* obj);

/*
 * Internal: DTrace-specific functions to be called during probes::EnterScript
 * and probes::ExitScript. These will not be inlined, but the argument
 * marshalling required for these probe points is expensive enough that it
 * shouldn't really matter.
 */
void DTraceEnterJSFun(JSContext* cx, JSFunction* fun, JSScript* script);
void DTraceExitJSFun(JSContext* cx, JSFunction* fun, JSScript* script);

} // namespace probes


#ifdef INCLUDE_MOZILLA_DTRACE
static const char* ObjectClassname(JSObject* obj) {
    if (!obj)
        return "(null object)";
    const Class* clasp = obj->getClass();
    if (!clasp)
        return "(null)";
    const char* class_name = clasp->name;
    if (!class_name)
        return "(null class name)";
    return class_name;
}
#endif

inline bool
probes::CreateObject(JSContext* cx, JSObject* obj)
{
    bool ok = true;

#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_OBJECT_CREATE_ENABLED())
        JAVASCRIPT_OBJECT_CREATE(ObjectClassname(obj), (uintptr_t)obj);
#endif

    return ok;
}

inline bool
probes::FinalizeObject(JSObject* obj)
{
    bool ok = true;

#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_OBJECT_FINALIZE_ENABLED()) {
        const Class* clasp = obj->getClass();

        /* the first arg is nullptr - reserved for future use (filename?) */
        JAVASCRIPT_OBJECT_FINALIZE(nullptr, (char*)clasp->name, (uintptr_t)obj);
    }
#endif

    return ok;
}

} /* namespace js */

#endif /* vm_Probes_h */
