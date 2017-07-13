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
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/top.h"
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
class GetMoreCmd : public BasicCommand {
    MONGO_DISALLOW_COPYING(GetMoreCmd);

public:
    GetMoreCmd() : BasicCommand("getMore") {}


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

    bool supportsReadConcern(const std::string& dbName, const BSONObj& cmdObj) const final {
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
        return GetMoreRequest::parseNs(dbname, cmdObj).ns();
    }

    Status checkAuthForCommand(Client* client,
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

    bool runParsed(OperationContext* opCtx,
                   const NamespaceString& origNss,
                   const GetMoreRequest& request,
                   const BSONObj& cmdObj,
                   BSONObjBuilder& result) {

        auto curOp = CurOp::get(opCtx);
        curOp->debug().cursorid = request.cursorid;

        // Validate term before acquiring locks, if provided.
        if (request.term) {
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            Status status = replCoord->updateTerm(opCtx, *request.term);
            // Note: updateTerm returns ok if term stayed the same.
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
        }

        // Cursors come in one of two flavors:
        // - Cursors owned by the collection cursor manager, such as those generated via the find
        //   command. For these cursors, we hold the appropriate collection lock for the duration of
        //   the getMore using AutoGetCollectionForRead.
        // - Cursors owned by the global cursor manager, such as those generated via the aggregate
        //   command. These cursors either hold no collection state or manage their collection state
        //   internally, so we acquire no locks.
        //
        // While we only need to acquire locks in the case of a cursor which is *not* globally
        // owned, we need to create an AutoStatsTracker in either case. This is responsible for
        // updating statistics in CurOp and Top. We avoid using AutoGetCollectionForReadCommand
        // because we may need to drop and reacquire locks when the cursor is awaitData, but we
        // don't want to update the stats twice.
        //
        // Note that we acquire our locks before our ClientCursorPin, in order to ensure that
        // the pin's destructor is called before the lock's destructor (if there is one) so that the
        // cursor cleanup can occur under the lock.
        boost::optional<AutoGetCollectionForRead> readLock;
        boost::optional<AutoStatsTracker> statsTracker;
        CursorManager* cursorManager;

        if (CursorManager::isGloballyManagedCursor(request.cursorid)) {
            cursorManager = CursorManager::getGlobalCursorManager();

            if (boost::optional<NamespaceString> nssForCurOp =
                    request.nss.isGloballyManagedNamespace()
                    ? request.nss.getTargetNSForGloballyManagedNamespace()
                    : request.nss) {
                const boost::optional<int> dbProfilingLevel = boost::none;
                statsTracker.emplace(
                    opCtx, *nssForCurOp, Top::LockType::NotLocked, dbProfilingLevel);
            }
        } else {
            readLock.emplace(opCtx, request.nss);
            const int doNotChangeProfilingLevel = 0;
            statsTracker.emplace(opCtx,
                                 request.nss,
                                 Top::LockType::ReadLocked,
                                 readLock->getDb() ? readLock->getDb()->getProfilingLevel()
                                                   : doNotChangeProfilingLevel);

            Collection* collection = readLock->getCollection();
            if (!collection) {
                return appendCommandStatus(result,
                                           Status(ErrorCodes::OperationFailed,
                                                  "collection dropped between getMore calls"));
            }
            cursorManager = collection->getCursorManager();
        }

        auto ccPin = cursorManager->pinCursor(opCtx, request.cursorid);
        if (!ccPin.isOK()) {
            return appendCommandStatus(result, ccPin.getStatus());
        }

        ClientCursor* cursor = ccPin.getValue().getCursor();

        // If the fail point is enabled, busy wait until it is disabled.
        while (MONGO_FAIL_POINT(keepCursorPinnedDuringGetMore)) {
            if (readLock) {
                // We unlock and re-acquire the locks periodically in order to avoid deadlock (see
                // SERVER-21997 for details).
                sleepFor(Milliseconds(10));
                readLock.reset();
                readLock.emplace(opCtx, request.nss);
            }
        }

        // A user can only call getMore on their own cursor. If there were multiple users
        // authenticated when the cursor was created, then at least one of them must be
        // authenticated in order to run getMore on the cursor.
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isCoauthorizedWith(cursor->getAuthenticatedUsers())) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::Unauthorized,
                       str::stream() << "cursor id " << request.cursorid
                                     << " was not created by the authenticated user"));
        }

        if (request.nss != cursor->nss()) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::Unauthorized,
                       str::stream() << "Requested getMore on namespace '" << request.nss.ns()
                                     << "', but cursor belongs to a different namespace "
                                     << cursor->nss().ns()));
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
        }

        if (request.awaitDataTimeout && !isCursorAwaitData(cursor)) {
            Status status(ErrorCodes::BadValue,
                          "cannot set maxTimeMS on getMore command for a non-awaitData cursor");
            return appendCommandStatus(result, status);
        }

        // On early return, get rid of the cursor.
        ScopeGuard cursorFreer = MakeGuard(&ClientCursorPin::deleteUnderlying, &ccPin.getValue());

        if (cursor->isReadCommitted())
            uassertStatusOK(opCtx->recoveryUnit()->setReadFromMajorityCommittedSnapshot());

        const bool hasOwnMaxTime = opCtx->hasDeadline();

        // We assume that cursors created through a DBDirectClient are always used from their
        // original OperationContext, so we do not need to move time to and from the cursor.
        if (!hasOwnMaxTime && !opCtx->getClient()->isInDirectClient()) {
            // There is no time limit set directly on this getMore command. If the cursor is
            // awaitData, then we supply a default time of one second. Otherwise we roll over
            // any leftover time from the maxTimeMS of the operation that spawned this cursor,
            // applying it to this getMore.
            if (isCursorAwaitData(cursor)) {
                opCtx->setDeadlineAfterNowBy(Seconds{1});
            } else if (cursor->getLeftoverMaxTimeMicros() < Microseconds::max()) {
                opCtx->setDeadlineAfterNowBy(cursor->getLeftoverMaxTimeMicros());
            }
        }
        opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

        PlanExecutor* exec = cursor->getExecutor();
        exec->reattachToOperationContext(opCtx);
        exec->restoreState();

        auto planSummary = Explain::getPlanSummary(exec);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setPlanSummary_inlock(planSummary);

            // Ensure that the original query or command object is available in the slow query log,
            // profiler and currentOp.
            auto originatingCommand = cursor->getOriginatingCommandObj();
            if (!originatingCommand.isEmpty()) {
                curOp->setOriginatingCommand_inlock(originatingCommand);
            }
        }

        CursorId respondWithId = 0;
        CursorResponseBuilder nextBatch(/*isInitialResponse*/ false, &result);
        BSONObj obj;
        // generateBatch() will not initialize 'state' if it exceeds the time limiting generating
        // the next batch for an awaitData cursor. In this case, 'state' should be
        // PlanExecutor::ADVANCED, so we do not attempt to get another batch.
        PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
        long long numResults = 0;

        // We report keysExamined and docsExamined to OpDebug for a given getMore operation. To
        // obtain these values we need to take a diff of the pre-execution and post-execution
        // metrics, as they accumulate over the course of a cursor's lifetime.
        PlanSummaryStats preExecutionStats;
        Explain::getSummaryStats(*exec, &preExecutionStats);

        // Mark this as an AwaitData operation if appropriate.
        if (isCursorAwaitData(cursor)) {
            if (request.lastKnownCommittedOpTime)
                clientsLastKnownCommittedOpTime(opCtx) = request.lastKnownCommittedOpTime.get();
            shouldWaitForInserts(opCtx) = true;
        }

        Status batchStatus = generateBatch(opCtx, cursor, request, &nextBatch, &state, &numResults);
        if (!batchStatus.isOK()) {
            return appendCommandStatus(result, batchStatus);
        }

        PlanSummaryStats postExecutionStats;
        Explain::getSummaryStats(*exec, &postExecutionStats);
        postExecutionStats.totalKeysExamined -= preExecutionStats.totalKeysExamined;
        postExecutionStats.totalDocsExamined -= preExecutionStats.totalDocsExamined;
        curOp->debug().setPlanSummaryMetrics(postExecutionStats);

        // We do not report 'execStats' for aggregation or other globally managed cursors, both in
        // the original request and subsequent getMore. It would be useful to have this information
        // for an aggregation, but the source PlanExecutor could be destroyed before we know whether
        // we need execStats and we do not want to generate for all operations due to cost.
        if (!CursorManager::isGloballyManagedCursor(request.cursorid) && curOp->shouldDBProfile()) {
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
                cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());
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
        }

        return true;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // Counted as a getMore, not as a command.
        globalOpCounters.gotGetMore();

        StatusWith<GetMoreRequest> parsedRequest = GetMoreRequest::parseFromBSON(dbname, cmdObj);
        if (!parsedRequest.isOK()) {
            return appendCommandStatus(result, parsedRequest.getStatus());
        }
        auto request = parsedRequest.getValue();
        return runParsed(opCtx, request.nss, request, cmdObj, result);
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
    Status generateBatch(OperationContext* opCtx,
                         ClientCursor* cursor,
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

                // As soon as we get a result, this operation no longer waits.
                shouldWaitForInserts(opCtx) = false;
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

        if (PlanExecutor::FAILURE == *state) {
            nextBatch->abandon();

            error() << "GetMore command executor error: " << PlanExecutor::statestr(*state)
                    << ", stats: " << redact(Explain::getWinningPlanStats(exec));

            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "GetMore command executor error: "
                                        << WorkingSetCommon::toStatusString(obj));
        } else if (PlanExecutor::DEAD == *state) {
            nextBatch->abandon();

            return Status(ErrorCodes::QueryPlanKilled,
                          str::stream() << "PlanExecutor killed: "
                                        << WorkingSetCommon::toStatusString(obj));
        }

        return Status::OK();
    }

} getMoreCmd;

}  // namespace mongo
