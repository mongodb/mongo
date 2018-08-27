/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/mongo_process_common.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {

/**
 * Class to provide access to mongod-specific implementations of methods required by some
 * document sources.
 */
class MongoDInterface : public MongoProcessCommon {
public:
    static std::shared_ptr<MongoProcessInterface> create(OperationContext* opCtx);

    MongoDInterface(OperationContext* opCtx);

    virtual ~MongoDInterface() = default;

    void setOperationContext(OperationContext* opCtx) final;
    DBClientBase* directClient() final;
    bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final;
    virtual void insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const NamespaceString& ns,
                        const std::vector<BSONObj>& objs);
    virtual void update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const NamespaceString& ns,
                        const std::vector<BSONObj>& queries,
                        const std::vector<BSONObj>& updates,
                        bool upsert,
                        bool multi);
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
    StatusWith<std::unique_ptr<Pipeline, PipelineDeleter>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts = MakePipelineOptions{}) final;
    Status attachCursorSourceToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        Pipeline* pipeline) final;
    std::string getShardName(OperationContext* opCtx) const final;
    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFields(
        OperationContext* opCtx, NamespaceStringOrUUID nssOrUUID) const final;
    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) final;
    std::vector<GenericCursor> getCursors(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const final;
    void fsyncLock(OperationContext* opCtx) final;
    void fsyncUnlock(OperationContext* opCtx) final;
    BackupCursorState openBackupCursor(OperationContext* opCtx) final;
    void closeBackupCursor(OperationContext* opCtx, std::uint64_t cursorId) final;

    std::vector<BSONObj> getMatchingPlanCacheEntryStats(OperationContext*,
                                                        const NamespaceString&,
                                                        const MatchExpression*) const final;

    bool uniqueKeyIsSupportedByIndex(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     const NamespaceString& nss,
                                     const std::set<FieldPath>& uniqueKeyPaths) const final;

protected:
    BSONObj _reportCurrentOpForClient(OperationContext* opCtx,
                                      Client* client,
                                      CurrentOpTruncateMode truncateOps) const final;

    void _reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                          CurrentOpUserMode userMode,
                                          std::vector<BSONObj>* ops) const final;

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

/**
 * Specialized version of the MongoDInterface when this node is a shard server.
 */
class MongoDInterfaceShardServer final : public MongoDInterface {
public:
    using MongoDInterface::MongoDInterface;

    /**
     * Inserts the documents 'objs' into the namespace 'ns' using the ClusterWriter for locking,
     * routing, stale config handling, etc.
     */
    void insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                const std::vector<BSONObj>& objs) final;

    /**
     * Replaces the documents matching 'queries' with 'updates' using the ClusterWriter for locking,
     * routing, stale config handling, etc.
     */
    void update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                const std::vector<BSONObj>& queries,
                const std::vector<BSONObj>& updates,
                bool upsert,
                bool multi) final;
};

}  // namespace mongo
