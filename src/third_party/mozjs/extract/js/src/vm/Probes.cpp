/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Probes-inl.h"

#ifdef INCLUDE_MOZILLA_DTRACE
#  include "vm/JSScript-inl.h"
#endif

using namespace js;

const char probes::nullName[] = "(null)";
const char probes::anonymousName[] = "(anonymous)";

bool probes::ProfilingActive = true;

#ifdef INCLUDE_MOZILLA_DTRACE
static const char* ScriptFilename(const JSScript* script) {
  if (!script) {
    return probes::nullName;
  }
  if (!script->filename()) {
    return probes::anonymousName;
  }
  return script->filename();
}

static const char* FunctionName(JSContext* cx, JSFunction* fun,
                                UniqueChars* bytes) {
  if (!fun) {
    return probes::nullName;
  }
  if (!fun->maybePartialDisplayAtom()) {
    return probes::anonymousName;
  }
  // TODO: Should be JS_EncodeStringToUTF8, but that'd introduce a rooting
  // hazard, because JS_EncodeStringToUTF8 can GC.
  *bytes = JS_EncodeStringToLatin1(cx, fun->maybePartialDisplayAtom());
  return *bytes ? bytes->get() : probes::nullName;
}

/*
 * These functions call the DTrace macros for the JavaScript USDT probes.
 * Originally this code was inlined in the JavaScript code; however since
 * a number of operations are called, these have been placed into functions
 * to reduce any negative compiler optimization effect that the addition of
 * a number of usually unused lines of code would cause.
 */
void probes::DTraceEnterJSFun(JSContext* cx, JSFunction* fun,
                              JSScript* script) {
  UniqueChars funNameBytes;
  JAVASCRIPT_FUNCTION_ENTRY(ScriptFilename(script), probes::nullName,
                            FunctionName(cx, fun, &funNameBytes));
}

void probes::DTraceExitJSFun(JSContext* cx, JSFunction* fun, JSScript* script) {
  UniqueChars funNameBytes;
  JAVASCRIPT_FUNCTION_RETURN(ScriptFilename(script), probes::nullName,
                             FunctionName(cx, fun, &funNameBytes));
}
#endif
