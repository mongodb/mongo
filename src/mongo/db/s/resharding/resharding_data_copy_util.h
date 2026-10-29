// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/resharding/recipient_resume_document_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class NamespaceString;
class OperationContext;
class Pipeline;

namespace resharding::data_copy {

/**
 * Creates the specified collection with the given options if the collection does not already exist.
 * If the collection already exists, we do not compare the options because the resharding process
 * will always use the same options for the same namespace.
 */
void ensureCollectionExists(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionOptions& options);

/**
 * Drops the specified collection or returns without error if the collection has already been
 * dropped. A particular incarnation of the collection can be dropped by specifying its UUID.
 *
 * This functions assumes the collection being dropped doesn't have any two-phase index builds
 * active on it.
 */
void ensureCollectionDropped(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& uuid = boost::none);

/**
 * Removes documents from the oplog applier progress and transaction applier progress collections
 * that are associated with an in-progress resharding operation. Also drops all oplog buffer
 * collections and conflict stash collections that are associated with the in-progress resharding
 * operation.
 */
void ensureOplogCollectionsDropped(OperationContext* opCtx,
                                   const UUID& reshardingUUID,
                                   const UUID& sourceUUID,
                                   const std::vector<DonorShardFetchTimestamp>& donorShards);

/**
 * Renames the temporary resharding collection to the source namespace string, or is a no-op if the
 * collection has already been renamed to it.
 *
 * This function throws an exception if the collection doesn't exist as the temporary resharding
 * namespace string or the source namespace string.
 */
void ensureTemporaryReshardingCollectionRenamed(OperationContext* opCtx,
                                                const CommonReshardingMetadata& metadata);

bool isCollectionCapped(OperationContext* opCtx, const NamespaceString& nss);
/**
 * Removes all entries matching the given reshardingUUID from the recipient resume data table.
 */
void deleteRecipientResumeData(OperationContext* opCtx, const UUID& reshardingUUID);

/**
 * Returns the largest _id value in the collection.
 */
Value findHighestInsertedId(OperationContext* opCtx, const CollectionAcquisition& collection);

/**
 * Returns the full document of the largest _id value in the collection.
 */
boost::optional<Document> findDocWithHighestInsertedId(OperationContext* opCtx,
                                                       const CollectionAcquisition& collection);

/**
 * Atomically inserts a batch of documents in a single multi-document transaction, and updates
 * the resume token and increments the number of documents and bytes copied (only if 'storeProgress'
 * is true) in the same transaction. Returns the number of bytes inserted.
 */
int insertBatchTransactionally(OperationContext* opCtx,
                               const NamespaceString& nss,
                               TxnNumber& txnNumber,
                               std::vector<InsertStatement>& batch,
                               const UUID& reshardingUUID,
                               const ShardId& donorShard,
                               const HostAndPort& donorHost,
                               const BSONObj& resumeToken,
                               bool storeProgress);

/**
 * Atomically inserts a batch of documents in a single storage transaction. Returns the number of
 * bytes inserted.
 *
 * Throws NamespaceNotFound if the collection doesn't already exist.
 */
int insertBatch(OperationContext* opCtx,
                const NamespaceString& nss,
                std::vector<InsertStatement>& batch);

/**
 * Checks out the logical session in the opCtx and runs the supplied lambda function in a
 * transaction, using the transaction number supplied in the opCtx.
 */
void runWithTransactionFromOpCtx(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 unique_function<void(OperationContext*)> func);

/**
 * Updates this shard's config.transactions table based on a retryable write or multi-statement
 * transaction that already executed on some donor shard.
 *
 * This function assumes it is being called while the corresponding logical session is checked out
 * by the supplied OperationContext.
 */
void updateSessionRecord(OperationContext* opCtx,
                         BSONObj o2Field,
                         std::vector<StmtId> stmtIds,
                         boost::optional<repl::OpTime> preImageOpTime,
                         boost::optional<repl::OpTime> postImageOpTime,
                         NamespaceString sourceNss);

/**
 * Retrieves the resume data natural order scans for all donor shards.
 */
std::vector<ReshardingRecipientResumeData> getRecipientResumeData(OperationContext* opCtx,
                                                                  const UUID& reshardingUUID);
}  // namespace resharding::data_copy
}  // namespace mongo
