/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/future.h"
#include "mongo/util/out_of_line_executor.h"

#include <string>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class NamespaceString;
class OperationContext;
class ShardRegistry;
template <typename T>
class StatusWith;
class TaskExecutor;

/**
 * Set of functions used to update a document's shard key.
 */
namespace documentShardKeyUpdateUtil {

static constexpr StringData kDuplicateKeyErrorContext =
    "Failed to update document's shard key "
    "field. There is either an orphan for this document or _id for this collection is not "
    "globally unique."_sd;

static constexpr StringData kNonDuplicateKeyErrorContext =
    "Update operation was converted into a "
    "distributed transaction because the document being updated would move shards and that "
    "transaction failed."_sd;

/**
 * TODO SERVER-67429 Remove this function.
 *
 * Handles performing a shard key update that changes a document's owning shard when the update is
 * being run in a transaction. This is utilized by the 'update' and 'bulkWrite' commands.
 *
 * - 'nss' specifies the namespace the update is to be performed on.
 * - 'documentKeyChangeInfo' specifies the information returned from the document's current owning
 *    shard which is used to construct the necessary commands to perform the shard key update.
 *
 * Returns a std::pair containing:
 *  - a boolean indicating whether a document shard key update was actually performed.
 *  - if an upsert was performed, the _id for the upserted document.
 */
std::pair<bool, boost::optional<BSONObj>> handleWouldChangeOwningShardErrorTransactionLegacy(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const mongo::WouldChangeOwningShardInfo& documentKeyChangeInfo);

/**
 *  A function provided by the caller of 'handleWouldChangeOwningShardErrorRetryableWriteLegacy'
 *  to rerun the original write command that generated the WCOS error - either the 'update' or
 * 'bulkWrite' command.
 */
typedef std::function<boost::optional<WouldChangeOwningShardInfo>()> RerunOriginalWriteFn;
/**
 *  A function provided by the caller of 'handleWouldChangeOwningShardErrorRetryableWriteLegacy'
 *  to do custom processing of a WriteConcernError encountered while committing the transaction,
 *  i.e. saving the error to  set on the command response. The logic for this differs between
 * 'update' and 'bulkWrite' as they have different response types.
 */
typedef std::function<void(std::unique_ptr<WriteConcernErrorDetail>)> ProcessWCErrorFn;
/**
 *  A function provided by the caller of 'handleWouldChangeOwningShardErrorRetryableWriteLegacy'
 *  to do custom processing of any exception that occurs while executing the transaction. The logic
 * for this differs between 'update' and 'bulkWrite' as they have different response types.
 */
typedef std::function<void(DBException&)> ProcessWriteErrorFn;

/**
 * TODO SERVER-67429 Remove this function.
 *
 * Handles performing a shard key update that changes a document's owning shard when the update is
 * being run in a retryable write, by starting a transaction and performing the necessary delete and
 * update within that transaction, and committing it. This is utilized by the 'update' and
 * 'bulkWrite' commands.
 *
 * - 'nss' specifies the namespace the update is to be performed on.
 * - 'rerunWriteFn', 'processWCErrorFn', and 'processWriteErrorFn' specify functions containing
 *   logic specific to the write command that is calling this function; see the types' doc comments
 *   for more information.
 *
 * Returns a std::pair containing:
 *  - a boolean indicating whether a document shard key update was actually performed.
 *  - if an upsert was performed, the _id for the upserted document.
 */
std::pair<bool, boost::optional<BSONObj>> handleWouldChangeOwningShardErrorRetryableWriteLegacy(
    OperationContext* opCtx,
    const NamespaceString& nss,
    RerunOriginalWriteFn rerunWriteFn,
    ProcessWCErrorFn processWCErrorFn,
    ProcessWriteErrorFn processWriteErrorFn);

/**
 * TODO SERVER-67429 Remove this function.
 *
 * Coordinating method and external point of entry for updating a document's shard key. This method
 * creates the necessary extra operations. It will then run each operation using the ClusterWriter.
 * If any statement throws, an exception will leave this method, and must be handled by external
 * callers.
 */
bool updateShardKeyForDocumentLegacy(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const WouldChangeOwningShardInfo& documentKeyChangeInfo,
                                     bool isTimeseriesViewRequest,
                                     bool fleCrudProcessed = false);

/**
 * Coordinating method and external point of entry for updating a document's shard key. This method
 * creates the necessary extra operations. It will then run each operation using the given
 * transaction client. If any statement throws, an exception will leave this method, and must be
 * handled by external callers.
 *
 * Returns an error on any error returned by a command. If the original update was sent with
 * {upsert: false}, returns whether or not we deleted the original doc and inserted the new one
 * sucessfully. If the original update was sent with {upsert: true}, returns whether or not we
 * inserted the new doc successfully.
 */
SemiFuture<bool> updateShardKeyForDocument(const txn_api::TransactionClient& txnClient,
                                           OperationContext* opCtx,
                                           ExecutorPtr txnExec,
                                           const NamespaceString& nss,
                                           const WouldChangeOwningShardInfo& changeInfo,
                                           bool fleCrudProcessed = false);

/**
 * Starts a transaction on this session. This method is called when WouldChangeOwningShard is thrown
 * for a write that is not in a transaction already.
 */
void startTransactionForShardKeyUpdate(OperationContext* opCtx);

/**
 * Commits the transaction on this session. This method is called to commit the transaction started
 * when WouldChangeOwningShard is thrown for a write that is not in a transaction already.
 */
BSONObj commitShardKeyUpdateTransaction(OperationContext* opCtx);

/**
 * Creates the BSONObj that will be used to delete the pre-image document. Will also attach
 * necessary generic transaction and passthrough field transaction information.
 *
 * This method should not be called outside of this class. It is only temporarily exposed for
 * intermediary test coverage.
 */
BSONObj constructShardKeyDeleteCmdObj(const NamespaceString& nss,
                                      const BSONObj& updatePreImageOrPredicate,
                                      bool shouldUpsert);

/*
 * Creates the BSONObj that will be used to insert the new document with the post-update image.
 * Will attach all necessary generic transaction and passthrough field transaction information.
 *
 * This method should not be called outside of this class. It is only temporarily exposed for
 * intermediary test coverage.
 */
BSONObj constructShardKeyInsertCmdObj(const NamespaceString& nss,
                                      const BSONObj& updatePostImage,
                                      bool fleCrudProcessed);
}  // namespace documentShardKeyUpdateUtil
}  // namespace mongo
