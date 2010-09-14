/* queryoptimizer.cpp */

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
#include <queue>

//#define DEBUGQO(x) cout << x << endl;
#define DEBUGQO(x)

namespace mongo {

    void checkTableScanAllowed( const char * ns ){
        if ( ! cmdLine.notablescan )
            return;
        
        if ( strstr( ns , ".system." ) ||
             strstr( ns , "local." ) )
            return;
        
        if ( ! nsdetails( ns ) )
            return;

        uassert( 10111 ,  (string)"table scans not allowed:" + ns , ! cmdLine.notablescan );
    }

    double elementDirection( const BSONElement &e ) {
        if ( e.isNumber() )
            return e.number();
        return 1;
    }
    
    QueryPlan::QueryPlan( 
        NamespaceDetails *_d, int _idxNo,
        const FieldRangeSet &fbs, const FieldRangeSet &originalFrs, const BSONObj &originalQuery, const BSONObj &order, const BSONObj &startKey, const BSONObj &endKey , string special ) :
    d(_d), idxNo(_idxNo),
    fbs_( fbs ),
    _originalQuery( originalQuery ),
    order_( order ),
    index_( 0 ),
    optimal_( false ),
    scanAndOrderRequired_( true ),
    exactKeyMatch_( false ),
    direction_( 0 ),
    endKeyInclusive_( endKey.isEmpty() ),
    unhelpful_( false ),
    _special( special ),
    _type(0),
    _startOrEndSpec( !startKey.isEmpty() || !endKey.isEmpty() ){

        if ( !fbs_.matchPossible() ) {
            unhelpful_ = true;
            scanAndOrderRequired_ = false;
            return;
        }

        if( idxNo >= 0 ) {
            index_ = &d->idx(idxNo);
        } else {
            // full table scan case
            if ( order_.isEmpty() || !strcmp( order_.firstElement().fieldName(), "$natural" ) )
                scanAndOrderRequired_ = false;
            return;
        }

        if ( _special.size() ){
            optimal_ = true;
            _type  = index_->getSpec().getType();
            massert( 13040 , (string)"no type for special: " + _special , _type );
            // hopefully safe to use original query in these contexts - don't think we can mix special with $or clause separation yet
            scanAndOrderRequired_ = _type->scanAndOrderRequired( _originalQuery , order );
            return;
        }

        BSONObj idxKey = index_->keyPattern();
        BSONObjIterator o( order );
        BSONObjIterator k( idxKey );
        if ( !o.moreWithEOO() )
            scanAndOrderRequired_ = false;
        while( o.moreWithEOO() ) {
            BSONElement oe = o.next();
            if ( oe.eoo() ) {
                scanAndOrderRequired_ = false;
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
                if ( !fbs.range( ke.fieldName() ).equality() )
                    goto doneCheckOrder;
            }
            int d = elementDirection( oe ) == elementDirection( ke ) ? 1 : -1;
            if ( direction_ == 0 )
                direction_ = d;
            else if ( direction_ != d )
                break;
        }
    doneCheckOrder:
        if ( scanAndOrderRequired_ )
            direction_ = 0;
        BSONObjIterator i( idxKey );
        int exactIndexedQueryCount = 0;
        int optimalIndexedQueryCount = 0;
        bool stillOptimalIndexedQueryCount = true;
        set< string > orderFieldsUnindexed;
        order.getFieldNames( orderFieldsUnindexed );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const FieldRange &fb = fbs.range( e.fieldName() );
            if ( stillOptimalIndexedQueryCount ) {
                if ( fb.nontrivial() )
                    ++optimalIndexedQueryCount;
                if ( !fb.equality() )
                    stillOptimalIndexedQueryCount = false;
            } else {
                if ( fb.nontrivial() )
                    optimalIndexedQueryCount = -1;
            }
            if ( fb.equality() ) {
                BSONElement e = fb.max();
                if ( !e.isNumber() && !e.mayEncapsulate() && e.type() != RegEx )
                    ++exactIndexedQueryCount;
            }
            orderFieldsUnindexed.erase( e.fieldName() );
        }
        if ( !scanAndOrderRequired_ &&
             ( optimalIndexedQueryCount == fbs.nNontrivialRanges() ) )
            optimal_ = true;
        if ( exactIndexedQueryCount == fbs.nNontrivialRanges() &&
            orderFieldsUnindexed.size() == 0 &&
            exactIndexedQueryCount == index_->keyPattern().nFields() &&
            exactIndexedQueryCount == _originalQuery.nFields() ) {
            exactKeyMatch_ = true;
        }
        _frv.reset( new FieldRangeVector( fbs, idxKey, direction_ ) );
        _originalFrv.reset( new FieldRangeVector( originalFrs, idxKey, direction_ ) );
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

        if ( ( scanAndOrderRequired_ || order_.isEmpty() ) &&
            !fbs.range( idxKey.firstElement().fieldName() ).nontrivial() ) {
            unhelpful_ = true;
        }
    }
    
    shared_ptr<Cursor> QueryPlan::newCursor( const DiskLoc &startLoc , int numWanted ) const {

        if ( _type ) {
            // hopefully safe to use original query in these contexts - don't think we can mix type with $or clause separation yet   
            return _type->newCursor( _originalQuery , order_ , numWanted );
        }
        
        if ( !fbs_.matchPossible() ){
            if ( fbs_.nNontrivialRanges() )
                checkTableScanAllowed( fbs_.ns() );
            return shared_ptr<Cursor>( new BasicCursor( DiskLoc() ) );
        }
        if ( !index_ ){
            if ( fbs_.nNontrivialRanges() )
                checkTableScanAllowed( fbs_.ns() );
            return findTableScan( fbs_.ns(), order_, startLoc );
        }

        massert( 10363 ,  "newCursor() with start location not implemented for indexed plans", startLoc.isNull() );
        
        if ( _startOrEndSpec ) {
            // we are sure to spec endKeyInclusive_
            return shared_ptr<Cursor>( new BtreeCursor( d, idxNo, *index_, _startKey, _endKey, endKeyInclusive_, direction_ >= 0 ? 1 : -1 ) );
        } else if ( index_->getSpec().getType() ) {
            return shared_ptr<Cursor>( new BtreeCursor( d, idxNo, *index_, _frv->startKey(), _frv->endKey(), true, direction_ >= 0 ? 1 : -1 ) );            
        } else {
            return shared_ptr<Cursor>( new BtreeCursor( d, idxNo, *index_, _frv, direction_ >= 0 ? 1 : -1 ) );
        }
    }
    
    shared_ptr<Cursor> QueryPlan::newReverseCursor() const {
        if ( !fbs_.matchPossible() )
            return shared_ptr<Cursor>( new BasicCursor( DiskLoc() ) );
        if ( !index_ ) {
            int orderSpec = order_.getIntField( "$natural" );
            if ( orderSpec == INT_MIN )
                orderSpec = 1;
            return findTableScan( fbs_.ns(), BSON( "$natural" << -orderSpec ) );
        }
        massert( 10364 ,  "newReverseCursor() not implemented for indexed plans", false );
        return shared_ptr<Cursor>();
    }
    
    BSONObj QueryPlan::indexKey() const {
        if ( !index_ )
            return BSON( "$natural" << 1 );
        return index_->keyPattern();
    }
    
    void QueryPlan::registerSelf( long long nScanned ) const {
        if ( fbs_.matchPossible() ) {
            scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
            NamespaceDetailsTransient::get_inlock( ns() ).registerIndexForPattern( fbs_.pattern( order_ ), indexKey(), nScanned );  
        }
    }
    
    QueryPlanSet::QueryPlanSet( const char *_ns, auto_ptr< FieldRangeSet > frs, auto_ptr< FieldRangeSet > originalFrs, const BSONObj &originalQuery, const BSONObj &order, const BSONElement *hint, bool honorRecordedPlan, const BSONObj &min, const BSONObj &max, bool bestGuessOnly, bool mayYield ) :
    ns(_ns),
    _originalQuery( originalQuery ),
    fbs_( frs ),
    _originalFrs( originalFrs ),
    mayRecordPlan_( true ),
    usingPrerecordedPlan_( false ),
    hint_( BSONObj() ),
    order_( order.getOwned() ),
    oldNScanned_( 0 ),
    honorRecordedPlan_( honorRecordedPlan ),
    min_( min.getOwned() ),
    max_( max.getOwned() ),
    _bestGuessOnly( bestGuessOnly ),
    _mayYield( mayYield ),
    _yieldSometimesTracker( 256, 20 ){
        if ( hint && !hint->eoo() ) {
            hint_ = hint->wrap();
        }
        init();
    }
    
    void QueryPlanSet::addHint( IndexDetails &id ) {
        if ( !min_.isEmpty() || !max_.isEmpty() ) {
            string errmsg;
            BSONObj keyPattern = id.keyPattern();
            // This reformats min_ and max_ to be used for index lookup.
            massert( 10365 ,  errmsg, indexDetailsForRange( fbs_->ns(), errmsg, min_, max_, keyPattern ) );
        }
        NamespaceDetails *d = nsdetails(ns);
        plans_.push_back( PlanPtr( new QueryPlan( d, d->idxNo(id), *fbs_, *_originalFrs, _originalQuery, order_, min_, max_ ) ) );
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
            if ( !strcmp( hintobj.firstElement().fieldName(), "$natural" ) ) {
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
        plans_.clear();
        mayRecordPlan_ = true;
        usingPrerecordedPlan_ = false;
        
        const char *ns = fbs_->ns();
        NamespaceDetails *d = nsdetails( ns );
        if ( !d || !fbs_->matchPossible() ) {
            // Table scan plan, when no matches are possible
            plans_.push_back( PlanPtr( new QueryPlan( d, -1, *fbs_, *_originalFrs, _originalQuery, order_ ) ) );
            return;
        }
        
        BSONElement hint = hint_.firstElement();
        if ( !hint.eoo() ) {
            mayRecordPlan_ = false;
            IndexDetails *id = parseHint( hint, d );
            if ( id ) {
                addHint( *id );
            } else {
                massert( 10366 ,  "natural order cannot be specified with $min/$max", min_.isEmpty() && max_.isEmpty() );
                // Table scan plan
                plans_.push_back( PlanPtr( new QueryPlan( d, -1, *fbs_, *_originalFrs, _originalQuery, order_ ) ) );                
            }
            return;
        }
        
        if ( !min_.isEmpty() || !max_.isEmpty() ) {
            string errmsg;
            BSONObj keyPattern;
            IndexDetails *idx = indexDetailsForRange( ns, errmsg, min_, max_, keyPattern );
            massert( 10367 ,  errmsg, idx );
            plans_.push_back( PlanPtr( new QueryPlan( d, d->idxNo(*idx), *fbs_, *_originalFrs, _originalQuery, order_, min_, max_ ) ) );
            return;
        }

        if ( isSimpleIdQuery( _originalQuery ) ){
            int idx = d->findIdIndex();
            if ( idx >= 0 ){
                usingPrerecordedPlan_ = true;
                mayRecordPlan_ = false;
                plans_.push_back( PlanPtr( new QueryPlan( d , idx , *fbs_ , *fbs_ , _originalQuery, order_ ) ) );
                return;
            }
        }

        if ( _originalQuery.isEmpty() && order_.isEmpty() ){
            plans_.push_back( PlanPtr( new QueryPlan( d, -1, *fbs_, *_originalFrs, _originalQuery, order_ ) ) );
            return;
        }

        DEBUGQO( "\t special : " << fbs_->getSpecial() );
        if ( fbs_->getSpecial().size() ){
            _special = fbs_->getSpecial();
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                int j = i.pos();
                IndexDetails& ii = i.next();
                const IndexSpec& spec = ii.getSpec();
                if ( spec.getTypeName() == _special && spec.suitability( _originalQuery , order_ ) ){
                    usingPrerecordedPlan_ = true;
                    mayRecordPlan_ = false;
                    plans_.push_back( PlanPtr( new QueryPlan( d , j , *fbs_ , *fbs_ , _originalQuery, order_ , 
                                                              BSONObj() , BSONObj() , _special ) ) );
                    return;
                }
            }
            uassert( 13038 , (string)"can't find special index: " + _special + " for: " + _originalQuery.toString() , 0 );
        }

        if ( honorRecordedPlan_ ) {
            scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
            NamespaceDetailsTransient& nsd = NamespaceDetailsTransient::get_inlock( ns );
            BSONObj bestIndex = nsd.indexForPattern( fbs_->pattern( order_ ) );
            if ( !bestIndex.isEmpty() ) {
                PlanPtr p;
                oldNScanned_ = nsd.nScannedForPattern( fbs_->pattern( order_ ) );
                if ( !strcmp( bestIndex.firstElement().fieldName(), "$natural" ) ) {
                    // Table scan plan
                    p.reset( new QueryPlan( d, -1, *fbs_, *_originalFrs, _originalQuery, order_ ) );
                }

                NamespaceDetails::IndexIterator i = d->ii();
                while( i.more() ) {
                    int j = i.pos();
                    IndexDetails& ii = i.next();
                    if( ii.keyPattern().woCompare(bestIndex) == 0 ) {
                        p.reset( new QueryPlan( d, j, *fbs_, *_originalFrs, _originalQuery, order_ ) );
                    }
                }

                massert( 10368 ,  "Unable to locate previously recorded index", p.get() );
                if ( !( _bestGuessOnly && p->scanAndOrderRequired() ) ) {
                    usingPrerecordedPlan_ = true;
                    mayRecordPlan_ = false;
                    plans_.push_back( p );
                    return;
                }
            }
        }
        
        addOtherPlans( false );
    }
    
    void QueryPlanSet::addOtherPlans( bool checkFirst ) {
        const char *ns = fbs_->ns();
        NamespaceDetails *d = nsdetails( ns );
        if ( !d )
            return;

        // If table scan is optimal or natural order requested or tailable cursor requested
        if ( !fbs_->matchPossible() || ( fbs_->nNontrivialRanges() == 0 && order_.isEmpty() ) ||
            ( !order_.isEmpty() && !strcmp( order_.firstElement().fieldName(), "$natural" ) ) ) {
            // Table scan plan
            addPlan( PlanPtr( new QueryPlan( d, -1, *fbs_, *_originalFrs, _originalQuery, order_ ) ), checkFirst );
            return;
        }
        
        bool normalQuery = hint_.isEmpty() && min_.isEmpty() && max_.isEmpty();

        PlanSet plans;
        for( int i = 0; i < d->nIndexes; ++i ) {
            IndexDetails& id = d->idx(i);
            const IndexSpec& spec = id.getSpec();
            IndexSuitability suitability = HELPFUL;
            if ( normalQuery ){
                suitability = spec.suitability( fbs_->simplifiedQuery() , order_ );
                if ( suitability == USELESS )
                    continue;
            }

            PlanPtr p( new QueryPlan( d, i, *fbs_, *_originalFrs, _originalQuery, order_ ) );
            if ( p->optimal() ) {
                addPlan( p, checkFirst );
                return;
            } else if ( !p->unhelpful() ) {
                plans.push_back( p );
            }
        }
        for( PlanSet::iterator i = plans.begin(); i != plans.end(); ++i )
            addPlan( *i, checkFirst );

        // Table scan plan
        addPlan( PlanPtr( new QueryPlan( d, -1, *fbs_, *_originalFrs, _originalQuery, order_ ) ), checkFirst );
    }
    
    shared_ptr< QueryOp > QueryPlanSet::runOp( QueryOp &op ) {
        if ( usingPrerecordedPlan_ ) {
            Runner r( *this, op );
            shared_ptr< QueryOp > res = r.run();
            // plans_.size() > 1 if addOtherPlans was called in Runner::run().
            if ( _bestGuessOnly || res->complete() || plans_.size() > 1 )
                return res;
            {
                scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
                NamespaceDetailsTransient::get_inlock( fbs_->ns() ).registerIndexForPattern( fbs_->pattern( order_ ), BSONObj(), 0 );
            }
            init();
        }
        Runner r( *this, op );
        return r.run();
    }
    
    BSONObj QueryPlanSet::explain() const {
        vector< BSONObj > arr;
        for( PlanSet::const_iterator i = plans_.begin(); i != plans_.end(); ++i ) {
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

    QueryPlanSet::PlanPtr QueryPlanSet::getBestGuess() const {
        assert( plans_.size() );
        if ( plans_[ 0 ]->scanAndOrderRequired() ){
            for ( unsigned i=1; i<plans_.size(); i++ ){
                if ( ! plans_[i]->scanAndOrderRequired() )
                    return plans_[i];
            }
            
            stringstream ss;
            ss << "best guess plan requested, but scan and order required:";
            ss << " query: " << fbs_->simplifiedQuery();
            ss << " order: " << order_;
            ss << " choices: ";
            for ( unsigned i=0; i<plans_.size(); i++ ){
                ss << plans_[i]->indexKey() << " ";
            }

            string s = ss.str();
            msgassertedNoTrace( 13284, s.c_str() );
        }
        return plans_[0];
    }
    
    QueryPlanSet::Runner::Runner( QueryPlanSet &plans, QueryOp &op ) :
    op_( op ),
    plans_( plans ) {
    }
    
    void QueryPlanSet::Runner::mayYield( const vector< shared_ptr< QueryOp > > &ops ) {
        if ( plans_._mayYield ) {
            if ( plans_._yieldSometimesTracker.ping() ) {
                int micros = ClientCursor::yieldSuggest();
                if ( micros > 0 ) {
                    for( vector< shared_ptr< QueryOp > >::const_iterator i = ops.begin(); i != ops.end(); ++i ) {
                        if ( !prepareToYield( **i ) ) {
                            return;
                        }
                    }
                    ClientCursor::staticYield( micros );
                    for( vector< shared_ptr< QueryOp > >::const_iterator i = ops.begin(); i != ops.end(); ++i ) {
                        recoverFromYield( **i );
                    }                        
                }
            }
        }        
    }
    
    struct OpHolder {
        OpHolder( const shared_ptr< QueryOp > &op ) : _op( op ), _offset() {}
        shared_ptr< QueryOp > _op;
        long long _offset;
        bool operator<( const OpHolder &other ) const {
            return _op->nscanned() + _offset > other._op->nscanned() + other._offset;
        }
    };
    
    shared_ptr< QueryOp > QueryPlanSet::Runner::run() {
        massert( 10369 ,  "no plans", plans_.plans_.size() > 0 );
        
        vector< shared_ptr< QueryOp > > ops;
        if ( plans_._bestGuessOnly ) {
            shared_ptr< QueryOp > op( op_.createChild() );
            op->setQueryPlan( plans_.getBestGuess().get() );
            ops.push_back( op );            
        } else {
            if ( plans_.plans_.size() > 1 )
                log(1) << "  running multiple plans" << endl;            
            for( PlanSet::iterator i = plans_.plans_.begin(); i != plans_.plans_.end(); ++i ) {
                shared_ptr< QueryOp > op( op_.createChild() );
                op->setQueryPlan( i->get() );
                ops.push_back( op );
            }
        }

        for( vector< shared_ptr< QueryOp > >::iterator i = ops.begin(); i != ops.end(); ++i ) {
            initOp( **i );
            if ( (*i)->complete() )
                return *i;
        }
        
        std::priority_queue< OpHolder > queue;
        for( vector< shared_ptr< QueryOp > >::iterator i = ops.begin(); i != ops.end(); ++i ) {
            queue.push( *i );
        }
        
        while( !queue.empty() ) {
            mayYield( ops );
            OpHolder holder = queue.top();
            queue.pop();
            QueryOp &op = *holder._op;
            nextOp( op );
            if ( op.complete() ) {
                if ( plans_.mayRecordPlan_ && op.mayRecordPlan() ) {
                    op.qp().registerSelf( op.nscanned() );
                }
                return holder._op;
            }
            if ( op.error() ) {
                continue;
            }
            queue.push( holder );
            if ( !plans_._bestGuessOnly && plans_.usingPrerecordedPlan_ && op.nscanned() > plans_.oldNScanned_ * 10 && plans_._special.empty() ) {
                holder._offset = -op.nscanned();
                plans_.addOtherPlans( true );
                PlanSet::iterator i = plans_.plans_.begin();
                ++i;
                for( ; i != plans_.plans_.end(); ++i ) {
                    shared_ptr< QueryOp > op( op_.createChild() );
                    op->setQueryPlan( i->get() );
                    ops.push_back( op );
                    initOp( *op );
                    if ( op->complete() )
                        return op;
                    queue.push( op );
                }                
                plans_.mayRecordPlan_ = true;
                plans_.usingPrerecordedPlan_ = false;
            }            
        }
        return ops[ 0 ];
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

    bool QueryPlanSet::Runner::prepareToYield( QueryOp &op ) {
        GUARD_OP_EXCEPTION( op,
                           if ( op.error() ) {
                               return true;
                           } else {
                               return op.prepareToYield();
                           } );
        return true;
    }

    void QueryPlanSet::Runner::recoverFromYield( QueryOp &op ) {
        GUARD_OP_EXCEPTION( op, if ( !op.error() ) { op.recoverFromYield(); } );
    }
    
    
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
    _fros( ns, _query ),
    _i(),
    _honorRecordedPlan( honorRecordedPlan ),
    _bestGuessOnly( bestGuessOnly ),
    _hint( ( hint && !hint->eoo() ) ? hint->wrap() : BSONObj() ),
    _mayYield( mayYield ),
    _tableScanned()
    {
        if ( !order.isEmpty() || !min.isEmpty() || !max.isEmpty() || !_fros.getSpecial().empty() ) {
            _or = false;
        }
        if ( _or && uselessOr( _hint.firstElement() ) ) {
            _or = false;
        }
        // if _or == false, don't use or clauses for index selection
        if ( !_or ) {
            auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns, _query ) );
            auto_ptr< FieldRangeSet > oldFrs( new FieldRangeSet( *frs ) );
            _currentQps.reset( new QueryPlanSet( ns, frs, oldFrs, _query, order, hint, honorRecordedPlan, min, max, _bestGuessOnly, _mayYield ) );
        } else {
            BSONElement e = _query.getField( "$or" );
            massert( 13268, "invalid $or spec", e.type() == Array && e.embeddedObject().nFields() > 0 );
        }
    }

    shared_ptr< QueryOp > MultiPlanScanner::runOpOnce( QueryOp &op ) {
        massert( 13271, "can't run more ops", mayRunMore() );
        if ( !_or ) {
            ++_i;
            return _currentQps->runOp( op );
        }
        ++_i;
        auto_ptr< FieldRangeSet > frs( _fros.topFrs() );
        auto_ptr< FieldRangeSet > originalFrs( _fros.topFrsOriginal() );
        BSONElement hintElt = _hint.firstElement();
        _currentQps.reset( new QueryPlanSet( _ns, frs, originalFrs, _query, BSONObj(), &hintElt, _honorRecordedPlan, BSONObj(), BSONObj(), _bestGuessOnly, _mayYield ) );
        shared_ptr< QueryOp > ret( _currentQps->runOp( op ) );
        if ( ret->qp().willScanTable() ) {
            _tableScanned = true;
        }
        _fros.popOrClause();
        return ret;
    }
    
    shared_ptr< QueryOp > MultiPlanScanner::runOp( QueryOp &op ) {
        shared_ptr< QueryOp > ret = runOpOnce( op );
        while( !ret->stopRequested() && mayRunMore() ) {
            ret = runOpOnce( *ret );
        }
        return ret;
    }
    
    bool MultiPlanScanner::uselessOr( const BSONElement &hint ) const {
        NamespaceDetails *nsd = nsdetails( _ns );
        if ( !nsd ) {
            return true;
        }
        IndexDetails *id = 0;
        if ( !hint.eoo() ) {
            IndexDetails *id = parseHint( hint, nsd );
            if ( !id ) {
                return true;
            }
        }
        vector< BSONObj > ret;
        _fros.allClausesSimplified( ret );
        for( vector< BSONObj >::const_iterator i = ret.begin(); i != ret.end(); ++i ) {
            if ( id ) {
                if ( id->getSpec().suitability( *i, BSONObj() ) == USELESS ) {
                    return true;
                }
            } else {
                bool useful = false;
                NamespaceDetails::IndexIterator j = nsd->ii();
                while( j.more() ) {
                    IndexDetails &id = j.next();
                    if ( id.getSpec().suitability( *i, BSONObj() ) != USELESS ) {
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
    
    pair< int, int > keyAudit( const BSONObj &min, const BSONObj &max ) {
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

    pair< int, int > flexibleKeyAudit( const BSONObj &min, const BSONObj &max ) {
        if ( min.isEmpty() || max.isEmpty() ) {
            return make_pair( 1, -1 );
        } else {
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
        
        pair< int, int > ret = flexibleKeyAudit( min, max );
        if ( ret == make_pair( -1, -1 ) ) {
            errmsg = "min and max keys do not share pattern";
            return 0;
        }
        if ( keyPattern.isEmpty() ) {
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                IndexDetails& ii = i.next();
                if ( indexWorks( ii.keyPattern(), min.isEmpty() ? max : min, ret.first, ret.second ) ) {
                    if ( ii.getSpec().getType() == 0 ){
                        id = &ii;
                        keyPattern = ii.keyPattern();
                        break;
                    }
                }
            }
            
        } else {            
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
                     ii.isIdIndex() ){
                    id = &ii;
                    break;
                }
                     
            }
        }

        if ( min.isEmpty() ) {
            min = extremeKeyForIndex( keyPattern, -1 );
        } else if ( max.isEmpty() ) {
            max = extremeKeyForIndex( keyPattern, 1 );
        }
                
        if ( !id ) {
            errmsg = (string)"no index found for specified keyPattern: " + keyPattern.toString();
            return 0;
        }
        
        min = min.extractFieldsUnDotted( keyPattern );
        max = max.extractFieldsUnDotted( keyPattern );

        return id;
    }
        
} // namespace mongo
