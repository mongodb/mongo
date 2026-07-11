// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/rpc/message.h"
#include "mongo/s/service_entry_point_router_role.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo::txn_api::details {

/**
 * Behaviors for running cluster commands from a non-router process, ie mongod.
 */
class [[MONGO_MOD_PUBLIC]] ClusterSEPTransactionClientBehaviors
    : public SEPTransactionClientBehaviors {
public:
    ClusterSEPTransactionClientBehaviors(ServiceContext* service) {}

    BSONObj maybeModifyCommand(BSONObj cmdObj) const override;

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request,
                                     Date_t started) const override;

    bool runsClusterOperations() const override {
        // Cluster commands will attach appropriate shard versions for any targeted namespaces, so
        // it is safe to use this client within a caller's operation with shard versions.
        return true;
    }
};

}  // namespace mongo::txn_api::details
