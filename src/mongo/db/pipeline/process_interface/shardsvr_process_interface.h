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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/common_mongod_process_interface.h"
#include "mongo/s/catalog_cache.h"

namespace mongo {

/**
 * Specialized version of the MongoDInterface when this node is a shard server.
 */
class ShardServerProcessInterface final : public CommonMongodProcessInterface {
public:
    using CommonMongodProcessInterface::CommonMongodProcessInterface;

    ShardServerProcessInterface(OperationContext* opCtx,
                                std::shared_ptr<executor::TaskExecutor> executor);

    /**
     * Note: Cannot be called while holding a lock. Refreshes from the config servers if the
     * metadata for the given namespace does not exist. Otherwise, will not automatically refresh,
     * so the answer may be stale or become stale after calling. Caller should always attach
     * shardVersion when sending request against nss based on this information.
     */
    bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final;

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString& nss,
                                      ChunkVersion targetCollectionVersion) const final;

    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFieldsForHostedCollection(
        OperationContext* opCtx, const NamespaceString&, UUID) const final;

    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext*, const NamespaceString&) const final {
        // We don't expect anyone to use this method on the shard itself (yet). This is currently
        // only used for $merge. For $out in a sharded cluster, the mongos is responsible for
        // collecting the document key fields before serializing them and sending them to the
        // shards. This is logically a MONGO_UNREACHABLE, but a malicious user could construct a
        // request to send directly to the shards which does not include the uniqueKey, so we must
        // be prepared to gracefully error.
        uasserted(50997, "Unexpected attempt to consult catalog cache on a shard server");
    }

    /**
     * Inserts the documents 'objs' into the namespace 'ns' using the ClusterWriter for locking,
     * routing, stale config handling, etc.
     */
    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::vector<BSONObj>&& objs,
                  const WriteConcernOptions& wc,
                  boost::optional<OID> targetEpoch) final;

    /**
     * Replaces the documents matching 'queries' with 'updates' using the ClusterWriter for locking,
     * routing, stale config handling, etc.
     */
    StatusWith<UpdateResult> update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    BatchedObjects&& batch,
                                    const WriteConcernOptions& wc,
                                    UpsertType upsert,
                                    bool multi,
                                    boost::optional<OID> targetEpoch) final;

    BSONObj attachCursorSourceAndExplain(Pipeline* ownedPipeline,
                                         ExplainOptions::Verbosity verbosity) final;

    std::unique_ptr<ShardFilterer> getShardFilterer(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const override final;

    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) final;

    std::list<BSONObj> getIndexSpecs(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     bool includeBuildUUIDs) final;
    void renameIfOptionsAndIndexesHaveNotChanged(OperationContext* opCtx,
                                                 const BSONObj& renameCommandObj,
                                                 const NamespaceString& targetNs,
                                                 const BSONObj& originalCollectionOptions,
                                                 const std::list<BSONObj>& originalIndexes) final;
    void createCollection(OperationContext* opCtx,
                          const std::string& dbName,
                          const BSONObj& cmdObj) final;
    void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const std::vector<BSONObj>& indexSpecs) final;
    void dropCollection(OperationContext* opCtx, const NamespaceString& collection) final;

    /**
     * If 'allowTargetingShards' is true, splits the pipeline and dispatch half to the shards,
     * leaving the merging half executing in this process after attaching a $mergeCursors. Will
     * retry on network errors and also on StaleConfig errors to avoid restarting the entire
     * operation.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        Pipeline* pipeline, bool allowTargetingShards) final;

    void setExpectedShardVersion(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 boost::optional<ChunkVersion> chunkVersion) final;

private:
    // If the current operation is versioned, then we attach the DB version to the command object;
    // otherwise, it is returned unmodified. Used when running internal commands, as the parent
    // operation may be unversioned if run by a client connecting directly to the shard. If a shard
    // version is supplied it will be set on the command, otherwise no shard version is attached.
    // This allows sub-operations to set a version where necessary (e.g. to enforce that only
    // unsharded collections can be renamed) while omitting it in cases where it is not relevant
    // (e.g. obtaining a list of indexes for a collection that may be sharded or unsharded).
    BSONObj _versionCommandIfAppropriate(BSONObj cmdObj,
                                         const CachedDatabaseInfo& cachedDbInfo,
                                         boost::optional<ChunkVersion> shardVersion = boost::none);

    // Records whether the initial operation which creates this MongoProcessInterface is versioned.
    // We want to avoid applying versions to sub-operations in cases where the client has connected
    // directly to a shard. Since getMores are never versioned, we must retain the versioning state
    // of the original operation so that we can decide whether we should version sub-operations
    // across the entire lifetime of the pipeline that owns this MongoProcessInterface.
    bool _opIsVersioned = false;
};

}  // namespace mongo
