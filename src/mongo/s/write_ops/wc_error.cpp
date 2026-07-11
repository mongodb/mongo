// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/write_ops/wc_error.h"

#include "mongo/s/transaction_router.h"

namespace mongo {
boost::optional<WriteConcernErrorDetail> mergeWriteConcernErrors(
    const std::vector<ShardWCError>& wcErrors) {
    if (!wcErrors.size()) {
        return boost::none;
    }

    StringBuilder msg;
    auto errCode = wcErrors.front().error.toStatus().code();
    if (wcErrors.size() != 1) {
        msg << "Multiple errors reported :: ";
    }

    for (auto it = wcErrors.begin(); it != wcErrors.end(); ++it) {
        if (it != wcErrors.begin()) {
            msg << " :: and :: ";
        }

        msg << it->error.toString();

        if (it->shardName) {
            msg << " at " << *it->shardName;
        }
    }

    return boost::make_optional<WriteConcernErrorDetail>(Status(errCode, msg.str()));
}

boost::optional<WriteConcernOptions> getWriteConcernForShardRequest(OperationContext* opCtx) {
    // Per-operation write concern is not supported in transactions.
    if (TransactionRouter::get(opCtx)) {
        return boost::none;
    }

    // Retrieve the WC specified by the remote client; in case of "fire and forget" request, the WC
    // needs to be upgraded to "w: 1" for the sharding protocol to correctly handle internal
    // writeErrors.
    auto wc = opCtx->getWriteConcern();
    if (!wc.requiresWriteAcknowledgement()) {
        wc.w = 1;
    }

    return boost::make_optional(std::move(wc));
}
}  // namespace mongo
