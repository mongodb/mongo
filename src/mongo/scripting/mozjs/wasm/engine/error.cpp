/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "error.h"

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

    JS::ExceptionStack exnStack(_cx);
    if (!JS::StealPendingExceptionStack(_cx, &exnStack)) {
        JS_ClearPendingException(_cx);
        return;
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
