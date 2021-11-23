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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/run_aggregate.h"

#include <boost/optional.hpp>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/change_stream_proxy.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/mongo_process_interface.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/string_map.h"

namespace mongo {

using boost::intrusive_ptr;
using std::endl;
using std::shared_ptr;
using std::string;
using std::stringstream;
using std::unique_ptr;
using stdx::make_unique;

namespace {
/**
 * Returns true if this PlanExecutor is for a Pipeline.
 */
bool isPipelineExecutor(const PlanExecutor* exec) {
    invariant(exec);
    auto rootStage = exec->getRootStage();
    return rootStage->stageType() == StageType::STAGE_PIPELINE_PROXY ||
        rootStage->stageType() == StageType::STAGE_CHANGE_STREAM_PROXY;
}

/**
 * If a pipeline is empty (assuming that a $cursor stage hasn't been created yet), it could mean
 * that we were able to absorb all pipeline stages and pull them into a single PlanExecutor. So,
 * instead of creating a whole pipeline to do nothing more than forward the results of its cursor
 * document source, we can optimize away the entire pipeline and answer the request using the query
 * engine only. This function checks if such optimization is possible.
 */
bool canOptimizeAwayPipeline(const Pipeline* pipeline,
                             const PlanExecutor* exec,
                             const AggregationRequest& request,
                             bool hasGeoNearStage,
                             bool hasChangeStreamStage) {
    return pipeline && exec && !hasGeoNearStage && !hasChangeStreamStage &&
        pipeline->getSources().empty() &&
        // For exchange we will create a number of pipelines consisting of a single
        // DocumentSourceExchange stage, so cannot not optimize it away.
        !request.getExchangeSpec() &&
        !QueryPlannerCommon::hasNode(exec->getCanonicalQuery()->root(), MatchExpression::TEXT);
}

/**
 * Returns true if we need to keep a ClientCursor saved for this pipeline (for future getMore
 * requests). Otherwise, returns false. The passed 'nsForCursor' is only used to determine the
 * namespace used in the returned cursor, which will be registered with the global cursor manager,
 * and thus will be different from that in 'request'.
 */
bool handleCursorCommand(OperationContext* opCtx,
                         const NamespaceString& nsForCursor,
                         std::vector<ClientCursor*> cursors,
                         const AggregationRequest& request,
                         rpc::ReplyBuilderInterface* result) {
    invariant(!cursors.empty());
    long long batchSize = request.getBatchSize();

    if (cursors.size() > 1) {

        uassert(
            ErrorCodes::BadValue, "the exchange initial batch size must be zero", batchSize == 0);

        BSONArrayBuilder cursorsBuilder;
        for (size_t idx = 0; idx < cursors.size(); ++idx) {
            invariant(cursors[idx]);

            BSONObjBuilder cursorResult;
            appendCursorResponseObject(
                cursors[idx]->cursorid(), nsForCursor.ns(), BSONArray(), &cursorResult);
            cursorResult.appendBool("ok", 1);

            cursorsBuilder.append(cursorResult.obj());

            // If a time limit was set on the pipeline, remaining time is "rolled over" to the
            // cursor (for use by future getmore ops).
            cursors[idx]->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

            // Cursor needs to be in a saved state while we yield locks for getmore. State
            // will be restored in getMore().
            cursors[idx]->getExecutor()->saveState();
            cursors[idx]->getExecutor()->detachFromOperationContext();
        }

        auto bodyBuilder = result->getBodyBuilder();
        bodyBuilder.appendArray("cursors", cursorsBuilder.obj());

        return true;
    }

    CursorResponseBuilder::Options options;
    options.isInitialResponse = true;
    CursorResponseBuilder responseBuilder(result, options);

    auto curOp = CurOp::get(opCtx);
    auto cursor = cursors[0];
    invariant(cursor);
    auto exec = cursor->getExecutor();
    invariant(exec);

    BSONObj next;
    bool stashedResult = false;
    for (int objCount = 0; objCount < batchSize; objCount++) {
        // The initial getNext() on a PipelineProxyStage may be very expensive so we don't
        // do it when batchSize is 0 since that indicates a desire for a fast return.
        PlanExecutor::ExecState state;

        try {
            state = exec->getNext(&next, nullptr);
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // This exception is thrown when a $changeStream stage encounters an event
            // that invalidates the cursor. We should close the cursor and return without
            // error.
            cursor = nullptr;
            exec = nullptr;
            break;
        }

        if (state == PlanExecutor::IS_EOF) {
            if (!cursor->isTailable()) {
                // make it an obvious error to use cursor or executor after this point
                cursor = nullptr;
                exec = nullptr;
            }
            break;
        }

        if (PlanExecutor::ADVANCED != state) {
            // We should always have a valid status member object at this point.
            auto status = WorkingSetCommon::getMemberObjectStatus(next);
            invariant(!status.isOK());
            warning() << "Aggregate command executor error: " << PlanExecutor::statestr(state)
                      << ", status: " << status
                      << ", stats: " << redact(Explain::getWinningPlanStats(exec));

            uassertStatusOK(status.withContext("PlanExecutor error during aggregation"));
        }

        // If adding this object will cause us to exceed the message size limit, then we stash it
        // for later.
        if (!FindCommon::haveSpaceForNext(next, objCount, responseBuilder.bytesUsed())) {
            exec->enqueue(next);
            stashedResult = true;
            break;
        }

        // TODO SERVER-38539: We need to set both the latestOplogTimestamp and the PBRT until the
        // former is removed in a future release.
        responseBuilder.setLatestOplogTimestamp(exec->getLatestOplogTimestamp());
        responseBuilder.setPostBatchResumeToken(exec->getPostBatchResumeToken());
        responseBuilder.append(next);
    }

    if (cursor) {
        invariant(cursor->getExecutor() == exec);

        // For empty batches, or in the case where the final result was added to the batch rather
        // than being stashed, we update the PBRT to ensure that it is the most recent available.
        if (!stashedResult) {
            // TODO SERVER-38539: We need to set both the latestOplogTimestamp and the PBRT until
            // the former is removed in a future release.
            responseBuilder.setLatestOplogTimestamp(exec->getLatestOplogTimestamp());
            responseBuilder.setPostBatchResumeToken(exec->getPostBatchResumeToken());
        }
        // If a time limit was set on the pipeline, remaining time is "rolled over" to the
        // cursor (for use by future getmore ops).
        cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

        curOp->debug().cursorid = cursor->cursorid();

        // Cursor needs to be in a saved state while we yield locks for getmore. State
        // will be restored in getMore().
        exec->saveState();
        exec->detachFromOperationContext();
    } else {
        curOp->debug().cursorExhausted = true;
    }

    const CursorId cursorId = cursor ? cursor->cursorid() : 0LL;
    responseBuilder.done(cursorId, nsForCursor.ns());

    return static_cast<bool>(cursor);
}

StatusWith<StringMap<ExpressionContext::ResolvedNamespace>> resolveInvolvedNamespaces(
    OperationContext* opCtx, const AggregationRequest& request) {
    const LiteParsedPipeline liteParsedPipeline(request);
    const auto& pipelineInvolvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

    // If there are no involved namespaces, return before attempting to take any locks. This is
    // important for collectionless aggregations, which may be expected to run without locking.
    if (pipelineInvolvedNamespaces.empty()) {
        return {StringMap<ExpressionContext::ResolvedNamespace>()};
    }

    // We intentionally do not drop and reacquire our system.views collection lock after resolving
    // the view definition in order to prevent the definition for any view namespaces we've already
    // resolved from changing. This is necessary to prevent a cycle from being formed among the view
    // definitions cached in 'resolvedNamespaces' because we won't re-resolve a view namespace we've
    // already encountered.
    AutoGetCollection autoColl(opCtx,
                               NamespaceString(request.getNamespaceString().db(),
                                               NamespaceString::kSystemDotViewsCollectionName),
                               MODE_IS);
    Database* const db = autoColl.getDb();
    ViewCatalog* viewCatalog = db ? ViewCatalog::get(db) : nullptr;

    std::deque<NamespaceString> involvedNamespacesQueue(pipelineInvolvedNamespaces.begin(),
                                                        pipelineInvolvedNamespaces.end());
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;

    while (!involvedNamespacesQueue.empty()) {
        auto involvedNs = std::move(involvedNamespacesQueue.front());
        involvedNamespacesQueue.pop_front();

        if (resolvedNamespaces.find(involvedNs.coll()) != resolvedNamespaces.end()) {
            continue;
        }

        // If 'ns' refers to a view namespace, then we resolve its definition.
        auto resolveViewDefinition = [&](const NamespaceString& ns) -> Status {
            auto resolvedView = viewCatalog->resolveView(opCtx, ns);
            if (!resolvedView.isOK()) {
                return {ErrorCodes::FailedToParse,
                        str::stream() << "Failed to resolve view '" << involvedNs.ns()
                                      << "': " << resolvedView.getStatus().toString()};
            }

            resolvedNamespaces[ns.coll()] = {resolvedView.getValue().getNamespace(),
                                             resolvedView.getValue().getPipeline()};

            // We parse the pipeline corresponding to the resolved view in case we must resolve
            // other view namespaces that are also involved.
            LiteParsedPipeline resolvedViewLitePipeline(
                {resolvedView.getValue().getNamespace(), resolvedView.getValue().getPipeline()});

            const auto& resolvedViewInvolvedNamespaces =
                resolvedViewLitePipeline.getInvolvedNamespaces();
            involvedNamespacesQueue.insert(involvedNamespacesQueue.end(),
                                           resolvedViewInvolvedNamespaces.begin(),
                                           resolvedViewInvolvedNamespaces.end());
            return Status::OK();
        };

        // If the involved namespace is not in the same database as the aggregation, it must be
        // from a $merge to a collection in a different database.
        if (involvedNs.db() != request.getNamespaceString().db()) {
            // SERVER-51886: It is not correct to assume that we are reading from a collection
            // because the collection targeted by $out/$merge on a given database can have the same
            // name as a view on the source database. As such, we determine whether the collection
            // name references a view on the aggregation request's database. Note that the inverse
            // scenario (mistaking a view for a collection) is not an issue because $merge/$out
            // cannot target a view.
            auto nssToCheck = NamespaceString(request.getNamespaceString().db(), involvedNs.coll());
            if (viewCatalog && viewCatalog->lookup(opCtx, nssToCheck.ns())) {
                auto status = resolveViewDefinition(nssToCheck);
                if (!status.isOK()) {
                    return status;
                }
            } else {
                resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
            }
        } else if (!db || CollectionCatalog::get(opCtx).lookupCollectionByNamespace(involvedNs)) {
            // If the aggregation database exists and 'involvedNs' refers to a collection namespace,
            // then we resolve it as an empty pipeline in order to read directly from the underlying
            // collection. If the database doesn't exist, then we still resolve it as an empty
            // pipeline because 'involvedNs' doesn't refer to a view namespace in our consistent
            // snapshot of the view catalog.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
        } else if (viewCatalog->lookup(opCtx, involvedNs.ns())) {
            auto status = resolveViewDefinition(involvedNs);
            if (!status.isOK()) {
                return status;
            }
        } else {
            // 'involvedNs' is neither a view nor a collection, so resolve it as an empty pipeline
            // to treat it as reading from a non-existent collection.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
        }
    }

    return resolvedNamespaces;
}

/**
 * Returns Status::OK if each view namespace in 'pipeline' has a default collator equivalent to
 * 'collator'. Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
 */
Status collatorCompatibleWithPipeline(OperationContext* opCtx,
                                      Database* db,
                                      const CollatorInterface* collator,
                                      const LiteParsedPipeline& liteParsedPipeline) {
    if (!db) {
        return Status::OK();
    }
    for (auto&& potentialViewNs : liteParsedPipeline.getInvolvedNamespaces()) {
        if (db->getCollection(opCtx, potentialViewNs)) {
            continue;
        }

        auto view = ViewCatalog::get(db)->lookup(opCtx, potentialViewNs.ns());
        if (!view) {
            continue;
        }
        if (!CollatorInterface::collatorsMatch(view->defaultCollator(), collator)) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    str::stream() << "Cannot override default collation of view "
                                  << potentialViewNs.ns()};
        }
    }
    return Status::OK();
}

/**
 * Resolves the collator to either the user-specified collation or, if none was specified, to the
 * collection-default collation.
 */
std::unique_ptr<CollatorInterface> resolveCollator(OperationContext* opCtx,
                                                   const AggregationRequest& request,
                                                   const Collection* collection) {
    if (!request.getCollation().isEmpty()) {
        return uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                   ->makeFromBSON(request.getCollation()));
    }

    return (collection && collection->getDefaultCollator()
                ? collection->getDefaultCollator()->clone()
                : nullptr);
}

boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const AggregationRequest& request,
    std::unique_ptr<CollatorInterface> collator,
    boost::optional<UUID> uuid) {
    boost::intrusive_ptr<ExpressionContext> expCtx =
        new ExpressionContext(opCtx,
                              request,
                              std::move(collator),
                              MongoProcessInterface::create(opCtx),
                              uassertStatusOK(resolveInvolvedNamespaces(opCtx, request)),
                              uuid);
    expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";
    expCtx->inMultiDocumentTransaction = opCtx->inMultiDocumentTransaction();

    return expCtx;
}

/**
 * Upconverts the read concern for a change stream aggregation, if necesssary.
 *
 * If there is no given read concern level on the given object, upgrades the level to 'majority' and
 * waits for read concern. If a read concern level is already specified on the given read concern
 * object, this method does nothing.
 */
void _adjustChangeStreamReadConcern(OperationContext* opCtx) {
    repl::ReadConcernArgs& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    // There is already a read concern level set. Do nothing.
    if (readConcernArgs.hasLevel()) {
        return;
    }
    // We upconvert an empty read concern to 'majority'.
    {
        // We must obtain the client lock to set the ReadConcernArgs on the operation
        // context as it may be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);

        // Change streams are allowed to use the speculative majority read mechanism, if
        // the storage engine doesn't support majority reads directly.
        if (!serverGlobalParams.enableMajorityReadConcern) {
            readConcernArgs.setMajorityReadMechanism(
                repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative);
        }
    }

    // Wait for read concern again since we changed the original read concern.
    uassertStatusOK(waitForReadConcern(
        opCtx, readConcernArgs, true, PrepareConflictBehavior::kIgnoreConflicts));
}

/**
 * If the aggregation 'request' contains an exchange specification, create a new pipeline for each
 * consumer and put it into the resulting vector. Otherwise, return the original 'pipeline' as a
 * single vector element.
 */
std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> createExchangePipelinesIfNeeded(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const AggregationRequest& request,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    boost::optional<UUID> uuid) {
    std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> pipelines;

    if (request.getExchangeSpec() && !expCtx->explain) {
        boost::intrusive_ptr<Exchange> exchange =
            new Exchange(request.getExchangeSpec().get(), std::move(pipeline));

        for (size_t idx = 0; idx < exchange->getConsumers(); ++idx) {
            // For every new pipeline we have create a new ExpressionContext as the context
            // cannot be shared between threads. There is no synchronization for pieces of
            // the execution machinery above the Exchange, so nothing above the Exchange can be
            // shared between different exchange-producer cursors.
            expCtx = makeExpressionContext(opCtx,
                                           request,
                                           expCtx->getCollator() ? expCtx->getCollator()->clone()
                                                                 : nullptr,
                                           uuid);

            // Create a new pipeline for the consumer consisting of a single
            // DocumentSourceExchange.
            boost::intrusive_ptr<DocumentSource> consumer = new DocumentSourceExchange(
                expCtx, exchange, idx, expCtx->mongoProcessInterface->getResourceYielder());
            pipelines.emplace_back(uassertStatusOK(Pipeline::create({consumer}, expCtx)));
        }
    } else {
        pipelines.emplace_back(std::move(pipeline));
    }

    return pipelines;
}

/**
 * Create a PlanExecutor to execute the given 'pipeline'.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> createOuterPipelineProxyExecutor(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    bool hasChangeStream) {
    // Transfer ownership of the Pipeline to the PipelineProxyStage.
    auto ws = make_unique<WorkingSet>();
    auto proxy = hasChangeStream
        ? make_unique<ChangeStreamProxyStage>(opCtx, std::move(pipeline), ws.get())
        : make_unique<PipelineProxyStage>(opCtx, std::move(pipeline), ws.get());

    // This PlanExecutor will simply forward requests to the Pipeline, so does not need
    // to yield or to be registered with any collection's CursorManager to receive
    // invalidations. The Pipeline may contain PlanExecutors which *are* yielding
    // PlanExecutors and which *are* registered with their respective collection's
    // CursorManager
    return uassertStatusOK(
        PlanExecutor::make(opCtx, std::move(ws), std::move(proxy), nss, PlanExecutor::NO_YIELD));
}
}  // namespace

Status runAggregate(OperationContext* opCtx,
                    const NamespaceString& origNss,
                    const AggregationRequest& request,
                    const BSONObj& cmdObj,
                    const PrivilegeVector& privileges,
                    rpc::ReplyBuilderInterface* result) {
    // For operations on views, this will be the underlying namespace.
    NamespaceString nss = request.getNamespaceString();

    // The collation to use for this aggregation. boost::optional to distinguish between the case
    // where the collation has not yet been resolved, and where it has been resolved to nullptr.
    boost::optional<std::unique_ptr<CollatorInterface>> collatorToUse;

    // The UUID of the collection for the execution namespace of this aggregation.
    boost::optional<UUID> uuid;

    // If emplaced, AutoGetCollectionForReadCommand will throw if the sharding version for this
    // connection is out of date. If the namespace is a view, the lock will be released before
    // re-running the expanded aggregation.
    boost::optional<AutoGetCollectionForReadCommand> ctx;

    std::vector<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs;
    boost::intrusive_ptr<ExpressionContext> expCtx;
    const LiteParsedPipeline liteParsedPipeline(request);
    auto curOp = CurOp::get(opCtx);
    {

        try {
            // Check whether the parsed pipeline supports the given read concern.
            liteParsedPipeline.assertSupportsReadConcern(
                opCtx, request.getExplain(), serverGlobalParams.enableMajorityReadConcern);
        } catch (const DBException& ex) {
            // If we are in a multi-document transaction, we intercept the 'readConcern'
            // assertion in order to provide a more descriptive error message and code.
            if (opCtx->inMultiDocumentTransaction()) {
                return {ErrorCodes::OperationNotSupportedInTransaction,
                        ex.toStatus("Operation not permitted in transaction").reason()};
            }
            return ex.toStatus();
        }

        const auto& pipelineInvolvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

        // If this is a collectionless aggregation, we won't create 'ctx' but will still need an
        // AutoStatsTracker to record CurOp and Top entries.
        boost::optional<AutoStatsTracker> statsTracker;

        // If this is a change stream, perform special checks and change the execution namespace.
        if (liteParsedPipeline.hasChangeStream()) {
            // Replace the execution namespace with that of the oplog.
            nss = NamespaceString::kRsOplogNamespace;

            // Upgrade and wait for read concern if necessary.
            _adjustChangeStreamReadConcern(opCtx);

            // AutoGetCollectionForReadCommand will raise an error if 'origNss' is a view. We do not
            // need to check this if we are opening a stream on an entire db or across the cluster.
            if (!origNss.isCollectionlessAggregateNS()) {
                AutoGetCollectionForReadCommand origNssCtx(opCtx, origNss);
            }

            // If the user specified an explicit collation, adopt it; otherwise, use the simple
            // collation. We do not inherit the collection's default collation or UUID, since
            // the stream may be resuming from a point before the current UUID existed.
            collatorToUse.emplace(resolveCollator(opCtx, request, nullptr));

            // Obtain collection locks on the execution namespace; that is, the oplog.
            ctx.emplace(opCtx, nss, AutoGetCollection::ViewMode::kViewsForbidden);
        } else if (nss.isCollectionlessAggregateNS() && pipelineInvolvedNamespaces.empty()) {
            // If this is a collectionless agg with no foreign namespaces, don't acquire any locks.
            statsTracker.emplace(opCtx,
                                 nss,
                                 Top::LockType::NotLocked,
                                 AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                 0);
            collatorToUse.emplace(resolveCollator(opCtx, request, nullptr));
        } else {
            // This is a regular aggregation. Lock the collection or view.
            ctx.emplace(opCtx, nss, AutoGetCollection::ViewMode::kViewsPermitted);
            collatorToUse.emplace(resolveCollator(opCtx, request, ctx->getCollection()));
            uuid = ctx->getCollection() ? ctx->getCollection()->uuid() : boost::none;
        }

        Collection* collection = ctx ? ctx->getCollection() : nullptr;

        // If this is a view, resolve it by finding the underlying collection and stitching view
        // pipelines and this request's pipeline together. We then release our locks before
        // recursively calling runAggregate(), which will re-acquire locks on the underlying
        // collection.  (The lock must be released because recursively acquiring locks on the
        // database will prohibit yielding.)
        if (ctx && ctx->getView() && !liteParsedPipeline.startsWithCollStats()) {
            invariant(nss != NamespaceString::kRsOplogNamespace);
            invariant(!nss.isCollectionlessAggregateNS());

            // Check that the default collation of 'view' is compatible with the operation's
            // collation. The check is skipped if the request did not specify a collation.
            if (!request.getCollation().isEmpty()) {
                invariant(collatorToUse);  // Should already be resolved at this point.
                if (!CollatorInterface::collatorsMatch(ctx->getView()->defaultCollator(),
                                                       collatorToUse->get())) {
                    return {ErrorCodes::OptionNotSupportedOnView,
                            "Cannot override a view's default collation"};
                }
            }

            auto resolvedView =
                uassertStatusOK(ViewCatalog::get(ctx->getDb())->resolveView(opCtx, nss));
            uassert(std::move(resolvedView),
                    "On sharded systems, resolved views must be executed by mongos",
                    !ShardingState::get(opCtx)->enabled());

            // With the view & collation resolved, we can relinquish locks.
            ctx.reset();

            // Parse the resolved view into a new aggregation request.
            auto newRequest = resolvedView.asExpandedViewAggregation(request);
            auto newCmd = newRequest.serializeToCommandObj().toBson();

            auto status = runAggregate(opCtx, origNss, newRequest, newCmd, privileges, result);

            {
                // Set the namespace of the curop back to the view namespace so ctx records
                // stats on this view namespace on destruction.
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                curOp->setNS_inlock(nss.ns());
            }

            return status;
        }

        invariant(collatorToUse);
        expCtx = makeExpressionContext(opCtx, request, std::move(*collatorToUse), uuid);

        expCtx->startExpressionCounters();
        auto pipeline = uassertStatusOK(Pipeline::parse(request.getPipeline(), expCtx));
        expCtx->stopExpressionCounters();

        // Check that the view's collation matches the collation of any views involved in the
        // pipeline.
        if (!pipelineInvolvedNamespaces.empty()) {
            invariant(ctx);
            auto pipelineCollationStatus = collatorCompatibleWithPipeline(
                opCtx, ctx->getDb(), expCtx->getCollator(), liteParsedPipeline);
            if (!pipelineCollationStatus.isOK()) {
                return pipelineCollationStatus;
            }
        }

        pipeline->optimizePipeline();

        // Check if the pipeline has a $geoNear stage, as it will be ripped away during the build
        // query executor phase below (to be replaced with a $geoNearCursorStage later during the
        // executor attach phase).
        auto hasGeoNearStage = !pipeline->getSources().empty() &&
            dynamic_cast<DocumentSourceGeoNear*>(pipeline->peekFront());

        // Prepare a PlanExecutor to provide input into the pipeline, if needed.
        std::pair<PipelineD::AttachExecutorCallback,
                  std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
            attachExecutorCallback;
        if (liteParsedPipeline.hasChangeStream()) {
            // If we are using a change stream, the cursor stage should have a simple collation,
            // regardless of what the user's collation was.
            std::unique_ptr<CollatorInterface> collatorForCursor = nullptr;
            auto collatorStash = expCtx->temporarilyChangeCollator(std::move(collatorForCursor));
            attachExecutorCallback =
                PipelineD::buildInnerQueryExecutor(collection, nss, &request, pipeline.get());
        } else {
            attachExecutorCallback =
                PipelineD::buildInnerQueryExecutor(collection, nss, &request, pipeline.get());
        }

        if (canOptimizeAwayPipeline(pipeline.get(),
                                    attachExecutorCallback.second.get(),
                                    request,
                                    hasGeoNearStage,
                                    liteParsedPipeline.hasChangeStream())) {
            // This pipeline is currently empty, but once completed it will have only one source,
            // which is a DocumentSourceCursor. Instead of creating a whole pipeline to do nothing
            // more than forward the results of its cursor document source, we can use the
            // PlanExecutor by itself. The resulting cursor will look like what the client would
            // have gotten from find command.
            execs.emplace_back(std::move(attachExecutorCallback.second));
        } else {
            // Complete creation of the initial $cursor stage, if needed.
            PipelineD::attachInnerQueryExecutorToPipeline(collection,
                                                          attachExecutorCallback.first,
                                                          std::move(attachExecutorCallback.second),
                                                          pipeline.get());

            // Optimize again, since there may be additional optimizations that can be done after
            // adding the initial cursor stage.
            pipeline->optimizePipeline();

            auto pipelines =
                createExchangePipelinesIfNeeded(opCtx, expCtx, request, std::move(pipeline), uuid);
            for (auto&& pipelineIt : pipelines) {
                execs.emplace_back(createOuterPipelineProxyExecutor(
                    opCtx, nss, std::move(pipelineIt), liteParsedPipeline.hasChangeStream()));
            }

            // With the pipelines created, we can relinquish locks as they will manage the locks
            // internally further on. We still need to keep the lock for an optimized away pipeline
            // though, as we will be changing its lock policy to 'kLockExternally' (see details
            // below), and in order to execute the initial getNext() call in 'handleCursorCommand',
            // we need to hold the collection lock.
            ctx.reset();
        }

        {
            auto planSummary = Explain::getPlanSummary(execs[0].get());
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setPlanSummary_inlock(std::move(planSummary));
        }
    }

    // Having released the collection lock, we can now create a cursor that returns results from the
    // pipeline. This cursor owns no collection state, and thus we register it with the global
    // cursor manager. The global cursor manager does not deliver invalidations or kill
    // notifications; the underlying PlanExecutor(s) used by the pipeline will be receiving
    // invalidations and kill notifications themselves, not the cursor we create here.

    std::vector<ClientCursorPin> pins;
    std::vector<ClientCursor*> cursors;

    auto cursorFreer = makeGuard([&] {
        for (auto& p : pins) {
            p.deleteUnderlying();
        }
    });
    for (auto&& exec : execs) {
        // PlanExecutors for pipelines always have a 'kLocksInternally' policy. If this executor is
        // not for a pipeline, though, that means the pipeline was optimized away and the
        // PlanExecutor will answer the query using the query engine only. Without the
        // DocumentSourceCursor to do its locking, an executor needs a 'kLockExternally' policy.
        auto lockPolicy = isPipelineExecutor(exec.get())
            ? ClientCursorParams::LockPolicy::kLocksInternally
            : ClientCursorParams::LockPolicy::kLockExternally;

        ClientCursorParams cursorParams(
            std::move(exec),
            origNss,
            AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
            opCtx->getWriteConcern(),
            repl::ReadConcernArgs::get(opCtx),
            cmdObj,
            lockPolicy,
            privileges);
        if (expCtx->tailableMode == TailableModeEnum::kTailable) {
            cursorParams.setTailable(true);
        } else if (expCtx->tailableMode == TailableModeEnum::kTailableAndAwaitData) {
            cursorParams.setTailable(true);
            cursorParams.setAwaitData(true);
        }

        auto pin = CursorManager::get(opCtx)->registerCursor(opCtx, std::move(cursorParams));
        invariant(!exec);

        cursors.emplace_back(pin.getCursor());
        pins.emplace_back(std::move(pin));
    }

    // Report usage statistics for each stage in the pipeline.
    liteParsedPipeline.tickGlobalStageCounters();

    // If both explain and cursor are specified, explain wins.
    if (expCtx->explain) {
        auto explainExecutor = pins[0].getCursor()->getExecutor();
        auto bodyBuilder = result->getBodyBuilder();
        if (isPipelineExecutor(explainExecutor)) {
            Explain::explainPipelineExecutor(explainExecutor, *(expCtx->explain), &bodyBuilder);
        } else {
            invariant(pins[0].getCursor()->lockPolicy() ==
                      ClientCursorParams::LockPolicy::kLockExternally);
            invariant(!explainExecutor->isDetached());
            invariant(explainExecutor->getOpCtx() == opCtx);
            // The explainStages() function for a non-pipeline executor expects to be called with
            // the appropriate collection lock already held. Make sure it has not been released yet.
            invariant(ctx);
            Explain::explainStages(explainExecutor,
                                   ctx->getCollection(),
                                   *(expCtx->explain),
                                   BSON("optimizedPipeline" << true),
                                   &bodyBuilder);
        }
    } else {
        // Cursor must be specified, if explain is not.
        const bool keepCursor =
            handleCursorCommand(opCtx, origNss, std::move(cursors), request, result);
        if (keepCursor) {
            cursorFreer.dismiss();
        }

        PlanSummaryStats stats;
        Explain::getSummaryStats(*(pins[0].getCursor()->getExecutor()), &stats);
        curOp->debug().setPlanSummaryMetrics(stats);
        curOp->debug().nreturned = stats.nReturned;
        // For an optimized away pipeline, signal the cache that a query operation has completed.
        // For normal pipelines this is done in DocumentSourceCursor.
        if (ctx && ctx->getCollection()) {
            ctx->getCollection()->infoCache()->notifyOfQuery(opCtx, stats.indexesUsed);
        }
    }

    // The aggregation pipeline may change the namespace of the curop and we need to set it back to
    // the original namespace to correctly report command stats. One example when the namespace can
    // be changed is when the pipeline contains an $out stage, which executes an internal command to
    // create a temp collection, changing the curop namespace to the name of this temp collection.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp->setNS_inlock(origNss.ns());
    }

    // Any code that needs the cursor pinned must be inside the try block, above.
    return Status::OK();
}

}  // namespace mongo
