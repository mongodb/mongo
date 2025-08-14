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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/local_catalog/collection_operation_source.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/write_ops/single_write_result_gen.h"
#include "mongo/db/query/write_ops/update_result.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_exec_util.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CanonicalQuery;
class CurOp;
class DeleteRequest;
class OpDebug;
class PlanExecutor;
class UpdateRequest;

// TODO SERVER-107768: Remove the forward declaration here.
namespace timeseries {
class CollectionPreConditions;
}  // namespace timeseries

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
                                const timeseries::CollectionPreConditions& preConditions,
                                bool ordered,
                                std::vector<InsertStatement>& batch,
                                OperationSource source,
                                LastOpFixer* lastOpFixer,
                                WriteResult* out);

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
                           UpdateRequest* updateRequest,
                           const timeseries::CollectionPreConditions& preConditions,
                           bool isTimeseriesLogicalRequest);

/**
 * Executes a delete, supports returning the deleted document. the returned document is placed into
 * docFound (if applicable). Should be called in a writeConflictRetry loop. Returns the number of
 * documents deleted as a long long to conform to OpDebug interface.
 */
long long performDelete(OperationContext* opCtx,
                        const NamespaceString& nss,
                        DeleteRequest* deleteRequest,
                        CurOp* curOp,
                        bool inTransaction,
                        const boost::optional<mongo::UUID>& collectionUUID,
                        boost::optional<BSONObj>& docFound,
                        const timeseries::CollectionPreConditions& preConditions,
                        bool isTimeseriesLogicalRequest = false);

/**
 * Generates a WriteError for a given Status.
 */
boost::optional<write_ops::WriteError> generateError(OperationContext* opCtx,
                                                     const Status& status,
                                                     int index,
                                                     size_t numErrors);

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
 * Note: performInserts() gets called for both user and internal (like initial sync oplog buffer)
 * inserts.
 *
 * Note: these functions do not by themselves handle any logic dealing with recognizing whether an
 * operation is a logical time-series operation or not. This has to be handled at the layer above
 * it, and passed in through the CollectionPreConditions parameter. If the CollectionPreCondition
 * object is not passed in, a CollectionPreCondition object will still be constructed, but it will
 * be assumed that we are not performing a logical time-series operation.
 */
WriteResult performInserts(
    OperationContext* opCtx,
    const write_ops::InsertCommandRequest& op,
    boost::optional<const timeseries::CollectionPreConditions&> preConditions = boost::none,
    OperationSource source = OperationSource::kStandard);
WriteResult performUpdates(
    OperationContext* opCtx,
    const write_ops::UpdateCommandRequest& op,
    boost::optional<const timeseries::CollectionPreConditions&> preConditions = boost::none,
    OperationSource source = OperationSource::kStandard);
WriteResult performDeletes(
    OperationContext* opCtx,
    const write_ops::DeleteCommandRequest& op,
    boost::optional<const timeseries::CollectionPreConditions&> preConditions = boost::none,
    OperationSource source = OperationSource::kStandard);

void runTimeseriesRetryableUpdates(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const write_ops::UpdateCommandRequest& wholeOp,
                                   const timeseries::CollectionPreConditions& preConditions,
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
bool shouldRetryDuplicateKeyException(OperationContext* opCtx,
                                      const UpdateRequest& updateRequest,
                                      const CanonicalQuery& cq,
                                      const DuplicateKeyErrorInfo& errorInfo,
                                      int retryAttempts);

/*
 * Populates 'result' with the explain information for the write requests.
 */
void explainUpdate(OperationContext* opCtx,
                   UpdateRequest& updateRequest,
                   bool isTimeseriesViewRequest,
                   const SerializationContext& serializationContext,
                   const BSONObj& command,
                   const timeseries::CollectionPreConditions& preConditions,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result);

void explainDelete(OperationContext* opCtx,
                   DeleteRequest& deleteRequest,
                   bool isTimeseriesViewRequest,
                   const SerializationContext& serializationContext,
                   const BSONObj& command,
                   const timeseries::CollectionPreConditions& preConditions,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result);

}  // namespace write_ops_exec
}  // namespace mongo
