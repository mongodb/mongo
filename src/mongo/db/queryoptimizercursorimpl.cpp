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

#include "mongo/db/queryoptimizercursorimpl.h"

#include "mongo/db/btreecursor.h"
#include "mongo/db/query_plan_selection_policy.h"
#include "mongo/db/query_plan_summary.h"
#include "mongo/db/query_optimizer_internal.h"
#include "mongo/db/queryutil.h"

namespace mongo {
    
    extern bool useHints;

    QueryOptimizerCursorImpl* QueryOptimizerCursorImpl::make
            ( auto_ptr<MultiPlanScanner>& mps,
              const QueryPlanSelectionPolicy& planPolicy,
              bool requireOrder,
              bool explain ) {
        auto_ptr<QueryOptimizerCursorImpl> ret( new QueryOptimizerCursorImpl( mps, planPolicy,
                                                                              requireOrder ) );
        ret->init( explain );
        return ret.release();
    }
        
    bool QueryOptimizerCursorImpl::ok() {
        return _takeover ? _takeover->ok() : !currLoc().isNull();
    }
        
    Record* QueryOptimizerCursorImpl::_current() {
        if ( _takeover ) {
            return _takeover->_current();
        }
        assertOk();
        return currLoc().rec();
    }
        
    BSONObj QueryOptimizerCursorImpl::current() {
        if ( _takeover ) {
            return _takeover->current();
        }
        assertOk();
        return currLoc().obj();
    }
        
    DiskLoc QueryOptimizerCursorImpl::currLoc() {
        return _takeover ? _takeover->currLoc() : _currLoc();
    }
        
    DiskLoc QueryOptimizerCursorImpl::_currLoc() const {
        dassert( !_takeover );
        return _currRunner ? _currRunner->currLoc() : DiskLoc();
    }
        
    bool QueryOptimizerCursorImpl::advance() {
        return _advance( false );
    }
        
    BSONObj QueryOptimizerCursorImpl::currKey() const {
        if ( _takeover ) {
            return _takeover->currKey();
        }
        assertOk();
        return _currRunner->currKey();
    }

    DiskLoc QueryOptimizerCursorImpl::refLoc() {
        return _takeover ? _takeover->refLoc() : DiskLoc();
    }

    BSONObj QueryOptimizerCursorImpl::indexKeyPattern() {
        if ( _takeover ) {
            return _takeover->indexKeyPattern();
        }
        assertOk();
        return _currRunner->cursor()->indexKeyPattern();
    }

    void QueryOptimizerCursorImpl::prepareToTouchEarlierIterate() {
        if ( _takeover ) {
            _takeover->prepareToTouchEarlierIterate();
        }
        else if ( _currRunner ) {
            if ( _mps->currentNPlans() == 1 ) {
                // This single plan version is a bit more performant, so we use it when possible.
                _currRunner->prepareToTouchEarlierIterate();
            }
            else {
                // With multiple plans, the 'earlier iterate' could be the current iterate of one of
                // the component plans.  We do a full yield of all plans, using ClientCursors.
                _mps->prepareToYield();
            }
        }
    }

    void QueryOptimizerCursorImpl::recoverFromTouchingEarlierIterate() {
        if ( _takeover ) {
            _takeover->recoverFromTouchingEarlierIterate();
        }
        else if ( _currRunner ) {
            if ( _mps->currentNPlans() == 1 ) {
                _currRunner->recoverFromTouchingEarlierIterate();
            }
            else {
                recoverFromYield();
            }
        }
    }

    void QueryOptimizerCursorImpl::prepareToYield() {
        if ( _takeover ) {
            _takeover->prepareToYield();
        }
        else if ( _currRunner ) {
            _mps->prepareToYield();
        }
    }
        
    void QueryOptimizerCursorImpl::recoverFromYield() {
        if ( _takeover ) {
            _takeover->recoverFromYield();
            return;
        }
        if ( _currRunner ) {
            _mps->recoverFromYield();
            if ( _currRunner->error() || !ok() ) {
                // Advance to a non error op if one of the ops errored out.
                // Advance to a following $or clause if the $or clause returned all results.
                verify( !_mps->doneRunners() );
                _advance( true );
            }
        }
    }

    bool QueryOptimizerCursorImpl::getsetdup(DiskLoc loc) {
        if ( _takeover ) {
            if ( getdupInternal( loc ) ) {
                return true;
            }
            return _takeover->getsetdup( loc );
        }
        assertOk();
        return getsetdupInternal( loc );
    }
        
    bool QueryOptimizerCursorImpl::isMultiKey() const {
        if ( _takeover ) {
            return _takeover->isMultiKey();
        }
        assertOk();
        return _currRunner->cursor()->isMultiKey();
    }
        
    bool QueryOptimizerCursorImpl::capped() const {
        // Initial capped wrapping cases (before takeover) are handled internally by a component
        // ClientCursor.
        return _takeover ? _takeover->capped() : false;
    }

    long long QueryOptimizerCursorImpl::nscanned() {
        return _takeover ? _takeover->nscanned() : _nscanned;
    }

    CoveredIndexMatcher* QueryOptimizerCursorImpl::matcher() const {
        if ( _takeover ) {
            return _takeover->matcher();
        }
        assertOk();
        return _currRunner->queryPlan().matcher().get();
    }

    bool QueryOptimizerCursorImpl::currentMatches( MatchDetails* details ) {
        if ( _takeover ) {
            return _takeover->currentMatches( details );
        }
        assertOk();
        return _currRunner->currentMatches( details );
    }
        
    const FieldRangeSet* QueryOptimizerCursorImpl::initialFieldRangeSet() const {
        if ( _takeover ) {
            return 0;
        }
        assertOk();
        return &_currRunner->queryPlan().multikeyFrs();
    }
        
    bool QueryOptimizerCursorImpl::currentPlanScanAndOrderRequired() const {
        if ( _takeover ) {
            return _takeover->queryPlan().scanAndOrderRequired();
        }
        assertOk();
        return _currRunner->queryPlan().scanAndOrderRequired();
    }
        
    const Projection::KeyOnly* QueryOptimizerCursorImpl::keyFieldsOnly() const {
        if ( _takeover ) {
            return _takeover->keyFieldsOnly();
        }
        assertOk();
        return _currRunner->keyFieldsOnly();
    }
        
    bool QueryOptimizerCursorImpl::runningInitialInOrderPlan() const {
        if ( _takeover ) {
            return false;
        }
        assertOk();
        return _mps->haveInOrderPlan();
    }

    bool QueryOptimizerCursorImpl::hasPossiblyExcludedPlans() const {
        if ( _takeover ) {
            return false;
        }
        assertOk();
        return _mps->hasPossiblyExcludedPlans();
    }

    void QueryOptimizerCursorImpl::clearIndexesForPatterns() {
        if ( !_takeover ) {
            _mps->clearIndexesForPatterns();
        }
    }
        
    void QueryOptimizerCursorImpl::abortOutOfOrderPlans() {
        _requireOrder = true;
    }

    void QueryOptimizerCursorImpl::noteIterate( bool match, bool loadedDocument, bool chunkSkip ) {
        if ( _explainQueryInfo ) {
            _explainQueryInfo->noteIterate( match, loadedDocument, chunkSkip );
        }
        if ( _takeover ) {
            _takeover->noteIterate( match, loadedDocument );
        }
    }
        
    void QueryOptimizerCursorImpl::noteYield() {
        if ( _explainQueryInfo ) {
            _explainQueryInfo->noteYield();
        }
    }
        
    QueryOptimizerCursorImpl::QueryOptimizerCursorImpl( auto_ptr<MultiPlanScanner>& mps,
                                                        const QueryPlanSelectionPolicy& planPolicy,
                                                        bool requireOrder ) :
        _requireOrder( requireOrder ),
        _mps( mps ),
        _initialCandidatePlans( _mps->possibleInOrderPlan(), _mps->possibleOutOfOrderPlan() ),
        _originalRunner( new QueryPlanRunner( _nscanned,
                                              planPolicy,
                                              _requireOrder,
                                              !_initialCandidatePlans.hybridPlanSet() ) ),
        _currRunner(),
        _completePlanOfHybridSetScanAndOrderRequired(),
        _nscanned() {
    }
        
    void QueryOptimizerCursorImpl::init( bool explain ) {
        _mps->initialRunner( _originalRunner );
        if ( explain ) {
            _explainQueryInfo = _mps->generateExplainInfo();
        }
        shared_ptr<QueryPlanRunner> runner = _mps->nextRunner();
        rethrowOnError( runner );
        if ( !runner->complete() ) {
            _currRunner = runner.get();
        }
    }

    bool QueryOptimizerCursorImpl::_advance( bool force ) {
        if ( _takeover ) {
            return _takeover->advance();
        }

        if ( !force && !ok() ) {
            return false;
        }

        _currRunner = 0;
        shared_ptr<QueryPlanRunner> runner = _mps->nextRunner();
        rethrowOnError( runner );

        if ( !runner->complete() ) {
            // The 'runner' will be valid until we call _mps->nextOp() again.  We return 'current'
            // values from this op.
            _currRunner = runner.get();
        }
        else if ( runner->stopRequested() ) {
            if ( runner->cursor() ) {
                _takeover.reset( new MultiCursor( _mps,
                                                  runner->cursor(),
                                                  runner->queryPlan().matcher(),
                                                  runner->explainInfo(),
                                                  *runner,
                                                  _nscanned - runner->cursor()->nscanned() ) );
            }
        }
        else {
            if ( _initialCandidatePlans.hybridPlanSet() ) {
                _completePlanOfHybridSetScanAndOrderRequired =
                        runner->queryPlan().scanAndOrderRequired();
            }
        }

        return ok();
    }
    
    /** Forward an exception when the runner errs out. */
    void QueryOptimizerCursorImpl::rethrowOnError( const shared_ptr< QueryPlanRunner > &runner ) {
        if ( runner->error() ) {
            throw MsgAssertionException( runner->exception() );   
        }
    }

    bool QueryOptimizerCursorImpl::getsetdupInternal(const DiskLoc &loc) {
        return _dups.getsetdup( loc );
    }

    bool QueryOptimizerCursorImpl::getdupInternal(const DiskLoc &loc) {
        dassert( _takeover );
        return _dups.getdup( loc );
    }
    
    shared_ptr<Cursor> newQueryOptimizerCursor( auto_ptr<MultiPlanScanner> mps,
                                               const QueryPlanSelectionPolicy &planPolicy,
                                               bool requireOrder, bool explain ) {
        try {
            shared_ptr<QueryOptimizerCursorImpl> ret
                    ( QueryOptimizerCursorImpl::make( mps, planPolicy, requireOrder, explain ) );
            return ret;
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
    
    CursorGenerator::CursorGenerator( const StringData &ns,
                                     const BSONObj &query,
                                     const BSONObj &order,
                                     const QueryPlanSelectionPolicy &planPolicy,
                                     const shared_ptr<const ParsedQuery> &parsedQuery,
                                     bool requireOrder,
                                     QueryPlanSummary *singlePlanSummary ) :
    _ns( ns ),
    _query( query ),
    _order( order ),
    _planPolicy( planPolicy ),
    _parsedQuery( parsedQuery ),
    _requireOrder( requireOrder ),
    _singlePlanSummary( singlePlanSummary ) {
        // Initialize optional return variables.
        if ( _singlePlanSummary ) {
            *_singlePlanSummary = QueryPlanSummary();
        }
    }
    
    BSONObj CursorGenerator::hint() const {
        return _argumentsHint.isEmpty() ? _planPolicy.planHint( _ns ) : _argumentsHint;
    }

    void CursorGenerator::setArgumentsHint() {
        if ( useHints && _parsedQuery ) {
            _argumentsHint = _parsedQuery->getHint();
        }
        
        if ( snapshot() ) {
            NamespaceDetails *d = nsdetails( _ns );
            if ( d ) {
                int i = d->findIdIndex();
                if( i < 0 ) {
                    if ( _ns.find( ".system." ) == string::npos )
                        log() << "warning: no _id index on $snapshot query, ns:" << _ns << endl;
                }
                else {
                    /* [dm] the name of an _id index tends to vary, so we build the hint the hard
                     way here. probably need a better way to specify "use the _id index" as a hint.
                     if someone is in the query optimizer please fix this then!
                     */
                    _argumentsHint = BSON( "$hint" << d->idx(i).indexName() );
                }
            }
        }
    }
    
    shared_ptr<Cursor> CursorGenerator::shortcutCursor() const {
        if ( !mayShortcutQueryOptimizer() ) {
            return shared_ptr<Cursor>();
        }
        
        if ( _planPolicy.permitOptimalNaturalPlan() && _query.isEmpty() && _order.isEmpty() ) {
            return theDataFileMgr.findAll( _ns );
        }
        if ( _planPolicy.permitOptimalIdPlan() && isSimpleIdQuery( _query ) ) {
            Database *database = cc().database();
            verify( database );
            NamespaceDetails *d = database->namespaceIndex().details( _ns );
            if ( d ) {
                int idxNo = d->findIdIndex();
                if ( idxNo >= 0 ) {
                    IndexDetails& i = d->idx( idxNo );
                    BSONObj key = i.getKeyFromQuery( _query );
                    return shared_ptr<Cursor>( BtreeCursor::make( d, i, key, key, true, 1 ) );
                }
            }
        }
        
        return shared_ptr<Cursor>();
    }
    
    void CursorGenerator::setMultiPlanScanner() {
        _mps.reset( MultiPlanScanner::make( _ns, _query, _order, _parsedQuery, hint(),
                                           explain() ? QueryPlanGenerator::Ignore :
                                                QueryPlanGenerator::Use,
                                           min(), max() ) );
    }
    
    shared_ptr<Cursor> CursorGenerator::singlePlanCursor() {
        const QueryPlan *singlePlan = _mps->singlePlan();
        if ( !singlePlan || ( isOrderRequired() && singlePlan->scanAndOrderRequired() ) ) {
            return shared_ptr<Cursor>();
        }
        if ( !_planPolicy.permitPlan( *singlePlan ) ) {
            return shared_ptr<Cursor>();
        }
        
        if ( _singlePlanSummary ) {
            *_singlePlanSummary = singlePlan->summary();
        }
        shared_ptr<Cursor> single = singlePlan->newCursor( DiskLoc(),
                                                           _planPolicy.requestIntervalCursor() );
        if ( !_query.isEmpty() && !single->matcher() ) {

            // The query plan must have a matcher.  The matcher's constructor performs some aspects
            // of query validation that should occur before a cursor is returned.
            fassert( 16449, singlePlan->matcher() );

            if ( // If a matcher is requested or ...
                 _planPolicy.requestMatcher() ||
                 // ... the index ranges do not exactly match the query or ...
                 singlePlan->mayBeMatcherNecessary() ||
                 // ... the matcher must look at the full record ...
                 singlePlan->matcher()->needRecord() ) {

                // ... then set the cursor's matcher to the query plan's matcher.
                single->setMatcher( singlePlan->matcher() );
            }
        }
        if ( singlePlan->keyFieldsOnly() ) {
            single->setKeyFieldsOnly( singlePlan->keyFieldsOnly() );
        }
        return single;
    }
    
    shared_ptr<Cursor> CursorGenerator::generate() {

        setArgumentsHint();
        shared_ptr<Cursor> cursor = shortcutCursor();
        if ( cursor ) {
            return cursor;
        }
        
        setMultiPlanScanner();
        cursor = singlePlanCursor();
        if ( cursor ) {
            return cursor;
        }
        
        return newQueryOptimizerCursor( _mps, _planPolicy, isOrderRequired(), explain() );
    }

    /** This interface is just available for testing. */
    shared_ptr<Cursor> newQueryOptimizerCursor
    ( const char *ns, const BSONObj &query, const BSONObj &order,
     const QueryPlanSelectionPolicy &planPolicy, bool requireOrder,
     const shared_ptr<const ParsedQuery> &parsedQuery ) {
        auto_ptr<MultiPlanScanner> mps( MultiPlanScanner::make( ns, query, order, parsedQuery ) );
        return newQueryOptimizerCursor( mps, planPolicy, requireOrder, false );
    }
        
} // namespace mongo;
