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

#include <boost/intrusive_ptr.hpp>
#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_group.h"
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

/**
 * PipelineD is an extension of the Pipeline class, but with additional material that references
 * symbols that are not available in mongos, where the remainder of the Pipeline class also
 * functions.  PipelineD is a friend of Pipeline so that it can have equal access to Pipeline's
 * members.
 *
 * See the friend declaration in Pipeline.
 */
class PipelineD {
public:
    /**
     * This callback function is called to attach a query PlanExecutor to the given Pipeline by
     * creating a specific DocumentSourceCursor stage using the provided PlanExecutor, and adding
     * the new stage to the pipeline.
     */
    using AttachExecutorCallback = std::function<void(
        Collection*, std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>, Pipeline*)>;

    /**
     * This method looks for early pipeline stages that can be folded into the underlying
     * PlanExecutor, and removes those stages from the pipeline when they can be absorbed by the
     * PlanExecutor. For example, an early $match can be removed and replaced with a
     * DocumentSourceCursor containing a PlanExecutor that will do an index scan.
     *
     * Callers must take care to ensure that 'nss' is locked in at least IS-mode.
     *
     * When not null, 'aggRequest' provides access to pipeline command options such as hint.
     *
     * The 'collection' parameter is optional and can be passed as 'nullptr'.
     *
     * This method will not add a $cursor stage to the pipeline, but will create a PlanExecutor and
     * a callback function. The executor and the callback can later be used to create the $cursor
     * stage and add it to the pipeline by calling 'attachInnerQueryExecutorToPipeline()' method.
     * If the pipeline doesn't require a $cursor stage, the plan executor will be returned as
     * 'nullptr'.
     */
    static std::pair<AttachExecutorCallback, std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
    buildInnerQueryExecutor(Collection* collection,
                            const NamespaceString& nss,
                            const AggregationRequest* aggRequest,
                            Pipeline* pipeline);

    /**
     * Completes creation of the $cursor stage using the given callback pair obtained by calling
     * 'buildInnerQueryExecutor()' method. If the callback doesn't hold a valid PlanExecutor, the
     * method does nothing. Otherwise, a new $cursor stage is created using the given PlanExecutor,
     * and added to the pipeline. The 'collection' parameter is optional and can be passed as
     * 'nullptr'.
     */
    static void attachInnerQueryExecutorToPipeline(
        Collection* collection,
        AttachExecutorCallback attachExecutorCallback,
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
        Pipeline* pipeline);

    /**
     * This method combines 'buildInnerQueryExecutor()' and 'attachInnerQueryExecutorToPipeline()'
     * into a single call to support auto completion of the cursor stage creation process. Can be
     * used when the executor attachment phase doesn't need to be deferred and the $cursor stage
     * can be created right after buiding the executor.
     */
    static void buildAndAttachInnerQueryExecutorToPipeline(Collection* collection,
                                                           const NamespaceString& nss,
                                                           const AggregationRequest* aggRequest,
                                                           Pipeline* pipeline);


    static std::string getPlanSummaryStr(const Pipeline* pipeline);

    static void getPlanSummaryStats(const Pipeline* pipeline, PlanSummaryStats* statsOut);

    static Timestamp getLatestOplogTimestamp(const Pipeline* pipeline);

private:
    PipelineD();  // does not exist:  prevent instantiation

    /**
     * Build a PlanExecutor and prepare callback to create a generic DocumentSourceCursor for
     * the 'pipeline'.
     */
    static std::pair<AttachExecutorCallback, std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
    buildInnerQueryExecutorGeneric(Collection* collection,
                                   const NamespaceString& nss,
                                   const AggregationRequest* aggRequest,
                                   Pipeline* pipeline);

    /**
     * Build a PlanExecutor and prepare a callback to create a special DocumentSourceGeoNearCursor
     * for the 'pipeline'. Unlike 'buildInnerQueryExecutorGeneric()', throws if 'collection' does
     * not exist, as the $geoNearCursor requires a 2d or 2dsphere index.
     */
    static std::pair<AttachExecutorCallback, std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
    buildInnerQueryExecutorGeoNear(Collection* collection,
                                   const NamespaceString& nss,
                                   const AggregationRequest* aggRequest,
                                   Pipeline* pipeline);

    /**
     * Creates a PlanExecutor to be used in the initial cursor source. If the query system can use
     * an index to provide a more efficient sort or projection, the sort and/or projection will be
     * incorporated into the PlanExecutor.
     *
     * 'sortObj' will be set to an empty object if the query system cannot provide a non-blocking
     * sort, and 'projectionObj' will be set to an empty object if the query system cannot provide a
     * covered projection.
     *
     * Set 'rewrittenGroupStage' when the pipeline uses $match+$sort+$group stages that are
     * compatible with a DISTINCT_SCAN plan that visits the first document in each group
     * (SERVER-9507).
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> prepareExecutor(
        OperationContext* opCtx,
        Collection* collection,
        const NamespaceString& nss,
        Pipeline* pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool oplogReplay,
        const boost::intrusive_ptr<DocumentSourceSort>& sortStage,
        std::unique_ptr<GroupFromFirstDocumentTransformation> rewrittenGroupStage,
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
