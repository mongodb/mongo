/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec_util.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {

class CanonicalQuery;
class CurOp;
class DeleteRequest;
class OpDebug;
class PlanExecutor;
class UpdateRequest;

namespace write_ops_exec {

/**
 * The result of performing a single write, possibly within a batch.
 */
struct WriteResult {
    /**
     * Maps 1-to-1 to single ops in request. May be shorter than input if there are errors.
     */
    std::vector<StatusWith<SingleWriteResult>> results;

    // Stores the statement ids for the ops that had already been executed, thus were not executed
    // by this write.
    std::vector<StmtId> retriedStmtIds;

    // In case of an error, whether the operation can continue.
    bool canContinue = true;
};

/**
 * Returns true if the batch can continue, false to stop the batch, or throws to fail the command.
 */
bool handleError(OperationContext* opCtx,
                 const DBException& ex,
                 const NamespaceString& nss,
                 bool ordered,
                 bool isMultiUpdate,
                 boost::optional<UUID> sampleId,
                 WriteResult* out);

bool getFleCrudProcessed(OperationContext* opCtx,
                         const boost::optional<EncryptionInformation>& encryptionInfo,
                         const boost::optional<TenantId>& tenantId);

/**
 * Returns true if caller should try to insert more documents. Does nothing else if batch is empty.
 */
bool insertBatchAndHandleErrors(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const boost::optional<mongo::UUID>& collectionUUID,
                                bool ordered,
                                std::vector<InsertStatement>& batch,
                                OperationSource source,
                                LastOpFixer* lastOpFixer,
                                WriteResult* out);

/**
 * If the operation succeeded, then returns either a document to return to the client, or
 * boost::none if no matching document to update/remove was found. If the operation failed, throws.
 * Accepts the name of the operation (e.g. "delete", "update", "findAndModify") for use in the
 * exception and log messages.
 */
boost::optional<BSONObj> advanceExecutor(OperationContext* opCtx,
                                         PlanExecutor* exec,
                                         bool isRemove,
                                         StringData operationName);

/**
 * Executes an update, supports returning a pre/post image. The returned document is placed into
 * docFound (if applicable). Should be called in a writeConflictRetry loop.
 */
UpdateResult performUpdate(OperationContext* opCtx,
                           const NamespaceString& nss,
                           CurOp* curOp,
                           bool inTransaction,
                           bool remove,
                           bool upsert,
                           const boost::optional<mongo::UUID>& collectionUUID,
                           boost::optional<BSONObj>& docFound,
                           UpdateRequest* updateRequest);

/**
 * Executes a delete, supports returning the deleted document. the returned document is placed into
 * docFound (if applicable). Should be called in a writeConflictRetry loop.
 */
long long performDelete(OperationContext* opCtx,
                        const NamespaceString& nss,
                        DeleteRequest* deleteRequest,
                        CurOp* curOp,
                        bool inTransaction,
                        const boost::optional<mongo::UUID>& collectionUUID,
                        boost::optional<BSONObj>& docFound);

/**
 * Generates a WriteError for a given Status.
 *
 * This function may throw.
 */
boost::optional<write_ops::WriteError> generateError(OperationContext* opCtx,
                                                     const Status& status,
                                                     int index,
                                                     size_t numErrors);

/**
 * Generates a WriteError for a given Status. Does not handle tenant migration errors.
 *
 * Marked as 'noexcept' as we need to safely be able to call this function during exception
 * handling.
 */
boost::optional<write_ops::WriteError> generateErrorNoTenantMigration(OperationContext* opCtx,
                                                                      const Status& status,
                                                                      int index,
                                                                      size_t numErrors) noexcept;

/**
 * Updates the retryable write stats if the write op contains retry.
 */
void updateRetryStats(OperationContext* opCtx, bool containsRetry);

/**
 * Marks the op as complete, log it and profile if appropriate.
 */
void logOperationAndProfileIfNeeded(OperationContext* opCtx, CurOp* curOp);

/**
 * Performs a batch of inserts, updates, or deletes.
 *
 * These functions handle all of the work of doing the writes, including locking, incrementing
 * counters, managing CurOp, and of course actually doing the write. Waiting for the writeConcern is
 * *not* handled by these functions and is expected to be done by the caller if needed.
 *
 * NotPrimaryErrorTracker is updated for failures of individual writes, but not for batch errors
 * reported by an exception being thrown from these functions. Callers are responsible for managing
 * NotPrimaryErrorTracker in that case. This should generally be combined with
 * NotPrimaryErrorTracker handling from parse failures.
 *
 * 'type' indicates whether the operation was induced by a standard write, a chunk migration, or a
 * time-series insert.
 *
 * Note: performInserts() gets called for both user and internal (like tenant collection cloner,
 * and initial sync/tenant migration oplog buffer) inserts.
 */
WriteResult performInserts(OperationContext* opCtx,
                           const write_ops::InsertCommandRequest& op,
                           OperationSource source = OperationSource::kStandard);
WriteResult performUpdates(OperationContext* opCtx,
                           const write_ops::UpdateCommandRequest& op,
                           OperationSource source = OperationSource::kStandard);
WriteResult performDeletes(OperationContext* opCtx,
                           const write_ops::DeleteCommandRequest& op,
                           OperationSource source = OperationSource::kStandard);

Status performAtomicTimeseriesWrites(OperationContext* opCtx,
                                     const std::vector<write_ops::InsertCommandRequest>& insertOps,
                                     const std::vector<write_ops::UpdateCommandRequest>& updateOps);

/**
 * Runs a time-series update command in a transaction and collects the write result from each
 * statement.
 *
 * Assumes the update command is a retryable write and targeted on the time-series view namespace.
 */
void runTimeseriesRetryableUpdates(OperationContext* opCtx,
                                   const NamespaceString& bucketNs,
                                   const write_ops::UpdateCommandRequest& wholeOp,
                                   std::shared_ptr<executor::TaskExecutor> executor,
                                   write_ops_exec::WriteResult* reply);

/**
 * Populate 'opDebug' with stats describing the execution of an update operation. Illegal to call
 * with a null OpDebug pointer.
 */
void recordUpdateResultInOpDebug(const UpdateResult& updateResult, OpDebug* opDebug);

/**
 * Returns true if an update failure due to a given DuplicateKey error is eligible for retry.
 */
bool shouldRetryDuplicateKeyException(const UpdateRequest& updateRequest,
                                      const CanonicalQuery& cq,
                                      const DuplicateKeyErrorInfo& errorInfo);

/**
 * Returns an InsertCommandReply if the timeseries writes succeeded.
 */
write_ops::InsertCommandReply performTimeseriesWrites(
    OperationContext* opCtx, const write_ops::InsertCommandRequest& request);

write_ops::InsertCommandReply performTimeseriesWrites(
    OperationContext* opCtx, const write_ops::InsertCommandRequest& request, CurOp* curOp);

/*
 * Populates 'result' with the explain information for the write requests.
 */
void explainUpdate(OperationContext* opCtx,
                   UpdateRequest& updateRequest,
                   bool isTimeseriesViewRequest,
                   const SerializationContext& serializationContext,
                   const BSONObj& command,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result);

void explainDelete(OperationContext* opCtx,
                   DeleteRequest& deleteRequest,
                   bool isTimeseriesViewRequest,
                   const SerializationContext& serializationContext,
                   const BSONObj& command,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result);

}  // namespace write_ops_exec
}  // namespace mongo
