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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/service_context.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::endl;
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

bool shouldSaveCursor(OperationContext* txn,
                      const Collection* collection,
                      PlanExecutor::ExecState finalState,
                      PlanExecutor* exec) {
    if (PlanExecutor::FAILURE == finalState || PlanExecutor::DEAD == finalState) {
        return false;
    }

    const LiteParsedQuery& pq = exec->getCanonicalQuery()->getParsed();
    if (!pq.wantMore() && !pq.isTailable()) {
        return false;
    }

    if (pq.getNToReturn().value_or(0) == 1) {
        return false;
    }

    // We keep a tailable cursor around unless the collection we're tailing has no
    // records.
    //
    // SERVER-13955: we should be able to create a tailable cursor that waits on
    // an empty collection. Right now we do not keep a cursor if the collection
    // has zero records.
    if (pq.isTailable()) {
        return collection && collection->numRecords(txn) != 0U;
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

void beginQueryOp(OperationContext* txn,
                  const NamespaceString& nss,
                  const BSONObj& queryObj,
                  long long ntoreturn,
                  long long ntoskip) {
    auto curop = CurOp::get(txn);
    curop->debug().query = queryObj;
    curop->debug().ntoreturn = ntoreturn;
    curop->debug().ntoskip = ntoskip;
    stdx::lock_guard<Client> lk(*txn->getClient());
    curop->setQuery_inlock(queryObj);
    curop->setNS_inlock(nss.ns());
}

void endQueryOp(OperationContext* txn,
                Collection* collection,
                const PlanExecutor& exec,
                int dbProfilingLevel,
                long long numResults,
                CursorId cursorId) {
    auto curop = CurOp::get(txn);

    // Fill out basic curop query exec properties.
    curop->debug().nreturned = numResults;
    curop->debug().cursorid = (0 == cursorId ? -1 : cursorId);
    curop->debug().cursorExhausted = (0 == cursorId);

    // Fill out curop based on explain summary statistics.
    PlanSummaryStats summaryStats;
    Explain::getSummaryStats(exec, &summaryStats);
    curop->debug().hasSortStage = summaryStats.hasSortStage;
    curop->debug().keysExamined = summaryStats.totalKeysExamined;
    curop->debug().docsExamined = summaryStats.totalDocsExamined;
    curop->debug().idhack = summaryStats.isIdhack;

    if (collection) {
        collection->infoCache()->notifyOfQuery(txn, summaryStats.indexesUsed);
    }

    const logger::LogComponent queryLogComponent = logger::LogComponent::kQuery;
    const logger::LogSeverity logLevelOne = logger::LogSeverity::Debug(1);

    // Set debug information for consumption by the profiler and slow query log.
    if (dbProfilingLevel > 0 || curop->elapsedMillis() > serverGlobalParams.slowMS ||
        logger::globalLogDomain()->shouldLog(queryLogComponent, logLevelOne)) {
        // Generate plan summary string.
        stdx::lock_guard<Client>(*txn->getClient());
        curop->setPlanSummary_inlock(Explain::getPlanSummary(&exec));
    }

    // Set debug information for consumption by the profiler only.
    if (dbProfilingLevel > 0) {
        // Get BSON stats.
        unique_ptr<PlanStageStats> execStats(exec.getStats());
        BSONObjBuilder statsBob;
        Explain::statsToBSON(*execStats, &statsBob);
        curop->debug().execStats.set(statsBob.obj());

        // Replace exec stats with plan summary if stats cannot fit into CachedBSONObj.
        if (curop->debug().execStats.tooBig() && !curop->getPlanSummary().empty()) {
            BSONObjBuilder bob;
            bob.append("summary", curop->getPlanSummary());
            curop->debug().execStats.set(bob.done());
        }
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
    while (PlanExecutor::ADVANCED == (*state = exec->getNext(&obj, NULL))) {
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

        if (FindCommon::enoughForGetMore(ntoreturn, *numResults, bb->len())) {
            break;
        }
    }

    if (PlanExecutor::DEAD == *state || PlanExecutor::FAILURE == *state) {
        // Propagate this error to caller.
        const unique_ptr<PlanStageStats> stats(exec->getStats());
        error() << "getMore executor error, stats: " << Explain::statsToBSON(*stats);
        uasserted(17406, "getMore executor error: " + WorkingSetCommon::toStatusString(obj));
    }
}

}  // namespace

/**
 * Called by db/instance.cpp.  This is the getMore entry point.
 */
QueryResult::View getMore(OperationContext* txn,
                          const char* ns,
                          int ntoreturn,
                          long long cursorid,
                          bool* exhaust,
                          bool* isCursorAuthorized) {
    CurOp& curop = *CurOp::get(txn);

    // For testing, we may want to fail if we receive a getmore.
    if (MONGO_FAIL_POINT(failReceivedGetmore)) {
        invariant(0);
    }

    *exhaust = false;

    const NamespaceString nss(ns);

    // Depending on the type of cursor being operated on, we hold locks for the whole getMore,
    // or none of the getMore, or part of the getMore.  The three cases in detail:
    //
    // 1) Normal cursor: we lock with "ctx" and hold it for the whole getMore.
    // 2) Cursor owned by global cursor manager: we don't lock anything.  These cursors don't own
    //    any collection state. These cursors are generated either by the listCollections or
    //    listIndexes commands, as these special cursor-generating commands operate over catalog
    //    data rather than targeting the data within a collection.
    // 3) Agg cursor: we lock with "ctx", then release, then relock with "unpinDBLock" and
    //    "unpinCollLock".  This is because agg cursors handle locking internally (hence the
    //    release), but the pin and unpin of the cursor must occur under the collection lock.
    //    We don't use our AutoGetCollectionForRead "ctx" to relock, because
    //    AutoGetCollectionForRead checks the sharding version (and we want the relock for the
    //    unpin to succeed even if the sharding version has changed).
    //
    // Note that we declare our locks before our ClientCursorPin, in order to ensure that the
    // pin's destructor is called before the lock destructors (so that the unpin occurs under
    // the lock).
    unique_ptr<AutoGetCollectionForRead> ctx;
    unique_ptr<Lock::DBLock> unpinDBLock;
    unique_ptr<Lock::CollectionLock> unpinCollLock;

    CursorManager* cursorManager;
    if (nss.isListIndexesCursorNS() || nss.isListCollectionsCursorNS()) {
        // List collections and list indexes are special cursor-generating commands whose
        // cursors are managed globally, as they operate over catalog data rather than targeting
        // the data within a collection.
        cursorManager = CursorManager::getGlobalCursorManager();
    } else {
        ctx = stdx::make_unique<AutoGetCollectionForRead>(txn, nss);
        Collection* collection = ctx->getCollection();
        uassert(17356, "collection dropped between getMore calls", collection);
        cursorManager = collection->getCursorManager();
    }

    LOG(5) << "Running getMore, cursorid: " << cursorid << endl;

    // This checks to make sure the operation is allowed on a replicated node.  Since we are not
    // passing in a query object (necessary to check SlaveOK query option), the only state where
    // reads are allowed is PRIMARY (or master in master/slave).  This function uasserts if
    // reads are not okay.
    Status status = repl::getGlobalReplicationCoordinator()->checkCanServeReadsFor(txn, nss, true);
    uassertStatusOK(status);

    // A pin performs a CC lookup and if there is a CC, increments the CC's pin value so it
    // doesn't time out.  Also informs ClientCursor that there is somebody actively holding the
    // CC, so don't delete it.
    ClientCursorPin ccPin(cursorManager, cursorid);
    ClientCursor* cc = ccPin.c();
    // These are set in the QueryResult msg we return.
    int resultFlags = ResultFlag_AwaitCapable;

    int numResults = 0;
    int startingResult = 0;

    const int InitialBufSize =
        512 + sizeof(QueryResult::Value) + FindCommon::kMaxBytesToReturnToClientAtOnce;

    BufBuilder bb(InitialBufSize);
    bb.skip(sizeof(QueryResult::Value));

    if (NULL == cc) {
        cursorid = 0;
        resultFlags = ResultFlag_CursorNotFound;
    } else {
        // Check for spoofing of the ns such that it does not match the one originally
        // there for the cursor.
        uassert(ErrorCodes::Unauthorized,
                str::stream() << "Requested getMore on namespace " << ns << ", but cursor "
                              << cursorid << " belongs to namespace " << cc->ns(),
                ns == cc->ns());
        *isCursorAuthorized = true;

        if (cc->isReadCommitted())
            uassertStatusOK(txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot());

        // Reset timeout timer on the cursor since the cursor is still in use.
        cc->setIdleTime(0);

        // If the operation that spawned this cursor had a time limit set, apply leftover
        // time to this getmore.
        curop.setMaxTimeMicros(cc->getLeftoverMaxTimeMicros());
        txn->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

        // Ensure that the original query or command object is available in the slow query log,
        // profiler, and currentOp.
        curop.debug().query = cc->getQuery();
        {
            stdx::lock_guard<Client> lk(*txn->getClient());
            curop.setQuery_inlock(cc->getQuery());
        }

        cc->updateSlaveLocation(txn);

        if (cc->isAggCursor()) {
            // Agg cursors handle their own locking internally.
            ctx.reset();  // unlocks
        }

        // If we're replaying the oplog, we save the last time that we read.
        Timestamp slaveReadTill;

        // What number result are we starting at?  Used to fill out the reply.
        startingResult = cc->pos();

        uint64_t notifierVersion = 0;
        std::shared_ptr<CappedInsertNotifier> notifier;
        if (isCursorAwaitData(cc)) {
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

        PlanExecutor* exec = cc->getExecutor();
        exec->reattachToOperationContext(txn);
        exec->restoreState();
        PlanExecutor::ExecState state;

        generateBatch(ntoreturn, cc, &bb, &numResults, &slaveReadTill, &state);

        // If this is an await data cursor, and we hit EOF without generating any results, then
        // we block waiting for new data to arrive.
        if (isCursorAwaitData(cc) && state == PlanExecutor::IS_EOF && numResults == 0) {
            // Save the PlanExecutor and drop our locks.
            exec->saveState();
            ctx.reset();

            // Block waiting for data for up to 1 second.
            Seconds timeout(1);
            notifier->wait(notifierVersion, timeout);
            notifier.reset();

            // Set expected latency to match wait time. This makes sure the logs aren't spammed
            // by awaitData queries that exceed slowms due to blocking on the CappedInsertNotifier.
            curop.setExpectedLatencyMs(durationCount<Milliseconds>(timeout));

            // Reacquiring locks.
            ctx = make_unique<AutoGetCollectionForRead>(txn, nss);
            exec->restoreState();

            // We woke up because either the timed_wait expired, or there was more data. Either
            // way, attempt to generate another batch of results.
            generateBatch(ntoreturn, cc, &bb, &numResults, &slaveReadTill, &state);
        }

        // We have to do this before re-acquiring locks in the agg case because
        // shouldSaveCursorGetMore() can make a network call for agg cursors.
        //
        // TODO: Getting rid of PlanExecutor::isEOF() in favor of PlanExecutor::IS_EOF would mean
        // that this network operation is no longer necessary.
        const bool shouldSaveCursor = shouldSaveCursorGetMore(state, exec, isCursorTailable(cc));

        // In order to deregister a cursor, we need to be holding the DB + collection lock and
        // if the cursor is aggregation, we release these locks.
        if (cc->isAggCursor()) {
            invariant(NULL == ctx.get());
            unpinDBLock = make_unique<Lock::DBLock>(txn->lockState(), nss.db(), MODE_IS);
            unpinCollLock = make_unique<Lock::CollectionLock>(txn->lockState(), nss.ns(), MODE_IS);
        }

        // Our two possible ClientCursorPin cleanup paths are:
        // 1) If the cursor is not going to be saved, we call deleteUnderlying() on the pin.
        // 2) If the cursor is going to be saved, we simply let the pin go out of scope.  In
        //    this case, the pin's destructor will be invoked, which will call release() on the
        //    pin.  Because our ClientCursorPin is declared after our lock is declared, this
        //    will happen under the lock.
        if (!shouldSaveCursor) {
            ccPin.deleteUnderlying();

            // cc is now invalid, as is the executor
            cursorid = 0;
            cc = NULL;
            curop.debug().cursorExhausted = true;

            LOG(5) << "getMore NOT saving client cursor, ended with state "
                   << PlanExecutor::statestr(state) << endl;
        } else {
            // Continue caching the ClientCursor.
            cc->incPos(numResults);
            exec->saveState();
            exec->detachFromOperationContext();
            LOG(5) << "getMore saving client cursor ended with state "
                   << PlanExecutor::statestr(state) << endl;

            // Possibly note slave's position in the oplog.
            if ((cc->queryOptions() & QueryOption_OplogReplay) && !slaveReadTill.isNull()) {
                cc->slaveReadTill(slaveReadTill);
            }

            *exhaust = cc->queryOptions() & QueryOption_Exhaust;

            // If the getmore had a time limit, remaining time is "rolled over" back to the
            // cursor (for use by future getmore ops).
            cc->setLeftoverMaxTimeMicros(curop.getRemainingMaxTimeMicros());
        }
    }

    QueryResult::View qr = bb.buf();
    qr.msgdata().setLen(bb.len());
    qr.msgdata().setOperation(opReply);
    qr.setResultFlags(resultFlags);
    qr.setCursorId(cursorid);
    qr.setStartingFrom(startingResult);
    qr.setNReturned(numResults);
    bb.decouple();
    LOG(5) << "getMore returned " << numResults << " results\n";
    return qr;
}

std::string runQuery(OperationContext* txn,
                     QueryMessage& q,
                     const NamespaceString& nss,
                     Message& result) {
    CurOp& curop = *CurOp::get(txn);
    // Validate the namespace.
    uassert(16256, str::stream() << "Invalid ns [" << nss.ns() << "]", nss.isValid());
    invariant(!nss.isCommand());

    // Set curop information.
    beginQueryOp(txn, nss, q.query, q.ntoreturn, q.ntoskip);

    // Parse the qm into a CanonicalQuery.

    auto statusWithCQ = CanonicalQuery::canonicalize(q, ExtensionsCallbackReal(txn, &nss));
    if (!statusWithCQ.isOK()) {
        uasserted(
            17287,
            str::stream() << "Can't canonicalize query: " << statusWithCQ.getStatus().toString());
    }
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
    invariant(cq.get());

    LOG(5) << "Running query:\n" << cq->toString();
    LOG(2) << "Running query: " << cq->toStringShort();

    // Parse, canonicalize, plan, transcribe, and get a plan executor.
    AutoGetCollectionForRead ctx(txn, nss);
    Collection* collection = ctx.getCollection();

    const int dbProfilingLevel =
        ctx.getDb() ? ctx.getDb()->getProfilingLevel() : serverGlobalParams.defaultProfile;

    // We have a parsed query. Time to get the execution plan for it.
    std::unique_ptr<PlanExecutor> exec = uassertStatusOK(
        getExecutorFind(txn, collection, nss, std::move(cq), PlanExecutor::YIELD_AUTO));

    const LiteParsedQuery& pq = exec->getCanonicalQuery()->getParsed();

    // If it's actually an explain, do the explain and return rather than falling through
    // to the normal query execution loop.
    if (pq.isExplain()) {
        BufBuilder bb;
        bb.skip(sizeof(QueryResult::Value));

        BSONObjBuilder explainBob;
        Explain::explainStages(exec.get(), ExplainCommon::EXEC_ALL_PLANS, &explainBob);

        // Add the resulting object to the return buffer.
        BSONObj explainObj = explainBob.obj();
        bb.appendBuf((void*)explainObj.objdata(), explainObj.objsize());

        // TODO: Does this get overwritten/do we really need to set this twice?
        curop.debug().query = q.query;

        // Set query result fields.
        QueryResult::View qr = bb.buf();
        bb.decouple();
        qr.setResultFlagsToOk();
        qr.msgdata().setLen(bb.len());
        curop.debug().responseLength = bb.len();
        qr.msgdata().setOperation(opReply);
        qr.setCursorId(0);
        qr.setStartingFrom(0);
        qr.setNReturned(1);
        result.setData(qr.view2ptr(), true);
        return "";
    }

    ShardingState* const shardingState = ShardingState::get(txn);

    // We freak out later if this changes before we're done with the query.
    const ChunkVersion shardingVersionAtStart = shardingState->getVersion(nss.ns());

    // Handle query option $maxTimeMS (not used with commands).
    curop.setMaxTimeMicros(static_cast<unsigned long long>(pq.getMaxTimeMS()) * 1000);
    txn->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

    // uassert if we are not on a primary, and not a secondary with SlaveOk query parameter set.
    bool slaveOK = pq.isSlaveOk() || pq.hasReadPref();
    Status serveReadsStatus =
        repl::getGlobalReplicationCoordinator()->checkCanServeReadsFor(txn, nss, slaveOK);
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
    // uint64_t numMisplacedDocs = 0;

    // Get summary info about which plan the executor is using.
    {
        stdx::lock_guard<Client> lk(*txn->getClient());
        curop.setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
    }

    while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
        // Add result to output buffer.
        bb.appendBuf((void*)obj.objdata(), obj.objsize());

        // Count the result.
        ++numResults;

        // Possibly note slave's position in the oplog.
        if (pq.isOplogReplay()) {
            BSONElement e = obj["ts"];
            if (Date == e.type() || bsonTimestamp == e.type()) {
                slaveReadTill = e.timestamp();
            }
        }

        if (FindCommon::enoughForFirstBatch(pq, numResults, bb.len())) {
            LOG(5) << "Enough for first batch, wantMore=" << pq.wantMore()
                   << " ntoreturn=" << pq.getNToReturn().value_or(0) << " numResults=" << numResults
                   << endl;
            break;
        }
    }

    // If we cache the executor later, we want to deregister it as it receives notifications
    // anyway by virtue of being cached.
    //
    // If we don't cache the executor later, we are deleting it, so it must be deregistered.
    //
    // So, no matter what, deregister the executor.
    exec->deregisterExec();

    // Caller expects exceptions thrown in certain cases.
    if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
        const unique_ptr<PlanStageStats> stats(exec->getStats());
        error() << "Plan executor error during find: " << PlanExecutor::statestr(state)
                << ", stats: " << Explain::statsToBSON(*stats);
        uasserted(17144, "Executor error: " + WorkingSetCommon::toStatusString(obj));
    }

    // TODO: Currently, chunk ranges are kept around until all ClientCursors created while the
    // chunk belonged on this node are gone. Separating chunk lifetime management from
    // ClientCursor should allow this check to go away.
    if (!shardingState->getVersion(nss.ns()).isWriteCompatibleWith(shardingVersionAtStart)) {
        // if the version changed during the query we might be missing some data and its safe to
        // send this as mongos can resend at this point
        throw SendStaleConfigException(nss.ns(),
                                       "version changed during initial query",
                                       shardingVersionAtStart,
                                       shardingState->getVersion(nss.ns()));
    }

    // Fill out curop based on query results. If we have a cursorid, we will fill out curop with
    // this cursorid later.
    long long ccId = 0;

    if (shouldSaveCursor(txn, collection, state, exec.get())) {
        // We won't use the executor until it's getMore'd.
        exec->saveState();
        exec->detachFromOperationContext();

        // Allocate a new ClientCursor.  We don't have to worry about leaking it as it's
        // inserted into a global map by its ctor.
        ClientCursor* cc =
            new ClientCursor(collection->getCursorManager(),
                             exec.release(),
                             nss.ns(),
                             txn->recoveryUnit()->isReadingFromMajorityCommittedSnapshot(),
                             pq.getOptions(),
                             pq.getFilter());
        ccId = cc->cursorid();

        LOG(5) << "caching executor with cursorid " << ccId << " after returning " << numResults
               << " results" << endl;

        // TODO document
        if (pq.isOplogReplay() && !slaveReadTill.isNull()) {
            cc->slaveReadTill(slaveReadTill);
        }

        // TODO document
        if (pq.isExhaust()) {
            curop.debug().exhaust = true;
        }

        cc->setPos(numResults);

        // If the query had a time limit, remaining time is "rolled over" to the cursor (for
        // use by future getmore ops).
        cc->setLeftoverMaxTimeMicros(curop.getRemainingMaxTimeMicros());

        endQueryOp(txn, collection, *cc->getExecutor(), dbProfilingLevel, numResults, ccId);
    } else {
        LOG(5) << "Not caching executor but returning " << numResults << " results.\n";
        endQueryOp(txn, collection, *exec, dbProfilingLevel, numResults, ccId);
    }

    // Add the results from the query into the output buffer.
    result.appendData(bb.buf(), bb.len());
    bb.decouple();

    // Fill out the output buffer's header.
    QueryResult::View qr = result.header().view2ptr();
    qr.setCursorId(ccId);
    qr.setResultFlagsToOk();
    qr.msgdata().setOperation(opReply);
    qr.setStartingFrom(0);
    qr.setNReturned(numResults);

    // curop.debug().exhaust is set above.
    return curop.debug().exhaust ? nss.ns() : "";
}

}  // namespace mongo
