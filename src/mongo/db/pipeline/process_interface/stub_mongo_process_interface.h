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
    virtual ~StubMongoProcessInterface() = default;

    std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const override {
        MONGO_UNREACHABLE;
    }

    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return false;
    }

    void updateClientOperationTime(OperationContext* opCtx) const override {}

    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::vector<BSONObj>&& objs,
                  const WriteConcernOptions& wc,
                  boost::optional<OID>) override {
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

    boost::optional<BSONObj> getCatalogEntry(OperationContext* opCtx,
                                             const NamespaceString& ns) const override {
        MONGO_UNREACHABLE;
    }

    void appendLatencyStats(OperationContext* opCtx,
                            const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    Status appendStorageStats(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const StorageStatsSpec& spec,
                              BSONObjBuilder* builder) const override {
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

    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) override {
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

    void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const std::vector<BSONObj>& indexSpecs) override {
        MONGO_UNREACHABLE;
    }
    void dropCollection(OperationContext* opCtx, const NamespaceString& ns) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        Pipeline* pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) override {
        MONGO_UNREACHABLE;
    }

    BSONObj preparePipelineAndExplain(Pipeline* ownedPipeline,
                                      ExplainOptions::Verbosity verbosity) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipelineForLocalRead(
        Pipeline* pipeline) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<ShardFilterer> getShardFilterer(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<BSONObj> getCurrentOps(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       CurrentOpConnectionsMode connMode,
                                       CurrentOpSessionsMode sessionMode,
                                       CurrentOpUserMode userMode,
                                       CurrentOpTruncateMode truncateMode,
                                       CurrentOpCursorMode cursorMode,
                                       CurrentOpBacktraceMode backtraceMode) const override {
        MONGO_UNREACHABLE;
    }

    std::string getShardName(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    bool inShardedEnvironment(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    std::string getHostAndPort(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext*, const NamespaceString&) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) {
        MONGO_UNREACHABLE;
    }

    boost::optional<Document> lookupSingleDocumentLocally(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey) {
        MONGO_UNREACHABLE_TASSERT(6148002);
    }

    std::vector<GenericCursor> getIdleCursors(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              CurrentOpUserMode userMode) const {
        MONGO_UNREACHABLE;
    }

    BackupCursorState openBackupCursor(OperationContext* opCtx,
                                       const StorageEngine::BackupOptions& options) override {
        return BackupCursorState{UUID::gen(), boost::none, nullptr, {}};
    }

    void closeBackupCursor(OperationContext* opCtx, const UUID& backupId) final {}

    BackupCursorExtendState extendBackupCursor(OperationContext* opCtx,
                                               const UUID& backupId,
                                               const Timestamp& extendTo) final {
        return {{}};
    }

    std::vector<BSONObj> getMatchingPlanCacheEntryStats(OperationContext*,
                                                        const NamespaceString&,
                                                        const MatchExpression*) const override {
        MONGO_UNREACHABLE;
    }

    bool fieldsHaveSupportingUniqueIndex(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const NamespaceString& nss,
                                         const std::set<FieldPath>& fieldPaths) const override {
        return true;
    }

    boost::optional<ChunkVersion> refreshAndGetCollectionVersion(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss) const override {
        return boost::none;
    }

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString&,
                                      ChunkVersion) const override {
        uasserted(51019, "Unexpected check of routing table");
    }

    std::unique_ptr<ResourceYielder> getResourceYielder(StringData cmdName) const override {
        return nullptr;
    }

    std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
    ensureFieldsUniqueOrResolveDocumentKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           boost::optional<std::set<FieldPath>> fieldPaths,
                                           boost::optional<ChunkVersion> targetCollectionVersion,
                                           const NamespaceString& outputNs) const override {
        if (!fieldPaths) {
            return {std::set<FieldPath>{"_id"}, targetCollectionVersion};
        }

        return {*fieldPaths, targetCollectionVersion};
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

    void checkOnPrimaryShardForDb(OperationContext* opCtx, const NamespaceString& nss) override {
        // Do nothing.
    }

    std::unique_ptr<TemporaryRecordStore> createTemporaryRecordStore(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const {
        MONGO_UNREACHABLE;
    }

    void writeRecordsToRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   RecordStore* rs,
                                   std::vector<Record>* records,
                                   const std::vector<Timestamp>& ts) const {
        MONGO_UNREACHABLE;
    }

    Document readRecordFromRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       RecordStore* rs,
                                       RecordId rID) const {
        MONGO_UNREACHABLE;
    }

    void deleteRecordFromRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     RecordStore* rs,
                                     RecordId rID) const {
        MONGO_UNREACHABLE;
    }

    void truncateRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             RecordStore* rs) const {
        MONGO_UNREACHABLE;
    }
};
}  // namespace mongo
