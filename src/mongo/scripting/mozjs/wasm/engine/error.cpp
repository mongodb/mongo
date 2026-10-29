// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "error.h"

#include "mongo/scripting/mozjs/common/exception.h"

#include <string>

#include "js/CharacterEncoding.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"

namespace mongo {
namespace mozjs {
namespace wasm {

void ExecutionCheck::capture(err_code_t fallback) {
    if (!_out)
        return;

    clear_error(_out);
    _out->code = fallback;

    if (!JS_IsExceptionPending(_cx))
        return;

    _out->code = SM_E_PENDING_EXCEPTION;

    // Before consuming the exception via ErrorReportBuilder, check whether it
    // is a MongoStatusInfo object.  statusToJSException() round-trips a
    // DBException through JS as a MongoStatusInfo, so jsExceptionToStatus()
    // can recover the original ErrorCodes::Error.  We save it in
    // mongo_error_code so the bridge can rethrow with the right code.
    {
        JS::RootedValue excn(_cx);
        if (JS_GetPendingException(_cx, &excn)) {
            Status status = jsExceptionToStatus(_cx, excn, ErrorCodes::JSInterpreterFailure, "");
            if (status.code() != ErrorCodes::JSInterpreterFailure) {
                _out->mongo_error_code = static_cast<uint32_t>(status.code());
            }
        }
    }

    JS::ExceptionStack exnStack(_cx);
    if (!JS::StealPendingExceptionStack(_cx, &exnStack)) {
        JS_ClearPendingException(_cx);
        return;
    }

    // Extract the JS stack trace from the exception object's 'stack' property.
    // This is required so the host can include the full stacktrace in error messages.
    JS::RootedValue exnVal(_cx, exnStack.exception());
    if (exnVal.isObject()) {
        JS::RootedObject exnObj(_cx, &exnVal.toObject());
        JS::RootedValue stackVal(_cx);
        if (JS_GetProperty(_cx, exnObj, "stack", &stackVal) && stackVal.isString()) {
            JS::RootedString stackStr(_cx, stackVal.toString());
            JS::UniqueChars stackChars = JS_EncodeStringToUTF8(_cx, stackStr);
            if (stackChars)
                set_string(&_out->stack, &_out->stack_len, stackChars.get());
        }
        JS_ClearPendingException(_cx);
    }

    JS::ErrorReportBuilder report(_cx);
    if (!report.init(_cx, exnStack, JS::ErrorReportBuilder::WithSideEffects)) {
        JS_ClearPendingException(_cx);
        return;
    }

    JSErrorReport* r = report.report();
    if (!r)
        return;

    if (r->message())
        set_string(&_out->msg, &_out->msg_len, r->message().c_str());
    if (r->filename)
        set_string(&_out->filename, &_out->filename_len, r->filename.c_str());
    _out->line = r->lineno;
    _out->column = r->column.oneOriginValue();
}

}  // namespace wasm

JSString* mongoErrorReportToString(JSContext* cx, JSErrorReport* reportp) {
    if (!reportp) {
        return JS_NewStringCopyZ(cx, "Unknown error");
    }

    std::string msg;
    constexpr size_t kErrorMsgReserveSize = 256;
    msg.reserve(kErrorMsgReserveSize);
    if (reportp->message()) {
        msg.append(reportp->message().c_str());
    } else {
        msg.append("JavaScript error");
    }

    if (reportp->filename) {
        msg.append(" at ");
        msg.append(reportp->filename.c_str());
        if (reportp->lineno > 0) {
            msg.push_back(':');
            msg.append(std::to_string(reportp->lineno));
        }
    }

    return JS_NewStringCopyZ(cx, msg.c_str());
}

}  // namespace mozjs
}  // namespace mongo
