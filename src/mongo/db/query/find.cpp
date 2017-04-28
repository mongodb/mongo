/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

#include "mongo/db/query/find.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::unique_ptr;
using stdx::make_unique;

// Failpoint for checking whether we've received a getmore.
MONGO_FP_DECLARE(failReceivedGetmore);

bool isCursorTailable(const ClientCursor* cursor) {
    return cursor->queryOptions() & QueryOption_CursorTailable;
}

bool isCursorAwaitData(const ClientCursor* cursor) {
    return cursor->queryOptions() & QueryOption_AwaitData;
}

bool shouldSaveCursor(OperationContext* opCtx,
                      const Collection* collection,
                      PlanExecutor::ExecState finalState,
                      PlanExecutor* exec) {
    if (PlanExecutor::FAILURE == finalState || PlanExecutor::DEAD == finalState) {
        return false;
    }

    const QueryRequest& qr = exec->getCanonicalQuery()->getQueryRequest();
    if (!qr.wantMore()) {
        return false;
    }

    // We keep a tailable cursor around unless the collection we're tailing has no
    // records.
    //
    // SERVER-13955: we should be able to create a tailable cursor that waits on
    // an empty collection. Right now we do not keep a cursor if the collection
    // has zero records.
    if (qr.isTailable()) {
        return collection && collection->numRecords(opCtx) != 0U;
    }

    return !exec->isEOF();
}

bool shouldSaveCursorGetMore(PlanExecutor::ExecState finalState,
                             PlanExecutor* exec,
                             bool isTailable) {
    if (PlanExecutor::FAILURE == finalState || PlanExecutor::DEAD == finalState) {
        return false;
    }

    if (isTailable) {
        return true;
    }

    return !exec->isEOF();
}

void beginQueryOp(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const BSONObj& queryObj,
                  long long ntoreturn,
                  long long ntoskip) {
    auto curOp = CurOp::get(opCtx);
    curOp->debug().ntoreturn = ntoreturn;
    curOp->debug().ntoskip = ntoskip;
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    curOp->setOpDescription_inlock(queryObj);
    curOp->setNS_inlock(nss.ns());
}

void endQueryOp(OperationContext* opCtx,
                Collection* collection,
                const PlanExecutor& exec,
                long long numResults,
                CursorId cursorId) {
    auto curOp = CurOp::get(opCtx);

    // Fill out basic CurOp query exec properties.
    curOp->debug().nreturned = numResults;
    curOp->debug().cursorid = (0 == cursorId ? -1 : cursorId);
    curOp->debug().cursorExhausted = (0 == cursorId);

    // Fill out CurOp based on explain summary statistics.
    PlanSummaryStats summaryStats;
    Explain::getSummaryStats(exec, &summaryStats);
    curOp->debug().setPlanSummaryMetrics(summaryStats);

    if (collection) {
        collection->infoCache()->notifyOfQuery(opCtx, summaryStats.indexesUsed);
    }

    if (curOp->shouldDBProfile()) {
        BSONObjBuilder statsBob;
        Explain::getWinningPlanStats(&exec, &statsBob);
        curOp->debug().execStats = statsBob.obj();
    }
}

namespace {

/**
 * Uses 'cursor' to fill out 'bb' with the batch of result documents to
 * be returned by this getMore.
 *
 * Returns the number of documents in the batch in 'numResults', which must be initialized to
 * zero by the caller. Returns the final ExecState returned by the cursor in *state. Returns
 * whether or not to save the ClientCursor in 'shouldSaveCursor'. Returns the slave's time to
 * read until in 'slaveReadTill' (for master/slave).
 *
 * Returns an OK status if the batch was successfully generated, and a non-OK status if the
 * PlanExecutor encounters a failure.
 */
void generateBatch(int ntoreturn,
                   ClientCursor* cursor,
                   BufBuilder* bb,
                   int* numResults,
                   Timestamp* slaveReadTill,
                   PlanExecutor::ExecState* state) {
    PlanExecutor* exec = cursor->getExecutor();

    BSONObj obj;
    while (!FindCommon::enoughForGetMore(ntoreturn, *numResults) &&
           PlanExecutor::ADVANCED == (*state = exec->getNext(&obj, NULL))) {
        // If we can't fit this result inside the current batch, then we stash it for later.
        if (!FindCommon::haveSpaceForNext(obj, *numResults, bb->len())) {
            exec->enqueue(obj);
            break;
        }

        // Add result to output buffer.
        bb->appendBuf((void*)obj.objdata(), obj.objsize());

        // Count the result.
        (*numResults)++;

        // Possibly note slave's position in the oplog.
        if (cursor->queryOptions() & QueryOption_OplogReplay) {
            BSONElement e = obj["ts"];
            if (BSONType::Date == e.type() || BSONType::bsonTimestamp == e.type()) {
                *slaveReadTill = e.timestamp();
            }
        }
    }

    // Propagate any errors to the caller.
    if (PlanExecutor::FAILURE == *state) {
        error() << "getMore executor error, stats: " << redact(Explain::getWinningPlanStats(exec));
        uasserted(17406, "getMore executor error: " + WorkingSetCommon::toStatusString(obj));
    } else if (PlanExecutor::DEAD == *state) {
        uasserted(ErrorCodes::QueryPlanKilled,
                  str::stream() << "PlanExecutor killed: "
                                << WorkingSetCommon::toStatusString(obj));
    }
}

}  // namespace

/**
 * Called by db/instance.cpp.  This is the getMore entry point.
 */
Message getMore(OperationContext* opCtx,
                const char* ns,
                int ntoreturn,
                long long cursorid,
                bool* exhaust,
                bool* isCursorAuthorized) {
    invariant(ntoreturn >= 0);

    CurOp& curOp = *CurOp::get(opCtx);

    // For testing, we may want to fail if we receive a getmore.
    if (MONGO_FAIL_POINT(failReceivedGetmore)) {
        invariant(0);
    }

    *exhaust = false;

    const NamespaceString nss(ns);

    // Cursors come in one of two flavors:
    // - Cursors owned by the collection cursor manager, such as those generated via the find
    //   command. For these cursors, we hold the appropriate collection lock for the duration of
    //   the getMore using AutoGetCollectionForRead. This will automatically update the CurOp
    //   object appropriately and record execution time via Top upon completion.
    // - Cursors owned by the global cursor manager, such as those generated via the aggregate
    //   command. These cursors either hold no collection state or manage their collection state
    //   internally, so we acquire no locks. In this case we use the AutoStatsTracker object to
    //   update the CurOp object appropriately and record execution time via Top upon
    //   completion.
    //
    // Thus, only one of 'readLock' and 'statsTracker' will be populated as we populate
    // 'cursorManager'.
    boost::optional<AutoGetCollectionForReadCommand> readLock;
    boost::optional<AutoStatsTracker> statsTracker;
    CursorManager* cursorManager;

    if (CursorManager::isGloballyManagedCursor(cursorid)) {
        cursorManager = CursorManager::getGlobalCursorManager();

        if (boost::optional<NamespaceString> nssForCurOp = nss.isGloballyManagedNamespace()
                ? nss.getTargetNSForGloballyManagedNamespace()
                : nss) {
            AutoGetDb autoDb(opCtx, nssForCurOp->db(), MODE_IS);
            const auto profilingLevel = autoDb.getDb()
                ? boost::optional<int>{autoDb.getDb()->getProfilingLevel()}
                : boost::none;
            statsTracker.emplace(opCtx, *nssForCurOp, Top::LockType::NotLocked, profilingLevel);
            uassert(
                ErrorCodes::CommandNotSupportedOnView,
                str::stream() << "Namespace " << nssForCurOp->ns()
                              << " is a view. OP_GET_MORE operations are not supported on views. "
                              << "Only clients which support the getMore command can be used to "
                                 "query views.",
                !autoDb.getDb()->getViewCatalog()->lookup(opCtx, nssForCurOp->ns()));
        }
    } else {
        readLock.emplace(opCtx, nss);
        Collection* collection = readLock->getCollection();
        uassert(
            ErrorCodes::OperationFailed, "collection dropped between getMore calls", collection);
        cursorManager = collection->getCursorManager();
    }

    LOG(5) << "Running getMore, cursorid: " << cursorid;

    // This checks to make sure the operation is allowed on a replicated node.  Since we are not
    // passing in a query object (necessary to check SlaveOK query option), the only state where
    // reads are allowed is PRIMARY (or master in master/slave).  This function uasserts if
    // reads are not okay.
    Status status =
        repl::getGlobalReplicationCoordinator()->checkCanServeReadsFor_UNSAFE(opCtx, nss, true);
    uassertStatusOK(status);

    // A pin performs a CC lookup and if there is a CC, increments the CC's pin value so it
    // doesn't time out.  Also informs ClientCursor that there is somebody actively holding the
    // CC, so don't delete it.
    auto ccPin = cursorManager->pinCursor(opCtx, cursorid);

    // These are set in the QueryResult msg we return.
    int resultFlags = ResultFlag_AwaitCapable;

    int numResults = 0;
    int startingResult = 0;

    const int InitialBufSize =
        512 + sizeof(QueryResult::Value) + FindCommon::kMaxBytesToReturnToClientAtOnce;

    BufBuilder bb(InitialBufSize);
    bb.skip(sizeof(QueryResult::Value));

    if (!ccPin.isOK()) {
        if (ccPin == ErrorCodes::CursorNotFound) {
            cursorid = 0;
            resultFlags = ResultFlag_CursorNotFound;
        } else {
            invariant(ccPin == ErrorCodes::QueryPlanKilled);
            uassertStatusOK(ccPin.getStatus());
        }
    } else {
        ClientCursor* cc = ccPin.getValue().getCursor();

        // Check for spoofing of the ns such that it does not match the one originally
        // there for the cursor.
        uassert(ErrorCodes::Unauthorized,
                str::stream() << "Requested getMore on namespace " << ns << ", but cursor "
                              << cursorid
                              << " belongs to namespace "
                              << cc->nss().ns(),
                nss == cc->nss());

        // A user can only call getMore on their own cursor. If there were multiple users
        // authenticated when the cursor was created, then at least one of them must be
        // authenticated in order to run getMore on the cursor.
        uassert(ErrorCodes::Unauthorized,
                str::stream() << "cursor id " << cursorid
                              << " was not created by the authenticated user",
                AuthorizationSession::get(opCtx->getClient())
                    ->isCoauthorizedWith(cc->getAuthenticatedUsers()));

        *isCursorAuthorized = true;

        if (cc->isReadCommitted())
            uassertStatusOK(opCtx->recoveryUnit()->setReadFromMajorityCommittedSnapshot());

        // If the operation that spawned this cursor had a time limit set, apply leftover
        // time to this getmore.
        if (cc->getLeftoverMaxTimeMicros() < Microseconds::max()) {
            uassert(40136,
                    "Illegal attempt to set operation deadline within DBDirectClient",
                    !opCtx->getClient()->isInDirectClient());
            opCtx->setDeadlineAfterNowBy(cc->getLeftoverMaxTimeMicros());
        }
        opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

        cc->updateSlaveLocation(opCtx);

        // If we're replaying the oplog, we save the last time that we read.
        Timestamp slaveReadTill;

        // What number result are we starting at?  Used to fill out the reply.
        startingResult = cc->pos();

        uint64_t notifierVersion = 0;
        std::shared_ptr<CappedInsertNotifier> notifier;
        if (isCursorAwaitData(cc)) {
            invariant(readLock->getCollection()->isCapped());
            // Retrieve the notifier which we will wait on until new data arrives. We make sure
            // to do this in the lock because once we drop the lock it is possible for the
            // collection to become invalid. The notifier itself will outlive the collection if
            // the collection is dropped, as we keep a shared_ptr to it.
            notifier = readLock->getCollection()->getCappedInsertNotifier();

            // Must get the version before we call generateBatch in case a write comes in after
            // that call and before we call wait on the notifier.
            notifierVersion = notifier->getVersion();
        }

        PlanExecutor* exec = cc->getExecutor();
        exec->reattachToOperationContext(opCtx);
        exec->restoreState();

        auto planSummary = Explain::getPlanSummary(exec);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp.setPlanSummary_inlock(planSummary);

            // Ensure that the original query object is available in the slow query log, profiler
            // and currentOp. Upconvert _query to resemble a getMore command, and set the original
            // command or upconverted legacy query in the originatingCommand field.
            curOp.setOpDescription_inlock(upconvertGetMoreEntry(nss, cursorid, ntoreturn));
            curOp.setOriginatingCommand_inlock(cc->getOriginatingCommandObj());
        }

        PlanExecutor::ExecState state;

        // We report keysExamined and docsExamined to OpDebug for a given getMore operation. To
        // obtain these values we need to take a diff of the pre-execution and post-execution
        // metrics, as they accumulate over the course of a cursor's lifetime.
        PlanSummaryStats preExecutionStats;
        Explain::getSummaryStats(*exec, &preExecutionStats);

        generateBatch(ntoreturn, cc, &bb, &numResults, &slaveReadTill, &state);

        // If this is an await data cursor, and we hit EOF without generating any results, then
        // we block waiting for new data to arrive.
        if (isCursorAwaitData(cc) && state == PlanExecutor::IS_EOF && numResults == 0) {
            // Save the PlanExecutor and drop our locks.
            exec->saveState();
            readLock.reset();

            // Block waiting for data for up to 1 second.
            Seconds timeout(1);
            notifier->wait(notifierVersion, timeout);
            notifier.reset();

            // Set expected latency to match wait time. This makes sure the logs aren't spammed
            // by awaitData queries that exceed slowms due to blocking on the CappedInsertNotifier.
            curOp.setExpectedLatencyMs(durationCount<Milliseconds>(timeout));

            // Reacquiring locks.
            readLock.emplace(opCtx, nss);
            exec->restoreState();

            // We woke up because either the timed_wait expired, or there was more data. Either
            // way, attempt to generate another batch of results.
            generateBatch(ntoreturn, cc, &bb, &numResults, &slaveReadTill, &state);
        }

        PlanSummaryStats postExecutionStats;
        Explain::getSummaryStats(*exec, &postExecutionStats);
        postExecutionStats.totalKeysExamined -= preExecutionStats.totalKeysExamined;
        postExecutionStats.totalDocsExamined -= preExecutionStats.totalDocsExamined;
        curOp.debug().setPlanSummaryMetrics(postExecutionStats);

        // We do not report 'execStats' for aggregation or other globally managed cursors, both in
        // the original request and subsequent getMore. It would be useful to have this information
        // for an aggregation, but the source PlanExecutor could be destroyed before we know whether
        // we need execStats and we do not want to generate for all operations due to cost.
        if (!CursorManager::isGloballyManagedCursor(cursorid) && curOp.shouldDBProfile()) {
            BSONObjBuilder execStatsBob;
            Explain::getWinningPlanStats(exec, &execStatsBob);
            curOp.debug().execStats = execStatsBob.obj();
        }

        // Our two possible ClientCursorPin cleanup paths are:
        // 1) If the cursor is not going to be saved, we call deleteUnderlying() on the pin.
        // 2) If the cursor is going to be saved, we simply let the pin go out of scope. In this
        //    case, the pin's destructor will be invoked, which will call release() on the pin.
        //    Because our ClientCursorPin is declared after our lock is declared, this will happen
        //    under the lock if any locking was necessary.
        if (!shouldSaveCursorGetMore(state, exec, isCursorTailable(cc))) {
            ccPin.getValue().deleteUnderlying();

            // cc is now invalid, as is the executor
            cursorid = 0;
            cc = nullptr;
            curOp.debug().cursorExhausted = true;

            LOG(5) << "getMore NOT saving client cursor, ended with state "
                   << PlanExecutor::statestr(state);
        } else {
            // Continue caching the ClientCursor.
            cc->incPos(numResults);
            exec->saveState();
            exec->detachFromOperationContext();
            LOG(5) << "getMore saving client cursor ended with state "
                   << PlanExecutor::statestr(state);

            // Possibly note slave's position in the oplog.
            if ((cc->queryOptions() & QueryOption_OplogReplay) && !slaveReadTill.isNull()) {
                cc->slaveReadTill(slaveReadTill);
            }

            *exhaust = cc->queryOptions() & QueryOption_Exhaust;

            // If the getmore had a time limit, remaining time is "rolled over" back to the
            // cursor (for use by future getmore ops).
            cc->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());
        }
    }

    QueryResult::View qr = bb.buf();
    qr.msgdata().setLen(bb.len());
    qr.msgdata().setOperation(opReply);
    qr.setResultFlags(resultFlags);
    qr.setCursorId(cursorid);
    qr.setStartingFrom(startingResult);
    qr.setNReturned(numResults);
    LOG(5) << "getMore returned " << numResults << " results\n";
    return Message(bb.release());
}

std::string runQuery(OperationContext* opCtx,
                     QueryMessage& q,
                     const NamespaceString& nss,
                     Message& result) {
    CurOp& curOp = *CurOp::get(opCtx);

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid ns [" << nss.ns() << "]",
            nss.isValid());
    invariant(!nss.isCommand());

    // Set CurOp information.
    beginQueryOp(opCtx, nss, q.query, q.ntoreturn, q.ntoskip);

    // Parse the qm into a CanonicalQuery.

    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, q, ExtensionsCallbackReal(opCtx, &nss));
    if (!statusWithCQ.isOK()) {
        uasserted(17287,
                  str::stream() << "Can't canonicalize query: "
                                << statusWithCQ.getStatus().toString());
    }
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
    invariant(cq.get());

    LOG(5) << "Running query:\n" << redact(cq->toString());
    LOG(2) << "Running query: " << redact(cq->toStringShort());

    // Parse, canonicalize, plan, transcribe, and get a plan executor.
    AutoGetCollectionOrViewForReadCommand ctx(opCtx, nss);
    Collection* collection = ctx.getCollection();

    if (ctx.getView()) {
        uasserted(ErrorCodes::CommandNotSupportedOnView,
                  str::stream()
                      << "Namespace "
                      << nss.ns()
                      << " is a view. Legacy find operations are not supported on views. "
                      << "Only clients which support the find command can be used to query views.");
    }

    // We have a parsed query. Time to get the execution plan for it.
    auto exec = uassertStatusOK(
        getExecutorFind(opCtx, collection, nss, std::move(cq), PlanExecutor::YIELD_AUTO));

    const QueryRequest& qr = exec->getCanonicalQuery()->getQueryRequest();

    // If it's actually an explain, do the explain and return rather than falling through
    // to the normal query execution loop.
    if (qr.isExplain()) {
        BufBuilder bb;
        bb.skip(sizeof(QueryResult::Value));

        BSONObjBuilder explainBob;
        Explain::explainStages(
            exec.get(), collection, ExplainOptions::Verbosity::kExecAllPlans, &explainBob);

        // Add the resulting object to the return buffer.
        BSONObj explainObj = explainBob.obj();
        bb.appendBuf((void*)explainObj.objdata(), explainObj.objsize());

        // Set query result fields.
        QueryResult::View qr = bb.buf();
        qr.setResultFlagsToOk();
        qr.msgdata().setLen(bb.len());
        curOp.debug().responseLength = bb.len();
        qr.msgdata().setOperation(opReply);
        qr.setCursorId(0);
        qr.setStartingFrom(0);
        qr.setNReturned(1);
        result.setData(bb.release());
        return "";
    }

    // Handle query option $maxTimeMS (not used with commands).
    if (qr.getMaxTimeMS() > 0) {
        uassert(40116,
                "Illegal attempt to set operation deadline within DBDirectClient",
                !opCtx->getClient()->isInDirectClient());
        opCtx->setDeadlineAfterNowBy(Milliseconds{qr.getMaxTimeMS()});
    }
    opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

    // uassert if we are not on a primary, and not a secondary with SlaveOk query parameter set.
    bool slaveOK = qr.isSlaveOk() || qr.hasReadPref();
    Status serveReadsStatus =
        repl::getGlobalReplicationCoordinator()->checkCanServeReadsFor_UNSAFE(opCtx, nss, slaveOK);
    uassertStatusOK(serveReadsStatus);

    // Run the query.
    // bb is used to hold query results
    // this buffer should contain either requested documents per query or
    // explain information, but not both
    BufBuilder bb(FindCommon::kInitReplyBufferSize);
    bb.skip(sizeof(QueryResult::Value));

    // How many results have we obtained from the executor?
    int numResults = 0;

    // If we're replaying the oplog, we save the last time that we read.
    Timestamp slaveReadTill;

    BSONObj obj;
    PlanExecutor::ExecState state;

    // Get summary info about which plan the executor is using.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
    }

    while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
        // If we can't fit this result inside the current batch, then we stash it for later.
        if (!FindCommon::haveSpaceForNext(obj, numResults, bb.len())) {
            exec->enqueue(obj);
            break;
        }

        // Add result to output buffer.
        bb.appendBuf((void*)obj.objdata(), obj.objsize());

        // Count the result.
        ++numResults;

        // Possibly note slave's position in the oplog.
        if (qr.isOplogReplay()) {
            BSONElement e = obj["ts"];
            if (Date == e.type() || bsonTimestamp == e.type()) {
                slaveReadTill = e.timestamp();
            }
        }

        if (FindCommon::enoughForFirstBatch(qr, numResults)) {
            LOG(5) << "Enough for first batch, wantMore=" << qr.wantMore()
                   << " ntoreturn=" << qr.getNToReturn().value_or(0)
                   << " numResults=" << numResults;
            break;
        }
    }

    // Caller expects exceptions thrown in certain cases.
    if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
        error() << "Plan executor error during find: " << PlanExecutor::statestr(state)
                << ", stats: " << redact(Explain::getWinningPlanStats(exec.get()));
        uasserted(17144, "Executor error: " + WorkingSetCommon::toStatusString(obj));
    }

    // Before saving the cursor, ensure that whatever plan we established happened with the expected
    // collection version
    auto css = CollectionShardingState::get(opCtx, nss);
    css->checkShardVersionOrThrow(opCtx);

    // Fill out CurOp based on query results. If we have a cursorid, we will fill out CurOp with
    // this cursorid later.
    long long ccId = 0;

    if (shouldSaveCursor(opCtx, collection, state, exec.get())) {
        // We won't use the executor until it's getMore'd.
        exec->saveState();
        exec->detachFromOperationContext();

        // Allocate a new ClientCursor and register it with the cursor manager.
        ClientCursorPin pinnedCursor = collection->getCursorManager()->registerCursor(
            opCtx,
            {std::move(exec),
             nss,
             AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
             opCtx->recoveryUnit()->isReadingFromMajorityCommittedSnapshot(),
             upconvertQueryEntry(q.query, qr.nss(), q.ntoreturn, q.ntoskip)});
        ccId = pinnedCursor.getCursor()->cursorid();

        LOG(5) << "caching executor with cursorid " << ccId << " after returning " << numResults
               << " results";

        // TODO document
        if (qr.isOplogReplay() && !slaveReadTill.isNull()) {
            pinnedCursor.getCursor()->slaveReadTill(slaveReadTill);
        }

        // TODO document
        if (qr.isExhaust()) {
            curOp.debug().exhaust = true;
        }

        pinnedCursor.getCursor()->setPos(numResults);

        // If the query had a time limit, remaining time is "rolled over" to the cursor (for
        // use by future getmore ops).
        pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

        endQueryOp(opCtx, collection, *pinnedCursor.getCursor()->getExecutor(), numResults, ccId);
    } else {
        LOG(5) << "Not caching executor but returning " << numResults << " results.";
        endQueryOp(opCtx, collection, *exec, numResults, ccId);
    }

    // Fill out the output buffer's header.
    QueryResult::View queryResultView = bb.buf();
    queryResultView.setCursorId(ccId);
    queryResultView.setResultFlagsToOk();
    queryResultView.msgdata().setLen(bb.len());
    queryResultView.msgdata().setOperation(opReply);
    queryResultView.setStartingFrom(0);
    queryResultView.setNReturned(numResults);

    // Add the results from the query into the output buffer.
    result.setData(bb.release());

    // curOp.debug().exhaust is set above.
    return curOp.debug().exhaust ? nss.ns() : "";
}

}  // namespace mongo
