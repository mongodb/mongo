// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_parser.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Contains common functionality shared between the bulkWrite command in mongos and mongod.
 */

namespace mongo {

class DeleteRequest;
class UpdateRequest;

namespace bulk_write_common {

/**
 * Validates the given bulkWrite command request and throws if the request is malformed.
 */
void validateRequest(const BulkWriteCommandRequest& req, bool isRouter);

/**
 * Get the privileges needed to perform the given bulkWrite command.
 */
std::vector<Privilege> getPrivileges(const BulkWriteCommandRequest& req);

/**
 * Get the statement ID for an operation within a bulkWrite command, taking into consideration
 * whether the stmtId / stmtIds fields are present on the request.
 */
int32_t getStatementId(const BulkWriteCommandRequest& req, size_t currentOpIdx);

/**
 * From a serialized BulkWriteCommandRequest containing a single NamespaceInfoEntry,
 * extract that NamespaceInfoEntry. For bulkWrite with queryable encryption.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] NamespaceInfoEntry getFLENamespaceInfoEntry(
    const BSONObj& bulkWrite);


/**
 * Return true when the operation uses unacknowledged writeConcern, i.e. {w: 0, j: false}.
 */
bool isUnacknowledgedBulkWrite(OperationContext* opCtx);

/**
 * Helper for FLE support. Build a InsertCommandRequest from a BulkWriteCommandRequest.
 */
write_ops::InsertCommandRequest makeInsertCommandRequestForFLE(
    const std::vector<mongo::BSONObj>& documents,
    const BulkWriteCommandRequest& req,
    const mongo::NamespaceInfoEntry& nsInfoEntry);

/**
 * Helper function to build an UpdateOpEntry based off the BulkWriteUpdateOp passed in.
 */
write_ops::UpdateOpEntry makeUpdateOpEntryFromUpdateOp(const BulkWriteUpdateOp* op);

/**
 * Helper function to build an UpdateRequest based off the BulkWriteUpdateOp passed in and its
 * namespace and top-level 'let' parameter.
 */
UpdateRequest makeUpdateRequestFromUpdateOp(OperationContext* opCtx,
                                            const NamespaceInfoEntry& nsEntry,
                                            const BulkWriteUpdateOp* op,
                                            const StmtId& stmtId,
                                            const boost::optional<BSONObj>& letParameters,
                                            const mongo::OptionalBool& bypassEmptyTsReplacement);

/**
 * Helper function to build a DeleteRequest based off the BulkWriteDeleteOp passed in and its
 * namespace and top-level 'let' parameter.
 */
DeleteRequest makeDeleteRequestFromDeleteOp(OperationContext* opCtx,
                                            const NamespaceInfoEntry& nsEntry,
                                            const BulkWriteDeleteOp* op,
                                            const StmtId& stmtId,
                                            const boost::optional<BSONObj>& letParameters);

/**
 * Helper function to build an UpdateCommandRequest based off the update operation in the bulkWrite
 * request at index currentOpIdx.
 */
write_ops::UpdateCommandRequest makeUpdateCommandRequestFromUpdateOp(
    OperationContext* opCtx,
    const BulkWriteUpdateOp* op,
    const BulkWriteCommandRequest& req,
    size_t currentOpIdx);

/**
 * Helper for FLE support. Build a DeleteCommandRequest from a BulkWriteDeleteOp.
 */
write_ops::DeleteCommandRequest makeDeleteCommandRequestForFLE(
    OperationContext* opCtx,
    const BulkWriteDeleteOp* op,
    const BulkWriteCommandRequest& req,
    const mongo::NamespaceInfoEntry& nsEntry);

BulkWriteCommandRequest makeSingleOpBulkWriteCommandRequest(
    const BulkWriteCommandRequest& bulkWriteReq, size_t opIdx);

/**
 * Helper for bulkWrite use of incrementUpdateMetrics.
 */
void incrementBulkWriteUpdateMetrics(
    QueryCounters& queryCounters,
    ClusterRole role,
    const write_ops::UpdateModification& updateMod,
    const mongo::NamespaceString& ns,
    const boost::optional<std::vector<mongo::BSONObj>>& arrayFilters,
    bool isMulti);
}  // namespace bulk_write_common
}  // namespace mongo
