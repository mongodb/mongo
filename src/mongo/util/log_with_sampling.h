// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/random.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <utility>

[[MONGO_MOD_PUBLIC]];

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
        client->getPrng().trueWithProbability(serverGlobalParams.sampleRate.load());

    // Log the transaction if we should sample and its duration is greater than or equal to the
    // slowMS command threshold.
    const bool shouldLogSlowOp = shouldSample && opDuration >= slowMS;

    return std::pair<bool, bool>(componentHasTargetLogVerbosity || shouldLogSlowOp, shouldSample);
}

}  // namespace mongo
