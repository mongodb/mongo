/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <utility>

#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"

namespace mongo {

/*
 * Return a pair of booleans. The first value is if we should log the operation and the second value
 * is if we should sample this operation for profiling.
 */
inline std::pair<bool, bool> shouldLogSlowOpWithSampling(OperationContext* opCtx,
                                                         logv2::LogComponent logComponent,
                                                         Milliseconds opDuration,
                                                         Milliseconds slowMS) {
    // Log the operation if log message verbosity for operation component is >= 1.
    const bool componentHasTargetLogVerbosity =
        shouldLog(logComponent, logv2::LogSeverity::Debug(1));

    const auto client = opCtx->getClient();
    const bool shouldSample =
        client->getPrng().nextCanonicalDouble() < serverGlobalParams.sampleRate;

    // Log the transaction if we should sample and its duration is greater than or equal to the
    // slowMS command threshold.
    const bool shouldLogSlowOp = shouldSample && opDuration >= slowMS;

    return std::pair<bool, bool>(componentHasTargetLogVerbosity || shouldLogSlowOp, shouldSample);
}

}  // namespace mongo
