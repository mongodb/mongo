/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote_gen.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/decorable.h"
#include "mongo/util/uuid.h"

namespace mongo {
static constexpr auto kReturnStoredSourceArg = "returnStoredSource"_sd;
static constexpr auto kSlowQueryLogFieldName = "slowQueryLog"_sd;

using RemoteCursorMap = absl::flat_hash_map<size_t, std::unique_ptr<executor::TaskExecutorCursor>>;
using RemoteExplainVector = std::vector<BSONObj>;

extern FailPoint searchReturnEofImmediately;
namespace search_helpers {
/**
 * Consult mongot to get planning information for sharded search queries.
 */
InternalSearchMongotRemoteSpec planShardedSearch(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx, const BSONObj& searchRequest);

/**
 * Helper function that determines whether the document source references the $$SEARCH_META
 * variable.
 */
bool hasReferenceToSearchMeta(const DocumentSource& ds);

/**
 * Check if this is a $search pipeline, specifically that the front of the pipeline is
 * a $search stage.
 */
bool isSearchPipeline(const Pipeline* pipeline);

/**
 * Check if this is a $searchMeta pipeline, specifically that the front of the pipeline is
 * a $searchMeta stage.
 */
bool isSearchMetaPipeline(const Pipeline* pipeline);

/**
 * Check if this is a $search stage.
 */
bool isSearchStage(DocumentSource* stage);

/**
 * Check if this is a $searchMeta stage.
 */
bool isSearchMetaStage(DocumentSource* stage);

/**
 * Asserts that $$SEARCH_META is accessed correctly; that is, it is set by a prior stage, and is
 * not accessed in a subpipline.
 */
void assertSearchMetaAccessValid(const Pipeline::SourceContainer& pipeline,
                                 ExpressionContext* expCtx);

/**
 * Overload used to check that $$SEARCH_META is being referenced correctly in a pipeline split
 * for execution on a sharded cluster.
 */
void assertSearchMetaAccessValid(const Pipeline::SourceContainer& shardsPipeline,
                                 const Pipeline::SourceContainer& mergePipeline,
                                 ExpressionContext* expCtx);

/**
 * This method works on preparation for $search in top level pipeline, or inner pipeline that is
 * dispatched to shards. Nothing is done if first stage in the pipeline is not $search, and this
 * method should only be invoked for the DocumentSource-based implementation.
 * The preparation works includes:
 * 1. Desugars $search stage into $_internalSearchMongotRemote and $_internalSearchIdLookup
 * stages.
 * 2. injects shard filterer for $_internalSearchIdLookup stage on shard only.
 *
 * This function is only called for preparing the pipeline for execution in the classic engine,
 * since $search in SBE is not desugared.
 */
void prepareSearchForTopLevelPipeline(Pipeline* pipeline);

/**
 * This method works on preparation for $search in nested pipeline, e.g. sub-pipeline of
 * $lookup, for local read. Nothing is done if first stage in the pipeline is not $search, and
 * this method should only be invoked for the DocumentSource-based implementation.
 * The preparation works desugars $search stage into $_internalSearchMongotRemote and
 * $_internalSearchIdLookup stages.
 *
 * This function is only called for preparing the pipeline for execution in the classic engine,
 * since $search in SBE is not desugared.
 */
void prepareSearchForNestedPipeline(Pipeline* pipeline);

/**
 * Check to see if in the current environment an additional pipeline needs to be run by the
 * aggregation command to generate metadata results. Either returns the additional pipeline
 * or nullptr if no pipeline is necessary.
 *
 * Also retrieves search cursors from mongot and attaches them to their relevant search and metadata
 * stages.
 *
 * This can modify the passed in pipeline but does not take ownership of it.
 */
std::unique_ptr<Pipeline, PipelineDeleter> generateMetadataPipelineAndAttachCursorsForSearch(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const AggregateCommandRequest& request,
    Pipeline* origPipeline,
    boost::optional<UUID> uuid);


/**
 * Executes the cursor for $search query.
 */
void establishSearchQueryCursors(boost::intrusive_ptr<ExpressionContext> expCtx,
                                 DocumentSource* stage,
                                 std::unique_ptr<PlanYieldPolicy>);

/**
 * Encode $search/$searchMeta to SBE plan cache.
 * Returns true if $search/$searchMeta is at the front of the 'pipeline' and encoding is done.
 */
bool encodeSearchForSbeCache(const ExpressionContext* expCtx,
                             DocumentSource* ds,
                             BufBuilder* bufBuilder);
/**
 * Executes the metadata cursor for $search query.
 */
void establishSearchMetaCursor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               DocumentSource* stage,
                               std::unique_ptr<PlanYieldPolicy>);

boost::optional<executor::TaskExecutorCursor> getSearchMetadataCursor(DocumentSource* ds);

std::function<void(BSONObjBuilder& bob)> buildSearchGetMoreFunc(
    std::function<boost::optional<long long>()> calcDocsNeeded);

std::unique_ptr<RemoteCursorMap> getSearchRemoteCursors(
    const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline);

std::unique_ptr<RemoteExplainVector> getSearchRemoteExplains(
    const ExpressionContext* expCtx,
    const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline);

/**
 * Create the initial search pipeline which can be used for both $search and $searchMeta. The
 * returned list is unique and mutable.
 */
template <typename TargetSearchDocumentSource>
std::list<boost::intrusive_ptr<DocumentSource>> createInitialSearchPipeline(
    BSONObj specObj, const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    uassert(6600901,
            "Running search command in non-allowed context (update pipeline)",
            !expCtx->isParsingPipelineUpdate);

    // This is only called from user pipelines during desugaring of $search/$searchMeta, so the
    // `specObj` should be the search query itself.
    auto executor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
    if ((!expCtx->mongoProcessInterface->isExpectedToExecuteQueries() ||
         !expCtx->mongoProcessInterface->inShardedEnvironment(expCtx->opCtx)) ||
        MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return {make_intrusive<TargetSearchDocumentSource>(std::move(specObj), expCtx, executor)};
    }

    // Send a planShardedSearch command to mongot to get the relevant planning information,
    // including the metadata merging pipeline and the optional merge sort spec.
    auto params = planShardedSearch(expCtx, specObj);

    return {make_intrusive<TargetSearchDocumentSource>(std::move(params), expCtx, executor)};
}
}  // namespace search_helpers
}  // namespace mongo
