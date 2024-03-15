/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS runtime exception classes.
 */

#ifndef jsexn_h
#define jsexn_h

#include "mozilla/Assertions.h"

#include "jspubtd.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/friend/ErrorMessages.h"  // JSErr_Limit
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"

extern const JSErrorFormatString js_ErrorFormatString[JSErr_Limit];

namespace js {

class ErrorObject;

UniquePtr<JSErrorNotes::Note> CopyErrorNote(JSContext* cx,
                                            JSErrorNotes::Note* note);

UniquePtr<JSErrorReport> CopyErrorReport(JSContext* cx, JSErrorReport* report);

bool CaptureStack(JSContext* cx, MutableHandleObject stack);

JSString* ComputeStackString(JSContext* cx);

/*
 * Given a JSErrorReport, check to see if there is an exception associated with
 * the error number.  If there is, then create an appropriate Error object,
 * set it as the pending exception.
 *
 * It's possible we fail (due to OOM or some other error) and end up setting
 * JSContext::unwrappedException to a different exception.
 * The original error described by reportp typically won't be reported anywhere
 * in this case.
 *
 * Returns true if the error was converted to an exception. If the error code
 * is unrecognized, we fail due to OOM, or if we decided to do nothing in order
 * to avoid recursion, we return false and this error is just being swept under
 * the rug.
 */
extern bool ErrorToException(JSContext* cx, JSErrorReport* reportp,
                             JSErrorCallback callback, void* userRef);

extern JSErrorReport* ErrorFromException(JSContext* cx, HandleObject obj);

/*
 * Make a copy of errobj parented to cx's compartment's global.
 *
 * errobj may be in a different compartment than cx, but it must be an Error
 * object (not a wrapper of one) and it must not be one of the standard error
 * prototype objects (errobj->getPrivate() must not be nullptr).
 */
extern JSObject* CopyErrorObject(JSContext* cx,
                                 JS::Handle<ErrorObject*> errobj);

static_assert(
    JSEXN_ERR == 0 &&
        JSProto_Error + int(JSEXN_INTERNALERR) == JSProto_InternalError &&
        JSProto_Error + int(JSEXN_AGGREGATEERR) == JSProto_AggregateError &&
        JSProto_Error + int(JSEXN_EVALERR) == JSProto_EvalError &&
        JSProto_Error + int(JSEXN_RANGEERR) == JSProto_RangeError &&
        JSProto_Error + int(JSEXN_REFERENCEERR) == JSProto_ReferenceError &&
        JSProto_Error + int(JSEXN_SYNTAXERR) == JSProto_SyntaxError &&
        JSProto_Error + int(JSEXN_TYPEERR) == JSProto_TypeError &&
        JSProto_Error + int(JSEXN_URIERR) == JSProto_URIError &&
        JSProto_Error + int(JSEXN_DEBUGGEEWOULDRUN) ==
            JSProto_DebuggeeWouldRun &&
        JSProto_Error + int(JSEXN_WASMCOMPILEERROR) == JSProto_CompileError &&
        JSProto_Error + int(JSEXN_WASMLINKERROR) == JSProto_LinkError &&
        JSProto_Error + int(JSEXN_WASMRUNTIMEERROR) == JSProto_RuntimeError &&
        JSEXN_WASMRUNTIMEERROR + 1 == JSEXN_WARN &&
        JSEXN_WARN + 1 == JSEXN_NOTE && JSEXN_NOTE + 1 == JSEXN_LIMIT,
    "GetExceptionProtoKey and ExnTypeFromProtoKey require that "
    "each corresponding JSExnType and JSProtoKey value be separated "
    "by the same constant value");

static inline constexpr JSProtoKey GetExceptionProtoKey(JSExnType exn) {
  MOZ_ASSERT(JSEXN_ERR <= exn);
  MOZ_ASSERT(exn < JSEXN_WARN);
  return JSProtoKey(JSProto_Error + int(exn));
}

static inline JSExnType ExnTypeFromProtoKey(JSProtoKey key) {
  JSExnType type = static_cast<JSExnType>(key - JSProto_Error);
  MOZ_ASSERT(type >= JSEXN_ERR);
  MOZ_ASSERT(type < JSEXN_ERROR_LIMIT);
  return type;
}

static inline bool IsErrorProtoKey(JSProtoKey key) {
  int type = key - JSProto_Error;
  return type >= JSEXN_ERR && type < JSEXN_ERROR_LIMIT;
}

class AutoClearPendingException {
  JSContext* cx;

 public:
  explicit AutoClearPendingException(JSContext* cxArg) : cx(cxArg) {}

  ~AutoClearPendingException() { JS_ClearPendingException(cx); }
};

extern const char* ValueToSourceForError(JSContext* cx, HandleValue val,
                                         JS::UniqueChars& bytes);

bool GetInternalError(JSContext* cx, unsigned errorNumber,
                      MutableHandleValue error);
bool GetTypeError(JSContext* cx, unsigned errorNumber,
                  MutableHandleValue error);
bool GetAggregateError(JSContext* cx, unsigned errorNumber,
                       MutableHandleValue error);

}  // namespace js

#endif /* jsexn_h */
