// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/oid.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/bulk_write_exec.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace cluster {

/**
 * If 'targetEpoch' is set, throws a 'StaleEpoch' error if the targeted namespace is found to no
 * longer have the epoch given by 'targetEpoch'.
 */
void write(OperationContext* opCtx,
           const BatchedCommandRequest& request,
           NamespaceString* nss,
           BatchWriteExecStats* stats,
           BatchedCommandResponse* response,
           boost::optional<OID> targetEpoch = boost::none);

/**
 * Execute a bulkWrite request as a router.
 *
 * Note: Caller is responsible for passing in a valid BulkWriteCommandRequest.
 */
bulk_write_exec::BulkWriteReplyInfo bulkWrite(
    OperationContext* opCtx,
    const BulkWriteCommandRequest& request,
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    bulk_write_exec::BulkWriteExecStats& execStats);

}  // namespace cluster
}  // namespace mongo
