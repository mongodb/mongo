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

#include "mongo/base/string_data.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_severity.h"

namespace mongo {
namespace log_backoff_detail {
void logAndBackoffImpl(size_t numAttempts);
}  // namespace log_backoff_detail

/**
 * Will log a message at 'logLevel' for the given 'logComponent' and will perform truncated
 * exponential backoff, with the backoff period based on 'numAttempts'.
 */
template <size_t N, typename... Args>
void logAndBackoff(int32_t logId,
                   logv2::LogComponent component,
                   logv2::LogSeverity severity,
                   size_t numAttempts,
                   const char (&msg)[N],
                   const fmt::internal::named_arg<Args, char>&... args) {
    logv2::detail::doLog(logId, severity, {component}, msg, args..., "attempts"_attr = numAttempts);
    log_backoff_detail::logAndBackoffImpl(numAttempts);
}

}  // namespace mongo
