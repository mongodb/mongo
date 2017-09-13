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
#include "jscntxt.h"
#include "NamespaceImports.h"

namespace js {
class ErrorObject;

JSErrorReport*
CopyErrorReport(JSContext* cx, JSErrorReport* report);

JSString*
ComputeStackString(JSContext* cx);

/*
 * Given a JSErrorReport, check to see if there is an exception associated with
 * the error number.  If there is, then create an appropriate exception object,
 * set it as the pending exception, and set the JSREPORT_EXCEPTION flag on the
 * error report.  Exception-aware host error reporters should probably ignore
 * error reports so flagged.
 *
 * Return true if cx->throwing and cx->exception were set.
 *
 * This means that:
 *
 *   - If the error is successfully converted to an exception and stored in
 *     cx->exception, the return value is true. This is the "normal", happiest
 *     case for the caller.
 *
 *   - If we try to convert, but fail with OOM or some other error that ends up
 *     setting cx->throwing to true and setting cx->exception, then we also
 *     return true (because callers want to treat that case the same way).
 *     The original error described by *reportp typically won't be reported
 *     anywhere; instead OOM is reported.
 *
 *   - If *reportp is just a warning, or the error code is unrecognized, or if
 *     we decided to do nothing in order to avoid recursion, then return
 *     false. In those cases, this error is just being swept under the rug
 *     unless the caller decides to call CallErrorReporter explicitly.
 */
extern bool
ErrorToException(JSContext* cx, const char* message, JSErrorReport* reportp,
                 JSErrorCallback callback, void* userRef);

/*
 * Called if a JS API call to js_Execute or js_InternalCall fails; calls the
 * error reporter with the error report associated with any uncaught exception
 * that has been raised.  Returns true if there was an exception pending, and
 * the error reporter was actually called.
 *
 * The JSErrorReport * that the error reporter is called with is currently
 * associated with a JavaScript object, and is not guaranteed to persist after
 * the object is collected.  Any persistent uses of the JSErrorReport contents
 * should make their own copy.
 *
 * The flags field of the JSErrorReport will have the JSREPORT_EXCEPTION flag
 * set; embeddings that want to silently propagate JavaScript exceptions to
 * other contexts may want to use an error reporter that ignores errors with
 * this flag.
 */
extern bool
ReportUncaughtException(JSContext* cx);

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
              JSEXN_URIERR + 1 == JSEXN_LIMIT,
              "GetExceptionProtoKey and ExnTypeFromProtoKey require that "
              "each corresponding JSExnType and JSProtoKey value be separated "
              "by the same constant value");

static inline JSProtoKey
GetExceptionProtoKey(JSExnType exn)
{
    MOZ_ASSERT(JSEXN_ERR <= exn);
    MOZ_ASSERT(exn < JSEXN_LIMIT);
    return JSProtoKey(JSProto_Error + int(exn));
}

static inline JSExnType
ExnTypeFromProtoKey(JSProtoKey key)
{
    JSExnType type = static_cast<JSExnType>(key - JSProto_Error);
    MOZ_ASSERT(type >= JSEXN_ERR);
    MOZ_ASSERT(type < JSEXN_LIMIT);
    return type;
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

extern const char*
ValueToSourceForError(JSContext* cx, HandleValue val, JSAutoByteString& bytes);

} // namespace js

#endif /* jsexn_h */
