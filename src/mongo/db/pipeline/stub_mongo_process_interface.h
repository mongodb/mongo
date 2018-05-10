/**
 *    Copyright (C) 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
        MONGO_UNREACHABLE;
    }

    BSONObj insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   const NamespaceString& ns,
                   const std::vector<BSONObj>& objs) override {
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

    Status renameIfOptionsAndIndexesHaveNotChanged(
        OperationContext* opCtx,
        const BSONObj& renameCommandObj,
        const NamespaceString& targetNs,
        const BSONObj& originalCollectionOptions,
        const std::list<BSONObj>& originalIndexes) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<std::unique_ptr<Pipeline, PipelineDeleter>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts) override {
        MONGO_UNREACHABLE;
    }

    Status attachCursorSourceToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        Pipeline* pipeline) override {
        MONGO_UNREACHABLE;
    }

    std::vector<BSONObj> getCurrentOps(OperationContext* opCtx,
                                       CurrentOpConnectionsMode connMode,
                                       CurrentOpSessionsMode sessionMode,
                                       CurrentOpUserMode userMode,
                                       CurrentOpTruncateMode truncateMode) const override {
        MONGO_UNREACHABLE;
    }

    std::string getShardName(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFields(OperationContext*,
                                                                     UUID) const override {
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

    std::vector<GenericCursor> getCursors(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
        MONGO_UNREACHABLE;
    }
};
}  // namespace mongo
