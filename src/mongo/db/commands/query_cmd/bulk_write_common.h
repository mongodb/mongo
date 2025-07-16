/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_parser.h"
#include "mongo/db/stats/counters.h"

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
NamespaceInfoEntry getFLENamespaceInfoEntry(const BSONObj& bulkWrite);


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
 * Helper function to build an BulkWriteUpdateOp based off the UpdateOpEntry passed in.
 */
BulkWriteUpdateOp toBulkWriteUpdate(const write_ops::UpdateOpEntry& op);

/**
 * Helper function to build an BulkWriteDeleteOp based off the DeleteOpEntry passed in.
 */
BulkWriteDeleteOp toBulkWriteDelete(const write_ops::DeleteOpEntry& op);
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
