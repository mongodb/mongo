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

#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace mozjs {

namespace {

JSErrorFormatString kFormatString = {"{0}", 1, JSEXN_ERR};
const JSErrorFormatString* errorCallback(void* data, const unsigned code) {
    return &kFormatString;
}

}  // namespace

void mongoToJSException(JSContext* cx) {
    auto status = exceptionToStatus();

    JS_ReportErrorNumber(cx, errorCallback, nullptr, status.code(), status.reason().c_str());
}

void setJSException(JSContext* cx, ErrorCodes::Error code, StringData sd) {
    JS_ReportErrorNumber(cx, errorCallback, nullptr, code, sd.rawData());
}

Status currentJSExceptionToStatus(JSContext* cx, ErrorCodes::Error altCode, StringData altReason) {
    JS::RootedValue vp(cx);
    if (!JS_GetPendingException(cx, &vp))
        return Status(altCode, altReason.rawData());

    JS::RootedObject obj(cx, vp.toObjectOrNull());
    JSErrorReport* report = JS_ErrorFromException(cx, obj);
    if (!report)
        return Status(altCode, altReason.rawData());

    JSStringWrapper jsstr(cx, js::ErrorReportToString(cx, report));
    if (!jsstr)
        return Status(altCode, altReason.rawData());

    /**
     * errorNumber is only set by library consumers of MozJS, and then only via
     * JS_ReportErrorNumber, so all of the codes we see here are ours.
     */
    return Status(report->errorNumber ? static_cast<ErrorCodes::Error>(report->errorNumber)
                                      : altCode,
                  jsstr.toStringData().toString());
}

void throwCurrentJSException(JSContext* cx, ErrorCodes::Error altCode, StringData altReason) {
    auto status = currentJSExceptionToStatus(cx, altCode, altReason);
    uasserted(status.code(), status.reason());
}

}  // namespace mozjs
}  // namespace mongo
