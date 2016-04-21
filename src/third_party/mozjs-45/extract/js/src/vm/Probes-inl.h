/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Probes_inl_h
#define vm_Probes_inl_h

#include "vm/Probes.h"

#include "jscntxt.h"

namespace js {

/*
 * Many probe handlers are implemented inline for minimal performance impact,
 * especially important when no backends are enabled.
 */

inline bool
probes::CallTrackingActive(JSContext* cx)
{
#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_FUNCTION_ENTRY_ENABLED() || JAVASCRIPT_FUNCTION_RETURN_ENABLED())
        return true;
#endif
    return false;
}

inline bool
probes::EnterScript(JSContext* cx, JSScript* script, JSFunction* maybeFun,
                    InterpreterFrame* fp)
{
#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_FUNCTION_ENTRY_ENABLED())
        DTraceEnterJSFun(cx, maybeFun, script);
#endif

    JSRuntime* rt = cx->runtime();
    if (rt->spsProfiler.enabled()) {
        if (!rt->spsProfiler.enter(cx, script, maybeFun))
            return false;
        MOZ_ASSERT_IF(!fp->script()->isGenerator(), !fp->hasPushedSPSFrame());
        fp->setPushedSPSFrame();
    }

    return true;
}

inline void
probes::ExitScript(JSContext* cx, JSScript* script, JSFunction* maybeFun, bool popSPSFrame)
{
#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_FUNCTION_RETURN_ENABLED())
        DTraceExitJSFun(cx, maybeFun, script);
#endif

    if (popSPSFrame)
        cx->runtime()->spsProfiler.exit(script, maybeFun);
}

inline bool
probes::StartExecution(JSScript* script)
{
    bool ok = true;

#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_EXECUTE_START_ENABLED())
        JAVASCRIPT_EXECUTE_START((script->filename() ? (char*)script->filename() : nullName),
                                 script->lineno());
#endif

    return ok;
}

inline bool
probes::StopExecution(JSScript* script)
{
    bool ok = true;

#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_EXECUTE_DONE_ENABLED())
        JAVASCRIPT_EXECUTE_DONE((script->filename() ? (char*)script->filename() : nullName),
                                script->lineno());
#endif

    return ok;
}

} /* namespace js */

#endif /* vm_Probes_inl_h */
