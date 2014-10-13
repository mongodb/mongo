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

#include "mongo/db/query/new_find.h"

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
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/d_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
    // The .h for this in find_constants.h.
    const int32_t MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;
}  // namespace mongo

namespace {

    // TODO: Remove this or use it.
    bool hasIndexSpecifier(const mongo::LiteParsedQuery& pq) {
        return !pq.getHint().isEmpty() || !pq.getMin().isEmpty() || !pq.getMax().isEmpty();
    }

    /**
     * Quote:
     * if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there
     * is only a size limit.  The idea is that on a find() where one doesn't use much results,
     * we don't return much, but once getmore kicks in, we start pushing significant quantities.
     *
     * The n limit (vs. size) is important when someone fetches only one small field from big
     * objects, which causes massive scanning server-side.
     */
    bool enoughForFirstBatch(const mongo::LiteParsedQuery& pq, int n, int len) {
        if (0 == pq.getNumToReturn()) {
            return (len > 1024 * 1024) || n >= 101;
        }
        return n >= pq.getNumToReturn() || len > mongo::MaxBytesToReturnToClientAtOnce;
    }

    bool enough(const mongo::LiteParsedQuery& pq, int n) {
        if (0 == pq.getNumToReturn()) { return false; }
        return n >= pq.getNumToReturn();
    }

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

}  // namespace

namespace mongo {

    // Failpoint for checking whether we've received a getmore.
    MONGO_FP_DECLARE(failReceivedGetmore);

    // TODO: Move this and the other command stuff in newRunQuery outta here and up a level.
    static bool runCommands(OperationContext* txn,
                            const char *ns,
                            BSONObj& jsobj,
                            CurOp& curop,
                            BufBuilder &b,
                            BSONObjBuilder& anObjBuilder,
                            bool fromRepl,
                            int queryOptions) {
        try {
            return _runCommands(txn, ns, jsobj, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch( SendStaleConfigException& ){
            throw;
        }
        catch ( AssertionException& e ) {
            verify( e.getCode() != SendStaleConfigCode && e.getCode() != RecvStaleConfigCode );

            Command::appendCommandStatus(anObjBuilder, e.toStatus());
            curop.debug().exceptionInfo = e.getInfo();
        }
        BSONObj x = anObjBuilder.done();
        b.appendBuf((void*) x.objdata(), x.objsize());
        return true;
    }

    struct ScopedRecoveryUnitSwapper {
        explicit ScopedRecoveryUnitSwapper(ClientCursor* cc, OperationContext* txn)
            : _cc(cc), _txn(txn) {

            // Save this for later.  We restore it upon destruction.
            _txnPreviousRecoveryUnit = txn->releaseRecoveryUnit();

            // Transfer ownership of the RecoveryUnit from the ClientCursor to the OpCtx.
            RecoveryUnit* ccRecoveryUnit = cc->releaseOwnedRecoveryUnit();
            txn->setRecoveryUnit(ccRecoveryUnit);
        }

        ~ScopedRecoveryUnitSwapper() {
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
    QueryResult::View newGetMore(OperationContext* txn,
                            const char* ns,
                            int ntoreturn,
                            long long cursorid,
                            CurOp& curop,
                            int pass,
                            bool& exhaust,
                            bool* isCursorAuthorized,
                            bool fromDBDirectClient) {

        // For testing, we may want to fail if we receive a getmore.
        if (MONGO_FAIL_POINT(failReceivedGetmore)) {
            invariant(0);
        }

        exhaust = false;

        // This is a read lock.
        const NamespaceString nss(ns);
        scoped_ptr<AutoGetCollectionForRead> ctx(new AutoGetCollectionForRead(txn, nss));
        Collection* collection = ctx->getCollection();
        uassert( 17356, "collection dropped between getMore calls", collection );

        QLOG() << "Running getMore, cursorid: " << cursorid << endl;

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
        ClientCursorPin ccPin(collection, cursorid);
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
            // Quote: check for spoofing of the ns such that it does not match the one originally
            // there for the cursor
            uassert(17011, "auth error", str::equals(ns, cc->ns().c_str()));
            *isCursorAuthorized = true;

            // Restore the RecoveryUnit if we need to.
            if (fromDBDirectClient) {
                invariant(txn->recoveryUnit() == cc->getUnownedRecoveryUnit());
            }
            else {
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

            if (cc->isAggCursor) {
                // Agg cursors handle their own locking internally.
                ctx.reset(); // unlocks
            }

            CollectionMetadataPtr collMetadata = cc->getCollMetadata();

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

            if (PlanExecutor::IS_EOF == state && 0 == numResults
                && (queryOptions & QueryOption_CursorTailable)
                && (queryOptions & QueryOption_AwaitData) && (pass < 1000)) {
                // If the cursor is tailable we don't kill it if it's eof.  We let it try to get
                // data some # of times first.
                exec->saveState();
                return 0;
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

            if (PlanExecutor::DEAD == state || PlanExecutor::EXEC_ERROR == state) {
                // Propagate this error to caller.
                if (PlanExecutor::EXEC_ERROR == state) {
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

            if (!saveClientCursor) {
                ruSwapper.reset();
                ccPin.deleteUnderlying();
                // cc is now invalid, as is the executor
                cursorid = 0;
                cc = NULL;
                QLOG() << "getMore NOT saving client cursor, ended with state "
                       << PlanExecutor::statestr(state)
                       << endl;
            }
            else {
                // Continue caching the ClientCursor.
                cc->incPos(numResults);
                exec->saveState();
                QLOG() << "getMore saving client cursor ended with state "
                       << PlanExecutor::statestr(state)
                       << endl;

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
        QLOG() << "getMore returned " << numResults << " results\n";
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

        // Make an oplog start finding stage.
        WorkingSet* oplogws = new WorkingSet();
        OplogStart* stage = new OplogStart(txn, collection, tsExpr, oplogws);

        // Takes ownership of ws and stage.
        auto_ptr<PlanExecutor> exec(new PlanExecutor(txn, oplogws, stage, collection));
        exec->registerExecInternalPlan();

        // The stage returns a DiskLoc of where to start.
        DiskLoc startLoc;
        PlanExecutor::ExecState state = exec->getNext(NULL, &startLoc);

        // This is normal.  The start of the oplog is the beginning of the collection.
        if (PlanExecutor::IS_EOF == state) {
            return getExecutor(txn, collection, autoCq.release(), execOut);
        }

        // This is not normal.  An error was encountered.
        if (PlanExecutor::ADVANCED != state) {
            return Status(ErrorCodes::InternalError,
                          "quick oplog start location had error...?");
        }

        // cout << "diskloc is " << startLoc.toString() << endl;

        // Build our collection scan...
        CollectionScanParams params;
        params.collection = collection;
        params.start = startLoc;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = cq->getParsed().getOptions().tailable;

        WorkingSet* ws = new WorkingSet();
        CollectionScan* cs = new CollectionScan(txn, params, ws, cq->root());
        // Takes ownership of 'ws', 'cs', and 'cq'.
        *execOut = new PlanExecutor(txn, ws, cs, autoCq.release(), collection);
        return Status::OK();
    }

    std::string newRunQuery(OperationContext* txn,
                            Message& m,
                            QueryMessage& q,
                            CurOp& curop,
                            Message &result,
                            bool fromDBDirectClient) {
        // Validate the namespace.
        const char *ns = q.ns;
        uassert(16332, "can't have an empty ns", ns[0]);

        const NamespaceString nsString(ns);
        uassert(16256, str::stream() << "Invalid ns [" << ns << "]", nsString.isValid());

        // Set curop information.
        curop.debug().ns = ns;
        curop.debug().ntoreturn = q.ntoreturn;
        curop.debug().query = q.query;
        curop.setQuery(q.query);

        // If the query is really a command, run it.
        if (nsString.isCommand()) {
            int nToReturn = q.ntoreturn;
            uassert(16979, str::stream() << "bad numberToReturn (" << nToReturn
                                         << ") for $cmd type ns - can only be 1 or -1",
                    nToReturn == 1 || nToReturn == -1);

            curop.markCommand();

            BufBuilder bb;
            bb.skip(sizeof(QueryResult::Value));

            BSONObjBuilder cmdResBuf;
            if (!runCommands(txn, ns, q.query, curop, bb, cmdResBuf, false, q.queryOptions)) {
                uasserted(13530, "bad or malformed command request?");
            }

            curop.debug().iscommand = true;
            // TODO: Does this get overwritten/do we really need to set this twice?
            curop.debug().query = q.query;

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

        const NamespaceString nss(q.ns);

        // Parse the qm into a CanonicalQuery.
        CanonicalQuery* cq;
        Status canonStatus = CanonicalQuery::canonicalize(
                                    q, &cq, WhereCallbackReal(txn, StringData(nss.db())));
        if (!canonStatus.isOK()) {
            uasserted(17287, str::stream() << "Can't canonicalize query: " << canonStatus.toString());
        }

        QLOG() << "Running query:\n" << cq->toString();
        LOG(2) << "Running query: " << cq->toStringShort();

        // Parse, canonicalize, plan, transcribe, and get a plan executor.
        PlanExecutor* rawExec = NULL;

        // We use this a lot below.
        const LiteParsedQuery& pq = cq->getParsed();

        AutoGetCollectionForRead ctx(txn, nss);

        const int dbProfilingLevel = (ctx.getDb() != NULL) ? ctx.getDb()->getProfilingLevel() :
                                                             serverGlobalParams.defaultProfile;

        Collection* collection = ctx.getCollection();

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
        if (collection == NULL) {
            LOG(2) << "Collection " << ns << " does not exist."
                   << " Using EOF stage: " << cq->toStringShort();
            EOFStage* eofStage = new EOFStage();
            WorkingSet* ws = new WorkingSet();
            // Takes ownership of 'cq'.
            rawExec = new PlanExecutor(txn, ws, eofStage, cq, NULL);
        }
        else if (pq.getOptions().oplogReplay) {
            // Takes ownership of 'cq'.
            status = getOplogStartHack(txn, collection, cq, &rawExec);
        }
        else {
            size_t options = QueryPlannerParams::DEFAULT;
            if (shardingState.needCollectionMetadata(pq.ns())) {
                options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
            }
            // Takes ownership of 'cq'.
            status = getExecutor(txn, collection, cq, &rawExec, options);
        }

        if (!status.isOK()) {
            // NOTE: Do not access cq as getExecutor has deleted it.
            uasserted(17007, "Unable to execute query: " + status.reason());
        }

        verify(NULL != rawExec);
        auto_ptr<PlanExecutor> exec(rawExec);

        // If it's actually an explain, do the explain and return rather than falling through
        // to the normal query execution loop.
        if (pq.isExplain()) {
            BufBuilder bb;
            bb.skip(sizeof(QueryResult::Value));

            BSONObjBuilder explainBob;
            Status explainStatus = Explain::explainStages(txn,
                                                          exec.get(),
                                                          ExplainCommon::EXEC_ALL_PLANS,
                                                          &explainBob);
            if (!explainStatus.isOK()) {
                uasserted(18521, "Explain error: " + explainStatus.reason());
            }

            // Add the resulting object to the return buffer.
            BSONObj explainObj = explainBob.obj();
            bb.appendBuf((void*)explainObj.objdata(), explainObj.objsize());

            curop.debug().iscommand = true;
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
        const ChunkVersion shardingVersionAtStart = shardingState.getVersion(cq->ns());

        // Handle query option $maxTimeMS (not used with commands).
        curop.setMaxTimeMicros(static_cast<unsigned long long>(pq.getMaxTimeMS()) * 1000);
        txn->checkForInterrupt(); // May trigger maxTimeAlwaysTimeOut fail point.

        // uassert if we are not on a primary, and not a secondary with SlaveOk query parameter set.
        bool slaveOK = pq.getOptions().slaveOk || pq.hasReadPref();
        status = repl::getGlobalReplicationCoordinator()->checkCanServeReadsFor(
                txn,
                NamespaceString(cq->ns()),
                slaveOK);
        uassertStatusOK(status);

        // If this exists, the collection is sharded.
        // If it doesn't exist, we can assume we're not sharded.
        // If we're sharded, we might encounter data that is not consistent with our sharding state.
        // We must ignore this data.
        CollectionMetadataPtr collMetadata;
        if (!shardingState.needCollectionMetadata(pq.ns())) {
            collMetadata = CollectionMetadataPtr();
        }
        else {
            collMetadata = shardingState.getCollectionMetadata(pq.ns());
        }

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

        // Do we save the PlanExecutor in a ClientCursor for getMore calls later?
        bool saveClientCursor = false;

        // The executor registers itself with the active executors list in ClientCursor.
        auto_ptr<ScopedExecutorRegistration> safety(new ScopedExecutorRegistration(exec.get()));

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
            if (pq.getOptions().oplogReplay) {
                BSONElement e = obj["ts"];
                if (Date == e.type() || Timestamp == e.type()) {
                    slaveReadTill = e._opTime();
                }
            }

            // TODO: only one type of 2d search doesn't support this.  We need a way to pull it out
            // of CanonicalQuery. :(
            const bool supportsGetMore = true;
            if (!supportsGetMore && (enough(pq, numResults)
                                     || bb.len() >= MaxBytesToReturnToClientAtOnce)) {
                break;
            }
            else if (enoughForFirstBatch(pq, numResults, bb.len())) {
                QLOG() << "Enough for first batch, wantMore=" << pq.wantMore()
                       << " numToReturn=" << pq.getNumToReturn()
                       << " numResults=" << numResults
                       << endl;
                // If only one result requested assume it's a findOne() and don't save the cursor.
                if (pq.wantMore() && 1 != pq.getNumToReturn()) {
                    QLOG() << " executor EOF=" << exec->isEOF() << endl;
                    saveClientCursor = !exec->isEOF();
                }
                break;
            }
        }

        // If we cache the executor later, we want to deregister it as it receives notifications
        // anyway by virtue of being cached.
        //
        // If we don't cache the executor later, we are deleting it, so it must be deregistered.
        //
        // So, no matter what, deregister the executor.
        safety.reset();

        // Caller expects exceptions thrown in certain cases.
        if (PlanExecutor::EXEC_ERROR == state) {
            scoped_ptr<PlanStageStats> stats(exec->getStats());
            error() << "Plan executor error, stats: "
                    << Explain::statsToBSON(*stats);
            uasserted(17144, "Executor error: " + WorkingSetCommon::toStatusString(obj));
        }

        // Why save a dead executor?
        if (PlanExecutor::DEAD == state) {
            saveClientCursor = false;
        }
        else if (pq.getOptions().tailable) {
            // If we're tailing a capped collection, we don't bother saving the cursor if the
            // collection is empty. Otherwise, the semantics of the tailable cursor is that the
            // client will keep trying to read from it. So we'll keep it around.
            if (collection && collection->numRecords(txn) != 0 && pq.getNumToReturn() != 1) {
                saveClientCursor = true;
            }
        }

        // TODO(greg): This will go away soon.
        if (!shardingState.getVersion(pq.ns()).isWriteCompatibleWith(shardingVersionAtStart)) {
            // if the version changed during the query we might be missing some data and its safe to
            // send this as mongos can resend at this point
            throw SendStaleConfigException(pq.ns(), "version changed during initial query",
                                           shardingVersionAtStart,
                                           shardingState.getVersion(pq.ns()));
        }

        const logger::LogComponent queryLogComponent = logger::LogComponent::kQuery;
        const logger::LogSeverity logLevelOne = logger::LogSeverity::Debug(1);

        // Set debug information for consumption by the profiler.
        if (dbProfilingLevel > 0 ||
            curop.elapsedMillis() > serverGlobalParams.slowMS ||
            logger::globalLogDomain()->shouldLog(queryLogComponent, logLevelOne)) {
            PlanSummaryStats newStats;
            Explain::getSummaryStats(exec.get(), &newStats);

            curop.debug().ntoskip = pq.getSkip();
            curop.debug().nreturned = numResults;
            curop.debug().scanAndOrder = newStats.hasSortStage;
            curop.debug().nscanned = newStats.totalKeysExamined;
            curop.debug().nscannedObjects = newStats.totalDocsExamined;
            curop.debug().idhack = newStats.isIdhack;

            // Get BSON stats.
            scoped_ptr<PlanStageStats> execStats(exec->getStats());
            BSONObjBuilder statsBob;
            Explain::statsToBSON(*execStats, &statsBob);
            curop.debug().execStats.set(statsBob.obj());

            // Replace exec stats with plan summary if stats cannot fit into CachedBSONObj.
            if (curop.debug().execStats.tooBig() && !curop.debug().planSummary.empty()) {
                BSONObjBuilder bob;
                bob.append("summary", curop.debug().planSummary.toString());
                curop.debug().execStats.set(bob.done());
            }
        }

        long long ccId = 0;
        if (saveClientCursor) {
            // We won't use the executor until it's getMore'd.
            exec->saveState();

            // Allocate a new ClientCursor.  We don't have to worry about leaking it as it's
            // inserted into a global map by its ctor.
            ClientCursor* cc = new ClientCursor(collection, exec.get(),
                                                cq->getParsed().getOptions().toInt(),
                                                cq->getParsed().getFilter());
            ccId = cc->cursorid();

            if (fromDBDirectClient) {
                cc->setUnownedRecoveryUnit(txn->recoveryUnit());
            }
            else {
                // We stash away the RecoveryUnit in the ClientCursor.  It's used for subsequent
                // getMore requests.  The calling OpCtx gets a fresh RecoveryUnit.
                cc->setOwnedRecoveryUnit(txn->releaseRecoveryUnit());
                StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
                txn->setRecoveryUnit(storageEngine->newRecoveryUnit(txn));
            }

            QLOG() << "caching executor with cursorid " << ccId
                   << " after returning " << numResults << " results" << endl;

            // ClientCursor takes ownership of executor.  Release to make sure it's not deleted.
            exec.release();

            // TODO document
            if (pq.getOptions().oplogReplay && !slaveReadTill.isNull()) {
                cc->slaveReadTill(slaveReadTill);
            }

            // TODO document
            if (pq.getOptions().exhaust) {
                curop.debug().exhaust = true;
            }

            // Set attributes for getMore.
            cc->setCollMetadata(collMetadata);
            cc->setPos(numResults);

            // If the query had a time limit, remaining time is "rolled over" to the cursor (for
            // use by future getmore ops).
            cc->setLeftoverMaxTimeMicros(curop.getRemainingMaxTimeMicros());
        }
        else {
            QLOG() << "Not caching executor but returning " << numResults << " results.\n";
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
        return curop.debug().exhaust ? pq.ns() : "";
    }

}  // namespace mongo
