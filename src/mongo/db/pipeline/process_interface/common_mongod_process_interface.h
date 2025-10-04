/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
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
#include "mongo/db/pipeline/javascript_execution.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/db/query/client_cursor/generic_cursor_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/write_ops/write_ops_exec.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
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
                                        StringData host,
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
    UUID fetchCollectionUUIDFromPrimary(OperationContext* opCtx,
                                        const NamespaceString& nss) override;
    query_shape::CollectionType getCollectionType(OperationContext* opCtx,
                                                  const NamespaceString& nss) override;
    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        Pipeline* pipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none,
        bool shouldUseCollectionDefaultCollator = false,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{}) final;

    std::unique_ptr<Pipeline> finalizeAndAttachCursorToPipelineForLocalRead(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        Pipeline* ownedPipeline,
        bool attachCursorAfterOptimizing,
        std::function<void(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           Pipeline* pipeline,
                           CollectionMetadata collData)> finalizePipeline = nullptr,
        bool shouldUseCollectionDefaultCollator = false,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{}) final;

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

protected:
    BSONObj getCollectionOptionsLocally(OperationContext* opCtx, const NamespaceString& nss);

    query_shape::CollectionType getCollectionTypeLocally(OperationContext* opCtx,
                                                         const NamespaceString& nss);

    boost::optional<Document> doLookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        boost::optional<UUID> collectionUUID,
        const Document& documentKey,
        MakePipelineOptions opts);

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

private:
    // Object which contains a JavaScript Scope, used for executing JS in pipeline stages and
    // expressions. Owned by the process interface so that there is one common scope for the
    // lifetime of a pipeline.
    std::unique_ptr<JsExecution> _jsExec;
};

}  // namespace mongo
