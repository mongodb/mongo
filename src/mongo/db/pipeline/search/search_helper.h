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

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds_gen.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/decorable.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using RemoteCursorMap = absl::flat_hash_map<size_t, std::unique_ptr<executor::TaskExecutorCursor>>;
using RemoteExplainVector = std::vector<BSONObj>;

extern FailPoint searchReturnEofImmediately;
namespace search_helpers {

static constexpr StringData kViewFieldName = "view"_sd;
static constexpr StringData kProtocolStoredFieldsName = "storedSource"_sd;

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

/**
 * Checks that the *user* pipeline contains a search stage and sets the view on expCtx.
 */
void checkAndSetViewOnExpCtx(boost::intrusive_ptr<ExpressionContext> expCtx,
                             std::vector<mongo::BSONObj> pipelineObj,
                             ResolvedView resolvedView,
                             const NamespaceString& viewName);

/**
 * Check if this is a stored source $search or $_internalSearchMongot pipeline.
 */
bool isStoredSource(const Pipeline* pipeline);

/**
 * Check if this is a search-related pipeline, specifically that the front of the pipeline is a
 * stage that will rely on calls to mongot.
 */
bool isMongotPipeline(const Pipeline* pipeline);

/**
 * Check if this is a $search stage.
 */
bool isSearchStage(const DocumentSource* stage);

/**
 * Check if this is a $searchMeta stage.
 */
bool isSearchMetaStage(const DocumentSource* stage);

/**
 * Check if this is a search-related stage that will rely on calls to mongot.
 */
bool isMongotStage(DocumentSource* stage);

/**
 * Asserts that $$SEARCH_META is accessed correctly; that is, it is set by a prior stage, and is
 * not accessed in a subpipline.
 */
void assertSearchMetaAccessValid(const DocumentSourceContainer& pipeline,
                                 ExpressionContext* expCtx);

/**
 * Overload used to check that $$SEARCH_META is being referenced correctly in a pipeline split
 * for execution on a sharded cluster.
 */
void assertSearchMetaAccessValid(const DocumentSourceContainer& shardsPipeline,
                                 const DocumentSourceContainer& mergePipeline,
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
std::unique_ptr<Pipeline> prepareSearchForTopLevelPipelineLegacyExecutor(
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

boost::optional<SearchQueryViewSpec> getViewFromExpCtx(
    boost::intrusive_ptr<ExpressionContext> expCtx);

boost::optional<SearchQueryViewSpec> getViewFromBSONObj(const BSONObj& spec);

void validateViewNotSetByUser(boost::intrusive_ptr<ExpressionContext> expCtx, const BSONObj& spec);

/**
 * Validates that search stages on views are only allowed when the respective feature flag
 * is enabled.
 */
void validateMongotIndexedViewsFF(boost::intrusive_ptr<ExpressionContext> expCtx,
                                  const std::vector<BSONObj>& effectivePipeline);

/**
 * This function promotes the fields in storedSource to root if applicable, otherwise adds an
 * internalSearchMongotRemote stage to the desugared pipeline.
 */
void promoteStoredSourceOrAddIdLookup(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::list<boost::intrusive_ptr<DocumentSource>>& desugaredPipeline,
    bool isStoredSource,
    long long limit,
    boost::optional<SearchQueryViewSpec> view);

/**
 * Creates a Search QSN from the given DocumentSource stage, if isSearchStage(stage) or
 * isSearchMetaStage(stage) return 'true'. Else, an error is thrown.
 */
std::unique_ptr<SearchNode> getSearchNode(DocumentSource* stage);
}  // namespace search_helpers
}  // namespace mongo
