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

#include "pch.h"

#include "db.h"
#include "btree.h"
#include "pdfile.h"
#include "queryoptimizer.h"
#include "cmdline.h"
#include "clientcursor.h"

//#define DEBUGQO(x) cout << x << endl;
#define DEBUGQO(x)

namespace mongo {

    void checkTableScanAllowed( const char * ns ) {
        if ( ! cmdLine.noTableScan )
            return;

        if ( strstr( ns , ".system." ) ||
                strstr( ns , "local." ) )
            return;

        if ( ! nsdetails( ns ) )
            return;

        uassert( 10111 ,  (string)"table scans not allowed:" + ns , ! cmdLine.noTableScan );
    }

    double elementDirection( const BSONElement &e ) {
        if ( e.isNumber() )
            return e.number();
        return 1;
    }

    QueryPlan::QueryPlan(
        NamespaceDetails *d, int idxNo,
        const FieldRangeSetPair &frsp, const FieldRangeSetPair *originalFrsp, const BSONObj &originalQuery, const BSONObj &order, bool mustAssertOnYieldFailure, const BSONObj &startKey, const BSONObj &endKey , string special ) :
        _d(d), _idxNo(idxNo),
        _frs( frsp.frsForIndex( _d, _idxNo ) ),
        _frsMulti( frsp.frsForIndex( _d, -1 ) ),
        _originalQuery( originalQuery ),
        _order( order ),
        _index( 0 ),
        _optimal( false ),
        _scanAndOrderRequired( true ),
        _exactKeyMatch( false ),
        _direction( 0 ),
        _endKeyInclusive( endKey.isEmpty() ),
        _unhelpful( false ),
        _impossible( false ),
        _special( special ),
        _type(0),
        _startOrEndSpec( !startKey.isEmpty() || !endKey.isEmpty() ),
        _mustAssertOnYieldFailure( mustAssertOnYieldFailure ) {

        BSONObj idxKey = _idxNo < 0 ? BSONObj() : d->idx( _idxNo ).keyPattern();
            
        if ( !_frs.matchPossibleForIndex( idxKey ) ) {
            _impossible = true;
            _scanAndOrderRequired = false;
            return;
        }
            
        if ( willScanTable() ) {
            if ( _order.isEmpty() || !strcmp( _order.firstElementFieldName(), "$natural" ) )
                _scanAndOrderRequired = false;
            return;                
        }
            
        _index = &d->idx(_idxNo);

        // If the parsing or index indicates this is a special query, don't continue the processing
        if ( _special.size() ||
            ( _index->getSpec().getType() && _index->getSpec().getType()->suitability( originalQuery, order ) != USELESS ) ) {

            if( _special.size() ) _optimal = true;

            _type  = _index->getSpec().getType();
            if( !_special.size() ) _special = _index->getSpec().getType()->getPlugin()->getName();

            massert( 13040 , (string)"no type for special: " + _special , _type );
            // hopefully safe to use original query in these contexts - don't think we can mix special with $or clause separation yet
            _scanAndOrderRequired = _type->scanAndOrderRequired( _originalQuery , order );
            return;
        }

        const IndexSpec &idxSpec = _index->getSpec();
        BSONObjIterator o( order );
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
        bool stillOptimalIndexedQueryCount = true;
        set<string> orderFieldsUnindexed;
        order.getFieldNames( orderFieldsUnindexed );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const FieldRange &fr = _frs.range( e.fieldName() );
            if ( stillOptimalIndexedQueryCount ) {
                if ( fr.nontrivial() )
                    ++optimalIndexedQueryCount;
                if ( !fr.equality() )
                    stillOptimalIndexedQueryCount = false;
            }
            else {
                if ( fr.nontrivial() )
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
                ( optimalIndexedQueryCount == _frs.nNontrivialRanges() ) )
            _optimal = true;
        if ( exactIndexedQueryCount == _frs.nNontrivialRanges() &&
                orderFieldsUnindexed.size() == 0 &&
                exactIndexedQueryCount == idxKey.nFields() &&
                exactIndexedQueryCount == _originalQuery.nFields() ) {
            _exactKeyMatch = true;
        }
        _frv.reset( new FieldRangeVector( _frs, idxSpec, _direction ) );
        if ( originalFrsp ) {
            _originalFrv.reset( new FieldRangeVector( originalFrsp->frsForIndex( _d, _idxNo ), idxSpec, _direction ) );
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
                !_frs.range( idxKey.firstElementFieldName() ).nontrivial() ) {
            _unhelpful = true;
        }
    }

    shared_ptr<Cursor> QueryPlan::newCursor( const DiskLoc &startLoc , int numWanted ) const {

        if ( _type ) {
            // hopefully safe to use original query in these contexts - don't think we can mix type with $or clause separation yet
            return _type->newCursor( _originalQuery , _order , numWanted );
        }

        if ( _impossible ) {
            // TODO We might want to allow this dummy table scan even in no table
            // scan mode, since it won't scan anything.
            if ( _frs.nNontrivialRanges() )
                checkTableScanAllowed( _frs.ns() );
            return shared_ptr<Cursor>( new BasicCursor( DiskLoc() ) );
        }

        if ( willScanTable() ) {
            if ( _frs.nNontrivialRanges() ) {
                checkTableScanAllowed( _frs.ns() );
                
                // if we are doing a table scan on _id
                // and it's a capped collection
                // we warn /*disallow*/ as it's a common user error
                // .system. and local collections are exempt
                if ( _d && _d->capped && _frs.range( "_id" ).nontrivial() ) {
                    if ( cc().isSyncThread() ||
                         str::contains( _frs.ns() , ".system." ) || 
                         str::startsWith( _frs.ns() , "local." ) ) {
                        // ok
                    }
                    else {
                        warning() << "_id query on capped collection without an _id index, performance will be poor collection: " << _frs.ns() << endl;
                        //uassert( 14820, str::stream() << "doing _id query on a capped collection without an index is not allowed: " << _frs.ns() ,
                    }
                }
            }
            return findTableScan( _frs.ns(), _order, startLoc );
        }
                
        massert( 10363 ,  "newCursor() with start location not implemented for indexed plans", startLoc.isNull() );

        if ( _startOrEndSpec ) {
            // we are sure to spec _endKeyInclusive
            return shared_ptr<Cursor>( BtreeCursor::make( _d, _idxNo, *_index, _startKey, _endKey, _endKeyInclusive, _direction >= 0 ? 1 : -1 ) );
        }
        else if ( _index->getSpec().getType() ) {
            return shared_ptr<Cursor>( BtreeCursor::make( _d, _idxNo, *_index, _frv->startKey(), _frv->endKey(), true, _direction >= 0 ? 1 : -1 ) );
        }
        else {
            return shared_ptr<Cursor>( BtreeCursor::make( _d, _idxNo, *_index, _frv, _direction >= 0 ? 1 : -1 ) );
        }
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

    void QueryPlan::registerSelf( long long nScanned ) const {
        // Impossible query constraints can be detected before scanning, and we
        // don't have a reserved pattern enum value for impossible constraints.
        if ( _impossible ) {
            return;
        }

        SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
        NamespaceDetailsTransient::get_inlock( ns() ).registerIndexForPattern( _frs.pattern( _order ), indexKey(), nScanned );
    }
    
    /**
     * @return a copy of the inheriting class, which will be run with its own
     * query plan.  If multiple plan sets are required for an $or query, the
     * QueryOp of the winning plan from a given set will be cloned to generate
     * QueryOps for the subsequent plan set.  This function should only be called
     * after the query op has completed executing.
     */    
    QueryOp *QueryOp::createChild() {
        if( _orConstraint.get() ) {
            _matcher->advanceOrClause( _orConstraint );
            _orConstraint.reset();
        }
        QueryOp *ret = _createChild();
        ret->_oldMatcher = _matcher;
        return ret;
    }    

    bool QueryPlan::isMultiKey() const {
        if ( _idxNo < 0 )
            return false;
        return _d->isMultikey( _idxNo );
    }
    
    void QueryOp::init() {
        if ( _oldMatcher.get() ) {
            _matcher.reset( _oldMatcher->nextClauseMatcher( qp().indexKey() ) );
        }
        else {
            _matcher.reset( new CoveredIndexMatcher( qp().originalQuery(), qp().indexKey(), alwaysUseRecord() ) );
        }
        _init();
    }    

    QueryPlanSet::QueryPlanSet( const char *ns, auto_ptr<FieldRangeSetPair> frsp, auto_ptr<FieldRangeSetPair> originalFrsp, const BSONObj &originalQuery, const BSONObj &order, bool mustAssertOnYieldFailure, const BSONElement *hint, bool honorRecordedPlan, const BSONObj &min, const BSONObj &max, bool bestGuessOnly, bool mayYield ) :
        _ns(ns),
        _originalQuery( originalQuery ),
        _frsp( frsp ),
        _originalFrsp( originalFrsp ),
        _mayRecordPlan( false ),
        _usingCachedPlan( false ),
        _hint( BSONObj() ),
        _order( order.getOwned() ),
        _oldNScanned( 0 ),
        _honorRecordedPlan( honorRecordedPlan ),
        _min( min.getOwned() ),
        _max( max.getOwned() ),
        _bestGuessOnly( bestGuessOnly ),
        _mayYield( mayYield ),
	    _yieldSometimesTracker( 256, 20 ),
        _mustAssertOnYieldFailure( mustAssertOnYieldFailure ) {
        if ( hint && !hint->eoo() ) {
            _hint = hint->wrap();
        }
        init();
    }

    bool QueryPlanSet::modifiedKeys() const {
        for( PlanSet::const_iterator i = _plans.begin(); i != _plans.end(); ++i )
            if ( (*i)->isMultiKey() )
                return true;
        return false;
    }

    bool QueryPlanSet::hasMultiKey() const {
        for( PlanSet::const_iterator i = _plans.begin(); i != _plans.end(); ++i )
            if ( (*i)->isMultiKey() )
                return true;
        return false;
    }


    void QueryPlanSet::addHint( IndexDetails &id ) {
        if ( !_min.isEmpty() || !_max.isEmpty() ) {
            string errmsg;
            BSONObj keyPattern = id.keyPattern();
            // This reformats _min and _max to be used for index lookup.
            massert( 10365 ,  errmsg, indexDetailsForRange( _frsp->ns(), errmsg, _min, _max, keyPattern ) );
        }
        NamespaceDetails *d = nsdetails(_ns);
        _plans.push_back( QueryPlanPtr( new QueryPlan( d, d->idxNo(id), *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure, _min, _max ) ) );
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

    void QueryPlanSet::init() {
        DEBUGQO( "QueryPlanSet::init " << ns << "\t" << _originalQuery );
        _runner.reset();
        _plans.clear();
        _usingCachedPlan = false;

        const char *ns = _frsp->ns();
        NamespaceDetails *d = nsdetails( ns );
        if ( !d || !_frsp->matchPossible() ) {
            // Table scan plan, when no matches are possible
            _plans.push_back( QueryPlanPtr( new QueryPlan( d, -1, *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure ) ) );
            return;
        }

        BSONElement hint = _hint.firstElement();
        if ( !hint.eoo() ) {
            IndexDetails *id = parseHint( hint, d );
            if ( id ) {
                addHint( *id );
            }
            else {
                massert( 10366 ,  "natural order cannot be specified with $min/$max", _min.isEmpty() && _max.isEmpty() );
                // Table scan plan
                _plans.push_back( QueryPlanPtr( new QueryPlan( d, -1, *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure ) ) );
            }
            return;
        }

        if ( !_min.isEmpty() || !_max.isEmpty() ) {
            string errmsg;
            BSONObj keyPattern;
            IndexDetails *idx = indexDetailsForRange( ns, errmsg, _min, _max, keyPattern );
            massert( 10367 ,  errmsg, idx );
            _plans.push_back( QueryPlanPtr( new QueryPlan( d, d->idxNo(*idx), *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure, _min, _max ) ) );
            return;
        }

        if ( isSimpleIdQuery( _originalQuery ) ) {
            int idx = d->findIdIndex();
            if ( idx >= 0 ) {
                _plans.push_back( QueryPlanPtr( new QueryPlan( d , idx , *_frsp , _originalFrsp.get() , _originalQuery, _order, _mustAssertOnYieldFailure ) ) );
                return;
            }
        }

        if ( _originalQuery.isEmpty() && _order.isEmpty() ) {
            _plans.push_back( QueryPlanPtr( new QueryPlan( d, -1, *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure ) ) );
            return;
        }

        DEBUGQO( "\t special : " << _frsp->getSpecial() );
        if ( _frsp->getSpecial().size() ) {
            _special = _frsp->getSpecial();
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                int j = i.pos();
                IndexDetails& ii = i.next();
                const IndexSpec& spec = ii.getSpec();
                if ( spec.getTypeName() == _special && spec.suitability( _originalQuery , _order ) ) {
                    _plans.push_back( QueryPlanPtr( new QueryPlan( d , j , *_frsp , _originalFrsp.get() , _originalQuery, _order ,
                                                    _mustAssertOnYieldFailure , BSONObj() , BSONObj() , _special ) ) );
                    return;
                }
            }
            uassert( 13038 , (string)"can't find special index: " + _special + " for: " + _originalQuery.toString() , 0 );
        }

        if ( _honorRecordedPlan ) {
            pair< BSONObj, long long > best = QueryUtilIndexed::bestIndexForPatterns( *_frsp, _order );
            BSONObj bestIndex = best.first;
            long long oldNScanned = best.second;
            if ( !bestIndex.isEmpty() ) {
                QueryPlanPtr p;
                _oldNScanned = oldNScanned;
                if ( !strcmp( bestIndex.firstElementFieldName(), "$natural" ) ) {
                    // Table scan plan
                    p.reset( new QueryPlan( d, -1, *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure ) );
                }

                NamespaceDetails::IndexIterator i = d->ii();
                while( i.more() ) {
                    int j = i.pos();
                    IndexDetails& ii = i.next();
                    if( ii.keyPattern().woCompare(bestIndex) == 0 ) {
                        p.reset( new QueryPlan( d, j, *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure ) );
                    }
                }

                massert( 10368 ,  "Unable to locate previously recorded index", p.get() );
                if ( !( _bestGuessOnly && p->scanAndOrderRequired() ) ) {
                    _usingCachedPlan = true;
                    _plans.push_back( p );
                    return;
                }
            }
        }

        addOtherPlans( false );
    }

    void QueryPlanSet::addOtherPlans( bool checkFirst ) {
        const char *ns = _frsp->ns();
        NamespaceDetails *d = nsdetails( ns );
        if ( !d )
            return;

        // If table scan is optimal or natural order requested or tailable cursor requested
        if ( !_frsp->matchPossible() || ( _frsp->noNontrivialRanges() && _order.isEmpty() ) ||
                ( !_order.isEmpty() && !strcmp( _order.firstElementFieldName(), "$natural" ) ) ) {
            // Table scan plan
            addPlan( QueryPlanPtr( new QueryPlan( d, -1, *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure ) ), checkFirst );
            return;
        }

        bool normalQuery = _hint.isEmpty() && _min.isEmpty() && _max.isEmpty();

        PlanSet plans;
        QueryPlanPtr optimalPlan;
        QueryPlanPtr specialPlan;
        for( int i = 0; i < d->nIndexes; ++i ) {
            if ( normalQuery ) {
                BSONObj keyPattern = d->idx( i ).keyPattern();
                if ( !_frsp->matchPossibleForIndex( d, i, keyPattern ) ) {
                    // If no match is possible, only generate a trival plan that won't
                    // scan any documents.
                    QueryPlanPtr p( new QueryPlan( d, i, *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure ) );
                    addPlan( p, checkFirst );
                    return;
                }
                if ( !QueryUtilIndexed::indexUseful( *_frsp, d, i, _order ) ) {
                    continue;
                }
            }

            QueryPlanPtr p( new QueryPlan( d, i, *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure ) );
            if ( p->optimal() ) {
                if ( !optimalPlan.get() ) {
                    optimalPlan = p;
                }
            }
            else if ( !p->unhelpful() ) {
                if ( p->special().empty() ) {
                    plans.push_back( p );
                }
                else {
                    specialPlan = p;
                }
            }
        }
        if ( optimalPlan.get() ) {
            addPlan( optimalPlan, checkFirst );
            return;
        }
        for( PlanSet::const_iterator i = plans.begin(); i != plans.end(); ++i ) {
            addPlan( *i, checkFirst );
        }

        // Only add a special plan if no standard btree plans have been added. SERVER-4531
        if ( plans.empty() && specialPlan ) {
            addPlan( specialPlan, checkFirst );
            return;
        }

        // Table scan plan
        addPlan( QueryPlanPtr( new QueryPlan( d, -1, *_frsp, _originalFrsp.get(), _originalQuery, _order, _mustAssertOnYieldFailure ) ), checkFirst );
        _mayRecordPlan = true;
    }

    shared_ptr<QueryOp> QueryPlanSet::runOp( QueryOp &op ) {
        if ( _usingCachedPlan ) {
            Runner r( *this, op );
            shared_ptr<QueryOp> res = r.runUntilFirstCompletes();
            // _plans.size() > 1 if addOtherPlans was called in Runner::runUntilFirstCompletes().
            if ( _bestGuessOnly || res->complete() || _plans.size() > 1 )
                return res;
            // A cached plan was used, so clear the plan for this query pattern and retry the query without a cached plan.
            // Carefull here, as the namespace may have been dropped.
            QueryUtilIndexed::clearIndexesForPatterns( *_frsp, _order );
            init();
        }
        Runner r( *this, op );
        return r.runUntilFirstCompletes();
    }
    
    shared_ptr<QueryOp> QueryPlanSet::nextOp( QueryOp &originalOp, bool retried ) {
        if ( !_runner ) {
            _runner.reset( new Runner( *this, originalOp ) );
            shared_ptr<QueryOp> op = _runner->init();
            if ( op->complete() ) {
                return op;   
            }
        }
        shared_ptr<QueryOp> op = _runner->nextNonError();
        if ( !op->error() ) {
            return op;   
        }
        if ( !_usingCachedPlan || _bestGuessOnly || _plans.size() > 1 ) {
            return op;
        }

        // Avoid an infinite loop here - this should never occur.
        verify( 15878, !retried );

        // A cached plan was used, so clear the plan for this query pattern and retry the query without a cached plan.
        QueryUtilIndexed::clearIndexesForPatterns( *_frsp, _order );
        init();
        return nextOp( originalOp, true );
    }

    bool QueryPlanSet::prepareToYield() {
        return _runner ? _runner->prepareToYield() : true;   
    }
    
    void QueryPlanSet::recoverFromYield() {
        if ( _runner ) {
            _runner->recoverFromYield();   
        }
    }
    
    void QueryPlanSet::clearRunner() {
        if ( _runner ) {
            _runner.reset();
        }
    }
    
    BSONObj QueryPlanSet::explain() const {
        vector<BSONObj> arr;
        for( PlanSet::const_iterator i = _plans.begin(); i != _plans.end(); ++i ) {
            shared_ptr<Cursor> c = (*i)->newCursor();
            BSONObjBuilder explain;
            explain.append( "cursor", c->toString() );
            explain.append( "indexBounds", c->prettyIndexBounds() );
            arr.push_back( explain.obj() );
        }
        BSONObjBuilder b;
        b.append( "allPlans", arr );
        return b.obj();
    }

    QueryPlanSet::QueryPlanPtr QueryPlanSet::getBestGuess() const {
        assert( _plans.size() );
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

    QueryPlanSet::Runner::Runner( QueryPlanSet &plans, QueryOp &op ) :
        _op( op ),
        _plans( plans ) {
    }

    bool QueryPlanSet::Runner::prepareToYield() {
        for( vector<shared_ptr<QueryOp> >::const_iterator i = _ops.begin(); i != _ops.end(); ++i ) {
            if ( !prepareToYieldOp( **i ) ) {
                return false;
            }
        }
        return true;
    }

    void QueryPlanSet::Runner::recoverFromYield() {
        for( vector<shared_ptr<QueryOp> >::const_iterator i = _ops.begin(); i != _ops.end(); ++i ) {
            recoverFromYieldOp( **i );
        }        
    }

    void QueryPlanSet::Runner::mayYield() {
        if ( ! _plans._mayYield ) 
            return;
        
        if ( ! _plans._yieldSometimesTracker.intervalHasElapsed() ) 
            return;
        
        int micros = ClientCursor::suggestYieldMicros();
        if ( micros <= 0 ) 
            return;

        if ( !prepareToYield() ) 
            return;   
        
        ClientCursor::staticYield( micros , _plans._ns , 0 );
        recoverFromYield();
    }

    shared_ptr<QueryOp> QueryPlanSet::Runner::init() {
        massert( 10369 ,  "no plans", _plans._plans.size() > 0 );
        
        if ( _plans._bestGuessOnly ) {
            shared_ptr<QueryOp> op( _op.createChild() );
            shared_ptr<QueryPlan> plan = _plans.getBestGuess();
            massert( 15894, "no index matches QueryPlanSet's sort with _bestGuessOnly", plan.get() );
            op->setQueryPlan( plan.get() );
            _ops.push_back( op );
        }
        else {
            if ( _plans._plans.size() > 1 )
                log(1) << "  running multiple plans" << endl;
            for( PlanSet::iterator i = _plans._plans.begin(); i != _plans._plans.end(); ++i ) {
                shared_ptr<QueryOp> op( _op.createChild() );
                op->setQueryPlan( i->get() );
                _ops.push_back( op );
            }
        }
        
        // Initialize ops.
        for( vector<shared_ptr<QueryOp> >::iterator i = _ops.begin(); i != _ops.end(); ++i ) {
            initOp( **i );
            if ( (*i)->complete() )
                return *i;
        }
        
        // Put runnable ops in the priority queue.
        for( vector<shared_ptr<QueryOp> >::iterator i = _ops.begin(); i != _ops.end(); ++i ) {
            if ( !(*i)->error() ) {
                _queue.push( *i );
            }
        }
        
        return *_ops.begin();
    }
    
    shared_ptr<QueryOp> QueryPlanSet::Runner::nextNonError() {
        if ( _queue.empty() ) {
            return *_ops.begin();   
        }
        shared_ptr<QueryOp> ret;
        do {
            ret = next();
        } while( ret->error() && !_queue.empty() );
        return ret;
    }
    
    shared_ptr<QueryOp> QueryPlanSet::Runner::next() {
        mayYield();
        dassert( !_queue.empty() );
        OpHolder holder = _queue.pop();
        QueryOp &op = *holder._op;
        nextOp( op );
        if ( op.complete() ) {
            if ( _plans._mayRecordPlan && op.mayRecordPlan() ) {
                op.qp().registerSelf( op.nscanned() );
            }
            return holder._op;
        }
        if ( op.error() ) {
            return holder._op;
        }
        if ( !_plans._bestGuessOnly && _plans._usingCachedPlan && op.nscanned() > _plans._oldNScanned * 10 && _plans._special.empty() ) {
            holder._offset = -op.nscanned();
            _plans.addOtherPlans( /* avoid duplicating the initial plan */ true );
            PlanSet::iterator i = _plans._plans.begin();
            ++i;
            for( ; i != _plans._plans.end(); ++i ) {
                shared_ptr<QueryOp> op( _op.createChild() );
                op->setQueryPlan( i->get() );
                _ops.push_back( op );
                initOp( *op );
                if ( op->complete() )
                    return op;
                _queue.push( op );
            }
            _plans._usingCachedPlan = false;
        }
        _queue.push( holder );
        return holder._op;
    }
    
    shared_ptr<QueryOp> QueryPlanSet::Runner::runUntilFirstCompletes() {
        shared_ptr<QueryOp> potentialFinisher = init();
        if ( potentialFinisher->complete() ) {
         	return potentialFinisher;
        }
        
        while( !_queue.empty() ) {
            shared_ptr<QueryOp> potentialFinisher = next();
            if ( potentialFinisher->complete() ) {
                return potentialFinisher;
            }
        }
        return _ops[ 0 ];
    }

#define GUARD_OP_EXCEPTION( op, expression ) \
    try { \
        expression; \
    } \
    catch ( DBException& e ) { \
        op.setException( e.getInfo() ); \
    } \
    catch ( const std::exception &e ) { \
        op.setException( ExceptionInfo( e.what() , 0 ) ); \
    } \
    catch ( ... ) { \
        op.setException( ExceptionInfo( "Caught unknown exception" , 0 ) ); \
    }


    void QueryPlanSet::Runner::initOp( QueryOp &op ) {
        GUARD_OP_EXCEPTION( op, op.init() );
    }

    void QueryPlanSet::Runner::nextOp( QueryOp &op ) {
        GUARD_OP_EXCEPTION( op, if ( !op.error() ) { op.next(); } );
    }

    bool QueryPlanSet::Runner::prepareToYieldOp( QueryOp &op ) {
        GUARD_OP_EXCEPTION( op,
        if ( op.error() ) {
            return true;
        }
        else {
            return op.prepareToYield();
        } );
        return true;
    }

    void QueryPlanSet::Runner::recoverFromYieldOp( QueryOp &op ) {
        GUARD_OP_EXCEPTION( op, if ( !op.error() ) { op.recoverFromYield(); } );
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
    
    MultiPlanScanner::MultiPlanScanner( const char *ns,
                                        const BSONObj &query,
                                        const BSONObj &order,
                                        const BSONElement *hint,
                                        bool honorRecordedPlan,
                                        const BSONObj &min,
                                        const BSONObj &max,
                                        bool bestGuessOnly,
                                        bool mayYield ) :
        _ns( ns ),
        _or( !query.getField( "$or" ).eoo() ),
        _query( query.getOwned() ),
        _i(),
        _honorRecordedPlan( honorRecordedPlan ),
        _bestGuessOnly( bestGuessOnly ),
        _hint( ( hint && !hint->eoo() ) ? hint->wrap() : BSONObj() ),
        _mayYield( mayYield ),
        _tableScanned() {
        if ( !order.isEmpty() || !min.isEmpty() || !max.isEmpty() ) {
            _or = false;
        }
        if ( _or ) {
            // Only construct an OrRangeGenerator if we may handle $or clauses.
            _org.reset( new OrRangeGenerator( _ns.c_str(), _query ) );
            if ( !_org->getSpecial().empty() ) {
                _or = false;
            }
            else if ( uselessOr( _hint.firstElement() ) ) {
                _or = false;   
            }
        }
        // if _or == false, don't use or clauses for index selection
        if ( !_or ) {
            auto_ptr<FieldRangeSetPair> frsp( new FieldRangeSetPair( _ns.c_str(), _query, true ) );
            _currentQps.reset( new QueryPlanSet( _ns.c_str(), frsp, auto_ptr<FieldRangeSetPair>(),
                                                _query, order, false, hint, honorRecordedPlan, min,
                                                max, _bestGuessOnly, _mayYield ) );
        }
        else {
            BSONElement e = _query.getField( "$or" );
            massert( 13268, "invalid $or spec", e.type() == Array && e.embeddedObject().nFields() > 0 );
        }
    }

    shared_ptr<QueryOp> MultiPlanScanner::runOpOnce( QueryOp &op ) {
        assertMayRunMore();
        if ( !_or ) {
            ++_i;
            return _currentQps->runOp( op );
        }
        ++_i;
        auto_ptr<FieldRangeSetPair> frsp( _org->topFrsp() );
        auto_ptr<FieldRangeSetPair> originalFrsp( _org->topFrspOriginal() );
        BSONElement hintElt = _hint.firstElement();
        _currentQps.reset( new QueryPlanSet( _ns.c_str(), frsp, originalFrsp, _query, BSONObj(),
                                            true, &hintElt, _honorRecordedPlan, BSONObj(),
                                            BSONObj(), _bestGuessOnly, _mayYield ) );
        shared_ptr<QueryOp> ret( _currentQps->runOp( op ) );
        if ( ! ret->complete() )
            throw MsgAssertionException( ret->exception() );
        if ( ret->qp().willScanTable() ) {
            _tableScanned = true;
        } else {
            // If the full table was scanned, don't bother popping the last or clause.
	        _org->popOrClause( ret->qp().nsd(), ret->qp().idxNo(), ret->qp().indexed() ? ret->qp().indexKey() : BSONObj() );
        }
        return ret;
    }

    shared_ptr<QueryOp> MultiPlanScanner::runOp( QueryOp &op ) {
        shared_ptr<QueryOp> ret = runOpOnce( op );
        while( !ret->stopRequested() && mayRunMore() ) {
            ret = runOpOnce( *ret );
        }
        return ret;
    }
    
    shared_ptr<QueryOp> MultiPlanScanner::nextOpHandleEndOfClause() {
        shared_ptr<QueryOp> op = _currentQps->nextOp( *_baseOp );
        if ( !op->complete() ) {
            return op;   
        }
        if ( op->qp().willScanTable() ) {
            _tableScanned = true;   
        } else {
            _org->popOrClause( op->qp().nsd(), op->qp().idxNo(), op->qp().indexed() ? op->qp().indexKey() : BSONObj() );         	   
        }
        return op;
    }
    
    shared_ptr<QueryOp> MultiPlanScanner::nextOpBeginningClause() {
        assertMayRunMore();
        shared_ptr<QueryOp> op;
        while( mayRunMore() ) {
	        ++_i;
    	    auto_ptr<FieldRangeSetPair> frsp( _org->topFrsp() );
        	auto_ptr<FieldRangeSetPair> originalFrsp( _org->topFrspOriginal() );
	        BSONElement hintElt = _hint.firstElement();
    	    _currentQps.reset( new QueryPlanSet( _ns.c_str(), frsp, originalFrsp, _query,
                                                BSONObj(), true, &hintElt, _honorRecordedPlan,
                                                BSONObj(), BSONObj(), _bestGuessOnly,
                                                _mayYield ) );
            op = nextOpHandleEndOfClause();
            if ( !op->complete() ) {
             	return op;
            }
            _baseOp = op;
        }
        return op;
    }

    shared_ptr<QueryOp> MultiPlanScanner::nextOp() {
        if ( !_or ) {
            if ( _i == 0 ) {
                assertMayRunMore();
	         	++_i;
            }            
            return _currentQps->nextOp( *_baseOp );   
        }
        if ( _i == 0 ) {
            return nextOpBeginningClause();
        }
        shared_ptr<QueryOp> op = nextOpHandleEndOfClause();
        if ( !op->complete() ) {
            return op;   
        }
        if ( !op->stopRequested() && mayRunMore() ) {
            // Finished scanning the clause, but stop hasn't been requested.
            // Start scanning the next clause.
            _baseOp = op;
            return nextOpBeginningClause();
        }
        return op;
    }
    
    bool MultiPlanScanner::prepareToYield() {
        return _currentQps.get() ? _currentQps->prepareToYield() : true;
    }
    
    void MultiPlanScanner::recoverFromYield() {
        if ( _currentQps.get() ) {
            _currentQps->recoverFromYield();
        }
    }

    void MultiPlanScanner::clearRunner() {
        if ( _currentQps.get() ) {
            _currentQps->clearRunner();
        }    
    }
    
    int MultiPlanScanner::currentNPlans() const {
        return _currentQps.get() ? _currentQps->nPlans() : 0;
    }

    shared_ptr<Cursor> MultiPlanScanner::singleCursor() const {
        const QueryPlan *qp = singlePlan();
        if ( !qp ) {
            return shared_ptr<Cursor>();            
        }
        // If there is only one plan and it does not require an in memory
        // sort, we do not expect its cursor op to throw an exception and
        // so do not need a QueryOptimizerCursor to handle this case.
        return qp->newCursor();
    }

    const QueryPlan *MultiPlanScanner::singlePlan() const {
        if ( _or || _currentQps->nPlans() != 1 || _currentQps->firstPlan()->scanAndOrderRequired() || _currentQps->usingCachedPlan() ) {
            return 0;
        }
        return _currentQps->firstPlan().get();
    }

    bool MultiPlanScanner::uselessOr( const BSONElement &hint ) const {
        NamespaceDetails *nsd = nsdetails( _ns.c_str() );
        if ( !nsd ) {
            return true;
        }
        if ( !hint.eoo() ) {
            IndexDetails *id = parseHint( hint, nsd );
            if ( !id ) {
                return true;
            }
            return QueryUtilIndexed::uselessOr( *_org, nsd, nsd->idxNo( *id ) );
        }
        return QueryUtilIndexed::uselessOr( *_org, nsd, -1 );
    }
    
    MultiCursor::MultiCursor( const char *ns, const BSONObj &pattern, const BSONObj &order, shared_ptr<CursorOp> op, bool mayYield )
    : _mps( new MultiPlanScanner( ns, pattern, order, 0, true, BSONObj(), BSONObj(), !op.get(), mayYield ) ), _nscanned() {
        if ( op.get() ) {
            _op = op;
        }
        else {
            _op.reset( new NoOp() );
        }
        if ( _mps->mayRunMore() ) {
            nextClause();
            if ( !ok() ) {
                advance();
            }
        }
        else {
            _c.reset( new BasicCursor( DiskLoc() ) );
        }
    }    

    MultiCursor::MultiCursor( auto_ptr<MultiPlanScanner> mps, const shared_ptr<Cursor> &c, const shared_ptr<CoveredIndexMatcher> &matcher, const QueryOp &op, long long nscanned )
    : _op( new NoOp( op ) ), _c( c ), _mps( mps ), _matcher( matcher ), _nscanned( nscanned ) {
        _mps->setBestGuessOnly();
        _mps->mayYield( false ); // with a NoOp, there's no need to yield in QueryPlanSet
        if ( !ok() ) {
            // would have been advanced by UserQueryOp if possible
            advance();
        }
    }
    
    void MultiCursor::nextClause() {
        if ( _nscanned >= 0 && _c.get() ) {
            _nscanned += _c->nscanned();
        }
        shared_ptr<CursorOp> best = _mps->runOpOnce( *_op );
        if ( ! best->complete() )
            throw MsgAssertionException( best->exception() );
        _c = best->newCursor();
        _matcher = best->matcher( _c );
        _op = best;
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
                assert( false );
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
    
    bool isSimpleIdQuery( const BSONObj& query ) {
        BSONObjIterator i(query);
        
        if( !i.more() ) 
            return false;

        BSONElement e = i.next();

        if( i.more() ) 
            return false;

        if( strcmp("_id", e.fieldName()) != 0 ) 
            return false;
        
        if ( e.isSimpleType() ) // e.g. not something like { _id : { $gt : ...
            return true;

        if ( e.type() == Object )
            return e.Obj().firstElementFieldName()[0] != '$';

        return false;
    }

    shared_ptr<Cursor> bestGuessCursor( const char *ns, const BSONObj &query, const BSONObj &sort ) {
        if( !query.getField( "$or" ).eoo() ) {
            return shared_ptr<Cursor>( new MultiCursor( ns, query, sort ) );
        }
        else {
            auto_ptr<FieldRangeSetPair> frsp( new FieldRangeSetPair( ns, query, true ) );
            auto_ptr<FieldRangeSetPair> origFrsp( new FieldRangeSetPair( *frsp ) );

            QueryPlanSet qps( ns, frsp, origFrsp, query, sort, false );
            QueryPlanSet::QueryPlanPtr qpp = qps.getBestGuess();
            if( ! qpp.get() ) return shared_ptr<Cursor>();

            shared_ptr<Cursor> ret = qpp->newCursor();

            // If we don't already have a matcher, supply one.
            if ( !query.isEmpty() && ! ret->matcher() ) {
                shared_ptr<CoveredIndexMatcher> matcher( new CoveredIndexMatcher( query, ret->indexKeyPattern() ) );
                ret->setMatcher( matcher );
            }
            return ret;
        }
    }

    bool QueryUtilIndexed::indexUseful( const FieldRangeSetPair &frsp, NamespaceDetails *d, int idxNo, const BSONObj &order ) {
        DEV frsp.assertValidIndex( d, idxNo );
        BSONObj keyPattern = d->idx( idxNo ).keyPattern();
        if ( !frsp.matchPossibleForIndex( d, idxNo, keyPattern ) ) {
            // No matches are possible in the index so the index may be useful.
            return true;   
        }
        return d->idx( idxNo ).getSpec().suitability( frsp.simplifiedQueryForIndex( d, idxNo, keyPattern ), order ) != USELESS;
    }
    
    void QueryUtilIndexed::clearIndexesForPatterns( const FieldRangeSetPair &frsp, const BSONObj &order ) {
        SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
        NamespaceDetailsTransient& nsd = NamespaceDetailsTransient::get_inlock( frsp.ns() );
        nsd.registerIndexForPattern( frsp._singleKey.pattern( order ), BSONObj(), 0 );
        nsd.registerIndexForPattern( frsp._multiKey.pattern( order ), BSONObj(), 0 );
    }
    
    pair< BSONObj, long long > QueryUtilIndexed::bestIndexForPatterns( const FieldRangeSetPair &frsp, const BSONObj &order ) {
        SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
        NamespaceDetailsTransient& nsd = NamespaceDetailsTransient::get_inlock( frsp.ns() );
        // TODO Maybe it would make sense to return the index with the lowest
        // nscanned if there are two possibilities.
        if ( frsp._singleKey.matchPossible() ) {
            QueryPattern pattern = frsp._singleKey.pattern( order );
            BSONObj oldIdx = nsd.indexForPattern( pattern );
            if ( !oldIdx.isEmpty() ) {
                long long oldNScanned = nsd.nScannedForPattern( pattern );
                return make_pair( oldIdx, oldNScanned );
            }
        }
        if ( frsp._multiKey.matchPossible() ) {
            QueryPattern pattern = frsp._multiKey.pattern( order );
            BSONObj oldIdx = nsd.indexForPattern( pattern );
            if ( !oldIdx.isEmpty() ) {
                long long oldNScanned = nsd.nScannedForPattern( pattern );
                return make_pair( oldIdx, oldNScanned );
            }
        }
        return make_pair( BSONObj(), 0 );
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
