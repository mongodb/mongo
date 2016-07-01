/**
 * Copyright (c) 2011-2014 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/pipeline_proxy.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

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
 * requests).  Otherwise, returns false.
 */
bool handleCursorCommand(OperationContext* txn,
                         const string& ns,
                         ClientCursorPin* pin,
                         PlanExecutor* exec,
                         const AggregationRequest& request,
                         BSONObjBuilder& result) {
    ClientCursor* cursor = pin ? pin->c() : NULL;
    if (pin) {
        invariant(cursor);
        invariant(cursor->getExecutor() == exec);
        invariant(cursor->isAggCursor());
    }

    invariant(request.getBatchSize());
    long long batchSize = request.getBatchSize().get();

    // can't use result BSONObjBuilder directly since it won't handle exceptions correctly.
    BSONArrayBuilder resultsArray;
    BSONObj next;
    for (int objCount = 0; objCount < batchSize; objCount++) {
        // The initial getNext() on a PipelineProxyStage may be very expensive so we don't
        // do it when batchSize is 0 since that indicates a desire for a fast return.
        PlanExecutor::ExecState state;
        if ((state = exec->getNext(&next, NULL)) == PlanExecutor::IS_EOF) {
            // make it an obvious error to use cursor or executor after this point
            cursor = NULL;
            exec = NULL;
            break;
        }

        uassert(34426,
                "Plan executor error during aggregation: " + WorkingSetCommon::toStatusString(next),
                PlanExecutor::ADVANCED == state);

        // If adding this object will cause us to exceed the message size limit, then we stash it
        // for later.
        if (!FindCommon::haveSpaceForNext(next, objCount, resultsArray.len())) {
            exec->enqueue(next);
            break;
        }

        resultsArray.append(next);
    }

    // NOTE: exec->isEOF() can have side effects such as writing by $out. However, it should
    // be relatively quick since if there was no pin then the input is empty. Also, this
    // violates the contract for batchSize==0. Sharding requires a cursor to be returned in that
    // case. This is ok for now however, since you can't have a sharded collection that doesn't
    // exist.
    const bool canReturnMoreBatches = pin;
    if (!canReturnMoreBatches && exec && !exec->isEOF()) {
        // msgasserting since this shouldn't be possible to trigger from today's aggregation
        // language. The wording assumes that the only reason pin would be null is if the
        // collection doesn't exist.
        msgasserted(
            17391,
            str::stream() << "Aggregation has more results than fit in initial batch, but can't "
                          << "create cursor since collection "
                          << ns
                          << " doesn't exist");
    }

    if (cursor) {
        // If a time limit was set on the pipeline, remaining time is "rolled over" to the
        // cursor (for use by future getmore ops).
        cursor->setLeftoverMaxTimeMicros(txn->getRemainingMaxTimeMicros());

        CurOp::get(txn)->debug().cursorid = cursor->cursorid();

        // Cursor needs to be in a saved state while we yield locks for getmore. State
        // will be restored in getMore().
        exec->saveState();
        exec->detachFromOperationContext();
    } else {
        CurOp::get(txn)->debug().cursorExhausted = true;
    }

    const long long cursorId = cursor ? cursor->cursorid() : 0LL;
    appendCursorResponseObject(cursorId, ns, resultsArray.arr(), &result);

    return static_cast<bool>(cursor);
}

/**
 * Round trips the pipeline through serialization by calling serialize(), then Pipeline::parse().
 * fasserts if it fails to parse after being serialized.
 */
boost::intrusive_ptr<Pipeline> reparsePipeline(
    const boost::intrusive_ptr<Pipeline>& pipeline,
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
                << Value(request.getPipeline()).toString()
                << ", serialized pipeline: " << Value(serialized).toString();
        fassertFailedWithStatusNoTrace(40175, reparsedPipeline.getStatus());
    }

    reparsedPipeline.getValue()->injectExpressionContext(expCtx);
    reparsedPipeline.getValue()->optimizePipeline();
    return reparsedPipeline.getValue();
}

class PipelineCommand : public Command {
public:
    PipelineCommand()
        : Command(AggregationRequest::kCommandName) {}  // command is called "aggregate"

    // Locks are managed manually, in particular by DocumentSourceCursor.
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return Pipeline::aggSupportsWriteConcern(cmd);
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    bool supportsReadConcern() const final {
        return true;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

    virtual void help(stringstream& help) const {
        help << "{ pipeline: [ { $operator: {...}}, ... ]"
             << ", explain: <bool>"
             << ", allowDiskUse: <bool>"
             << ", cursor: {batchSize: <number>}"
             << " }" << endl
             << "See http://dochub.mongodb.org/core/aggregation for more details.";
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        return Pipeline::checkAuthForCommand(client, dbname, cmdObj);
    }

    virtual bool run(OperationContext* txn,
                     const string& db,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result) {
        const std::string ns = parseNs(db, cmdObj);
        if (nsToCollectionSubstring(ns).empty()) {
            errmsg = "missing collection name";
            return false;
        }
        NamespaceString nss(ns);

        // Parse the options for this request.
        auto request = AggregationRequest::parseFromBSON(nss, cmdObj);
        if (!request.isOK()) {
            return appendCommandStatus(result, request.getStatus());
        }

        // Set up the ExpressionContext.
        intrusive_ptr<ExpressionContext> expCtx = new ExpressionContext(txn, request.getValue());
        expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";

        // Parse the pipeline.
        auto statusWithPipeline = Pipeline::parse(request.getValue().getPipeline(), expCtx);
        if (!statusWithPipeline.isOK()) {
            return appendCommandStatus(result, statusWithPipeline.getStatus());
        }
        auto pipeline = std::move(statusWithPipeline.getValue());

        unique_ptr<ClientCursorPin> pin;  // either this OR the exec will be non-null
        unique_ptr<PlanExecutor> exec;
        auto curOp = CurOp::get(txn);
        {
            // This will throw if the sharding version for this connection is out of date. If the
            // namespace is a view, the lock will be released before re-running the aggregation.
            // Otherwise, the lock must be held continuously from now until we have we created both
            // the output ClientCursor and the input executor. This ensures that both are using the
            // same sharding version that we synchronize on here. This is also why we always need to
            // create a ClientCursor even when we aren't outputting to a cursor. See the comment on
            // ShardFilterStage for more details.
            AutoGetCollectionOrViewForRead ctx(txn, nss);
            Collection* collection = ctx.getCollection();

            // If this is a view, resolve it by finding the underlying collection and stitching view
            // pipelines and this request's pipeline together. We then release our locks before
            // recursively calling run, which will re-acquire locks on the underlying collection.
            // (The lock must be released because recursively acquiring locks on the database will
            // prohibit yielding.)
            if (ctx.getView()) {
                auto resolvedView = ctx.getDb()->getViewCatalog()->resolveView(txn, nss);
                if (!resolvedView.isOK()) {
                    return appendCommandStatus(result, resolvedView.getStatus());
                }

                // With the view resolved, we can relinquish locks.
                ctx.releaseLocksForView();

                // Parse the resolved view into a new aggregation request.
                BSONObj viewCmd =
                    resolvedView.getValue().asExpandedViewAggregation(request.getValue());

                return this->run(txn, db, viewCmd, options, errmsg, result);
            }

            // If the pipeline does not have a user-specified collation, set it from the collection
            // default.
            if (request.getValue().getCollation().isEmpty() && collection &&
                collection->getDefaultCollator()) {
                invariant(!expCtx->getCollator());
                expCtx->setCollator(collection->getDefaultCollator()->clone());
            }

            // Propagate the ExpressionContext throughout all of the pipeline's stages and
            // expressions.
            pipeline->injectExpressionContext(expCtx);

            // The pipeline must be optimized after the correct collator has been set on it (by
            // injecting the ExpressionContext containing the collator). This is necessary because
            // optimization may make string comparisons, e.g. optimizing {$eq: [<str1>, <str2>]} to
            // a constant.
            pipeline->optimizePipeline();

            if (kDebugBuild && !expCtx->isExplain && !expCtx->inShard) {
                // Make sure all operations round-trip through Pipeline::serialize() correctly by
                // re-parsing every command in debug builds. This is important because sharded
                // aggregations rely on this ability.  Skipping when inShard because this has
                // already been through the transformation (and this un-sets expCtx->inShard).
                pipeline = reparsePipeline(pipeline, request.getValue(), expCtx);
            }

            // This does mongod-specific stuff like creating the input PlanExecutor and adding
            // it to the front of the pipeline if needed.
            std::shared_ptr<PlanExecutor> input =
                PipelineD::prepareCursorSource(txn, collection, nss, pipeline, expCtx);

            // Create the PlanExecutor which returns results from the pipeline. The WorkingSet
            // ('ws') and the PipelineProxyStage ('proxy') will be owned by the created
            // PlanExecutor.
            auto ws = make_unique<WorkingSet>();
            auto proxy = make_unique<PipelineProxyStage>(txn, pipeline, input, ws.get());

            auto statusWithPlanExecutor = (NULL == collection)
                ? PlanExecutor::make(
                      txn, std::move(ws), std::move(proxy), nss.ns(), PlanExecutor::YIELD_MANUAL)
                : PlanExecutor::make(
                      txn, std::move(ws), std::move(proxy), collection, PlanExecutor::YIELD_MANUAL);
            invariant(statusWithPlanExecutor.isOK());
            exec = std::move(statusWithPlanExecutor.getValue());

            {
                auto planSummary = Explain::getPlanSummary(exec.get());
                stdx::lock_guard<Client>(*txn->getClient());
                curOp->setPlanSummary_inlock(std::move(planSummary));
            }

            if (collection) {
                PlanSummaryStats stats;
                Explain::getSummaryStats(*exec, &stats);
                collection->infoCache()->notifyOfQuery(txn, stats.indexesUsed);
            }

            if (collection) {
                const bool isAggCursor = true;  // enable special locking behavior
                ClientCursor* cursor =
                    new ClientCursor(collection->getCursorManager(),
                                     exec.release(),
                                     nss.ns(),
                                     txn->recoveryUnit()->isReadingFromMajorityCommittedSnapshot(),
                                     0,
                                     cmdObj.getOwned(),
                                     isAggCursor);
                pin.reset(new ClientCursorPin(collection->getCursorManager(), cursor->cursorid()));
                // Don't add any code between here and the start of the try block.
            }

            // At this point, it is safe to release the collection lock.
            // - In the case where we have a collection: we will need to reacquire the
            //   collection lock later when cleaning up our ClientCursorPin.
            // - In the case where we don't have a collection: our PlanExecutor won't be
            //   registered, so it will be safe to clean it up outside the lock.
            invariant(!exec || !collection);
        }

        try {
            // Unless set to true, the ClientCursor created above will be deleted on block exit.
            bool keepCursor = false;

            // Use of the aggregate command without specifying to use a cursor is deprecated.
            // Applications should migrate to using cursors. Cursors are strictly more useful than
            // outputting the results as a single document, since results that fit inside a single
            // BSONObj will also fit inside a single batch.
            //
            // We occasionally log a deprecation warning.
            if (!request.getValue().isCursorCommand()) {
                RARELY {
                    warning()
                        << "Use of the aggregate command without the 'cursor' "
                           "option is deprecated. See "
                           "http://dochub.mongodb.org/core/aggregate-without-cursor-deprecation.";
                }
            }

            // If both explain and cursor are specified, explain wins.
            if (expCtx->isExplain) {
                result << "stages" << Value(pipeline->writeExplainOps());
            } else if (request.getValue().isCursorCommand()) {
                keepCursor = handleCursorCommand(txn,
                                                 nss.ns(),
                                                 pin.get(),
                                                 pin ? pin->c()->getExecutor() : exec.get(),
                                                 request.getValue(),
                                                 result);
            } else {
                pipeline->run(result);
            }

            if (!expCtx->isExplain) {
                PlanSummaryStats stats;
                Explain::getSummaryStats(pin ? *pin->c()->getExecutor() : *exec.get(), &stats);
                curOp->debug().setPlanSummaryMetrics(stats);
                curOp->debug().nreturned = stats.nReturned;
            }

            // Clean up our ClientCursorPin, if needed.  We must reacquire the collection lock
            // in order to do so.
            if (pin) {
                // We acquire locks here with DBLock and CollectionLock instead of using
                // AutoGetCollectionForRead.  AutoGetCollectionForRead will throw if the
                // sharding version is out of date, and we don't care if the sharding version
                // has changed.
                Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_IS);
                Lock::CollectionLock collLock(txn->lockState(), nss.ns(), MODE_IS);
                if (keepCursor) {
                    pin->release();
                } else {
                    pin->deleteUnderlying();
                }
            }
        } catch (...) {
            // On our way out of scope, we clean up our ClientCursorPin if needed.
            if (pin) {
                Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_IS);
                Lock::CollectionLock collLock(txn->lockState(), nss.ns(), MODE_IS);
                pin->deleteUnderlying();
            }
            throw;
        }
        // Any code that needs the cursor pinned must be inside the try block, above.

        return true;
    }
};

MONGO_INITIALIZER(PipelineCommand)(InitializerContext* context) {
    new PipelineCommand();

    return Status::OK();
}

}  // namespace
}  // namespace mongo
