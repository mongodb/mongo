/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/db/pipeline/merizo_process_common.h"
#include "merizo/db/pipeline/pipeline.h"
#include "merizo/s/async_requests_sender.h"
#include "merizo/s/catalog_cache.h"
#include "merizo/s/query/cluster_aggregation_planner.h"
#include "merizo/s/query/owned_remote_cursor.h"

namespace merizo {

/**
 * Class to provide access to merizos-specific implementations of methods required by some
 * document sources.
 */
class MerizoSInterface final : public MerizoProcessCommon {
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

    MerizoSInterface() = default;

    virtual ~MerizoSInterface() = default;

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

    repl::OplogEntry lookUpOplogEntryByOpTime(OperationContext* opCtx,
                                              repl::OpTime lookupTime) final {
        MERIZO_UNREACHABLE;
    }

    DBClientBase* directClient() final {
        MERIZO_UNREACHABLE;
    }

    bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final;

    void insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                std::vector<BSONObj>&& objs,
                const WriteConcernOptions& wc,
                boost::optional<OID>) final {
        MERIZO_UNREACHABLE;
    }

    void update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                std::vector<BSONObj>&& queries,
                std::vector<BSONObj>&& updates,
                const WriteConcernOptions& wc,
                bool upsert,
                bool multi,
                boost::optional<OID>) final {
        MERIZO_UNREACHABLE;
    }

    CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                          const NamespaceString& ns) final {
        MERIZO_UNREACHABLE;
    }

    void appendLatencyStats(OperationContext* opCtx,
                            const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const final {
        MERIZO_UNREACHABLE;
    }

    Status appendStorageStats(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& param,
                              BSONObjBuilder* builder) const final {
        MERIZO_UNREACHABLE;
    }

    Status appendRecordCount(OperationContext* opCtx,
                             const NamespaceString& nss,
                             BSONObjBuilder* builder) const final {
        MERIZO_UNREACHABLE;
    }

    BSONObj getCollectionOptions(const NamespaceString& nss) final {
        MERIZO_UNREACHABLE;
    }

    void renameIfOptionsAndIndexesHaveNotChanged(OperationContext* opCtx,
                                                 const BSONObj& renameCommandObj,
                                                 const NamespaceString& targetNs,
                                                 const BSONObj& originalCollectionOptions,
                                                 const std::list<BSONObj>& originalIndexes) final {
        MERIZO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) final;

    std::string getShardName(OperationContext* opCtx) const final {
        MERIZO_UNREACHABLE;
    }

    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFieldsForHostedCollection(
        OperationContext* opCtx, const NamespaceString&, UUID) const final {
        MERIZO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions pipelineOptions) final;

    /**
     * The following methods only make sense for data-bearing nodes and should never be called on
     * a merizos.
     */
    BackupCursorState openBackupCursor(OperationContext* opCtx) final {
        MERIZO_UNREACHABLE;
    }

    void closeBackupCursor(OperationContext* opCtx, const UUID& backupId) final {
        MERIZO_UNREACHABLE;
    }

    BackupCursorExtendState extendBackupCursor(OperationContext* opCtx,
                                               const UUID& backupId,
                                               const Timestamp& extendTo) final {
        MERIZO_UNREACHABLE;
    }

    /**
     * Merizos does not have a plan cache, so this method should never be called on merizos. Upstream
     * checks are responsible for generating an error if a user attempts to introspect the plan
     * cache on merizos.
     */
    std::vector<BSONObj> getMatchingPlanCacheEntryStats(OperationContext*,
                                                        const NamespaceString&,
                                                        const MatchExpression*) const final {
        MERIZO_UNREACHABLE;
    }

    bool uniqueKeyIsSupportedByIndex(const boost::intrusive_ptr<ExpressionContext>&,
                                     const NamespaceString&,
                                     const std::set<FieldPath>& uniqueKeyPaths) const final;

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>&,
                                      const NamespaceString&,
                                      ChunkVersion) const final {
        MERIZO_UNREACHABLE;
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
        // This implementation is a no-op, since merizoS does not maintain a SessionCatalog or
        // hold stashed locks for idle sessions.
    }
};

}  // namespace merizo
