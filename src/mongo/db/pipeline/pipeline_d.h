/**
 * Copyright (C) 2012-2014 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/mongo_process_common.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {
class Collection;
class DocumentSourceCursor;
class DocumentSourceMatch;
class DocumentSourceSort;
class ExpressionContext;
class OperationContext;
class Pipeline;
struct PlanSummaryStats;
class BSONObj;
struct DepsTracker;

/*
  PipelineD is an extension of the Pipeline class, but with additional
  material that references symbols that are not available in mongos,
  where the remainder of the Pipeline class also functions.  PipelineD
  is a friend of Pipeline so that it can have equal access to Pipeline's
  members.

  See the friend declaration in Pipeline.
 */
class PipelineD {
public:
    class MongoDInterface final : public MongoProcessCommon {
    public:
        MongoDInterface(OperationContext* opCtx);

        void setOperationContext(OperationContext* opCtx) final;
        DBClientBase* directClient() final;
        bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final;
        BSONObj insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const NamespaceString& ns,
                       const std::vector<BSONObj>& objs) final;
        CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                              const NamespaceString& ns) final;
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
        Status renameIfOptionsAndIndexesHaveNotChanged(
            OperationContext* opCtx,
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
        std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFields(OperationContext* opCtx,
                                                                         UUID uuid) const final;
        boost::optional<Document> lookupSingleDocument(
            const boost::intrusive_ptr<ExpressionContext>& expCtx,
            const NamespaceString& nss,
            UUID collectionUUID,
            const Document& documentKey,
            boost::optional<BSONObj> readConcern) final;
        std::vector<GenericCursor> getCursors(
            const boost::intrusive_ptr<ExpressionContext>& expCtx) const final;

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
     * If the first stage in the pipeline does not generate its own output documents, attaches a
     * cursor document source to the front of the pipeline which will output documents from the
     * collection to feed into the pipeline.
     *
     * This method looks for early pipeline stages that can be folded into the underlying
     * PlanExecutor, and removes those stages from the pipeline when they can be absorbed by the
     * PlanExecutor. For example, an early $match can be removed and replaced with a
     * DocumentSourceCursor containing a PlanExecutor that will do an index scan.
     *
     * Callers must take care to ensure that 'nss' is locked in at least IS-mode.
     *
     * When not null, 'aggRequest' provides access to pipeline command options such as hint.
     */
    static void prepareCursorSource(Collection* collection,
                                    const NamespaceString& nss,
                                    const AggregationRequest* aggRequest,
                                    Pipeline* pipeline);

    /**
     * Prepare a generic DocumentSourceCursor for 'pipeline'.
     */
    static void prepareGenericCursorSource(Collection* collection,
                                           const NamespaceString& nss,
                                           const AggregationRequest* aggRequest,
                                           Pipeline* pipeline);

    /**
     * Prepare a special DocumentSourceGeoNearCursor for 'pipeline'. Unlike
     * 'prepareGenericCursorSource()', throws if 'collection' does not exist, as the $geoNearCursor
     * requires a 2d or 2dsphere index.
     */
    static void prepareGeoNearCursorSource(Collection* collection,
                                           const NamespaceString& nss,
                                           const AggregationRequest* aggRequest,
                                           Pipeline* pipeline);

    /**
     * Injects a MongodInterface into stages which require access to mongod-specific functionality.
     */
    static void injectMongodInterface(Pipeline* pipeline);

    static std::string getPlanSummaryStr(const Pipeline* pipeline);

    static void getPlanSummaryStats(const Pipeline* pipeline, PlanSummaryStats* statsOut);

    static Timestamp getLatestOplogTimestamp(const Pipeline* pipeline);

private:
    PipelineD();  // does not exist:  prevent instantiation

    /**
     * Creates a PlanExecutor to be used in the initial cursor source. If the query system can use
     * an index to provide a more efficient sort or projection, the sort and/or projection will be
     * incorporated into the PlanExecutor.
     *
     * 'sortObj' will be set to an empty object if the query system cannot provide a non-blocking
     * sort, and 'projectionObj' will be set to an empty object if the query system cannot provide a
     * covered projection.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> prepareExecutor(
        OperationContext* opCtx,
        Collection* collection,
        const NamespaceString& nss,
        Pipeline* pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool oplogReplay,
        const boost::intrusive_ptr<DocumentSourceSort>& sortStage,
        const DepsTracker& deps,
        const BSONObj& queryObj,
        const AggregationRequest* aggRequest,
        const MatchExpressionParser::AllowedFeatureSet& matcherFeatures,
        BSONObj* sortObj,
        BSONObj* projectionObj);

    /**
     * Adds 'cursor' to the front of 'pipeline', using 'deps' to inform the cursor of its
     * dependencies. If specified, 'queryObj', 'sortObj' and 'projectionObj' are passed to the
     * cursor for explain reporting.
     */
    static void addCursorSource(Pipeline* pipeline,
                                boost::intrusive_ptr<DocumentSourceCursor> cursor,
                                DepsTracker deps,
                                const BSONObj& queryObj = BSONObj(),
                                const BSONObj& sortObj = BSONObj(),
                                const BSONObj& projectionObj = BSONObj());
};

}  // namespace mongo
