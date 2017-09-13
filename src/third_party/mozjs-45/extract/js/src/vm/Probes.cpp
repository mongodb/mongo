/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Probes-inl.h"

#include "jscntxt.h"

#ifdef INCLUDE_MOZILLA_DTRACE
#include "jsscriptinlines.h"
#endif

#define TYPEOF(cx,v)    (v.isNull() ? JSTYPE_NULL : JS_TypeOfValue(cx,v))

using namespace js;

const char probes::nullName[] = "(null)";
const char probes::anonymousName[] = "(anonymous)";

bool probes::ProfilingActive = true;

#ifdef INCLUDE_MOZILLA_DTRACE
static const char*
ScriptFilename(const JSScript* script)
{
    if (!script)
        return probes::nullName;
    if (!script->filename())
        return probes::anonymousName;
    return script->filename();
}

static const char*
FunctionName(JSContext* cx, JSFunction* fun, JSAutoByteString* bytes)
{
    if (!fun)
        return probes::nullName;
    if (!fun->displayAtom())
        return probes::anonymousName;
    return bytes->encodeLatin1(cx, fun->displayAtom()) ? bytes->ptr() : probes::nullName;
}

/*
 * These functions call the DTrace macros for the JavaScript USDT probes.
 * Originally this code was inlined in the JavaScript code; however since
 * a number of operations are called, these have been placed into functions
 * to reduce any negative compiler optimization effect that the addition of
 * a number of usually unused lines of code would cause.
 */
void
probes::DTraceEnterJSFun(JSContext* cx, JSFunction* fun, JSScript* script)
{
    JSAutoByteString funNameBytes;
    JAVASCRIPT_FUNCTION_ENTRY(ScriptFilename(script), probes::nullName,
                              FunctionName(cx, fun, &funNameBytes));
}

void
probes::DTraceExitJSFun(JSContext* cx, JSFunction* fun, JSScript* script)
{
    JSAutoByteString funNameBytes;
    JAVASCRIPT_FUNCTION_RETURN(ScriptFilename(script), probes::nullName,
                               FunctionName(cx, fun, &funNameBytes));
}
#endif
