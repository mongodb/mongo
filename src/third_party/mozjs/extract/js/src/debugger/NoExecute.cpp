/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "debugger/NoExecute.h"

#include "mozilla/Sprintf.h"  // for SprintfLiteral

#include <stdio.h>  // for fprintf, stdout

#include "debugger/Debugger.h"        // for Debugger
#include "js/friend/DumpFunctions.h"  // for DumpBacktrace
#include "js/friend/ErrorMessages.h"  // for GetErrorMessage, JSMSG_DEBUGGEE_WOULD_RUN
#include "js/Promise.h"               // for AutoDebuggerJobQueueInterruption
#include "js/RootingAPI.h"            // for Handle
#include "vm/JSContext.h"             // for ProtectedDataContextArg, JSContext
#include "vm/JSScript.h"              // for JSScript
#include "vm/Realm.h"                 // for AutoRealm, Realm
#include "vm/Warnings.h"              // for WarnNumberUTF8

#include "gc/StableCellHasher-inl.h"
#include "vm/Realm-inl.h"  // for AutoRealm::AutoRealm

using namespace js;

EnterDebuggeeNoExecute::EnterDebuggeeNoExecute(
    JSContext* cx, Debugger& dbg,
    const JS::AutoDebuggerJobQueueInterruption& adjqiProof)
    : dbg_(dbg), unlocked_(nullptr), reported_(false) {
  MOZ_ASSERT(adjqiProof.initialized());
  stack_ = &cx->noExecuteDebuggerTop.ref();
  prev_ = *stack_;
  *stack_ = this;
}

#ifdef DEBUG
/* static */
bool EnterDebuggeeNoExecute::isLockedInStack(JSContext* cx, Debugger& dbg) {
  for (EnterDebuggeeNoExecute* it = cx->noExecuteDebuggerTop; it;
       it = it->prev_) {
    if (&it->debugger() == &dbg) {
      return !it->unlocked_;
    }
  }
  return false;
}
#endif

/* static */
EnterDebuggeeNoExecute* EnterDebuggeeNoExecute::findInStack(JSContext* cx) {
  Realm* debuggee = cx->realm();
  for (EnterDebuggeeNoExecute* it = cx->noExecuteDebuggerTop; it;
       it = it->prev_) {
    Debugger& dbg = it->debugger();
    if (!it->unlocked_ && dbg.observesGlobal(debuggee->maybeGlobal())) {
      return it;
    }
  }
  return nullptr;
}

/* static */
bool EnterDebuggeeNoExecute::reportIfFoundInStack(JSContext* cx,
                                                  HandleScript script) {
  if (EnterDebuggeeNoExecute* nx = findInStack(cx)) {
    bool warning = !cx->options().throwOnDebuggeeWouldRun();
    if (!warning || !nx->reported_) {
      AutoRealm ar(cx, nx->debugger().toJSObject());
      nx->reported_ = true;
      if (cx->options().dumpStackOnDebuggeeWouldRun()) {
        fprintf(stdout, "Dumping stack for DebuggeeWouldRun:\n");
        DumpBacktrace(cx);
      }
      const char* filename = script->filename() ? script->filename() : "(none)";
      char linenoStr[15];
      SprintfLiteral(linenoStr, "%u", script->lineno());
      if (warning) {
        return WarnNumberUTF8(cx, JSMSG_DEBUGGEE_WOULD_RUN, filename,
                              linenoStr);
      }

      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_DEBUGGEE_WOULD_RUN, filename, linenoStr);
      return false;
    }
  }
  return true;
}
