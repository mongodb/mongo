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

#include "btree.h"
#include "pdfile.h"
#include "queryoptimizer.h"

namespace mongo {

    QueryPlan::QueryPlan( const FieldBoundSet &fbs, const BSONObj &order, const IndexDetails *index ) :
    fbs_( fbs ),
    order_( order ),
    index_( index ),
    optimal_( false ),
    scanAndOrderRequired_( true ),
    keyMatch_( false ),
    exactKeyMatch_( false ),
    direction_( 0 ),
    unhelpful_( false ) {
        // full table scan case
        if ( !index_ ) {
            if ( order_.isEmpty() || !strcmp( order_.firstElement().fieldName(), "$natural" ) )
                scanAndOrderRequired_ = false;
            return;
        }

        BSONObj idxKey = index->keyPattern();
        BSONObjIterator o( order );
        BSONObjIterator k( idxKey );
        if ( !o.more() )
            scanAndOrderRequired_ = false;
        while( o.more() ) {
            BSONElement oe = o.next();
            if ( oe.eoo() ) {
                scanAndOrderRequired_ = false;
                break;
            }
            if ( !k.more() )
                break;
            BSONElement ke;
            while( 1 ) {
                ke = k.next();
                if ( ke.eoo() )
                    goto doneCheckOrder;
                if ( strcmp( oe.fieldName(), ke.fieldName() ) == 0 )
                    break;
                if ( !fbs.bound( ke.fieldName() ).equality() )
                    goto doneCheckOrder;
            }
            int d = oe.number() == ke.number() ? 1 : -1;
            if ( direction_ == 0 )
                direction_ = d;
            else if ( direction_ != d )
                break;
        }
    doneCheckOrder:
        if ( scanAndOrderRequired_ )
            direction_ = 0;
        BSONObjIterator i( idxKey );
        int indexedQueryCount = 0;
        int exactIndexedQueryCount = 0;
        int optimalIndexedQueryCount = 0;
        bool stillOptimalIndexedQueryCount = true;
        set< string > orderFieldsUnindexed;
        order.getFieldNames( orderFieldsUnindexed );
        BSONObjBuilder startKeyBuilder;
        BSONObjBuilder endKeyBuilder;
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const FieldBound &fb = fbs.bound( e.fieldName() );
            int number = (int) e.number(); // returns 0.0 if not numeric
            bool forward = ( ( number >= 0 ? 1 : -1 ) * ( direction_ >= 0 ? 1 : -1 ) > 0 );
            startKeyBuilder.appendAs( forward ? fb.lower() : fb.upper(), "" );
            endKeyBuilder.appendAs( forward ? fb.upper() : fb.lower(), "" );
            if ( fb.nontrivial() )
                ++indexedQueryCount;
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
                BSONElement e = fb.upper();
                if ( !e.isNumber() && !e.mayEncapsulate() && e.type() != RegEx )
                    ++exactIndexedQueryCount;
            }
            orderFieldsUnindexed.erase( e.fieldName() );
        }
        if ( !scanAndOrderRequired_ &&
             ( optimalIndexedQueryCount == fbs.nNontrivialBounds() ) )
            optimal_ = true;
        if ( indexedQueryCount == fbs.nNontrivialBounds() &&
            orderFieldsUnindexed.size() == 0 ) {
            keyMatch_ = true;
            if ( exactIndexedQueryCount == fbs.nNontrivialBounds() )
                exactKeyMatch_ = true;
        }
        startKey_ = startKeyBuilder.obj();
        endKey_ = endKeyBuilder.obj();
        if ( !keyMatch_ &&
            ( scanAndOrderRequired_ || order_.isEmpty() ) &&
            !fbs.bound( idxKey.firstElement().fieldName() ).nontrivial() )
            unhelpful_ = true;
    }
    
    auto_ptr< Cursor > QueryPlan::newCursor() const {
        if ( !fbs_.matchPossible() )
            return auto_ptr< Cursor >( new BasicCursor( DiskLoc() ) );
        if ( !index_ )
            return findTableScan( fbs_.ns(), order_, 0 );
        //TODO This constructor should really take a const ref to the index details.
        return auto_ptr< Cursor >( new BtreeCursor( *const_cast< IndexDetails* >( index_ ), startKey_, endKey_, direction_ >= 0 ? 1 : -1 ) );
    }
    
    BSONObj QueryPlan::indexKey() const {
        if ( !index_ )
            return BSON( "$natural" << 1 );
        return index_->keyPattern();
    }
    
    QueryPlanSet::QueryPlanSet( const char *ns, const BSONObj &query, const BSONObj &order, const BSONElement *hint ) :
    fbs_( ns, query ),
    mayRecordPlan_( true ),
    usingPrerecordedPlan_( false ),
    hint_( emptyObj ),
    order_( order.copy() ) {
        if ( hint && !hint->eoo() ) {
            BSONObjBuilder b;
            b.append( *hint );
            hint_ = b.obj();
        }
        init();
    }
    
    void QueryPlanSet::init() {
        mayRecordPlan_ = true;
        usingPrerecordedPlan_ = false;
        
        const char *ns = fbs_.ns();
        NamespaceDetails *d = nsdetails( ns );
        if ( !d || !fbs_.matchPossible() ) {
            // Table scan plan, when no matches are possible
            plans_.push_back( PlanPtr( new QueryPlan( fbs_, order_ ) ) );
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
                        plans_.push_back( PlanPtr( new QueryPlan( fbs_, order_, &ii ) ) );
                        return;
                    }
                }
            }
            else if( hint.type() == Object ) { 
                BSONObj hintobj = hint.embeddedObject();
                uassert( "bad hint", !hintobj.isEmpty() );
                if ( !strcmp( hintobj.firstElement().fieldName(), "$natural" ) ) {
                    // Table scan plan
                    plans_.push_back( PlanPtr( new QueryPlan( fbs_, order_ ) ) );
                    return;
                }
                for (int i = 0; i < d->nIndexes; i++ ) {
                    IndexDetails& ii = d->indexes[i];
                    if( ii.keyPattern().woCompare(hintobj) == 0 ) {
                        plans_.push_back( PlanPtr( new QueryPlan( fbs_, order_, &ii ) ) );
                        return;
                    }
                }
            }
            uassert( "bad hint", false );
        }
        
        BSONObj bestIndex = indexForPattern( ns, fbs_.pattern() );
        if ( !bestIndex.isEmpty() ) {
            usingPrerecordedPlan_ = true;
            if ( !strcmp( bestIndex.firstElement().fieldName(), "$natural" ) ) {
                // Table scan plan
                plans_.push_back( PlanPtr( new QueryPlan( fbs_, order_ ) ) );
                return;
            }
            for (int i = 0; i < d->nIndexes; i++ ) {
                IndexDetails& ii = d->indexes[i];
                if( ii.keyPattern().woCompare(bestIndex) == 0 ) {
                    plans_.push_back( PlanPtr( new QueryPlan( fbs_, order_, &ii ) ) );
                    return;
                }
            }
            assert( false );
        }
        
        // Table scan plan
        plans_.push_back( PlanPtr( new QueryPlan( fbs_, order_ ) ) );

        // If table scan is optimal
        if ( fbs_.nNontrivialBounds() == 0 && order_.isEmpty() )
            return;
        
        // Only table scan can give natural order.
        if ( !order_.isEmpty() && !strcmp( order_.firstElement().fieldName(), "$natural" ) )
            return;
        
        PlanSet plans;
        for( int i = 0; i < d->nIndexes; ++i ) {
            PlanPtr p( new QueryPlan( fbs_, order_, &d->indexes[ i ] ) );
            if ( p->optimal() ) {
                plans_.push_back( p );
                return;
            } else if ( !p->unhelpful() ) {
                plans.push_back( p );
            }
        }
        for( PlanSet::iterator i = plans.begin(); i != plans.end(); ++i )
            plans_.push_back( *i );
    }
    
    shared_ptr< QueryOp > QueryPlanSet::runOp( QueryOp &op ) {
        if ( usingPrerecordedPlan_ ) {
            Runner r( *this, op );
            shared_ptr< QueryOp > res = r.run();
            if ( res->complete() )
                return res;
            registerIndexForPattern( fbs_.ns(), fbs_.pattern(), BSONObj() );
            init();
        }
        Runner r( *this, op );
        return r.run();
    }
    
    QueryPlanSet::Runner::Runner( QueryPlanSet &plans, QueryOp &op ) :
    op_( op ),
    plans_( plans ) {
    }
    
    shared_ptr< QueryOp > QueryPlanSet::Runner::run() {
        massert( "no plans", plans_.plans_.size() > 0 );
        
        vector< shared_ptr< QueryOp > > ops;
        for( PlanSet::iterator i = plans_.plans_.begin(); i != plans_.plans_.end(); ++i ) {
            shared_ptr< QueryOp > op( op_.clone() );
            op->setQueryPlan( i->get() );
            ops.push_back( op );
        }

        for( vector< shared_ptr< QueryOp > >::iterator i = ops.begin(); i != ops.end(); ++i )
            (*i)->init();
        
        while( 1 ) {
            unsigned errCount = 0;
            for( vector< shared_ptr< QueryOp > >::iterator i = ops.begin(); i != ops.end(); ++i ) {
                QueryOp &op = **i;
                try {
                    if ( !op.error() )
                        op.next();
                } catch ( const std::exception &e ) {
                    op.setExceptionMessage( e.what() );
                } catch ( ... ) {
                    op.setExceptionMessage( "Caught unknown exception" );
                }
                if ( op.complete() ) {
                    if ( plans_.mayRecordPlan_ && op.mayRecordPlan() )
                        op.qp().registerSelf();
                    return *i;
                }
                if ( op.error() )
                    ++errCount;
            }
            if ( errCount == ops.size() )
                break;
        }
        return ops[ 0 ];
    }

} // namespace mongo
