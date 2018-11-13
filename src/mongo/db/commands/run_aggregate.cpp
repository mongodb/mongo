
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
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/pipeline_proxy.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_exchange.h"
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
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_participant.h"
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

    ClientCursor* cursor = cursors[0];
    invariant(cursor);

    BSONObj next;
    for (int objCount = 0; objCount < batchSize; objCount++) {
        // The initial getNext() on a PipelineProxyStage may be very expensive so we don't
        // do it when batchSize is 0 since that indicates a desire for a fast return.
        PlanExecutor::ExecState state;

        try {
            state = cursor->getExecutor()->getNext(&next, nullptr);
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // This exception is thrown when a $changeStream stage encounters an event
            // that invalidates the cursor. We should close the cursor and return without
            // error.
            cursor = nullptr;
            break;
        }

        if (state == PlanExecutor::IS_EOF) {
            responseBuilder.setLatestOplogTimestamp(
                cursor->getExecutor()->getLatestOplogTimestamp());
            if (!cursor->isTailable()) {
                // make it an obvious error to use cursor or executor after this point
                cursor = nullptr;
            }
            break;
        }

        if (PlanExecutor::ADVANCED != state) {
            uassertStatusOK(WorkingSetCommon::getMemberObjectStatus(next).withContext(
                "PlanExecutor error during aggregation"));
        }

        // If adding this object will cause us to exceed the message size limit, then we stash it
        // for later.
        if (!FindCommon::haveSpaceForNext(next, objCount, responseBuilder.bytesUsed())) {
            cursor->getExecutor()->enqueue(next);
            break;
        }

        responseBuilder.setLatestOplogTimestamp(cursor->getExecutor()->getLatestOplogTimestamp());
        responseBuilder.append(next);
    }

    if (cursor) {
        // If a time limit was set on the pipeline, remaining time is "rolled over" to the
        // cursor (for use by future getmore ops).
        cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

        CurOp::get(opCtx)->debug().cursorid = cursor->cursorid();

        // Cursor needs to be in a saved state while we yield locks for getmore. State
        // will be restored in getMore().
        cursor->getExecutor()->saveState();
        cursor->getExecutor()->detachFromOperationContext();
    } else {
        CurOp::get(opCtx)->debug().cursorExhausted = true;
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

    // We intentionally do not drop and reacquire our DB lock after resolving the view definition in
    // order to prevent the definition for any view namespaces we've already resolved from changing.
    // This is necessary to prevent a cycle from being formed among the view definitions cached in
    // 'resolvedNamespaces' because we won't re-resolve a view namespace we've already encountered.
    AutoGetDb autoDb(opCtx, request.getNamespaceString().db(), MODE_IS);
    Database* const db = autoDb.getDb();
    ViewCatalog* viewCatalog = db ? db->getViewCatalog() : nullptr;

    std::deque<NamespaceString> involvedNamespacesQueue(pipelineInvolvedNamespaces.begin(),
                                                        pipelineInvolvedNamespaces.end());
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;

    while (!involvedNamespacesQueue.empty()) {
        auto involvedNs = std::move(involvedNamespacesQueue.front());
        involvedNamespacesQueue.pop_front();

        if (resolvedNamespaces.find(involvedNs.coll()) != resolvedNamespaces.end()) {
            continue;
        }

        if (involvedNs.db() != request.getNamespaceString().db()) {
            // If the involved namespace is not in the same database as the aggregation, it must be
            // from a $out to a collection in a different database. Since we cannot write to views,
            // simply assume that the namespace is a collection.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
        } else if (!db || db->getCollection(opCtx, involvedNs)) {
            // If the aggregation database exists and 'involvedNs' refers to a collection namespace,
            // then we resolve it as an empty pipeline in order to read directly from the underlying
            // collection. If the database doesn't exist, then we still resolve it as an empty
            // pipeline because 'involvedNs' doesn't refer to a view namespace in our consistent
            // snapshot of the view catalog.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
        } else if (viewCatalog->lookup(opCtx, involvedNs.ns())) {
            // If 'involvedNs' refers to a view namespace, then we resolve its definition.
            auto resolvedView = viewCatalog->resolveView(opCtx, involvedNs);
            if (!resolvedView.isOK()) {
                return {ErrorCodes::FailedToParse,
                        str::stream() << "Failed to resolve view '" << involvedNs.ns() << "': "
                                      << resolvedView.getStatus().toString()};
            }

            resolvedNamespaces[involvedNs.coll()] = {resolvedView.getValue().getNamespace(),
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
                                      const Pipeline* pipeline) {
    if (!db || !pipeline) {
        return Status::OK();
    }
    for (auto&& potentialViewNs : pipeline->getInvolvedCollections()) {
        if (db->getCollection(opCtx, potentialViewNs)) {
            continue;
        }

        auto view = db->getViewCatalog()->lookup(opCtx, potentialViewNs.ns());
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
    auto txnParticipant = TransactionParticipant::get(opCtx);
    expCtx->inMultiDocumentTransaction =
        txnParticipant && txnParticipant->inMultiDocumentTransaction();

    return expCtx;
}
}  // namespace

Status runAggregate(OperationContext* opCtx,
                    const NamespaceString& origNss,
                    const AggregationRequest& request,
                    const BSONObj& cmdObj,
                    rpc::ReplyBuilderInterface* result) {
    // For operations on views, this will be the underlying namespace.
    NamespaceString nss = request.getNamespaceString();

    // The collation to use for this aggregation. boost::optional to distinguish between the case
    // where the collation has not yet been resolved, and where it has been resolved to nullptr.
    boost::optional<std::unique_ptr<CollatorInterface>> collatorToUse;

    // The UUID of the collection for the execution namespace of this aggregation. For change
    // streams, this will be the UUID of the original namespace instead of the oplog namespace.
    boost::optional<UUID> uuid;

    std::vector<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs;
    boost::intrusive_ptr<ExpressionContext> expCtx;
    auto curOp = CurOp::get(opCtx);
    {
        const LiteParsedPipeline liteParsedPipeline(request);

        try {
            // Check whether the parsed pipeline supports the given read concern.
            liteParsedPipeline.assertSupportsReadConcern(opCtx, request.getExplain());
        } catch (const DBException& ex) {
            auto txnParticipant = TransactionParticipant::get(opCtx);
            // If we are in a multi-document transaction, we intercept the 'readConcern'
            // assertion in order to provide a more descriptive error message and code.
            if (txnParticipant && txnParticipant->inMultiDocumentTransaction()) {
                return {ErrorCodes::OperationNotSupportedInTransaction,
                        ex.toStatus("Operation not permitted in transaction").reason()};
            }
            return ex.toStatus();
        }

        if (liteParsedPipeline.hasChangeStream()) {
            nss = NamespaceString::kRsOplogNamespace;

            // If the read concern is not specified, upgrade to 'majority' and wait to make sure
            // we have a snapshot available.
            auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            if (!readConcernArgs.hasLevel()) {
                {
                    // We must obtain the client lock to set the ReadConcernArgs on the operation
                    // context as it may be concurrently read by CurrentOp.
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    readConcernArgs =
                        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);
                }
                uassertStatusOK(waitForReadConcern(opCtx, readConcernArgs, true));
            }

            if (!origNss.isCollectionlessAggregateNS()) {
                // AutoGetCollectionForReadCommand will raise an error if 'origNss' is a view.
                AutoGetCollectionForReadCommand origNssCtx(opCtx, origNss);

                // Resolve the collator to either the user-specified collation or the default
                // collation of the collection on which $changeStream was invoked, so that we do not
                // end up resolving the collation on the oplog.
                invariant(!collatorToUse);
                Collection* origColl = origNssCtx.getCollection();
                collatorToUse.emplace(resolveCollator(opCtx, request, origColl));

                // Get the collection UUID to be set on the expression context.
                uuid = origColl ? origColl->uuid() : boost::none;
            }
        }

        const auto& pipelineInvolvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

        // If emplaced, AutoGetCollectionForReadCommand will throw if the sharding version for this
        // connection is out of date. If the namespace is a view, the lock will be released before
        // re-running the expanded aggregation.
        boost::optional<AutoGetCollectionForReadCommand> ctx;

        // If this is a collectionless aggregation, we won't create 'ctx' but will still need an
        // AutoStatsTracker to record CurOp and Top entries.
        boost::optional<AutoStatsTracker> statsTracker;

        // If this is a collectionless aggregation with no foreign namespaces, we don't want to
        // acquire any locks. Otherwise, lock the collection or view.
        if (nss.isCollectionlessAggregateNS() && pipelineInvolvedNamespaces.empty()) {
            statsTracker.emplace(opCtx,
                                 nss,
                                 Top::LockType::NotLocked,
                                 AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                                 0);
        } else {
            ctx.emplace(opCtx, nss, AutoGetCollection::ViewMode::kViewsPermitted);
        }

        Collection* collection = ctx ? ctx->getCollection() : nullptr;

        // For change streams, the UUID will already have been set for the original namespace.
        if (!liteParsedPipeline.hasChangeStream()) {
            uuid = collection ? collection->uuid() : boost::none;
        }

        // The collator may already have been set if this is a $changeStream pipeline. If not,
        // resolve the collator to either the user-specified collation or the collection default.
        if (!collatorToUse) {
            collatorToUse.emplace(resolveCollator(opCtx, request, collection));
        }

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
                uassertStatusOK(ctx->getDb()->getViewCatalog()->resolveView(opCtx, nss));
            uassert(std::move(resolvedView),
                    "On sharded systems, resolved views must be executed by mongos",
                    !ShardingState::get(opCtx)->enabled());

            // With the view & collation resolved, we can relinquish locks.
            ctx.reset();

            // Parse the resolved view into a new aggregation request.
            auto newRequest = resolvedView.asExpandedViewAggregation(request);
            auto newCmd = newRequest.serializeToCommandObj().toBson();

            auto status = runAggregate(opCtx, origNss, newRequest, newCmd, result);

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

        auto pipeline = uassertStatusOK(Pipeline::parse(request.getPipeline(), expCtx));

        // Check that the view's collation matches the collation of any views involved in the
        // pipeline.
        if (!pipelineInvolvedNamespaces.empty()) {
            invariant(ctx);
            auto pipelineCollationStatus = collatorCompatibleWithPipeline(
                opCtx, ctx->getDb(), expCtx->getCollator(), pipeline.get());
            if (!pipelineCollationStatus.isOK()) {
                return pipelineCollationStatus;
            }
        }

        pipeline->optimizePipeline();

        // Prepare a PlanExecutor to provide input into the pipeline, if needed.
        if (liteParsedPipeline.hasChangeStream()) {
            // If we are using a change stream, the cursor stage should have a simple collation,
            // regardless of what the user's collation was.
            std::unique_ptr<CollatorInterface> collatorForCursor = nullptr;
            auto collatorStash = expCtx->temporarilyChangeCollator(std::move(collatorForCursor));
            PipelineD::prepareCursorSource(collection, nss, &request, pipeline.get());
        } else {
            PipelineD::prepareCursorSource(collection, nss, &request, pipeline.get());
        }
        // Optimize again, since there may be additional optimizations that can be done after adding
        // the initial cursor stage. Note this has to be done outside the above blocks to ensure
        // this process uses the correct collation if it does any string comparisons.
        pipeline->optimizePipeline();

        std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> pipelines;

        if (request.getExchangeSpec() && !expCtx->explain) {
            boost::intrusive_ptr<Exchange> exchange =
                new Exchange(request.getExchangeSpec().get(), std::move(pipeline));

            for (size_t idx = 0; idx < exchange->getConsumers(); ++idx) {
                // For every new pipeline we have create a new ExpressionContext as the context
                // cannot be shared between threads. There is no synchronization for pieces of the
                // execution machinery above the Exchange, so nothing above the Exchange can be
                // shared between different exchange-producer cursors.
                expCtx = makeExpressionContext(
                    opCtx,
                    request,
                    expCtx->getCollator() ? expCtx->getCollator()->clone() : nullptr,
                    uuid);

                // Create a new pipeline for the consumer consisting of a single
                // DocumentSourceExchange.
                boost::intrusive_ptr<DocumentSource> consumer =
                    new DocumentSourceExchange(expCtx, exchange, idx);
                pipelines.emplace_back(uassertStatusOK(Pipeline::create({consumer}, expCtx)));
            }
        } else {
            pipelines.emplace_back(std::move(pipeline));
        }

        for (size_t idx = 0; idx < pipelines.size(); ++idx) {
            // Transfer ownership of the Pipeline to the PipelineProxyStage.
            auto ws = make_unique<WorkingSet>();
            auto proxy =
                make_unique<PipelineProxyStage>(opCtx, std::move(pipelines[idx]), ws.get());

            // This PlanExecutor will simply forward requests to the Pipeline, so does not need to
            // yield or to be registered with any collection's CursorManager to receive
            // invalidations. The Pipeline may contain PlanExecutors which *are* yielding
            // PlanExecutors and which *are* registered with their respective collection's
            // CursorManager

            auto statusWithPlanExecutor = PlanExecutor::make(
                opCtx, std::move(ws), std::move(proxy), nss, PlanExecutor::NO_YIELD);
            invariant(statusWithPlanExecutor.isOK());
            execs.emplace_back(std::move(statusWithPlanExecutor.getValue()));
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

    ScopeGuard cursorFreer = MakeGuard(
        [](std::vector<ClientCursorPin>* pins) {
            for (auto& p : *pins) {
                p.deleteUnderlying();
            }
        },
        &pins);

    for (size_t idx = 0; idx < execs.size(); ++idx) {
        ClientCursorParams cursorParams(
            std::move(execs[idx]),
            origNss,
            AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
            repl::ReadConcernArgs::get(opCtx),
            cmdObj);
        if (expCtx->tailableMode == TailableModeEnum::kTailable) {
            cursorParams.setTailable(true);
        } else if (expCtx->tailableMode == TailableModeEnum::kTailableAndAwaitData) {
            cursorParams.setTailable(true);
            cursorParams.setAwaitData(true);
        }

        auto pin =
            CursorManager::getGlobalCursorManager()->registerCursor(opCtx, std::move(cursorParams));
        cursors.emplace_back(pin.getCursor());
        pins.emplace_back(std::move(pin));
    }

    // If both explain and cursor are specified, explain wins.
    if (expCtx->explain) {
        auto bodyBuilder = result->getBodyBuilder();
        Explain::explainPipelineExecutor(
            pins[0].getCursor()->getExecutor(), *(expCtx->explain), &bodyBuilder);
    } else {
        // Cursor must be specified, if explain is not.
        const bool keepCursor =
            handleCursorCommand(opCtx, origNss, std::move(cursors), request, result);
        if (keepCursor) {
            cursorFreer.Dismiss();
        }
    }

    if (!expCtx->explain) {
        PlanSummaryStats stats;
        Explain::getSummaryStats(*(pins[0].getCursor()->getExecutor()), &stats);
        curOp->debug().setPlanSummaryMetrics(stats);
        curOp->debug().nreturned = stats.nReturned;
    }

    // Any code that needs the cursor pinned must be inside the try block, above.
    return Status::OK();
}

}  // namespace mongo
