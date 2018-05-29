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
#include "mongo/db/curop_failpoint_helpers.h"
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
#include "mongo/db/repl/replication_coordinator.h"
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

MONGO_FAIL_POINT_DEFINE(rsStopGetMoreCmd);

MONGO_FAIL_POINT_DEFINE(waitWithPinnedCursorDuringGetMoreBatch);

/**
 * Validates that the lsid of 'opCtx' matches that of 'cursor'. This must be called after
 * authenticating, so that it is safe to report the lsid of 'cursor'.
 */
void validateLSID(OperationContext* opCtx, const GetMoreRequest& request, ClientCursor* cursor) {
    uassert(50736,
            str::stream() << "Cannot run getMore on cursor " << request.cursorid
                          << ", which was not created in a session, in session "
                          << *opCtx->getLogicalSessionId(),
            !opCtx->getLogicalSessionId() || cursor->getSessionId());

    uassert(50737,
            str::stream() << "Cannot run getMore on cursor " << request.cursorid
                          << ", which was created in session "
                          << *cursor->getSessionId()
                          << ", without an lsid",
            opCtx->getLogicalSessionId() || !cursor->getSessionId());

    // TODO: SERVER-35323 - compare logicalSessionId that include userId.
    uassert(50738,
            str::stream() << "Cannot run getMore on cursor " << request.cursorid
                          << ", which was created in session "
                          << *cursor->getSessionId()
                          << ", in session "
                          << *opCtx->getLogicalSessionId(),
            !opCtx->getLogicalSessionId() || !cursor->getSessionId() ||
                (opCtx->getLogicalSessionId()->getId() == cursor->getSessionId()->getId()));
}

/**
 * Validates that the txnNumber of 'opCtx' matches that of 'cursor'. This must be called after
 * authenticating, so that it is safe to report the txnNumber of 'cursor'.
 */
void validateTxnNumber(OperationContext* opCtx,
                       const GetMoreRequest& request,
                       ClientCursor* cursor) {
    uassert(50739,
            str::stream() << "Cannot run getMore on cursor " << request.cursorid
                          << ", which was not created in a transaction, in transaction "
                          << *opCtx->getTxnNumber(),
            !opCtx->getTxnNumber() || cursor->getTxnNumber());

    uassert(50740,
            str::stream() << "Cannot run getMore on cursor " << request.cursorid
                          << ", which was created in transaction "
                          << *cursor->getTxnNumber()
                          << ", without a txnNumber",
            opCtx->getTxnNumber() || !cursor->getTxnNumber());

    uassert(50741,
            str::stream() << "Cannot run getMore on cursor " << request.cursorid
                          << ", which was created in transaction "
                          << *cursor->getTxnNumber()
                          << ", in transaction "
                          << *opCtx->getTxnNumber(),
            !opCtx->getTxnNumber() || !cursor->getTxnNumber() ||
                (*opCtx->getTxnNumber() == *cursor->getTxnNumber()));
}

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

    virtual bool allowsAfterClusterTime(const BSONObj& cmdObj) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const final {
        // Uses the readConcern setting from whatever created the cursor.
        return level == repl::ReadConcernLevel::kLocalReadConcern;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

    std::string help() const override {
        return "retrieve more results from an existing cursor";
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
                               const BSONObj& cmdObj) const override {
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
            uassertStatusOK(status);
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
                uasserted(ErrorCodes::OperationFailed, "collection dropped between getMore calls");
            }
            cursorManager = collection->getCursorManager();
        }

        auto ccPin = cursorManager->pinCursor(opCtx, request.cursorid);
        uassertStatusOK(ccPin.getStatus());

        ClientCursor* cursor = ccPin.getValue().getCursor();

        // Only used by the failpoints.
        const auto dropAndReaquireReadLock = [&readLock, opCtx, &request]() {
            // Make sure an interrupted operation does not prevent us from reacquiring the lock.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            readLock.reset();
            readLock.emplace(opCtx, request.nss);
        };

        // If the 'waitAfterPinningCursorBeforeGetMoreBatch' fail point is enabled, set the 'msg'
        // field of this operation's CurOp to signal that we've hit this point and then repeatedly
        // release and re-acquire the collection readLock at regular intervals until the failpoint
        // is released. This is done in order to avoid deadlocks caused by the pinned-cursor
        // failpoints in this file (see SERVER-21997).
        if (MONGO_FAIL_POINT(waitAfterPinningCursorBeforeGetMoreBatch)) {
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &waitAfterPinningCursorBeforeGetMoreBatch,
                opCtx,
                "waitAfterPinningCursorBeforeGetMoreBatch",
                dropAndReaquireReadLock);
        }

        // A user can only call getMore on their own cursor. If there were multiple users
        // authenticated when the cursor was created, then at least one of them must be
        // authenticated in order to run getMore on the cursor.
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isCoauthorizedWith(cursor->getAuthenticatedUsers())) {
            uasserted(ErrorCodes::Unauthorized,
                      str::stream() << "cursor id " << request.cursorid
                                    << " was not created by the authenticated user");
        }

        if (request.nss != cursor->nss()) {
            uasserted(ErrorCodes::Unauthorized,
                      str::stream() << "Requested getMore on namespace '" << request.nss.ns()
                                    << "', but cursor belongs to a different namespace "
                                    << cursor->nss().ns());
        }

        // Ensure the lsid and txnNumber of the getMore match that of the originating command.
        validateLSID(opCtx, request, cursor);
        validateTxnNumber(opCtx, request, cursor);

        if (request.nss.isOplog() && MONGO_FAIL_POINT(rsStopGetMoreCmd)) {
            uasserted(ErrorCodes::CommandFailed,
                      str::stream() << "getMore on " << request.nss.ns()
                                    << " rejected due to active fail point rsStopGetMoreCmd");
        }

        // Validation related to awaitData.
        if (cursor->isAwaitData()) {
            invariant(cursor->isTailable());
        }

        if (request.awaitDataTimeout && !cursor->isAwaitData()) {
            Status status(ErrorCodes::BadValue,
                          "cannot set maxTimeMS on getMore command for a non-awaitData cursor");
            uassertStatusOK(status);
        }

        // On early return, get rid of the cursor.
        ScopeGuard cursorFreer = MakeGuard(&ClientCursorPin::deleteUnderlying, &ccPin.getValue());

        const auto replicationMode = repl::ReplicationCoordinator::get(opCtx)->getReplicationMode();
        if (replicationMode == repl::ReplicationCoordinator::modeReplSet &&
            cursor->getReadConcernLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
            opCtx->recoveryUnit()->setTimestampReadSource(
                RecoveryUnit::ReadSource::kMajorityCommitted);
            uassertStatusOK(opCtx->recoveryUnit()->obtainMajorityCommittedSnapshot());
        }

        const bool disableAwaitDataFailpointActive =
            MONGO_FAIL_POINT(disableAwaitDataForGetMoreCmd);

        // We assume that cursors created through a DBDirectClient are always used from their
        // original OperationContext, so we do not need to move time to and from the cursor.
        if (!opCtx->getClient()->isInDirectClient()) {
            // There is no time limit set directly on this getMore command. If the cursor is
            // awaitData, then we supply a default time of one second. Otherwise we roll over
            // any leftover time from the maxTimeMS of the operation that spawned this cursor,
            // applying it to this getMore.
            if (cursor->isAwaitData() && !disableAwaitDataFailpointActive) {
                awaitDataState(opCtx).waitForInsertsDeadline =
                    opCtx->getServiceContext()->getPreciseClockSource()->now() +
                    request.awaitDataTimeout.value_or(Seconds{1});
            } else if (cursor->getLeftoverMaxTimeMicros() < Microseconds::max()) {
                opCtx->setDeadlineAfterNowBy(cursor->getLeftoverMaxTimeMicros());
            }
        }
        if (!cursor->isAwaitData()) {
            opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.
        }

        PlanExecutor* exec = cursor->getExecutor();
        exec->reattachToOperationContext(opCtx);
        uassertStatusOK(exec->restoreState());

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
        PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
        long long numResults = 0;

        // We report keysExamined and docsExamined to OpDebug for a given getMore operation. To
        // obtain these values we need to take a diff of the pre-execution and post-execution
        // metrics, as they accumulate over the course of a cursor's lifetime.
        PlanSummaryStats preExecutionStats;
        Explain::getSummaryStats(*exec, &preExecutionStats);

        // Mark this as an AwaitData operation if appropriate.
        if (cursor->isAwaitData() && !disableAwaitDataFailpointActive) {
            if (request.lastKnownCommittedOpTime)
                clientsLastKnownCommittedOpTime(opCtx) = request.lastKnownCommittedOpTime.get();
            awaitDataState(opCtx).shouldWaitForInserts = true;
        }

        // We're about to begin running the PlanExecutor in order to fill the getMore batch. If the
        // 'waitWithPinnedCursorDuringGetMoreBatch' failpoint is active, set the 'msg' field of this
        // operation's CurOp to signal that we've hit this point and then spin until the failpoint
        // is released.
        if (MONGO_FAIL_POINT(waitWithPinnedCursorDuringGetMoreBatch)) {
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &waitWithPinnedCursorDuringGetMoreBatch,
                opCtx,
                "waitWithPinnedCursorDuringGetMoreBatch",
                dropAndReaquireReadLock);
        }

        Status batchStatus = generateBatch(opCtx, cursor, request, &nextBatch, &state, &numResults);
        uassertStatusOK(batchStatus);

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

        if (shouldSaveCursorGetMore(state, exec, cursor->isTailable())) {
            respondWithId = request.cursorid;

            exec->saveState();
            exec->detachFromOperationContext();

            cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());
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

        // We're about to unpin the cursor as the ClientCursorPin goes out of scope (or delete it,
        // if it has been exhausted). If the 'waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch'
        // failpoint is active, set the 'msg' field of this operation's CurOp to signal that we've
        // hit this point and then spin until the failpoint is released.
        if (MONGO_FAIL_POINT(waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch)) {
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch,
                opCtx,
                "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch",
                dropAndReaquireReadLock);
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
        uassertStatusOK(parsedRequest.getStatus());
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
                awaitDataState(opCtx).shouldWaitForInserts = false;
                // Add result to output buffer.
                nextBatch->setLatestOplogTimestamp(exec->getLatestOplogTimestamp());
                nextBatch->append(obj);
                (*numResults)++;
            }
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // FAILURE state will make getMore command close the cursor even if it's tailable.
            *state = PlanExecutor::FAILURE;
            return Status::OK();
        }

        switch (*state) {
            case PlanExecutor::FAILURE:
                // Log an error message and then perform the same cleanup as DEAD.
                error() << "GetMore command executor error: " << PlanExecutor::statestr(*state)
                        << ", stats: " << redact(Explain::getWinningPlanStats(exec));
            case PlanExecutor::DEAD: {
                nextBatch->abandon();
                // We should always have a valid status member object at this point.
                auto status = WorkingSetCommon::getMemberObjectStatus(obj);
                invariant(!status.isOK());
                return status;
            }
            case PlanExecutor::IS_EOF:
                // This causes the reported latest oplog timestamp to advance even when there are
                // no results for this particular query.
                nextBatch->setLatestOplogTimestamp(exec->getLatestOplogTimestamp());
            default:
                return Status::OK();
        }

        MONGO_UNREACHABLE;
    }

} getMoreCmd;

}  // namespace
}  // namespace mongo
