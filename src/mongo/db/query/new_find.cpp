/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/query/new_find.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/oplogstart.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/query/find_constants.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/single_solution_runner.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/db/repl/repl_reads_ok.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/stale_exception.h"
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

    bool enoughForExplain(const mongo::LiteParsedQuery& pq, long long n) {
        if (pq.wantMore() || 0 == pq.getNumToReturn()) { return false; }
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

    /**
     * Also called by db/ops/query.cpp.  This is the new getMore entry point.
     *
     * pass - when QueryOption_AwaitData is in use, the caller will make repeated calls 
     *        when this method returns an empty result, incrementing pass on each call.  
     *        Thus, pass == 0 indicates this is the first "attempt" before any 'awaiting'.
     */
    QueryResult* newGetMore(OperationContext* txn,
                            const char* ns,
                            int ntoreturn,
                            long long cursorid,
                            CurOp& curop,
                            int pass,
                            bool& exhaust,
                            bool* isCursorAuthorized) {
        exhaust = false;

        // This is a read lock.
        scoped_ptr<Client::ReadContext> ctx(new Client::ReadContext(ns));
        Collection* collection = ctx->ctx().db()->getCollection(ns);
        uassert( 17356, "collection dropped between getMore calls", collection );

        QLOG() << "Running getMore, cursorid: " << cursorid << endl;

        // This checks to make sure the operation is allowed on a replicated node.  Since we are not
        // passing in a query object (necessary to check SlaveOK query option), the only state where
        // reads are allowed is PRIMARY (or master in master/slave).  This function uasserts if
        // reads are not okay.
        repl::replVerifyReadsOk(ns, NULL);

        // A pin performs a CC lookup and if there is a CC, increments the CC's pin value so it
        // doesn't time out.  Also informs ClientCursor that there is somebody actively holding the
        // CC, so don't delete it.
        ClientCursorPin ccPin(collection, cursorid);
        ClientCursor* cc = ccPin.c();

        // These are set in the QueryResult msg we return.
        int resultFlags = ResultFlag_AwaitCapable;

        int numResults = 0;
        int startingResult = 0;

        const int InitialBufSize = 512 + sizeof(QueryResult) + MaxBytesToReturnToClientAtOnce;
        BufBuilder bb(InitialBufSize);
        bb.skip(sizeof(QueryResult));

        if (NULL == cc) {
            cursorid = 0;
            resultFlags = ResultFlag_CursorNotFound;
        }
        else {
            // Quote: check for spoofing of the ns such that it does not match the one originally
            // there for the cursor
            uassert(17011, "auth error", str::equals(ns, cc->ns().c_str()));
            *isCursorAuthorized = true;

            // Reset timeout timer on the cursor since the cursor is still in use.
            cc->setIdleTime(0);

            // TODO: fail point?

            // If the operation that spawned this cursor had a time limit set, apply leftover
            // time to this getmore.
            curop.setMaxTimeMicros(cc->getLeftoverMaxTimeMicros());
            txn->checkForInterrupt(); // May trigger maxTimeAlwaysTimeOut fail point.

            if (0 == pass) { 
                cc->updateSlaveLocation(curop); 
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
            Runner* runner = cc->getRunner();
            const int queryOptions = cc->queryOptions();

            // Get results out of the runner.
            runner->restoreState(txn);

            BSONObj obj;
            Runner::RunnerState state;
            while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&obj, NULL))) {
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

            if (Runner::RUNNER_EOF == state && 0 == numResults
                && (queryOptions & QueryOption_CursorTailable)
                && (queryOptions & QueryOption_AwaitData) && (pass < 1000)) {
                // If the cursor is tailable we don't kill it if it's eof.  We let it try to get
                // data some # of times first.
                return 0;
            }

            bool saveClientCursor = false;

            if (Runner::RUNNER_DEAD == state || Runner::RUNNER_ERROR == state) {
                // Propagate this error to caller.
                if (Runner::RUNNER_ERROR == state) {
                    // Stats are helpful when errors occur.
                    TypeExplain* bareExplain;
                    Status res = runner->getInfo(&bareExplain, NULL);
                    if (res.isOK()) {
                        boost::scoped_ptr<TypeExplain> errorExplain(bareExplain);
                        error() << "Runner error, stats:\n"
                                << errorExplain->stats.jsonString(Strict, true);
                    }

                    uasserted(17406, "getMore runner error: " +
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
            else if (Runner::RUNNER_EOF == state) {
                // EOF is also end of the line unless it's tailable.
                saveClientCursor = queryOptions & QueryOption_CursorTailable;
            }
            else {
                verify(Runner::RUNNER_ADVANCED == state);
                saveClientCursor = true;
            }

            if (!saveClientCursor) {
                ccPin.deleteUnderlying();
                // cc is now invalid, as is the runner
                cursorid = 0;
                cc = NULL;
                QLOG() << "getMore NOT saving client cursor, ended with state "
                       << Runner::statestr(state)
                       << endl;
            }
            else {
                // Continue caching the ClientCursor.
                cc->incPos(numResults);
                runner->saveState();
                QLOG() << "getMore saving client cursor ended with state "
                       << Runner::statestr(state)
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

        QueryResult* qr = reinterpret_cast<QueryResult*>(bb.buf());
        qr->len = bb.len();
        qr->setOperation(opReply);
        qr->_resultFlags() = resultFlags;
        qr->cursorId = cursorid;
        qr->startingFrom = startingResult;
        qr->nReturned = numResults;
        bb.decouple();
        QLOG() << "getMore returned " << numResults << " results\n";
        return qr;
    }

    Status getOplogStartHack(Collection* collection, CanonicalQuery* cq, Runner** runnerOut) {
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
        OplogStart* stage = new OplogStart(collection, tsExpr, oplogws);

        // Takes ownership of ws and stage.
        auto_ptr<InternalRunner> runner(new InternalRunner(collection, stage, oplogws));

        // The stage returns a DiskLoc of where to start.
        DiskLoc startLoc;
        Runner::RunnerState state = runner->getNext(NULL, &startLoc);

        // This is normal.  The start of the oplog is the beginning of the collection.
        if (Runner::RUNNER_EOF == state) { return getRunner(collection, cq, runnerOut); }

        // This is not normal.  An error was encountered.
        if (Runner::RUNNER_ADVANCED != state) {
            return Status(ErrorCodes::InternalError,
                          "quick oplog start location had error...?");
        }

        // cout << "diskloc is " << startLoc.toString() << endl;

        // Build our collection scan...
        CollectionScanParams params;
        params.collection = collection;
        params.start = startLoc;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = cq->getParsed().hasOption(QueryOption_CursorTailable);

        WorkingSet* ws = new WorkingSet();
        CollectionScan* cs = new CollectionScan(params, ws, cq->root());
        // Takes ownership of cq, cs, ws.
        *runnerOut = new SingleSolutionRunner(collection, cq, NULL, cs, ws);
        return Status::OK();
    }

    std::string newRunQuery(OperationContext* txn,
                            Message& m,
                            QueryMessage& q,
                            CurOp& curop,
                            Message &result) {
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
            bb.skip(sizeof(QueryResult));

            BSONObjBuilder cmdResBuf;
            if (!runCommands(txn, ns, q.query, curop, bb, cmdResBuf, false, q.queryOptions)) {
                uasserted(13530, "bad or malformed command request?");
            }

            curop.debug().iscommand = true;
            // TODO: Does this get overwritten/do we really need to set this twice?
            curop.debug().query = q.query;

            QueryResult* qr = reinterpret_cast<QueryResult*>(bb.buf());
            bb.decouple();
            qr->setResultFlagsToOk();
            qr->len = bb.len();
            curop.debug().responseLength = bb.len();
            qr->setOperation(opReply);
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            result.setData(qr, true);
            return "";
        }

        // This is a read lock.  We require this because if we're parsing a $where, the
        // where-specific parsing code assumes we have a lock and creates execution machinery that
        // requires it.
        Client::ReadContext ctx(q.ns);
        Collection* collection = ctx.ctx().db()->getCollection( ns );

        // Parse the qm into a CanonicalQuery.
        CanonicalQuery* cq;
        Status canonStatus = CanonicalQuery::canonicalize(
                                q, &cq, WhereCallbackReal(StringData(ctx.ctx().db()->name())));
        if (!canonStatus.isOK()) {
            uasserted(17287, str::stream() << "Can't canonicalize query: " << canonStatus.toString());
        }
        verify(cq);

        QLOG() << "Running query:\n" << cq->toString();
        LOG(2) << "Running query: " << cq->toStringShort();

        // Parse, canonicalize, plan, transcribe, and get a runner.
        Runner* rawRunner = NULL;

        // We use this a lot below.
        const LiteParsedQuery& pq = cq->getParsed();

        // We'll now try to get the query runner that will execute this query for us. There
        // are a few cases in which we know upfront which runner we should get and, therefore,
        // we shortcut the selection process here.
        //
        // (a) If the query is over a collection that doesn't exist, we get a special runner
        // that's is so (a runner) which doesn't return results, the EOFRunner.
        //
        // (b) if the query is a replication's initial sync one, we get a SingleSolutinRunner
        // that uses a specifically designed stage that skips extents faster (see details in
        // exec/oplogstart.h)
        //
        // Otherwise we go through the selection of which runner is most suited to the
        // query + run-time context at hand.
        Status status = Status::OK();
        if (collection == NULL) {
            rawRunner = new EOFRunner(cq, cq->ns());
        }
        else if (pq.hasOption(QueryOption_OplogReplay)) {
            status = getOplogStartHack(collection, cq, &rawRunner);
        }
        else {
            // Takes ownership of cq.
            size_t options = QueryPlannerParams::DEFAULT;
            if (shardingState.needCollectionMetadata(pq.ns())) {
                options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
            }
            status = getRunner(collection, cq, &rawRunner, options);
        }

        if (!status.isOK()) {
            // NOTE: Do not access cq as getRunner has deleted it.
            uasserted(17007, "Unable to execute query: " + status.reason());
        }

        verify(NULL != rawRunner);
        auto_ptr<Runner> runner(rawRunner);

        // We freak out later if this changes before we're done with the query.
        const ChunkVersion shardingVersionAtStart = shardingState.getVersion(cq->ns());

        // Handle query option $maxTimeMS (not used with commands).
        curop.setMaxTimeMicros(static_cast<unsigned long long>(pq.getMaxTimeMS()) * 1000);
        txn->checkForInterrupt(); // May trigger maxTimeAlwaysTimeOut fail point.

        // uassert if we are not on a primary, and not a secondary with SlaveOk query parameter set.
        repl::replVerifyReadsOk(cq->ns(), &pq);

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
        bb.skip(sizeof(QueryResult));

        // How many results have we obtained from the runner?
        int numResults = 0;

        // If we're replaying the oplog, we save the last time that we read.
        OpTime slaveReadTill;

        // Do we save the Runner in a ClientCursor for getMore calls later?
        bool saveClientCursor = false;

        // We turn on auto-yielding for the runner here.  The runner registers itself with the
        // active runners list in ClientCursor.
        auto_ptr<ScopedRunnerRegistration> safety(new ScopedRunnerRegistration(runner.get()));

        BSONObj obj;
        Runner::RunnerState state;
        // uint64_t numMisplacedDocs = 0;

        // set this outside loop. we will need to use this both within loop and when deciding
        // to fill in explain information
        const bool isExplain = pq.isExplain();

        // Have we retrieved info about which plan the runner will
        // use to execute the query yet?
        bool gotPlanInfo = false;
        PlanInfo* rawInfo;
        boost::scoped_ptr<PlanInfo> planInfo;

        while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&obj, NULL))) {
            // Add result to output buffer. This is unnecessary if explain info is requested
            if (!isExplain) {
                bb.appendBuf((void*)obj.objdata(), obj.objsize());
            }

            // Count the result.
            ++numResults;

            // In the case of the multi plan runner, we may not be able to
            // successfully retrieve plan info until after the query starts
            // to run. This is because the multi plan runner doesn't know what
            // plan it will end up using until it runs candidates and selects
            // the best.
            //
            // TODO: Do we ever want to output what the MPR is comparing?
            if (!gotPlanInfo) {
                Status infoStatus = runner->getInfo(NULL, &rawInfo);
                if (infoStatus.isOK()) {
                    gotPlanInfo = true;
                    planInfo.reset(rawInfo);
                    // planSummary is really a ThreadSafeString which copies the data from
                    // the provided pointer.
                    curop.debug().planSummary = planInfo->planSummary.c_str();
                }
            }

            // Possibly note slave's position in the oplog.
            if (pq.hasOption(QueryOption_OplogReplay)) {
                BSONElement e = obj["ts"];
                if (Date == e.type() || Timestamp == e.type()) {
                    slaveReadTill = e._opTime();
                }
            }

            // TODO: only one type of 2d search doesn't support this.  We need a way to pull it out
            // of CanonicalQuery. :(
            const bool supportsGetMore = true;
            if (isExplain) {
                if (enoughForExplain(pq, numResults)) {
                    break;
                }
            }
            else if (!supportsGetMore && (enough(pq, numResults)
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
                    QLOG() << " runner EOF=" << runner->isEOF() << endl;
                    saveClientCursor = !runner->isEOF();
                }
                break;
            }
        }

        // Try to get information about the plan which the runner
        // will use to execute the query, it we don't have it already.
        if (!gotPlanInfo) {
            Status infoStatus = runner->getInfo(NULL, &rawInfo);
            if (infoStatus.isOK()) {
                gotPlanInfo = true;
                planInfo.reset(rawInfo);
                // planSummary is really a ThreadSafeString which copies the data from
                // the provided pointer.
                curop.debug().planSummary = planInfo->planSummary.c_str();
            }
        }

        // If we cache the runner later, we want to deregister it as it receives notifications
        // anyway by virtue of being cached.
        //
        // If we don't cache the runner later, we are deleting it, so it must be deregistered.
        //
        // So, no matter what, deregister the runner.
        safety.reset();

        // Caller expects exceptions thrown in certain cases.
        if (Runner::RUNNER_ERROR == state) {
            TypeExplain* bareExplain;
            Status res = runner->getInfo(&bareExplain, NULL);
            if (res.isOK()) {
                boost::scoped_ptr<TypeExplain> errorExplain(bareExplain);
                error() << "Runner error, stats:\n"
                        << errorExplain->stats.jsonString(Strict, true);
            }
            uasserted(17144, "Runner error: " + WorkingSetCommon::toStatusString(obj));
        }

        // Why save a dead runner?
        if (Runner::RUNNER_DEAD == state) {
            saveClientCursor = false;
        }
        else if (pq.hasOption(QueryOption_CursorTailable)) {
            // If we're tailing a capped collection, we don't bother saving the cursor if the
            // collection is empty. Otherwise, the semantics of the tailable cursor is that the
            // client will keep trying to read from it. So we'll keep it around.
            Collection* collection = ctx.ctx().db()->getCollection(cq->ns());
            if (collection && collection->numRecords() != 0 && pq.getNumToReturn() != 1) {
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

        // Used to fill in explain and to determine if the query is slow enough to be logged.
        int elapsedMillis = curop.elapsedMillis();

        // Get explain information if:
        // 1) it is needed by an explain query;
        // 2) profiling is enabled; or
        // 3) profiling is disabled but we still need explain details to log a "slow" query.
        // Producing explain information is expensive and should be done only if we are certain
        // the information will be used.
        boost::scoped_ptr<TypeExplain> explain(NULL);
        if (isExplain ||
            ctx.ctx().db()->getProfilingLevel() > 0 ||
            elapsedMillis > serverGlobalParams.slowMS) {
            // Ask the runner to produce explain information.
            TypeExplain* bareExplain;
            Status res = runner->getInfo(&bareExplain, NULL);
            if (res.isOK()) {
                explain.reset(bareExplain);
            }
            else if (isExplain) {
                error() << "could not produce explain of query '" << pq.getFilter()
                        << "', error: " << res.reason();
                // If numResults and the data in bb don't correspond, we'll crash later when rooting
                // through the reply msg.
                BSONObj emptyObj;
                bb.appendBuf((void*)emptyObj.objdata(), emptyObj.objsize());
                // The explain output is actually a result.
                numResults = 1;
                // TODO: we can fill out millis etc. here just fine even if the plan screwed up.
            }
        }

        // Fill in the missing run-time fields in explain, starting with propeties of
        // the process running the query.
        if (isExplain && NULL != explain.get()) {
            std::string server = mongoutils::str::stream()
                << getHostNameCached() << ":" << serverGlobalParams.port;
            explain->setServer(server);

            // We might have skipped some results due to chunk migration etc. so our count is
            // correct.
            explain->setN(numResults);

            // Clock the whole operation.
            explain->setMillis(elapsedMillis);

            BSONObj explainObj = explain->toBSON();
            bb.appendBuf((void*)explainObj.objdata(), explainObj.objsize());

            // The explain output is actually a result.
            numResults = 1;
        }

        long long ccId = 0;
        if (saveClientCursor) {
            // We won't use the runner until it's getMore'd.
            runner->saveState();

            // Allocate a new ClientCursor.  We don't have to worry about leaking it as it's
            // inserted into a global map by its ctor.
            ClientCursor* cc = new ClientCursor(collection, runner.get(),
                                                cq->getParsed().getOptions(),
                                                cq->getParsed().getFilter());
            ccId = cc->cursorid();

            QLOG() << "caching runner with cursorid " << ccId
                   << " after returning " << numResults << " results" << endl;

            // ClientCursor takes ownership of runner.  Release to make sure it's not deleted.
            runner.release();

            // TODO document
            if (pq.hasOption(QueryOption_OplogReplay) && !slaveReadTill.isNull()) {
                cc->slaveReadTill(slaveReadTill);
            }

            // TODO document
            if (pq.hasOption(QueryOption_Exhaust)) {
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
            QLOG() << "Not caching runner but returning " << numResults << " results.\n";
        }

        // Add the results from the query into the output buffer.
        result.appendData(bb.buf(), bb.len());
        bb.decouple();

        // Fill out the output buffer's header.
        QueryResult* qr = static_cast<QueryResult*>(result.header());
        qr->cursorId = ccId;
        curop.debug().cursorid = (0 == ccId ? -1 : ccId);
        qr->setResultFlagsToOk();
        qr->setOperation(opReply);
        qr->startingFrom = 0;
        qr->nReturned = numResults;

        // Set debug information for consumption by the profiler.
        curop.debug().ntoskip = pq.getSkip();
        curop.debug().nreturned = numResults;
        if (NULL != explain.get()) {
            if (explain->isScanAndOrderSet()) {
                curop.debug().scanAndOrder = explain->getScanAndOrder();
            }
            else {
                curop.debug().scanAndOrder = false;
            }

            if (explain->isNScannedSet()) {
                curop.debug().nscanned = explain->getNScanned();
            }

            if (explain->isNScannedObjectsSet()) {
                curop.debug().nscannedObjects = explain->getNScannedObjects();
            }

            if (explain->isIDHackSet()) {
                curop.debug().idhack = explain->getIDHack();
            }

            if (!explain->stats.isEmpty()) {
                // execStats is a CachedBSONObj because it lives in the race-prone
                // curop.
                curop.debug().execStats.set(explain->stats);

                // Replace exec stats with plan summary if stats cannot fit into CachedBSONObj.
                if (curop.debug().execStats.tooBig() && !curop.debug().planSummary.empty()) {
                    BSONObjBuilder bob;
                    bob.append("summary", curop.debug().planSummary.toString());
                    curop.debug().execStats.set(bob.done());
                }

            }
        }

        // curop.debug().exhaust is set above.
        return curop.debug().exhaust ? pq.ns() : "";
    }

}  // namespace mongo
