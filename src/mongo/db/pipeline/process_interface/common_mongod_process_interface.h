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

#include "mongo/db/client.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/javascript_execution.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"

namespace mongo {

/**
 * Provides the implementations of interfaces that are shared across different types of mongod
 * nodes.
 */
class CommonMongodProcessInterface : public CommonProcessInterface {
public:
    using CommonProcessInterface::CommonProcessInterface;

    virtual ~CommonMongodProcessInterface() = default;

    std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const final;

    std::vector<Document> getIndexStats(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        StringData host,
                                        bool addShardName) final;

    std::deque<BSONObj> listCatalog(OperationContext* opCtx) const final;

    boost::optional<BSONObj> getCatalogEntry(OperationContext* opCtx,
                                             const NamespaceString& ns) const final;

    void appendLatencyStats(OperationContext* opCtx,
                            const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const final;
    Status appendStorageStats(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const StorageStatsSpec& spec,
                              BSONObjBuilder* builder) const final;
    Status appendRecordCount(OperationContext* opCtx,
                             const NamespaceString& nss,
                             BSONObjBuilder* builder) const final;
    Status appendQueryExecStats(OperationContext* opCtx,
                                const NamespaceString& nss,
                                BSONObjBuilder* builder) const final override;
    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) override;
    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipelineForLocalRead(
        Pipeline* pipeline) final;
    std::string getShardName(OperationContext* opCtx) const final;

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

    bool fieldsHaveSupportingUniqueIndex(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const NamespaceString& nss,
                                         const std::set<FieldPath>& fieldPaths) const;

    std::unique_ptr<ResourceYielder> getResourceYielder(StringData cmdName) const final;

    std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
    ensureFieldsUniqueOrResolveDocumentKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           boost::optional<std::set<FieldPath>> fieldPaths,
                                           boost::optional<ChunkVersion> targetCollectionVersion,
                                           const NamespaceString& outputNs) const final;

    std::unique_ptr<TemporaryRecordStore> createTemporaryRecordStore(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const final;

    void writeRecordsToRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   RecordStore* rs,
                                   std::vector<Record>* records,
                                   const std::vector<Timestamp>& ts) const final;

    Document readRecordFromRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       RecordStore* rs,
                                       RecordId rID) const final;

    void deleteRecordFromRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     RecordStore* rs,
                                     RecordId rID) const final;

    void truncateRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             RecordStore* rs) const final;

    boost::optional<Document> lookupSingleDocumentLocally(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey) final;

protected:
    BSONObj getCollectionOptionsLocally(OperationContext* opCtx, const NamespaceString& nss);

    boost::optional<Document> doLookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        MakePipelineOptions opts);

    /**
     * Builds an ordered insert op on namespace 'nss' and documents to be written 'objs'.
     */
    write_ops::InsertCommandRequest buildInsertOp(const NamespaceString& nss,
                                                  std::vector<BSONObj>&& objs,
                                                  bool bypassDocValidation);

    /**
     * Builds an ordered update op on namespace 'nss' with update entries contained in 'batch'.
     */
    write_ops::UpdateCommandRequest buildUpdateOp(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        BatchedObjects&& batch,
        UpsertType upsert,
        bool multi);

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

    /**
     * Converts a renameCollection command into an internalRenameIfOptionsAndIndexesMatch command.
     */
    BSONObj _convertRenameToInternalRename(OperationContext* opCtx,
                                           const NamespaceString& sourceNs,
                                           const NamespaceString& targetNs,
                                           const BSONObj& originalCollectionOptions,
                                           const std::list<BSONObj>& originalIndexes);

private:
    /**
     * Looks up the collection default collator for the collection given by 'collectionUUID'. A
     * collection's default collation is not allowed to change, so we cache the result to allow
     * for quick lookups in the future. Looks up the collection by UUID, and returns 'nullptr'
     * if the collection does not exist or if the collection's default collation is the simple
     * collation.
     */
    std::unique_ptr<CollatorInterface> _getCollectionDefaultCollator(OperationContext* opCtx,
                                                                     StringData dbName,
                                                                     UUID collectionUUID);

    std::map<UUID, std::unique_ptr<const CollatorInterface>> _collatorCache;

    // Object which contains a JavaScript Scope, used for executing JS in pipeline stages and
    // expressions. Owned by the process interface so that there is one common scope for the
    // lifetime of a pipeline.
    std::unique_ptr<JsExecution> _jsExec;
};

}  // namespace mongo
