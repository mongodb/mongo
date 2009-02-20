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
        massert( "Incompatible bounds", lower_.woCompare( upper_, false ) <= 0 );
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
    
    QueryPlan::QueryPlan( const FieldBoundSet &fbs, const BSONObj &order, const IndexDetails *index ) :
    fbs_( fbs ),
    order_( order ),
    index_( index ),
    optimal_( false ),
    scanAndOrderRequired_( true ),
    keyMatch_( false ),
    exactKeyMatch_( false ),
    direction_( 0 ) {
        // full table scan case
        if ( !index_ ) {
            if ( order_.isEmpty() )
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
        BSONObjBuilder lowKeyBuilder;
        BSONObjBuilder highKeyBuilder;
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const FieldBound &fb = fbs.bound( e.fieldName() );
            lowKeyBuilder.appendAs( fb.lower(), "" );
            highKeyBuilder.appendAs( fb.upper(), "" );
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
        BSONObj lowKey = lowKeyBuilder.obj();
        BSONObj highKey = highKeyBuilder.obj();
        startKey_ = ( direction_ >= 0 ) ? lowKey : highKey;
        endKey_ = ( direction_ >= 0 ) ? highKey : lowKey;
    }
    
    auto_ptr< Cursor > QueryPlan::newCursor() const {
        if ( !index_ )
            return theDataFileMgr.findAll( fbs_.ns() );
        else
            return auto_ptr< Cursor >( new BtreeCursor( *const_cast< IndexDetails* >( index_ ), startKey_, endKey_, direction_ ) );
            //TODO This constructor should really take a const ref to the index details.
    }
    
    QueryPlanSet::QueryPlanSet( const char *ns, const BSONObj &query, const BSONObj &order, const BSONElement *hint ) :
    fbs_( ns, query ) {
        NamespaceDetails *d = nsdetails( ns );
        assert( d );

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
        
        PlanSet plans;
        for( int i = 0; i < d->nIndexes; ++i ) {
            PlanPtr p( new QueryPlan( fbs_, order, &d->indexes[ i ] ) );
            if ( p->optimal() ) {
                plans_.push_back( p );
                return;
            }
            plans.push_back( p );
        }
        for( PlanSet::iterator i = plans.begin(); i != plans.end(); ++i )
            plans_.push_back( *i );
    }
    
    auto_ptr< QueryOp > QueryPlanSet::runOp( QueryOp &op ) {
        RunnerSet s( *this, op );
        return s.run();
    }
    
    QueryPlanSet::RunnerSet::RunnerSet( QueryPlanSet &plans, QueryOp &op ) :
    op_( op ),
    plans_( plans ),
    startBarrier_( plans_.nPlans() ),
    firstDone_( false ) {
    }
    
    auto_ptr< QueryOp > QueryPlanSet::RunnerSet::run() {
        boost::thread_group threads;
        auto_ptr< QueryOp > ops[ plans_.nPlans() ];
        for( int i = 0; i < plans_.nPlans(); ++i ) {
            ops[ i ] = auto_ptr< QueryOp >( op_.clone() );
            Runner r( *plans_.plans_[ i ], *this, *ops[ i ] );
            threads.create_thread( r );
        }
        threads.join_all();
        cout << "really done" << endl;        
        for( int i = 0; i < plans_.nPlans(); ++i )
            if ( ops[ i ]->done() )
                return ops[ i ];
        assert( false );
        return auto_ptr< QueryOp >( 0 );
    }

    class CountOp : public QueryOp {
    public:
        virtual void run( const QueryPlan &qp, QueryAborter &qa ) {
            for( int i = 0; i < 100000; ++i )
                qa.mayAbort();
            cout << "done" << endl;
        }
        virtual QueryOp *clone() const {
            return new CountOp( *this );
        }
        int count() const { return 1; }
    };
    
    int doCount( const char *ns, const BSONObj &cmd, string &err ) {
        BSONObj query = cmd.getObjectField("query");
        QueryPlanSet qps( ns, query, emptyObj );
        auto_ptr< QueryOp > original( new CountOp () );
        auto_ptr< QueryOp > o = qps.runOp( *original );
        return dynamic_cast< CountOp* >( o.get() )->count();
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
