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
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/decorable.h"
#include "mongo/util/uuid.h"

namespace mongo {
static constexpr auto kReturnStoredSourceArg = "returnStoredSource"_sd;

/**
 * A class that contains any functions needed to run $seach queries when the enterprise module
 * is compiled in. The enterprise module will override these functions, these are just stubs.
 */
class SearchDefaultHelperFunctions {
public:
    virtual ~SearchDefaultHelperFunctions() {}

    /**
     * Any access of $$SEARCH_META is invalid without enterprise.
     */
    virtual void assertSearchMetaAccessValid(const Pipeline::SourceContainer& pipeline,
                                             ExpressionContext* expCtx);
    /**
     * Overload used to check that $$SEARCH_META is being referenced correctly in a pipeline split
     * for execution on a sharded cluster.
     */
    virtual void assertSearchMetaAccessValid(const Pipeline::SourceContainer& shardsPipeline,
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
     */
    virtual void prepareSearchForTopLevelPipeline(Pipeline* pipeline) {}

    /**
     * This method works on preparation for $search in nested pipeline, e.g. sub-pipeline of
     * $lookup, for local read. Nothing is done if first stage in the pipeline is not $search, and
     * this method should only be invoked for the DocumentSource-based implementation.
     * The preparation works includes:
     * 1. Desugars $search stage into $_internalSearchMongotRemote and $_internalSearchIdLookup
     * stages.
     */
    virtual void prepareSearchForNestedPipeline(Pipeline* pipeline) {}

    /**
     * Check to see if in the current environment an additional pipeline needs to be run by the
     * aggregation command to generate metadata results. Either returns the additional pipeline
     * or nullptr if no pipeline is necessary.
     *
     * This can modify the passed in pipeline but does not take ownership of it.
     */
    virtual std::unique_ptr<Pipeline, PipelineDeleter> generateMetadataPipelineForSearch(
        OperationContext* opCtx,
        boost::intrusive_ptr<ExpressionContext> expCtx,
        const AggregateCommandRequest& request,
        Pipeline* origPipeline,
        boost::optional<UUID> uuid) {
        return nullptr;
    }

    /**
     * Check if this is a $search pipeline, specifically that the front of the pipeline is
     * a $search stage.
     */
    virtual bool isSearchPipeline(const Pipeline* pipeline) {
        return false;
    }

    /**
     * Check if this is a $searchMeta pipeline, specifically that the front of the pipeline is
     * a $searchMeta stage.
     */
    virtual bool isSearchMetaPipeline(const Pipeline* pipeline) {
        return false;
    }

    /**
     * Establish a cursor given the search query and CursorResponse from the initial execution.
     */
    virtual boost::optional<executor::TaskExecutorCursor> establishSearchCursor(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const boost::optional<UUID>& uuid,
        const boost::optional<ExplainOptions::Verbosity>& explain,
        const BSONObj& query,
        CursorResponse&& response,
        boost::optional<long long> docsRequested = boost::none,
        std::function<boost::optional<long long>()> calcDocsNeeded = nullptr,
        const boost::optional<int>& protocolVersion = boost::none) {
        return boost::none;
    }

    /**
     * Check if this is a $searchMeta stage.
     */
    virtual bool isSearchStage(DocumentSource* stage) {
        return false;
    }

    /**
     * Check if this is a $searchMeta stage.
     */
    virtual bool isSearchMetaStage(DocumentSource* stage) {
        return false;
    }

    /**
     * Gets the information for the search QSN from DocumentSourceSearch.
     * The results are returned as a tuple of the format:
     * <limit, mongotDocsRequested, searchQuery, taskExecutor, intermediateResultsProtocolVersion>
     */
    virtual std::unique_ptr<SearchNode> getSearchNode(DocumentSource* stage) {
        return nullptr;
    }

    /**
     * Executes the initial $search query to get the cursor id and first batch for building the
     * $search SBE plan.
     */
    virtual std::pair<CursorResponse, CursorResponse> establishSearchQueryCursors(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const SearchNode* searchNode) {
        return {CursorResponse(), CursorResponse()};
    }

    /**
     * Encode $search/$searchMeta to SBE plan cache.
     * Returns true if $search/$searchMeta is at the front of the 'pipeline' and encoding is done.
     */
    virtual bool encodeSearchForSbeCache(DocumentSource* ds, BufBuilder* bufBuilder) {
        return false;
    }
};

/**
 * A 'ServiceContext' decorator that allows enterprise to set its own version of the above class.
 */
extern ServiceContext::Decoration<std::unique_ptr<SearchDefaultHelperFunctions>> getSearchHelpers;

extern FailPoint searchReturnEofImmediately;

}  // namespace mongo
