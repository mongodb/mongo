/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/search_helper.h"
#include "mongo/db/query/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/query/search/document_source_search.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/executor/task_executor_cursor.h"

namespace mongo::mongot_cursor {

static constexpr StringData kSearchField = "search"_sd;
static constexpr StringData kCollectionUuidField = "collectionUUID"_sd;
static constexpr StringData kQueryField = "query"_sd;
static constexpr StringData kExplainField = "explain"_sd;
static constexpr StringData kVerbosityField = "verbosity"_sd;
static constexpr StringData kIntermediateField = "intermediate"_sd;
static constexpr StringData kCursorOptionsField = "cursorOptions"_sd;
static constexpr StringData kDocsRequestedField = "docsRequested"_sd;
static constexpr StringData kRequiresSearchSequenceToken = "requiresSearchSequenceToken"_sd;

/**
 * Create the RemoteCommandRequest for the provided command.
 */
executor::RemoteCommandRequest getRemoteCommandRequest(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const BSONObj& cmdObj);

/**
 * Run the given command against mongot and build one cursor object for each cursor returned from
 * mongot.
 */
std::vector<executor::TaskExecutorCursor> establishCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const executor::RemoteCommandRequest& command,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    bool preFetchNextBatch,
    std::function<void(BSONObjBuilder& bob)> augmentGetMore = nullptr,
    std::unique_ptr<PlanYieldPolicyRemoteCursor> yieldPolicy = nullptr);

// Default sort spec is to sort decreasing by search score.
static const BSONObj kSortSpec = BSON("$searchScore" << -1);
static constexpr StringData kSearchSortValuesFieldPrefix = "$searchSortValues."_sd;

/**
 * Run the given search query against mongot and build one cursor object for each
 * cursor returned from mongot.
 */
std::vector<executor::TaskExecutorCursor> establishSearchCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& query,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    boost::optional<long long> docsRequested = boost::none,
    std::function<void(BSONObjBuilder& bob)> augmentGetMore = nullptr,
    const boost::optional<int>& protocolVersion = boost::none,
    bool requiresSearchSequenceToken = false,
    std::unique_ptr<PlanYieldPolicyRemoteCursor> yieldPolicy = nullptr);

/**
 * Gets the explain information by issuing an explain command to mongot and blocking
 * until the response is retrieved. The 'query' argument is the original search query
 * that we are trying to explain, not a full explain command. Throws an exception on failure.
 */
BSONObj getExplainResponse(const ExpressionContext* expCtx,
                           const executor::RemoteCommandRequest& request,
                           executor::TaskExecutor* taskExecutor);

/**
 * Wrapper function for using getExplainResponse function with search commands.
 */
BSONObj getSearchExplainResponse(const ExpressionContext* expCtx,
                                 const BSONObj& query,
                                 executor::TaskExecutor* taskExecutor);

/**
 * Consult mongot to get planning information for sharded search queries.
 */
InternalSearchMongotRemoteSpec planShardedSearch(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx, const BSONObj& searchRequest);

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
        MONGO_unlikely(DocumentSourceSearch::skipSearchStageRemoteSetup())) {
        return {make_intrusive<TargetSearchDocumentSource>(std::move(specObj), expCtx, executor)};
    }

    // Send a planShardedSearch command to mongot to get the relevant planning information,
    // including the metadata merging pipeline and the optional merge sort spec.
    auto params = planShardedSearch(expCtx, specObj);

    return {make_intrusive<TargetSearchDocumentSource>(std::move(params), expCtx, executor)};
}

/**
 * Helper function that determines whether the document source references the $$SEARCH_META
 * variable.
 */
bool hasReferenceToSearchMeta(const DocumentSource& ds);

/**
 * Helper function to throw if mongot host is not properly configured, only if the command
 * is expected to execute queries.
 */
void throwIfNotRunningWithMongotHostConfigured(
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * A class that contains methods that are implemented as stubs in community that need to be
 * overridden.
 * TODO SERVER-83293 Clean up this class alongside SearchHelpers
 */
class SearchImplementedHelperFunctions : public SearchDefaultHelperFunctions {
public:
    void assertSearchMetaAccessValid(const Pipeline::SourceContainer& pipeline,
                                     ExpressionContext* expCtx) override final;
    void assertSearchMetaAccessValid(const Pipeline::SourceContainer& shardsPipeline,
                                     const Pipeline::SourceContainer& mergePipeline,
                                     ExpressionContext* expCtx) override final;
    void prepareSearchForTopLevelPipeline(Pipeline* pipeline) override final;
    void prepareSearchForNestedPipeline(Pipeline* pipeline) override final;
    std::unique_ptr<Pipeline, PipelineDeleter> generateMetadataPipelineForSearch(
        OperationContext* opCtx,
        boost::intrusive_ptr<ExpressionContext> expCtx,
        const AggregateCommandRequest& request,
        Pipeline* origPipeline,
        boost::optional<UUID> uuid) override final;
    bool isSearchPipeline(const Pipeline* pipeline) override final;
    bool isSearchMetaPipeline(const Pipeline* pipeline) override final;

    bool isSearchStage(DocumentSource* stage) override final;
    bool isSearchMetaStage(DocumentSource* stage) override final;

    std::unique_ptr<SearchNode> getSearchNode(DocumentSource* stage) override final;
    void establishSearchQueryCursors(boost::intrusive_ptr<ExpressionContext> expCtx,
                                     DocumentSource* stage,
                                     std::unique_ptr<PlanYieldPolicyRemoteCursor>) override final;

    bool encodeSearchForSbeCache(const ExpressionContext* expCtx,
                                 DocumentSource* ds,
                                 BufBuilder* bufBuilder) override final;

    void establishSearchMetaCursor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   DocumentSource* stage,
                                   std::unique_ptr<PlanYieldPolicyRemoteCursor>) override final;

    boost::optional<executor::TaskExecutorCursor> getSearchMetadataCursor(
        DocumentSource* ds) override final;

    std::function<void(BSONObjBuilder& bob)> buildSearchGetMoreFunc(
        std::function<boost::optional<long long>()> calcDocsNeeded) override final;

    std::unique_ptr<RemoteCursorMap> getSearchRemoteCursors(
        const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline) override final;

    std::unique_ptr<RemoteExplainVector> getSearchRemoteExplains(
        const ExpressionContext* expCtx,
        const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline) override final;
};

}  // namespace mongo::mongot_cursor
