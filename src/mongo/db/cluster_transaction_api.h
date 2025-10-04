/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/rpc/message.h"
#include "mongo/s/service_entry_point_router_role.h"
#include "mongo/util/future.h"

namespace mongo::txn_api::details {

/**
 * Behaviors for running cluster commands from a non-router process, ie mongod.
 */
class ClusterSEPTransactionClientBehaviors : public SEPTransactionClientBehaviors {
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
