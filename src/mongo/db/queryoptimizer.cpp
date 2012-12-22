// @file queryoptimizer.cpp

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
*/

#include "mongo/pch.h"

#include "mongo/db/queryoptimizer.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/btreecursor.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/db.h"
#include "mongo/db/intervalbtreecursor.h"
#include "mongo/db/pagefault.h"
#include "mongo/server.h"

//#define DEBUGQO(x) cout << x << endl;
#define DEBUGQO(x)

namespace mongo {

    QueryPlanSummary QueryPlan::summary() const { return QueryPlanSummary( *this ); }

    double elementDirection( const BSONElement &e ) {
        if ( e.isNumber() )
            return e.number();
        return 1;
    }

    // returns an IndexDetails * for a hint, 0 if hint is $natural.
    // hint must not be eoo()
    IndexDetails *parseHint( const BSONElement &hint, NamespaceDetails *d ) {
        massert( 13292, "hint eoo", !hint.eoo() );
        if( hint.type() == String ) {
            string hintstr = hint.valuestr();
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                IndexDetails& ii = i.next();
                if ( ii.indexName() == hintstr ) {
                    return &ii;
                }
            }
        }
        else if( hint.type() == Object ) {
            BSONObj hintobj = hint.embeddedObject();
            uassert( 10112 ,  "bad hint", !hintobj.isEmpty() );
            if ( !strcmp( hintobj.firstElementFieldName(), "$natural" ) ) {
                return 0;
            }
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                IndexDetails& ii = i.next();
                if( ii.keyPattern().woCompare(hintobj) == 0 ) {
                    return &ii;
                }
            }
        }
        uassert( 10113 ,  "bad hint", false );
        return 0;        
    }

    QueryPlan *QueryPlan::make( NamespaceDetails *d,
                               int idxNo,
                               const FieldRangeSetPair &frsp,
                               const FieldRangeSetPair *originalFrsp,
                               const BSONObj &originalQuery,
                               const BSONObj &order,
                               const shared_ptr<const ParsedQuery> &parsedQuery,
                               const BSONObj &startKey,
                               const BSONObj &endKey,
                               const std::string& special ) {
        auto_ptr<QueryPlan> ret( new QueryPlan( d, idxNo, frsp, originalQuery, order, parsedQuery,
                                               special ) );
        ret->init( originalFrsp, startKey, endKey );
        return ret.release();
    }
    
    QueryPlan::QueryPlan( NamespaceDetails *d,
                         int idxNo,
                         const FieldRangeSetPair &frsp,
                         const BSONObj &originalQuery,
                         const BSONObj &order,
                         const shared_ptr<const ParsedQuery> &parsedQuery,
                         const std::string& special ) :
        _d(d),
        _idxNo(idxNo),
        _frs( frsp.frsForIndex( _d, _idxNo ) ),
        _frsMulti( frsp.frsForIndex( _d, -1 ) ),
        _originalQuery( originalQuery ),
        _order( order ),
        _parsedQuery( parsedQuery ),
        _index( 0 ),
        _scanAndOrderRequired( true ),
        _matcherNecessary( true ),
        _direction( 0 ),
        _endKeyInclusive(),
        _utility( Helpful ),
        _special( special ),
        _type(0),
        _startOrEndSpec() {
    }
    
    void QueryPlan::init( const FieldRangeSetPair *originalFrsp,
                         const BSONObj &startKey,
                         const BSONObj &endKey ) {
        _endKeyInclusive = endKey.isEmpty();
        _startOrEndSpec = !startKey.isEmpty() || !endKey.isEmpty();
        
        BSONObj idxKey = _idxNo < 0 ? BSONObj() : _d->idx( _idxNo ).keyPattern();

        if ( !_frs.matchPossibleForIndex( idxKey ) ) {
            _utility = Impossible;
            _scanAndOrderRequired = false;
            return;
        }
            
        if ( willScanTable() ) {
            if ( _order.isEmpty() || !strcmp( _order.firstElementFieldName(), "$natural" ) )
                _scanAndOrderRequired = false;
            return;
        }

        _index = &_d->idx(_idxNo);

        // If the parsing or index indicates this is a special query, don't continue the processing
        if (!_special.empty() ||
            ( _index->getSpec().getType() &&
             _index->getSpec().getType()->suitability( _frs, _order ) != USELESS ) ) {

            _type  = _index->getSpec().getType();
            if (_special.empty()) _special = _index->getSpec().getType()->getPlugin()->getName();

            massert( 13040 , (string)"no type for special: " + _special , _type );
            // hopefully safe to use original query in these contexts;
            // don't think we can mix special with $or clause separation yet
            _scanAndOrderRequired = _type->scanAndOrderRequired( _originalQuery , _order );
            return;
        }

        const IndexSpec &idxSpec = _index->getSpec();
        BSONObjIterator o( _order );
        BSONObjIterator k( idxKey );
        if ( !o.moreWithEOO() )
            _scanAndOrderRequired = false;
        while( o.moreWithEOO() ) {
            BSONElement oe = o.next();
            if ( oe.eoo() ) {
                _scanAndOrderRequired = false;
                break;
            }
            if ( !k.moreWithEOO() )
                break;
            BSONElement ke;
            while( 1 ) {
                ke = k.next();
                if ( ke.eoo() )
                    goto doneCheckOrder;
                if ( strcmp( oe.fieldName(), ke.fieldName() ) == 0 )
                    break;
                if ( !_frs.range( ke.fieldName() ).equality() )
                    goto doneCheckOrder;
            }
            int d = elementDirection( oe ) == elementDirection( ke ) ? 1 : -1;
            if ( _direction == 0 )
                _direction = d;
            else if ( _direction != d )
                break;
        }
doneCheckOrder:
        if ( _scanAndOrderRequired )
            _direction = 0;
        BSONObjIterator i( idxKey );
        int exactIndexedQueryCount = 0;
        int optimalIndexedQueryCount = 0;
        bool awaitingLastOptimalField = true;
        set<string> orderFieldsUnindexed;
        _order.getFieldNames( orderFieldsUnindexed );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const FieldRange &fr = _frs.range( e.fieldName() );
            if ( awaitingLastOptimalField ) {
                if ( !fr.universal() )
                    ++optimalIndexedQueryCount;
                if ( !fr.equality() )
                    awaitingLastOptimalField = false;
            }
            else {
                if ( !fr.universal() )
                    optimalIndexedQueryCount = -1;
            }
            if ( fr.equality() ) {
                BSONElement e = fr.max();
                if ( !e.isNumber() && !e.mayEncapsulate() && e.type() != RegEx )
                    ++exactIndexedQueryCount;
            }
            orderFieldsUnindexed.erase( e.fieldName() );
        }
        if ( !_scanAndOrderRequired &&
                ( optimalIndexedQueryCount == _frs.numNonUniversalRanges() ) )
            _utility = Optimal;
        _frv.reset( new FieldRangeVector( _frs, idxSpec, _direction ) );

        if ( // If all field range constraints are on indexed fields and ...
             _utility == Optimal &&
             // ... the field ranges exactly represent the query and ...
             _frs.mustBeExactMatchRepresentation() &&
             // ... all indexed ranges are represented in the field range vector ...
             _frv->hasAllIndexedRanges() ) {

            // ... then the field range vector is sufficient to perform query matching against index
            // keys.  No matcher is required.
            _matcherNecessary = false;
        }

        if ( originalFrsp ) {
            _originalFrv.reset( new FieldRangeVector( originalFrsp->frsForIndex( _d, _idxNo ),
                                                     idxSpec, _direction ) );
        }
        else {
            _originalFrv = _frv;
        }
        if ( _startOrEndSpec ) {
            BSONObj newStart, newEnd;
            if ( !startKey.isEmpty() )
                _startKey = startKey;
            else
                _startKey = _frv->startKey();
            if ( !endKey.isEmpty() )
                _endKey = endKey;
            else
                _endKey = _frv->endKey();
        }

        if ( ( _scanAndOrderRequired || _order.isEmpty() ) && 
            _frs.range( idxKey.firstElementFieldName() ).universal() ) { // NOTE SERVER-2140
            _utility = Unhelpful;
        }
            
        if ( idxSpec.isSparse() && hasPossibleExistsFalsePredicate() ) {
            _utility = Disallowed;
        }

        if ( _parsedQuery && _parsedQuery->getFields() && !_d->isMultikey( _idxNo ) ) { // Does not check modifiedKeys()
            _keyFieldsOnly.reset( _parsedQuery->getFields()->checkKey( _index->keyPattern() ) );
        }
    }

    shared_ptr<Cursor> QueryPlan::newCursor( const DiskLoc& startLoc,
                                             bool requestIntervalCursor ) const {

        if ( _type ) {
            // hopefully safe to use original query in these contexts - don't think we can mix type with $or clause separation yet
            int numWanted = 0;
            if ( _parsedQuery ) {
                // SERVER-5390
                numWanted = _parsedQuery->getSkip() + _parsedQuery->getNumToReturn();
            }
            return _type->newCursor( _originalQuery , _order , numWanted );
        }

        if ( _utility == Impossible ) {
            // Dummy table scan cursor returning no results.  Allowed in --notablescan mode.
            return shared_ptr<Cursor>( new BasicCursor( DiskLoc() ) );
        }

        if ( willScanTable() ) {
            checkTableScanAllowed();
            return findTableScan( _frs.ns(), _order, startLoc );
        }
                
        massert( 10363 ,  "newCursor() with start location not implemented for indexed plans", startLoc.isNull() );

        if ( _startOrEndSpec ) {
            // we are sure to spec _endKeyInclusive
            return shared_ptr<Cursor>( BtreeCursor::make( _d,
                                                          *_index,
                                                          _startKey,
                                                          _endKey,
                                                          _endKeyInclusive,
                                                          _direction >= 0 ? 1 : -1 ) );
        }

        if ( _index->getSpec().getType() ) {
            return shared_ptr<Cursor>( BtreeCursor::make( _d,
                                                          *_index,
                                                          _frv->startKey(),
                                                          _frv->endKey(),
                                                          true,
                                                          _direction >= 0 ? 1 : -1 ) );
        }

        // An IntervalBtreeCursor is returned if explicitly requested AND _frv is exactly
        // represented by a single interval within the btree.
        if ( // If an interval cursor is requested and ...
             requestIntervalCursor &&
             // ... equalities come before ranges (a requirement of Optimal) and ...
             _utility == Optimal &&
             // ... the field range vector exactly represents a single interval ...
             _frv->isSingleInterval() ) {
            // ... and an interval cursor can be created ...
            shared_ptr<Cursor> ret( IntervalBtreeCursor::make( _d,
                                                               *_index,
                                                               _frv->startKey(),
                                                               _frv->startKeyInclusive(),
                                                               _frv->endKey(),
                                                               _frv->endKeyInclusive() ) );
            if ( ret ) {
                // ... then return the interval cursor.
                return ret;
            }
        }

        return shared_ptr<Cursor>( BtreeCursor::make( _d,
                                                      *_index,
                                                      _frv,
                                                      independentRangesSingleIntervalLimit(),
                                                      _direction >= 0 ? 1 : -1 ) );
    }

    shared_ptr<Cursor> QueryPlan::newReverseCursor() const {
        if ( willScanTable() ) {
            int orderSpec = _order.getIntField( "$natural" );
            if ( orderSpec == INT_MIN )
                orderSpec = 1;
            return findTableScan( _frs.ns(), BSON( "$natural" << -orderSpec ) );
        }
        massert( 10364 ,  "newReverseCursor() not implemented for indexed plans", false );
        return shared_ptr<Cursor>();
    }

    BSONObj QueryPlan::indexKey() const {
        if ( !_index )
            return BSON( "$natural" << 1 );
        return _index->keyPattern();
    }

    void QueryPlan::registerSelf( long long nScanned,
                                 CandidatePlanCharacter candidatePlans ) const {
        // Impossible query constraints can be detected before scanning and historically could not
        // generate a QueryPattern.
        if ( _utility == Impossible ) {
            return;
        }

        SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
        QueryPattern queryPattern = _frs.pattern( _order );
        CachedQueryPlan queryPlanToCache( indexKey(), nScanned, candidatePlans );
        NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get_inlock( ns() );
        nsdt.registerCachedQueryPlanForPattern( queryPattern, queryPlanToCache );
    }
    
    void QueryPlan::checkTableScanAllowed() const {
        if ( likely( !cmdLine.noTableScan ) )
            return;

        // TODO - is this desirable?  See SERVER-2222.
        if ( _frs.numNonUniversalRanges() == 0 )
            return;

        if ( strstr( ns() , ".system." ) ) 
            return;

        if( str::startsWith(ns(), "local.") )
            return;

        if ( ! nsdetails( ns() ) )
            return;

        uassert( 10111 ,  (string)"table scans not allowed:" + ns() , ! cmdLine.noTableScan );
    }

    int QueryPlan::independentRangesSingleIntervalLimit() const {
        if ( _scanAndOrderRequired &&
            _parsedQuery &&
            !_parsedQuery->wantMore() &&
            !isMultiKey() &&
            queryBoundsExactOrderSuffix() ) {
            verify( _direction == 0 );
            // Limit the results for each compound interval. SERVER-5063
            return _parsedQuery->getSkip() + _parsedQuery->getNumToReturn();
        }
        return 0;
    }

    /**
     * Detects $exists:false predicates in a matcher.  All $exists:false predicates will be
     * detected.  Some $exists:true predicates may be incorrectly reported as $exists:false due to
     * the approximate nature of the implementation.
     */
    class ExistsFalseDetector : public MatcherVisitor {
    public:
        ExistsFalseDetector( const Matcher& originalMatcher );
        bool hasFoundExistsFalse() const { return _foundExistsFalse; }
        void visitMatcher( const Matcher& matcher ) { _currentMatcher = &matcher; }
        void visitElementMatcher( const ElementMatcher& elementMatcher );
    private:
        const Matcher* _originalMatcher;
        const Matcher* _currentMatcher;
        bool _foundExistsFalse;
    };

    ExistsFalseDetector::ExistsFalseDetector( const Matcher& originalMatcher ) :
        _originalMatcher( &originalMatcher ),
        _currentMatcher( 0 ),
        _foundExistsFalse() {
    }

    /** Matches $exists:false and $not:{$exists:true} exactly. */
    static bool isExistsFalsePredicate( const ElementMatcher& elementMatcher ) {
        bool hasTrueValue = elementMatcher._toMatch.trueValue();
        bool hasNotModifier = elementMatcher._isNot;
        return hasNotModifier ? hasTrueValue : !hasTrueValue;
    }
    
    void ExistsFalseDetector::visitElementMatcher( const ElementMatcher& elementMatcher ) {
        if ( elementMatcher._compareOp != BSONObj::opEXISTS ) {
            // Only consider $exists predicates.
            return;
        }
        if ( _currentMatcher != _originalMatcher ) {
            // Treat all $exists predicates nested below the original matcher as $exists:false.
            // This approximation is used because a nesting operator may change the matching
            // semantics of $exists:true.
            _foundExistsFalse = true;
            return;
        }
        if ( isExistsFalsePredicate( elementMatcher ) ) {
            // Top level $exists operators are matched exactly.
            _foundExistsFalse = true;
        }
    }

    bool QueryPlan::hasPossibleExistsFalsePredicate() const {
        ExistsFalseDetector detector( matcher()->docMatcher() );
        matcher()->docMatcher().visit( detector );
        return detector.hasFoundExistsFalse();
    }
    
    bool QueryPlan::queryBoundsExactOrderSuffix() const {
        if ( !indexed() ||
             !_frs.matchPossible() ||
             !_frs.mustBeExactMatchRepresentation() ) {
            return false;
        }
        BSONObj idxKey = indexKey();
        BSONObjIterator index( idxKey );
        BSONObjIterator order( _order );
        int coveredNonUniversalRanges = 0;
        while( index.more() ) {
            const FieldRange& indexFieldRange = _frs.range( (*index).fieldName() );
            if ( !indexFieldRange.isPointIntervalSet() ) {
                if ( !indexFieldRange.universal() ) {
                    // The last indexed range may be a non point set containing a single interval.
                    // SERVER-5777
                    if ( indexFieldRange.intervals().size() > 1 ) {
                        return false;
                    }
                    ++coveredNonUniversalRanges;
                }
                break;
            }
            ++coveredNonUniversalRanges;
            if ( order.more() && str::equals( (*index).fieldName(), (*order).fieldName() ) ) {
                ++order;
            }
            ++index;
        }
        if ( coveredNonUniversalRanges != _frs.numNonUniversalRanges() ) {
            return false;
        }
        while( index.more() && order.more() ) {
            if ( !str::equals( (*index).fieldName(), (*order).fieldName() ) ) {
                return false;
            }
            if ( ( elementDirection( *index ) < 0 ) != ( elementDirection( *order ) < 0 ) ) {
                return false;
            }
            ++order;
            ++index;
        }
        return !order.more();
    }

    string QueryPlan::toString() const {
        return BSON(
                    "index" << indexKey() <<
                    "frv" << ( _frv ? _frv->toString() : "" ) <<
                    "order" << _order
                    ).jsonString();
    }
    
    shared_ptr<CoveredIndexMatcher> QueryPlan::matcher() const {
        if ( !_matcher ) {
            _matcher.reset( new CoveredIndexMatcher( originalQuery(), indexKey() ) );
        }
        return _matcher;
    }
    
    bool QueryPlan::isMultiKey() const {
        if ( _idxNo < 0 )
            return false;
        return _d->isMultikey( _idxNo );
    }

    std::ostream &operator<< ( std::ostream &out, const QueryPlan::Utility &utility ) {
        out << "QueryPlan::";
        switch( utility ) {
            case QueryPlan::Impossible: return out << "Impossible";
            case QueryPlan::Optimal:    return out << "Optimal";
            case QueryPlan::Helpful:    return out << "Helpful";
            case QueryPlan::Unhelpful:  return out << "Unhelpful";
            case QueryPlan::Disallowed: return out << "Disallowed";
            default:
                return out << "UNKNOWN(" << utility << ")";
        }
    }

    CachedMatchCounter::CachedMatchCounter( long long& aggregateNscanned,
                                            int cumulativeCount ) :
        _aggregateNscanned( aggregateNscanned ),
        _nscanned(),
        _cumulativeCount( cumulativeCount ),
        _count(),
        _checkDups(),
        _match( Unknown ),
        _counted() {
    }

    void CachedMatchCounter::resetMatch() {
        _match = Unknown;
        _counted = false;
    }

    bool CachedMatchCounter::setMatch( bool match ) {
        MatchState oldMatch = _match;
        _match = match ? True : False;
        return _match == True && oldMatch != True;
    }

    void CachedMatchCounter::incMatch( const DiskLoc& loc ) {
        if ( !_counted && _match == True && !getsetdup( loc ) ) {
            ++_cumulativeCount;
            ++_count;
            _counted = true;
        }
    }

    bool CachedMatchCounter::wouldIncMatch( const DiskLoc& loc ) const {
        return !_counted && _match == True && !getdup( loc );
    }
    
    bool CachedMatchCounter::enoughCumulativeMatchesToChooseAPlan() const {
        // This is equivalent to the default condition for switching from
        // a query to a getMore, which was the historical default match count for
        // choosing a plan.
        return _cumulativeCount >= 101;
    }

    bool CachedMatchCounter::enoughMatchesToRecordPlan() const {
        // Recording after 50 matches is a historical default (101 default limit / 2).
        return _count > 50;
    }

    void CachedMatchCounter::updateNscanned( long long nscanned ) {
        _aggregateNscanned += ( nscanned - _nscanned );
        _nscanned = nscanned;
    }

    bool CachedMatchCounter::getsetdup( const DiskLoc& loc ) {
        if ( !_checkDups ) {
            return false;
        }
        pair<set<DiskLoc>::iterator, bool> p = _dups.insert( loc );
        return !p.second;
    }

    bool CachedMatchCounter::getdup( const DiskLoc& loc ) const {
        if ( !_checkDups ) {
            return false;
        }
        return _dups.find( loc ) != _dups.end();
    }

    QueryPlanRunner::QueryPlanRunner( long long& aggregateNscanned,
                                      const QueryPlanSelectionPolicy& selectionPolicy,
                                      const bool& requireOrder,
                                      bool alwaysCountMatches,
                                      int cumulativeCount ) :
        _complete(),
        _stopRequested(),
        _queryPlan(),
        _error(),
        _matchCounter( aggregateNscanned, cumulativeCount ),
        _countingMatches(),
        _mustAdvance(),
        _capped(),
        _selectionPolicy( selectionPolicy ),
        _requireOrder( requireOrder ),
        _alwaysCountMatches( alwaysCountMatches ) {
    }

    void QueryPlanRunner::next() {
        checkCursorOrdering();
        
        mayAdvance();
        
        if ( countMatches() && _matchCounter.enoughCumulativeMatchesToChooseAPlan() ) {
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

    long long QueryPlanRunner::nscanned() const {
        return _c ? _c->nscanned() : _matchCounter.nscanned();
    }

    void QueryPlanRunner::prepareToYield() {
        if ( _c && !_cc ) {
            _cc.reset( new ClientCursor( QueryOption_NoCursorTimeout, _c, queryPlan().ns() ) );
            // Set 'doing deletes' as deletes may occur; if there are no deletes this has no
            // effect.
            _cc->setDoingDeletes( true );
        }
        if ( _cc ) {
            recordCursorLocation();
            _cc->prepareToYield( _yieldData );
        }
    }
    
    void QueryPlanRunner::recoverFromYield() {
        if ( _cc && !ClientCursor::recoverFromYield( _yieldData ) ) {
            // !!! The collection may be gone, and any namespace or index specific memory may
            // have become invalid.
            _c.reset();
            _cc.reset();
            
            if ( _capped ) {
                msgassertedNoTrace( 13338,
                                    str::stream() << "capped cursor overrun: "
                                    << queryPlan().ns() );
            }
            msgassertedNoTrace( 15892,
                                str::stream() <<
                                "QueryPlanRunner::recoverFromYield() failed to recover" );
        }
        else {
            checkCursorAdvanced();
        }
    }
    
    void QueryPlanRunner::prepareToTouchEarlierIterate() {
        recordCursorLocation();
        if ( _c ) {
            _c->prepareToTouchEarlierIterate();
        }
    }

    void QueryPlanRunner::recoverFromTouchingEarlierIterate() {
        if ( _c ) {
            _c->recoverFromTouchingEarlierIterate();
        }
        checkCursorAdvanced();
    }
    
    bool QueryPlanRunner::currentMatches( MatchDetails* details ) {
        if ( !_c || !_c->ok() ) {
            _matchCounter.setMatch( false );
            return false;
        }
        
        MatchDetails myDetails;
        if ( !details && _explainPlanInfo ) {
            details = &myDetails;
        }
        
        bool match = queryPlan().matcher()->matchesCurrent( _c.get(), details );
        // Cache the match, so we can count it in mayAdvance().
        bool newMatch = _matchCounter.setMatch( match );
        
        if ( _explainPlanInfo ) {
            // Note iterate results as if this is the only query plan running.  But do not account
            // for query parameters that may be appled to the whole result set (results from
            // interleaved plans), for example the 'skip' parameter.
            bool countableMatch = newMatch && _matchCounter.wouldIncMatch( _c->currLoc() );
            bool matchWouldBeLoadedForReturn = countableMatch && hasDocumentLoadingQueryPlan();
            _explainPlanInfo->noteIterate( countableMatch,
                                           details->hasLoadedRecord() ||
                                           matchWouldBeLoadedForReturn,
                                           *_c );
        }
        
        return match;
    }

    bool QueryPlanRunner::mayRecordPlan() const {
        return complete() && ( !stopRequested() || _matchCounter.enoughMatchesToRecordPlan() );
    }

    QueryPlanRunner* QueryPlanRunner::createChild() const {
        return new QueryPlanRunner( _matchCounter.aggregateNscanned(),
                                    _selectionPolicy,
                                    _requireOrder,
                                    _alwaysCountMatches,
                                    _matchCounter.cumulativeCount() );
    }
    
    void QueryPlanRunner::setQueryPlan( const QueryPlan* queryPlan ) {
        _queryPlan = queryPlan;
        verify( _queryPlan != NULL );
    }
    
    void QueryPlanRunner::init() {
        checkCursorOrdering();
        if ( !_selectionPolicy.permitPlan( queryPlan() ) ) {
            throw MsgAssertionException( 9011,
                                         str::stream()
                                         << "Plan not permitted by query plan selection policy '"
                                         << _selectionPolicy.name()
                                         << "'" );
        }
        
        _c = queryPlan().newCursor();
        // The basic and btree cursors used by this implementation do not supply their own
        // matchers, and a matcher from a query plan will be used instead.
        verify( !_c->matcher() );
        // Such cursors all support deduplication.
        verify( _c->autoDedup() );
        
        // The query plan must have a matcher.  The matcher's constructor performs some aspects
        // of query validation that should occur as part of this class's init() if not handled
        // already.
        fassert( 16249, queryPlan().matcher() );
        
        // All candidate cursors must support yields for QueryOptimizerCursorImpl's
        // prepareToYield() and prepareToTouchEarlierIterate() to work.
        verify( _c->supportYields() );
        _capped = _c->capped();
        
        // TODO This violates the current Cursor interface abstraction, but for now it's simpler to keep our own set of
        // dups rather than avoid poisoning the cursor's dup set with unreturned documents.  Deduping documents
        // matched in this QueryOptimizerCursorOp will run against the takeover cursor.
        _matchCounter.setCheckDups( countMatches() && _c->isMultiKey() );
        // TODO ok if cursor becomes multikey later?
        
        _matchCounter.updateNscanned( _c->nscanned() );
    }

    void QueryPlanRunner::setException( const DBException &e ) {
        _error = true;
        _exception = e.getInfo();
    }

    shared_ptr<ExplainPlanInfo> QueryPlanRunner::generateExplainInfo() {
        if ( !_c ) {
            return shared_ptr<ExplainPlanInfo>( new ExplainPlanInfo() );
        }
        _explainPlanInfo.reset( new ExplainPlanInfo() );
        _explainPlanInfo->notePlan( *_c, queryPlan().scanAndOrderRequired(),
                                    queryPlan().keyFieldsOnly() );
        return _explainPlanInfo;
    }

    void QueryPlanRunner::mayAdvance() {
        if ( !_c ) {
            return;
        }
        if ( countingMatches() ) {
            // Check match if not yet known.
            if ( !_matchCounter.knowMatch() ) {
                currentMatches( 0 );
            }
            _matchCounter.incMatch( currLoc() );
        }
        if ( _mustAdvance ) {
            _c->advance();
            handleCursorAdvanced();
        }
        _matchCounter.updateNscanned( _c->nscanned() );
    }

    bool QueryPlanRunner::countingMatches() {
        if ( _countingMatches ) {
            return true;
        }
        if ( countMatches() ) {
            // Only count matches after the first call to next(), which occurs before the first
            // result is returned.
            _countingMatches = true;
        }
        return false;
    }

    bool QueryPlanRunner::countMatches() const {
        return _alwaysCountMatches || !queryPlan().scanAndOrderRequired();
    }

    bool QueryPlanRunner::hasDocumentLoadingQueryPlan() const {
        if ( queryPlan().parsedQuery() && queryPlan().parsedQuery()->returnKey() ) {
            // Index keys will be returned using $returnKey.
            return false;
        }
        if ( queryPlan().scanAndOrderRequired() ) {
            // The in memory sort implementation operates on full documents.
            return true;
        }
        if ( keyFieldsOnly() ) {
            // A covered index projection will be used.
            return false;
        }
        // Documents will be loaded for a standard query.
        return true;
    }

    void QueryPlanRunner::recordCursorLocation() {
        _posBeforeYield = currLoc();
    }

    void QueryPlanRunner::checkCursorAdvanced() {
        // This check will not correctly determine if we are looking at a different document in
        // all cases, but it is adequate for updating the query plan's match count (just used to pick
        // plans, not returned to the client) and adjust iteration via _mustAdvance.
        if ( _posBeforeYield != currLoc() ) {
            // If the yield advanced our position, the next next() will be a no op.
            handleCursorAdvanced();
        }
    }

    void QueryPlanRunner::handleCursorAdvanced() {
        _mustAdvance = false;
        _matchCounter.resetMatch();
    }

    void QueryPlanRunner::checkCursorOrdering() {
        if ( _requireOrder && queryPlan().scanAndOrderRequired() ) {
            throw MsgAssertionException( OutOfOrderDocumentsAssertionCode, "order spec cannot be satisfied with index" );
        }
    }

    QueryPlanGenerator::QueryPlanGenerator( QueryPlanSet &qps,
                                           auto_ptr<FieldRangeSetPair> originalFrsp,
                                           const shared_ptr<const ParsedQuery> &parsedQuery,
                                           const BSONObj &hint,
                                           RecordedPlanPolicy recordedPlanPolicy,
                                           const BSONObj &min,
                                           const BSONObj &max,
                                           bool allowSpecial ) :
        _qps( qps ),
        _originalFrsp( originalFrsp ),
        _parsedQuery( parsedQuery ),
        _hint( hint.getOwned() ),
        _recordedPlanPolicy( recordedPlanPolicy ),
        _min( min.getOwned() ),
        _max( max.getOwned() ),
        _allowSpecial( allowSpecial ) {
    }

    void QueryPlanGenerator::addInitialPlans() {
        const char *ns = _qps.frsp().ns();
        NamespaceDetails *d = nsdetails( ns );
        
        if ( addShortCircuitPlan( d ) ) {
            return;
        }
        
        addStandardPlans( d );
        warnOnCappedIdTableScan();
    }
    
    void QueryPlanGenerator::addFallbackPlans() {
        const char *ns = _qps.frsp().ns();
        NamespaceDetails *d = nsdetails( ns );
        verify( d );
        
        vector<shared_ptr<QueryPlan> > plans;
        shared_ptr<QueryPlan> optimalPlan;
        shared_ptr<QueryPlan> specialPlan;
        for( int i = 0; i < d->nIndexes; ++i ) {
            
            if ( !QueryUtilIndexed::indexUseful( _qps.frsp(), d, i, _qps.order() ) ) {
                continue;
            }
            
            shared_ptr<QueryPlan> p = newPlan( d, i );
            switch( p->utility() ) {
                case QueryPlan::Impossible:
                    _qps.setSinglePlan( p );
                    return;
                case QueryPlan::Optimal:
                    if ( !optimalPlan ) {
                        optimalPlan = p;
                    }
                    break;
                case QueryPlan::Helpful:
                    if ( p->special().empty() ) {
                        // Not a 'special' plan.
                        plans.push_back( p );
                    }
                    else if ( _allowSpecial ) {
                        specialPlan = p;
                    }
                    break;
                default:
                    break;
            }
        }

        if ( optimalPlan ) {
            _qps.setSinglePlan( optimalPlan );
            // Record an optimal plan in the query cache immediately, with a small nscanned value
            // that will be ignored.
            optimalPlan->registerSelf
                    ( 0, CandidatePlanCharacter( !optimalPlan->scanAndOrderRequired(),
                                                optimalPlan->scanAndOrderRequired() ) );
            return;
        }
        
        // Only add a special plan if no standard btree plans have been added. SERVER-4531
        if ( plans.empty() && specialPlan ) {
            _qps.setSinglePlan( specialPlan );
            return;
        }

        for( vector<shared_ptr<QueryPlan> >::const_iterator i = plans.begin(); i != plans.end();
            ++i ) {
            _qps.addCandidatePlan( *i );
        }        
        
        _qps.addCandidatePlan( newPlan( d, -1 ) );
    }
    
    bool QueryPlanGenerator::addShortCircuitPlan( NamespaceDetails *d ) {
        return
            // The collection is missing.
            setUnindexedPlanIf( !d, d ) ||
            // No match is possible.
            setUnindexedPlanIf( !_qps.frsp().matchPossible(), d ) ||
            // The hint, min, or max parameters are specified.
            addHintPlan( d ) ||
            // A special index operation is requested.
            addSpecialPlan( d ) ||
            // No indexable ranges or ordering are specified.
            setUnindexedPlanIf( _qps.frsp().noNonUniversalRanges() && _qps.order().isEmpty(), d ) ||
            // $natural sort is requested.
            setUnindexedPlanIf( !_qps.order().isEmpty() &&
                               str::equals( _qps.order().firstElementFieldName(), "$natural" ), d );
    }
    
    bool QueryPlanGenerator::addHintPlan( NamespaceDetails *d ) {
        BSONElement hint = _hint.firstElement();
        if ( !hint.eoo() ) {
            IndexDetails *id = parseHint( hint, d );
            if ( id ) {
                setHintedPlanForIndex( *id );
            }
            else {
                uassert( 10366, "natural order cannot be specified with $min/$max",
                        _min.isEmpty() && _max.isEmpty() );
                setSingleUnindexedPlan( d );
            }
            return true;
        }

        if ( !_min.isEmpty() || !_max.isEmpty() ) {
            string errmsg;
            BSONObj keyPattern;
            IndexDetails *idx = indexDetailsForRange( _qps.frsp().ns(), errmsg, _min, _max,
                                                     keyPattern );
            uassert( 10367 ,  errmsg, idx );
            validateAndSetHintedPlan( newPlan( d, d->idxNo( *idx ), _min, _max ) );
            return true;
        }
        
        return false;
    }
    
    bool QueryPlanGenerator::addSpecialPlan( NamespaceDetails *d ) {
        DEBUGQO( "\t special : " << _qps.frsp().getSpecial().toString() );
        SpecialIndices special = _qps.frsp().getSpecial();
        if (!special.empty()) {
            // Try to handle the special part of the query with an index
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                int j = i.pos();
                IndexDetails& ii = i.next();
                const IndexSpec& spec = ii.getSpec();
                // TODO(hk): Make sure we can do a $near and $within query, one using
                // the index one using the matcher.
                if (special.has(spec.getTypeName()) &&
                    spec.suitability( _qps.frsp().frsForIndex(d, j), _qps.order() ) != USELESS ) {
                    uassert( 16330, "'special' query operator not allowed", _allowSpecial );
                    _qps.setSinglePlan( newPlan( d, j, BSONObj(), BSONObj(), spec.getTypeName()));
                    return true;
                }
            }
            if (special.anyRequireIndex()) {
                uassert(13038, "can't find any special indices: " + special.toString()
                               + " for: " + _qps.originalQuery().toString(), false );
            }
        }
        return false;
    }
    
    void QueryPlanGenerator::addStandardPlans( NamespaceDetails *d ) {
        if ( !addCachedPlan( d ) ) {
            addFallbackPlans();
        }
    }
    
    bool QueryPlanGenerator::addCachedPlan( NamespaceDetails *d ) {
        if ( _recordedPlanPolicy == Ignore ) {
            return false;
        }

        CachedQueryPlan best = QueryUtilIndexed::bestIndexForPatterns( _qps.frsp(), _qps.order() );
        BSONObj bestIndex = best.indexKey();
        if ( bestIndex.isEmpty() ) {
            return false;
        }

        shared_ptr<QueryPlan> p;
        if ( str::equals( bestIndex.firstElementFieldName(), "$natural" ) ) {
            p = newPlan( d, -1 );
        }
        
        NamespaceDetails::IndexIterator i = d->ii();
        while( i.more() ) {
            int j = i.pos();
            IndexDetails& ii = i.next();
            if( ii.keyPattern().woCompare(bestIndex) == 0 ) {
                p = newPlan( d, j );
            }
        }
        
        massert( 10368 ,  "Unable to locate previously recorded index", p );

        if ( p->utility() == QueryPlan::Unhelpful ||
            p->utility() == QueryPlan::Disallowed ) {
            return false;
        }
        
        if ( _recordedPlanPolicy == UseIfInOrder && p->scanAndOrderRequired() ) {
            return false;
        }

        if ( !_allowSpecial && !p->special().empty() ) {
            return false;
        }

        _qps.setCachedPlan( p, best );
        return true;
    }

    shared_ptr<QueryPlan> QueryPlanGenerator::newPlan( NamespaceDetails *d,
                                                      int idxNo,
                                                      const BSONObj &min,
                                                      const BSONObj &max,
                                                      const string &special ) const {
        shared_ptr<QueryPlan> ret( QueryPlan::make( d, idxNo, _qps.frsp(), _originalFrsp.get(),
                                                   _qps.originalQuery(), _qps.order(), _parsedQuery,
                                                   min, max, special ) );
        return ret;
    }

    bool QueryPlanGenerator::setUnindexedPlanIf( bool set, NamespaceDetails *d ) {
        if ( set ) {
            setSingleUnindexedPlan( d );
        }
        return set;
    }
    
    void QueryPlanGenerator::setSingleUnindexedPlan( NamespaceDetails *d ) {
        _qps.setSinglePlan( newPlan( d, -1 ) );
    }
    
    void QueryPlanGenerator::setHintedPlanForIndex( IndexDetails& id ) {
        if ( !_min.isEmpty() || !_max.isEmpty() ) {
            string errmsg;
            BSONObj keyPattern = id.keyPattern();
            // This reformats _min and _max to be used for index lookup.
            massert( 10365 ,  errmsg, indexDetailsForRange( _qps.frsp().ns(), errmsg, _min, _max,
                                                           keyPattern ) );
        }
        NamespaceDetails *d = nsdetails( _qps.frsp().ns() );
        validateAndSetHintedPlan( newPlan( d, d->idxNo( id ), _min, _max ) );
    }

    void QueryPlanGenerator::validateAndSetHintedPlan( const shared_ptr<QueryPlan>& plan ) {
        uassert( 16331, "'special' plan hint not allowed",
                 _allowSpecial || plan->special().empty() );
        _qps.setSinglePlan( plan );
    }

    void QueryPlanGenerator::warnOnCappedIdTableScan() const {
        // if we are doing a table scan on _id
        // and it's a capped collection
        // we warn as it's a common user error
        // .system. and local collections are exempt
        const char *ns = _qps.frsp().ns();
        NamespaceDetails *d = nsdetails( ns );
        if ( d &&
            d->isCapped() &&
            _qps.nPlans() == 1 &&
            ( _qps.firstPlan()->utility() != QueryPlan::Impossible ) &&
            !_qps.firstPlan()->indexed() &&
            !_qps.firstPlan()->multikeyFrs().range( "_id" ).universal() ) {
            if (!str::contains( ns , ".system." ) && !str::startsWith( ns , "local." )) {
                warning() << "unindexed _id query on capped collection, "
                          << "performance will be poor collection: " << ns << endl;
            }
        }
    }
    
    QueryPlanSet* QueryPlanSet::make( const char* ns,
                                      auto_ptr<FieldRangeSetPair> frsp,
                                      auto_ptr<FieldRangeSetPair> originalFrsp,
                                      const BSONObj& originalQuery,
                                      const BSONObj& order,
                                      const shared_ptr<const ParsedQuery>& parsedQuery,
                                      const BSONObj& hint,
                                      QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy,
                                      const BSONObj& min,
                                      const BSONObj& max,
                                      bool allowSpecial ) {
        auto_ptr<QueryPlanSet> ret( new QueryPlanSet( ns, frsp, originalFrsp, originalQuery, order,
                                                     parsedQuery, hint, recordedPlanPolicy, min,
                                                     max, allowSpecial ) );
        ret->init();
        return ret.release();
    }


    QueryPlanSet::QueryPlanSet( const char *ns,
                               auto_ptr<FieldRangeSetPair> frsp,
                               auto_ptr<FieldRangeSetPair> originalFrsp,
                               const BSONObj &originalQuery,
                               const BSONObj &order,
                               const shared_ptr<const ParsedQuery> &parsedQuery,
                               const BSONObj &hint,
                               QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy,
                               const BSONObj &min,
                               const BSONObj &max,
                               bool allowSpecial ) :
        _generator( *this, originalFrsp, parsedQuery, hint, recordedPlanPolicy, min, max,
                    allowSpecial ),
        _originalQuery( originalQuery ),
        _frsp( frsp ),
        _mayRecordPlan(),
        _usingCachedPlan(),
        _order( order.getOwned() ),
        _oldNScanned( 0 ),
        _yieldSometimesTracker( 256, 20 ),
        _allowSpecial( allowSpecial ) {
    }

    bool QueryPlanSet::hasMultiKey() const {
        for( PlanVector::const_iterator i = _plans.begin(); i != _plans.end(); ++i )
            if ( (*i)->isMultiKey() )
                return true;
        return false;
    }

    void QueryPlanSet::init() {
        DEBUGQO( "QueryPlanSet::init " << ns << "\t" << _originalQuery );
        _plans.clear();
        _usingCachedPlan = false;

        _generator.addInitialPlans();
    }

    void QueryPlanSet::setSinglePlan( const QueryPlanPtr &plan ) {
        if ( nPlans() == 0 ) {
            pushPlan( plan );
        }
    }
    
    void QueryPlanSet::setCachedPlan( const QueryPlanPtr &plan,
                                     const CachedQueryPlan &cachedPlan ) {
        verify( nPlans() == 0 );
        _usingCachedPlan = true;
        _oldNScanned = cachedPlan.nScanned();
        _cachedPlanCharacter = cachedPlan.planCharacter();
        pushPlan( plan );
    }

    void QueryPlanSet::addCandidatePlan( const QueryPlanPtr &plan ) {
        // If _plans is nonempty, the new plan may be supplementing a recorded plan at the first
        // position of _plans.  It must not duplicate the first plan.
        if ( nPlans() > 0 && plan->indexKey() == firstPlan()->indexKey() ) {
            return;
        }
        pushPlan( plan );
        _mayRecordPlan = true;
    }
    
    void QueryPlanSet::addFallbackPlans() {
        _generator.addFallbackPlans();
        _mayRecordPlan = true;
    }

    void QueryPlanSet::pushPlan( const QueryPlanSet::QueryPlanPtr& plan ) {
        verify( _allowSpecial || plan->special().empty() );
        _plans.push_back( plan );
    }

    bool QueryPlanSet::hasPossiblyExcludedPlans() const {
        return
            _usingCachedPlan &&
            ( nPlans() == 1 ) &&
            ( firstPlan()->utility() != QueryPlan::Optimal );
    }
    
    QueryPlanSet::QueryPlanPtr QueryPlanSet::getBestGuess() const {
        verify( _plans.size() );
        if ( _plans[ 0 ]->scanAndOrderRequired() ) {
            for ( unsigned i=1; i<_plans.size(); i++ ) {
                if ( ! _plans[i]->scanAndOrderRequired() )
                    return _plans[i];
            }

            warning() << "best guess query plan requested, but scan and order are required for all plans "
            		  << " query: " << _originalQuery
            		  << " order: " << _order
            		  << " choices: ";

            for ( unsigned i=0; i<_plans.size(); i++ )
            	warning() << _plans[i]->indexKey() << " ";
            warning() << endl;

            return QueryPlanPtr();
        }
        return _plans[0];
    }
    
    bool QueryPlanSet::haveInOrderPlan() const {
        for( PlanVector::const_iterator i = _plans.begin(); i != _plans.end(); ++i ) {
            if ( !(*i)->scanAndOrderRequired() ) {
                return true;
            }
        }
        return false;
    }

    bool QueryPlanSet::possibleInOrderPlan() const {
        if ( haveInOrderPlan() ) {
            return true;
        }
        return _cachedPlanCharacter.mayRunInOrderPlan();
    }

    bool QueryPlanSet::possibleOutOfOrderPlan() const {
        for( PlanVector::const_iterator i = _plans.begin(); i != _plans.end(); ++i ) {
            if ( (*i)->scanAndOrderRequired() ) {
                return true;
            }
        }
        return _cachedPlanCharacter.mayRunOutOfOrderPlan();
    }

    CandidatePlanCharacter QueryPlanSet::characterizeCandidatePlans() const {
        return CandidatePlanCharacter( possibleInOrderPlan(), possibleOutOfOrderPlan() );
    }
    
    bool QueryPlanSet::prepareToRetryQuery() {
        if ( !hasPossiblyExcludedPlans() || _plans.size() > 1 ) {
            return false;
        }
        
        // A cached plan was used, so clear the plan for this query pattern so the query may be
        // retried without a cached plan.
        QueryUtilIndexed::clearIndexesForPatterns( *_frsp, _order );
        init();
        return true;
    }

    string QueryPlanSet::toString() const {
        BSONArrayBuilder bab;
        for( PlanVector::const_iterator i = _plans.begin(); i != _plans.end(); ++i ) {
            bab << (*i)->toString();
        }
        return bab.arr().jsonString();
    }
    
    MultiPlanScanner *MultiPlanScanner::make( const StringData& ns,
                                             const BSONObj &query,
                                             const BSONObj &order,
                                             const shared_ptr<const ParsedQuery> &parsedQuery,
                                             const BSONObj &hint,
                                             QueryPlanGenerator::RecordedPlanPolicy
                                                    recordedPlanPolicy,
                                             const BSONObj &min,
                                             const BSONObj &max ) {
        auto_ptr<MultiPlanScanner> ret( new MultiPlanScanner( ns, query, parsedQuery, hint,
                                                             recordedPlanPolicy ) );
        ret->init( order, min, max );
        return ret.release();
    }
    
    shared_ptr<QueryPlanRunner> MultiPlanScanner::iterateRunnerQueue
            ( QueryPlanRunner& originalRunner, bool retried ) {

        if ( _runnerQueue ) {
            return _runnerQueue->next();
        }
        
        _runnerQueue.reset( new QueryPlanRunnerQueue( *_currentQps, originalRunner ) );
        shared_ptr<ExplainClauseInfo> explainClause;
        if ( _explainQueryInfo ) {
            explainClause = _runnerQueue->generateExplainInfo();
        }
        
        shared_ptr<QueryPlanRunner> runner = _runnerQueue->next();
        if ( runner->error() &&
            _currentQps->prepareToRetryQuery() ) {

            // Avoid an infinite loop here - this should never occur.
            verify( !retried );
            _runnerQueue.reset();
            return iterateRunnerQueue( originalRunner, true );
        }
        
        if ( _explainQueryInfo ) {
            _explainQueryInfo->addClauseInfo( explainClause );
        }
        return runner;
    }

    void MultiPlanScanner::updateCurrentQps( QueryPlanSet *qps ) {
        _currentQps.reset( qps );
        _runnerQueue.reset();
    }

    QueryPlanRunnerQueue::QueryPlanRunnerQueue( QueryPlanSet& plans,
                                                const QueryPlanRunner& prototypeRunner ) :
        _prototypeRunner( prototypeRunner ),
        _plans( plans ),
        _done() {
    }

    void QueryPlanRunnerQueue::prepareToYield() {
        for( vector<shared_ptr<QueryPlanRunner> >::const_iterator i = _runners.begin();
             i != _runners.end(); ++i ) {
            prepareToYieldRunner( **i );
        }
    }

    void QueryPlanRunnerQueue::recoverFromYield() {
        for( vector<shared_ptr<QueryPlanRunner> >::const_iterator i = _runners.begin();
             i != _runners.end(); ++i ) {
            recoverFromYieldRunner( **i );
        }        
    }

    shared_ptr<QueryPlanRunner> QueryPlanRunnerQueue::init() {
        massert( 10369 ,  "no plans", _plans.plans().size() > 0 );
        
        if ( _plans.plans().size() > 1 )
            LOG(1) << "  running multiple plans" << endl;
        for( QueryPlanSet::PlanVector::const_iterator i = _plans.plans().begin();
             i != _plans.plans().end(); ++i ) {
            shared_ptr<QueryPlanRunner> runner( _prototypeRunner.createChild() );
            runner->setQueryPlan( i->get() );
            _runners.push_back( runner );
        }
        
        // Initialize runners.
        for( vector<shared_ptr<QueryPlanRunner> >::iterator i = _runners.begin();
             i != _runners.end(); ++i ) {
            initRunner( **i );
            if ( _explainClauseInfo ) {
                _explainClauseInfo->addPlanInfo( (*i)->generateExplainInfo() );
            }
        }
        
        // See if an op has completed.
        for( vector<shared_ptr<QueryPlanRunner> >::iterator i = _runners.begin();
             i != _runners.end(); ++i ) {
            if ( (*i)->complete() ) {
                return *i;
            }
        }
        
        // Put runnable ops in the priority queue.
        for( vector<shared_ptr<QueryPlanRunner> >::iterator i = _runners.begin();
             i != _runners.end(); ++i ) {
            if ( !(*i)->error() ) {
                _queue.push( *i );
            }
        }
        
        if ( _queue.empty() ) {
            return _runners.front();
        }
        
        return shared_ptr<QueryPlanRunner>();
    }
    
    shared_ptr<QueryPlanRunner> QueryPlanRunnerQueue::next() {
        verify( !done() );

        if ( _runners.empty() ) {
            shared_ptr<QueryPlanRunner> initialRet = init();
            if ( initialRet ) {
                _done = true;
                return initialRet;
            }
        }

        shared_ptr<QueryPlanRunner> ret;
        do {
            ret = _next();
        } while( ret->error() && !_queue.empty() );

        if ( _queue.empty() ) {
            _done = true;
        }

        return ret;
    }
    
    shared_ptr<QueryPlanRunner> QueryPlanRunnerQueue::_next() {
        verify( !_queue.empty() );
        RunnerHolder holder = _queue.pop();
        QueryPlanRunner& runner = *holder._runner;
        nextRunner( runner );
        if ( runner.complete() ) {
            if ( _plans.mayRecordPlan() && runner.mayRecordPlan() ) {
                runner.queryPlan().registerSelf( runner.nscanned(),
                                                 _plans.characterizeCandidatePlans() );
            }
            _done = true;
            return holder._runner;
        }
        if ( runner.error() ) {
            return holder._runner;
        }
        if ( _plans.hasPossiblyExcludedPlans() &&
            runner.nscanned() > _plans.oldNScanned() * 10 ) {
            verify( _plans.nPlans() == 1 && _plans.firstPlan()->special().empty() );
            holder._offset = -runner.nscanned();
            _plans.addFallbackPlans();
            QueryPlanSet::PlanVector::const_iterator i = _plans.plans().begin();
            ++i;
            for( ; i != _plans.plans().end(); ++i ) {
                shared_ptr<QueryPlanRunner> runner( _prototypeRunner.createChild() );
                runner->setQueryPlan( i->get() );
                _runners.push_back( runner );
                initRunner( *runner );
                if ( runner->complete() )
                    return runner;
                _queue.push( runner );
            }
            _plans.setUsingCachedPlan( false );
        }
        _queue.push( holder );
        return holder._runner;
    }
    
#define GUARD_RUNNER_EXCEPTION( runner, expression ) \
    try { \
        expression; \
    } \
    catch ( DBException& e ) { \
        runner.setException( e.getInfo() ); \
    } \
    catch ( const std::exception &e ) { \
        runner.setException( ExceptionInfo( e.what(), 0 ) ); \
    } \
    catch ( PageFaultException& pfe ) { \
        throw pfe; \
    } \
    catch ( ... ) { \
        runner.setException( ExceptionInfo( "Caught unknown exception", 0 ) ); \
    }


    void QueryPlanRunnerQueue::initRunner( QueryPlanRunner &runner ) {
        GUARD_RUNNER_EXCEPTION( runner, runner.init() );
    }

    void QueryPlanRunnerQueue::nextRunner( QueryPlanRunner& runner ) {
        GUARD_RUNNER_EXCEPTION( runner, if ( !runner.error() ) { runner.next(); } );
    }

    void QueryPlanRunnerQueue::prepareToYieldRunner( QueryPlanRunner& runner ) {
        GUARD_RUNNER_EXCEPTION( runner, if ( !runner.error() ) { runner.prepareToYield(); } );
    }

    void QueryPlanRunnerQueue::recoverFromYieldRunner( QueryPlanRunner& runner ) {
        GUARD_RUNNER_EXCEPTION( runner, if ( !runner.error() ) { runner.recoverFromYield(); } );
    }

    /**
     * NOTE on our $or implementation: In our current qo implementation we don't
     * keep statistics on our data, but we can conceptualize the problem of
     * selecting an index when statistics exist for all index ranges.  The
     * d-hitting set problem on k sets and n elements can be reduced to the
     * problem of index selection on k $or clauses and n index ranges (where
     * d is the max number of indexes, and the number of ranges n is unbounded).
     * In light of the fact that d-hitting set is np complete, and we don't even
     * track statistics (so cost calculations are expensive) our first
     * implementation uses the following greedy approach: We take one $or clause
     * at a time and treat each as a separate query for index selection purposes.
     * But if an index range is scanned for a particular $or clause, we eliminate
     * that range from all subsequent clauses.  One could imagine an opposite
     * implementation where we select indexes based on the union of index ranges
     * for all $or clauses, but this can have much poorer worst case behavior.
     * (An index range that suits one $or clause may not suit another, and this
     * is worse than the typical case of index range choice staleness because
     * with $or the clauses may likely be logically distinct.)  The greedy
     * implementation won't do any worse than all the $or clauses individually,
     * and it can often do better.  In the first cut we are intentionally using
     * QueryPattern tracking to record successful plans on $or clauses for use by
     * subsequent $or clauses, even though there may be a significant aggregate
     * $nor component that would not be represented in QueryPattern.    
     */
    
    MultiPlanScanner::MultiPlanScanner( const StringData& ns,
                                       const BSONObj &query,
                                       const shared_ptr<const ParsedQuery> &parsedQuery,
                                       const BSONObj &hint,
                                       QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy ) :
        _ns( ns.toString() ),
        _or( !query.getField( "$or" ).eoo() ),
        _query( query.getOwned() ),
        _parsedQuery( parsedQuery ),
        _i(),
        _recordedPlanPolicy( recordedPlanPolicy ),
        _hint( hint.getOwned() ),
        _tableScanned(),
        _doneRunners() {
    }
    
    void MultiPlanScanner::init( const BSONObj &order, const BSONObj &min, const BSONObj &max ) {
        if ( !order.isEmpty() || !min.isEmpty() || !max.isEmpty() ) {
            _or = false;
        }
        if ( _or ) {
            // Only construct an OrRangeGenerator if we may handle $or clauses.
            _org.reset( new OrRangeGenerator( _ns.c_str(), _query ) );
            if ( !_org->getSpecial().empty() ) {
                _or = false;
            }
            else if ( haveUselessOr() ) {
                _or = false;
            }
        }

        // if _or == false, don't use or clauses for index selection
        if ( !_or ) {
            ++_i;
            auto_ptr<FieldRangeSetPair> frsp( new FieldRangeSetPair( _ns.c_str(), _query, true ) );
            updateCurrentQps( QueryPlanSet::make( _ns.c_str(), frsp, auto_ptr<FieldRangeSetPair>(),
                                                 _query, order, _parsedQuery, _hint,
                                                 _recordedPlanPolicy,
                                                 min, max, true ) );
        }
        else {
            BSONElement e = _query.getField( "$or" );
            massert( 13268, "invalid $or spec",
                    e.type() == Array && e.embeddedObject().nFields() > 0 );
            handleBeginningOfClause();
        }
    }

    void MultiPlanScanner::handleEndOfClause( const QueryPlan &clausePlan ) {
        if ( clausePlan.willScanTable() ) {
            _tableScanned = true;   
        } else {
            _org->popOrClause( clausePlan.nsd(), clausePlan.idxNo(),
                              clausePlan.indexed() ? clausePlan.indexKey() : BSONObj() );
        }
    }
    
    void MultiPlanScanner::handleBeginningOfClause() {
        assertHasMoreClauses();
        ++_i;
        auto_ptr<FieldRangeSetPair> frsp( _org->topFrsp() );
        auto_ptr<FieldRangeSetPair> originalFrsp( _org->topFrspOriginal() );
        updateCurrentQps( QueryPlanSet::make( _ns.c_str(), frsp, originalFrsp, _query,
                                             BSONObj(), _parsedQuery, _hint, _recordedPlanPolicy,
                                             BSONObj(), BSONObj(),
                                             // 'Special' plans are not supported within $or.
                                             false ) );
    }

    bool MultiPlanScanner::mayHandleBeginningOfClause() {
        if ( hasMoreClauses() ) {
            handleBeginningOfClause();
            return true;
        }
        return false;
    }

    shared_ptr<QueryPlanRunner> MultiPlanScanner::nextRunner() {
        verify( !doneRunners() );
        shared_ptr<QueryPlanRunner> ret = _or ? nextRunnerOr() : nextRunnerSimple();
        if ( ret->error() || ret->complete() ) {
            _doneRunners = true;
        }
        return ret;
    }

    shared_ptr<QueryPlanRunner> MultiPlanScanner::nextRunnerSimple() {
        return iterateRunnerQueue( *_baseRunner );
    }

    shared_ptr<QueryPlanRunner> MultiPlanScanner::nextRunnerOr() {
        shared_ptr<QueryPlanRunner> runner;
        do {
            runner = nextRunnerSimple();
            if ( !runner->completeWithoutStop() ) {
                return runner;
            }
            handleEndOfClause( runner->queryPlan() );
            _baseRunner = runner;
        } while( mayHandleBeginningOfClause() );
        return runner;
    }
    
    const QueryPlan *MultiPlanScanner::nextClauseBestGuessPlan( const QueryPlan &currentPlan ) {
        assertHasMoreClauses();
        handleEndOfClause( currentPlan );
        if ( !hasMoreClauses() ) {
            return 0;
        }
        handleBeginningOfClause();
        shared_ptr<QueryPlan> bestGuess = _currentQps->getBestGuess();
        verify( bestGuess );
        return bestGuess.get();
    }
    
    void MultiPlanScanner::prepareToYield() {
        if ( _runnerQueue ) {
            _runnerQueue->prepareToYield();
        }
    }
    
    void MultiPlanScanner::recoverFromYield() {
        if ( _runnerQueue ) {
            _runnerQueue->recoverFromYield();   
        }
    }

    void MultiPlanScanner::clearRunnerQueue() {
        if ( _runnerQueue ) {
            _runnerQueue.reset();
        }
    }
    
    int MultiPlanScanner::currentNPlans() const {
        return _currentQps->nPlans();
    }

    const QueryPlan *MultiPlanScanner::singlePlan() const {
        if ( _or ||
            _currentQps->nPlans() != 1 ||
            _currentQps->hasPossiblyExcludedPlans() ) {
            return 0;
        }
        return _currentQps->firstPlan().get();
    }

    bool MultiPlanScanner::haveUselessOr() const {
        NamespaceDetails *nsd = nsdetails( _ns );
        if ( !nsd ) {
            return true;
        }
        BSONElement hintElt = _hint.firstElement();
        if ( !hintElt.eoo() ) {
            IndexDetails *id = parseHint( hintElt, nsd );
            if ( !id ) {
                return true;
            }
            return QueryUtilIndexed::uselessOr( *_org, nsd, nsd->idxNo( *id ) );
        }
        return QueryUtilIndexed::uselessOr( *_org, nsd, -1 );
    }
    
    BSONObj MultiPlanScanner::cachedPlanExplainSummary() const {
        if ( _or || !_currentQps->usingCachedPlan() ) {
            return BSONObj();
        }
        QueryPlanSet::QueryPlanPtr plan = _currentQps->firstPlan();
        shared_ptr<Cursor> cursor = plan->newCursor();
        return BSON( "cursor" << cursor->toString()
                    << "indexBounds" << cursor->prettyIndexBounds() );
    }
    
    void MultiPlanScanner::clearIndexesForPatterns() const {
        QueryUtilIndexed::clearIndexesForPatterns( _currentQps->frsp(), _currentQps->order() );
    }
    
    bool MultiPlanScanner::haveInOrderPlan() const {
        return _or ? true : _currentQps->haveInOrderPlan();
    }

    bool MultiPlanScanner::possibleInOrderPlan() const {
        return _or ? true : _currentQps->possibleInOrderPlan();
    }

    bool MultiPlanScanner::possibleOutOfOrderPlan() const {
        return _or ? false : _currentQps->possibleOutOfOrderPlan();
    }

    string MultiPlanScanner::toString() const {
        return BSON(
                    "or" << _or <<
                    "currentQps" << _currentQps->toString()
                    ).jsonString();
    }
    
    MultiCursor::MultiCursor( auto_ptr<MultiPlanScanner> mps, const shared_ptr<Cursor> &c,
                             const shared_ptr<CoveredIndexMatcher> &matcher,
                             const shared_ptr<ExplainPlanInfo> &explainPlanInfo,
                             const QueryPlanRunner& runner, long long nscanned ) :
    _mps( mps ),
    _c( c ),
    _matcher( matcher ),
    _queryPlan( &runner.queryPlan() ),
    _nscanned( nscanned ),
    _explainPlanInfo( explainPlanInfo ) {
        _mps->clearRunnerQueue();
        _mps->setRecordedPlanPolicy( QueryPlanGenerator::UseIfInOrder );
        if ( !ok() ) {
            // If the supplied cursor is exhausted, try to advance it.
            advance();
        }
    }
    
    bool MultiCursor::advance() {
        _c->advance();
        advanceExhaustedClauses();
        return ok();
    }

    void MultiCursor::recoverFromYield() {
        Cursor::recoverFromYield();
        advanceExhaustedClauses();
    }
    
    void MultiCursor::advanceClause() {
        _nscanned += _c->nscanned();
        if ( _explainPlanInfo ) _explainPlanInfo->noteDone( *_c );
        shared_ptr<FieldRangeVector> oldClauseFrv = _queryPlan->originalFrv();
        _queryPlan = _mps->nextClauseBestGuessPlan( *_queryPlan );
        if ( _queryPlan ) {
            _matcher.reset( _matcher->nextClauseMatcher( oldClauseFrv, _queryPlan->indexKey() ) );
            _c = _queryPlan->newCursor();
            // The basic and btree cursors used by this implementation support deduplication.
            verify( _c->autoDedup() );
            // All sub cursors must support yields.
            verify( _c->supportYields() );
            if ( _explainPlanInfo ) {
                _explainPlanInfo.reset( new ExplainPlanInfo() );
                _explainPlanInfo->notePlan( *_c, _queryPlan->scanAndOrderRequired(),
                                           _queryPlan->keyFieldsOnly() );
                shared_ptr<ExplainClauseInfo> clauseInfo( new ExplainClauseInfo() );
                clauseInfo->addPlanInfo( _explainPlanInfo );
                _mps->addClauseInfo( clauseInfo );
            }
        }
    }

    void MultiCursor::advanceExhaustedClauses() {
        while( !ok() && _mps->hasMoreClauses() ) {
            advanceClause();
        }
    }

    void MultiCursor::noteIterate( bool match, bool loadedRecord ) {
        if ( _explainPlanInfo ) _explainPlanInfo->noteIterate( match, loadedRecord, *_c );
    }
    
    bool indexWorks( const BSONObj &idxPattern, const BSONObj &sampleKey, int direction, int firstSignificantField ) {
        BSONObjIterator p( idxPattern );
        BSONObjIterator k( sampleKey );
        int i = 0;
        while( 1 ) {
            BSONElement pe = p.next();
            BSONElement ke = k.next();
            if ( pe.eoo() && ke.eoo() )
                return true;
            if ( pe.eoo() || ke.eoo() )
                return false;
            if ( strcmp( pe.fieldName(), ke.fieldName() ) != 0 )
                return false;
            if ( ( i == firstSignificantField ) && !( ( direction > 0 ) == ( pe.number() > 0 ) ) )
                return false;
            ++i;
        }
        return false;
    }

    BSONObj extremeKeyForIndex( const BSONObj &idxPattern, int baseDirection ) {
        BSONObjIterator i( idxPattern );
        BSONObjBuilder b;
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            int idxDirection = e.number() >= 0 ? 1 : -1;
            int direction = idxDirection * baseDirection;
            switch( direction ) {
            case 1:
                b.appendMaxKey( e.fieldName() );
                break;
            case -1:
                b.appendMinKey( e.fieldName() );
                break;
            default:
                verify( false );
            }
        }
        return b.obj();
    }

    pair<int,int> keyAudit( const BSONObj &min, const BSONObj &max ) {
        int direction = 0;
        int firstSignificantField = 0;
        BSONObjIterator i( min );
        BSONObjIterator a( max );
        while( 1 ) {
            BSONElement ie = i.next();
            BSONElement ae = a.next();
            if ( ie.eoo() && ae.eoo() )
                break;
            if ( ie.eoo() || ae.eoo() || strcmp( ie.fieldName(), ae.fieldName() ) != 0 ) {
                return make_pair( -1, -1 );
            }
            int cmp = ie.woCompare( ae );
            if ( cmp < 0 )
                direction = 1;
            if ( cmp > 0 )
                direction = -1;
            if ( direction != 0 )
                break;
            ++firstSignificantField;
        }
        return make_pair( direction, firstSignificantField );
    }

    pair<int,int> flexibleKeyAudit( const BSONObj &min, const BSONObj &max ) {
        if ( min.isEmpty() || max.isEmpty() ) {
            return make_pair( 1, -1 );
        }
        else {
            return keyAudit( min, max );
        }
    }

    // NOTE min, max, and keyPattern will be updated to be consistent with the selected index.
    IndexDetails *indexDetailsForRange( const char *ns, string &errmsg, BSONObj &min, BSONObj &max, BSONObj &keyPattern ) {
        if ( min.isEmpty() && max.isEmpty() ) {
            errmsg = "one of min or max must be specified";
            return 0;
        }

        Client::Context ctx( ns );
        IndexDetails *id = 0;
        NamespaceDetails *d = nsdetails( ns );
        if ( !d ) {
            errmsg = "ns not found";
            return 0;
        }

        pair<int,int> ret = flexibleKeyAudit( min, max );
        if ( ret == make_pair( -1, -1 ) ) {
            errmsg = "min and max keys do not share pattern";
            return 0;
        }
        if ( keyPattern.isEmpty() ) {
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                IndexDetails& ii = i.next();
                if ( indexWorks( ii.keyPattern(), min.isEmpty() ? max : min, ret.first, ret.second ) ) {
                    if ( ii.getSpec().getType() == 0 ) {
                        id = &ii;
                        keyPattern = ii.keyPattern();
                        break;
                    }
                }
            }

        }
        else {
            if ( !indexWorks( keyPattern, min.isEmpty() ? max : min, ret.first, ret.second ) ) {
                errmsg = "requested keyPattern does not match specified keys";
                return 0;
            }
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                IndexDetails& ii = i.next();
                if( ii.keyPattern().woCompare(keyPattern) == 0 ) {
                    id = &ii;
                    break;
                }
                if ( keyPattern.nFields() == 1 && ii.keyPattern().nFields() == 1 &&
                        IndexDetails::isIdIndexPattern( keyPattern ) &&
                        ii.isIdIndex() ) {
                    id = &ii;
                    break;
                }

            }
        }

        if ( min.isEmpty() ) {
            min = extremeKeyForIndex( keyPattern, -1 );
        }
        else if ( max.isEmpty() ) {
            max = extremeKeyForIndex( keyPattern, 1 );
        }

        if ( !id ) {
            errmsg = str::stream() << "no index found for specified keyPattern: " << keyPattern.toString() 
                                   << " min: " << min << " max: " << max;
            return 0;
        }

        min = min.extractFieldsUnDotted( keyPattern );
        max = max.extractFieldsUnDotted( keyPattern );

        return id;
    }
    
    shared_ptr<Cursor> NamespaceDetailsTransient::bestGuessCursor( const char *ns,
                                                                  const BSONObj &query,
                                                                  const BSONObj &sort ) {
        auto_ptr<FieldRangeSetPair> frsp( new FieldRangeSetPair( ns, query, true ) );
        auto_ptr<FieldRangeSetPair> origFrsp( new FieldRangeSetPair( *frsp ) );

        scoped_ptr<QueryPlanSet> qps( QueryPlanSet::make( ns, frsp, origFrsp, query, sort,
                                                         shared_ptr<const ParsedQuery>(), BSONObj(),
                                                         QueryPlanGenerator::UseIfInOrder,
                                                         BSONObj(), BSONObj(), true ) );
        QueryPlanSet::QueryPlanPtr qpp = qps->getBestGuess();
        if( ! qpp.get() ) return shared_ptr<Cursor>();

        shared_ptr<Cursor> ret = qpp->newCursor();

        // If we don't already have a matcher, supply one.
        if ( !query.isEmpty() && ! ret->matcher() ) {
            ret->setMatcher( qpp->matcher() );
        }
        return ret;
    }

    bool QueryUtilIndexed::indexUseful( const FieldRangeSetPair &frsp, NamespaceDetails *d, int idxNo, const BSONObj &order ) {
        DEV frsp.assertValidIndex( d, idxNo );
        BSONObj keyPattern = d->idx( idxNo ).keyPattern();
        if ( !frsp.matchPossibleForIndex( d, idxNo, keyPattern ) ) {
            // No matches are possible in the index so the index may be useful.
            return true;   
        }

        return d->idx( idxNo ).getSpec().suitability( frsp.frsForIndex( d , idxNo ) , order )
               != USELESS;
    }
    
    void QueryUtilIndexed::clearIndexesForPatterns( const FieldRangeSetPair &frsp, const BSONObj &order ) {
        SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
        NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get_inlock( frsp.ns() );
        CachedQueryPlan noCachedPlan;
        nsdt.registerCachedQueryPlanForPattern( frsp._singleKey.pattern( order ), noCachedPlan );
        nsdt.registerCachedQueryPlanForPattern( frsp._multiKey.pattern( order ), noCachedPlan );
    }
    
    CachedQueryPlan QueryUtilIndexed::bestIndexForPatterns( const FieldRangeSetPair &frsp, const BSONObj &order ) {
        SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
        NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get_inlock( frsp.ns() );
        // TODO Maybe it would make sense to return the index with the lowest
        // nscanned if there are two possibilities.
        {
            QueryPattern pattern = frsp._singleKey.pattern( order );
            CachedQueryPlan cachedQueryPlan = nsdt.cachedQueryPlanForPattern( pattern );
            if ( !cachedQueryPlan.indexKey().isEmpty() ) {
                return cachedQueryPlan;
            }
        }
        {
            QueryPattern pattern = frsp._multiKey.pattern( order );
            CachedQueryPlan cachedQueryPlan = nsdt.cachedQueryPlanForPattern( pattern );
            if ( !cachedQueryPlan.indexKey().isEmpty() ) {
                return cachedQueryPlan;
            }
        }
        return CachedQueryPlan();
    }
    
    bool QueryUtilIndexed::uselessOr( const OrRangeGenerator &org, NamespaceDetails *d, int hintIdx ) {
        for( list<FieldRangeSetPair>::const_iterator i = org._originalOrSets.begin(); i != org._originalOrSets.end(); ++i ) {
            if ( hintIdx != -1 ) {
                if ( !indexUseful( *i, d, hintIdx, BSONObj() ) ) {
                    return true;   
                }
            }
            else {
                bool useful = false;
                for( int j = 0; j < d->nIndexes; ++j ) {
                    if ( indexUseful( *i, d, j, BSONObj() ) ) {
                        useful = true;
                        break;
                    }
                }
                if ( !useful ) {
                    return true;
                }
            }
        }
        return false;
    }
    
} // namespace mongo
