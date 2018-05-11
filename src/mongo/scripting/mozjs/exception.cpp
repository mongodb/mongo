/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
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

void mongoToJSException(JSContext* cx) {
    auto status = exceptionToStatus();

    if (status.code() != ErrorCodes::JSUncatchableError) {
        JS::RootedValue val(cx);
        statusToJSException(cx, status, &val);

        JS_SetPendingException(cx, val);
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

    return jsExceptionToStatus(cx, vp, altCode, altReason);
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
            invariant(!ErrorCodes::shouldHaveExtraInfo(error));
        }
    }

    return Status(error, jsstr.toStringData().toString());
}

void throwCurrentJSException(JSContext* cx, ErrorCodes::Error altCode, StringData altReason) {
    uassertStatusOK(currentJSExceptionToStatus(cx, altCode, altReason));
    MONGO_UNREACHABLE;
}

/**
 * Turns a status into a js exception
 */
void statusToJSException(JSContext* cx, Status status, JS::MutableHandleValue out) {
    MongoStatusInfo::fromStatus(cx, std::move(status), out);
}

/**
 * Turns a js exception into a status
 */
Status jsExceptionToStatus(JSContext* cx,
                           JS::HandleValue excn,
                           ErrorCodes::Error altCode,
                           StringData altReason) {
    auto scope = getScope(cx);

    if (!excn.isObject()) {
        return Status(altCode, ValueWriter(cx, excn).toString());
    }

    if (scope->getProto<MongoStatusInfo>().instanceOf(excn)) {
        return MongoStatusInfo::toStatus(cx, excn);
    }

    JS::RootedObject obj(cx, excn.toObjectOrNull());

    JSErrorReport* report = JS_ErrorFromException(cx, obj);
    if (!report)
        return Status(altCode, altReason.rawData());

    return JSErrorReportToStatus(cx, report, altCode, altReason);
}

}  // namespace mozjs
}  // namespace mongo
