/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/resharding/recipient_resume_document_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
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

/**
 * Calls and returns the value from the supplied lambda function.
 *
 * If a StaleConfig error is thrown during its execution, then this function will attempt to refresh
 * the collection and invoke the supplied lambda function a second time.
 *
 * TODO SERVER-77402: Replace this function with the new ShardRole retry loop utility
 */
template <typename Callable>
auto staleConfigShardLoop(OperationContext* opCtx, Callable&& callable) {
    try {
        return callable();
    } catch (const ExceptionFor<ErrorCategory::StaleShardVersionError>& ex) {
        if (auto sce = ex.extraInfo<StaleConfigInfo>()) {

            if (sce->getVersionWanted() &&
                (sce->getVersionReceived().placementVersion() <=>
                 sce->getVersionWanted()->placementVersion()) == std::partial_ordering::less) {
                // The shard is recovered and the router is staler than the shard, so we cannot
                // retry locally.
                throw;
            }

            // Recover the sharding metadata if there was no wanted version in the staleConfigInfo
            // or it was older than the received version.
            uassertStatusOK(
                FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                    opCtx, sce->getNss(), sce->getVersionReceived().placementVersion()));

            return callable();
        }
        throw;
    }
}

}  // namespace resharding::data_copy
}  // namespace mongo
