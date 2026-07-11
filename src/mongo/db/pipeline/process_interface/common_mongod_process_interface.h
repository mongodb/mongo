// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/catalog_resource_handle.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/javascript_execution.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/db/query/client_cursor/generic_cursor_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/write_ops/write_ops_exec.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/shard_role/resource_yielder.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/storage/backup_cursor_state.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/spill_table.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <deque>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Provides the implementations of interfaces that are shared across different types of mongod
 * nodes.
 */
class CommonMongodProcessInterface : public CommonProcessInterface {
public:
    using CommonProcessInterface::CommonProcessInterface;

    ~CommonMongodProcessInterface() override = default;

    std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const final;

    std::vector<Document> getIndexStats(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        std::string_view host,
                                        bool addShardName) final;

    std::deque<BSONObj> listCatalog(OperationContext* opCtx) const final;

    boost::optional<BSONObj> getCatalogEntry(OperationContext* opCtx,
                                             const NamespaceString& ns,
                                             const boost::optional<UUID>& collUUID) const final;

    void appendLatencyStats(OperationContext* opCtx,
                            const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const final;
    Status appendStorageStats(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const NamespaceString& nss,
                              const StorageStatsSpec& spec,
                              BSONObjBuilder* builder,
                              const boost::optional<BSONObj>& filterObj) const final;
    Status appendRecordCount(OperationContext* opCtx,
                             const NamespaceString& nss,
                             BSONObjBuilder* builder) const final;
    Status appendQueryExecStats(OperationContext* opCtx,
                                const NamespaceString& nss,
                                BSONObjBuilder* builder) const final;
    void appendOperationStats(OperationContext* opCtx,
                              const NamespaceString& nss,
                              BSONObjBuilder* builder) const final;
    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) override;
    ListCollectionsReplyItem getCollectionInfoFromPrimary(
        OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) override;

    query_shape::CollectionType getCollectionType(OperationContext* opCtx,
                                                  const NamespaceString& nss) override;

    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalReadWithCatalog(
        std::unique_ptr<Pipeline> pipeline,
        const MultipleCollectionAccessor& collections,
        const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle) final;

    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        std::unique_ptr<Pipeline> pipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final;

    std::unique_ptr<Pipeline> finalizeAndAttachCursorToPipelineForLocalRead(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<Pipeline> pipeline,
        bool attachCursorAfterOptimizing,
        std::function<void(Pipeline* pipeline)> optimizePipeline = nullptr,
        bool shouldUseCollectionDefaultCollator = false,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none) final;

    std::string getShardName(OperationContext* opCtx) const final;

    boost::optional<ShardId> getShardId(OperationContext* opCtx) const final;

    bool inShardedEnvironment(OperationContext* opCtx) const final;

    std::vector<GenericCursor> getIdleCursors(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              CurrentOpUserMode userMode) const final;
    BackupCursorState openBackupCursor(OperationContext* opCtx,
                                       const StorageEngine::BackupOptions& options) final;
    void closeBackupCursor(OperationContext* opCtx, const UUID& backupId) final;
    BackupCursorExtendState extendBackupCursor(OperationContext* opCtx,
                                               const UUID& backupId,
                                               const Timestamp& extendTo) final;

    std::vector<BSONObj> getMatchingPlanCacheEntryStats(OperationContext*,
                                                        const NamespaceString&,
                                                        const MatchExpression*) const final;

    SupportingUniqueIndex fieldsHaveSupportingUniqueIndex(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const std::set<FieldPath>& fieldPaths) const override;

    DocumentKeyResolutionMetadata ensureFieldsUniqueOrResolveDocumentKey(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<std::set<FieldPath>> fieldPaths,
        boost::optional<ChunkVersion> targetCollectionPlacementVersion,
        const NamespaceString& outputNs) const final;

    std::unique_ptr<SpillTable> createSpillTable(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const final;

    void writeRecordsToSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  SpillTable& spillTable,
                                  std::vector<Record>* records) const final;

    Document readRecordFromSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const SpillTable& spillTable,
                                      RecordId rID) const final;

    bool checkRecordInSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 const SpillTable& spillTable,
                                 RecordId rID) const final;

    void deleteRecordFromSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    SpillTable& spillTable,
                                    RecordId rID) const final;

    void truncateSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            SpillTable& spillTable) const final;

    boost::optional<Document> lookupSingleDocumentLocally(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey) final;

    boost::optional<ScopedSetShardRole> setLocalRouting(
        OperationContext* opCtx, const NamespaceString& subPipelineNss) final;

protected:
    BSONObj getCollectionOptionsLocally(OperationContext* opCtx, const NamespaceString& nss);

    query_shape::CollectionType getCollectionTypeLocally(OperationContext* opCtx,
                                                         const NamespaceString& nss);

    boost::optional<Document> doLookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        boost::optional<UUID> collectionUUID,
        const Document& documentKey,
        pipeline_factory::MakePipelineOptions opts);

    BSONObj _reportCurrentOpForClient(WithLock,
                                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
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

    /**
     * Converts a renameCollection command into an internalRenameIfOptionsAndIndexesMatch command.
     */
    BSONObj _convertRenameToInternalRename(OperationContext* opCtx,
                                           const NamespaceString& sourceNs,
                                           const NamespaceString& targetNs,
                                           const BSONObj& originalCollectionOptions,
                                           const std::vector<BSONObj>& originalIndexes);

    void _handleTimeseriesCreateError(const DBException& ex,
                                      OperationContext* opCtx,
                                      const NamespaceString& ns,
                                      TimeseriesOptions userOpts);

    /**
     * If passed namespace is a timeseries, returns TimeseriesOptions. Otherwise, returns
     * boost::none.
     */
    virtual boost::optional<TimeseriesOptions> _getTimeseriesOptions(OperationContext* opCtx,
                                                                     const NamespaceString& ns);

    /**
     * Runs the given command against the primary of the database, returning the raw command result.
     * The default implementation runs the command locally via DBDirectClient (suitable when this
     * node is the primary). Subclasses that may not be the primary should override this to route
     * the command appropriately.
     */
    virtual BSONObj runDatabaseCommandOnPrimary(OperationContext* opCtx,
                                                const DatabaseName& dbName,
                                                const BSONObj& cmdBSON);

private:
    // Object which contains a JavaScript Scope, used for executing JS in pipeline stages and
    // expressions. Owned by the process interface so that there is one common scope for the
    // lifetime of a pipeline.
    std::unique_ptr<JsExecution> _jsExec;
};

}  // namespace mongo
