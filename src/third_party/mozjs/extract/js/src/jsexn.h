/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS runtime exception classes.
 */

#ifndef jsexn_h
#define jsexn_h

#include "jsapi.h"
#include "NamespaceImports.h"

#include "vm/JSContext.h"

namespace js {
class ErrorObject;

JSErrorNotes::Note*
CopyErrorNote(JSContext* cx, JSErrorNotes::Note* note);

JSErrorReport*
CopyErrorReport(JSContext* cx, JSErrorReport* report);

JSString*
ComputeStackString(JSContext* cx);

/*
 * Given a JSErrorReport, check to see if there is an exception associated with
 * the error number.  If there is, then create an appropriate exception object,
 * set it as the pending exception, and set the JSREPORT_EXCEPTION flag on the
 * error report.
 *
 * It's possible we fail (due to OOM or some other error) and end up setting
 * cx->exception to a different exception. The original error described by
 * *reportp typically won't be reported anywhere in this case.
 *
 * If the error code is unrecognized, or if we decided to do nothing in order to
 * avoid recursion, we simply return and this error is just being swept under
 * the rug.
 */
extern void
ErrorToException(JSContext* cx, JSErrorReport* reportp,
                 JSErrorCallback callback, void* userRef);

extern JSErrorReport*
ErrorFromException(JSContext* cx, HandleObject obj);

/*
 * Make a copy of errobj parented to cx's compartment's global.
 *
 * errobj may be in a different compartment than cx, but it must be an Error
 * object (not a wrapper of one) and it must not be one of the standard error
 * prototype objects (errobj->getPrivate() must not be nullptr).
 */
extern JSObject*
CopyErrorObject(JSContext* cx, JS::Handle<ErrorObject*> errobj);

static_assert(JSEXN_ERR == 0 &&
              JSProto_Error + JSEXN_INTERNALERR == JSProto_InternalError &&
              JSProto_Error + JSEXN_EVALERR == JSProto_EvalError &&
              JSProto_Error + JSEXN_RANGEERR == JSProto_RangeError &&
              JSProto_Error + JSEXN_REFERENCEERR == JSProto_ReferenceError &&
              JSProto_Error + JSEXN_SYNTAXERR == JSProto_SyntaxError &&
              JSProto_Error + JSEXN_TYPEERR == JSProto_TypeError &&
              JSProto_Error + JSEXN_URIERR == JSProto_URIError &&
              JSProto_Error + JSEXN_DEBUGGEEWOULDRUN == JSProto_DebuggeeWouldRun &&
              JSProto_Error + JSEXN_WASMCOMPILEERROR == JSProto_CompileError &&
              JSProto_Error + JSEXN_WASMLINKERROR == JSProto_LinkError &&
              JSProto_Error + JSEXN_WASMRUNTIMEERROR == JSProto_RuntimeError &&
              JSEXN_WASMRUNTIMEERROR + 1 == JSEXN_WARN &&
              JSEXN_WARN + 1 == JSEXN_NOTE &&
              JSEXN_NOTE + 1 == JSEXN_LIMIT,
              "GetExceptionProtoKey and ExnTypeFromProtoKey require that "
              "each corresponding JSExnType and JSProtoKey value be separated "
              "by the same constant value");

static inline JSProtoKey
GetExceptionProtoKey(JSExnType exn)
{
    MOZ_ASSERT(JSEXN_ERR <= exn);
    MOZ_ASSERT(exn < JSEXN_WARN);
    return JSProtoKey(JSProto_Error + int(exn));
}

static inline JSExnType
ExnTypeFromProtoKey(JSProtoKey key)
{
    JSExnType type = static_cast<JSExnType>(key - JSProto_Error);
    MOZ_ASSERT(type >= JSEXN_ERR);
    MOZ_ASSERT(type < JSEXN_ERROR_LIMIT);
    return type;
}

static inline bool
IsErrorProtoKey(JSProtoKey key)
{
    int type = key - JSProto_Error;
    return type >= JSEXN_ERR && type < JSEXN_ERROR_LIMIT;
}

class AutoClearPendingException
{
    JSContext* cx;

  public:
    explicit AutoClearPendingException(JSContext* cxArg)
      : cx(cxArg)
    { }

    ~AutoClearPendingException() {
        JS_ClearPendingException(cx);
    }
};

class AutoAssertNoPendingException
{
    mozilla::DebugOnly<JSContext*> cx;

  public:
    explicit AutoAssertNoPendingException(JSContext* cxArg)
      : cx(cxArg)
    { }

    ~AutoAssertNoPendingException() {
        MOZ_ASSERT(!JS_IsExceptionPending(cx));
    }
};

extern const char*
ValueToSourceForError(JSContext* cx, HandleValue val, JSAutoByteString& bytes);

bool
GetInternalError(JSContext* cx, unsigned errorNumber, MutableHandleValue error);
bool
GetTypeError(JSContext* cx, unsigned errorNumber, MutableHandleValue error);

} // namespace js

#endif /* jsexn_h */
