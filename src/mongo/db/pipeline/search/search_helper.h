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
#include "mongo/db/pipeline/visitors/docs_needed_bounds_gen.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/decorable.h"
#include "mongo/util/uuid.h"

namespace mongo {

using RemoteCursorMap = absl::flat_hash_map<size_t, std::unique_ptr<executor::TaskExecutorCursor>>;
using RemoteExplainVector = std::vector<BSONObj>;

extern FailPoint searchReturnEofImmediately;
namespace search_helpers {
/**
 * Consult mongot to get planning information for sharded search queries, used to configure the
 * metadataMergeProtocolVersion, metaPipeline, and sortSpec fields in the existing mongot remote
 * spec.
 */
void planShardedSearch(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                       InternalSearchMongotRemoteSpec* remoteSpec);

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

void setResolvedNamespaceForSearch(const NamespaceString& origNss,
                                   const ResolvedView& resolvedView,
                                   boost::intrusive_ptr<ExpressionContext> expCtx,
                                   boost::optional<UUID> uuid = boost::none);
/**
 * Check if this is a search-related pipeline, specifically that the front of the pipeline is a
 * stage that will rely on calls to mongot.
 */
bool isMongotPipeline(const Pipeline* pipeline);

/**
 * Check if this is a $search stage.
 */
bool isSearchStage(DocumentSource* stage);

/**
 * Check if this is a $searchMeta stage.
 */
bool isSearchMetaStage(DocumentSource* stage);

/**
 * Check if this is a search-related stage that will rely on calls to mongot.
 */
bool isMongotStage(DocumentSource* stage);

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
 * method should only be invoked for the DocumentSource-based implementation (legacy executor).
 * The preparation works includes:
 * 1. Desugars $search stage into $_internalSearchMongotRemote and $_internalSearchIdLookup
 * stages.
 * 2. Injects shard filterer for $_internalSearchIdLookup stage on shard only.
 * 3. Checks to see if in the current environment an additional pipeline needs to be run to generate
 * metadata results.
 * 4. Establishes search cursors with mongot and attaches them to their relevant search and metadata
 * stages.
 *
 * This can modify the passed in pipeline but does not take ownership of it.
 *
 * Returns the additional pipline used for metadata, or nullptr if no pipeline is necessary.
 */
std::unique_ptr<Pipeline, PipelineDeleter> prepareSearchForTopLevelPipelineLegacyExecutor(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    Pipeline* origPipeline,
    DocsNeededBounds bounds,
    boost::optional<int64_t> userBatchSize);

/**
 * This method works on preparation for $search in nested pipeline, e.g. sub-pipeline of
 * $lookup, for local read. Nothing is done if first stage in the pipeline is not $search, and
 * this method should only be invoked for the DocumentSource-based implementation (legacy executor).
 * The preparation works desugars $search stage into $_internalSearchMongotRemote and
 * $_internalSearchIdLookup stages.
 */
void prepareSearchForNestedPipelineLegacyExecutor(Pipeline* pipeline);

/**
 * Establishes the cursor for $search queries run in SBE.
 */
void establishSearchCursorsSBE(boost::intrusive_ptr<ExpressionContext> expCtx,
                               DocumentSource* stage,
                               std::unique_ptr<PlanYieldPolicy>);

/**
 * Encode $search/$searchMeta to SBE plan cache.
 * Returns true if $search/$searchMeta is at the front of the 'pipeline' and encoding is done.
 */
bool encodeSearchForSbeCache(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             DocumentSource* ds,
                             BufBuilder* bufBuilder);
/**
 * Establishes the metadata cursor for $search queries run in SBE.
 */
void establishSearchMetaCursorSBE(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  DocumentSource* stage,
                                  std::unique_ptr<PlanYieldPolicy>);

std::unique_ptr<executor::TaskExecutorCursor> getSearchMetadataCursor(DocumentSource* ds);

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
    InternalSearchMongotRemoteSpec remoteSpec(specObj.getOwned());
    planShardedSearch(expCtx, &remoteSpec);

    return {make_intrusive<TargetSearchDocumentSource>(std::move(remoteSpec), expCtx, executor)};
}
}  // namespace search_helpers
}  // namespace mongo
