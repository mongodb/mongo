/**
 * Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/pipeline/mongo_process_common.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
/**
 * PipelineS is an extension of the Pipeline class to provide additional utility functions on
 * mongoS. For example, it can inject the pipeline with an implementation of MongoProcessInterface
 * which provides mongos-specific versions of methods required by some document sources.
 */
class PipelineS {
public:
    /**
     * Class to provide access to mongos-specific implementations of methods required by some
     * document sources.
     */
    class MongoSInterface final : public MongoProcessCommon {
    public:
        MongoSInterface() = default;

        virtual ~MongoSInterface() = default;

        void setOperationContext(OperationContext* opCtx) final {}

        boost::optional<Document> lookupSingleDocument(
            const boost::intrusive_ptr<ExpressionContext>& expCtx,
            const NamespaceString& nss,
            UUID collectionUUID,
            const Document& documentKey,
            boost::optional<BSONObj> readConcern) final;

        std::vector<GenericCursor> getCursors(
            const boost::intrusive_ptr<ExpressionContext>& expCtx) const final;

        DBClientBase* directClient() final {
            MONGO_UNREACHABLE;
        }

        bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final {
            MONGO_UNREACHABLE;
        }

        BSONObj insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const NamespaceString& ns,
                       const std::vector<BSONObj>& objs) final {
            MONGO_UNREACHABLE;
        }

        CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                              const NamespaceString& ns) final {
            MONGO_UNREACHABLE;
        }

        void appendLatencyStats(OperationContext* opCtx,
                                const NamespaceString& nss,
                                bool includeHistograms,
                                BSONObjBuilder* builder) const final {
            MONGO_UNREACHABLE;
        }

        Status appendStorageStats(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const BSONObj& param,
                                  BSONObjBuilder* builder) const final {
            MONGO_UNREACHABLE;
        }

        Status appendRecordCount(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 BSONObjBuilder* builder) const final {
            MONGO_UNREACHABLE;
        }

        BSONObj getCollectionOptions(const NamespaceString& nss) final {
            MONGO_UNREACHABLE;
        }

        Status renameIfOptionsAndIndexesHaveNotChanged(
            OperationContext* opCtx,
            const BSONObj& renameCommandObj,
            const NamespaceString& targetNs,
            const BSONObj& originalCollectionOptions,
            const std::list<BSONObj>& originalIndexes) final {
            MONGO_UNREACHABLE;
        }

        Status attachCursorSourceToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            Pipeline* pipeline) final {
            MONGO_UNREACHABLE;
        }

        std::string getShardName(OperationContext* opCtx) const final {
            MONGO_UNREACHABLE;
        }

        std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFields(OperationContext*,
                                                                         UUID) const final {
            MONGO_UNREACHABLE;
        }

        StatusWith<std::unique_ptr<Pipeline, PipelineDeleter>> makePipeline(
            const std::vector<BSONObj>& rawPipeline,
            const boost::intrusive_ptr<ExpressionContext>& expCtx,
            const MakePipelineOptions pipelineOptions) final {
            MONGO_UNREACHABLE;
        }

    protected:
        BSONObj _reportCurrentOpForClient(OperationContext* opCtx,
                                          Client* client,
                                          CurrentOpTruncateMode truncateOps) const final;

        void _reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                              CurrentOpUserMode userMode,
                                              std::vector<BSONObj>* ops) const final {
            // This implementation is a no-op, since mongoS does not maintain a SessionCatalog or
            // hold stashed locks for idle sessions.
        }
    };

private:
    PipelineS() = delete;  // Should never be instantiated.
};

}  // namespace mongo
