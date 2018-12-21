
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

#include "mongo/db/pipeline/mongo_process_interface.h"
#include "mongo/db/pipeline/pipeline.h"

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
    virtual ~StubMongoProcessInterface() = default;

    void setOperationContext(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    DBClientBase* directClient() override {
        MONGO_UNREACHABLE;
    }

    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return false;
    }

    void insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                std::vector<BSONObj>&& objs,
                const WriteConcernOptions& wc,
                boost::optional<OID>) override {
        MONGO_UNREACHABLE;
    }

    void update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const NamespaceString& ns,
                std::vector<BSONObj>&& queries,
                std::vector<BSONObj>&& updates,
                const WriteConcernOptions& wc,
                bool upsert,
                bool multi,
                boost::optional<OID>) final {
        MONGO_UNREACHABLE;
    }

    CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                          const NamespaceString& ns) override {
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
                              const BSONObj& param,
                              BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    Status appendRecordCount(OperationContext* opCtx,
                             const NamespaceString& nss,
                             BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    BSONObj getCollectionOptions(const NamespaceString& nss) override {
        MONGO_UNREACHABLE;
    }

    void renameIfOptionsAndIndexesHaveNotChanged(
        OperationContext* opCtx,
        const BSONObj& renameCommandObj,
        const NamespaceString& targetNs,
        const BSONObj& originalCollectionOptions,
        const std::list<BSONObj>& originalIndexes) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) override {
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

    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFieldsForHostedCollection(
        OperationContext*, const NamespaceString&, UUID) const override {
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
        boost::optional<BSONObj> readConcern,
        bool allowSpeculativeMajorityRead) {
        MONGO_UNREACHABLE;
    }

    std::vector<GenericCursor> getIdleCursors(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              CurrentOpUserMode userMode) const {
        MONGO_UNREACHABLE;
    }

    BackupCursorState openBackupCursor(OperationContext* opCtx) final {
        return BackupCursorState{UUID::gen(), boost::none, {}};
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

    bool uniqueKeyIsSupportedByIndex(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     const NamespaceString& nss,
                                     const std::set<FieldPath>& uniqueKeyPaths) const override {
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

    std::unique_ptr<ResourceYielder> getResourceYielder() const override {
        return nullptr;
    }
};
}  // namespace mongo
