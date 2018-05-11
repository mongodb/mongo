
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/exception.h"

#include <jsfriendapi.h>
#include <limits>

#include "mongo/base/static_assert.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace mozjs {

namespace {

JSErrorFormatString kErrorFormatString = {"{0}", 1, JSEXN_ERR};
const JSErrorFormatString* errorCallback(void* data, const unsigned code) {
    return &kErrorFormatString;
}

MONGO_STATIC_ASSERT_MSG(
    UINT_MAX - JSErr_Limit > ErrorCodes::MaxError,
    "Not enough space in an unsigned int for Mongo ErrorCodes and JSErrorNumbers");

}  // namespace

void mongoToJSException(JSContext* cx) {
    auto status = exceptionToStatus();

    if (status.code() != ErrorCodes::JSUncatchableError) {
        JS_ReportErrorNumber(
            cx, errorCallback, nullptr, JSErr_Limit + status.code(), status.reason().c_str());
    } else {
        // If a JSAPI callback returns false without setting a pending exception, SpiderMonkey will
        // treat it as an uncatchable error.
        auto scope = getScope(cx);
        scope->setStatus(status);
    }
}

std::string currentJSStackToString(JSContext* cx) {
    auto scope = getScope(cx);

    JS::RootedValue error(cx);
    scope->getProto<ErrorInfo>().newInstance(&error);

    return ObjectWrapper(cx, error).getString("stack");
}

Status currentJSExceptionToStatus(JSContext* cx, ErrorCodes::Error altCode, StringData altReason) {
    JS::RootedValue vp(cx);
    if (!JS_GetPendingException(cx, &vp))
        return Status(altCode, altReason.rawData());

    if (!vp.isObject()) {
        return Status(altCode, ValueWriter(cx, vp).toString());
    }

    JS::RootedObject obj(cx, vp.toObjectOrNull());
    JSErrorReport* report = JS_ErrorFromException(cx, obj);
    if (!report)
        return Status(altCode, altReason.rawData());

    return JSErrorReportToStatus(cx, report, altCode, altReason);
}

Status JSErrorReportToStatus(JSContext* cx,
                             JSErrorReport* report,
                             ErrorCodes::Error altCode,
                             StringData altReason) {
    JSStringWrapper jsstr(cx, js::ErrorReportToString(cx, report));
    if (!jsstr)
        return Status(altCode, altReason.rawData());

    ErrorCodes::Error error = altCode;

    if (report->errorNumber) {
        if (report->errorNumber < JSErr_Limit) {
            error = ErrorCodes::JSInterpreterFailure;
        } else {
            error = ErrorCodes::Error(report->errorNumber - JSErr_Limit);
        }
    }

    return Status(error, jsstr.toStringData().toString());
}

void throwCurrentJSException(JSContext* cx, ErrorCodes::Error altCode, StringData altReason) {
    auto status = currentJSExceptionToStatus(cx, altCode, altReason);
    uasserted(status.code(), status.reason());
}

}  // namespace mozjs
}  // namespace mongo
