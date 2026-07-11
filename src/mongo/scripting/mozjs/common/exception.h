// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

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
                           std::string_view altReason);

/**
 * Converts the current pending js expection into a status
 *
 * The altCode and altReason are used if no JS exception is pending
 */
Status currentJSExceptionToStatus(JSContext* cx,
                                  ErrorCodes::Error altCode,
                                  std::string_view altReason);

/**
 * Converts a JSErrorReport to status
 */
Status JSErrorReportToStatus(JSContext* cx,
                             JSErrorReport* report,
                             ErrorCodes::Error altCode,
                             std::string_view altReason);

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
                                                     std::string_view altReason);

}  // namespace mozjs
}  // namespace mongo
