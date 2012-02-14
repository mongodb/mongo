// @file queryoptimizercursorimpl.cpp

/**
 *    Copyright (C) 2011 10gen Inc.
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
 */

#include "queryoptimizercursorimpl.h"

#include "pch.h"
#include "pdfile.h"
#include "clientcursor.h"
#include "btree.h"
#include "explain.h"

namespace mongo {
    
    extern bool useHints;
    
    static const int OutOfOrderDocumentsAssertionCode = 14810;
        
    QueryPlanSelectionPolicy::Any QueryPlanSelectionPolicy::__any;
    const QueryPlanSelectionPolicy &QueryPlanSelectionPolicy::any() { return __any; }
    
    bool QueryPlanSelectionPolicy::IndexOnly::permitPlan( const QueryPlan &plan ) const {
        return !plan.willScanTable();
    }
    QueryPlanSelectionPolicy::IndexOnly QueryPlanSelectionPolicy::__indexOnly;
    const QueryPlanSelectionPolicy &QueryPlanSelectionPolicy::indexOnly() { return __indexOnly; }
    
    bool QueryPlanSelectionPolicy::IdElseNatural::permitPlan( const QueryPlan &plan ) const {
        return !plan.indexed() || plan.index()->isIdIndex();
    }
    BSONObj QueryPlanSelectionPolicy::IdElseNatural::planHint( const char *ns ) const {
        NamespaceDetails *nsd = nsdetails( ns );
        if ( !nsd || !nsd->haveIdIndex() ) {
            return BSON( "$hint" << BSON( "$natural" << 1 ) );
        }
        return BSON( "$hint" << nsd->idx( nsd->findIdIndex() ).indexName() );
    }
    QueryPlanSelectionPolicy::IdElseNatural QueryPlanSelectionPolicy::__idElseNatural;
    const QueryPlanSelectionPolicy &QueryPlanSelectionPolicy::idElseNatural() {
        return __idElseNatural;
    }

    /**
     * A QueryOp implementation utilized by the QueryOptimizerCursor
     */
    class QueryOptimizerCursorOp : public QueryOp {
    public:
        /**
         * @param aggregateNscanned - shared long long counting total nscanned for
         * query ops for all cursors.
         */
        QueryOptimizerCursorOp( long long &aggregateNscanned, const QueryPlanSelectionPolicy &selectionPolicy,
                               const bool &requireOrder, int cumulativeCount = 0 ) :
        _matchCounter( aggregateNscanned, cumulativeCount ),
        _countingMatches(),
        _mustAdvance(),
        _capped(),
        _yieldRecoveryFailed(),
        _selectionPolicy( selectionPolicy ),
        _requireOrder( requireOrder ) {
        }
        
        virtual void _init() {
            checkCursorOrdering();
            if ( !_selectionPolicy.permitPlan( qp() ) ) {
                throw MsgAssertionException( 9011,
                                            str::stream()
                                            << "Plan not permitted by query plan selection policy '"
                                            << _selectionPolicy.name()
                                            << "'" );
            }
            
            _c = qp().newCursor();

            // The QueryOptimizerCursor::prepareToTouchEarlierIterate() implementation requires _c->prepareToYield() to work.
//            verify( 15940, _c->supportYields() );
            _capped = _c->capped();

            // TODO This violates the current Cursor interface abstraction, but for now it's simpler to keep our own set of
            // dups rather than avoid poisoning the cursor's dup set with unreturned documents.  Deduping documents
            // matched in this QueryOptimizerCursorOp will run against the takeover cursor.
            _matchCounter.setCheckDups( _c->isMultiKey() );
            // TODO ok if cursor becomes multikey later?

            _matchCounter.updateNscanned( _c->nscanned() );
        }
        
        virtual long long nscanned() {
            return _c ? _c->nscanned() : _matchCounter.nscanned();
        }
        
        virtual bool prepareToYield() {
            if ( _c && !_cc ) {
                _cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , _c , qp().ns() ) );
            }
            if ( _cc ) {
                recordCursorLocation();
                return _cc->prepareToYield( _yieldData );
            }
            // no active cursor - ok to yield
            return true;
        }
        
        virtual void recoverFromYield() {
            if ( _explainPlanInfo ) _explainPlanInfo->noteYield();
            if ( _cc && !ClientCursor::recoverFromYield( _yieldData ) ) {
                _yieldRecoveryFailed = true;
                _c.reset();
                _cc.reset();
                
                if ( _capped ) {
                    msgassertedNoTrace( 13338, str::stream() << "capped cursor overrun: " << qp().ns() );
                }
                else if ( qp().mustAssertOnYieldFailure() ) {
                    msgassertedNoTrace( 15892, str::stream() << "QueryOptimizerCursorOp::recoverFromYield() failed to recover" );
                }
                else {
                    // we don't fail query since we're fine with returning partial data if collection dropped
                    // also, see SERVER-2454
                }
            }
            else {
                checkCursorAdvanced();
            }
        }

        void prepareToTouchEarlierIterate() {
            recordCursorLocation();
            if ( _c ) {
                _c->prepareToTouchEarlierIterate();
            }
        }

        void recoverFromTouchingEarlierIterate() {
            if ( _c ) {
                _c->recoverFromTouchingEarlierIterate();
            }
            checkCursorAdvanced();
        }
        
        virtual void next() {
            checkCursorOrdering();

            mayAdvance();
            
            if ( !qp().scanAndOrderRequired() && _matchCounter.enoughCumulativeMatchesToChooseAPlan() ) {
                setStop();
                if ( _explainPlanInfo ) _explainPlanInfo->notePicked();
                return;
            }
            if ( !_c || !_c->ok() ) {
                if ( _explainPlanInfo && _c ) _explainPlanInfo->noteDone( *_c );
                setComplete();
                return;
            }
            
            _mustAdvance = true;
        }
        virtual QueryOp *_createChild() const {
            return new QueryOptimizerCursorOp( _matchCounter.aggregateNscanned(), _selectionPolicy, _requireOrder, _matchCounter.cumulativeCount() );
        }
        DiskLoc currLoc() const { return _c ? _c->currLoc() : DiskLoc(); }
        BSONObj currKey() const { return _c ? _c->currKey() : BSONObj(); }
        bool currentMatches( MatchDetails *details ) {
            if ( !_c || !_c->ok() ) {
                _matchCounter.setMatch( false );
                return false;
            }
            
            MatchDetails myDetails;
            bool wantDetails = details || _explainPlanInfo;

            bool match = matcher( _c.get() )->matchesCurrent( _c.get(),
                                                             wantDetails ? &myDetails : 0 );
            // Cache the match, so we can count it in mayAdvance().
            bool newMatch = _matchCounter.setMatch( match );

            if ( _explainPlanInfo ) {
                bool countableMatch = newMatch && _matchCounter.wouldCountMatch( _c->currLoc() );
                _explainPlanInfo->noteIterate( countableMatch,
                                              countableMatch || myDetails._loadedObject,
                                              false, *_c );
            }
            if ( details ) *details = myDetails;

            return match;
        }
        virtual bool mayRecordPlan() const {
            return !_yieldRecoveryFailed && complete() && ( !stopRequested() || _matchCounter.enoughMatchesToRecordPlan() );
        }
        shared_ptr<Cursor> cursor() const { return _c; }
        virtual shared_ptr<ExplainPlanInfo> generateExplainInfo() {
            if ( !_c ) {
                return QueryOp::generateExplainInfo();
            }
            _explainPlanInfo.reset( new ExplainPlanInfo() );
            _explainPlanInfo->notePlan( *_c, qp().scanAndOrderRequired() );
            return _explainPlanInfo;
        }
        shared_ptr<ExplainPlanInfo> explainInfo() const { return _explainPlanInfo; }
    private:
        void mayAdvance() {
            if ( !_c ) {
                return;
            }
            if ( countingMatches() ) {
                // Check match if not yet known.
                if ( !_matchCounter.knowMatch() ) {
                    currentMatches( 0 );
                }
                _matchCounter.countMatch( currLoc() );
            }
            if ( _mustAdvance ) {
                _c->advance();
                handleCursorAdvanced();
            }
            _matchCounter.updateNscanned( _c->nscanned() );
        }
        // Don't count matches on the first call to next(), which occurs before the first result is returned.
        bool countingMatches() {
            if ( _countingMatches ) {
                return true;
            }
            _countingMatches = true;
            return false;
        }

        void recordCursorLocation() {
            _posBeforeYield = currLoc();
        }
        void checkCursorAdvanced() {
            // This check will not correctly determine if we are looking at a different document in
            // all cases, but it is adequate for updating the query plan's match count (just used to pick
            // plans, not returned to the client) and adjust iteration via _mustAdvance.
            if ( _posBeforeYield != currLoc() ) {
                // If the yield advanced our position, the next next() will be a no op.
                handleCursorAdvanced();
            }
        }
        void handleCursorAdvanced() {
            _mustAdvance = false;
            _matchCounter.resetMatch();
        }
        void checkCursorOrdering() {
            if ( _requireOrder && qp().scanAndOrderRequired() ) {
                throw MsgAssertionException( OutOfOrderDocumentsAssertionCode, "order spec cannot be satisfied with index" );
            }
        }

        CachedMatchCounter _matchCounter;
        bool _countingMatches;
        bool _mustAdvance;
        bool _capped;
        shared_ptr<Cursor> _c;
        ClientCursor::CleanupPointer _cc;
        DiskLoc _posBeforeYield;
        ClientCursor::YieldData _yieldData;
        bool _yieldRecoveryFailed;
        const QueryPlanSelectionPolicy &_selectionPolicy;
        const bool &_requireOrder; // TODO don't use a ref for this, but signal change explicitly
        shared_ptr<ExplainPlanInfo> _explainPlanInfo;
    };
    
    /**
     * This cursor runs a MultiPlanScanner iteratively and returns results from
     * the scanner's cursors as they become available.  Once the scanner chooses
     * a single plan, this cursor becomes a simple wrapper around that single
     * plan's cursor (called the 'takeover' cursor).
     */
    class QueryOptimizerCursorImpl : public QueryOptimizerCursor {
    public:
        QueryOptimizerCursorImpl( auto_ptr<MultiPlanScanner> &mps, const QueryPlanSelectionPolicy &planPolicy, bool requireOrder ) :
        _requireOrder( requireOrder ),
        _mps( mps ),
        _originalOp( new QueryOptimizerCursorOp( _nscanned, planPolicy, _requireOrder ) ),
        _currOp(),
        _completeOp(),
        _nscanned() {
            _mps->initialOp( _originalOp );
            _explainQueryInfo = _mps->generateExplainInfo();
            shared_ptr<QueryOp> op = _mps->nextOp();
            rethrowOnError( op );
            if ( !op->complete() ) {
                _currOp = dynamic_cast<QueryOptimizerCursorOp*>( op.get() );
            }
        }
        
        virtual bool ok() { return _takeover ? _takeover->ok() : !currLoc().isNull(); }
        
        virtual Record* _current() {
            if ( _takeover ) {
                return _takeover->_current();
            }
            assertOk();
            return currLoc().rec();
        }
        
        virtual BSONObj current() {
            if ( _takeover ) {
                return _takeover->current();
            }
            assertOk();
            return currLoc().obj();
        }
        
        virtual DiskLoc currLoc() { return _takeover ? _takeover->currLoc() : _currLoc(); }
        
        DiskLoc _currLoc() const {
            dassert( !_takeover );
            return _currOp ? _currOp->currLoc() : DiskLoc();
        }
        
        virtual bool advance() {
            return _advance( false );
        }
        
        virtual BSONObj currKey() const {
            if ( _takeover ) {
             	return _takeover->currKey();   
            }
            assertOk();
            return _currOp->currKey();
        }
        
        /**
         * When return value isNull(), our cursor will be ignored for yielding by the client cursor implementation.
         * In such cases, an internal ClientCursor will update the position of component cursors when necessary.
         */
        virtual DiskLoc refLoc() { return _takeover ? _takeover->refLoc() : DiskLoc(); }
        
        virtual BSONObj indexKeyPattern() {
            if ( _takeover ) {
                return _takeover->indexKeyPattern();
            }
            assertOk();
            return _currOp->cursor()->indexKeyPattern();
        }
        
        virtual bool supportGetMore() { return true; }

        virtual bool supportYields() { return _takeover ? _takeover->supportYields() : true; }
        
        virtual void prepareToTouchEarlierIterate() {
            if ( _takeover ) {
                _takeover->prepareToTouchEarlierIterate();
            }
            else if ( _currOp ) {
                if ( _mps->currentNPlans() == 1 ) {
                    // This single plan version is a bit more performant, so we use it when possible.
                    _currOp->prepareToTouchEarlierIterate();
                }
                else {
                    // With multiple plans, the 'earlier iterate' could be the current iterate of one of
                    // the component plans.  We do a full yield of all plans, using ClientCursors.
                    verify( 15941, _mps->prepareToYield() );
                }
            }
        }

        virtual void recoverFromTouchingEarlierIterate() {
            if ( _takeover ) {
                _takeover->recoverFromTouchingEarlierIterate();
            }
            else if ( _currOp ) {
                if ( _mps->currentNPlans() == 1 ) {
                    _currOp->recoverFromTouchingEarlierIterate();
                }
                else {
                    recoverFromYield();
                }
            }
        }

        virtual bool prepareToYield() {
            if ( _takeover ) {
                return _takeover->prepareToYield();
            }
            else if ( _currOp ) {
                return _mps->prepareToYield();
            }
            else {
                // No state needs to be protected, so yielding is fine.
                return true;
            }
        }
        
        virtual void recoverFromYield() {
            if ( _takeover ) {
                _takeover->recoverFromYield();
                return;
            }
            if ( _currOp ) {
                _mps->recoverFromYield();
                if ( _currOp->error() || !ok() ) {
                    // Advance to a non error op if one of the ops errored out.
                    // Advance to a following $or clause if the $or clause returned all results.
                    _advance( true );
                }
            }
        }
        
        virtual string toString() { return "QueryOptimizerCursor"; }
        
        virtual bool getsetdup(DiskLoc loc) {
            if ( _takeover ) {
                if ( getdupInternal( loc ) ) {
                    return true;   
                }
             	return _takeover->getsetdup( loc );   
            }
            assertOk();
            return getsetdupInternal( loc );                
        }
        
        /** Matcher needs to know if the the cursor being forwarded to is multikey. */
        virtual bool isMultiKey() const {
            if ( _takeover ) {
                return _takeover->isMultiKey();
            }
            assertOk();
            return _currOp->cursor()->isMultiKey();
        }
        
        virtual bool modifiedKeys() const { return true; }

        /** Initial capped wrapping cases (before takeover) are handled internally by a component ClientCursor. */
        virtual bool capped() const { return _takeover ? _takeover->capped() : false; }

        virtual long long nscanned() { return _takeover ? _takeover->nscanned() : _nscanned; }

        virtual shared_ptr<CoveredIndexMatcher> matcherPtr() const {
            if ( _takeover ) {
                return _takeover->matcherPtr();
            }
            assertOk();
            return _currOp->matcher( _currOp->cursor() );
        }

        virtual CoveredIndexMatcher* matcher() const {
            if ( _takeover ) {
                return _takeover->matcher();
            }
            assertOk();
            return _currOp->matcher( _currOp->cursor() ).get();
        }

        virtual bool currentMatches( MatchDetails *details = 0 ) {
            if ( _takeover ) {
                return _takeover->currentMatches( details );
            }
            assertOk();
            return _currOp->currentMatches( details );
        }
        
        virtual const QueryPlan *queryPlan() const {
            assertOk();
            return _currOp ? &_currOp->qp() : 0;
        }

        virtual const Cursor *queryCursor() const {
            assertOk();
            return _currOp ? _currOp->cursor().get() : 0;
        }

        virtual const QueryPlan *completeQueryPlan() const {
            return _completeOp ? &_completeOp->qp() : 0;
        }

        virtual const Cursor *completeQueryCursor() const {
            return _completeOp ? _completeOp->cursor().get() : 0;
        }

        virtual const MultiPlanScanner *multiPlanScanner() const {
            return _mps.get();
        }
        
        virtual void abortUnorderedPlans() {
            _requireOrder = true;
        }
        
        virtual void noteIterate( bool match, bool loadedDocument, bool chunkSkip ) {
            if ( _explainQueryInfo ) {
                _explainQueryInfo->noteIterate( match, loadedDocument, chunkSkip );
            }
            if ( _takeover ) {
                _takeover->noteIterate( match, loadedDocument );
            }
        }
        
        virtual shared_ptr<ExplainQueryInfo> explainQueryInfo() const {
            return _explainQueryInfo;
        }
        
    private:
        /**
         * Advances the QueryPlanSet::Runner.
         * @param force - advance even if the current query op is not valid.  The 'force' param should only be specified
         * when there are plans left in the runner.
         */
        bool _advance( bool force ) {
            if ( _takeover ) {
                return _takeover->advance();
            }

            if ( !force && !ok() ) {
                return false;
            }

            DiskLoc prevLoc = _currLoc();

            _currOp = 0;
            shared_ptr<QueryOp> op = _mps->nextOp();
            rethrowOnError( op );

            // Avoiding dynamic_cast here for performance.  Soon we won't need to
            // do a cast at all.
            QueryOptimizerCursorOp *qocop = (QueryOptimizerCursorOp*)( op.get() );

            if ( !op->complete() ) {
                // The 'qocop' will be valid until we call _mps->nextOp() again.  We return 'current' values from this op.
                _currOp = qocop;
            }
            else if ( op->stopRequested() ) {
//                log() << "stop requested" << endl;
                if ( qocop->cursor() ) {
                    // Ensure that prepareToTouchEarlierIterate() may be called safely when a BasicCursor takes over.
                    if ( !prevLoc.isNull() && prevLoc == qocop->currLoc() ) {
                        qocop->cursor()->advance();
                    }
                    _takeover.reset( new MultiCursor( _mps,
                                                     qocop->cursor(),
                                                     op->matcher( qocop->cursor() ),
                                                     qocop->explainInfo(),
                                                     *op,
                                                     _nscanned - qocop->cursor()->nscanned() ) );
                }
            }
            else {
                // TODO not set if takeover
                _completeOp = qocop;
            }

            return ok();
        }
        /** Forward an exception when the runner errs out. */
        void rethrowOnError( const shared_ptr< QueryOp > &op ) {
            if ( op->error() ) {
                throw MsgAssertionException( op->exception() );   
            }
        }
        
        void assertOk() const {
            massert( 14809, "Invalid access for cursor that is not ok()", !_currLoc().isNull() );
        }

        /** Insert and check for dups before takeover occurs */
        bool getsetdupInternal(const DiskLoc &loc) {
            return _dups.getsetdup( loc );
        }

        /** Just check for dups - after takeover occurs */
        bool getdupInternal(const DiskLoc &loc) {
            dassert( _takeover );
            return _dups.getdup( loc );
        }
        
        bool _requireOrder;
        auto_ptr<MultiPlanScanner> _mps;
        shared_ptr<QueryOptimizerCursorOp> _originalOp;
        QueryOptimizerCursorOp *_currOp;
        QueryOptimizerCursorOp *_completeOp;
        shared_ptr<MultiCursor> _takeover;
        long long _nscanned;
        // Using a SmallDupSet seems a bit hokey, but I've measured a 5% performance improvement
        // with ~100 document non multi key scans.
        SmallDupSet _dups;
        shared_ptr<ExplainQueryInfo> _explainQueryInfo;
    };
    
    shared_ptr<Cursor> newQueryOptimizerCursor( auto_ptr<MultiPlanScanner> mps, const QueryPlanSelectionPolicy &planPolicy, bool requireOrder ) {
        try {
            return shared_ptr<Cursor>( new QueryOptimizerCursorImpl( mps, planPolicy, requireOrder ) );
        } catch( const AssertionException &e ) {
            if ( e.getCode() == OutOfOrderDocumentsAssertionCode ) {
                // If no indexes follow the requested sort order, return an
                // empty pointer.  This is legacy behavior based on bestGuessCursor().
                return shared_ptr<Cursor>();
            }
            throw;
        }
        return shared_ptr<Cursor>();
    }
    
    shared_ptr<Cursor> NamespaceDetailsTransient::getCursor( const char *ns, const BSONObj &query,
                                                            const BSONObj &order,
                                                            const QueryPlanSelectionPolicy
                                                            &planPolicy,
                                                            bool *simpleEqualityMatch,
                                                            const ParsedQuery *parsedQuery ) {
        if ( simpleEqualityMatch ) {
            *simpleEqualityMatch = false;
        }

        BSONElement hint = (useHints && parsedQuery) ? parsedQuery->getHint() : BSONElement();
        bool snapshot = parsedQuery && parsedQuery->isSnapshot();

        BSONObj snapshotHint; // put here to keep the data in scope
        if( snapshot ) {
            NamespaceDetails *d = nsdetails(ns);
            if ( d ) {
                int i = d->findIdIndex();
                if( i < 0 ) {
                    if ( strstr( ns , ".system." ) == 0 )
                        log() << "warning: no _id index on $snapshot query, ns:" << ns << endl;
                }
                else {
                    /* [dm] the name of an _id index tends to vary, so we build the hint the hard way here.
                     probably need a better way to specify "use the _id index" as a hint.  if someone is
                     in the query optimizer please fix this then!
                     */
                    BSONObjBuilder b;
                    b.append("$hint", d->idx(i).indexName());
                    snapshotHint = b.obj();
                    hint = snapshotHint.firstElement();
                }
            }
        }

        bool mayShortcutQueryOptimizer =
        ( !parsedQuery ||
         ( parsedQuery->getMin().isEmpty() &&
          parsedQuery->getMax().isEmpty() ) ) &&
        hint.eoo();
        if ( mayShortcutQueryOptimizer ) {
            if ( planPolicy.permitOptimalNaturalPlan() && query.isEmpty() && order.isEmpty() && !requireIndex ) {
                // TODO This will not use a covered index currently.
                return theDataFileMgr.findAll( ns );
            }
            if ( planPolicy.permitOptimalIdPlan() && isSimpleIdQuery( query ) ) {
                Database *database = cc().database();
                verify( 15985, database );
                NamespaceDetails *d = database->namespaceIndex.details(ns);
                if ( d ) {
                    int idxNo = d->findIdIndex();
                    if ( idxNo >= 0 ) {
                        IndexDetails& i = d->idx( idxNo );
                        BSONObj key = i.getKeyFromQuery( query );
                        return shared_ptr<Cursor>( BtreeCursor::make( d, idxNo, i, key, key, true, 1 ) );
                    }
                }
            }
        }
        auto_ptr<MultiPlanScanner> mps( new MultiPlanScanner( ns, query, order, &hint, ( parsedQuery && parsedQuery->isExplain() ) ? QueryPlanSet::Ignore : QueryPlanSet::Use, parsedQuery ? parsedQuery->getMin() : BSONObj(), parsedQuery ? parsedQuery->getMax() : BSONObj() ) ); // mayYield == false
        const QueryPlan *singlePlan = mps->singlePlan();
        bool requireOrder = ( parsedQuery == 0 );
        if ( singlePlan && !singlePlan->scanAndOrderRequired() ) {
            if ( planPolicy.permitPlan( *singlePlan ) ) {
                shared_ptr<Cursor> single = singlePlan->newCursor();
                if ( !query.isEmpty() && !single->matcher() ) {
                    shared_ptr<CoveredIndexMatcher> matcher( new CoveredIndexMatcher( query, single->indexKeyPattern() ) );
                    single->setMatcher( matcher );
                }
                if ( simpleEqualityMatch ) {
                    if ( singlePlan->exactKeyMatch() && !single->matcher()->needRecord() ) {
                        *simpleEqualityMatch = true;
                    }
                }
                return single;
            }
        }
        if ( parsedQuery && parsedQuery->isExplain() ) {
            mps->generateExplainInfo();
        }
        bool requireOrder = ( parsedQuery == 0 );
        return newQueryOptimizerCursor( mps, planPolicy, requireOrder );
    }

    /** This interface is just available for testing. */
    shared_ptr<Cursor> newQueryOptimizerCursor
    ( const char *ns, const BSONObj &query, const BSONObj &order, const QueryPlanSelectionPolicy &planPolicy,
     bool requireOrder ) {
        auto_ptr<MultiPlanScanner> mps( new MultiPlanScanner( ns, query, order ) ); // mayYield == false
        return newQueryOptimizerCursor( mps, planPolicy, requireOrder );
    }
        
} // namespace mongo;
