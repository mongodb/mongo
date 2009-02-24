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

    FieldBound::FieldBound( const BSONElement &e ) :
    lower_( minKey.firstElement() ),
    upper_( maxKey.firstElement() ) {
        if ( e.eoo() )
            return;
        if ( e.type() == RegEx ) {
            const char *r = e.simpleRegex();
            if ( r ) {
                lower_ = addObj( BSON( "" << r ) ).firstElement();
                upper_ = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
            }            
            return;
        }
        switch( e.getGtLtOp() ) {
            case JSMatcher::Equality:
                lower_ = e;
                upper_ = e;   
                break;
            case JSMatcher::LT:
            case JSMatcher::LTE:
                upper_ = e;
                break;
            case JSMatcher::GT:
            case JSMatcher::GTE:
                lower_ = e;
                break;
            case JSMatcher::opIN: {
                massert( "$in requires array", e.type() == Array );
                BSONElement max = minKey.firstElement();
                BSONElement min = maxKey.firstElement();
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    if ( max.woCompare( f, false ) < 0 )
                        max = f;
                    if ( min.woCompare( f, false ) > 0 )
                        min = f;
                }
                lower_ = min;
                upper_ = max;
            }
            default:
                break;
        }
    }
    
    FieldBound &FieldBound::operator&=( const FieldBound &other ) {
        if ( other.upper_.woCompare( upper_, false ) < 0 )
            upper_ = other.upper_;
        if ( other.lower_.woCompare( lower_, false ) > 0 )
            lower_ = other.lower_;
        for( vector< BSONObj >::const_iterator i = other.objData_.begin(); i != other.objData_.end(); ++i )
            objData_.push_back( *i );
        return *this;
    }
    
    string FieldBound::simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }    
    
    BSONObj FieldBound::addObj( const BSONObj &o ) {
        objData_.push_back( o );
        return o;
    }
    
    FieldBoundSet::FieldBoundSet( const char *ns, const BSONObj &query ) :
    ns_( ns ),
    query_( query.copy() ) {
        BSONObjIterator i( query_ );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( getGtLtOp( e ) == JSMatcher::Equality ) {
                bounds_[ e.fieldName() ] &= FieldBound( e );
            }
            else {
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    bounds_[ e.fieldName() ] &= FieldBound( f );
                }                
            }
        }
    }
    
    FieldBound *FieldBoundSet::trivialBound_ = 0;
    FieldBound &FieldBoundSet::trivialBound() {
        if ( trivialBound_ == 0 )
            trivialBound_ = new FieldBound();
        return *trivialBound_;
    }
    
    BSONObj FieldBoundSet::simplifiedQuery() const {
        BSONObjBuilder b;
        for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i ) {
            if ( i->second.equality() )
                b.appendAs( i->second.lower(), i->first.c_str() );
            else if ( i->second.nontrivial() ) {
                BSONObjBuilder c;
                if ( i->second.lower().type() != MinKey )
                    c.appendAs( i->second.lower(), "$gte" );
                if ( i->second.upper().type() != MaxKey )
                    c.appendAs( i->second.upper(), "$lte" );
                b.append( i->first.c_str(), c.done() );
            }
        }
        return b.obj();
    }
    
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
        return index_->keyPattern();
    }
    
    QueryPlanSet::QueryPlanSet( const char *ns, const BSONObj &query, const BSONObj &order, const BSONElement *hint ) :
    fbs_( ns, query ) {
        NamespaceDetails *d = nsdetails( ns );
        if ( !d || !fbs_.matchPossible() ) {
            // Table scan plan only
            plans_.push_back( PlanPtr( new QueryPlan( fbs_, order ) ) );
            return;
        }
        
        if ( hint && !hint->eoo() ) {
            if( hint->type() == String ) {
                string hintstr = hint->valuestr();
                for (int i = 0; i < d->nIndexes; i++ ) {
                    IndexDetails& ii = d->indexes[i];
                    if ( ii.indexName() == hintstr ) {
                        plans_.push_back( PlanPtr( new QueryPlan( fbs_, order, &ii ) ) );
                        return;
                    }
                }
            }
            else if( hint->type() == Object ) { 
                BSONObj hintobj = hint->embeddedObject();
                uassert( "bad hint", !hintobj.isEmpty() );
                if ( !strcmp( hintobj.firstElement().fieldName(), "$natural" ) ) {
                    // Table scan plan
                    plans_.push_back( PlanPtr( new QueryPlan( fbs_, order ) ) );
                    return;
                }
                for (int i = 0; i < d->nIndexes; i++ ) {
                    IndexDetails& ii = d->indexes[i];
                    if( ii.keyPattern().woCompare(hintobj) == 0 ) {
                        plans_.push_back( PlanPtr( new QueryPlan( fbs_, order, &ii ) ) );
                        return;
                    }
                }
            }
            uassert( "bad hint", false );
        }
        
        // Table scan plan
        plans_.push_back( PlanPtr( new QueryPlan( fbs_, order ) ) );

        // If table scan is optimal
        if ( fbs_.nNontrivialBounds() == 0 && order.isEmpty() )
            return;
        
        // Only table scan can give natural order.
        if ( !order.isEmpty() && !strcmp( order.firstElement().fieldName(), "$natural" ) )
            return;
        
        PlanSet plans;
        for( int i = 0; i < d->nIndexes; ++i ) {
            PlanPtr p( new QueryPlan( fbs_, order, &d->indexes[ i ] ) );
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
        RunnerSet s( *this, op );
        return s.run();
    }
    
    QueryPlanSet::RunnerSet::RunnerSet( QueryPlanSet &plans, QueryOp &op ) :
    op_( op ),
    plans_( plans ),
    startBarrier_( plans_.nPlans() ),
    firstDone_( false ) {
    }
    
    shared_ptr< QueryOp > QueryPlanSet::RunnerSet::run() {
        massert( "no plans", plans_.plans_.size() > 0 );
        if ( plans_.plans_.size() == 1 ) {
            shared_ptr< QueryOp > op( op_.clone() );
            Runner r( *plans_.plans_[ 0 ], *this, *op );
            r();
            return op;
        }
        
        boost::thread_group threads;
        vector< shared_ptr< QueryOp > > ops;
        for( PlanSet::iterator i = plans_.plans_.begin(); i != plans_.plans_.end(); ++i ) {
            ops.push_back( shared_ptr< QueryOp >( op_.clone() ) );
            Runner r( **i, *this, *ops.back() );
            threads.create_thread( r );
        }
        threads.join_all();
        for( vector< shared_ptr< QueryOp > >::iterator i = ops.begin(); i != ops.end(); ++i )
            if ( (*i)->complete() )
                return *i;
        return ops[ 0 ];
    }

    
//    QueryPlan QueryOptimizer::getPlan(
//        const char *ns,
//        BSONObj* query,
//        BSONObj* order,
//        BSONObj* hint)
//    {
//        QueryPlan plan;
//
//
//
//        return plan;
//    }

} // namespace mongo
