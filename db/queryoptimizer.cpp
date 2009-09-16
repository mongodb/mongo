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

#include "stdafx.h"

#include "db.h"
#include "btree.h"
#include "pdfile.h"
#include "queryoptimizer.h"
#include "cmdline.h"

namespace mongo {

    void checkTableScanAllowed( const char * ns ){
        if ( ! cmdLine.notablescan )
            return;
        
        if ( strstr( ns , ".system." ) ||
             strstr( ns , "local." ) )
            return;
        
        uassert( "table scans not allowed" , ! cmdLine.notablescan );
    }
    
    double elementDirection( const BSONElement &e ) {
        if ( e.isNumber() )
            return e.number();
        return 1;
    }
    
    QueryPlan::QueryPlan( 
        NamespaceDetails *_d, int _idxNo,
        const FieldRangeSet &fbs, const BSONObj &order, const BSONObj &startKey, const BSONObj &endKey ) :
    d(_d), idxNo(_idxNo),
    fbs_( fbs ),
    order_( order ),
    index_( 0 ),
    optimal_( false ),
    scanAndOrderRequired_( true ),
    exactKeyMatch_( false ),
    direction_( 0 ),
    endKeyInclusive_( endKey.isEmpty() ),
    unhelpful_( false ) {

        if ( !fbs_.matchPossible() ) {
            unhelpful_ = true;
            scanAndOrderRequired_ = false;
            return;
        }

        if( idxNo >= 0 ) {
            index_ = &d->indexes[idxNo];
        } else {
            // full table scan case
            if ( order_.isEmpty() || !strcmp( order_.firstElement().fieldName(), "$natural" ) )
                scanAndOrderRequired_ = false;
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
            exactIndexedQueryCount == fbs.query().nFields() ) {
            exactKeyMatch_ = true;
        }
        indexBounds_ = fbs.indexBounds( idxKey, direction_ );
        if ( !startKey.isEmpty() || !endKey.isEmpty() ) {
            BSONObj newStart, newEnd;
            if ( !startKey.isEmpty() )
                newStart = startKey;
            else
                newStart = indexBounds_[ 0 ].first;
            if ( !endKey.isEmpty() )
                newEnd = endKey;
            else
                newEnd = indexBounds_[ indexBounds_.size() - 1 ].second;
            BoundList newBounds;
            newBounds.push_back( make_pair( newStart, newEnd ) );
            indexBounds_ = newBounds;
        }
        if ( ( scanAndOrderRequired_ || order_.isEmpty() ) &&
            !fbs.range( idxKey.firstElement().fieldName() ).nontrivial() )
            unhelpful_ = true;
    }
    
    auto_ptr< Cursor > QueryPlan::newCursor( const DiskLoc &startLoc ) const {
        if ( !fbs_.matchPossible() ){
            checkTableScanAllowed( fbs_.ns() );
            return auto_ptr< Cursor >( new BasicCursor( DiskLoc() ) );
        }
        if ( !index_ ){
            checkTableScanAllowed( fbs_.ns() );
            return findTableScan( fbs_.ns(), order_, startLoc );
        }

        massert( "newCursor() with start location not implemented for indexed plans", startLoc.isNull() );
        
        if ( indexBounds_.size() < 2 ) {
            // we are sure to spec endKeyInclusive_
            return auto_ptr< Cursor >( new BtreeCursor( d, idxNo, *index_, indexBounds_[ 0 ].first, indexBounds_[ 0 ].second, endKeyInclusive_, direction_ >= 0 ? 1 : -1 ) );
        } else {
            return auto_ptr< Cursor >( new BtreeCursor( d, idxNo, *index_, indexBounds_, direction_ >= 0 ? 1 : -1 ) );
        }
    }
    
    auto_ptr< Cursor > QueryPlan::newReverseCursor() const {
        if ( !fbs_.matchPossible() )
            return auto_ptr< Cursor >( new BasicCursor( DiskLoc() ) );
        if ( !index_ ) {
            int orderSpec = order_.getIntField( "$natural" );
            if ( orderSpec == INT_MIN )
                orderSpec = 1;
            return findTableScan( fbs_.ns(), BSON( "$natural" << -orderSpec ) );
        }
        massert( "newReverseCursor() not implemented for indexed plans", false );
        return auto_ptr< Cursor >( 0 );
    }
    
    BSONObj QueryPlan::indexKey() const {
        if ( !index_ )
            return BSON( "$natural" << 1 );
        return index_->keyPattern();
    }
    
    void QueryPlan::registerSelf( long long nScanned ) const {
        if ( fbs_.matchPossible() )
            NamespaceDetailsTransient::get( ns() ).registerIndexForPattern( fbs_.pattern( order_ ), indexKey(), nScanned );  
    }
    
    QueryPlanSet::QueryPlanSet( const char *_ns, const BSONObj &query, const BSONObj &order, const BSONElement *hint, bool honorRecordedPlan, const BSONObj &min, const BSONObj &max ) :
    ns(_ns),
    fbs_( _ns, query ),
    mayRecordPlan_( true ),
    usingPrerecordedPlan_( false ),
    hint_( BSONObj() ),
    order_( order.getOwned() ),
    oldNScanned_( 0 ),
    honorRecordedPlan_( honorRecordedPlan ),
    min_( min.getOwned() ),
    max_( max.getOwned() ) {
        if ( hint && !hint->eoo() ) {
            BSONObjBuilder b;
            b.append( *hint );
            hint_ = b.obj();
        }
        init();
    }
    
    void QueryPlanSet::addHint( IndexDetails &id ) {
        if ( !min_.isEmpty() || !max_.isEmpty() ) {
            string errmsg;
            BSONObj keyPattern = id.keyPattern();
            // This reformats min_ and max_ to be used for index lookup.
            massert( errmsg, indexDetailsForRange( fbs_.ns(), errmsg, min_, max_, keyPattern ) );
        }
        NamespaceDetails *d = nsdetails(ns);
        plans_.push_back( PlanPtr( new QueryPlan( d, d->idxNo(id), fbs_, order_, min_, max_ ) ) );
    }
    
    void QueryPlanSet::init() {
        plans_.clear();
        mayRecordPlan_ = true;
        usingPrerecordedPlan_ = false;
        
        const char *ns = fbs_.ns();
        NamespaceDetails *d = nsdetails( ns );
        if ( !d || !fbs_.matchPossible() ) {
            // Table scan plan, when no matches are possible
            plans_.push_back( PlanPtr( new QueryPlan( d, -1, fbs_, order_ ) ) );
            return;
        }
        
        BSONElement hint = hint_.firstElement();
        if ( !hint.eoo() ) {
            mayRecordPlan_ = false;
            if( hint.type() == String ) {
                string hintstr = hint.valuestr();
                for (int i = 0; i < d->nIndexes; i++ ) {
                    IndexDetails& ii = d->indexes[i];
                    if ( ii.indexName() == hintstr ) {
                        addHint( ii );
                        return;
                    }
                }
            }
            else if( hint.type() == Object ) { 
                BSONObj hintobj = hint.embeddedObject();
                uassert( "bad hint", !hintobj.isEmpty() );
                if ( !strcmp( hintobj.firstElement().fieldName(), "$natural" ) ) {
                    massert( "natural order cannot be specified with $min/$max", min_.isEmpty() && max_.isEmpty() );
                    // Table scan plan
                    plans_.push_back( PlanPtr( new QueryPlan( d, -1, fbs_, order_ ) ) );
                    return;
                }
                for (int i = 0; i < d->nIndexes; i++ ) {
                    IndexDetails& ii = d->indexes[i];
                    if( ii.keyPattern().woCompare(hintobj) == 0 ) {
                        addHint( ii );
                        return;
                    }
                }
            }
            uassert( "bad hint", false );
        }
        
        if ( !min_.isEmpty() || !max_.isEmpty() ) {
            string errmsg;
            BSONObj keyPattern;
            IndexDetails *idx = indexDetailsForRange( ns, errmsg, min_, max_, keyPattern );
            massert( errmsg, idx );
            plans_.push_back( PlanPtr( new QueryPlan( d, d->idxNo(*idx), fbs_, order_, min_, max_ ) ) );
            return;
        }
        
        if ( honorRecordedPlan_ ) {
            BSONObj bestIndex = NamespaceDetailsTransient::get( ns ).indexForPattern( fbs_.pattern( order_ ) );
            if ( !bestIndex.isEmpty() ) {
                usingPrerecordedPlan_ = true;
                mayRecordPlan_ = false;
                oldNScanned_ = NamespaceDetailsTransient::get( ns ).nScannedForPattern( fbs_.pattern( order_ ) );
                if ( !strcmp( bestIndex.firstElement().fieldName(), "$natural" ) ) {
                    // Table scan plan
                    plans_.push_back( PlanPtr( new QueryPlan( d, -1, fbs_, order_ ) ) );
                    return;
                }
                for (int i = 0; i < d->nIndexes; i++ ) {
                    IndexDetails& ii = d->indexes[i];
                    if( ii.keyPattern().woCompare(bestIndex) == 0 ) {
                        plans_.push_back( PlanPtr( new QueryPlan( d, i, fbs_, order_ ) ) );
                        return;
                    }
                }
                massert( "Unable to locate previously recorded index", false );
            }
        }
        
        addOtherPlans( false );
    }
    
    void QueryPlanSet::addOtherPlans( bool checkFirst ) {
        const char *ns = fbs_.ns();
        NamespaceDetails *d = nsdetails( ns );
        if ( !d )
            return;

        // If table scan is optimal or natural order requested
        if ( !fbs_.matchPossible() || ( fbs_.nNontrivialRanges() == 0 && order_.isEmpty() ) ||
            ( !order_.isEmpty() && !strcmp( order_.firstElement().fieldName(), "$natural" ) ) ) {
            // Table scan plan
            addPlan( PlanPtr( new QueryPlan( d, -1, fbs_, order_ ) ), checkFirst );
            return;
        }
        
        PlanSet plans;
        for( int i = 0; i < d->nIndexes; ++i ) {
            PlanPtr p( new QueryPlan( d, i, fbs_, order_ ) );
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
        addPlan( PlanPtr( new QueryPlan( d, -1, fbs_, order_ ) ), checkFirst );
    }
    
    shared_ptr< QueryOp > QueryPlanSet::runOp( QueryOp &op ) {
        if ( usingPrerecordedPlan_ ) {
            Runner r( *this, op );
            shared_ptr< QueryOp > res = r.run();
            // plans_.size() > 1 if addOtherPlans was called in Runner::run().
            if ( res->complete() || plans_.size() > 1 )
                return res;
            NamespaceDetailsTransient::get( fbs_.ns() ).registerIndexForPattern( fbs_.pattern( order_ ), BSONObj(), 0 );
            init();
        }
        Runner r( *this, op );
        return r.run();
    }
    
    BSONObj QueryPlanSet::explain() const {
        vector< BSONObj > arr;
        for( PlanSet::const_iterator i = plans_.begin(); i != plans_.end(); ++i ) {
            auto_ptr< Cursor > c = (*i)->newCursor();
            arr.push_back( BSON( "cursor" << c->toString() << "startKey" << c->prettyStartKey() << "endKey" << c->prettyEndKey() ) );
        }
        BSONObjBuilder b;
        b.append( "allPlans", arr );
        return b.obj();
    }
    
    QueryPlanSet::Runner::Runner( QueryPlanSet &plans, QueryOp &op ) :
    op_( op ),
    plans_( plans ) {
    }
    
    shared_ptr< QueryOp > QueryPlanSet::Runner::run() {
        massert( "no plans", plans_.plans_.size() > 0 );
        
        if ( plans_.plans_.size() > 1 )
            log(1) << "  running multiple plans" << endl;

        vector< shared_ptr< QueryOp > > ops;
        for( PlanSet::iterator i = plans_.plans_.begin(); i != plans_.plans_.end(); ++i ) {
            shared_ptr< QueryOp > op( op_.clone() );
            op->setQueryPlan( i->get() );
            ops.push_back( op );
        }

        for( vector< shared_ptr< QueryOp > >::iterator i = ops.begin(); i != ops.end(); ++i ) {
            initOp( **i );
            if ( (*i)->complete() )
                return *i;
        }
        
        long long nScanned = 0;
        long long nScannedBackup = 0;
        while( 1 ) {
            ++nScanned;
            unsigned errCount = 0;
            bool first = true;
            for( vector< shared_ptr< QueryOp > >::iterator i = ops.begin(); i != ops.end(); ++i ) {
                QueryOp &op = **i;
                nextOp( op );
                if ( op.complete() ) {
                    if ( first )
                        nScanned += nScannedBackup;
                    if ( plans_.mayRecordPlan_ && op.mayRecordPlan() )
                        op.qp().registerSelf( nScanned );
                    return *i;
                }
                if ( op.error() )
                    ++errCount;
                first = false;
            }
            if ( errCount == ops.size() )
                break;
            if ( plans_.usingPrerecordedPlan_ && nScanned > plans_.oldNScanned_ * 10 ) {
                plans_.addOtherPlans( true );
                PlanSet::iterator i = plans_.plans_.begin();
                ++i;
                for( ; i != plans_.plans_.end(); ++i ) {
                    shared_ptr< QueryOp > op( op_.clone() );
                    op->setQueryPlan( i->get() );
                    ops.push_back( op );
                    initOp( *op );
                    if ( op->complete() )
                        return op;
                }                
                plans_.mayRecordPlan_ = true;
                plans_.usingPrerecordedPlan_ = false;
                nScannedBackup = nScanned;
                nScanned = 0;
            }
        }
        return ops[ 0 ];
    }
    
    void QueryPlanSet::Runner::initOp( QueryOp &op ) {
        try {
            op.init();
        } catch ( const std::exception &e ) {
            op.setExceptionMessage( e.what() );
        } catch ( ... ) {
            op.setExceptionMessage( "Caught unknown exception" );
        }        
    }

    void QueryPlanSet::Runner::nextOp( QueryOp &op ) {
        try {
            if ( !op.error() )
                op.next();
        } catch ( const std::exception &e ) {
            op.setExceptionMessage( e.what() );
        } catch ( ... ) {
            op.setExceptionMessage( "Caught unknown exception" );
        }        
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
        
        setClient( ns );
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
            for (int i = 0; i < d->nIndexes; i++ ) {
                IndexDetails& ii = d->indexes[i];
                if ( indexWorks( ii.keyPattern(), min.isEmpty() ? max : min, ret.first, ret.second ) ) {
                    id = &ii;
                    keyPattern = ii.keyPattern();
                    break;
                }
            }
            
        } else {            
            if ( !indexWorks( keyPattern, min.isEmpty() ? max : min, ret.first, ret.second ) ) {
                errmsg = "requested keyPattern does not match specified keys";
                return 0;
            }
            for (int i = 0; i < d->nIndexes; i++ ) {
                IndexDetails& ii = d->indexes[i];
                if( ii.keyPattern().woCompare(keyPattern) == 0 ) {
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
            errmsg = "no index found for specified keyPattern";
            return 0;
        }
        
        min = min.extractFieldsUnDotted( keyPattern );
        max = max.extractFieldsUnDotted( keyPattern );

        return id;
    }
        
} // namespace mongo
