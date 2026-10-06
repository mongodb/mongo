// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/server_initiated_transaction.h"

namespace mongo {
namespace {
const auto serverInitiatedTransaction =
    OperationContext::declareDecoration<boost::optional<bool>>();
}

boost::optional<bool>& participantIsInServerInitiatedTransaction(OperationContext* opCtx) {
    return serverInitiatedTransaction(opCtx);
}

}  // namespace mongo
