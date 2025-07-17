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

#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/executor/task_executor_cursor.h"

namespace mongo::mongot_cursor {
inline auto makeRetryOnNetworkErrorPolicy() {
    return [retried = false](const Status& st) mutable {
        return std::exchange(retried, true) ? false : ErrorCodes::isNetworkError(st);
    };
}

static constexpr StringData kSearchField = "search"_sd;
static constexpr StringData kVectorSearchCmd = "vectorSearch"_sd;
static constexpr StringData kCollectionUuidField = "collectionUUID"_sd;
static constexpr StringData kQueryField = "query"_sd;
static constexpr StringData kViewNameField = "viewName"_sd;
static constexpr StringData kExplainField = "explain"_sd;
static constexpr StringData kVerbosityField = "verbosity"_sd;
static constexpr StringData kIntermediateField = "intermediate"_sd;
static constexpr StringData kCursorOptionsField = "cursorOptions"_sd;
static constexpr StringData kDocsRequestedField = "docsRequested"_sd;
static constexpr StringData kBatchSizeField = "batchSize"_sd;
static constexpr StringData kRequiresSearchSequenceToken = "requiresSearchSequenceToken"_sd;
static constexpr StringData kReturnStoredSourceArg = "returnStoredSource"_sd;
static constexpr StringData kReturnScopeArg = "returnScope"_sd;
static constexpr StringData kSlowQueryLogFieldName = "slowQueryLog"_sd;
static constexpr StringData kScoreDetailsFieldName = "scoreDetails"_sd;
static constexpr StringData kSearchRootDocumentIdFieldName = "searchRootDocumentId"_sd;
static constexpr StringData kOptimizationFlagsField = "optimizationFlags"_sd;
static constexpr StringData kOmitSearchDocumentResultsField = "omitSearchDocumentResults"_sd;

static constexpr long long kMinimumMongotBatchSize = 10;
static constexpr long long kDefaultMongotBatchSize = 101;

// Default sort spec is to sort decreasing by search score.
static const BSONObj kSortSpec = BSON("$searchScore" << -1);
static constexpr StringData kSearchSortValuesFieldPrefix = "$searchSortValues."_sd;

/**
 * Set of OptimizationFlags that can be passed in a mongot search request
 */
struct OptimizationFlags {
    // When true, indicates that mongod is 100% sure that mongot can omit values from the response.
    // Used primarily for $searchMeta.
    bool omitSearchDocumentResults = false;

    BSONObj serialize() const {
        BSONObjBuilder ofBob;
        ofBob.append(kOmitSearchDocumentResultsField, omitSearchDocumentResults);
        return ofBob.obj();
    }
};

/**
 * Gets optimization flags for $searchMeta commands.
 */
OptimizationFlags getOptimizationFlagsForSearchMeta();

/**
 * Gets optimization flags for $search commands.
 */
OptimizationFlags getOptimizationFlagsForSearch();

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
std::vector<std::unique_ptr<executor::TaskExecutorCursor>> establishCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const executor::RemoteCommandRequest& command,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    std::unique_ptr<executor::TaskExecutorCursorGetMoreStrategy> getMoreStrategy,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy = nullptr);

/**
 * Run the given search query against mongot and build one cursor object for each
 * cursor returned from mongot.
 * TODO SERVER-86733 Bounds should not be optional once batchSize tuning is enabled for SBE.
 */
std::vector<std::unique_ptr<executor::TaskExecutorCursor>> establishCursorsForSearchStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const InternalSearchMongotRemoteSpec& spec,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    boost::optional<int64_t> userBatchSize = boost::none,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy = nullptr,
    std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
        searchIdLookupMetrics = nullptr);

/**
 * Parallel to establishCursorsForSearchStage() but limited to the arguments expected for
 * $searchMeta.
 */
std::vector<std::unique_ptr<executor::TaskExecutorCursor>> establishCursorsForSearchMetaStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& query,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    const boost::optional<int>& protocolVersion = boost::none,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy = nullptr,
    boost::optional<SearchQueryViewSpec> view = boost::none);

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
                                 executor::TaskExecutor* taskExecutor,
                                 const OptimizationFlags& optimizationFlags,
                                 boost::optional<SearchQueryViewSpec> view = boost::none);

/**
 * Send the search command `cmdObj` to the remote search server this process is connected to.
 * Retry the command on failure whenever the retryPolicy argument indicates we should; the policy
 * accepts a Status encoding the error the command failed with (local or remote) and returns a
 * bool that is `true` when we should retry. The default is to retry once on network errors.
 *
 * Returns the RemoteCommandResponse we received from the remote. If we fail to get an OK
 * response from the remote after all retry attempts conclude, we throw the error the most
 * recent attempt failed with.
 */
executor::RemoteCommandResponse runSearchCommandWithRetries(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& cmdObj,
    std::function<bool(Status)> retryPolicy = makeRetryOnNetworkErrorPolicy());

/**
 * Helper function to throw if mongot host is not properly configured, only if the command
 * is expected to execute queries.
 */
void throwIfNotRunningWithMongotHostConfigured(
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Helper function to determine whether or not pinned connection mode should be used.
 *
 * Mongod=>Mongot communication over the MongoDB gRPC protocol requires that all
 * getMore/killCursor commands must be sent over the same stream as the command that
 * initiated the cursor(s), which is currently implemented through the pinConnection functionality
 * in TaskExecutorCursor.
 */
bool shouldPinConnection();
}  // namespace mongo::mongot_cursor
