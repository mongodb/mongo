// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/async_rpc_util.h"

#include "mongo/logv2/log.h"

// TODO(SERVER-98556): kTest debug statements for the purpose of helping with diagnosing BFs.
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::async_rpc {

void logErrorDetails(int responsesLeft, Status errorStatus) {
    LOGV2_DEBUG(9771001,
                1,
                "Error response received from shard",
                "responsesLeft"_attr = responsesLeft,
                "status"_attr = errorStatus.toString());
}

}  // namespace mongo::async_rpc
