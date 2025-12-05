/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/util/assert_util.h"

namespace mongo {
/**
 * Retries a callable up to `maxNumRetries` times if it throws ExceptionFor<E>. Returns the result
 * of the callable or propagates the exception once retries are exhausted.
 */
template <ErrorCodes::Error E, typename Fn>
auto retryOn(Fn&& fn, size_t maxNumRetries = 10) {
    for (size_t attempt = 0; attempt <= maxNumRetries; ++attempt) {
        try {
            return fn();
        } catch (const ExceptionFor<E>& ex) {
            if (attempt == maxNumRetries) {
                throw;
            }
            logv2::detail::doLog(11486800,
                                 logv2::LogSeverity::Debug(1),
                                 {logv2::LogComponent::kQuery},
                                 "Retrying after retryable error",
                                 "errorCode"_attr = E,
                                 "attempt"_attr = attempt,
                                 "maxRetries"_attr = maxNumRetries,
                                 "reason"_attr = ex.reason());
        }
    }
    MONGO_UNREACHABLE;
}
}  // namespace mongo
