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

#include <ios>
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/db/query/search/mongot_cursor.h"

#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search_helper.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/query/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/query/search/document_source_search.h"
#include "mongo/db/query/search/document_source_search_meta.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/plan_sharded_search_gen.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/query/vector_search/document_source_vector_search.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

namespace mongo::mongot_cursor {
MONGO_FAIL_POINT_DEFINE(shardedSearchOpCtxDisconnect);

namespace {

executor::TaskExecutorCursor::Options getSearchCursorOptions(
    bool preFetchNextBatch,
    std::function<void(BSONObjBuilder& bob)> augmentGetMore,
    std::unique_ptr<PlanYieldPolicyRemoteCursor> yieldPolicy) {
    executor::TaskExecutorCursor::Options opts;
    opts.yieldPolicy = std::move(yieldPolicy);
    // If we are pushing down a limit to mongot, then we should avoid prefetching the next
    // batch. We optimistically assume that we will only need a single batch and attempt to
    // avoid doing unnecessary work on mongot. If $idLookup filters out enough documents
    // such that we are not able to satisfy the limit, then we will fetch the next batch
    // syncronously on the subsequent 'getNext()' call.
    opts.preFetchNextBatch = preFetchNextBatch;
    if (!opts.preFetchNextBatch) {
        // Only set this function if we will not be prefetching.
        opts.getMoreAugmentationWriter = augmentGetMore;
    }
    return opts;
}

executor::RemoteCommandRequest getRemoteCommandRequestForSearchQuery(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& uuid,
    const boost::optional<ExplainOptions::Verbosity>& explain,
    const BSONObj& query,
    const boost::optional<long long> docsRequested,
    const bool requiresSearchSequenceToken = false,
    const boost::optional<int> protocolVersion = boost::none) {
    BSONObjBuilder cmdBob;
    cmdBob.append(kSearchField, nss.coll());
    uassert(
        6584801,
        str::stream() << "A uuid is required for a search query, but was missing. Got namespace "
                      << nss.toStringForErrorMsg(),
        uuid);
    uuid.value().appendToBuilder(&cmdBob, kCollectionUuidField);
    cmdBob.append(kQueryField, query);
    if (explain) {
        cmdBob.append(kExplainField,
                      BSON(kVerbosityField << ExplainOptions::verbosityString(*explain)));
    }
    if (protocolVersion) {
        cmdBob.append(kIntermediateField, *protocolVersion);
    }
    // (Ignore FCV check): This feature is enabled on an earlier FCV.
    const auto needsSetDocsRequested =
        feature_flags::gFeatureFlagSearchBatchSizeLimit.isEnabledAndIgnoreFCVUnsafe() &&
        docsRequested.has_value();
    if (needsSetDocsRequested || requiresSearchSequenceToken) {
        BSONObjBuilder cursorOptionsBob(cmdBob.subobjStart(kCursorOptionsField));
        if (needsSetDocsRequested) {
            cursorOptionsBob.append(kDocsRequestedField, docsRequested.get());
        }
        if (requiresSearchSequenceToken) {
            // Indicate to mongot that the user wants to paginate so mongot returns pagination
            // tokens alongside the _id values.
            cursorOptionsBob.append(kRequiresSearchSequenceToken, true);
        }
        cursorOptionsBob.doneFast();
    }


    return getRemoteCommandRequest(opCtx, nss, cmdBob.obj());
}

auto makeRetryOnNetworkErrorPolicy() {
    return [retried = false](const Status& st) mutable {
        return std::exchange(retried, true) ? false : ErrorCodes::isNetworkError(st);
    };
}

std::pair<boost::optional<executor::TaskExecutorCursor>,
          boost::optional<executor::TaskExecutorCursor>>
parseMongotResponseCursors(std::vector<executor::TaskExecutorCursor> cursors) {
    // mongot can return zero cursors for an empty collection, one without metadata, or two for
    // results and metadata.
    tassert(7856000, "Expected less than or exactly two cursors from mongot", cursors.size() <= 2);

    if (cursors.size() == 1 && !cursors[0].getType()) {
        return {std::move(cursors[0]), boost::none};
    }

    std::pair<boost::optional<executor::TaskExecutorCursor>,
              boost::optional<executor::TaskExecutorCursor>>
        result;

    for (auto it = cursors.begin(); it != cursors.end(); it++) {
        auto cursorLabel = it->getType();
        // If a cursor is unlabeled mongot does not support metadata cursors. $$SEARCH_META
        // should not be supported in this query.
        tassert(7856001, "Expected cursors to be labeled if there are more than one", cursorLabel);

        switch (CursorType_parse(IDLParserContext("ShardedAggHelperCursorType"), *cursorLabel)) {
            case CursorTypeEnum::DocumentResult:
                result.first.emplace(std::move(*it));
                break;
            case CursorTypeEnum::SearchMetaResult:
                result.second.emplace(std::move(*it));
                break;
            default:
                tasserted(7856003,
                          str::stream() << "Unexpected cursor type '" << *cursorLabel << "'");
        }
    }
    return result;
}

BSONObj getSearchRemoteExplain(const ExpressionContext* expCtx,
                               const BSONObj& searchQuery,
                               size_t remoteCursorId,
                               boost::optional<BSONObj> sortSpec) {
    auto executor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
    auto explainObj = getSearchExplainResponse(expCtx, searchQuery, executor.get());
    BSONObjBuilder builder;
    builder << "id" << static_cast<int>(remoteCursorId) << "mongotQuery" << searchQuery << "explain"
            << explainObj;
    if (sortSpec) {
        builder << "sortSpec" << *sortSpec;
    }
    return builder.obj();
}

void doThrowIfNotRunningWithMongotHostConfigured() {
    uassert(
        ErrorCodes::SearchNotEnabled,
        str::stream()
            << "Using $search and $vectorSearch aggregation stages requires additional "
            << "configuration. Please connect to Atlas or an AtlasCLI local deployment to enable."
            << "For more information on how to connect, see "
            << "https://dochub.mongodb.org/core/atlas-cli-deploy-local-reqs.",
        globalMongotParams.enabled);
}
}  // namespace

executor::RemoteCommandRequest getRemoteCommandRequest(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const BSONObj& cmdObj) {
    doThrowIfNotRunningWithMongotHostConfigured();
    auto swHostAndPort = HostAndPort::parse(globalMongotParams.host);
    // This host and port string is configured and validated at startup.
    invariant(swHostAndPort.getStatus().isOK());
    executor::RemoteCommandRequest rcr(
        executor::RemoteCommandRequest(swHostAndPort.getValue(), nss.dbName(), cmdObj, opCtx));
    rcr.sslMode = transport::ConnectSSLMode::kDisableSSL;
    return rcr;
}

std::vector<executor::TaskExecutorCursor> establishCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const executor::RemoteCommandRequest& command,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    bool preFetchNextBatch,
    std::function<void(BSONObjBuilder& bob)> augmentGetMore,
    std::unique_ptr<PlanYieldPolicyRemoteCursor> yieldPolicy) {
    std::vector<executor::TaskExecutorCursor> cursors;
    auto initialCursor = makeTaskExecutorCursor(
        expCtx->opCtx,
        taskExecutor,
        command,
        getSearchCursorOptions(preFetchNextBatch, augmentGetMore, std::move(yieldPolicy)),
        makeRetryOnNetworkErrorPolicy());

    auto additionalCursors = initialCursor.releaseAdditionalCursors();
    cursors.push_back(std::move(initialCursor));
    // Preserve cursor order. Expect cursors to be labeled, so this may not be necessary.
    for (auto& thisCursor : additionalCursors) {
        cursors.push_back(std::move(thisCursor));
    }

    return cursors;
}

bool SearchImplementedHelperFunctions::isSearchPipeline(const Pipeline* pipeline) {
    if (!pipeline || pipeline->getSources().empty()) {
        return false;
    }
    return isSearchStage(pipeline->peekFront());
}

bool SearchImplementedHelperFunctions::isSearchMetaPipeline(const Pipeline* pipeline) {
    if (!pipeline || pipeline->getSources().empty()) {
        return false;
    }
    return isSearchMetaStage(pipeline->peekFront());
}

/** Because 'DocumentSourceSearchMeta' inherits from 'DocumentSourceInternalSearchMongotRemote',
 *  to make sure a DocumentSource is a $search stage and not $searchMeta check it is either:
 *    - a 'DocumentSourceSearch'.
 *    - a 'DocumentSourceInternalSearchMongotRemote' and not a 'DocumentSourceSearchMeta'.
 * TODO: SERVER-78159 refactor after DocumentSourceInternalSearchMongotRemote and
 * DocumentSourceInternalIdLookup are merged into into DocumentSourceSearch.
 */
bool SearchImplementedHelperFunctions::isSearchStage(DocumentSource* stage) {
    return stage &&
        (dynamic_cast<mongo::DocumentSourceSearch*>(stage) ||
         (dynamic_cast<mongo::DocumentSourceInternalSearchMongotRemote*>(stage) &&
          !dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage)));
}

bool SearchImplementedHelperFunctions::isSearchMetaStage(DocumentSource* stage) {
    return stage && dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage);
}

/**
 * Creates an additional pipeline to be run during a query if the query needs to generate metadata.
 * Does not take ownership of the passed in pipeline, and returns a pipeline containing only a
 * metadata generating $search stage. Can return null if no metadata pipeline is required.
 */
std::unique_ptr<Pipeline, PipelineDeleter>
SearchImplementedHelperFunctions::generateMetadataPipelineForSearch(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const AggregateCommandRequest& request,
    Pipeline* origPipeline,
    boost::optional<UUID> uuid) {
    if (expCtx->explain || !isSearchPipeline(origPipeline)) {
        // $search doesn't return documents or metadata from explain regardless of the verbosity.
        return nullptr;
    }

    auto origSearchStage =
        dynamic_cast<DocumentSourceInternalSearchMongotRemote*>(origPipeline->peekFront());
    tassert(6253727, "Expected search stage", origSearchStage);

    // We only want to return multiple cursors if we are not in mongos and we plan on getting
    // unmerged metadata documents back from mongot.
    auto shouldBuildMetadataPipeline =
        !expCtx->inMongos && origSearchStage->getIntermediateResultsProtocolVersion();

    uassert(
        6253506, "Cannot have exchange specified in a $search pipeline", !request.getExchange());

    // Some tests build $search pipelines without actually setting up a mongot. In this case either
    // return a dummy stage or nothing depending on the environment. Note that in this case we don't
    // actually make any queries, the document source will return eof immediately.
    if (MONGO_unlikely(DocumentSourceSearch::skipSearchStageRemoteSetup())) {
        if (shouldBuildMetadataPipeline) {
            // Construct a duplicate ExpressionContext for our cloned pipeline. This is necessary
            // so that the duplicated pipeline and the cloned pipeline do not accidentally
            // share an OperationContext.
            auto newExpCtx = expCtx->copyWith(expCtx->ns, expCtx->uuid);
            return Pipeline::create({origSearchStage->clone(newExpCtx)}, newExpCtx);
        }
        return nullptr;
    }

    // The search stage has not yet established its cursor on mongoT. Establish the cursor for it.
    auto cursors = mongot_cursor::establishSearchCursors(
        expCtx,
        origSearchStage->getSearchQuery(),
        origSearchStage->getTaskExecutor(),
        origSearchStage->getMongotDocsRequested(),
        buildSearchGetMoreFunc([origSearchStage] { return origSearchStage->calcDocsNeeded(); }),
        origSearchStage->getIntermediateResultsProtocolVersion(),
        origSearchStage->getPaginationFlag());

    // mongot can return zero cursors for an empty collection, one without metadata, or two for
    // results and metadata.
    tassert(6253500, "Expected less than or exactly two cursors from mongot", cursors.size() <= 2);

    if (cursors.size() == 0) {
        origSearchStage->markCollectionEmpty();
    }

    std::unique_ptr<Pipeline, PipelineDeleter> newPipeline = nullptr;
    for (auto it = cursors.begin(); it != cursors.end(); it++) {
        auto cursorLabel = it->getType();
        if (!cursorLabel) {
            // If a cursor is unlabeled mongot does not support metadata cursors. $$SEARCH_META
            // should not be supported in this query.
            tassert(6253301,
                    "Expected cursors to be labeled if there are more than one",
                    cursors.size() == 1);
            origSearchStage->setCursor(std::move(cursors.front()));
            return nullptr;
        }
        auto cursorType =
            CursorType_parse(IDLParserContext("ShardedAggHelperCursorType"), cursorLabel.value());
        if (cursorType == CursorTypeEnum::DocumentResult) {
            origSearchStage->setCursor(std::move(*it));
            origPipeline->pipelineType = CursorTypeEnum::DocumentResult;
        } else if (cursorType == CursorTypeEnum::SearchMetaResult) {
            // If we don't think we're in a sharded environment, mongot should not have sent
            // metadata.
            tassert(
                6253303, "Didn't expect metadata cursor from mongot", shouldBuildMetadataPipeline);
            tassert(
                6253726, "Expected to not already have created a metadata pipeline", !newPipeline);

            // Construct a duplicate ExpressionContext for our cloned pipeline. This is necessary
            // so that the duplicated pipeline and the cloned pipeline do not accidentally
            // share an OperationContext.
            auto newExpCtx = expCtx->copyWith(expCtx->ns, expCtx->uuid);

            // Clone the MongotRemote stage and set the metadata cursor.
            auto newStage = origSearchStage->copyForAlternateSource(std::move(*it), newExpCtx);

            // Build a new pipeline with the metadata source as the only stage.
            newPipeline = Pipeline::create({newStage}, newExpCtx);
            newPipeline->pipelineType = CursorTypeEnum::SearchMetaResult;
        } else {
            tasserted(6253302,
                      str::stream() << "Unexpected cursor type '" << cursorLabel.value() << "'");
        }
    }

    // Can return null if we did not build a metadata pipeline.
    return newPipeline;
}

BSONObj getExplainResponse(const ExpressionContext* expCtx,
                           const executor::RemoteCommandRequest& request,
                           executor::TaskExecutor* taskExecutor) {
    auto [promise, future] = makePromiseFuture<executor::TaskExecutor::RemoteCommandCallbackArgs>();
    auto promisePtr = std::make_shared<Promise<executor::TaskExecutor::RemoteCommandCallbackArgs>>(
        std::move(promise));
    auto scheduleResult = taskExecutor->scheduleRemoteCommand(
        std::move(request), [promisePtr](const auto& args) { promisePtr->emplaceValue(args); });
    if (!scheduleResult.isOK()) {
        // Since the command failed to be scheduled, the callback above did not and will not run.
        // Thus, it is safe to fulfill the promise here without worrying about synchronizing access
        // with the executor's thread.
        promisePtr->setError(scheduleResult.getStatus());
    }
    auto response = future.getNoThrow(expCtx->opCtx);
    uassertStatusOK(response.getStatus());
    uassertStatusOK(response.getValue().response.status);
    BSONObj responseData = response.getValue().response.data;
    uassertStatusOK(getStatusFromCommandResult(responseData));
    auto explain = responseData["explain"];
    uassert(4895000,
            "Response must contain an 'explain' field that is of type 'Object'",
            explain.type() == BSONType::Object);
    return explain.embeddedObject().getOwned();
}

std::vector<executor::TaskExecutorCursor> establishSearchCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& query,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    boost::optional<long long> docsRequested,
    std::function<void(BSONObjBuilder& bob)> augmentGetMore,
    const boost::optional<int>& protocolVersion,
    bool requiresSearchSequenceToken,
    std::unique_ptr<PlanYieldPolicyRemoteCursor> yieldPolicy) {
    // UUID is required for mongot queries. If not present, no results for the query as the
    // collection has not been created yet.
    if (!expCtx->uuid) {
        return {};
    }

    return establishCursors(expCtx,
                            getRemoteCommandRequestForSearchQuery(expCtx->opCtx,
                                                                  expCtx->ns,
                                                                  expCtx->uuid,
                                                                  expCtx->explain,
                                                                  query,
                                                                  docsRequested,
                                                                  requiresSearchSequenceToken,
                                                                  protocolVersion),
                            taskExecutor,
                            !docsRequested.has_value(),
                            augmentGetMore,
                            std::move(yieldPolicy));
}

BSONObj getSearchExplainResponse(const ExpressionContext* expCtx,
                                 const BSONObj& query,
                                 executor::TaskExecutor* taskExecutor) {
    const auto request = getRemoteCommandRequestForSearchQuery(
        expCtx->opCtx, expCtx->ns, expCtx->uuid, expCtx->explain, query, boost::none);
    return getExplainResponse(expCtx, request, taskExecutor);
}

namespace {

// Asserts that $$SEARCH_META is accessed correctly, that is, it is set by a prior stage, and is
// not accessed in a subpipline. It is assumed that if there is a
// 'DocumentSourceInternalSearchMongotRemote' then '$$SEARCH_META' will be set at some point in the
// pipeline. Depending on the configuration of the cluster,
// 'DocumentSourceSetVariableFromSubPipeline' could do the actual setting of the variable, but it
// can only be generated alongside a 'DocumentSourceInternalSearchMongotRemote'.
void assertSearchMetaAccessValidHelper(
    const std::vector<const Pipeline::SourceContainer*>& pipelines) {
    // Whether or not there was a sub-pipeline stage previously in this pipeline.
    bool subPipeSeen = false;
    bool searchMetaSet = false;

    for (const auto* pipeline : pipelines) {
        for (const auto& source : *pipeline) {
            // Check if this is a stage that sets $$SEARCH_META.
            static constexpr StringData kSetVarName =
                DocumentSourceSetVariableFromSubPipeline::kStageName;
            auto stageName = StringData(source->getSourceName());
            if (stageName == DocumentSourceInternalSearchMongotRemote::kStageName ||
                stageName == DocumentSourceSearch::kStageName || stageName == kSetVarName) {
                searchMetaSet = true;
                if (stageName == kSetVarName) {
                    tassert(6448003,
                            str::stream()
                                << "Expecting all " << kSetVarName << " stages to be setting "
                                << Variables::getBuiltinVariableName(Variables::kSearchMetaId),
                            checked_cast<DocumentSourceSetVariableFromSubPipeline*>(source.get())
                                    ->variableId() == Variables::kSearchMetaId);
                    // $setVariableFromSubPipeline has a "sub pipeline", but it is the exception to
                    // the scoping rule, since it is defining the $$SEARCH_META variable.
                    continue;
                }
            }

            // If this stage has a sub-pipeline, $$SEARCH_META is not allowed after this stage.
            auto thisStageSubPipeline = source->getSubPipeline();
            if (thisStageSubPipeline) {
                subPipeSeen = true;
                if (!thisStageSubPipeline->empty()) {
                    assertSearchMetaAccessValidHelper({thisStageSubPipeline});
                }
            }

            // Check if this stage references $$SEARCH_META.
            std::set<Variables::Id> refs;
            source->addVariableRefs(&refs);
            if (Variables::hasVariableReferenceTo(refs, {Variables::kSearchMetaId})) {
                uassert(6347901,
                        "Can't access $$SEARCH_META after a stage with a sub-pipeline",
                        !subPipeSeen || thisStageSubPipeline);
                uassert(
                    6347902,
                    "Can't access $$SEARCH_META without a $search stage earlier in the pipeline",
                    searchMetaSet);
            }
        }
    }
}

/**
 * Preparing for pipeline starting with $search on the DocumentSource-based implementation before
 * the query execution.
 * 'applyShardFilter' should be true for top level pipelines only.
 */
void prepareSearchPipeline(Pipeline* pipeline, bool applyShardFilter) {
    auto searchStage = pipeline->popFrontWithName(DocumentSourceSearch::kStageName);
    auto& sources = pipeline->getSources();
    if (searchStage) {
        auto desugaredPipeline = dynamic_cast<DocumentSourceSearch*>(searchStage.get())->desugar();
        sources.insert(sources.begin(), desugaredPipeline.begin(), desugaredPipeline.end());
        Pipeline::stitch(&sources);
    }

    auto internalSearchLookupIt = sources.begin();
    // Bail early if the pipeline is not $_internalSearchMongotRemote stage or doesn't need to apply
    // shardFilter.
    if (internalSearchLookupIt == sources.end() || !applyShardFilter ||
        (mongo::DocumentSourceInternalSearchMongotRemote::kStageName !=
             (*internalSearchLookupIt)->getSourceName() &&
         mongo::DocumentSourceVectorSearch::kStageName !=
             (*internalSearchLookupIt)->getSourceName())) {
        return;
    }

    while (internalSearchLookupIt != sources.end()) {
        if (DocumentSourceInternalSearchIdLookUp::kStageName ==
            (*internalSearchLookupIt)->getSourceName()) {
            break;
        }
        internalSearchLookupIt++;
    }

    if (internalSearchLookupIt != sources.end()) {
        auto expCtx = pipeline->getContext();
        if (OperationShardingState::isComingFromRouter(expCtx->opCtx)) {
            // We can only rely on the ownership filter if the operation is coming from the router
            // (i.e. it is versioned).
            auto collectionFilter =
                CollectionShardingState::acquire(expCtx->opCtx, expCtx->ns)
                    ->getOwnershipFilter(
                        expCtx->opCtx,
                        CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup);
            auto doc = new DocumentSourceInternalShardFilter(
                expCtx, std::make_unique<ShardFiltererImpl>(std::move(collectionFilter)));
            internalSearchLookupIt++;
            sources.insert(internalSearchLookupIt, doc);
            Pipeline::stitch(&sources);
        }
    }
}

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
    std::function<bool(Status)> retryPolicy = makeRetryOnNetworkErrorPolicy()) {
    using namespace fmt::literals;
    auto taskExecutor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
    executor::RemoteCommandResponse response =
        Status(ErrorCodes::InternalError, "Internal error running search command");
    for (;;) {
        Status err = Status::OK();
        do {
            auto swCbHnd = taskExecutor->scheduleRemoteCommand(
                getRemoteCommandRequest(expCtx->opCtx, expCtx->ns, cmdObj),
                [&](const auto& args) { response = args.response; });
            err = swCbHnd.getStatus();
            if (!err.isOK()) {
                // scheduling error
                err.addContext("Failed to execute search command: {}"_format(cmdObj.toString()));
                break;
            }
            if (MONGO_likely(shardedSearchOpCtxDisconnect.shouldFail())) {
                expCtx->opCtx->markKilled();
            }
            // It is imperative to wrap the wait() call in a try/catch. If an exception is thrown
            // and not caught, planShardedSearch will exit and all stack-allocated variables will be
            // destroyed. Then later when the executor thread tries to run the callbackFn of
            // scheduleRemoteCommand (the lambda above), it will try to access the `response` var,
            // which had been captured by reference and thus lived on the stack and therefore
            // destroyed as part of stack unwinding, and the server will segfault.

            // By catching the exception and then wait-ing for the callbackFn to run, we
            // ensure that planShardedSearch isn't exited (and the `response` object isn't
            // destroyed) before the callbackFn (which has a reference to `response`) executes.
            try {
                taskExecutor->wait(swCbHnd.getValue(), expCtx->opCtx);
            } catch (const DBException& exception) {
                LOGV2_ERROR(8049900,
                            "An interruption occured while the MongotTaskExecutor was waiting for "
                            "a response",
                            "error"_attr = exception.toStatus());
                // If waiting for the response is interrupted, like by a ClientDisconnectError, then
                // we still have a callback-handle out and registered with the TaskExecutor to run
                // when the response finally does come back.

                // Since the callback-handle references local state, cbResponse, it would
                // be invalid for the callback-handle to run after leaving the this function.
                // Therefore, cancel() stops any work associated with the callback handle (eg
                // network work in the case of scheduleRemoteCommand).

                // The contract for executor::scheduleRemoteCommand(....., callbackFn) requires that
                // callbackFn (the lambda in our case) is always run. Thefore after the cancel(), we
                // wait() for the callbackFn to be run with a not-ok status to inform the executor
                // that the original callback handle call (scheduleRemoteCommand) was canceled.
                taskExecutor->cancel(swCbHnd.getValue());
                taskExecutor->wait(swCbHnd.getValue());
                throw;
            }
            err = response.status;
            if (!err.isOK()) {
                // Local error running the command.
                err.addContext("Failed to execute search command: {}"_format(cmdObj.toString()));
                break;
            }
            err = getStatusFromCommandResult(response.data);
            if (!err.isOK()) {
                // Mongot ran the command and returned an error.
                err.addContext("mongot returned an error");
                break;
            }
        } while (0);

        if (err.isOK())
            return response;
        if (!retryPolicy(err))
            uassertStatusOK(err);
    }
}

ServiceContext::ConstructorActionRegisterer searchQueryImplementation{
    "searchQueryImplementation", {"searchQueryHelperRegisterer"}, [](ServiceContext* context) {
        static SearchImplementedHelperFunctions searchImplementedHelperFunctions;

        invariant(context);
        getSearchHelpers(context) = &searchImplementedHelperFunctions;
    }};
}  // namespace

InternalSearchMongotRemoteSpec planShardedSearch(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& searchRequest) {
    // Mongos issues the 'planShardedSearch' command rather than 'search' in order to:
    // * Create the merging pipeline.
    // * Get a sortSpec.
    const auto cmdObj = [&]() {
        PlanShardedSearchSpec cmd(expCtx->ns.coll().rawData() /* planShardedSearch */,
                                  searchRequest /* query */);

        if (expCtx->explain) {
            cmd.setExplain(BSON("verbosity" << ExplainOptions::verbosityString(*expCtx->explain)));
        }

        // Add the searchFeatures field.
        cmd.setSearchFeatures(
            BSON(SearchFeatures_serializer(SearchFeaturesEnum::kShardedSort) << 1));

        return cmd.toBSON();
    }();
    // Send the planShardedSearch to the remote, retrying on network errors.
    auto response = runSearchCommandWithRetries(expCtx, cmdObj);

    InternalSearchMongotRemoteSpec remoteSpec(searchRequest.getOwned(),
                                              response.data["protocolVersion"_sd].Int());
    auto parsedPipeline = mongo::Pipeline::parseFromArray(response.data["metaPipeline"], expCtx);
    remoteSpec.setMergingPipeline(parsedPipeline->serializeToBson());
    if (response.data.hasElement("sortSpec")) {
        remoteSpec.setSortSpec(response.data["sortSpec"].Obj().getOwned());
    }

    return remoteSpec;
}

void SearchImplementedHelperFunctions::assertSearchMetaAccessValid(
    const Pipeline::SourceContainer& pipeline, ExpressionContext* expCtx) {
    if (pipeline.empty()) {
        return;
    }

    // If we already validated this pipeline on mongos, no need to do it again on a shard. Check
    // mergeCursors because we could be on a shard doing the merge.
    if ((expCtx->inMongos || !expCtx->needsMerge) &&
        pipeline.front()->getSourceName() != DocumentSourceMergeCursors::kStageName) {
        assertSearchMetaAccessValidHelper({&pipeline});
    }
}

void SearchImplementedHelperFunctions::assertSearchMetaAccessValid(
    const Pipeline::SourceContainer& shardsPipeline,
    const Pipeline::SourceContainer& mergePipeline,
    ExpressionContext* expCtx) {
    assertSearchMetaAccessValidHelper({&shardsPipeline, &mergePipeline});
}

void mongo::mongot_cursor::SearchImplementedHelperFunctions::prepareSearchForTopLevelPipeline(
    Pipeline* pipeline) {
    prepareSearchPipeline(pipeline, true);
}

void mongo::mongot_cursor::SearchImplementedHelperFunctions::prepareSearchForNestedPipeline(
    Pipeline* pipeline) {
    prepareSearchPipeline(pipeline, false);
}

bool hasReferenceToSearchMeta(const DocumentSource& ds) {
    std::set<Variables::Id> refs;
    ds.addVariableRefs(&refs);
    return Variables::hasVariableReferenceTo(refs,
                                             std::set<Variables::Id>{Variables::kSearchMetaId});
}

void throwIfNotRunningWithMongotHostConfigured(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // We must validate if a mongot is configured. However, we might just be parsing or validating
    // the query without executing it. In this scenario, there is no reason to check if we are
    // running with a mongot configured, since we will never make a call to the mongot host. For
    // example, if we are in query analysis, performing pipeline-style updates, or creating query
    // shapes. Additionally, it would be an error to validate this inside query analysis, since
    // query analysis doesn't have access to the mongot host.
    //
    // This validation should occur before parsing so in the case of a parse and configuration
    // error, the configuration error is thrown.
    if (expCtx->mongoProcessInterface->isExpectedToExecuteQueries()) {
        doThrowIfNotRunningWithMongotHostConfigured();
    }
}

std::unique_ptr<SearchNode> SearchImplementedHelperFunctions::getSearchNode(DocumentSource* stage) {
    if (isSearchStage(stage)) {
        auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage);
        auto node = std::make_unique<SearchNode>(false,
                                                 searchStage->getSearchQuery(),
                                                 searchStage->getLimit(),
                                                 searchStage->getSortSpec(),
                                                 searchStage->getRemoteCursorId(),
                                                 searchStage->getRemoteCursorVars());
        return node;
    } else if (isSearchMetaStage(stage)) {
        auto searchStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage);
        return std::make_unique<SearchNode>(true,
                                            searchStage->getSearchQuery(),
                                            boost::none /* limit */,
                                            boost::none /* sortSpec */,
                                            searchStage->getRemoteCursorId(),
                                            searchStage->getRemoteCursorVars());
    } else {
        tasserted(7855801, str::stream() << "Unknown stage type" << stage->getSourceName());
    }
}

void SearchImplementedHelperFunctions::establishSearchQueryCursors(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    DocumentSource* stage,
    std::unique_ptr<PlanYieldPolicyRemoteCursor> yieldPolicy) {
    if (!expCtx->uuid || !isSearchStage(stage) ||
        MONGO_unlikely(DocumentSourceSearch::skipSearchStageRemoteSetup())) {
        return;
    }
    auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage);
    auto executor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
    auto cursors =
        mongot_cursor::establishSearchCursors(expCtx,
                                              searchStage->getSearchQuery(),
                                              executor,
                                              searchStage->getLimit(),
                                              nullptr,
                                              searchStage->getIntermediateResultsProtocolVersion(),
                                              searchStage->getSearchPaginationFlag(),
                                              std::move(yieldPolicy));

    auto [documentCursor, metaCursor] = parseMongotResponseCursors(std::move(cursors));

    if (documentCursor) {
        searchStage->setRemoteCursorVars(documentCursor->getCursorVars());
        searchStage->setCursor(std::move(*documentCursor));
    }

    if (metaCursor) {
        // If we don't think we're in a sharded environment, mongot should not have sent
        // metadata.
        tassert(7856002,
                "Didn't expect metadata cursor from mongot",
                !expCtx->inMongos && searchStage->getIntermediateResultsProtocolVersion());
        searchStage->setMetadataCursor(std::move(*metaCursor));
    }
}

void SearchImplementedHelperFunctions::establishSearchMetaCursor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    DocumentSource* stage,
    std::unique_ptr<PlanYieldPolicyRemoteCursor> yieldPolicy) {
    if (!expCtx->uuid || !isSearchMetaStage(stage) ||
        MONGO_unlikely(DocumentSourceSearch::skipSearchStageRemoteSetup())) {
        return;
    }

    auto searchStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage);
    auto executor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
    auto cursors =
        mongot_cursor::establishSearchCursors(expCtx,
                                              searchStage->getSearchQuery(),
                                              executor,
                                              boost::none,
                                              nullptr,
                                              searchStage->getIntermediateResultsProtocolVersion(),
                                              false /* requiresSearchSequenceToken */,
                                              std::move(yieldPolicy));

    auto [documentCursor, metaCursor] = parseMongotResponseCursors(std::move(cursors));

    if (metaCursor) {
        searchStage->setCursor(std::move(*metaCursor));
    } else {
        tassert(7856203,
                "If there's one cursor we expect to get SEARCH_META from the attached vars",
                !searchStage->getIntermediateResultsProtocolVersion() &&
                    !documentCursor->getType() && documentCursor->getCursorVars());
        searchStage->setRemoteCursorVars(documentCursor->getCursorVars());
        searchStage->setCursor(std::move(*documentCursor));
    }
}

bool SearchImplementedHelperFunctions::encodeSearchForSbeCache(const ExpressionContext* expCtx,
                                                               DocumentSource* ds,
                                                               BufBuilder* bufBuilder) {
    if ((!isSearchStage(ds) && !isSearchMetaStage(ds))) {
        return false;
    }
    // Encoding for $search/$searchMeta with its stage name, we also includes storedSource flag
    // for $search as well. We don't need to encode other info from the stage, such as search
    // query, limit, sortSpec, because they are all parameterized into slots.
    bufBuilder->appendStr(ds->getSourceName(), false /* includeEndingNull */);
    if (auto searchStage = dynamic_cast<DocumentSourceSearch*>(ds)) {
        bufBuilder->appendChar(searchStage->isStoredSource() ? '1' : '0');
        // The remoteCursorId is the offset of the cursor in opCtx, we expect it to be same across
        // query runs, but we encode it in the key for safety. Currently the id is fixed to be '0'
        // because there is only one possible cursor in an executor.
        bufBuilder->appendNum(searchStage->getRemoteCursorId());
    } else if (auto searchStage = dynamic_cast<DocumentSourceSearchMeta*>(ds)) {
        // See comment above for DocumentSourceSearch.
        bufBuilder->appendNum(searchStage->getRemoteCursorId());
    } else {
        MONGO_UNREACHABLE;
    }
    // We usually don't cache explain query, except inside $lookup sub-pipeline.
    bufBuilder->appendChar(expCtx->explain ? '1' : '0');
    return true;
}

boost::optional<executor::TaskExecutorCursor>
SearchImplementedHelperFunctions::getSearchMetadataCursor(DocumentSource* ds) {
    if (auto search = dynamic_cast<DocumentSourceSearch*>(ds)) {
        return search->getMetadataCursor();
    }
    return boost::none;
}

std::function<void(BSONObjBuilder& bob)> SearchImplementedHelperFunctions::buildSearchGetMoreFunc(
    std::function<boost::optional<long long>()> calcDocsNeeded) {
    if (!calcDocsNeeded) {
        return nullptr;
    }
    return [calcDocsNeeded](BSONObjBuilder& bob) {
        auto docsNeeded = calcDocsNeeded();
        // (Ignore FCV check): This feature is enabled on an earlier FCV.
        if (feature_flags::gFeatureFlagSearchBatchSizeLimit.isEnabledAndIgnoreFCVUnsafe() &&
            docsNeeded.has_value()) {
            BSONObjBuilder cursorOptionsBob(bob.subobjStart(mongot_cursor::kCursorOptionsField));
            cursorOptionsBob.append(mongot_cursor::kDocsRequestedField, docsNeeded.get());
            cursorOptionsBob.doneFast();
        }
    };
}

std::unique_ptr<RemoteCursorMap> SearchImplementedHelperFunctions::getSearchRemoteCursors(
    const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline) {
    if (cqPipeline.empty() || MONGO_unlikely(DocumentSourceSearch::skipSearchStageRemoteSetup())) {
        return nullptr;
    }
    // We currently only put the first search stage into RemoteCursorMap since only one search
    // is possible in the pipeline and sub-pipeline is in separate PlanExecutorSBE. In the future we
    // will need to recursively check search in every pipeline.
    auto stage = cqPipeline.front().get();
    if (auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage)) {
        auto cursor = searchStage->getCursor();
        if (!cursor) {
            return nullptr;
        }
        auto cursorMap = std::make_unique<RemoteCursorMap>();
        cursorMap->insert({searchStage->getRemoteCursorId(),
                           std::make_unique<executor::TaskExecutorCursor>(std::move(*cursor))});
        return cursorMap;
    } else if (auto searchMetaStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage)) {
        auto cursor = searchMetaStage->getCursor();
        if (!cursor) {
            return nullptr;
        }
        auto cursorMap = std::make_unique<RemoteCursorMap>();
        cursorMap->insert({searchMetaStage->getRemoteCursorId(),
                           std::make_unique<executor::TaskExecutorCursor>(std::move(*cursor))});
        return cursorMap;
    }
    return nullptr;
}

std::unique_ptr<RemoteExplainVector> SearchImplementedHelperFunctions::getSearchRemoteExplains(
    const ExpressionContext* expCtx,
    const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline) {
    if (cqPipeline.empty() || !expCtx->explain ||
        MONGO_unlikely(DocumentSourceSearch::skipSearchStageRemoteSetup())) {
        return nullptr;
    }
    // We currently only put the first search stage explain into RemoteExplainVector since only one
    // search is possible in the pipeline and sub-pipeline is in separate PlanExecutorSBE. In the
    // future we will need to recursively check search in every pipeline.
    auto stage = cqPipeline.front().get();
    if (auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage)) {
        auto explainMap = std::make_unique<RemoteExplainVector>();
        explainMap->push_back(getSearchRemoteExplain(expCtx,
                                                     searchStage->getSearchQuery(),
                                                     searchStage->getRemoteCursorId(),
                                                     searchStage->getSortSpec()));
        return explainMap;
    } else if (auto searchMetaStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage)) {
        auto explainMap = std::make_unique<RemoteExplainVector>();
        explainMap->push_back(getSearchRemoteExplain(expCtx,
                                                     searchMetaStage->getSearchQuery(),
                                                     searchMetaStage->getRemoteCursorId(),
                                                     boost::none /* sortSpec */));
        return explainMap;
    }
    return nullptr;
}
}  // namespace mongo::mongot_cursor
