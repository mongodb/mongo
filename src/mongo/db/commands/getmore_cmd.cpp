/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/chunk_version.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {
MONGO_FP_DECLARE(rsStopGetMoreCmd);
}  // namespace

/**
 * A command for running getMore() against an existing cursor registered with a CursorManager.
 * Used to generate the next batch of results for a ClientCursor.
 *
 * Can be used in combination with any cursor-generating command (e.g. find, aggregate,
 * listIndexes).
 */
class GetMoreCmd : public Command {
    MONGO_DISALLOW_COPYING(GetMoreCmd);

public:
    GetMoreCmd() : Command("getMore") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const override {
        return true;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsReadConcern() const final {
        // Uses the readConcern setting from whatever created the cursor.
        return false;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

    void help(std::stringstream& help) const override {
        help << "retrieve more results from an existing cursor";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opGetMore;
    }

    std::size_t reserveBytesForReply() const override {
        // The extra 1K is an artifact of how we construct batches. We consider a batch to be full
        // when it exceeds the goal batch size. In the case that we are just below the limit and
        // then read a large document, the extra 1K helps prevent a final realloc+memcpy.
        return FindCommon::kMaxBytesToReturnToClientAtOnce + 1024u;
    }

    /**
     * A getMore command increments the getMore counter, not the command counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return GetMoreRequest::parseNs(dbname, cmdObj);
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        StatusWith<GetMoreRequest> parseStatus = GetMoreRequest::parseFromBSON(dbname, cmdObj);
        if (!parseStatus.isOK()) {
            return parseStatus.getStatus();
        }
        const GetMoreRequest& request = parseStatus.getValue();

        return AuthorizationSession::get(client)->checkAuthForGetMore(
            request.nss, request.cursorid, request.term.is_initialized());
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        // Counted as a getMore, not as a command.
        globalOpCounters.gotGetMore();

        if (txn->getClient()->isInDirectClient()) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::IllegalOperation, "Cannot run getMore command from eval()"));
        }

        StatusWith<GetMoreRequest> parseStatus = GetMoreRequest::parseFromBSON(dbname, cmdObj);
        if (!parseStatus.isOK()) {
            return appendCommandStatus(result, parseStatus.getStatus());
        }
        const GetMoreRequest& request = parseStatus.getValue();

        auto curOp = CurOp::get(txn);
        curOp->debug().cursorid = request.cursorid;

        // Disable shard version checking - getmore commands are always unversioned
        OperationShardingState::get(txn).setShardVersion(request.nss, ChunkVersion::IGNORED());

        // Validate term before acquiring locks, if provided.
        if (request.term) {
            auto replCoord = repl::ReplicationCoordinator::get(txn);
            Status status = replCoord->updateTerm(txn, *request.term);
            // Note: updateTerm returns ok if term stayed the same.
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
        }

        // Depending on the type of cursor being operated on, we hold locks for the whole
        // getMore, or none of the getMore, or part of the getMore.  The three cases in detail:
        //
        // 1) Normal cursor: we lock with "ctx" and hold it for the whole getMore.
        // 2) Cursor owned by global cursor manager: we don't lock anything.  These cursors
        //    don't own any collection state. These cursors are generated either by the
        //    listCollections or listIndexes commands, as these special cursor-generating commands
        //    operate over catalog data rather than targeting the data within a collection.
        // 3) Agg cursor: we lock with "ctx", then release, then relock with "unpinDBLock" and
        //    "unpinCollLock".  This is because agg cursors handle locking internally (hence the
        //    release), but the pin and unpin of the cursor must occur under the collection
        //    lock. We don't use our AutoGetCollectionForRead "ctx" to relock, because
        //    AutoGetCollectionForRead checks the sharding version (and we want the relock for
        //    the unpin to succeed even if the sharding version has changed).
        //
        // Note that we declare our locks before our ClientCursorPin, in order to ensure that
        // the pin's destructor is called before the lock destructors (so that the unpin occurs
        // under the lock).
        std::unique_ptr<AutoGetCollectionForRead> ctx;
        std::unique_ptr<Lock::DBLock> unpinDBLock;
        std::unique_ptr<Lock::CollectionLock> unpinCollLock;

        CursorManager* cursorManager;
        if (request.nss.isListIndexesCursorNS() || request.nss.isListCollectionsCursorNS()) {
            cursorManager = CursorManager::getGlobalCursorManager();
        } else {
            ctx = stdx::make_unique<AutoGetCollectionForRead>(txn, request.nss);
            Collection* collection = ctx->getCollection();
            if (!collection) {
                return appendCommandStatus(result,
                                           Status(ErrorCodes::OperationFailed,
                                                  "collection dropped between getMore calls"));
            }
            cursorManager = collection->getCursorManager();
        }

        ClientCursorPin ccPin(cursorManager, request.cursorid);
        ClientCursor* cursor = ccPin.c();
        if (!cursor) {
            // We didn't find the cursor.
            return appendCommandStatus(
                result,
                Status(ErrorCodes::CursorNotFound,
                       str::stream() << "Cursor not found, cursor id: " << request.cursorid));
        }

        // If the fail point is enabled, busy wait until it is disabled. We unlock and re-acquire
        // the locks periodically in order to avoid deadlock (see SERVER-21997 for details).
        while (MONGO_FAIL_POINT(keepCursorPinnedDuringGetMore)) {
            invariant(ctx);
            invariant(!unpinDBLock);
            invariant(!unpinCollLock);
            sleepFor(Milliseconds(10));
            ctx.reset();
            ctx = stdx::make_unique<AutoGetCollectionForRead>(txn, request.nss);
        }

        if (request.nss.ns() != cursor->ns()) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::Unauthorized,
                       str::stream() << "Requested getMore on namespace '" << request.nss.ns()
                                     << "', but cursor belongs to a different namespace"));
        }

        if (request.nss.isOplog() && MONGO_FAIL_POINT(rsStopGetMoreCmd)) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::CommandFailed,
                       str::stream() << "getMore on " << request.nss.ns()
                                     << " rejected due to active fail point rsStopGetMoreCmd"));
        }

        // Validation related to awaitData.
        if (isCursorAwaitData(cursor)) {
            invariant(isCursorTailable(cursor));

            if (cursor->isAggCursor()) {
                Status status(ErrorCodes::BadValue,
                              "awaitData cannot be set on an aggregation cursor");
                return appendCommandStatus(result, status);
            }
        }

        if (request.awaitDataTimeout && !isCursorAwaitData(cursor)) {
            Status status(ErrorCodes::BadValue,
                          "cannot set maxTimeMS on getMore command for a non-awaitData cursor");
            return appendCommandStatus(result, status);
        }

        // On early return, get rid of the cursor.
        ScopeGuard cursorFreer = MakeGuard(&GetMoreCmd::cleanupCursor, txn, &ccPin, request);

        if (cursor->isReadCommitted())
            uassertStatusOK(txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot());

        // Reset timeout timer on the cursor since the cursor is still in use.
        cursor->setIdleTime(0);

        const bool hasOwnMaxTime = txn->hasDeadline();

        if (!hasOwnMaxTime) {
            // There is no time limit set directly on this getMore command. If the cursor is
            // awaitData, then we supply a default time of one second. Otherwise we roll over
            // any leftover time from the maxTimeMS of the operation that spawned this cursor,
            // applying it to this getMore.
            if (isCursorAwaitData(cursor)) {
                uassert(40117,
                        "Illegal attempt to set operation deadline within DBDirectClient",
                        !txn->getClient()->isInDirectClient());
                txn->setDeadlineAfterNowBy(Seconds{1});
            } else if (cursor->getLeftoverMaxTimeMicros() < Microseconds::max()) {
                uassert(40118,
                        "Illegal attempt to set operation deadline within DBDirectClient",
                        !txn->getClient()->isInDirectClient());
                txn->setDeadlineAfterNowBy(cursor->getLeftoverMaxTimeMicros());
            }
        }
        txn->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

        if (cursor->isAggCursor()) {
            // Agg cursors handle their own locking internally.
            ctx.reset();  // unlocks
        }

        PlanExecutor* exec = cursor->getExecutor();
        exec->reattachToOperationContext(txn);
        exec->restoreState();

        auto planSummary = Explain::getPlanSummary(exec);
        {
            stdx::lock_guard<Client>(*txn->getClient());
            curOp->setPlanSummary_inlock(planSummary);

            // Ensure that the original query or command object is available in the slow query log,
            // profiler and currentOp.
            auto originatingCommand = cursor->getQuery();
            if (!originatingCommand.isEmpty()) {
                curOp->setOriginatingCommand_inlock(originatingCommand);
            }
        }

        uint64_t notifierVersion = 0;
        std::shared_ptr<CappedInsertNotifier> notifier;
        if (isCursorAwaitData(cursor)) {
            invariant(ctx->getCollection()->isCapped());
            // Retrieve the notifier which we will wait on until new data arrives. We make sure
            // to do this in the lock because once we drop the lock it is possible for the
            // collection to become invalid. The notifier itself will outlive the collection if
            // the collection is dropped, as we keep a shared_ptr to it.
            notifier = ctx->getCollection()->getCappedInsertNotifier();

            // Must get the version before we call generateBatch in case a write comes in after
            // that call and before we call wait on the notifier.
            notifierVersion = notifier->getVersion();
        }

        CursorId respondWithId = 0;
        CursorResponseBuilder nextBatch(/*isInitialResponse*/ false, &result);
        BSONObj obj;
        PlanExecutor::ExecState state;
        long long numResults = 0;

        // We report keysExamined and docsExamined to OpDebug for a given getMore operation. To
        // obtain these values we need to take a diff of the pre-execution and post-execution
        // metrics, as they accumulate over the course of a cursor's lifetime.
        PlanSummaryStats preExecutionStats;
        Explain::getSummaryStats(*exec, &preExecutionStats);

        Status batchStatus = generateBatch(cursor, request, &nextBatch, &state, &numResults);
        if (!batchStatus.isOK()) {
            return appendCommandStatus(result, batchStatus);
        }

        // If this is an await data cursor, and we hit EOF without generating any results, then
        // we block waiting for new data to arrive.
        if (isCursorAwaitData(cursor) && state == PlanExecutor::IS_EOF && numResults == 0) {
            auto replCoord = repl::ReplicationCoordinator::get(txn);
            // Return immediately if we need to update the commit time.
            if (!request.lastKnownCommittedOpTime ||
                (request.lastKnownCommittedOpTime == replCoord->getLastCommittedOpTime())) {
                // Retrieve the notifier which we will wait on until new data arrives. We make sure
                // to do this in the lock because once we drop the lock it is possible for the
                // collection to become invalid. The notifier itself will outlive the collection if
                // the collection is dropped, as we keep a shared_ptr to it.
                auto notifier = ctx->getCollection()->getCappedInsertNotifier();

                // Save the PlanExecutor and drop our locks.
                exec->saveState();
                ctx.reset();

                // Block waiting for data.
                const auto timeout = txn->getRemainingMaxTimeMicros();
                notifier->wait(notifierVersion, timeout);
                notifier.reset();

                // Set expected latency to match wait time. This makes sure the logs aren't spammed
                // by awaitData queries that exceed slowms due to blocking on the
                // CappedInsertNotifier.
                curOp->setExpectedLatencyMs(durationCount<Milliseconds>(timeout));

                ctx.reset(new AutoGetCollectionForRead(txn, request.nss));
                exec->restoreState();

                // We woke up because either the timed_wait expired, or there was more data. Either
                // way, attempt to generate another batch of results.
                batchStatus = generateBatch(cursor, request, &nextBatch, &state, &numResults);
                if (!batchStatus.isOK()) {
                    return appendCommandStatus(result, batchStatus);
                }
            }
        }

        PlanSummaryStats postExecutionStats;
        Explain::getSummaryStats(*exec, &postExecutionStats);
        postExecutionStats.totalKeysExamined -= preExecutionStats.totalKeysExamined;
        postExecutionStats.totalDocsExamined -= preExecutionStats.totalDocsExamined;
        curOp->debug().setPlanSummaryMetrics(postExecutionStats);

        // We do not report 'execStats' for aggregation, both in the original request and
        // subsequent getMore. The reason for this is that aggregation's source PlanExecutor
        // could be destroyed before we know whether we need execStats and we do not want to
        // generate for all operations due to cost.
        if (!cursor->isAggCursor() && curOp->shouldDBProfile(curOp->elapsedMillis())) {
            BSONObjBuilder execStatsBob;
            Explain::getWinningPlanStats(exec, &execStatsBob);
            curOp->debug().execStats = execStatsBob.obj();
        }

        if (shouldSaveCursorGetMore(state, exec, isCursorTailable(cursor))) {
            respondWithId = request.cursorid;

            exec->saveState();
            exec->detachFromOperationContext();

            // If maxTimeMS was set directly on the getMore rather than being rolled over
            // from a previous find, then don't roll remaining micros over to the next
            // getMore.
            if (!hasOwnMaxTime) {
                cursor->setLeftoverMaxTimeMicros(txn->getRemainingMaxTimeMicros());
            }

            cursor->incPos(numResults);
        } else {
            curOp->debug().cursorExhausted = true;
        }

        nextBatch.done(respondWithId, request.nss.ns());

        // Ensure log and profiler include the number of results returned in this getMore's response
        // batch.
        curOp->debug().nreturned = numResults;

        if (respondWithId) {
            cursorFreer.Dismiss();

            // If we are operating on an aggregation cursor, then we dropped our collection lock
            // earlier and need to reacquire it in order to clean up our ClientCursorPin.
            if (cursor->isAggCursor()) {
                invariant(NULL == ctx.get());
                unpinDBLock.reset(new Lock::DBLock(txn->lockState(), request.nss.db(), MODE_IS));
                unpinCollLock.reset(
                    new Lock::CollectionLock(txn->lockState(), request.nss.ns(), MODE_IS));
            }
        }

        return true;
    }

    /**
     * Uses 'cursor' and 'request' to fill out 'nextBatch' with the batch of result documents to
     * be returned by this getMore.
     *
     * Returns the number of documents in the batch in *numResults, which must be initialized to
     * zero by the caller. Returns the final ExecState returned by the cursor in *state.
     *
     * Returns an OK status if the batch was successfully generated, and a non-OK status if the
     * PlanExecutor encounters a failure.
     */
    Status generateBatch(ClientCursor* cursor,
                         const GetMoreRequest& request,
                         CursorResponseBuilder* nextBatch,
                         PlanExecutor::ExecState* state,
                         long long* numResults) {
        PlanExecutor* exec = cursor->getExecutor();
        const bool isAwaitData = isCursorAwaitData(cursor);

        // If an awaitData getMore is killed during this process due to our max time expiring at
        // an interrupt point, we just continue as normal and return rather than reporting a
        // timeout to the user.
        BSONObj obj;
        try {
            while (!FindCommon::enoughForGetMore(request.batchSize.value_or(0), *numResults) &&
                   PlanExecutor::ADVANCED == (*state = exec->getNext(&obj, NULL))) {
                // If adding this object will cause us to exceed the message size limit, then we
                // stash it for later.
                if (!FindCommon::haveSpaceForNext(obj, *numResults, nextBatch->bytesUsed())) {
                    exec->enqueue(obj);
                    break;
                }

                // Add result to output buffer.
                nextBatch->append(obj);
                (*numResults)++;
            }
        } catch (const UserException& except) {
            if (isAwaitData && except.getCode() == ErrorCodes::ExceededTimeLimit) {
                // We ignore exceptions from interrupt points due to max time expiry for
                // awaitData cursors.
            } else {
                throw;
            }
        }

        if (PlanExecutor::FAILURE == *state || PlanExecutor::DEAD == *state) {
            nextBatch->abandon();

            error() << "GetMore command executor error: " << PlanExecutor::statestr(*state)
                    << ", stats: " << Explain::getWinningPlanStats(exec);

            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "GetMore command executor error: "
                                        << WorkingSetCommon::toStatusString(obj));
        }

        return Status::OK();
    }

    /**
     * Called via a ScopeGuard on early return in order to ensure that the ClientCursor gets
     * cleaned up properly.
     */
    static void cleanupCursor(OperationContext* txn,
                              ClientCursorPin* ccPin,
                              const GetMoreRequest& request) {
        ClientCursor* cursor = ccPin->c();

        std::unique_ptr<Lock::DBLock> unpinDBLock;
        std::unique_ptr<Lock::CollectionLock> unpinCollLock;

        if (cursor->isAggCursor()) {
            unpinDBLock.reset(new Lock::DBLock(txn->lockState(), request.nss.db(), MODE_IS));
            unpinCollLock.reset(
                new Lock::CollectionLock(txn->lockState(), request.nss.ns(), MODE_IS));
        }

        ccPin->deleteUnderlying();
    }

} getMoreCmd;

}  // namespace mongo
