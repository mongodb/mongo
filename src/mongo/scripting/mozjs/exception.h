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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>

#include <jsapi.h>

#include <js/ErrorReport.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Turns a current C++ exception into a JS exception
 */
void mongoToJSException(JSContext* cx);

/**
 * Turns a status into a js exception
 */
void statusToJSException(JSContext* cx, Status status, JS::MutableHandleValue out);

/**
 * Turns a status into a js exception
 */
Status jsExceptionToStatus(JSContext* cx,
                           JS::HandleValue excn,
                           ErrorCodes::Error altCode,
                           StringData altReason);

/**
 * Converts the current pending js expection into a status
 *
 * The altCode and altReason are used if no JS exception is pending
 */
Status currentJSExceptionToStatus(JSContext* cx, ErrorCodes::Error altCode, StringData altReason);

/**
 * Converts a JSErrorReport to status
 */
Status JSErrorReportToStatus(JSContext* cx,
                             JSErrorReport* report,
                             ErrorCodes::Error altCode,
                             StringData altReason);

/**
 * Returns the current stack as a string
 */
std::string currentJSStackToString(JSContext* cx);

/**
 * Turns the current JS exception into a C++ exception
 *
 * The altCode and altReason are used if no JS exception is pending
 */
MONGO_COMPILER_NORETURN void throwCurrentJSException(JSContext* cx,
                                                     ErrorCodes::Error altCode,
                                                     StringData altReason);

}  // namespace mozjs
}  // namespace mongo
