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

#include <boost/scoped_ptr.hpp>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/oplogstart.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_constants.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/d_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

using boost::scoped_ptr;
using std::auto_ptr;
using std::endl;

namespace mongo {
    // The .h for this in find_constants.h.
    const int32_t MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;
}  // namespace mongo

namespace {

    /**
     * Returns true if 'me' is a GTE or GE predicate over the "ts" field.
     * Such predicates can be used for the oplog start hack.
     */
    bool isOplogTsPred(const mongo::MatchExpression* me) {
        if (mongo::MatchExpression::GT != me->matchType()
            && mongo::MatchExpression::GTE != me->matchType()) {
            return false;
        }

        return mongoutils::str::equals(me->path().rawData(), "ts");
    }

    mongo::BSONElement extractOplogTsOptime(const mongo::MatchExpression* me) {
        invariant(isOplogTsPred(me));
        return static_cast<const mongo::ComparisonMatchExpression*>(me)->getData();
    }

}  // namespace

namespace mongo {

    // Failpoint for checking whether we've received a getmore.
    MONGO_FP_DECLARE(failReceivedGetmore);

    /**
     * If ntoreturn is zero, we stop generating additional results as soon as we have either 101
     * documents or at least 1MB of data. On subsequent getmores, there is no limit on the number
     * of results; we will stop as soon as we have at least 4 MB of data.  The idea is that on a
     * find() where one doesn't use much results, we don't return much, but once getmore kicks in,
     * we start pushing significant quantities.
     *
     * If ntoreturn is non-zero, the we stop building the first batch once we either have ntoreturn
     * results, or when the result set exceeds 4 MB.
     */
    bool enoughForFirstBatch(const LiteParsedQuery& pq, int numDocs, int bytesBuffered) {
        if (0 == pq.getNumToReturn()) {
            return (bytesBuffered > 1024 * 1024) || numDocs >= 101;
        }
        return numDocs >= pq.getNumToReturn() || bytesBuffered > MaxBytesToReturnToClientAtOnce;
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

        if (pq.getNumToReturn() == 1) {
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

    void beginQueryOp(const NamespaceString& nss,
                      const BSONObj& queryObj,
                      int ntoreturn,
                      int ntoskip,
                      CurOp* curop) {
        curop->debug().ns = nss.ns();
        curop->debug().query = queryObj;
        curop->debug().ntoreturn = ntoreturn;
        curop->debug().ntoskip = ntoskip;
        curop->setQuery(queryObj);
    }

    void endQueryOp(PlanExecutor* exec,
                    int dbProfilingLevel,
                    int numResults,
                    CursorId cursorId,
                    CurOp* curop) {
        invariant(exec);
        invariant(curop);

        // Fill out basic curop query exec properties.
        curop->debug().nreturned = numResults;
        curop->debug().cursorid = (0 == cursorId ? -1 : cursorId);

        // Fill out curop based on explain summary statistics.
        PlanSummaryStats summaryStats;
        Explain::getSummaryStats(exec, &summaryStats);
        curop->debug().scanAndOrder = summaryStats.hasSortStage;
        curop->debug().nscanned = summaryStats.totalKeysExamined;
        curop->debug().nscannedObjects = summaryStats.totalDocsExamined;
        curop->debug().idhack = summaryStats.isIdhack;

        const logger::LogComponent queryLogComponent = logger::LogComponent::kQuery;
        const logger::LogSeverity logLevelOne = logger::LogSeverity::Debug(1);

        // Set debug information for consumption by the profiler and slow query log.
        if (dbProfilingLevel > 0
                || curop->elapsedMillis() > serverGlobalParams.slowMS
                || logger::globalLogDomain()->shouldLog(queryLogComponent, logLevelOne)) {
            // Generate plan summary string.
            curop->debug().planSummary = Explain::getPlanSummary(exec);
        }

        // Set debug information for consumption by the profiler only.
        if (dbProfilingLevel > 0) {
            // Get BSON stats.
            scoped_ptr<PlanStageStats> execStats(exec->getStats());
            BSONObjBuilder statsBob;
            Explain::statsToBSON(*execStats, &statsBob);
            curop->debug().execStats.set(statsBob.obj());

            // Replace exec stats with plan summary if stats cannot fit into CachedBSONObj.
            if (curop->debug().execStats.tooBig() && !curop->debug().planSummary.empty()) {
                BSONObjBuilder bob;
                bob.append("summary", curop->debug().planSummary.toString());
                curop->debug().execStats.set(bob.done());
            }
        }
    }

    struct ScopedRecoveryUnitSwapper {
        explicit ScopedRecoveryUnitSwapper(ClientCursor* cc, OperationContext* txn)
            : _cc(cc), _txn(txn) {

            // Save this for later.  We restore it upon destruction.
            _txn->recoveryUnit()->commitAndRestart();
            _txnPreviousRecoveryUnit = txn->releaseRecoveryUnit();

            // Transfer ownership of the RecoveryUnit from the ClientCursor to the OpCtx.
            RecoveryUnit* ccRecoveryUnit = cc->releaseOwnedRecoveryUnit();
            txn->setRecoveryUnit(ccRecoveryUnit);
        }

        ~ScopedRecoveryUnitSwapper() {
            _txn->recoveryUnit()->commitAndRestart();
            _cc->setOwnedRecoveryUnit(_txn->releaseRecoveryUnit());
            _txn->setRecoveryUnit(_txnPreviousRecoveryUnit);
        }

        ClientCursor* _cc;
        OperationContext* _txn;
        RecoveryUnit* _txnPreviousRecoveryUnit;
    };

    /**
     * Called by db/instance.cpp.  This is the getMore entry point.
     *
     * pass - when QueryOption_AwaitData is in use, the caller will make repeated calls 
     *        when this method returns an empty result, incrementing pass on each call.  
     *        Thus, pass == 0 indicates this is the first "attempt" before any 'awaiting'.
     */
    QueryResult::View getMore(OperationContext* txn,
                              const char* ns,
                              int ntoreturn,
                              long long cursorid,
                              CurOp& curop,
                              int pass,
                              bool& exhaust,
                              bool* isCursorAuthorized) {

        // For testing, we may want to fail if we receive a getmore.
        if (MONGO_FAIL_POINT(failReceivedGetmore)) {
            invariant(0);
        }

        exhaust = false;

        const NamespaceString nss(ns);

        // Depending on the type of cursor being operated on, we hold locks for the whole getMore,
        // or none of the getMore, or part of the getMore.  The three cases in detail:
        //
        // 1) Normal cursor: we lock with "ctx" and hold it for the whole getMore.
        // 2) Cursor owned by global cursor manager: we don't lock anything.  These cursors don't
        //    own any collection state.
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
        boost::scoped_ptr<AutoGetCollectionForRead> ctx;
        boost::scoped_ptr<Lock::DBLock> unpinDBLock;
        boost::scoped_ptr<Lock::CollectionLock> unpinCollLock;

        CursorManager* cursorManager;
        CursorManager* globalCursorManager = CursorManager::getGlobalCursorManager();
        if (globalCursorManager->ownsCursorId(cursorid)) {
            cursorManager = globalCursorManager;
        }
        else {
            ctx.reset(new AutoGetCollectionForRead(txn, nss));
            Collection* collection = ctx->getCollection();
            uassert( 17356, "collection dropped between getMore calls", collection );
            cursorManager = collection->getCursorManager();
        }

        LOG(5) << "Running getMore, cursorid: " << cursorid << endl;

        // This checks to make sure the operation is allowed on a replicated node.  Since we are not
        // passing in a query object (necessary to check SlaveOK query option), the only state where
        // reads are allowed is PRIMARY (or master in master/slave).  This function uasserts if
        // reads are not okay.
        Status status = repl::getGlobalReplicationCoordinator()->checkCanServeReadsFor(
                txn,
                nss,
                true);
        uassertStatusOK(status);

        // A pin performs a CC lookup and if there is a CC, increments the CC's pin value so it
        // doesn't time out.  Also informs ClientCursor that there is somebody actively holding the
        // CC, so don't delete it.
        ClientCursorPin ccPin(cursorManager, cursorid);
        ClientCursor* cc = ccPin.c();

        // If we're not being called from DBDirectClient we want to associate the RecoveryUnit
        // used to create the execution machinery inside the cursor with our OperationContext.
        // If we throw or otherwise exit this method in a disorderly fashion, we must ensure
        // that further calls to getMore won't fail, and that the provided OperationContext
        // has a valid RecoveryUnit.  As such, we use RAII to accomplish this.
        //
        // This must be destroyed before the ClientCursor is destroyed.
        std::auto_ptr<ScopedRecoveryUnitSwapper> ruSwapper;

        // These are set in the QueryResult msg we return.
        int resultFlags = ResultFlag_AwaitCapable;

        int numResults = 0;
        int startingResult = 0;

        const int InitialBufSize =
            512 + sizeof(QueryResult::Value) + MaxBytesToReturnToClientAtOnce;

        BufBuilder bb(InitialBufSize);
        bb.skip(sizeof(QueryResult::Value));

        if (NULL == cc) {
            cursorid = 0;
            resultFlags = ResultFlag_CursorNotFound;
        }
        else {
            // Check for spoofing of the ns such that it does not match the one originally
            // there for the cursor.
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Requested getMore on namespace " << ns << ", but cursor "
                                  << cursorid << " belongs to namespace " << cc->ns(),
                    ns == cc->ns());
            *isCursorAuthorized = true;

            // Restore the RecoveryUnit if we need to.
            if (txn->getClient()->isInDirectClient()) {
                if (cc->hasRecoveryUnit())
                    invariant(txn->recoveryUnit() == cc->getUnownedRecoveryUnit());
            }
            else {
                if (!cc->hasRecoveryUnit()) {
                    // Start using a new RecoveryUnit
                    cc->setOwnedRecoveryUnit(
                        getGlobalEnvironment()->getGlobalStorageEngine()->newRecoveryUnit());

                }
                // Swap RecoveryUnit(s) between the ClientCursor and OperationContext.
                ruSwapper.reset(new ScopedRecoveryUnitSwapper(cc, txn));
            }

            // Reset timeout timer on the cursor since the cursor is still in use.
            cc->setIdleTime(0);

            // TODO: fail point?

            // If the operation that spawned this cursor had a time limit set, apply leftover
            // time to this getmore.
            curop.setMaxTimeMicros(cc->getLeftoverMaxTimeMicros());
            txn->checkForInterrupt(); // May trigger maxTimeAlwaysTimeOut fail point.

            if (0 == pass) { 
                cc->updateSlaveLocation(txn, curop); 
            }

            if (cc->isAggCursor()) {
                // Agg cursors handle their own locking internally.
                ctx.reset(); // unlocks
            }

            // If we're replaying the oplog, we save the last time that we read.
            OpTime slaveReadTill;

            // What number result are we starting at?  Used to fill out the reply.
            startingResult = cc->pos();

            // What gives us results.
            PlanExecutor* exec = cc->getExecutor();
            const int queryOptions = cc->queryOptions();

            // Get results out of the executor.
            exec->restoreState(txn);

            BSONObj obj;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
                // Add result to output buffer.
                bb.appendBuf((void*)obj.objdata(), obj.objsize());

                // Count the result.
                ++numResults;

                // Possibly note slave's position in the oplog.
                if (queryOptions & QueryOption_OplogReplay) {
                    BSONElement e = obj["ts"];
                    if (Date == e.type() || Timestamp == e.type()) {
                        slaveReadTill = e._opTime();
                    }
                }

                if ((ntoreturn && numResults >= ntoreturn)
                    || bb.len() > MaxBytesToReturnToClientAtOnce) {
                    break;
                }
            }

            // We save the client cursor when there might be more results, and hence we may receive
            // another getmore. If we receive a EOF or an error, or 'exec' is dead, then we know
            // that we will not be producing more results. We indicate that the cursor is closed by
            // sending a cursorId of 0 back to the client.
            //
            // On the other hand, if we retrieve all results necessary for this batch, then
            // 'saveClientCursor' is true and we send a valid cursorId back to the client. In
            // this case, there may or may not actually be more results (for example, the next call
            // to getNext(...) might just return EOF).
            bool saveClientCursor = false;

            if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
                // Propagate this error to caller.
                if (PlanExecutor::FAILURE == state) {
                    scoped_ptr<PlanStageStats> stats(exec->getStats());
                    error() << "Plan executor error, stats: "
                            << Explain::statsToBSON(*stats);
                    uasserted(17406, "getMore executor error: " +
                              WorkingSetCommon::toStatusString(obj));
                }

                // If we're dead there's no way to get more results.
                saveClientCursor = false;

                // In the old system tailable capped cursors would be killed off at the
                // cursorid level.  If a tailable capped cursor is nuked the cursorid
                // would vanish.
                //
                // In the new system they die and are cleaned up later (or time out).
                // So this is where we get to remove the cursorid.
                if (0 == numResults) {
                    resultFlags = ResultFlag_CursorNotFound;
                }
            }
            else if (PlanExecutor::IS_EOF == state) {
                // EOF is also end of the line unless it's tailable.
                saveClientCursor = queryOptions & QueryOption_CursorTailable;
            }
            else {
                verify(PlanExecutor::ADVANCED == state);
                saveClientCursor = true;
            }

            // If we are operating on an aggregation cursor, then we dropped our collection lock
            // earlier and need to reacquire it in order to clean up our ClientCursorPin.
            //
            // TODO: We need to ensure that this relock happens if we release the pin above in
            // response to PlanExecutor::getNext() throwing an exception.
            if (cc->isAggCursor()) {
                invariant(NULL == ctx.get());
                unpinDBLock.reset(new Lock::DBLock(txn->lockState(), nss.db(), MODE_IS));
                unpinCollLock.reset(new Lock::CollectionLock(txn->lockState(), nss.ns(), MODE_IS));
            }

            // Our two possible ClientCursorPin cleanup paths are:
            // 1) If the cursor is not going to be saved, we call deleteUnderlying() on the pin.
            // 2) If the cursor is going to be saved, we simply let the pin go out of scope.  In
            //    this case, the pin's destructor will be invoked, which will call release() on the
            //    pin.  Because our ClientCursorPin is declared after our lock is declared, this
            //    will happen under the lock.
            if (!saveClientCursor) {
                ruSwapper.reset();
                ccPin.deleteUnderlying();
                // cc is now invalid, as is the executor
                cursorid = 0;
                cc = NULL;
                LOG(5) << "getMore NOT saving client cursor, ended with state "
                       << PlanExecutor::statestr(state)
                       << endl;
            }
            else {
                // Continue caching the ClientCursor.
                cc->incPos(numResults);
                exec->saveState();
                LOG(5) << "getMore saving client cursor ended with state "
                       << PlanExecutor::statestr(state)
                       << endl;

                if (PlanExecutor::IS_EOF == state && (queryOptions & QueryOption_CursorTailable)) {
                    if (!txn->getClient()->isInDirectClient()) {
                        // Don't stash the RU. Get a new one on the next getMore.
                        ruSwapper.reset();
                        delete cc->releaseOwnedRecoveryUnit();
                    }

                    if ((queryOptions & QueryOption_AwaitData)
                            && (numResults == 0)
                            && (pass < 1000)) {
                        // Bubble up to the AwaitData handling code in receivedGetMore which will
                        // try again.
                        return NULL;
                    }
                }

                // Possibly note slave's position in the oplog.
                if ((queryOptions & QueryOption_OplogReplay) && !slaveReadTill.isNull()) {
                    cc->slaveReadTill(slaveReadTill);
                }

                exhaust = (queryOptions & QueryOption_Exhaust);

                // If the getmore had a time limit, remaining time is "rolled over" back to the
                // cursor (for use by future getmore ops).
                cc->setLeftoverMaxTimeMicros( curop.getRemainingMaxTimeMicros() );
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

    Status getOplogStartHack(OperationContext* txn,
                             Collection* collection,
                             CanonicalQuery* cq,
                             PlanExecutor** execOut) {
        invariant(cq);
        auto_ptr<CanonicalQuery> autoCq(cq);

        if ( collection == NULL )
            return Status(ErrorCodes::InternalError,
                          "getOplogStartHack called with a NULL collection" );

        // A query can only do oplog start finding if it has a top-level $gt or $gte predicate over
        // the "ts" field (the operation's timestamp). Find that predicate and pass it to
        // the OplogStart stage.
        MatchExpression* tsExpr = NULL;
        if (MatchExpression::AND == cq->root()->matchType()) {
            // The query has an AND at the top-level. See if any of the children
            // of the AND are $gt or $gte predicates over 'ts'.
            for (size_t i = 0; i < cq->root()->numChildren(); ++i) {
                MatchExpression* me = cq->root()->getChild(i);
                if (isOplogTsPred(me)) {
                    tsExpr = me;
                    break;
                }
            }
        }
        else if (isOplogTsPred(cq->root())) {
            // The root of the tree is a $gt or $gte predicate over 'ts'.
            tsExpr = cq->root();
        }

        if (NULL == tsExpr) {
            return Status(ErrorCodes::OplogOperationUnsupported,
                          "OplogReplay query does not contain top-level "
                          "$gt or $gte over the 'ts' field.");
        }

        boost::optional<RecordId> startLoc = boost::none;

        // See if the RecordStore supports the oplogStartHack
        const BSONElement tsElem = extractOplogTsOptime(tsExpr);
        if (tsElem.type() == Timestamp) {
            StatusWith<RecordId> goal = oploghack::keyForOptime(tsElem._opTime());
            if (goal.isOK()) {
                startLoc = collection->getRecordStore()->oplogStartHack(txn, goal.getValue());
            }
        }

        if (startLoc) {
            LOG(3) << "Using direct oplog seek";
        }
        else {
            LOG(3) << "Using OplogStart stage";

            // Fallback to trying the OplogStart stage.
            WorkingSet* oplogws = new WorkingSet();
            OplogStart* stage = new OplogStart(txn, collection, tsExpr, oplogws);
            PlanExecutor* rawExec;

            // Takes ownership of oplogws and stage.
            Status execStatus = PlanExecutor::make(txn, oplogws, stage, collection,
                                                   PlanExecutor::YIELD_AUTO, &rawExec);
            invariant(execStatus.isOK());
            scoped_ptr<PlanExecutor> exec(rawExec);

            // The stage returns a RecordId of where to start.
            startLoc = RecordId();
            PlanExecutor::ExecState state = exec->getNext(NULL, startLoc.get_ptr());

            // This is normal.  The start of the oplog is the beginning of the collection.
            if (PlanExecutor::IS_EOF == state) {
                return getExecutor(txn, collection, autoCq.release(), PlanExecutor::YIELD_AUTO,
                                   execOut);
            }

            // This is not normal.  An error was encountered.
            if (PlanExecutor::ADVANCED != state) {
                return Status(ErrorCodes::InternalError,
                              "quick oplog start location had error...?");
            }
        }

        // Build our collection scan...
        CollectionScanParams params;
        params.collection = collection;
        params.start = *startLoc;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = cq->getParsed().isTailable();

        WorkingSet* ws = new WorkingSet();
        CollectionScan* cs = new CollectionScan(txn, params, ws, cq->root());
        // Takes ownership of 'ws', 'cs', and 'cq'.
        return PlanExecutor::make(txn, ws, cs, autoCq.release(), collection,
                                  PlanExecutor::YIELD_AUTO, execOut);
    }

    std::string runQuery(OperationContext* txn,
                         QueryMessage& q,
                         const NamespaceString& nss,
                         CurOp& curop,
                         Message &result) {
        // Validate the namespace.
        uassert(16256, str::stream() << "Invalid ns [" << nss.ns() << "]", nss.isValid());
        invariant(!nss.isCommand());

        // Set curop information.
        beginQueryOp(nss, q.query, q.ntoreturn, q.ntoskip, &curop);

        // Parse the qm into a CanonicalQuery.
        std::auto_ptr<CanonicalQuery> cq;
        {
            CanonicalQuery* cqRaw;
            Status canonStatus = CanonicalQuery::canonicalize(q,
                                                              &cqRaw,
                                                              WhereCallbackReal(txn, nss.db()));
            if (!canonStatus.isOK()) {
                uasserted(17287, str::stream() << "Can't canonicalize query: "
                                               << canonStatus.toString());
            }
            cq.reset(cqRaw);
        }
        invariant(cq.get());

        LOG(5) << "Running query:\n" << cq->toString();
        LOG(2) << "Running query: " << cq->toStringShort();

        // Parse, canonicalize, plan, transcribe, and get a plan executor.
        PlanExecutor* rawExec = NULL;

        AutoGetCollectionForRead ctx(txn, nss);
        Collection* collection = ctx.getCollection();

        const int dbProfilingLevel = ctx.getDb() ? ctx.getDb()->getProfilingLevel() :
                                                   serverGlobalParams.defaultProfile;

        // We'll now try to get the query executor that will execute this query for us. There
        // are a few cases in which we know upfront which executor we should get and, therefore,
        // we shortcut the selection process here.
        //
        // (a) If the query is over a collection that doesn't exist, we use an EOFStage.
        //
        // (b) if the query is a replication's initial sync one, we use a specifically designed
        // stage that skips extents faster (see details in exec/oplogstart.h).
        //
        // Otherwise we go through the selection of which executor is most suited to the
        // query + run-time context at hand.
        Status status = Status::OK();
        if (NULL != collection && cq->getParsed().isOplogReplay()) {
            status = getOplogStartHack(txn, collection, cq.release(), &rawExec);
        }
        else {
            size_t options = QueryPlannerParams::DEFAULT;
            if (shardingState.needCollectionMetadata(nss.ns())) {
                options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
            }
            status = getExecutor(txn, collection, cq.release(), PlanExecutor::YIELD_AUTO, &rawExec,
                                 options);
        }
        invariant(cq.get() == NULL); // cq has been released above.

        if (!status.isOK()) {
            uasserted(17007, "Unable to execute query: " + status.reason());
        }

        verify(NULL != rawExec);
        auto_ptr<PlanExecutor> exec(rawExec);

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

        // We freak out later if this changes before we're done with the query.
        const ChunkVersion shardingVersionAtStart = shardingState.getVersion(nss.ns());

        // Handle query option $maxTimeMS (not used with commands).
        curop.setMaxTimeMicros(static_cast<unsigned long long>(pq.getMaxTimeMS()) * 1000);
        txn->checkForInterrupt(); // May trigger maxTimeAlwaysTimeOut fail point.

        // uassert if we are not on a primary, and not a secondary with SlaveOk query parameter set.
        bool slaveOK = pq.isSlaveOk() || pq.hasReadPref();
        status = repl::getGlobalReplicationCoordinator()->checkCanServeReadsFor(
                txn,
                nss,
                slaveOK);
        uassertStatusOK(status);

        // Run the query.
        // bb is used to hold query results
        // this buffer should contain either requested documents per query or
        // explain information, but not both
        BufBuilder bb(32768);
        bb.skip(sizeof(QueryResult::Value));

        // How many results have we obtained from the executor?
        int numResults = 0;

        // If we're replaying the oplog, we save the last time that we read.
        OpTime slaveReadTill;

        BSONObj obj;
        PlanExecutor::ExecState state;
        // uint64_t numMisplacedDocs = 0;

        // Get summary info about which plan the executor is using.
        curop.debug().planSummary = Explain::getPlanSummary(exec.get());

        while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
            // Add result to output buffer.
            bb.appendBuf((void*)obj.objdata(), obj.objsize());

            // Count the result.
            ++numResults;

            // Possibly note slave's position in the oplog.
            if (pq.isOplogReplay()) {
                BSONElement e = obj["ts"];
                if (Date == e.type() || Timestamp == e.type()) {
                    slaveReadTill = e._opTime();
                }
            }

            if (enoughForFirstBatch(pq, numResults, bb.len())) {
                LOG(5) << "Enough for first batch, wantMore=" << pq.wantMore()
                       << " numToReturn=" << pq.getNumToReturn()
                       << " numResults=" << numResults
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
        if (PlanExecutor::FAILURE == state) {
            scoped_ptr<PlanStageStats> stats(exec->getStats());
            error() << "Plan executor error, stats: "
                    << Explain::statsToBSON(*stats);
            uasserted(17144, "Executor error: " + WorkingSetCommon::toStatusString(obj));
        }

        // TODO(greg): This will go away soon.
        if (!shardingState.getVersion(nss.ns()).isWriteCompatibleWith(shardingVersionAtStart)) {
            // if the version changed during the query we might be missing some data and its safe to
            // send this as mongos can resend at this point
            throw SendStaleConfigException(nss.ns(), "version changed during initial query",
                                           shardingVersionAtStart,
                                           shardingState.getVersion(nss.ns()));
        }

        // Fill out curop based on query results. If we have a cursorid, we will fill out curop with
        // this cursorid later.
        long long ccId = 0;
        endQueryOp(exec.get(), dbProfilingLevel, numResults, ccId, &curop);

        if (shouldSaveCursor(txn, collection, state, exec.get())) {
            // We won't use the executor until it's getMore'd.
            exec->saveState();

            // Allocate a new ClientCursor.  We don't have to worry about leaking it as it's
            // inserted into a global map by its ctor.
            ClientCursor* cc = new ClientCursor(collection->getCursorManager(),
                                                exec.release(),
                                                nss.ns(),
                                                pq.getOptions(),
                                                pq.getFilter());
            ccId = cc->cursorid();

            if (txn->getClient()->isInDirectClient()) {
                cc->setUnownedRecoveryUnit(txn->recoveryUnit());
            }
            else if (state == PlanExecutor::IS_EOF && pq.isTailable()) {
                // Don't stash the RU for tailable cursors at EOF, let them get a new RU on their
                // next getMore.
            }
            else {
                // We stash away the RecoveryUnit in the ClientCursor.  It's used for subsequent
                // getMore requests.  The calling OpCtx gets a fresh RecoveryUnit.
                txn->recoveryUnit()->commitAndRestart();
                cc->setOwnedRecoveryUnit(txn->releaseRecoveryUnit());
                StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
                txn->setRecoveryUnit(storageEngine->newRecoveryUnit());
            }

            LOG(5) << "caching executor with cursorid " << ccId
                   << " after returning " << numResults << " results" << endl;

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
        }
        else {
            LOG(5) << "Not caching executor but returning " << numResults << " results.\n";
        }

        // Add the results from the query into the output buffer.
        result.appendData(bb.buf(), bb.len());
        bb.decouple();

        // Fill out the output buffer's header.
        QueryResult::View qr = result.header().view2ptr();
        qr.setCursorId(ccId);
        curop.debug().cursorid = (0 == ccId ? -1 : ccId);
        qr.setResultFlagsToOk();
        qr.msgdata().setOperation(opReply);
        qr.setStartingFrom(0);
        qr.setNReturned(numResults);

        // curop.debug().exhaust is set above.
        return curop.debug().exhaust ? nss.ns() : "";
    }

}  // namespace mongo
