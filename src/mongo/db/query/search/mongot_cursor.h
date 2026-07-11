// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] mongot_cursor {
using namespace std::literals::string_view_literals;
inline auto makeRetryOnNetworkErrorPolicy() {
    return [retried = false](const Status& st) mutable {
        return std::exchange(retried, true) ? false : ErrorCodes::isNetworkError(st);
    };
}

static constexpr std::string_view kSearchField = "search"sv;
static constexpr std::string_view kVectorSearchCmd = "vectorSearch"sv;
static constexpr std::string_view kCollectionUuidField = "collectionUUID"sv;
static constexpr std::string_view kQueryField = "query"sv;
static constexpr std::string_view kViewNameField = "viewName"sv;
static constexpr std::string_view kExplainField = "explain"sv;
static constexpr std::string_view kVerbosityField = "verbosity"sv;
static constexpr std::string_view kIntermediateField = "intermediate"sv;
static constexpr std::string_view kCursorOptionsField = "cursorOptions"sv;
static constexpr std::string_view kDocsRequestedField = "docsRequested"sv;
static constexpr std::string_view kBatchSizeField = "batchSize"sv;
static constexpr std::string_view kRequiresSearchSequenceToken = "requiresSearchSequenceToken"sv;
static constexpr std::string_view kReturnStoredSourceArg = "returnStoredSource"sv;
static constexpr std::string_view kReturnScopeArg = "returnScope"sv;
static constexpr std::string_view kSlowQueryLogFieldName = "slowQueryLog"sv;
static constexpr std::string_view kScoreDetailsFieldName = "scoreDetails"sv;
static constexpr std::string_view kSearchRootDocumentIdFieldName = "searchRootDocumentId"sv;
static constexpr std::string_view kOptimizationFlagsField = "optimizationFlags"sv;
static constexpr std::string_view kOmitSearchDocumentResultsField = "omitSearchDocumentResults"sv;

static constexpr long long kMinimumMongotBatchSize = 10;
static constexpr long long kDefaultMongotBatchSize = 101;

// Default sort spec is to sort decreasing by search score.
static const BSONObj kSortSpec = BSON("$searchScore" << -1);
static constexpr std::string_view kSearchSortValuesFieldPrefix = "$searchSortValues."sv;

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
}  // namespace mongot_cursor
}  // namespace mongo
