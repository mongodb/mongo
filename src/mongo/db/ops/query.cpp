// query.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/ops/query.h"

#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/parsed_query.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query_plan_summary.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/query_optimizer_internal.h"
#include "mongo/db/queryoptimizercursor.h"
#include "mongo/db/query/new_find.h"
#include "mongo/db/repl/finding_start_cursor.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_reads_ok.h"
#include "mongo/db/scanandorder.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/stale_exception.h"  // for SendStaleConfigException
#include "mongo/server.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    const int32_t MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

    extern FailPoint maxTimeAlwaysTimeOut;

    MONGO_FP_DECLARE(getMoreError);

    bool runCommands(const char *ns, BSONObj& jsobj, CurOp& curop, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        try {
            return _runCommands(ns, jsobj, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch( SendStaleConfigException& ){
            throw;
        }
        catch ( AssertionException& e ) {
            verify( e.getCode() != SendStaleConfigCode && e.getCode() != RecvStaleConfigCode );

            e.getInfo().append( anObjBuilder , "assertion" , "assertionCode" );
            curop.debug().exceptionInfo = e.getInfo();
        }
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.appendBuf((void*) x.objdata(), x.objsize());
        return true;
    }


    BSONObj id_obj = fromjson("{\"_id\":1}");
    BSONObj empty_obj = fromjson("{}");


    //int dump = 0;

    /* empty result for error conditions */
    QueryResult* emptyMoreResult(long long cursorid) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        QueryResult *qr = (QueryResult *) b.buf();
        qr->cursorId = 0; // 0 indicates no more data to retrieve.
        qr->startingFrom = 0;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->initializeResultFlags();
        qr->nReturned = 0;
        b.decouple();
        return qr;
    }

    static BSONObj extractKey(Cursor* c, const KeyPattern& usingKeyPattern ) {
        KeyPattern currentIndex( c->indexKeyPattern() );
        if ( usingKeyPattern.isCoveredBy( currentIndex ) && ! currentIndex.isSpecial() ){
            BSONObj currKey = c->currKey();
            BSONObj prettyKey = currKey.replaceFieldNames( currentIndex.toBSON() );
            return usingKeyPattern.extractSingleKey( prettyKey );
        }

        return usingKeyPattern.extractSingleKey( c->current() );
    }

    QueryResult* processGetMore(const char* ns,
                                int ntoreturn,
                                long long cursorid,
                                CurOp& curop,
                                int pass,
                                bool& exhaust,
                                bool* isCursorAuthorized ) {
        if (isNewQueryFrameworkEnabled()) {
            return newGetMore(ns, ntoreturn, cursorid, curop, pass, exhaust, isCursorAuthorized);
        }

        exhaust = false;

        int bufSize = 512 + sizeof( QueryResult ) + MaxBytesToReturnToClientAtOnce;

        BufBuilder b( bufSize );
        b.skip(sizeof(QueryResult));
        int resultFlags = ResultFlag_AwaitCapable;
        int start = 0;
        int n = 0;

        scoped_ptr<Client::ReadContext> ctx(new Client::ReadContext(ns));
        // call this readlocked so state can't change
        replVerifyReadsOk();

        ClientCursorPin p(cursorid);
        ClientCursor *cc = p.c();

        if ( unlikely(!cc) ) {
            LOGSOME << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = ResultFlag_CursorNotFound;
        }
        else {
            // Some internal users create a ClientCursor with a Runner.  Don't crash if this
            // happens.  Instead, hand them off to the new framework.
            if (NULL != cc->getRunner()) {
                p.release();
                return newGetMore(ns, ntoreturn, cursorid, curop, pass, exhaust, isCursorAuthorized);
            }

            // check for spoofing of the ns such that it does not match the one originally there for the cursor
            uassert(14833, "auth error", str::equals(ns, cc->ns().c_str()));

            *isCursorAuthorized = true;

            // This must be done after auth check to ensure proper cleanup.
            uassert(16951, "failing getmore due to set failpoint",
                    !MONGO_FAIL_POINT(getMoreError));

            // If the operation that spawned this cursor had a time limit set, apply leftover
            // time to this getmore.
            curop.setMaxTimeMicros( cc->getLeftoverMaxTimeMicros() );
            if (MONGO_FAIL_POINT(maxTimeAlwaysTimeOut) && cc->getLeftoverMaxTimeMicros() != 0) {
                uasserted(ErrorCodes::ExceededTimeLimit,
                          "operation exceeded time limit [maxTimeAlwaysTimeOut]");
            }

            if ( pass == 0 )
                cc->updateSlaveLocation( curop );

            int queryOptions = cc->queryOptions();
            
            curop.debug().query = cc->query();
            curop.setQuery( cc->query() );

            start = cc->pos();
            Cursor *c = cc->c();

            if (!c->requiresLock()) {
                // make sure it won't be destroyed under us
                fassert(16952, !c->shouldDestroyOnNSDeletion());
                fassert(16953, !c->supportYields());
                ctx.reset(); // unlocks
            }

            c->recoverFromYield();
            DiskLoc last;

            // This metadata may be stale, but it's the state of chunking when the cursor was
            // created.
            CollectionMetadataPtr metadata = cc->getCollMetadata();
            KeyPattern keyPattern( metadata ? metadata->getKeyPattern() : BSONObj() );

            while ( 1 ) {
                if ( !c->ok() ) {
                    if ( c->tailable() ) {
                        // when a tailable cursor hits "EOF", ok() goes false, and current() is
                        // null.  however advance() can still be retries as a reactivation attempt.
                        // when there is new data, it will return true.  that's what we are doing
                        // here.
                        if ( c->advance() )
                            continue;

                        if( n == 0 && (queryOptions & QueryOption_AwaitData) && pass < 1000 ) {
                            return 0;
                        }

                        break;
                    }
                    p.release();
                    bool ok = ClientCursor::erase(cursorid);
                    verify(ok);
                    cursorid = 0;
                    cc = 0;
                    break;
                }

                MatchDetails details;
                if ( cc->fields && cc->fields->getArrayOpType() == Projection::ARRAY_OP_POSITIONAL ) {
                    // field projection specified, and contains an array operator
                    details.requestElemMatchKey();
                }

                // in some cases (clone collection) there won't be a matcher
                if ( !c->currentMatches( &details ) ) {
                }
                else if ( metadata && !metadata->keyBelongsToMe( extractKey(c, keyPattern ) ) ) {
                    LOG(2) << "cursor skipping document in un-owned chunk: " << c->current()
                               << endl;
                }
                else {
                    if( c->getsetdup(c->currLoc()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        last = c->currLoc();
                        n++;

                        // Fill out the fields requested by the query.
                        const Projection::KeyOnly *keyFieldsOnly = c->keyFieldsOnly();
                        if ( keyFieldsOnly ) {
                            fillQueryResultFromObj( b, 0, keyFieldsOnly->hydrate(
                            c->currKey() ), &details );
                        }
                        else {
                            DiskLoc loc = c->currLoc();
                            fillQueryResultFromObj( b, cc->fields.get(), c->current(), &details,
                                    ( ( cc->pq.get() && cc->pq->showDiskLoc() ) ? &loc : 0 ) );
                        }

                        if ( ( ntoreturn && n >= ntoreturn ) || b.len() > MaxBytesToReturnToClientAtOnce ) {
                            c->advance();
                            cc->incPos( n );
                            break;
                        }
                    }
                }
                c->advance();

                if ( ! cc->yieldSometimes( ( c->ok() && c->keyFieldsOnly() ) ?
                                          ClientCursor::DontNeed : ClientCursor::WillNeed ) ) {
                    ClientCursor::erase(cursorid);
                    cursorid = 0;
                    cc = 0;
                    break;
                }
            }
            
            if ( cc ) {
                if ( c->supportYields() ) {
                    ClientCursor::YieldData data;
                    verify( cc->prepareToYield( data ) );
                }
                else {
                    cc->c()->noteLocation();
                }
                cc->storeOpForSlave( last );
                exhaust = cc->queryOptions() & QueryOption_Exhaust;

                // If the getmore had a time limit, remaining time is "rolled over" back to the
                // cursor (for use by future getmore ops).
                cc->setLeftoverMaxTimeMicros( curop.getRemainingMaxTimeMicros() );
            }
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->_resultFlags() = resultFlags;
        qr->cursorId = cursorid;
        qr->startingFrom = start;
        qr->nReturned = n;
        b.decouple();

        return qr;
    }

    ResultDetails::ResultDetails() :
        match(),
        orderedMatch(),
        loadedRecord(),
        chunkSkip() {
    }

    ExplainRecordingStrategy::ExplainRecordingStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo ) :
    _ancillaryInfo( ancillaryInfo ) {
    }

    shared_ptr<ExplainQueryInfo> ExplainRecordingStrategy::doneQueryInfo() {
        shared_ptr<ExplainQueryInfo> ret = _doneQueryInfo();
        ret->setAncillaryInfo( _ancillaryInfo );
        return ret;
    }
    
    NoExplainStrategy::NoExplainStrategy() :
    ExplainRecordingStrategy( ExplainQueryInfo::AncillaryInfo() ) {
    }

    shared_ptr<ExplainQueryInfo> NoExplainStrategy::_doneQueryInfo() {
        verify( false );
        return shared_ptr<ExplainQueryInfo>();
    }
    
    MatchCountingExplainStrategy::MatchCountingExplainStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo ) :
    ExplainRecordingStrategy( ancillaryInfo ),
    _orderedMatches() {
    }
    
    void MatchCountingExplainStrategy::noteIterate( const ResultDetails& resultDetails ) {
        _noteIterate( resultDetails );
        if ( resultDetails.orderedMatch ) {
            ++_orderedMatches;
        }
    }
    
    SimpleCursorExplainStrategy::SimpleCursorExplainStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
     const shared_ptr<Cursor> &cursor ) :
    MatchCountingExplainStrategy( ancillaryInfo ),
    _cursor( cursor ),
    _explainInfo( new ExplainSinglePlanQueryInfo() ) {
    }
 
    void SimpleCursorExplainStrategy::notePlan( bool scanAndOrder, bool indexOnly ) {
        _explainInfo->notePlan( *_cursor, scanAndOrder, indexOnly );
    }

    void SimpleCursorExplainStrategy::_noteIterate( const ResultDetails& resultDetails ) {
        _explainInfo->noteIterate( resultDetails.match,
                                   resultDetails.loadedRecord,
                                   resultDetails.chunkSkip,
                                   *_cursor );
    }

    void SimpleCursorExplainStrategy::noteYield() {
        _explainInfo->noteYield();
    }

    shared_ptr<ExplainQueryInfo> SimpleCursorExplainStrategy::_doneQueryInfo() {
        _explainInfo->noteDone( *_cursor );
        return _explainInfo->queryInfo();
    }

    QueryOptimizerCursorExplainStrategy::QueryOptimizerCursorExplainStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
     const shared_ptr<QueryOptimizerCursor> &cursor ) :
    MatchCountingExplainStrategy( ancillaryInfo ),
    _cursor( cursor ) {
    }
    
    void QueryOptimizerCursorExplainStrategy::_noteIterate( const ResultDetails& resultDetails ) {
        // Note ordered matches only; if an unordered plan is selected, the explain result will
        // be updated with reviseN().
        _cursor->noteIterate( resultDetails.orderedMatch,
                              resultDetails.loadedRecord,
                              resultDetails.chunkSkip );
    }

    void QueryOptimizerCursorExplainStrategy::noteYield() {
        _cursor->noteYield();
    }

    shared_ptr<ExplainQueryInfo> QueryOptimizerCursorExplainStrategy::_doneQueryInfo() {
        return _cursor->explainQueryInfo();
    }

    ResponseBuildStrategy::ResponseBuildStrategy( const ParsedQuery &parsedQuery,
                                                  const shared_ptr<Cursor> &cursor,
                                                  BufBuilder &buf ) :
    _parsedQuery( parsedQuery ),
    _cursor( cursor ),
    _queryOptimizerCursor( dynamic_pointer_cast<QueryOptimizerCursor>( _cursor ) ),
    _buf( buf ) {
    }

    void ResponseBuildStrategy::resetBuf() {
        _buf.reset();
        _buf.skip( sizeof( QueryResult ) );
    }

    BSONObj ResponseBuildStrategy::current( bool allowCovered,
                                            ResultDetails* resultDetails ) const {
        if ( _parsedQuery.returnKey() ) {
            BSONObjBuilder bob;
            bob.appendKeys( _cursor->indexKeyPattern(), _cursor->currKey() );
            return bob.obj();
        }
        if ( allowCovered ) {
            const Projection::KeyOnly *keyFieldsOnly = _cursor->keyFieldsOnly();
            if ( keyFieldsOnly ) {
                return keyFieldsOnly->hydrate( _cursor->currKey() );
            }
        }
        resultDetails->loadedRecord = true;
        BSONObj ret = _cursor->current();
        verify( ret.isValid() );
        return ret;
    }

    OrderedBuildStrategy::OrderedBuildStrategy( const ParsedQuery &parsedQuery,
                                               const shared_ptr<Cursor> &cursor,
                                               BufBuilder &buf ) :
    ResponseBuildStrategy( parsedQuery, cursor, buf ),
    _skip( _parsedQuery.getSkip() ),
    _bufferedMatches() {
    }
    
    bool OrderedBuildStrategy::handleMatch( ResultDetails* resultDetails ) {
        DiskLoc loc = _cursor->currLoc();
        if ( _cursor->getsetdup( loc ) ) {
            return false;
        }
        if ( _skip > 0 ) {
            --_skip;
            return false;
        }
        BSONObj currentDocument = current( true, resultDetails );
        // Explain does not obey soft limits, so matches should not be buffered.
        if ( !_parsedQuery.isExplain() ) {
            fillQueryResultFromObj( _buf, _parsedQuery.getFields(),
                                    currentDocument, &resultDetails->matchDetails,
                                   ( _parsedQuery.showDiskLoc() ? &loc : 0 ) );
            ++_bufferedMatches;
        }
        resultDetails->match = true;
        resultDetails->orderedMatch = true;
        return true;
    }

    ReorderBuildStrategy* ReorderBuildStrategy::make( const ParsedQuery& parsedQuery,
                                                      const shared_ptr<Cursor>& cursor,
                                                      BufBuilder& buf,
                                                      const QueryPlanSummary& queryPlan ) {
        auto_ptr<ReorderBuildStrategy> ret( new ReorderBuildStrategy( parsedQuery, cursor, buf ) );
        ret->init( queryPlan );
        return ret.release();
    }

    ReorderBuildStrategy::ReorderBuildStrategy( const ParsedQuery &parsedQuery,
                                               const shared_ptr<Cursor> &cursor,
                                               BufBuilder &buf ) :
    ResponseBuildStrategy( parsedQuery, cursor, buf ),
    _bufferedMatches() {
    }
    
    void ReorderBuildStrategy::init( const QueryPlanSummary &queryPlan ) {
        _scanAndOrder.reset( newScanAndOrder( queryPlan ) );
    }

    bool ReorderBuildStrategy::handleMatch( ResultDetails* resultDetails ) {
        if ( _cursor->getsetdup( _cursor->currLoc() ) ) {
            return false;
        }
        _handleMatchNoDedup( resultDetails );
        resultDetails->match = true;
        return true;
    }
    
    void ReorderBuildStrategy::_handleMatchNoDedup( ResultDetails* resultDetails ) {
        DiskLoc loc = _cursor->currLoc();
        _scanAndOrder->add( current( false, resultDetails ),
                            _parsedQuery.showDiskLoc() ? &loc : 0 );
    }

    int ReorderBuildStrategy::rewriteMatches() {
        cc().curop()->debug().scanAndOrder = true;
        int ret = 0;
        _scanAndOrder->fill( _buf, &_parsedQuery, ret );
        _bufferedMatches = ret;
        return ret;
    }
    
    ScanAndOrder *
    ReorderBuildStrategy::newScanAndOrder( const QueryPlanSummary &queryPlan ) const {
        verify( !_parsedQuery.getOrder().isEmpty() );
        verify( _cursor->ok() );
        const FieldRangeSet *fieldRangeSet = 0;
        if ( queryPlan.valid() ) {
            fieldRangeSet = queryPlan.fieldRangeSetMulti.get();
        }
        else {
            verify( _queryOptimizerCursor );
            fieldRangeSet = _queryOptimizerCursor->initialFieldRangeSet();
        }
        verify( fieldRangeSet );
        return new ScanAndOrder( _parsedQuery.getSkip(),
                                _parsedQuery.getNumToReturn(),
                                _parsedQuery.getOrder(),
                                *fieldRangeSet );
    }

    HybridBuildStrategy* HybridBuildStrategy::make( const ParsedQuery& parsedQuery,
                                                    const shared_ptr<QueryOptimizerCursor>& cursor,
                                                    BufBuilder& buf ) {
        auto_ptr<HybridBuildStrategy> ret( new HybridBuildStrategy( parsedQuery, cursor, buf ) );
        ret->init();
        return ret.release();
    }

    HybridBuildStrategy::HybridBuildStrategy( const ParsedQuery &parsedQuery,
                                             const shared_ptr<QueryOptimizerCursor> &cursor,
                                             BufBuilder &buf ) :
    ResponseBuildStrategy( parsedQuery, cursor, buf ),
    _orderedBuild( _parsedQuery, _cursor, _buf ),
    _reorderedMatches() {
    }

    void HybridBuildStrategy::init() {
        _reorderBuild.reset( ReorderBuildStrategy::make( _parsedQuery, _cursor, _buf,
                                                         QueryPlanSummary() ) );
    }

    bool HybridBuildStrategy::handleMatch( ResultDetails* resultDetails ) {
        if ( !_queryOptimizerCursor->currentPlanScanAndOrderRequired() ) {
            return _orderedBuild.handleMatch( resultDetails );
        }
        return handleReorderMatch( resultDetails );
    }
    
    bool HybridBuildStrategy::handleReorderMatch( ResultDetails* resultDetails ) {
        DiskLoc loc = _cursor->currLoc();
        if ( _scanAndOrderDups.getsetdup( loc ) ) {
            return false;
        }
        resultDetails->match = true;
        try {
            _reorderBuild->_handleMatchNoDedup( resultDetails );
        } catch ( const UserException &e ) {
            if ( e.getCode() == ScanAndOrderMemoryLimitExceededAssertionCode ) {
                if ( _queryOptimizerCursor->hasPossiblyExcludedPlans() ) {
                    _queryOptimizerCursor->clearIndexesForPatterns();
                    throw QueryRetryException();
                }
                else if ( _queryOptimizerCursor->runningInitialInOrderPlan() ) {
                    _queryOptimizerCursor->abortOutOfOrderPlans();
                    return true;
                }
            }
            throw;
        }
        return true;
    }
    
    int HybridBuildStrategy::rewriteMatches() {
        if ( !_queryOptimizerCursor->completePlanOfHybridSetScanAndOrderRequired() ) {
            return _orderedBuild.rewriteMatches();
        }
        _reorderedMatches = true;
        resetBuf();
        return _reorderBuild->rewriteMatches();
    }

    int HybridBuildStrategy::bufferedMatches() const {
        return _reorderedMatches ?
                _reorderBuild->bufferedMatches() :
                _orderedBuild.bufferedMatches();
    }

    void HybridBuildStrategy::finishedFirstBatch() {
        _queryOptimizerCursor->abortOutOfOrderPlans();
    }
    
    QueryResponseBuilder *QueryResponseBuilder::make( const ParsedQuery &parsedQuery,
                                                     const shared_ptr<Cursor> &cursor,
                                                     const QueryPlanSummary &queryPlan,
                                                     const BSONObj &oldPlan ) {
        auto_ptr<QueryResponseBuilder> ret( new QueryResponseBuilder( parsedQuery, cursor ) );
        ret->init( queryPlan, oldPlan );
        return ret.release();
    }
    
    QueryResponseBuilder::QueryResponseBuilder( const ParsedQuery &parsedQuery,
                                               const shared_ptr<Cursor> &cursor ) :
    _parsedQuery( parsedQuery ),
    _cursor( cursor ),
    _queryOptimizerCursor( dynamic_pointer_cast<QueryOptimizerCursor>( _cursor ) ),
    _buf( 32768 ) { // TODO be smarter here
    }
    
    void QueryResponseBuilder::init( const QueryPlanSummary &queryPlan, const BSONObj &oldPlan ) {
        _collMetadata = newCollMetadata();
        _explain = newExplainRecordingStrategy( queryPlan, oldPlan );
        _builder = newResponseBuildStrategy( queryPlan );
        _builder->resetBuf();
    }

    bool QueryResponseBuilder::addMatch() {
        ResultDetails resultDetails;

        if ( _parsedQuery.getFields() && _parsedQuery.getFields()->getArrayOpType() == Projection::ARRAY_OP_POSITIONAL ) {
            // field projection specified, and contains an array operator
            resultDetails.matchDetails.requestElemMatchKey();
        }

        bool match =
                currentMatches( &resultDetails ) &&
                chunkMatches( &resultDetails ) &&
                _builder->handleMatch( &resultDetails );

        _explain->noteIterate( resultDetails );
        return match;
    }

    void QueryResponseBuilder::noteYield() {
        _explain->noteYield();
    }

    bool QueryResponseBuilder::enoughForFirstBatch() const {
        return _parsedQuery.enoughForFirstBatch( _builder->bufferedMatches(), _buf.len() );
    }

    bool QueryResponseBuilder::enoughTotalResults() const {
        if ( _parsedQuery.isExplain() ) {
            return _parsedQuery.enoughForExplain( _explain->orderedMatches() );
        }
        return ( _parsedQuery.enough( _builder->bufferedMatches() ) ||
                _buf.len() >= MaxBytesToReturnToClientAtOnce );
    }

    void QueryResponseBuilder::finishedFirstBatch() {
        _builder->finishedFirstBatch();
    }

    int QueryResponseBuilder::handoff( Message &result ) {
        int rewriteCount = _builder->rewriteMatches();
        if ( _parsedQuery.isExplain() ) {
            shared_ptr<ExplainQueryInfo> explainInfo = _explain->doneQueryInfo();
            if ( rewriteCount != -1 ) {
                explainInfo->reviseN( rewriteCount );
            }
            _builder->resetBuf();
            fillQueryResultFromObj( _buf, 0, explainInfo->bson() );
            result.appendData( _buf.buf(), _buf.len() );
            _buf.decouple();
            return 1;
        }
        if ( _buf.len() > 0 ) {
            result.appendData( _buf.buf(), _buf.len() );
            _buf.decouple();
        }
        return _builder->bufferedMatches();
    }

    CollectionMetadataPtr QueryResponseBuilder::newCollMetadata() const {
        if ( !shardingState.needCollectionMetadata( _parsedQuery.ns() ) ) {
            return CollectionMetadataPtr();
        }
        return shardingState.getCollectionMetadata( _parsedQuery.ns() );
    }

    shared_ptr<ExplainRecordingStrategy> QueryResponseBuilder::newExplainRecordingStrategy
    ( const QueryPlanSummary &queryPlan, const BSONObj &oldPlan ) const {
        if ( !_parsedQuery.isExplain() ) {
            return shared_ptr<ExplainRecordingStrategy>( new NoExplainStrategy() );
        }
        ExplainQueryInfo::AncillaryInfo ancillaryInfo;
        ancillaryInfo._oldPlan = oldPlan;
        if ( _queryOptimizerCursor ) {
            return shared_ptr<ExplainRecordingStrategy>
            ( new QueryOptimizerCursorExplainStrategy( ancillaryInfo, _queryOptimizerCursor ) );
        }
        shared_ptr<ExplainRecordingStrategy> ret
        ( new SimpleCursorExplainStrategy( ancillaryInfo, _cursor ) );
        ret->notePlan( queryPlan.valid() && queryPlan.scanAndOrderRequired,
                       queryPlan.keyFieldsOnly );
        return ret;
    }

    shared_ptr<ResponseBuildStrategy> QueryResponseBuilder::newResponseBuildStrategy
    ( const QueryPlanSummary &queryPlan ) {
        bool unordered = _parsedQuery.getOrder().isEmpty();
        bool empty = !_cursor->ok();
        bool singlePlan = !_queryOptimizerCursor;
        bool singleOrderedPlan =
                singlePlan && ( !queryPlan.valid() || !queryPlan.scanAndOrderRequired );
        CandidatePlanCharacter queryOptimizerPlans;
        if ( _queryOptimizerCursor ) {
            queryOptimizerPlans = _queryOptimizerCursor->initialCandidatePlans();
        }
        if ( unordered ||
            empty ||
            singleOrderedPlan ||
            ( !singlePlan && !queryOptimizerPlans.mayRunOutOfOrderPlan() ) ) {
            return shared_ptr<ResponseBuildStrategy>
            ( new OrderedBuildStrategy( _parsedQuery, _cursor, _buf ) );
        }
        if ( singlePlan ||
            !queryOptimizerPlans.mayRunInOrderPlan() ) {
            return shared_ptr<ResponseBuildStrategy>
            ( ReorderBuildStrategy::make( _parsedQuery, _cursor, _buf, queryPlan ) );
        }
        return shared_ptr<ResponseBuildStrategy>
        ( HybridBuildStrategy::make( _parsedQuery, _queryOptimizerCursor, _buf ) );
    }

    bool QueryResponseBuilder::currentMatches( ResultDetails* resultDetails ) {
        bool matches = _cursor->currentMatches( &resultDetails->matchDetails );
        if ( resultDetails->matchDetails.hasLoadedRecord() ) {
            resultDetails->loadedRecord = true;
        }
        return matches;
    }

    bool QueryResponseBuilder::chunkMatches( ResultDetails* resultDetails ) {
        if ( !_collMetadata ) {
            return true;
        }
        // TODO: should make this covered at some point
        resultDetails->loadedRecord = true;
        KeyPattern kp( _collMetadata->getKeyPattern() );
        if ( _collMetadata->keyBelongsToMe( kp.extractSingleKey( _cursor->current() ) ) ) {
            return true;
        }
        resultDetails->chunkSkip = true;
        return false;
    }
    
    /**
     * Run a query with a cursor provided by the query optimizer, or FindingStartCursor.
     * @yields the db lock.
     */
    string queryWithQueryOptimizer( int queryOptions, const string& ns,
                                    const BSONObj &jsobj, CurOp& curop,
                                    const BSONObj &query, const BSONObj &order,
                                    const shared_ptr<ParsedQuery> &pq_shared,
                                    const BSONObj &oldPlan,
                                    const ChunkVersion &shardingVersionAtStart,
                                    scoped_ptr<PageFaultRetryableSection>& parentPageFaultSection,
                                    scoped_ptr<NoPageFaultsAllowed>& noPageFault,
                                    Message &result ) {

        const ParsedQuery &pq( *pq_shared );
        shared_ptr<Cursor> cursor;
        QueryPlanSummary queryPlan;
        
        if ( pq.hasOption( QueryOption_OplogReplay ) ) {
            cursor = FindingStartCursor::getCursor( ns.c_str(), query, order );
        }
        else {
            cursor = getOptimizedCursor( ns.c_str(),
                                         query,
                                         order,
                                         QueryPlanSelectionPolicy::any(),
                                         pq_shared,
                                         false,
                                         &queryPlan );
        }
        verify( cursor );
        
        scoped_ptr<QueryResponseBuilder> queryResponseBuilder
                ( QueryResponseBuilder::make( pq, cursor, queryPlan, oldPlan ) );
        bool saveClientCursor = false;
        OpTime slaveReadTill;
        ClientCursorHolder ccPointer( new ClientCursor( QueryOption_NoCursorTimeout, cursor,
                                                         ns ) );
        
        for( ; cursor->ok(); cursor->advance() ) {

            bool yielded = false;
            if ( !ccPointer->yieldSometimes( ClientCursor::MaybeCovered, &yielded ) ||
                !cursor->ok() ) {
                cursor.reset();
                queryResponseBuilder->noteYield();
                // !!! TODO The queryResponseBuilder still holds cursor.  Currently it will not do
                // anything unsafe with the cursor in handoff(), but this is very fragile.
                //
                // We don't fail the query since we're fine with returning partial data if the
                // collection was dropped.
                // NOTE see SERVER-2454.
                // TODO This is wrong.  The cursor could be gone if the closeAllDatabases command
                // just ran.
                break;
            }

            if ( yielded ) {
                queryResponseBuilder->noteYield();
            }
            
            if ( pq.getMaxScan() && cursor->nscanned() > pq.getMaxScan() ) {
                break;
            }
            
            if ( !queryResponseBuilder->addMatch() ) {
                continue;
            }
            
            // Note slave's position in the oplog.
            if ( pq.hasOption( QueryOption_OplogReplay ) ) {
                BSONObj current = cursor->current();
                BSONElement e = current["ts"];
                if ( e.type() == Date || e.type() == Timestamp ) {
                    slaveReadTill = e._opTime();
                }
            }
            
            if ( !cursor->supportGetMore() || pq.isExplain() ) {
                if ( queryResponseBuilder->enoughTotalResults() ) {
                    break;
                }
            }
            else if ( queryResponseBuilder->enoughForFirstBatch() ) {
                // if only 1 requested, no cursor saved for efficiency...we assume it is findOne()
                if ( pq.wantMore() && pq.getNumToReturn() != 1 ) {
                    queryResponseBuilder->finishedFirstBatch();
                    if ( cursor->advance() ) {
                        saveClientCursor = true;
                    }
                }
                break;
            }
        }
        
        if ( cursor ) {
            if ( pq.hasOption( QueryOption_CursorTailable ) && pq.getNumToReturn() != 1 ) {
                cursor->setTailable();
            }
            
            // If the tailing request succeeded.
            if ( cursor->tailable() ) {
                saveClientCursor = true;
            }
        }
        
        if ( ! shardingState.getVersion( ns ).isWriteCompatibleWith( shardingVersionAtStart ) ) {
            // if the version changed during the query
            // we might be missing some data
            // and its safe to send this as mongos can resend
            // at this point
            throw SendStaleConfigException(ns, "version changed during initial query",
                                           shardingVersionAtStart,
                                           shardingState.getVersion(ns));
        }
        
        parentPageFaultSection.reset(0);
        noPageFault.reset( new NoPageFaultsAllowed() );

        int nReturned = queryResponseBuilder->handoff( result );

        ccPointer.reset();
        long long cursorid = 0;
        if ( saveClientCursor ) {
            // Create a new ClientCursor, with a default timeout.
            ccPointer.reset( new ClientCursor( queryOptions, cursor, ns,
                                              jsobj.getOwned() ) );
            cursorid = ccPointer->cursorid();
            DEV { MONGO_TLOG(2) << "query has more, cursorid: " << cursorid << endl; }
            if ( cursor->supportYields() ) {
                ClientCursor::YieldData data;
                ccPointer->prepareToYield( data );
            }
            else {
                ccPointer->c()->noteLocation();
            }
            
            // Save slave's position in the oplog.
            if ( pq.hasOption( QueryOption_OplogReplay ) && !slaveReadTill.isNull() ) {
                ccPointer->slaveReadTill( slaveReadTill );
            }
            
            if ( !ccPointer->ok() && ccPointer->c()->tailable() ) {
                DEV {
                    MONGO_TLOG(0) << "query has no more but tailable, cursorid: " << cursorid <<
                        endl;
                }
            }
            
            if( queryOptions & QueryOption_Exhaust ) {
                curop.debug().exhaust = true;
            }
            
            // Set attributes for getMore.
            ccPointer->setCollMetadata( queryResponseBuilder->collMetadata() );
            ccPointer->setPos( nReturned );
            ccPointer->pq = pq_shared;
            ccPointer->fields = pq.getFieldPtr();

            // If the query had a time limit, remaining time is "rolled over" to the cursor (for
            // use by future getmore ops).
            ccPointer->setLeftoverMaxTimeMicros( curop.getRemainingMaxTimeMicros() );

            ccPointer.release();
        }
        
        QueryResult *qr = (QueryResult *) result.header();
        qr->cursorId = cursorid;
        curop.debug().cursorid = ( cursorid == 0 ? -1 : qr->cursorId );
        qr->setResultFlagsToOk();
        // qr->len is updated automatically by appendData()
        curop.debug().responseLength = qr->len;
        qr->setOperation(opReply);
        qr->startingFrom = 0;
        qr->nReturned = nReturned;

        curop.debug().nscanned = ( cursor ? cursor->nscanned() : 0LL );
        curop.debug().ntoskip = pq.getSkip();
        curop.debug().nreturned = nReturned;

        return curop.debug().exhaust ? ns : "";
    }

    bool queryIdHack( const char* ns, const BSONObj& query, const ParsedQuery& pq, CurOp& curop, Message& result ) {
        // notes:
        //  do not touch result inside of PageFaultRetryableSection area

        Client& currentClient = cc(); // only here since its safe and takes time
        auto_ptr< QueryResult > qr;
        
        {
            // this extra bracing is not strictly needed
            // but makes it clear what the rules are in different spots
 
            scoped_ptr<PageFaultRetryableSection> pgfs;
            if ( ! currentClient.getPageFaultRetryableSection() )
                pgfs.reset( new PageFaultRetryableSection() );
            while ( 1 ) {
                try {
                    
                    int n = 0;
                    bool nsFound = false;
                    bool indexFound = false;
                    
                    BSONObj resObject; // put inside since we don't own the memory
                    
                    Client::ReadContext ctx( ns , dbpath ); // read locks
                    replVerifyReadsOk(&pq);
                    
                    bool found = Helpers::findById( currentClient, ns, query, resObject, &nsFound, &indexFound );
                    if ( nsFound && ! indexFound ) {
                        // we have to resort to a table scan
                        return false;
                    }
                    
                    if ( shardingState.needCollectionMetadata( ns ) ) {
                        CollectionMetadataPtr m = shardingState.getCollectionMetadata( ns );
                        if ( m ) {
                            KeyPattern kp( m->getKeyPattern() );
                            if ( !m->keyBelongsToMe( kp.extractSingleKey( resObject ) ) ) {
                                // I have something this _id
                                // but it doesn't belong to me
                                // so return nothing
                                resObject = BSONObj();
                                found = false;
                            }
                        }
                    }
                    
                    BufBuilder bb(sizeof(QueryResult)+resObject.objsize()+32);
                    bb.skip(sizeof(QueryResult));
                    
                    curop.debug().idhack = true;
                    if ( found ) {
                        n = 1;
                        fillQueryResultFromObj( bb , pq.getFields() , resObject );
                    }
                    
                    qr.reset( (QueryResult *) bb.buf() );
                    bb.decouple();
                    qr->setResultFlagsToOk();
                    qr->len = bb.len();
                    
                    curop.debug().responseLength = bb.len();
                    qr->setOperation(opReply);
                    qr->cursorId = 0;
                    qr->startingFrom = 0;
                    qr->nReturned = n;
                    
                    break;
                }
                catch ( PageFaultException& e ) {
                    e.touch();
                }
            }
        }

        result.setData( qr.release(), true );
        return true;
    }
    
    /**
     * Run a query -- includes checking for and running a Command.
     * @return points to ns if exhaust mode. 0=normal mode
     * @locks the db mutex for reading (and potentially for writing temporarily to create a new db).
     * @yields the db mutex periodically after acquiring it.
     * @asserts on scan and order memory exhaustion and other cases.
     */
    string runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result) {
        shared_ptr<ParsedQuery> pq_shared( new ParsedQuery(q) );
        ParsedQuery& pq( *pq_shared );
        BSONObj jsobj = q.query;
        int queryOptions = q.queryOptions;
        const char *ns = q.ns;
        
        uassert( 16332 , "can't have an empty ns" , ns[0] );

        LOG(2) << "runQuery called " << ns << " " << jsobj << endl;

        curop.debug().ns = ns;
        curop.debug().ntoreturn = pq.getNumToReturn();
        curop.debug().query = jsobj;
        curop.setQuery(jsobj);

        const NamespaceString nsString( ns );
        uassert( 16256, str::stream() << "Invalid ns [" << ns << "]", nsString.isValid() );

        // Run a command.

        if ( nsString.isCommand() ) {
            int nToReturn = pq.getNumToReturn();
            uassert( 16979, str::stream() << "bad numberToReturn (" << nToReturn
                                          << ") for $cmd type ns - can only be 1 or -1",
                     nToReturn == 1 || nToReturn == -1 );

            curop.markCommand();
            BufBuilder bb;
            bb.skip(sizeof(QueryResult));
            BSONObjBuilder cmdResBuf;
            if ( runCommands(ns, jsobj, curop, bb, cmdResBuf, false, queryOptions) ) {
                curop.debug().iscommand = true;
                curop.debug().query = jsobj;

                auto_ptr< QueryResult > qr;
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                curop.debug().responseLength = bb.len();
                qr->setOperation(opReply);
                qr->cursorId = 0;
                qr->startingFrom = 0;
                qr->nReturned = 1;
                result.setData( qr.release(), true );
            }
            else {
                uasserted(13530, "bad or malformed command request?");
            }
            return "";
        }

        bool explain = pq.isExplain();
        BSONObj order = pq.getOrder();
        BSONObj query = pq.getFilter();

        /* The ElemIter will not be happy if this isn't really an object. So throw exception
           here when that is true.
           (Which may indicate bad data from client.)
        */
        if ( query.objsize() == 0 ) {
            out() << "Bad query object?\n  jsobj:";
            out() << jsobj.toString() << "\n  query:";
            out() << query.toString() << endl;
            uassert( 10110 , "bad query object", false);
        }

        if (isNewQueryFrameworkEnabled()) {
            // TODO: Copy prequel curop debugging into runNewQuery
            return newRunQuery(m, q, curop, result);
        }

        // Handle query option $maxTimeMS (not used with commands).
        curop.setMaxTimeMicros(static_cast<unsigned long long>(pq.getMaxTimeMS()) * 1000);
        if (MONGO_FAIL_POINT(maxTimeAlwaysTimeOut) && pq.getMaxTimeMS() != 0) {
            uasserted(ErrorCodes::ExceededTimeLimit,
                      "operation exceeded time limit [maxTimeAlwaysTimeOut]");
        }

        // Run a simple id query.
        if ( ! (explain || pq.showDiskLoc()) && isSimpleIdQuery( query ) && !pq.hasOption( QueryOption_CursorTailable ) ) {
            if ( queryIdHack( ns, query, pq, curop, result ) ) {
                return "";
            }
        }

        // sanity check the query and projection
        if ( pq.getFields() != NULL )
            pq.getFields()->validateQuery( query );

        // these now may stored in a ClientCursor or somewhere else,
        // so make sure we use a real copy
        jsobj = jsobj.getOwned();
        query = query.getOwned();
        order = order.getOwned();

        bool hasRetried = false;
        scoped_ptr<PageFaultRetryableSection> pgfs;
        scoped_ptr<NoPageFaultsAllowed> npfe;
        while ( 1 ) {

            if ( ! cc().getPageFaultRetryableSection() ) {
                verify( ! pgfs );
                pgfs.reset( new PageFaultRetryableSection() );
            }
                
            try {
                Client::ReadContext ctx( ns , dbpath ); // read locks
                const ChunkVersion shardingVersionAtStart = shardingState.getVersion( ns );
                
                replVerifyReadsOk(&pq);
                
                if ( pq.hasOption( QueryOption_CursorTailable ) ) {
                    NamespaceDetails *d = nsdetails( ns );
                    uassert( 13051, "tailable cursor requested on non capped collection", d && d->isCapped() );
                    const BSONObj nat1 = BSON( "$natural" << 1 );
                    if ( order.isEmpty() ) {
                        order = nat1;
                    }
                    else {
                        uassert( 13052, "only {$natural:1} order allowed for tailable cursor", order == nat1 );
                    }
                }
                
                
                // Run a regular query.
                
                BSONObj oldPlan;
                if ( ! hasRetried && explain && ! pq.hasIndexSpecifier() ) {
                    scoped_ptr<MultiPlanScanner> mps( MultiPlanScanner::make( ns, query, order ) );
                    oldPlan = mps->cachedPlanExplainSummary();
                }
             
   
                return queryWithQueryOptimizer( queryOptions, ns, jsobj, curop, query, order,
                                                pq_shared, oldPlan, shardingVersionAtStart, 
                                                pgfs, npfe, result );
            }
            catch ( PageFaultException& e ) {
                e.touch();
            }
            catch ( const QueryRetryException & ) {
                // In some cases the query may be retried if there is an in memory sort size assertion.
                verify( ! hasRetried );
                hasRetried = true;
            }
        }
    }

} // namespace mongo
