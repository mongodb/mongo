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

#include "mongo/db/pipeline/visitors/docs_needed_bounds.h"
#include "mongo/executor/task_executor_cursor.h"

namespace mongo::mongot_cursor {
namespace {
auto makeRetryOnNetworkErrorPolicy() {
    return [retried = false](const Status& st) mutable {
        return std::exchange(retried, true) ? false : ErrorCodes::isNetworkError(st);
    };
}
}  // namespace

static constexpr StringData kSearchField = "search"_sd;
static constexpr StringData kVectorSearchCmd = "vectorSearch"_sd;
static constexpr StringData kCollectionUuidField = "collectionUUID"_sd;
static constexpr StringData kQueryField = "query"_sd;
static constexpr StringData kExplainField = "explain"_sd;
static constexpr StringData kVerbosityField = "verbosity"_sd;
static constexpr StringData kIntermediateField = "intermediate"_sd;
static constexpr StringData kCursorOptionsField = "cursorOptions"_sd;
static constexpr StringData kDocsRequestedField = "docsRequested"_sd;
static constexpr StringData kBatchSizeField = "batchSize"_sd;
static constexpr StringData kRequiresSearchSequenceToken = "requiresSearchSequenceToken"_sd;
static constexpr StringData kReturnStoredSourceArg = "returnStoredSource"_sd;
static constexpr StringData kSlowQueryLogFieldName = "slowQueryLog"_sd;

static constexpr long long kMinimumMongotBatchSize = 10;
static constexpr long long kDefaultMongotBatchSize = 101;

// Default sort spec is to sort decreasing by search score.
static const BSONObj kSortSpec = BSON("$searchScore" << -1);
static constexpr StringData kSearchSortValuesFieldPrefix = "$searchSortValues."_sd;

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
    bool preFetchNextBatch,
    std::function<boost::optional<long long>()> calcDocsNeededFn = nullptr,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy = nullptr,
    boost::optional<long long> batchSize = boost::none);

/**
 * Run the given search query against mongot and build one cursor object for each
 * cursor returned from mongot.
 * TODO SERVER-87077 This function should accept a InternalSearchMongotRemoteSpec rather than
 * require the fields passed individually.
 * TODO SERVER-86733 Bounds should not be optional once batchSize tuning is enabled for SBE.
 */
std::vector<std::unique_ptr<executor::TaskExecutorCursor>> establishCursorsForSearchStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& query,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    boost::optional<long long> docsRequested = boost::none,
    boost::optional<DocsNeededBounds> minDocsNeededBounds = boost::none,
    boost::optional<DocsNeededBounds> maxDocsNeededBounds = boost::none,
    boost::optional<int64_t> userBatchSize = boost::none,
    std::function<boost::optional<long long>()> calcDocsNeededFn = nullptr,
    const boost::optional<int>& protocolVersion = boost::none,
    bool requiresSearchSequenceToken = false,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy = nullptr);

/**
 * Parallel to establishCursorsForSearchStage() but limited to the arguments expected for
 * $searchMeta.
 */
std::vector<std::unique_ptr<executor::TaskExecutorCursor>> establishCursorsForSearchMetaStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& query,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    const boost::optional<int>& protocolVersion = boost::none,
    std::unique_ptr<PlanYieldPolicy> yieldPolicy = nullptr);

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
}  // namespace mongo::mongot_cursor
