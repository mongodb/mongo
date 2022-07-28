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

#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"

namespace mongo {

/**
 * Class to provide access to mongos-specific implementations of methods required by some
 * document sources.
 */
class MongosProcessInterface : public CommonProcessInterface {
public:
    using CommonProcessInterface::CommonProcessInterface;

    virtual ~MongosProcessInterface() = default;

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) final;

    boost::optional<Document> lookupSingleDocumentLocally(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey) final;

    std::vector<GenericCursor> getIdleCursors(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              CurrentOpUserMode userMode) const final;

    std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const override {
        MONGO_UNREACHABLE;
    }

    bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final;

    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::vector<BSONObj>&& objs,
                  const WriteConcernOptions& wc,
                  boost::optional<OID>) final {
        MONGO_UNREACHABLE;
    }

    StatusWith<UpdateResult> update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    BatchedObjects&& batch,
                                    const WriteConcernOptions& wc,
                                    UpsertType upsert,
                                    bool multi,
                                    boost::optional<OID>) final {
        MONGO_UNREACHABLE;
    }

    std::vector<Document> getIndexStats(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        StringData host,
                                        bool addShardName) final {
        MONGO_UNREACHABLE;
    }

    std::list<BSONObj> getIndexSpecs(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     bool includeBuildUUIDs) final {
        MONGO_UNREACHABLE;
    }

    std::deque<BSONObj> listCatalog(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    boost::optional<BSONObj> getCatalogEntry(OperationContext* opCtx,
                                             const NamespaceString& ns) const final {
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
                              const StorageStatsSpec& spec,
                              BSONObjBuilder* builder) const final {
        MONGO_UNREACHABLE;
    }

    Status appendRecordCount(OperationContext* opCtx,
                             const NamespaceString& nss,
                             BSONObjBuilder* builder) const final {
        MONGO_UNREACHABLE;
    }

    Status appendQueryExecStats(OperationContext* opCtx,
                                const NamespaceString& nss,
                                BSONObjBuilder* builder) const final {
        MONGO_UNREACHABLE;
    }

    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) final {
        MONGO_UNREACHABLE;
    }

    void renameIfOptionsAndIndexesHaveNotChanged(OperationContext* opCtx,
                                                 const NamespaceString& sourceNs,
                                                 const NamespaceString& targetNs,
                                                 bool dropTarget,
                                                 bool stayTemp,
                                                 const BSONObj& originalCollectionOptions,
                                                 const std::list<BSONObj>& originalIndexes) final {
        MONGO_UNREACHABLE;
    }

    void createCollection(OperationContext* opCtx,
                          const DatabaseName& dbName,
                          const BSONObj& cmdObj) final {
        MONGO_UNREACHABLE;
    }

    void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const std::vector<BSONObj>& indexSpecs) final {
        MONGO_UNREACHABLE;
    }

    void dropCollection(OperationContext* opCtx, const NamespaceString& collection) final {
        MONGO_UNREACHABLE;
    }

    BSONObj preparePipelineAndExplain(Pipeline* ownedPipeline,
                                      ExplainOptions::Verbosity verbosity) final;

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipelineForLocalRead(
        Pipeline* pipeline) final {
        // It is not meaningful to perform a "local read" on mongos.
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<ShardFilterer> getShardFilterer(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const override {
        return nullptr;
    }

    std::string getShardName(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    bool inShardedEnvironment(OperationContext* opCtx) const final {
        return true;
    }

    /**
     * The following methods only make sense for data-bearing nodes and should never be called on
     * a mongos.
     */
    BackupCursorState openBackupCursor(OperationContext* opCtx,
                                       const StorageEngine::BackupOptions& options) final {
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

    bool fieldsHaveSupportingUniqueIndex(const boost::intrusive_ptr<ExpressionContext>&,
                                         const NamespaceString&,
                                         const std::set<FieldPath>& fieldPaths) const;

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>&,
                                      const NamespaceString&,
                                      ChunkVersion) const final {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<ScopedExpectUnshardedCollection> expectUnshardedCollectionInScope(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const boost::optional<DatabaseVersion>& dbVersion) override {
        MONGO_UNREACHABLE;
    }

    void checkOnPrimaryShardForDb(OperationContext* opCtx, const NamespaceString& nss) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<ResourceYielder> getResourceYielder(StringData cmdName) const override {
        return nullptr;
    }

    std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
    ensureFieldsUniqueOrResolveDocumentKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           boost::optional<std::set<FieldPath>> fieldPaths,
                                           boost::optional<ChunkVersion> targetCollectionVersion,
                                           const NamespaceString& outputNs) const override;

    /**
     * If 'allowTargetingShards' is true, splits the pipeline and dispatch half to the shards,
     * leaving the merging half executing in this process after attaching a $mergeCursors. Will
     * retry on network errors and also on StaleConfig errors to avoid restarting the entire
     * operation.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        Pipeline* pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final;

    std::unique_ptr<TemporaryRecordStore> createTemporaryRecordStore(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const final {
        MONGO_UNREACHABLE;
    }
    void writeRecordsToRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   RecordStore* rs,
                                   std::vector<Record>* records,
                                   const std::vector<Timestamp>& ts) const final {
        MONGO_UNREACHABLE;
    }

    Document readRecordFromRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       RecordStore* rs,
                                       RecordId rID) const final {
        MONGO_UNREACHABLE;
    }

    void deleteRecordFromRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     RecordStore* rs,
                                     RecordId rID) const final {
        MONGO_UNREACHABLE;
    }

    void truncateRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             RecordStore* rs) const final {
        MONGO_UNREACHABLE;
    }

protected:
    BSONObj _reportCurrentOpForClient(OperationContext* opCtx,
                                      Client* client,
                                      CurrentOpTruncateMode truncateOps,
                                      CurrentOpBacktraceMode backtraceMode) const final;

    void _reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                          CurrentOpUserMode userMode,
                                          std::vector<BSONObj>* ops) const final;

    void _reportCurrentOpsForTransactionCoordinators(OperationContext* opCtx,
                                                     bool includeIdle,
                                                     std::vector<BSONObj>* ops) const final;

    void _reportCurrentOpsForPrimaryOnlyServices(OperationContext* opCtx,
                                                 CurrentOpConnectionsMode connMode,
                                                 CurrentOpSessionsMode sessionMode,
                                                 std::vector<BSONObj>* ops) const final;
};

}  // namespace mongo
