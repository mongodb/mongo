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

#include "../db/btree.h"

#include "../db/db.h"

#include "dbtests.h"

namespace BtreeTests {

    class Base {
    public:
        Base() {
            setClient( ns() );
            BSONObjBuilder builder;
            builder.append( "ns", ns() );
            builder.append( "name", "testIndex" );
            BSONObj bobj = builder.done();
            idx_.info =
                theDataFileMgr.insert( ns(), bobj.objdata(), bobj.objsize() );
            idx_.head = BtreeBucket::addHead( idx_ );
        }
        ~Base() {
            // FIXME cleanup all btree buckets.
            theDataFileMgr.deleteRecord( ns(), idx_.info.rec(), idx_.info );
            ASSERT( theDataFileMgr.findAll( ns() )->eof() );
        }
    protected:
        BtreeBucket* bt() const {
            return idx_.head.btree();
        }
        DiskLoc dl() const {
            return idx_.head;
        }
        IndexDetails& id() {
            return idx_;
        }
        static const char* ns() {
            return "unittests.btreetests";
        }
        // dummy, valid record loc
        static DiskLoc recordLoc() {
            return DiskLoc( 0, 2 );
        }
        void checkValid( int nKeys ) const {
            ASSERT( bt() );
            ASSERT( bt()->isHead() );
            bt()->assertValid( order(), true );
            ASSERT_EQUALS( nKeys, bt()->fullValidate( dl(), order() ) );
        }
        void insert( BSONObj &key ) {
            bt()->bt_insert( dl(), recordLoc(), key, order(), true, id(), true );
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
                bt()->locate( id(), dl(), key, order(), pos, found, recordLoc(), direction );
            ASSERT_EQUALS( expectedFound, found );
            ASSERT( location == expectedLocation );
            ASSERT_EQUALS( expectedPos, pos );
        }
        BSONObj order() const {
            return idx_.keyPattern();
        }
    private:
        dblock lk_;
        IndexDetails idx_;
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
    };

    class SplitRightHeavyBucket : public SplitUnevenBucketBase {
    private:
        virtual char shortToken( int i ) const {
            return leftToken( i );
        }
        virtual char longToken( int i ) const {
            return rightToken( i );
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
            for ( int i = 0; i < 10; ++i ) {
                BSONObj k = key( 'b' + 2 * i );
                insert( k );
            }
            BSONObj straddle = key( 'i' );
            locate( straddle, 0, false, dl(), 1 );
            straddle = key( 'k' );
            locate( straddle, 0, false, dl(), -1 );
        }
    private:
        BSONObj key( char c ) {
            return simpleKey( c, 800 );
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
        }
    } myall;
}

