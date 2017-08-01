/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
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
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/db/views/view_sharding_check.h"
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
                         ClientCursor* cursor,
                         const AggregationRequest& request,
                         BSONObjBuilder& result) {
    invariant(cursor);

    long long batchSize = request.getBatchSize();

    // can't use result BSONObjBuilder directly since it won't handle exceptions correctly.
    BSONArrayBuilder resultsArray;
    BSONObj next;
    for (int objCount = 0; objCount < batchSize; objCount++) {
        // The initial getNext() on a PipelineProxyStage may be very expensive so we don't
        // do it when batchSize is 0 since that indicates a desire for a fast return.
        PlanExecutor::ExecState state;
        if ((state = cursor->getExecutor()->getNext(&next, nullptr)) == PlanExecutor::IS_EOF) {
            if (!cursor->isTailable()) {
                // make it an obvious error to use cursor or executor after this point
                cursor = nullptr;
            }
            break;
        }

        if (PlanExecutor::ADVANCED != state) {
            auto status = WorkingSetCommon::getMemberObjectStatus(next);
            uasserted(status.code(),
                      "PlanExecutor error during aggregation: " +
                          WorkingSetCommon::toStatusString(next));
        }

        // If adding this object will cause us to exceed the message size limit, then we stash it
        // for later.
        if (!FindCommon::haveSpaceForNext(next, objCount, resultsArray.len())) {
            cursor->getExecutor()->enqueue(next);
            break;
        }

        resultsArray.append(next);
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
    appendCursorResponseObject(cursorId, nsForCursor.ns(), resultsArray.arr(), &result);

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

        if (!db || db->getCollection(opCtx, involvedNs)) {
            // If the database exists and 'involvedNs' refers to a collection namespace, then we
            // resolve it as an empty pipeline in order to read directly from the underlying
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
 * Round trips the pipeline through serialization by calling serialize(), then Pipeline::parse().
 * fasserts if it fails to parse after being serialized.
 */
std::unique_ptr<Pipeline, Pipeline::Deleter> reparsePipeline(
    const Pipeline* pipeline,
    const AggregationRequest& request,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto serialized = pipeline->serialize();

    // Convert vector<Value> to vector<BSONObj>.
    std::vector<BSONObj> parseableSerialization;
    parseableSerialization.reserve(serialized.size());
    for (auto&& serializedStage : serialized) {
        invariant(serializedStage.getType() == BSONType::Object);
        parseableSerialization.push_back(serializedStage.getDocument().toBson());
    }

    auto reparsedPipeline = Pipeline::parse(parseableSerialization, expCtx);
    if (!reparsedPipeline.isOK()) {
        error() << "Aggregation command did not round trip through parsing and serialization "
                   "correctly. Input pipeline: "
                << Value(request.getPipeline()) << ", serialized pipeline: " << Value(serialized);
        fassertFailedWithStatusNoTrace(40175, reparsedPipeline.getStatus());
    }

    reparsedPipeline.getValue()->optimizePipeline();
    return std::move(reparsedPipeline.getValue());
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
}  // namespace

Status runAggregate(OperationContext* opCtx,
                    const NamespaceString& origNss,
                    const AggregationRequest& request,
                    const BSONObj& cmdObj,
                    BSONObjBuilder& result) {
    // For operations on views, this will be the underlying namespace.
    NamespaceString nss = request.getNamespaceString();

    // Parse the user-specified collation, if any.
    std::unique_ptr<CollatorInterface> userSpecifiedCollator = request.getCollation().isEmpty()
        ? nullptr
        : uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                              ->makeFromBSON(request.getCollation()));

    unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
    boost::intrusive_ptr<ExpressionContext> expCtx;
    Pipeline* unownedPipeline;
    auto curOp = CurOp::get(opCtx);
    {
        const LiteParsedPipeline liteParsedPipeline(request);
        if (liteParsedPipeline.hasChangeNotification()) {
            nss = NamespaceString::kRsOplogNamespace;
        }

        const auto& pipelineInvolvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

        // If emplaced, AutoGetCollectionOrViewForReadCommand will throw if the sharding version for
        // this connection is out of date. If the namespace is a view, the lock will be released
        // before re-running the expanded aggregation.
        boost::optional<AutoGetCollectionOrViewForReadCommand> ctx;

        // If this is a collectionless aggregation, we won't create 'ctx' but will still need an
        // AutoStatsTracker to record CurOp and Top entries.
        boost::optional<AutoStatsTracker> statsTracker;

        // If this is a collectionless aggregation with no foreign namespaces, we don't want to
        // acquire any locks. Otherwise, lock the collection or view.
        if (nss.isCollectionlessAggregateNS() && pipelineInvolvedNamespaces.empty()) {
            statsTracker.emplace(opCtx, nss, Top::LockType::NotLocked, 0);
        } else {
            ctx.emplace(opCtx, nss);
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
            // collation. The check is skipped if the 'request' has the empty collation, which
            // means that no collation was specified.
            if (!request.getCollation().isEmpty()) {
                if (!CollatorInterface::collatorsMatch(ctx->getView()->defaultCollator(),
                                                       userSpecifiedCollator.get())) {
                    return {ErrorCodes::OptionNotSupportedOnView,
                            "Cannot override a view's default collation"};
                }
            }

            auto viewDefinition =
                ViewShardingCheck::getResolvedViewIfSharded(opCtx, ctx->getDb(), ctx->getView());
            if (!viewDefinition.isOK()) {
                return viewDefinition.getStatus();
            }

            if (!viewDefinition.getValue().isEmpty()) {
                return ViewShardingCheck::appendShardedViewResponse(viewDefinition.getValue(),
                                                                    &result);
            }

            auto resolvedView = ctx->getDb()->getViewCatalog()->resolveView(opCtx, nss);
            if (!resolvedView.isOK()) {
                return resolvedView.getStatus();
            }

            auto collationSpec = ctx->getView()->defaultCollator()
                ? ctx->getView()->defaultCollator()->getSpec().toBSON().getOwned()
                : CollationSpec::kSimpleSpec;

            // With the view & collation resolved, we can relinquish locks.
            ctx->releaseLocksForView();

            // Parse the resolved view into a new aggregation request.
            auto newRequest = resolvedView.getValue().asExpandedViewAggregation(request);
            newRequest.setCollation(collationSpec);
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

        // Determine the appropriate collation to make the ExpressionContext.

        // If the pipeline does not have a user-specified collation, set it from the collection
        // default. Be careful to consult the original request BSON to check if a collation was
        // specified, since a specification of {locale: "simple"} will result in a null
        // collator.
        auto collatorToUse = std::move(userSpecifiedCollator);
        if (request.getCollation().isEmpty() && collection && collection->getDefaultCollator()) {
            invariant(!collatorToUse);
            collatorToUse = collection->getDefaultCollator()->clone();
        }

        expCtx.reset(
            new ExpressionContext(opCtx,
                                  request,
                                  std::move(collatorToUse),
                                  uassertStatusOK(resolveInvolvedNamespaces(opCtx, request))));
        expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";

        if (liteParsedPipeline.hasChangeNotification()) {
            expCtx->tailableMode = ExpressionContext::TailableMode::kTailableAndAwaitData;
        }

        // Parse the pipeline.
        auto statusWithPipeline = Pipeline::parse(request.getPipeline(), expCtx);
        if (!statusWithPipeline.isOK()) {
            return statusWithPipeline.getStatus();
        }
        auto pipeline = std::move(statusWithPipeline.getValue());

        // Check that the view's collation matches the collation of any views involved
        // in the pipeline.
        if (!pipelineInvolvedNamespaces.empty()) {
            invariant(ctx);
            auto pipelineCollationStatus = collatorCompatibleWithPipeline(
                opCtx, ctx->getDb(), expCtx->getCollator(), pipeline.get());
            if (!pipelineCollationStatus.isOK()) {
                return pipelineCollationStatus;
            }
        }

        pipeline->optimizePipeline();

        if (kDebugBuild && !expCtx->explain && !expCtx->inShard) {
            // Make sure all operations round-trip through Pipeline::serialize() correctly by
            // re-parsing every command in debug builds. This is important because sharded
            // aggregations rely on this ability.  Skipping when inShard because this has
            // already been through the transformation (and this un-sets expCtx->inShard).
            pipeline = reparsePipeline(pipeline.get(), request, expCtx);
        }

        // This does mongod-specific stuff like creating the input PlanExecutor and adding
        // it to the front of the pipeline if needed.
        PipelineD::prepareCursorSource(collection, nss, &request, pipeline.get());

        // Transfer ownership of the Pipeline to the PipelineProxyStage.
        unownedPipeline = pipeline.get();
        auto ws = make_unique<WorkingSet>();
        auto proxy = make_unique<PipelineProxyStage>(opCtx, std::move(pipeline), ws.get());

        // This PlanExecutor will simply forward requests to the Pipeline, so does not need to
        // yield or to be registered with any collection's CursorManager to receive invalidations.
        // The Pipeline may contain PlanExecutors which *are* yielding PlanExecutors and which *are*
        // registered with their respective collection's CursorManager
        auto statusWithPlanExecutor =
            PlanExecutor::make(opCtx, std::move(ws), std::move(proxy), nss, PlanExecutor::NO_YIELD);
        invariant(statusWithPlanExecutor.isOK());
        exec = std::move(statusWithPlanExecutor.getValue());

        {
            auto planSummary = Explain::getPlanSummary(exec.get());
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setPlanSummary_inlock(std::move(planSummary));
        }
    }

    // Having released the collection lock, we can now create a cursor that returns results from the
    // pipeline. This cursor owns no collection state, and thus we register it with the global
    // cursor manager. The global cursor manager does not deliver invalidations or kill
    // notifications; the underlying PlanExecutor(s) used by the pipeline will be receiving
    // invalidations and kill notifications themselves, not the cursor we create here.
    ClientCursorParams cursorParams(
        std::move(exec),
        origNss,
        AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
        opCtx->recoveryUnit()->isReadingFromMajorityCommittedSnapshot(),
        cmdObj);
    if (expCtx->tailableMode == ExpressionContext::TailableMode::kTailableAndAwaitData) {
        cursorParams.setTailable(true);
        cursorParams.setAwaitData(true);
    }

    auto pin =
        CursorManager::getGlobalCursorManager()->registerCursor(opCtx, std::move(cursorParams));

    ScopeGuard cursorFreer = MakeGuard(&ClientCursorPin::deleteUnderlying, &pin);

    // If both explain and cursor are specified, explain wins.
    if (expCtx->explain) {
        result << "stages" << Value(unownedPipeline->writeExplainOps(*expCtx->explain));
    } else {
        // Cursor must be specified, if explain is not.
        const bool keepCursor =
            handleCursorCommand(opCtx, origNss, pin.getCursor(), request, result);
        if (keepCursor) {
            cursorFreer.Dismiss();
        }
    }

    if (!expCtx->explain) {
        PlanSummaryStats stats;
        Explain::getSummaryStats(*(pin.getCursor()->getExecutor()), &stats);
        curOp->debug().setPlanSummaryMetrics(stats);
        curOp->debug().nreturned = stats.nReturned;
    }

    // Any code that needs the cursor pinned must be inside the try block, above.
    return Status::OK();
}

}  // namespace mongo
