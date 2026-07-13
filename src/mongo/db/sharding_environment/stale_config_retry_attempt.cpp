// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/stale_config_retry_attempt.h"

namespace mongo {
namespace {
const auto staleConfigRetryAttemptDecoration =
    OperationContext::declareDecoration<boost::optional<int>>();
}

boost::optional<int>& staleConfigRetryAttempt(OperationContext* opCtx) {
    return staleConfigRetryAttemptDecoration(opCtx);
}

}  // namespace mongo
