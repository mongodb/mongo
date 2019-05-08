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
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/mongo_process_common.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {

using write_ops::Insert;
using write_ops::Update;

/**
 * Class to provide access to mongod-specific implementations of methods required by some
 * document sources.
 */
class MongoInterfaceStandalone : public MongoProcessCommon {
public:
    // static std::shared_ptr<MongoProcessInterface> create(OperationContext* opCtx);

    MongoInterfaceStandalone(OperationContext* opCtx);

    virtual ~MongoInterfaceStandalone() = default;

    void setOperationContext(OperationContext* opCtx) final;
    DBClientBase* directClient() final;
    virtual repl::OplogEntry lookUpOplogEntryByOpTime(OperationContext* opCtx,
                                                      repl::OpTime lookupTime) final;
    bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final;
    void insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                std::vector<BSONObj>&& objs,
                const WriteConcernOptions& wc,
                boost::optional<OID> targetEpoch) override;
    void update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                std::vector<BSONObj>&& queries,
                std::vector<write_ops::UpdateModification>&& updates,
                const WriteConcernOptions& wc,
                bool upsert,
                bool multi,
                boost::optional<OID> targetEpoch) override;
    WriteResult updateWithResult(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 const NamespaceString& ns,
                                 std::vector<BSONObj>&& queries,
                                 std::vector<write_ops::UpdateModification>&& updates,
                                 const WriteConcernOptions& wc,
                                 bool upsert,
                                 bool multi,
                                 boost::optional<OID> targetEpoch) override;

    CollectionIndexUsageMap getIndexStats(OperationContext* opCtx, const NamespaceString& ns) final;
    void appendLatencyStats(OperationContext* opCtx,
                            const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const final;
    Status appendStorageStats(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& param,
                              BSONObjBuilder* builder) const final;
    Status appendRecordCount(OperationContext* opCtx,
                             const NamespaceString& nss,
                             BSONObjBuilder* builder) const final;
    BSONObj getCollectionOptions(const NamespaceString& nss) final;
    void renameIfOptionsAndIndexesHaveNotChanged(OperationContext* opCtx,
                                                 const BSONObj& renameCommandObj,
                                                 const NamespaceString& targetNs,
                                                 const BSONObj& originalCollectionOptions,
                                                 const std::list<BSONObj>& originalIndexes) final;
    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts = MakePipelineOptions{}) final;
    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) override;
    std::string getShardName(OperationContext* opCtx) const final;
    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFieldsForHostedCollection(
        OperationContext* opCtx, const NamespaceString&, UUID) const override;
    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext* opCtx, const NamespaceString&) const override;
    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern,
        bool allowSpeculativeMajorityRead = false) final;
    std::vector<GenericCursor> getIdleCursors(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              CurrentOpUserMode userMode) const final;
    BackupCursorState openBackupCursor(OperationContext* opCtx) final;
    void closeBackupCursor(OperationContext* opCtx, const UUID& backupId) final;
    BackupCursorExtendState extendBackupCursor(OperationContext* opCtx,
                                               const UUID& backupId,
                                               const Timestamp& extendTo) final;

    std::vector<BSONObj> getMatchingPlanCacheEntryStats(OperationContext*,
                                                        const NamespaceString&,
                                                        const MatchExpression*) const final;

    bool uniqueKeyIsSupportedByIndex(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     const NamespaceString& nss,
                                     const std::set<FieldPath>& uniqueKeyPaths) const final;

    virtual void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              const NamespaceString& nss,
                                              ChunkVersion targetCollectionVersion) const override {
        uasserted(51020, "unexpected request to consult sharding catalog on non-shardsvr");
    }

    std::unique_ptr<ResourceYielder> getResourceYielder() const override;

protected:
    BSONObj _reportCurrentOpForClient(OperationContext* opCtx,
                                      Client* client,
                                      CurrentOpTruncateMode truncateOps) const final;

    void _reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                          CurrentOpUserMode userMode,
                                          std::vector<BSONObj>* ops) const final;

    /**
     * Builds an ordered insert op on namespace 'nss' and documents to be written 'objs'.
     */
    Insert buildInsertOp(const NamespaceString& nss,
                         std::vector<BSONObj>&& objs,
                         bool bypassDocValidation);

    /**
     * Builds an ordered update op on namespace 'nss' with update entries {q: <queries>, u:
     * <updates>}.
     *
     * Note that 'queries' and 'updates' must be the same length.
     */
    Update buildUpdateOp(const NamespaceString& nss,
                         std::vector<BSONObj>&& queries,
                         std::vector<write_ops::UpdateModification>&& updates,
                         bool upsert,
                         bool multi,
                         bool bypassDocValidation);

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

    DBDirectClient _client;
    std::map<UUID, std::unique_ptr<const CollatorInterface>> _collatorCache;
};

}  // namespace mongo
