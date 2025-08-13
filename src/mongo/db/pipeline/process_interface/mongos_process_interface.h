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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/db/query/client_cursor/generic_cursor_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/backup_cursor_state.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <deque>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Class to provide access to mongos-specific implementations of methods required by some
 * document sources.
 */
class MongosProcessInterface : public CommonProcessInterface {
public:
    using CommonProcessInterface::CommonProcessInterface;

    ~MongosProcessInterface() override = default;

    std::unique_ptr<WriteSizeEstimator> getWriteSizeEstimator(
        OperationContext* opCtx, const NamespaceString& ns) const final;

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        boost::optional<UUID> collectionUUID,
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

    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(OperationContext*,
                                                                  const NamespaceString&,
                                                                  RoutingContext*) const final;

    bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final;

    boost::optional<ShardId> determineSpecificMergeShard(OperationContext* opCtx,
                                                         const NamespaceString& ns) const final {
        return CommonProcessInterface::findOwningShard(opCtx, ns);
    }

    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                  const WriteConcernOptions& wc,
                  boost::optional<OID>) final {
        MONGO_UNREACHABLE;
    }

    Status insertTimeseries(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            const NamespaceString& ns,
                            std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                            const WriteConcernOptions& wc,
                            boost::optional<OID> targetEpoch) final {
        MONGO_UNREACHABLE;
    }

    StatusWith<UpdateResult> update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    std::unique_ptr<write_ops::UpdateCommandRequest> updateCommand,
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
                                             const NamespaceString& ns,
                                             const boost::optional<UUID>& collUUID) const final {
        MONGO_UNREACHABLE;
    }

    void appendLatencyStats(OperationContext* opCtx,
                            const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const final {
        MONGO_UNREACHABLE;
    }

    Status appendStorageStats(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const NamespaceString& nss,
                              const StorageStatsSpec& spec,
                              BSONObjBuilder* builder,
                              const boost::optional<BSONObj>& filterObj) const final {
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

    void appendOperationStats(OperationContext* opCtx,
                              const NamespaceString& nss,
                              BSONObjBuilder* builder) const final {
        MONGO_UNREACHABLE;
    }

    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) final {
        MONGO_UNREACHABLE;
    }

    UUID fetchCollectionUUIDFromPrimary(OperationContext* opCtx, const NamespaceString& nss) final {
        MONGO_UNREACHABLE;
    }

    query_shape::CollectionType getCollectionType(OperationContext* opCtx,
                                                  const NamespaceString& nss) final {
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

    void createTempCollection(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& collectionOptions,
                              boost::optional<ShardId> dataShard) final {
        MONGO_UNREACHABLE;
    }

    void createTimeseriesView(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& cmdObj,
                              const TimeseriesOptions& userOpts) final {
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

    void dropTempCollection(OperationContext* opCtx, const NamespaceString& nss) final {
        MONGO_UNREACHABLE;
    }

    BSONObj preparePipelineAndExplain(Pipeline* ownedPipeline,
                                      ExplainOptions::Verbosity verbosity) final;

    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        Pipeline* pipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest,
        bool shouldUseCollectionDefaultCollator,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{}) final {
        // It is not meaningful to perform a "local read" on mongos.
        MONGO_UNREACHABLE;
    }

    std::string getShardName(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    boost::optional<ShardId> getShardId(OperationContext*) const final {
        return {};
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

    SupportingUniqueIndex fieldsHaveSupportingUniqueIndex(
        const boost::intrusive_ptr<ExpressionContext>&,
        const NamespaceString&,
        const std::set<FieldPath>& fieldPaths) const override;

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

    DocumentKeyResolutionMetadata ensureFieldsUniqueOrResolveDocumentKey(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<std::set<FieldPath>> fieldPaths,
        boost::optional<ChunkVersion> targetCollectionPlacementVersion,
        const NamespaceString& outputNs) const override;

    /**
     * If 'allowTargetingShards' is true, splits the pipeline and dispatch half to the shards,
     * leaving the merging half executing in this process after attaching a $mergeCursors. Will
     * retry on network errors and also on StaleConfig errors to avoid restarting the entire
     * operation.
     */
    std::unique_ptr<Pipeline> preparePipelineForExecution(
        Pipeline* pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final;

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        Pipeline* pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final;

    std::unique_ptr<SpillTable> createSpillTable(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const final {
        MONGO_UNREACHABLE;
    }
    void writeRecordsToSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  SpillTable& spillTable,
                                  std::vector<Record>* records) const final {
        MONGO_UNREACHABLE;
    }

    Document readRecordFromSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const SpillTable& spillTable,
                                      RecordId rID) const final {
        MONGO_UNREACHABLE;
    }

    bool checkRecordInSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 const SpillTable& spillTable,
                                 RecordId rID) const final {
        MONGO_UNREACHABLE;
    }

    void deleteRecordFromSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    SpillTable& spillTable,
                                    RecordId rID) const final {
        MONGO_UNREACHABLE;
    }

    void truncateSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            SpillTable& spillTable) const final {
        MONGO_UNREACHABLE;
    }

    std::vector<DatabaseName> getAllDatabases(OperationContext* opCtx,
                                              boost::optional<TenantId> tenantId) final;

    std::vector<BSONObj> runListCollections(OperationContext* opCtx,
                                            const DatabaseName& db,
                                            bool addPrimaryShard) final;

protected:
    BSONObj _reportCurrentOpForClient(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      Client* client,
                                      CurrentOpTruncateMode truncateOps) const final;

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

    void _reportCurrentOpsForQueryAnalysis(OperationContext* opCtx,
                                           std::vector<BSONObj>* ops) const final;
};

}  // namespace mongo
