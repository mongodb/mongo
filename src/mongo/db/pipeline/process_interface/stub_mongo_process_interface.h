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

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * A stub MongoProcessInterface that provides default implementations of all methods, which can then
 * be individually overridden for testing. This class may also be used in scenarios where a
 * placeholder MongoProcessInterface is required by an interface but will not be called. To
 * guarantee the latter, method implementations in this class are marked MONGO_UNREACHABLE.
 */
class StubMongoProcessInterface : public MongoProcessInterface {
public:
    StubMongoProcessInterface() : MongoProcessInterface(nullptr) {}
    using MongoProcessInterface::MongoProcessInterface;
    ~StubMongoProcessInterface() override = default;

    std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const override {
        MONGO_UNREACHABLE;
    }

    class StubWriteSizeEstimator final : public WriteSizeEstimator {
    public:
        int estimateInsertHeaderSize(
            const write_ops::InsertCommandRequest& insertReq) const override {
            return 0;
        }

        int estimateUpdateHeaderSize(
            const write_ops::UpdateCommandRequest& insertReq) const override {
            return 0;
        }

        int estimateInsertSizeBytes(const BSONObj& insert) const override {
            MONGO_UNREACHABLE;
        }

        int estimateUpdateSizeBytes(const BatchObject& batchObject,
                                    UpsertType type) const override {
            MONGO_UNREACHABLE;
        }
    };

    std::unique_ptr<WriteSizeEstimator> getWriteSizeEstimator(
        OperationContext* opCtx, const NamespaceString& ns) const override {
        return std::make_unique<StubWriteSizeEstimator>();
    }

    bool isExpectedToExecuteQueries() override {
        return false;
    }

    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return false;
    }

    boost::optional<ShardId> determineSpecificMergeShard(
        OperationContext* opCtx, const NamespaceString& nss) const override {
        return boost::none;
    };

    void updateClientOperationTime(OperationContext* opCtx) const override {}

    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                  const WriteConcernOptions& wc,
                  boost::optional<OID>) override {
        MONGO_UNREACHABLE;
    }

    Status insertTimeseries(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            const NamespaceString& ns,
                            std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                            const WriteConcernOptions& wc,
                            boost::optional<OID> targetEpoch) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<UpdateResult> update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    std::unique_ptr<write_ops::UpdateCommandRequest> updateCommand,
                                    const WriteConcernOptions& wc,
                                    UpsertType upsert,
                                    bool multi,
                                    boost::optional<OID>) override {
        MONGO_UNREACHABLE;
    }

    std::vector<Document> getIndexStats(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        StringData host,
                                        bool addShardName) override {
        MONGO_UNREACHABLE;
    }

    std::list<BSONObj> getIndexSpecs(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     bool includeBuildUUIDs) override {
        MONGO_UNREACHABLE;
    }

    std::deque<BSONObj> listCatalog(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    void createTimeseriesView(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& cmdObj,
                              const TimeseriesOptions& userOpts) final {
        MONGO_UNREACHABLE;
    }

    boost::optional<BSONObj> getCatalogEntry(OperationContext* opCtx,
                                             const NamespaceString& ns,
                                             const boost::optional<UUID>& collUUID) const override {
        MONGO_UNREACHABLE;
    }

    void appendLatencyStats(OperationContext* opCtx,
                            const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    Status appendStorageStats(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const NamespaceString& nss,
                              const StorageStatsSpec& spec,
                              BSONObjBuilder* builder,
                              const boost::optional<BSONObj>& filterObj) const override {
        MONGO_UNREACHABLE;
    }

    Status appendRecordCount(OperationContext* opCtx,
                             const NamespaceString& nss,
                             BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    Status appendQueryExecStats(OperationContext* opCtx,
                                const NamespaceString& nss,
                                BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    void appendOperationStats(OperationContext* opCtx,
                              const NamespaceString& nss,
                              BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) override {
        MONGO_UNREACHABLE;
    }

    UUID fetchCollectionUUIDFromPrimary(OperationContext* opCtx, const NamespaceString& nss) final {
        MONGO_UNREACHABLE;
    }

    query_shape::CollectionType getCollectionType(OperationContext* opCtx,
                                                  const NamespaceString& nss) final {
        MONGO_UNREACHABLE;
    }

    void renameIfOptionsAndIndexesHaveNotChanged(
        OperationContext* opCtx,
        const NamespaceString& sourceNs,
        const NamespaceString& targetNs,
        bool dropTarget,
        bool stayTemp,
        const BSONObj& originalCollectionOptions,
        const std::list<BSONObj>& originalIndexes) override {
        MONGO_UNREACHABLE;
    }

    void createCollection(OperationContext* opCtx,
                          const DatabaseName& dbName,
                          const BSONObj& cmdObj) override {
        MONGO_UNREACHABLE;
    }

    void createTempCollection(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& collectionOptions,
                              boost::optional<ShardId> dataShard) override {
        MONGO_UNREACHABLE;
    }

    void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const std::vector<BSONObj>& indexSpecs) override {
        MONGO_UNREACHABLE;
    }

    void dropCollection(OperationContext* opCtx, const NamespaceString& ns) override {
        MONGO_UNREACHABLE;
    }

    void dropTempCollection(OperationContext* opCtx, const NamespaceString& nss) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        Pipeline* pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<mongo::ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        Pipeline* pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) override {
        MONGO_UNREACHABLE;
    }

    BSONObj preparePipelineAndExplain(Pipeline* ownedPipeline,
                                      ExplainOptions::Verbosity verbosity) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        Pipeline* pipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none,
        bool shouldUseCollectionDefaultCollator = false,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{}) override {
        MONGO_UNREACHABLE;
    }

    std::vector<BSONObj> getCurrentOps(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       CurrentOpConnectionsMode connMode,
                                       CurrentOpSessionsMode sessionMode,
                                       CurrentOpUserMode userMode,
                                       CurrentOpTruncateMode truncateMode,
                                       CurrentOpCursorMode cursorMode) const override {
        MONGO_UNREACHABLE;
    }

    std::string getShardName(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<ShardId> getShardId(OperationContext* opCtx) const override {
        return boost::none;
    }

    bool inShardedEnvironment(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    std::string getHostAndPort(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext*,
        const NamespaceString&,
        RoutingContext* routingCtx = nullptr) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        boost::optional<UUID> collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<Document> lookupSingleDocumentLocally(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey) override {
        MONGO_UNREACHABLE_TASSERT(6148002);
    }

    std::vector<GenericCursor> getIdleCursors(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              CurrentOpUserMode userMode) const override {
        MONGO_UNREACHABLE;
    }

    BackupCursorState openBackupCursor(OperationContext* opCtx,
                                       const StorageEngine::BackupOptions& options) override {
        return BackupCursorState{UUID::gen(), boost::none, nullptr, {}};
    }

    void closeBackupCursor(OperationContext* opCtx, const UUID& backupId) override {}

    BackupCursorExtendState extendBackupCursor(OperationContext* opCtx,
                                               const UUID& backupId,
                                               const Timestamp& extendTo) override {
        return {{}};
    }

    std::vector<BSONObj> getMatchingPlanCacheEntryStats(OperationContext*,
                                                        const NamespaceString&,
                                                        const MatchExpression*) const override {
        MONGO_UNREACHABLE;
    }

    SupportingUniqueIndex fieldsHaveSupportingUniqueIndex(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const std::set<FieldPath>& fieldPaths) const override {
        return SupportingUniqueIndex::Full;
    }

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString&,
                                      ChunkVersion) const override {
        uasserted(51019, "Unexpected check of routing table");
    }

    std::vector<DatabaseName> getAllDatabases(OperationContext* opCtx,
                                              boost::optional<TenantId> tenantId) override {
        MONGO_UNREACHABLE_TASSERT(9525801);
    }

    std::vector<BSONObj> runListCollections(OperationContext* opCtx,
                                            const DatabaseName& db,
                                            bool addPrimaryShard) override {
        MONGO_UNREACHABLE_TASSERT(9525802);
    }

    DocumentKeyResolutionMetadata ensureFieldsUniqueOrResolveDocumentKey(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<std::set<FieldPath>> fieldPaths,
        boost::optional<ChunkVersion> targetCollectionPlacementVersion,
        const NamespaceString& outputNs) const override {
        if (!fieldPaths) {
            return {std::set<FieldPath>{"_id"},
                    targetCollectionPlacementVersion,
                    SupportingUniqueIndex::Full};
        }

        return {*fieldPaths, targetCollectionPlacementVersion, SupportingUniqueIndex::Full};
    }

    std::unique_ptr<ScopedExpectUnshardedCollection> expectUnshardedCollectionInScope(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const boost::optional<DatabaseVersion>& dbVersion) override {
        class ScopedExpectUnshardedCollectionNoop : public ScopedExpectUnshardedCollection {
        public:
            ScopedExpectUnshardedCollectionNoop() = default;
        };

        return std::make_unique<ScopedExpectUnshardedCollectionNoop>();
    }

    std::unique_ptr<SpillTable> createSpillTable(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const override {
        MONGO_UNREACHABLE;
    }

    void writeRecordsToSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  SpillTable& spillTable,
                                  std::vector<Record>* records) const override {
        MONGO_UNREACHABLE;
    }

    Document readRecordFromSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const SpillTable& spillTable,
                                      RecordId rID) const override {
        MONGO_UNREACHABLE;
    }

    bool checkRecordInSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 const SpillTable& spillTable,
                                 RecordId rID) const override {
        MONGO_UNREACHABLE;
    }

    void deleteRecordFromSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    SpillTable& spillTable,
                                    RecordId rID) const override {
        MONGO_UNREACHABLE;
    }

    void truncateSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            SpillTable& spillTable) const override {
        MONGO_UNREACHABLE;
    }
};
}  // namespace mongo
