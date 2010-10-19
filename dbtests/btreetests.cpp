// btreetests.cpp : Btree unit tests
//

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

#include "../db/db.h"
#include "../db/btree.h"

#include "dbtests.h"

namespace BtreeTests {

    const char* ns() {
        return "unittests.btreetests";
    }
    
    class Ensure {
    public:
        Ensure() {
            _c.ensureIndex( ns(), BSON( "a" << 1 ), false, "testIndex" );
        }
        ~Ensure() {
            _c.dropIndexes( ns() );
        }
    private:
        DBDirectClient _c;
    };
    
    class Base : public Ensure {
    public:
        Base() : 
            _context( ns() ) {            
            {
                bool f = false;
                assert( f = true );
                massert( 10402 , "assert is misdefined", f);
            }
        }
    protected:
        BtreeBucket* bt() {
            return id().head.btree();
        }
        DiskLoc dl() {
            return id().head;
        }
        IndexDetails& id() {
            NamespaceDetails *nsd = nsdetails( ns() );
            assert( nsd );
            return nsd->idx( 1 );
        }
        // dummy, valid record loc
        static DiskLoc recordLoc() {
            return DiskLoc( 0, 2 );
        }
        void checkValid( int nKeys ) {
            ASSERT( bt() );
            ASSERT( bt()->isHead() );
            bt()->assertValid( order(), true );
            ASSERT_EQUALS( nKeys, bt()->fullValidate( dl(), order() ) );
        }
        void dump() {
            bt()->dumpTree( dl(), order() );
        }
        void insert( BSONObj &key ) {
            bt()->bt_insert( dl(), recordLoc(), key, Ordering::make(order()), true, id(), true );
        }
        void unindex( BSONObj &key ) {
            bt()->unindex( dl(), id(), key, recordLoc() );
        }
        static BSONObj simpleKey( char c, int n = 1 ) {
            BSONObjBuilder builder;
            string val( n, c );
            builder.append( "a", val );
            return builder.obj();
        }
        void locate( BSONObj &key, int expectedPos,
                     bool expectedFound, const DiskLoc &expectedLocation,
                     int direction = 1 ) {
            int pos;
            bool found;
            DiskLoc location =
                bt()->locate( id(), dl(), key, Ordering::make(order()), pos, found, recordLoc(), direction );
            ASSERT_EQUALS( expectedFound, found );
            ASSERT( location == expectedLocation );
            ASSERT_EQUALS( expectedPos, pos );
        }
        BSONObj order() {
            return id().keyPattern();
        }
        BtreeBucket *child( BtreeBucket *b, int i ) {
            assert( i <= b->nKeys() );
            DiskLoc d;
            if ( i == b->nKeys() ) {
                d = b->getNextChild();
            } else {
                d = const_cast< DiskLoc& >( b->keyNode( i ).prevChildBucket );
            }
            assert( !d.isNull() );
            return d.btree();
        }
    private:
        dblock lk_;
        Client::Context _context;
    };

    class Create : public Base {
    public:
        void run() {
            checkValid( 0 );
        }
    };

    class SimpleInsertDelete : public Base {
    public:
        void run() {
            BSONObj key = simpleKey( 'z' );
            insert( key );

            checkValid( 1 );
            locate( key, 0, true, dl() );

            unindex( key );

            checkValid( 0 );
            locate( key, 0, false, DiskLoc() );
        }
    };

    class SplitUnevenBucketBase : public Base {
    public:
        virtual ~SplitUnevenBucketBase() {}
        void run() {
            for ( int i = 0; i < 10; ++i ) {
                BSONObj shortKey = simpleKey( shortToken( i ), 1 );
                insert( shortKey );
                BSONObj longKey = simpleKey( longToken( i ), 800 );
                insert( longKey );
            }
            checkValid( 20 );
            ASSERT_EQUALS( 1, bt()->nKeys() );
            checkSplit();
        }
    protected:
        virtual char shortToken( int i ) const = 0;
        virtual char longToken( int i ) const = 0;
        static char leftToken( int i ) {
            return 'a' + i;
        }
        static char rightToken( int i ) {
            return 'z' - i;
        }
        virtual void checkSplit() = 0;
    };

    class SplitRightHeavyBucket : public SplitUnevenBucketBase {
    private:
        virtual char shortToken( int i ) const {
            return leftToken( i );
        }
        virtual char longToken( int i ) const {
            return rightToken( i );
        }
        virtual void checkSplit() {
            ASSERT_EQUALS( 15, child( bt(), 0 )->nKeys() );
            ASSERT_EQUALS( 4, child( bt(), 1 )->nKeys() );            
        }
    };

    class SplitLeftHeavyBucket : public SplitUnevenBucketBase {
    private:
        virtual char shortToken( int i ) const {
            return rightToken( i );
        }
        virtual char longToken( int i ) const {
            return leftToken( i );
        }
        virtual void checkSplit() {
            ASSERT_EQUALS( 4, child( bt(), 0 )->nKeys() );
            ASSERT_EQUALS( 15, child( bt(), 1 )->nKeys() );            
        }
    };

    class MissingLocate : public Base {
    public:
        void run() {
            for ( int i = 0; i < 3; ++i ) {
                BSONObj k = simpleKey( 'b' + 2 * i );
                insert( k );
            }

            locate( 1, 'a', 'b', dl() );
            locate( 1, 'c', 'd', dl() );
            locate( 1, 'e', 'f', dl() );
            locate( 1, 'g', 'g' + 1, DiskLoc() ); // of course, 'h' isn't in the index.

            // old behavior
            //       locate( -1, 'a', 'b', dl() );
            //       locate( -1, 'c', 'd', dl() );
            //       locate( -1, 'e', 'f', dl() );
            //       locate( -1, 'g', 'f', dl() );

            locate( -1, 'a', 'a' - 1, DiskLoc() ); // of course, 'a' - 1 isn't in the index
            locate( -1, 'c', 'b', dl() );
            locate( -1, 'e', 'd', dl() );
            locate( -1, 'g', 'f', dl() );
        }
    private:
        void locate( int direction, char token, char expectedMatch,
                     DiskLoc expectedLocation ) {
            BSONObj k = simpleKey( token );
            int expectedPos = ( expectedMatch - 'b' ) / 2;
            Base::locate( k, expectedPos, false, expectedLocation, direction );
        }
    };

    class MissingLocateMultiBucket : public Base {
    public:
        void run() {
            for ( int i = 0; i < 8; ++i ) {
                insert( i );
            }
            insert( 9 );
            insert( 8 );
//            dump();
            BSONObj straddle = key( 'i' );
            locate( straddle, 0, false, dl(), 1 );
            straddle = key( 'k' );
            locate( straddle, 0, false, dl(), -1 );
        }
    private:
        BSONObj key( char c ) {
            return simpleKey( c, 800 );
        }
        void insert( int i ) {
            BSONObj k = key( 'b' + 2 * i );
            Base::insert( k );            
        }
    };

    class SERVER983 : public Base {
    public:
        void run() {
            for ( int i = 0; i < 10; ++i ) {
                insert( i );
            }
//            dump();
            BSONObj straddle = key( 'o' );
            locate( straddle, 0, false, dl(), 1 );
            straddle = key( 'q' );
            locate( straddle, 0, false, dl(), -1 );
        }
    private:
        BSONObj key( char c ) {
            return simpleKey( c, 800 );
        }
        void insert( int i ) {
            BSONObj k = key( 'b' + 2 * i );
            Base::insert( k );            
        }        
    };

    class ReuseUnused : public Base {
    public:
        void run() {
            for ( int i = 0; i < 10; ++i ) {
                insert( i );
            }
//            dump();
            BSONObj root = key( 'p' );
            unindex( root );
            Base::insert( root );
            locate( root, 0, true, dl(), 1 );
        }
    private:
        BSONObj key( char c ) {
            return simpleKey( c, 800 );
        }
        void insert( int i ) {
            BSONObj k = key( 'b' + 2 * i );
            Base::insert( k );            
        }        
    };
    
    class PackUnused : public Base {
    public:
        void run() {
            for ( long long i = 0; i < 1000000; i += 1000 ) {
                insert( i );
            }
            string orig, after;
            {
                stringstream ss;
                bt()->shape( ss );
                orig = ss.str();
            }
            vector< string > toDel;
            vector< string > other;
            BSONObjBuilder start;
            start.appendMinKey( "a" );
            BSONObjBuilder end;
            end.appendMaxKey( "a" );
            auto_ptr< BtreeCursor > c( new BtreeCursor( nsdetails( ns() ), 1, id(), start.done(), end.done(), false, 1 ) );
            while( c->ok() ) {
                if ( !c->currKeyNode().prevChildBucket.isNull() ) {
                    toDel.push_back( c->currKey().firstElement().valuestr() );
                } else {
                    other.push_back( c->currKey().firstElement().valuestr() );                    
                }
                c->advance();
            }
            ASSERT( toDel.size() > 0 );
            for( vector< string >::const_iterator i = toDel.begin(); i != toDel.end(); ++i ) {
                BSONObj o = BSON( "a" << *i );
                unindex( o );
            }
            ASSERT( other.size() > 0 );
            for( vector< string >::const_iterator i = other.begin(); i != other.end(); ++i ) {
                BSONObj o = BSON( "a" << *i );
                unindex( o );
            }

            int unused = 0;
            ASSERT_EQUALS( 0, bt()->fullValidate( dl(), order(), &unused ) );

            for ( long long i = 50000; i < 50100; ++i ) {
                insert( i );
            }            

            int unused2 = 0;
            ASSERT_EQUALS( 100, bt()->fullValidate( dl(), order(), &unused2 ) );

            ASSERT( unused2 < unused );
        }
    protected:
        void insert( long long n ) {
            string val( 800, ' ' );
            for( int i = 0; i < 800; i += 8 ) {
                for( int j = 0; j < 8; ++j ) {
                    // probably we won't get > 56 bits
                    unsigned char v = 0x80 | ( n >> ( ( 8 - j - 1 ) * 7 ) & 0x000000000000007f );
                    val[ i + j ] = v;
                }
            }
            BSONObj k = BSON( "a" << val );
            Base::insert( k );            
        }        
    };

    class DontDropReferenceKey : public PackUnused {
    public:
        void run() {
            // with 80 root node is full
            for ( long long i = 0; i < 80; i += 1 ) {
                insert( i );
            }
            
            BSONObjBuilder start;
            start.appendMinKey( "a" );
            BSONObjBuilder end;
            end.appendMaxKey( "a" );
            BSONObj l = bt()->keyNode( 0 ).key;
            string toInsert;
            auto_ptr< BtreeCursor > c( new BtreeCursor( nsdetails( ns() ), 1, id(), start.done(), end.done(), false, 1 ) );
            while( c->ok() ) {
                if ( c->currKey().woCompare( l ) > 0 ) {
                    toInsert = c->currKey().firstElement().valuestr();
                    break;
                }
                c->advance();
            }
            // too much work to try to make this happen through inserts and deletes
            // we are intentionally manipulating the btree bucket directly here
            dur::writingDiskLoc( const_cast< DiskLoc& >( bt()->keyNode( 1 ).prevChildBucket ) ) = DiskLoc();
            dur::writingInt( const_cast< DiskLoc& >( bt()->keyNode( 1 ).recordLoc ).GETOFS() ) |= 1; // make unused
            BSONObj k = BSON( "a" << toInsert );
            Base::insert( k );
        }
    };
    
    class All : public Suite {
    public:
        All() : Suite( "btree" ){
        }
        
        void setupTests(){
            add< Create >();
            add< SimpleInsertDelete >();
            add< SplitRightHeavyBucket >();
            add< SplitLeftHeavyBucket >();
            add< MissingLocate >();
            add< MissingLocateMultiBucket >();
            add< SERVER983 >();
            add< ReuseUnused >();
            add< PackUnused >();
            add< DontDropReferenceKey >();
        }
    } myall;
}

