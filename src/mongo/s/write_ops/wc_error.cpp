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


#include "mongo/s/write_ops/wc_error.h"

#include "mongo/s/transaction_router.h"

namespace mongo {
boost::optional<WriteConcernErrorDetail> mergeWriteConcernErrors(
    const std::vector<ShardWCError>& wcErrors) {
    if (!wcErrors.size())
        return boost::none;

    StringBuilder msg;
    auto errCode = wcErrors.front().error.toStatus().code();
    if (wcErrors.size() != 1) {
        msg << "Multiple errors reported :: ";
    }

    for (auto it = wcErrors.begin(); it != wcErrors.end(); ++it) {
        if (it != wcErrors.begin()) {
            msg << " :: and :: ";
        }

        msg << it->error.toString() << " at " << it->shardName;
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
