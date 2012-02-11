// queryoptimizertests.cpp : query optimizer unit tests
//

/**
 *    Copyright (C) 2009 10gen Inc.
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
#include "../db/queryoptimizercursor.h"
#include "../db/queryoptimizer.h"
#include "../db/instance.h"
#include "../db/ops/delete.h"
#include "dbtests.h"

namespace mongo {
    void __forceLinkGeoPlugin();
    shared_ptr<Cursor> newQueryOptimizerCursor( const char *ns, const BSONObj &query,
                                               const BSONObj &order = BSONObj(),
                                               const QueryPlanSelectionPolicy &planPolicy =
                                               QueryPlanSelectionPolicy::any() );
} // namespace mongo

namespace QueryOptimizerCursorTests {
    
    void dropCollection( const char *ns ) {
     	string errmsg;
        BSONObjBuilder result;
        dropCollection( ns, errmsg, result );
    }
        
    using boost::shared_ptr;
    
    class CachedMatchCounterCount {
    public:
        void run() {
            long long aggregateNscanned;
            CachedMatchCounter c( aggregateNscanned, 0 );
            ASSERT_EQUALS( 0, c.count() );
            ASSERT_EQUALS( 0, c.cumulativeCount() );

            c.resetMatch();
            ASSERT( !c.knowMatch() );

            c.setMatch( false );
            ASSERT( c.knowMatch() );

            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 0, c.count() );
            ASSERT_EQUALS( 0, c.cumulativeCount() );
            
            c.resetMatch();
            ASSERT( !c.knowMatch() );
            
            c.setMatch( true );
            ASSERT( c.knowMatch() );
            
            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 1, c.count() );
            ASSERT_EQUALS( 1, c.cumulativeCount() );

            // Don't count the same match twice, without checking the document location.
            c.countMatch( DiskLoc( 1, 1 ) );
            ASSERT_EQUALS( 1, c.count() );
            ASSERT_EQUALS( 1, c.cumulativeCount() );

            // Reset and count another match.
            c.resetMatch();
            c.setMatch( true );
            c.countMatch( DiskLoc( 1, 1 ) );
            ASSERT_EQUALS( 2, c.count() );
            ASSERT_EQUALS( 2, c.cumulativeCount() );
        }
    };
    
    class CachedMatchCounterAccumulate {
    public:
        void run() {
            long long aggregateNscanned;
            CachedMatchCounter c( aggregateNscanned, 10 );
            ASSERT_EQUALS( 0, c.count() );
            ASSERT_EQUALS( 10, c.cumulativeCount() );
            
            c.setMatch( true );
            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 1, c.count() );
            ASSERT_EQUALS( 11, c.cumulativeCount() );
        }
    };
    
    class CachedMatchCounterDedup {
    public:
        void run() {
            long long aggregateNscanned;
            CachedMatchCounter c( aggregateNscanned, 0 );

            c.setCheckDups( true );
            c.setMatch( true );
            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 1, c.count() );

            c.resetMatch();
            c.setMatch( true );
            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 1, c.count() );
        }
    };

    class CachedMatchCounterNscanned {
    public:
        void run() {
            long long aggregateNscanned = 5;
            CachedMatchCounter c( aggregateNscanned, 0 );
            ASSERT_EQUALS( 0, c.nscanned() );
            ASSERT_EQUALS( 5, c.aggregateNscanned() );

            c.updateNscanned( 4 );
            ASSERT_EQUALS( 4, c.nscanned() );
            ASSERT_EQUALS( 9, c.aggregateNscanned() );
        }
    };
    
    class SmallDupSetUpgrade {
    public:
        void run() {
            SmallDupSet d;
            for( int i = 0; i < 100; ++i ) {
                ASSERT( !d.getsetdup( DiskLoc( 0, i ) ) );
                for( int j = 0; j <= i; ++j ) {
                    ASSERT( d.getdup( DiskLoc( 0, j ) ) );
                }
            }
        }
    };

    class SmallDupSetUpgradeRead {
    public:
        void run() {
            SmallDupSet d;
            d.getsetdup( DiskLoc( 0, 0 ) );
            for( int i = 0; i < 550; ++i ) {
                ASSERT( d.getdup( DiskLoc( 0, 0 ) ) );
            }
            ASSERT( d.getsetdup( DiskLoc( 0, 0 ) ) );
        }
    };

    class SmallDupSetUpgradeWrite {
    public:
        void run() {
            SmallDupSet d;
            for( int i = 0; i < 550; ++i ) {
                ASSERT( !d.getsetdup( DiskLoc( 0, i ) ) );
            }
            for( int i = 0; i < 550; ++i ) {
                ASSERT( d.getsetdup( DiskLoc( 0, i ) ) );
            }
        }
    };

    class Base {
    public:
        Base() {
            dblock lk;
            Client::Context ctx( ns() );
            string err;
            userCreateNS( ns(), BSONObj(), err, false );
            dropCollection( ns() );
        }
        ~Base() {
            cc().curop()->reset();
        }
    protected:
        DBDirectClient _cli;
        static const char *ns() { return "unittests.QueryOptimizerTests"; }
        void setQueryOptimizerCursor( const BSONObj &query, const BSONObj &order = BSONObj() ) {
            setQueryOptimizerCursorWithoutAdvancing( query, order );
            if ( ok() && !mayReturnCurrent() ) {
                advance();
            }
        }
        void setQueryOptimizerCursorWithoutAdvancing( const BSONObj &query, const BSONObj &order = BSONObj() ) {
            _c = newQueryOptimizerCursor( ns(), query, order );
        }
        bool ok() const { return _c->ok(); }
        /** Handles matching and deduping. */
        bool advance() {
            while( _c->advance() && !mayReturnCurrent() );
            return ok();
        }
        int itcount() {
            int ret = 0;
            while( ok() ) {
                ++ret;
                advance();
            }
            return ret;
        }
        BSONObj current() const { return _c->current(); }
        DiskLoc currLoc() const { return _c->currLoc(); }
        void prepareToTouchEarlierIterate() { _c->prepareToTouchEarlierIterate(); }
        void recoverFromTouchingEarlierIterate() { _c->recoverFromTouchingEarlierIterate(); }
        bool mayReturnCurrent() {
//            return _c->currentMatches() && !_c->getsetdup( _c->currLoc() );
            return ( !_c->matcher() || _c->matcher()->matchesCurrent( _c.get() ) ) && !_c->getsetdup( _c->currLoc() );
        }
        bool prepareToYield() const { return _c->prepareToYield(); }
        void recoverFromYield() {
            _c->recoverFromYield();
            if ( ok() && !mayReturnCurrent() ) {
                advance();   
            }
        }
        shared_ptr<Cursor> c() { return _c; }
        long long nscanned() const { return _c->nscanned(); }
        unsigned nNsCursors() const {
            set<CursorId> nsCursors;
            ClientCursor::find( ns(), nsCursors );
            return nsCursors.size();
        }
    private:
        shared_ptr<Cursor> _c;
    };
    
    /** No results for empty collection. */
    class Empty : public Base {
    public:
        void run() {
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSONObj() );
            ASSERT( !c->ok() );
            ASSERT_THROWS( c->_current(), AssertionException );
            ASSERT_THROWS( c->current(), AssertionException );
            ASSERT( c->currLoc().isNull() );
            ASSERT( !c->advance() );
            ASSERT_THROWS( c->currKey(), AssertionException );
            ASSERT_THROWS( c->getsetdup( DiskLoc() ), AssertionException );
            ASSERT_THROWS( c->isMultiKey(), AssertionException );
            ASSERT_THROWS( c->matcher(), AssertionException );
        }
    };
    
    /** Simple table scan. */
    class Unindexed : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSONObj() );
            ASSERT_EQUALS( 2, itcount() );
        }
    };
    
    /** Basic test with two indexes and deduping requirement. */
    class Basic : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
            ASSERT( ok() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 2 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 1 ), current() );
            ASSERT( !advance() );
            ASSERT( !ok() );
        }
    };
    
    class NoMatch : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 5 << LT << 4 << "a" << GT << 0 ) );
            ASSERT( !ok() );
        }            
    };
    
    /** Order of results indicates that interleaving is occurring. */
    class Interleaved : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 3 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
            ASSERT( ok() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 2 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 3 << "a" << 1 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 2 ), current() );
            ASSERT( !advance() );
            ASSERT( !ok() );
        }
    };
    
    /** Some values on each index do not match. */
    class NotMatch : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 10 ) );
            _cli.insert( ns(), BSON( "_id" << 10 << "a" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 11 << "a" << 12 ) );
            _cli.insert( ns(), BSON( "_id" << 12 << "a" << 11 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 5 << "a" << GT << 5 ) );
            ASSERT( ok() );
            ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), current() );
            ASSERT( !advance() );
            ASSERT( !ok() );
        }            
    };
    
    /** After the first 101 matches for a plan, we stop interleaving the plans. */
    class StopInterleaving : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            for( int i = 101; i < 200; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << (301-i) ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << GT << -1 ) );
            for( int i = 0; i < 200; ++i ) {
                ASSERT( ok() );
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !advance() );
            ASSERT( !ok() );                
        }
    };
    
    /** Test correct deduping with the takeover cursor. */
    class TakeoverWithDup : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            _cli.insert( ns(), BSON( "_id" << 500 << "a" << BSON_ARRAY( 0 << 300 ) ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << GT << -1 ) );
            ASSERT_EQUALS( 102, itcount() );
        }
    };
    
    /** Test usage of matcher with takeover cursor. */
    class TakeoverWithNonMatches : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            _cli.insert( ns(), BSON( "_id" << 101 << "a" << 600 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << LT << 500 ) );
            ASSERT_EQUALS( 101, itcount() );
        }
    };
    
    /** Check deduping of dups within just the takeover cursor. */
    class TakeoverWithTakeoverDup : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i*2 << "a" << 0 ) );
                _cli.insert( ns(), BSON( "_id" << i*2+1 << "a" << 1 ) );
            }
            _cli.insert( ns(), BSON( "_id" << 202 << "a" << BSON_ARRAY( 2 << 3 ) ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << GT << 0) );
            ASSERT_EQUALS( 102, itcount() );
        }
    };
    
    /** Basic test with $or query. */
    class BasicOr : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 0 ) << BSON( "a" << 1 ) ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
            ASSERT( !advance() );
        }
    };
    
    /** $or first clause empty. */
    class OrFirstClauseEmpty : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << -1 ) << BSON( "a" << 1 ) ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
            ASSERT( !advance() );
        }
    };        
    
    /** $or second clause empty. */
    class OrSecondClauseEmpty : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 0 ) << BSON( "_id" << -1 ) << BSON( "a" << 1 ) ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
            ASSERT( !advance() );
        }
    };
    
    /** $or multiple clauses empty empty. */
    class OrMultipleClausesEmpty : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 2 ) << BSON( "_id" << 4 ) << BSON( "_id" << 0 ) << BSON( "_id" << -1 ) << BSON( "_id" << 6 ) << BSON( "a" << 1 ) << BSON( "_id" << 9 ) ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
            ASSERT( !advance() );
        }
    };
    
    /** Check that takeover occurs at proper match count with $or clauses */
    class TakeoverCountOr : public Base {
    public:
        void run() {
            for( int i = 0; i < 60; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 0 ) );   
            }
            for( int i = 60; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 1 ) );
            }
            for( int i = 120; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << (200-i) ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "a" << 0 ) << BSON( "a" << 1 ) << BSON( "_id" << GTE << 120 << "a" << GT << 1 ) ) ) );
            for( int i = 0; i < 120; ++i ) {
                ASSERT( ok() );
                advance();
            }
            // Expect to be scanning on _id index only.
            for( int i = 120; i < 150; ++i ) {
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !ok() );
        }
    };
    
    /** Takeover just at end of clause. */
    class TakeoverEndOfOrClause : public Base {
    public:
        void run() {
            for( int i = 0; i < 102; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );   
            }
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 101 ) << BSON( "_id" << 101 ) ) ) );
            for( int i = 0; i < 102; ++i ) {
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !ok() );
        }
    };
    
    class TakeoverBeforeEndOfOrClause : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );   
            }
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 100 ) << BSON( "_id" << 100 ) ) ) );
            for( int i = 0; i < 101; ++i ) {
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !ok() );
        }
    };
    
    class TakeoverAfterEndOfOrClause : public Base {
    public:
        void run() {
            for( int i = 0; i < 103; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );   
            }
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 102 ) << BSON( "_id" << 102 ) ) ) );
            for( int i = 0; i < 103; ++i ) {
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !ok() );
        }
    };
    
    /** Test matching and deduping done manually by cursor client. */
    class ManualMatchingDeduping : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 10 ) );
            _cli.insert( ns(), BSON( "_id" << 10 << "a" << 0 ) ); 
            _cli.insert( ns(), BSON( "_id" << 11 << "a" << 12 ) );
            _cli.insert( ns(), BSON( "_id" << 12 << "a" << 11 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 5 << "a" << GT << 5 ) );
            ASSERT( c->ok() );
            
            // _id 10 {_id:1}
            ASSERT_EQUALS( 10, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 0 {a:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 0 {$natural:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 11 {_id:1}
            ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 12 {a:1}
            ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 10 {$natural:1}
            ASSERT_EQUALS( 10, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 12 {_id:1}
            ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 11 {a:1}
            ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 11 {$natural:1}
            ASSERT_EQUALS( 11, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            
            // {_id:1} scan is complete.
            ASSERT( !c->advance() );
            ASSERT( !c->ok() );       
            
            // Scan the results again - this time the winning plan has been
            // recorded.
            c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 5 << "a" << GT << 5 ) );
            ASSERT( c->ok() );
            
            // _id 10 {_id:1}
            ASSERT_EQUALS( 10, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 11 {_id:1}
            ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 12 {_id:1}
            ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            
            // {_id:1} scan complete
            ASSERT( !c->advance() );
            ASSERT( !c->ok() );
        }
    };
    
    /** Curr key must be correct for currLoc for correct matching. */
    class ManualMatchingUsingCurrKey : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << "a" ) );
            _cli.insert( ns(), BSON( "_id" << "b" ) );
            _cli.insert( ns(), BSON( "_id" << "ba" ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), fromjson( "{_id:/a/}" ) );
            ASSERT( c->ok() );
            // "a"
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            ASSERT( c->ok() );
            
            // "b"
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            ASSERT( c->ok() );
            
            // "ba"
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( !c->advance() );
        }
    };
    
    /** Test matching and deduping done manually by cursor client. */
    class ManualMatchingDedupingTakeover : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 0 ) );
            }
            _cli.insert( ns(), BSON( "_id" << 300 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 300 ) << BSON( "a" << 1 ) ) ) );
            for( int i = 0; i < 151; ++i ) {
                ASSERT( c->ok() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                c->advance();
            }
            ASSERT( !c->ok() );
        }
    };
    
    /** Test single key matching bounds. */
    class Singlekey : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << "10" ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), BSON( "a" << GT << 1 << LT << 5 ) );
            // Two sided bounds work.
            ASSERT( !c->ok() );
        }
    };
    
    /** Test multi key matching bounds. */
    class Multikey : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 10 ) ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "a" << GT << 5 << LT << 3 ) );
            // Multi key bounds work.
            ASSERT( ok() );
        }
    };
    
    /** Add other plans when the recorded one is doing more poorly than expected. */
    class AddOtherPlans : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 << "b" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 << "b" << 0 ) );
            for( int i = 100; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 100 << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << 0 << "b" << 0 ) );
            
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
            ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );

            ASSERT( c->advance() );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
            ASSERT_EQUALS( BSON( "b" << 1 ), c->indexKeyPattern() );
            
            ASSERT( c->advance() );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );                
            // Unindexed plan
            ASSERT_EQUALS( BSONObj(), c->indexKeyPattern() );
            ASSERT( !c->advance() );
            
            c = newQueryOptimizerCursor( ns(), BSON( "a" << 100 << "b" << 149 ) );
            // Try {a:1}, which was successful previously.
            for( int i = 0; i < 12; ++i ) {
                ASSERT( 149 != c->current().getIntField( "b" ) );
                ASSERT( c->advance() );
            }
            bool sawB1Index = false;
            do {
                if ( c->indexKeyPattern() == BSON( "b" << 1 ) ) {
                    ASSERT_EQUALS( 149, c->current().getIntField( "b" ) );
                    // We should try the {b:1} index and only see one result from it.
                    ASSERT( !sawB1Index );
                    sawB1Index = true;
                }
            } while ( c->advance() );
            ASSERT( sawB1Index );
        }
    };

    /** Add other plans when the recorded one is doing more poorly than expected, with deletion. */
    class AddOtherPlansDelete : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 << "b" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 << "b" << 0 ) );
            for( int i = 100; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 100 << "b" << i ) );
            }
            for( int i = 199; i >= 150; --i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 100 << "b" << 150 ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << 0 << "b" << 0 ) );
            
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
            ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );
            
            ASSERT( c->advance() );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
            ASSERT_EQUALS( BSON( "b" << 1 ), c->indexKeyPattern() );
            
            ASSERT( c->advance() );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );                
            // Unindexed plan
            ASSERT_EQUALS( BSONObj(), c->indexKeyPattern() );
            ASSERT( !c->advance() );
            
            c = newQueryOptimizerCursor( ns(), BSON( "a" << 100 << "b" << 150 ) );
            // Try {a:1}, which was successful previously.
            for( int i = 0; i < 12; ++i ) {
                ASSERT( 150 != c->current().getIntField( "b" ) );
                ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );
                ASSERT( c->advance() );
            }
            // Now try {b:1} plan.
            ASSERT_EQUALS( BSON( "b" << 1 ), c->indexKeyPattern() );
            ASSERT_EQUALS( 150, c->current().getIntField( "b" ) );
            ASSERT( c->currentMatches() );
            int id = c->current().getIntField( "_id" );
            c->advance();
            c->prepareToTouchEarlierIterate();
            _cli.remove( ns(), BSON( "_id" << id ) );
            c->recoverFromTouchingEarlierIterate();
            int count = 1;
            while( c->ok() ) {
                if ( c->currentMatches() ) {
                    ++count;
                    int id = c->current().getIntField( "_id" );
                    c->advance();
                    c->prepareToTouchEarlierIterate();
                    _cli.remove( ns(), BSON( "_id" << id ) );
                    c->recoverFromTouchingEarlierIterate();                    
                }
                else {
                    c->advance();
                }
            }
            ASSERT_EQUALS( 50, count );
        }
    };

    /**
     * Add other plans when the recorded one is doing more poorly than expected, with deletion before
     * and after adding the additional plans.
     */
    class AddOtherPlansContinuousDelete : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 << "b" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 << "b" << 0 ) );
            for( int i = 100; i < 400; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i << "b" << ( 499 - i ) ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << GTE << -1 << LTE << 0 << "b" << GTE << -1 << LTE << 0 ) );
            while( c->advance() );
            // {a:1} plan should be recorded now.
          
            c = newQueryOptimizerCursor( ns(), BSON( "a" << GTE << 100 << LTE << 400 << "b" << GTE << 100 << LTE << 400 ) );
            int count = 0;
            while( c->ok() ) {
                if ( c->currentMatches() ) {
                    ASSERT( !c->getsetdup( c->currLoc() ) );
                    ++count;
                    int id = c->current().getIntField( "_id" );
                    c->advance();
                    c->prepareToTouchEarlierIterate();
                    _cli.remove( ns(), BSON( "_id" << id ) );
                    c->recoverFromTouchingEarlierIterate();
                } else {
                    c->advance();
                }
            }
            ASSERT_EQUALS( 300, count );
            ASSERT_EQUALS( 2U, _cli.count( ns(), BSONObj() ) );
        }
    };

    /** Check $or clause range elimination. */
    class OrRangeElimination : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << GT << 0 ) << BSON( "_id" << 1 ) ) ) );
            ASSERT( c->ok() );
            ASSERT( !c->advance() );
        }
    };
    
    /** Check $or match deduping - in takeover cursor. */
    class OrDedup : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 140 ) << BSON( "_id" << 145 ) << BSON( "a" << 145 ) ) ) );
            
            while( c->current().getIntField( "_id" ) < 140 ) {
                ASSERT( c->advance() );
            }
            // Match from second $or clause.
            ASSERT_EQUALS( 145, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            // Match from third $or clause.
            ASSERT_EQUALS( 145, c->current().getIntField( "_id" ) );
            // $or deduping is handled by the matcher.
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->advance() );
        }
    };
    
    /** Standard dups with a multikey cursor. */
    class EarlyDups : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 0 << 1 << 200 ) ) );
            for( int i = 2; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "a" << i ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "a" << GT << -1 ) );
            ASSERT_EQUALS( 149, itcount() );
        }
    };
    
    /** Pop or clause in takeover cursor. */
    class OrPopInTakeover : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );   
            }
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << LTE << 147 ) << BSON( "_id" << 148 ) << BSON( "_id" << 149 ) ) ) );
            for( int i = 0; i < 150; ++i ) {
                ASSERT( c->ok() );
                ASSERT_EQUALS( i, c->current().getIntField( "_id" ) );
                c->advance();
            }
            ASSERT( !c->ok() );
        }
    };
    
    /** Or clause iteration abandoned once full collection scan is performed. */
    class OrCollectionScanAbort : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << BSON_ARRAY( 1 << 2 << 3 << 4 << 5 ) << "b" << 4 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << BSON_ARRAY( 6 << 7 << 8 << 9 << 10 ) << "b" << 4 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "a" << LT << 6 << "b" << 4 ) << BSON( "a" << GTE << 6 << "b" << 4 ) ) ) );
            
            ASSERT( c->ok() );
            
            // _id 0 on {a:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // _id 0 on {$natural:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // _id 0 on {a:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // _id 1 on {$natural:1}
            ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // _id 0 on {a:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // {$natural:1} finished
            ASSERT( !c->ok() );
        }
    };
    
    /** Yield cursor and delete current entry, then continue iteration. */
    class YieldNoOp : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
                ASSERT( prepareToYield() );
                recoverFromYield();
            }
        }            
    };
    
    /** Yield cursor and delete current entry. */
    class YieldDelete : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << 1 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( !ok() );
                ASSERT( !advance() );
            }
        }
    };
    
    /** Yield cursor and delete current entry, then continue iteration. */
    class YieldDeleteContinue : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        }            
    };
    
    /** Yield cursor and delete current entry, then continue iteration. */
    class YieldDeleteContinueFurther : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 3 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        }            
    };
    
    /** Yield and update current. */
    class YieldUpdate : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "a" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.update( ns(), BSON( "a" << 1 ), BSON( "$set" << BSON( "a" << 3 ) ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "a" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yield and drop collection. */
    class YieldDrop : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.dropCollection( ns() );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yield and drop collection with $or query. */
    class YieldDropOr : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 1 ) << BSON( "_id" << 2 ) ) ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.dropCollection( ns() );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                ASSERT_THROWS( recoverFromYield(), MsgAssertionException );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yield and remove document with $or query. */
    class YieldRemoveOr : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );

            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 1 ) << BSON( "_id" << 2 ) ) ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }

            _cli.remove( ns(), BSON( "_id" << 1 ) );

            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
            }
        }
    };

    /** Yield and overwrite current in capped collection. */
    class YieldCappedOverwrite : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "x" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "x" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "x" ) );
                ASSERT( prepareToYield() );
            }
            
            int x = 2;
            while( _cli.count( ns(), BSON( "x" << 1 ) ) > 0 ) {
                _cli.insert( ns(), BSON( "x" << x++ ) );   
            }
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                ASSERT_THROWS( recoverFromYield(), MsgAssertionException );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yield and drop unrelated index - see SERVER-2454. */
    class YieldDropIndex : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << 1 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.dropIndex( ns(), BSON( "a" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yielding with multiple plans active. */
    class YieldMultiplePlansNoOp : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yielding with advance and multiple plans active. */
    class YieldMultiplePlansAdvanceNoOp : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 3 << "a" << 3 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                advance();
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yielding with delete and multiple plans active. */
    class YieldMultiplePlansDelete : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 3 << "a" << 4 ) );
            _cli.insert( ns(), BSON( "_id" << 4 << "a" << 3 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                advance();
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 2 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                c()->recoverFromYield();
                ASSERT( ok() );
                // index {a:1} active during yield
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 4, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }                
        }
    };

    /** Yielding with delete, multiple plans active, and $or clause. */
    class YieldMultiplePlansDeleteOr : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );

            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 1 << "a" << 2 ) << BSON( "_id" << 2 << "a" << 1 ) ) ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }

            _cli.remove( ns(), BSON( "_id" << 1 ) );

            {
                dblock lk;
                Client::Context ctx( ns() );
                c()->recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        }
    };
    
    /** Yielding with delete, multiple plans active with advancement to the second, and $or clause. */
    class YieldMultiplePlansDeleteOrAdvance : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );

            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 1 << "a" << 2 ) << BSON( "_id" << 2 << "a" << 1 ) ) ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
                c()->advance();
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
            }

            _cli.remove( ns(), BSON( "_id" << 1 ) );

            {
                dblock lk;
                Client::Context ctx( ns() );
                c()->recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        }
    };

    /** Yielding with multiple plans and capped overwrite. */
    class YieldMultiplePlansCappedOverwrite : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "_id" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            int i = 1;
            while( _cli.count( ns(), BSON( "_id" << 1 ) ) > 0 ) {
                ++i;
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );
            }
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                // {$natural:1} plan does not recover, {_id:1} plan does.
                ASSERT( 1 < current().getIntField( "_id" ) );
            }                
        }
    };
    
    /**
     * Yielding with multiple plans and capped overwrite with unrecoverable cursor
     * active at time of yield.
     */
    class YieldMultiplePlansCappedOverwriteManual : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            shared_ptr<Cursor> c;
            {
                dblock lk;
                Client::Context ctx( ns() );
                c = newQueryOptimizerCursor( ns(), BSON( "a" << GT << 0 << "b" << GT << 0 ) );
                ASSERT_EQUALS( 1, c->current().getIntField( "a" ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                c->advance();
                ASSERT_EQUALS( 1, c->current().getIntField( "a" ) );
                ASSERT( c->getsetdup( c->currLoc() ) );
                ASSERT( c->prepareToYield() );
            }
            
            int i = 1;
            while( _cli.count( ns(), BSON( "a" << 1 ) ) > 0 ) {
                ++i;
                _cli.insert( ns(), BSON( "a" << i << "b" << i ) );
            }
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                c->recoverFromYield();
                ASSERT( c->ok() );
                // {$natural:1} plan does not recover, {_id:1} plan does.
                ASSERT( 1 < c->current().getIntField( "a" ) );
            }                
        }
    };
    
    /**
     * Yielding with multiple plans and capped overwrite with unrecoverable cursor
     * inctive at time of yield.
     */
    class YieldMultiplePlansCappedOverwriteManual2 : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "_id" << 1 ) );
            
            shared_ptr<Cursor> c;
            {
                dblock lk;
                Client::Context ctx( ns() );
                c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                ASSERT( c->prepareToYield() );
            }
            
            int n = 1;
            while( _cli.count( ns(), BSON( "_id" << 1 ) ) > 0 ) {
                ++n;
                _cli.insert( ns(), BSON( "_id" << n << "a" << n ) );
            }
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                c->recoverFromYield();
                ASSERT( c->ok() );
                // {$natural:1} plan does not recover, {_id:1} plan does.
                ASSERT( 1 < c->current().getIntField( "_id" ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                int i = c->current().getIntField( "_id" );
                ASSERT( c->advance() );
                ASSERT( c->getsetdup( c->currLoc() ) );
                while( i < n ) {
                    ASSERT( c->advance() );
                    ++i;
                    ASSERT_EQUALS( i, c->current().getIntField( "_id" ) );
                }
            }                
        }
    };
    
    /** Yield with takeover cursor. */
    class YieldTakeover : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GTE << 0 << "a" << GTE << 0 ) );
                for( int i = 0; i < 120; ++i ) {
                    ASSERT( advance() );
                }
                ASSERT( ok() );
                ASSERT_EQUALS( 120, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 120 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 121, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 122, current().getIntField( "_id" ) );
            }
        }
    };
    
    /** Yield with BacicCursor takeover cursor. */
    class YieldTakeoverBasic : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << BSON_ARRAY( i << i+1 ) ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            auto_ptr<ClientCursor> cc;
            auto_ptr<ClientCursor::YieldData> data( new ClientCursor::YieldData() );
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "b" << NE << 0 << "a" << GTE << 0 ) );
                cc.reset( new ClientCursor( QueryOption_NoCursorTimeout, c(), ns() ) );
                for( int i = 0; i < 120; ++i ) {
                    ASSERT( advance() );
                }
                ASSERT( ok() );
                ASSERT_EQUALS( 120, current().getIntField( "_id" ) );
                cc->prepareToYield( *data );
            }                
            _cli.remove( ns(), BSON( "_id" << 120 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                ASSERT( ClientCursor::recoverFromYield( *data ) );
                ASSERT( ok() );
                ASSERT_EQUALS( 121, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 122, current().getIntField( "_id" ) );
            }
        }
    };
    
    /** Yield with advance of inactive cursor. */
    class YieldInactiveCursorAdvance : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 10 - i ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT( ok() );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 9, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 9 ) );
            
            {
                dblock lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 8, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 7, current().getIntField( "_id" ) );
            }                    
        }
    };
    
    class OrderId : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );
            }
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSONObj(), BSON( "_id" << 1 ) );
            
            for( int i = 0; i < 10; ++i, advance() ) {
                ASSERT( ok() );
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
            }
        }
    };
    
    class OrderMultiIndex : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 1 ) );
            }
            _cli.ensureIndex( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GTE << 0 << "a" << GTE << 0 ), BSON( "_id" << 1 ) );
            
            for( int i = 0; i < 10; ++i, advance() ) {
                ASSERT( ok() );
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
            }
        }
    };
    
    class OrderReject : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i % 5 ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "a" << GTE << 3 ), BSON( "_id" << 1 ) );
            
            ASSERT( ok() );
            ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
            ASSERT( advance() );
            ASSERT_EQUALS( 4, current().getIntField( "_id" ) );
            ASSERT( advance() );
            ASSERT_EQUALS( 8, current().getIntField( "_id" ) );
            ASSERT( advance() );
            ASSERT_EQUALS( 9, current().getIntField( "_id" ) );
            ASSERT( !advance() );
        }
    };
    
    class OrderNatural : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 5 ) );
            _cli.insert( ns(), BSON( "_id" << 4 ) );
            _cli.insert( ns(), BSON( "_id" << 6 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 ), BSON( "$natural" << 1 ) );
            
            ASSERT( ok() );
            ASSERT_EQUALS( 5, current().getIntField( "_id" ) );
            ASSERT( advance() );
            ASSERT_EQUALS( 4, current().getIntField( "_id" ) );
            ASSERT( advance() );                
            ASSERT_EQUALS( 6, current().getIntField( "_id" ) );
            ASSERT( !advance() );                
        }
    };
    
    class OrderUnindexed : public Base {
    public:
        void run() {
            dblock lk;
            Client::Context ctx( ns() );
            ASSERT( !newQueryOptimizerCursor( ns(), BSONObj(), BSON( "a" << 1 ) ).get() );
        }
    };
    
    class RecordedOrderInvalid : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 2 << "b" << 2 ) );
            _cli.insert( ns(), BSON( "a" << 3 << "b" << 3 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            ASSERT( _cli.query( ns(), QUERY( "a" << 2 ).sort( "b" ) )->more() );
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << 2 ), BSON( "b" << 1 ) );
            // Check that we are scanning {b:1} not {a:1}.
            for( int i = 0; i < 3; ++i ) {
                ASSERT( c->ok() );
                c->advance();
            }
            ASSERT( !c->ok() );
        }
    };
    
    class KillOp : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            Client::ReadContext ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            ASSERT( ok() );
            cc().curop()->kill();
            // First advance() call throws, subsequent calls just fail.
            ASSERT_THROWS( advance(), MsgAssertionException );
            ASSERT( !advance() );
        }
    };
    
    class KillOpFirstClause : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            Client::ReadContext ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << GT << 0 ) << BSON( "b" << GT << 0 ) ) ) );
            ASSERT( c->ok() );
            cc().curop()->kill();
            // First advance() call throws, subsequent calls just fail.
            ASSERT_THROWS( c->advance(), MsgAssertionException );
            ASSERT( !c->advance() );
        }
    };
    
    class Nscanned : public Base {
    public:
        void run() {
            for( int i = 0; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );
            }
            
            dblock lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "_id" << GTE << 0 << "a" << GTE << 0 ) );
            ASSERT( c->ok() );
            ASSERT_EQUALS( 2, c->nscanned() );
            c->advance();
            ASSERT( c->ok() );
            ASSERT_EQUALS( 2, c->nscanned() );
            c->advance();
            for( int i = 3; i < 222; ++i ) {
                ASSERT( c->ok() );
                c->advance();
            }
            ASSERT( !c->ok() );
        }
    };

    /* Test 'touching earlier iterate' without doc modifications. */
    class TouchEarlierIterate : public Base {
    public:
        void run() {            
            _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );

            Client::ReadContext ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            
            ASSERT( c->ok() );
            while( c->ok() ) {
                DiskLoc loc = c->currLoc();
                BSONObj obj = c->current();
                c->prepareToTouchEarlierIterate();
                c->recoverFromTouchingEarlierIterate();
                ASSERT( loc == c->currLoc() );
                ASSERT_EQUALS( obj, c->current() );
                c->advance();
            }
        }
    };

    /* Test 'touching earlier iterate' with doc modifications. */
    class TouchEarlierIterateDelete : public Base {
    public:
        void run() {            
            _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            DiskLoc firstLoc;
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            ASSERT( ok() );
            firstLoc = currLoc();
            ASSERT( c()->advance() );
            prepareToTouchEarlierIterate();
            
            _cli.remove( ns(), BSON( "_id" << 1 ), true );

            recoverFromTouchingEarlierIterate();
            ASSERT( ok() );
            while( ok() ) {
                ASSERT( firstLoc != currLoc() );
                c()->advance();
            }
        }
    };

    /* Test 'touch earlier iterate' with several doc modifications. */
    class TouchEarlierIterateDeleteMultiple : public Base {
    public:
        void run() {
            for( int i = 1; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            set<DiskLoc> deleted;
            int id = 0;
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            while( 1 ) {
                if ( !ok() ) {
                    break;
                }
                ASSERT( deleted.count( currLoc() ) == 0 );
                id = current()["_id"].Int();
                deleted.insert( currLoc() );
                c()->advance();
                prepareToTouchEarlierIterate();
                
                _cli.remove( ns(), BSON( "_id" << id ), true );

                recoverFromTouchingEarlierIterate();
            }
            ASSERT_EQUALS( 9U, deleted.size() );
        }
    };

    /* Test 'touch earlier iterate' with takeover. */
    class TouchEarlierIterateTakeover : public Base {
    public:
        void run() {
            for( int i = 1; i < 600; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            Client::ReadContext ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            
            ASSERT( ok() );
            int count = 1;
            while( ok() ) {
                DiskLoc loc = currLoc();
                BSONObj obj = current();
                prepareToTouchEarlierIterate();
                recoverFromTouchingEarlierIterate();
                ASSERT( loc == currLoc() );
                ASSERT_EQUALS( obj, current() );
                count += mayReturnCurrent();
                c()->advance();
            }
            ASSERT_EQUALS( 599, count );
        }
    };

    /* Test 'touch earlier iterate' with takeover and deletes. */
    class TouchEarlierIterateTakeoverDeleteMultiple : public Base {
    public:
        void run() {
            for( int i = 1; i < 600; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            set<DiskLoc> deleted;
            int id = 0;

            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursorWithoutAdvancing( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            while( 1 ) {
                if ( !ok() ) {
                    break;
                }
                ASSERT( deleted.count( currLoc() ) == 0 );
                id = current()["_id"].Int();
                ASSERT( c()->currentMatches() );
                ASSERT( !c()->getsetdup( currLoc() ) );
                deleted.insert( currLoc() );
                c()->advance();
                prepareToTouchEarlierIterate();

                _cli.remove( ns(), BSON( "_id" << id ), true );

                recoverFromTouchingEarlierIterate();
            }
            ASSERT_EQUALS( 599U, deleted.size() );
        }
    };

    /* Test 'touch earlier iterate' with undexed cursor takeover and deletes. */
    class TouchEarlierIterateUnindexedTakeoverDeleteMultiple : public Base {
    public:
        void run() {
            for( int i = 1; i < 600; ++i ) {
                _cli.insert( ns(), BSON( "a" << BSON_ARRAY( i << i+1 ) << "b" << BSON_ARRAY( i << i+1 ) << "_id" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            set<DiskLoc> deleted;
            int id = 0;
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursorWithoutAdvancing( BSON( "a" << GT << 0 << "b" << GT << 0 ) );
            while( 1 ) {
                if ( !ok() ) {
                    break;
                }
                ASSERT( deleted.count( currLoc() ) == 0 );
                id = current()["_id"].Int();
                ASSERT( c()->currentMatches() );
                ASSERT( !c()->getsetdup( currLoc() ) );
                deleted.insert( currLoc() );
                c()->advance();
                prepareToTouchEarlierIterate();
                
                _cli.remove( ns(), BSON( "_id" << id ), true );
                
                recoverFromTouchingEarlierIterate();
            }
            ASSERT_EQUALS( 599U, deleted.size() );
        }
    };
    
    /* Test 'touch earlier iterate' with takeover and deletes, with multiple advances in a row. */
    class TouchEarlierIterateTakeoverDeleteMultipleMultiAdvance : public Base {
    public:
        void run() {
            for( int i = 1; i < 600; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            set<DiskLoc> deleted;
            int id = 0;

            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            while( 1 ) {
                if ( !ok() ) {
                    break;
                }
                ASSERT( deleted.count( currLoc() ) == 0 );
                id = current()["_id"].Int();
                ASSERT( c()->currentMatches() );
                deleted.insert( currLoc() );
                advance();
                prepareToTouchEarlierIterate();

                _cli.remove( ns(), BSON( "_id" << id ), true );
                
                recoverFromTouchingEarlierIterate();
            }
            ASSERT_EQUALS( 599U, deleted.size() );
        }
    };

    /* Test yield recovery failure of component capped cursor. */
    class InitialCappedWrapYieldRecoveryFailure : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "_id" << 1 << "x" << 1 ) );
            
            dblock lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "x" << GT << 0 ) );
            ASSERT_EQUALS( 1, current().getIntField( "x" ) );
            
            ClientCursor::CleanupPointer p;
            p.reset( new ClientCursor( QueryOption_NoCursorTimeout, c(), ns() ) );
            ClientCursor::YieldData yieldData;
            p->prepareToYield( yieldData );
            
            int x = 2;
            while( _cli.count( ns(), BSON( "x" << 1 ) ) > 0 ) {
                _cli.insert( ns(), BSON( "_id" << x << "x" << x ) );
                ++x;
            }

            // TODO - Might be preferable to return false rather than assert here.
            ASSERT_THROWS( ClientCursor::recoverFromYield( yieldData ), AssertionException );
        }
    };

    /* Test yield recovery failure of takeover capped cursor. */
    class TakeoverCappedWrapYieldRecoveryFailure : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 10000, true );
            for( int i = 0; i < 300; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "x" << i ) );                
            }

            ClientCursor::CleanupPointer p;
            ClientCursor::YieldData yieldData;
            {
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "x" << GTE << 0 ) );
                for( int i = 0; i < 299; ++i ) {
                    advance();
                }
                ASSERT_EQUALS( 299, current().getIntField( "x" ) );
                
                p.reset( new ClientCursor( QueryOption_NoCursorTimeout, c(), ns() ) );
                p->prepareToYield( yieldData );
            }
            
            int i = 300;
            while( _cli.count( ns(), BSON( "x" << 299 ) ) > 0 ) {
                _cli.insert( ns(), BSON( "_id" << i << "x" << i ) );
                ++i;
            }
            
            dblock lk;
            Client::Context ctx( ns() );
            ASSERT( !ClientCursor::recoverFromYield( yieldData ) );
        }
    };

    /** Test that a ClientCursor holding a QueryOptimizerCursor may be safely invalidated. */
    class InvalidateClientCursorHolder : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            ClientCursor::CleanupPointer p;
            p.reset
            ( new ClientCursor
             ( QueryOption_NoCursorTimeout,
              NamespaceDetailsTransient::getCursor
              ( ns(), BSON( "a" << GTE << 0 << "b" << GTE << 0 ) ),
              ns() ) );

            // Construct component client cursors.
            ClientCursor::YieldData yieldData;
            p->prepareToYield( yieldData );
            ASSERT( nNsCursors() > 1 );

            ClientCursor::invalidate( ns() );
            ASSERT_EQUALS( 0U, nNsCursors() );
        }
    };
    
    /** Test that a ClientCursor holding a QueryOptimizerCursor may be safely timed out. */
    class TimeoutClientCursorHolder : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            dblock lk;
            Client::Context ctx( ns() );
            ClientCursor::CleanupPointer p;
            p.reset
            ( new ClientCursor
             ( 0,
              NamespaceDetailsTransient::getCursor
              ( ns(), BSON( "a" << GTE << 0 << "b" << GTE << 0 ) ),
              ns() ) );
            
            // Construct component client cursors.
            ClientCursor::YieldData yieldData;
            p->prepareToYield( yieldData );
            ASSERT( nNsCursors() > 1 );
            
            ClientCursor::idleTimeReport( 600001 );
            ASSERT_EQUALS( 0U, nNsCursors() );
        }
    };
    
    namespace GetCursor {
        
        class Base : public QueryOptimizerCursorTests::Base {
        public:
            Base() {
                // create collection
                _cli.insert( ns(), BSON( "_id" << 5 ) );
            }
            virtual ~Base() {}
            void run() {
                dblock lk;
                Client::Context ctx( ns() );
                bool simpleEqualityMatch;
                if ( expectException() ) {
                    ASSERT_THROWS
                    ( NamespaceDetailsTransient::getCursor
                     ( ns(), query(), order(), planPolicy(), &simpleEqualityMatch ),
                     MsgAssertionException );
                    return;
                }
                shared_ptr<Cursor> c =
                NamespaceDetailsTransient::getCursor( ns(), query(), order(), planPolicy(),
                                                     &simpleEqualityMatch );
                ASSERT_EQUALS( expectSimpleEquality(), simpleEqualityMatch );
                string type = c->toString().substr( 0, expectedType().length() );
                ASSERT_EQUALS( expectedType(), type );
                check( c );
            }
        protected:
            virtual string expectedType() const { return "TESTDUMMY"; }
            virtual bool expectException() const { return false; }
            virtual bool expectSimpleEquality() const { return false; }
            virtual BSONObj query() const { return BSONObj(); }
            virtual BSONObj order() const { return BSONObj(); }
            virtual const QueryPlanSelectionPolicy &planPolicy() const {
                return QueryPlanSelectionPolicy::any();
            }
            virtual void check( const shared_ptr<Cursor> &c ) {
                ASSERT( c->ok() );
                ASSERT( !c->matcher() );
                ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
                ASSERT( !c->advance() );
            }
        };
        
        class NoConstraints : public Base {
            string expectedType() const { return "BasicCursor"; }
        };
        
        class SimpleId : public Base {
        public:
            SimpleId() {
                _cli.insert( ns(), BSON( "_id" << 0 ) );
                _cli.insert( ns(), BSON( "_id" << 10 ) );
            }
            string expectedType() const { return "BtreeCursor _id_"; }
            BSONObj query() const { return BSON( "_id" << 5 ); }
        };
        
        class OptimalIndex : public Base {
        public:
            OptimalIndex() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.insert( ns(), BSON( "a" << 5 ) );
                _cli.insert( ns(), BSON( "a" << 6 ) );
            }
            string expectedType() const { return "BtreeCursor a_1"; }
            BSONObj query() const { return BSON( "a" << GTE << 5 ); }
            void check( const shared_ptr<Cursor> &c ) {
                ASSERT( c->ok() );
                ASSERT( c->matcher() );
                ASSERT_EQUALS( 5, c->current().getIntField( "a" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->advance() );                    
                ASSERT_EQUALS( 6, c->current().getIntField( "a" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->advance() );                    
            }
        };
        
        class SimpleKeyMatch : public Base {
        public:
            SimpleKeyMatch() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.update( ns(), BSONObj(), BSON( "$set" << BSON( "a" << true ) ) );
            }
            string expectedType() const { return "BtreeCursor a_1"; }
            bool expectSimpleEquality() const { return true; }
            BSONObj query() const { return BSON( "a" << true ); }
            virtual void check( const shared_ptr<Cursor> &c ) {
                ASSERT( c->ok() );
                ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
                ASSERT( !c->advance() );
            }
        };
        
        class Geo : public Base {
        public:
            Geo() {
                _cli.insert( ns(), BSON( "_id" << 44 << "loc" << BSON_ARRAY( 44 << 45 ) ) );
                _cli.ensureIndex( ns(), BSON( "loc" << "2d" ) );
            }
            string expectedType() const { return "GeoSearchCursor"; }
            BSONObj query() const { return fromjson( "{ loc : { $near : [50,50] } }" ); }
            void check( const shared_ptr<Cursor> &c ) {
                ASSERT( c->ok() );
                ASSERT( c->matcher() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT_EQUALS( 44, c->current().getIntField( "_id" ) );
                ASSERT( !c->advance() );
            }
        };
        
        class OutOfOrder : public QueryOptimizerCursorTests::Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 5 ) );
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSONObj(), BSON( "b" << 1 ) );
                ASSERT( !c );
            }
        };
        
        class BestSavedOutOfOrder : public QueryOptimizerCursorTests::Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 5 << "b" << BSON_ARRAY( 1 << 2 << 3 << 4 << 5 ) ) );
                _cli.insert( ns(), BSON( "_id" << 1 << "b" << 6 ) );
                _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                // record {_id:1} index for this query
                ASSERT( _cli.query( ns(), QUERY( "_id" << GT << 0 << "b" << GT << 0 ).sort( "b" ) )->more() );
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSON( "_id" << GT << 0 << "b" << GT << 0 ), BSON( "b" << 1 ) );
                // {_id:1} requires scan and order, so {b:1} must be chosen.
                ASSERT( c );
                ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
            }
        };

        /**
         * If an optimal plan is a candidate, return a cursor for it rather than a QueryOptimizerCursor.  Avoid
         * caching optimal plans since simple cursors will not save a plan anyway (so in the most common case optimal
         * plans won't be cached) and because this simplifies the implementation for selecting a simple cursor.
         */
        class BestSavedOptimal : public QueryOptimizerCursorTests::Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "_id" << 1 << "q" << 1 ) );
                // {_id:1} index not recorded for these queries since it is an optimal index.
                ASSERT( _cli.query( ns(), QUERY( "_id" << GT << 0 ) )->more() );
                ASSERT( _cli.query( ns(), QUERY( "$or" << BSON_ARRAY( BSON( "_id" << GT << 0 ) ) ) )->more() );
                dblock lk;
                Client::Context ctx( ns() );
                // Check that no plan was recorded for this query.
                ASSERT( BSONObj().woCompare( NamespaceDetailsTransient::get_inlock( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "_id" << GT << 0 ), true ).pattern() ) ) == 0 );
                shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSON( "_id" << GT << 0 ) );
                // No need for query optimizer cursor since the plan is optimal.
                ASSERT_EQUALS( "BtreeCursor _id_", c->toString() );
            }
        };
        
        /** If a no optimal plan is a candidate a QueryOptimizerCursor should be returned, even if plan has been recorded. */
        class BestSavedNotOptimal : public QueryOptimizerCursorTests::Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 << "q" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "q" << 1 ) );
                // Record {_id:1} index for this query
                ASSERT( _cli.query( ns(), QUERY( "q" << 1 << "_id" << 1 ) )->more() );
                dblock lk;
                Client::Context ctx( ns() );
                ASSERT( BSON( "_id" << 1 ).woCompare( NamespaceDetailsTransient::get_inlock( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "q" << 1 << "_id" << 1 ), true ).pattern() ) ) == 0 );
                shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSON( "q" << 1 << "_id" << 1 ) );
                // Need query optimizer cursor since the cached plan is not optimal.
                ASSERT_EQUALS( "QueryOptimizerCursor", c->toString() );
            }
        };
        
        class MultiIndex : public Base {
        public:
            MultiIndex() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            }
            string expectedType() const { return "QueryOptimizerCursor"; }
            BSONObj query() const { return BSON( "_id" << GT << 0 << "a" << GT << 0 ); }
            void check( const shared_ptr<Cursor> &c ) {}
        };
        
        namespace RequireIndex {
            
            class Base : public GetCursor::Base {
                const QueryPlanSelectionPolicy &planPolicy() const {
                    return QueryPlanSelectionPolicy::indexOnly();
                }
            };
            
            class NoConstraints : public Base {
                bool expectException() const { return true; }
            };

            class SimpleId : public Base {
                string expectedType() const { return "BtreeCursor _id_"; }
                BSONObj query() const { return BSON( "_id" << 5 ); }
            };

            class UnindexedQuery : public Base {
                bool expectException() const { return true; }
                BSONObj query() const { return BSON( "a" << GTE << 5 ); }
            };

            class IndexedQuery : public Base {
            public:
                IndexedQuery() {
                    _cli.insert( ns(), BSON( "_id" << 6 << "a" << 6 << "c" << 4 ) );
                    _cli.ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 << "c" << 1 ) );
                }
                string expectedType() const { return "QueryOptimizerCursor"; }
                BSONObj query() const { return BSON( "a" << GTE << 5 << "c" << 4 ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->matcher() );
                    ASSERT_EQUALS( 6, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            };

            class SecondOrClauseIndexed : public Base {
            public:
                SecondOrClauseIndexed() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                    _cli.insert( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "b" << 1 ) );
                }
                string expectedType() const { return "QueryOptimizerCursor"; }
                BSONObj query() const { return fromjson( "{$or:[{a:1},{b:1}]}" ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->matcher() );
                    ASSERT( c->advance() );
                    ASSERT( !c->advance() ); // 2 matches exactly
                }
            };
            
            class SecondOrClauseUnindexed : public Base {
            public:
                SecondOrClauseUnindexed() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "a" << 1 ) );
                }
                bool expectException() const { return true; }
                BSONObj query() const { return fromjson( "{$or:[{a:1},{b:1}]}" ); }
            };

            class SecondOrClauseUnindexedUndetected : public Base {
            public:
                SecondOrClauseUnindexedUndetected() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 ) );
                    _cli.insert( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "b" << 1 ) );
                }
                string expectedType() const { return "QueryOptimizerCursor"; }
                BSONObj query() const { return fromjson( "{$or:[{a:1},{b:1}]}" ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->matcher() );
                    // An unindexed cursor is required for the second clause, but is not allowed.
                    ASSERT_THROWS( c->advance(), MsgAssertionException );
                }
            };
            
            class RecordedUnindexedPlan : public Base {
            public:
                RecordedUnindexedPlan() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 << 3 ) << "b" << 1 ) );
                    BSONObj explain = _cli.findOne( ns(), QUERY( "a" << GT << 0 << "b" << 1 ).explain() );
                    ASSERT_EQUALS( "BasicCursor", explain[ "cursor" ].String() );
                }
                string expectedType() const { return "QueryOptimizerCursor"; }
                BSONObj query() const { return BSON( "a" << GT << 0 << "b" << 1 ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );
                    while( c->advance() ) {
                        ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );                    
                    }
                }
            };
                
        } // namespace RequireIndex
        
        namespace IdElseNatural {

            class Base : public GetCursor::Base {
                const QueryPlanSelectionPolicy &planPolicy() const {
                    return QueryPlanSelectionPolicy::idElseNatural();
                }
            };
            
            class AllowOptimalNaturalPlan : public Base {
                string expectedType() const { return "BasicCursor"; }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( !c->matcher() );
                    ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            };

            class AllowOptimalIdPlan : public Base {
                string expectedType() const { return "BtreeCursor _id_"; }
                BSONObj query() const { return BSON( "_id" << 5 ); }
            };

            class HintedIdForQuery : public Base {
            public:
                HintedIdForQuery( const BSONObj &query ) : _query( query ) {
                    _cli.remove( ns(), BSONObj() );
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                }
                string expectedType() const { return "BtreeCursor _id_"; }
                BSONObj query() const { return _query; }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->currentMatches() );
                    ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            private:
                BSONObj _query;
            };

            class HintedNaturalForQuery : public Base {
            public:
                HintedNaturalForQuery( const BSONObj &query ) : _query( query ) {
                    _cli.dropCollection( ns() );
                    _cli.createCollection( ns(), 1024, true );
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                }
                ~HintedNaturalForQuery() {
                    _cli.dropCollection( ns() );
                }
                string expectedType() const { return "ForwardCappedCursor"; }
                BSONObj query() const { return _query; }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->currentMatches() );
                    ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            private:
                BSONObj _query;
            };

        } // namespace IdElseNatural
        
    } // namespace GetCursor
    
    class All : public Suite {
    public:
        All() : Suite( "queryoptimizercursor" ) {}
        
        void setupTests() {
            __forceLinkGeoPlugin();
            add<QueryOptimizerCursorTests::CachedMatchCounterCount>();
            add<QueryOptimizerCursorTests::CachedMatchCounterAccumulate>();
            add<QueryOptimizerCursorTests::CachedMatchCounterDedup>();
            add<QueryOptimizerCursorTests::CachedMatchCounterNscanned>();
            add<QueryOptimizerCursorTests::SmallDupSetUpgrade>();
            add<QueryOptimizerCursorTests::CachedMatchCounterCount>();
            add<QueryOptimizerCursorTests::SmallDupSetUpgradeRead>();
            add<QueryOptimizerCursorTests::SmallDupSetUpgradeWrite>();
            add<QueryOptimizerCursorTests::Empty>();
            add<QueryOptimizerCursorTests::Unindexed>();
            add<QueryOptimizerCursorTests::Basic>();
            add<QueryOptimizerCursorTests::NoMatch>();
            add<QueryOptimizerCursorTests::Interleaved>();
            add<QueryOptimizerCursorTests::NotMatch>();
            add<QueryOptimizerCursorTests::StopInterleaving>();
            add<QueryOptimizerCursorTests::TakeoverWithDup>();
            add<QueryOptimizerCursorTests::TakeoverWithNonMatches>();
            add<QueryOptimizerCursorTests::TakeoverWithTakeoverDup>();
            add<QueryOptimizerCursorTests::BasicOr>();
            add<QueryOptimizerCursorTests::OrFirstClauseEmpty>();
            add<QueryOptimizerCursorTests::OrSecondClauseEmpty>();
            add<QueryOptimizerCursorTests::OrMultipleClausesEmpty>();
            add<QueryOptimizerCursorTests::TakeoverCountOr>();
            add<QueryOptimizerCursorTests::TakeoverEndOfOrClause>();
            add<QueryOptimizerCursorTests::TakeoverBeforeEndOfOrClause>();
            add<QueryOptimizerCursorTests::TakeoverAfterEndOfOrClause>();
            add<QueryOptimizerCursorTests::ManualMatchingDeduping>();
            add<QueryOptimizerCursorTests::ManualMatchingUsingCurrKey>();
            add<QueryOptimizerCursorTests::ManualMatchingDedupingTakeover>();
            add<QueryOptimizerCursorTests::Singlekey>();
            add<QueryOptimizerCursorTests::Multikey>();
            add<QueryOptimizerCursorTests::AddOtherPlans>();
            add<QueryOptimizerCursorTests::AddOtherPlansDelete>();
            add<QueryOptimizerCursorTests::AddOtherPlansContinuousDelete>();
            add<QueryOptimizerCursorTests::OrRangeElimination>();
            add<QueryOptimizerCursorTests::OrDedup>();
            add<QueryOptimizerCursorTests::EarlyDups>();
            add<QueryOptimizerCursorTests::OrPopInTakeover>();
            add<QueryOptimizerCursorTests::OrCollectionScanAbort>();
            add<QueryOptimizerCursorTests::YieldNoOp>();
            add<QueryOptimizerCursorTests::YieldDelete>();
            add<QueryOptimizerCursorTests::YieldDeleteContinue>();
            add<QueryOptimizerCursorTests::YieldDeleteContinueFurther>();
            add<QueryOptimizerCursorTests::YieldUpdate>();
            add<QueryOptimizerCursorTests::YieldDrop>();
            add<QueryOptimizerCursorTests::YieldDropOr>();
            add<QueryOptimizerCursorTests::YieldRemoveOr>();
            add<QueryOptimizerCursorTests::YieldCappedOverwrite>();
            add<QueryOptimizerCursorTests::YieldDropIndex>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansNoOp>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansAdvanceNoOp>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansDelete>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansDeleteOr>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansDeleteOrAdvance>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansCappedOverwrite>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansCappedOverwriteManual>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansCappedOverwriteManual2>();
            add<QueryOptimizerCursorTests::YieldTakeover>();
            add<QueryOptimizerCursorTests::YieldTakeoverBasic>();
            add<QueryOptimizerCursorTests::YieldInactiveCursorAdvance>();
            add<QueryOptimizerCursorTests::OrderId>();
            add<QueryOptimizerCursorTests::OrderMultiIndex>();
            add<QueryOptimizerCursorTests::OrderReject>();
            add<QueryOptimizerCursorTests::OrderNatural>();
            add<QueryOptimizerCursorTests::OrderUnindexed>();
            add<QueryOptimizerCursorTests::RecordedOrderInvalid>();
            add<QueryOptimizerCursorTests::KillOp>();
            add<QueryOptimizerCursorTests::KillOpFirstClause>();
            add<QueryOptimizerCursorTests::Nscanned>();
            add<QueryOptimizerCursorTests::TouchEarlierIterate>();
            add<QueryOptimizerCursorTests::TouchEarlierIterateDelete>();
            add<QueryOptimizerCursorTests::TouchEarlierIterateDeleteMultiple>();
            add<QueryOptimizerCursorTests::TouchEarlierIterateTakeover>();
            add<QueryOptimizerCursorTests::TouchEarlierIterateTakeoverDeleteMultiple>();
            add<QueryOptimizerCursorTests::TouchEarlierIterateUnindexedTakeoverDeleteMultiple>();
            add<QueryOptimizerCursorTests::TouchEarlierIterateTakeoverDeleteMultipleMultiAdvance>();
            add<QueryOptimizerCursorTests::InitialCappedWrapYieldRecoveryFailure>();
            add<QueryOptimizerCursorTests::TakeoverCappedWrapYieldRecoveryFailure>();
            add<QueryOptimizerCursorTests::InvalidateClientCursorHolder>();
            add<QueryOptimizerCursorTests::TimeoutClientCursorHolder>();
            add<QueryOptimizerCursorTests::GetCursor::NoConstraints>();
            add<QueryOptimizerCursorTests::GetCursor::SimpleId>();
            add<QueryOptimizerCursorTests::GetCursor::OptimalIndex>();
            add<QueryOptimizerCursorTests::GetCursor::SimpleKeyMatch>();
            add<QueryOptimizerCursorTests::GetCursor::Geo>();
            add<QueryOptimizerCursorTests::GetCursor::OutOfOrder>();
            add<QueryOptimizerCursorTests::GetCursor::BestSavedOutOfOrder>();
            add<QueryOptimizerCursorTests::GetCursor::BestSavedOptimal>();
            add<QueryOptimizerCursorTests::GetCursor::BestSavedNotOptimal>();
            add<QueryOptimizerCursorTests::GetCursor::MultiIndex>();
            add<QueryOptimizerCursorTests::GetCursor::RequireIndex::NoConstraints>();
            add<QueryOptimizerCursorTests::GetCursor::RequireIndex::SimpleId>();
            add<QueryOptimizerCursorTests::GetCursor::RequireIndex::UnindexedQuery>();
            add<QueryOptimizerCursorTests::GetCursor::RequireIndex::IndexedQuery>();
            add<QueryOptimizerCursorTests::GetCursor::RequireIndex::SecondOrClauseIndexed>();
            add<QueryOptimizerCursorTests::GetCursor::RequireIndex::SecondOrClauseUnindexed>();
            add<QueryOptimizerCursorTests::GetCursor::RequireIndex::SecondOrClauseUnindexedUndetected>();
            add<QueryOptimizerCursorTests::GetCursor::RequireIndex::RecordedUnindexedPlan>();
            add<QueryOptimizerCursorTests::GetCursor::IdElseNatural::AllowOptimalNaturalPlan>();
            add<QueryOptimizerCursorTests::GetCursor::IdElseNatural::AllowOptimalIdPlan>();
            add<QueryOptimizerCursorTests::GetCursor::IdElseNatural::HintedIdForQuery>( BSON( "_id" << 1 ) );
            add<QueryOptimizerCursorTests::GetCursor::IdElseNatural::HintedIdForQuery>( BSON( "a" << 1 ) );
            add<QueryOptimizerCursorTests::GetCursor::IdElseNatural::HintedIdForQuery>( BSON( "_id" << 1 << "a" << 1 ) );
            add<QueryOptimizerCursorTests::GetCursor::IdElseNatural::HintedNaturalForQuery>( BSONObj() );
            add<QueryOptimizerCursorTests::GetCursor::IdElseNatural::HintedNaturalForQuery>( BSON( "_id" << 1 ) );
            add<QueryOptimizerCursorTests::GetCursor::IdElseNatural::HintedNaturalForQuery>( BSON( "a" << 1 ) );
            add<QueryOptimizerCursorTests::GetCursor::IdElseNatural::HintedNaturalForQuery>( BSON( "_id" << 1 << "a" << 1 ) );
        }
    } myall;
    
} // namespace QueryOptimizerTests

