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

#include "mongo/db/pipeline/mongo_process_common.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/query/cluster_aggregation_planner.h"
#include "mongo/s/query/owned_remote_cursor.h"

namespace mongo {

/**
 * Class to provide access to mongos-specific implementations of methods required by some
 * document sources.
 */
class MongoSInterface final : public MongoProcessCommon {
public:
    static BSONObj createPassthroughCommandForShard(OperationContext* opCtx,
                                                    const AggregationRequest& request,
                                                    const boost::optional<ShardId>& shardId,
                                                    Pipeline* pipeline,
                                                    BSONObj collationObj);

    /**
     * Appends information to the command sent to the shards which should be appended both if this
     * is a passthrough sent to a single shard and if this is a split pipeline.
     */
    static BSONObj genericTransformForShards(MutableDocument&& cmdForShards,
                                             OperationContext* opCtx,
                                             const boost::optional<ShardId>& shardId,
                                             const AggregationRequest& request,
                                             BSONObj collationObj);

    static BSONObj createCommandForTargetedShards(
        OperationContext* opCtx,
        const AggregationRequest& request,
        const LiteParsedPipeline& litePipe,
        const cluster_aggregation_planner::SplitPipeline& splitPipeline,
        const BSONObj collationObj,
        const boost::optional<cluster_aggregation_planner::ShardedExchangePolicy> exchangeSpec,
        bool needsMerge);

    static StatusWith<CachedCollectionRoutingInfo> getExecutionNsRoutingInfo(
        OperationContext* opCtx, const NamespaceString& execNss);

    MongoSInterface() = default;

    virtual ~MongoSInterface() = default;

    void setOperationContext(OperationContext* opCtx) final {}

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern,
        bool allowSpeculativeMajorityRead = false) final;

    std::vector<GenericCursor> getIdleCursors(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              CurrentOpUserMode userMode) const final;

    DBClientBase* directClient() final {
        MONGO_UNREACHABLE;
    }

    bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final;

    void insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                std::vector<BSONObj>&& objs,
                const WriteConcernOptions& wc,
                boost::optional<OID>) final {
        MONGO_UNREACHABLE;
    }

    void update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                std::vector<BSONObj>&& queries,
                std::vector<BSONObj>&& updates,
                const WriteConcernOptions& wc,
                bool upsert,
                bool multi,
                boost::optional<OID>) final {
        MONGO_UNREACHABLE;
    }

    CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                          const NamespaceString& ns) final {
        MONGO_UNREACHABLE;
    }

    void appendLatencyStats(OperationContext* opCtx,
                            const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const final {
        MONGO_UNREACHABLE;
    }

    Status appendStorageStats(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& param,
                              BSONObjBuilder* builder) const final {
        MONGO_UNREACHABLE;
    }

    Status appendRecordCount(OperationContext* opCtx,
                             const NamespaceString& nss,
                             BSONObjBuilder* builder) const final {
        MONGO_UNREACHABLE;
    }

    BSONObj getCollectionOptions(const NamespaceString& nss) final {
        MONGO_UNREACHABLE;
    }

    void renameIfOptionsAndIndexesHaveNotChanged(OperationContext* opCtx,
                                                 const BSONObj& renameCommandObj,
                                                 const NamespaceString& targetNs,
                                                 const BSONObj& originalCollectionOptions,
                                                 const std::list<BSONObj>& originalIndexes) final {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) final;

    std::string getShardName(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFieldsForHostedCollection(
        OperationContext* opCtx, const NamespaceString&, UUID) const final {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions pipelineOptions) final;

    /**
     * The following methods only make sense for data-bearing nodes and should never be called on
     * a mongos.
     */
    BackupCursorState openBackupCursor(OperationContext* opCtx) final {
        MONGO_UNREACHABLE;
    }

    void closeBackupCursor(OperationContext* opCtx, const UUID& backupId) final {
        MONGO_UNREACHABLE;
    }

    BackupCursorExtendState extendBackupCursor(OperationContext* opCtx,
                                               const UUID& backupId,
                                               const Timestamp& extendTo) final {
        MONGO_UNREACHABLE;
    }

    /**
     * Mongos does not have a plan cache, so this method should never be called on mongos. Upstream
     * checks are responsible for generating an error if a user attempts to introspect the plan
     * cache on mongos.
     */
    std::vector<BSONObj> getMatchingPlanCacheEntryStats(OperationContext*,
                                                        const NamespaceString&,
                                                        const MatchExpression*) const final {
        MONGO_UNREACHABLE;
    }

    bool uniqueKeyIsSupportedByIndex(const boost::intrusive_ptr<ExpressionContext>&,
                                     const NamespaceString&,
                                     const std::set<FieldPath>& uniqueKeyPaths) const final;

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>&,
                                      const NamespaceString&,
                                      ChunkVersion) const final {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<ResourceYielder> getResourceYielder() const override {
        return nullptr;
    }

protected:
    BSONObj _reportCurrentOpForClient(OperationContext* opCtx,
                                      Client* client,
                                      CurrentOpTruncateMode truncateOps) const final;

    void _reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                          CurrentOpUserMode userMode,
                                          std::vector<BSONObj>* ops) const final {
        // This implementation is a no-op, since mongoS does not maintain a SessionCatalog or
        // hold stashed locks for idle sessions.
    }
};

}  // namespace mongo
