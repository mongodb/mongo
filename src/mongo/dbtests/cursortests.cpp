// cusrortests.cpp // cursor related unit tests
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
#include "../db/clientcursor.h"
#include "../db/instance.h"
#include "../db/btree.h"
#include "../db/queryutil.h"
#include "dbtests.h"

namespace CursorTests {

    namespace BtreeCursorTests {

        // The ranges expressed in these tests are impossible given our query
        // syntax, so going to do them a hacky way.

        class Base {
        protected:
            static const char *ns() { return "unittests.cursortests.Base"; }
            FieldRangeVector *vec( int *vals, int len, int direction = 1 ) {
                FieldRangeSet s( "", BSON( "a" << 1 ), true );
                for( int i = 0; i < len; i += 2 ) {
                    _objs.push_back( BSON( "a" << BSON( "$gte" << vals[ i ] << "$lte" << vals[ i + 1 ] ) ) );
                    FieldRangeSet s2( "", _objs.back(), true );
                    if ( i == 0 ) {
                        s.range( "a" ) = s2.range( "a" );
                    }
                    else {
                        s.range( "a" ) |= s2.range( "a" );
                    }
                }
                // orphan idxSpec for this test
                IndexSpec *idxSpec = new IndexSpec( BSON( "a" << 1 ) );
                return new FieldRangeVector( s, *idxSpec, direction );
            }
            DBDirectClient _c;
        private:
            vector< BSONObj > _objs;
        };

        class MultiRange : public Base {
        public:
            void run() {
                Lock::GlobalWrite lk;
                const char *ns = "unittests.cursortests.BtreeCursorTests.MultiRange";
                {
                    DBDirectClient c;
                    for( int i = 0; i < 10; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    ASSERT( c.ensureIndex( ns, BSON( "a" << 1 ) ) );
                }
                int v[] = { 1, 2, 4, 6 };
                boost::shared_ptr< FieldRangeVector > frv( vec( v, 4 ) );
                Client::Context ctx( ns );
                scoped_ptr<BtreeCursor> _c( BtreeCursor::make( nsdetails( ns ), nsdetails( ns )->idx(1), frv, 1 ) );
                BtreeCursor &c = *_c.get();
                ASSERT_EQUALS( "BtreeCursor a_1 multi", c.toString() );
                double expected[] = { 1, 2, 4, 5, 6 };
                for( int i = 0; i < 5; ++i ) {
                    ASSERT( c.ok() );
                    ASSERT_EQUALS( expected[ i ], c.currKey().firstElement().number() );
                    c.advance();
                }
                ASSERT( !c.ok() );
            }
        };

        class MultiRangeGap : public Base {
        public:
            void run() {
                Lock::GlobalWrite lk;
                const char *ns = "unittests.cursortests.BtreeCursorTests.MultiRangeGap";
                {
                    DBDirectClient c;
                    for( int i = 0; i < 10; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    for( int i = 100; i < 110; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    ASSERT( c.ensureIndex( ns, BSON( "a" << 1 ) ) );
                }
                int v[] = { -50, 2, 40, 60, 109, 200 };
                boost::shared_ptr< FieldRangeVector > frv( vec( v, 6 ) );
                Client::Context ctx( ns );
                scoped_ptr<BtreeCursor> _c( BtreeCursor::make(nsdetails( ns ), nsdetails( ns )->idx(1), frv, 1 ) );
                BtreeCursor &c = *_c.get();
                ASSERT_EQUALS( "BtreeCursor a_1 multi", c.toString() );
                double expected[] = { 0, 1, 2, 109 };
                for( int i = 0; i < 4; ++i ) {
                    ASSERT( c.ok() );
                    ASSERT_EQUALS( expected[ i ], c.currKey().firstElement().number() );
                    c.advance();
                }
                ASSERT( !c.ok() );
            }
        };

        class MultiRangeReverse : public Base {
        public:
            void run() {
                Lock::GlobalWrite lk;
                const char *ns = "unittests.cursortests.BtreeCursorTests.MultiRangeReverse";
                {
                    DBDirectClient c;
                    for( int i = 0; i < 10; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    ASSERT( c.ensureIndex( ns, BSON( "a" << 1 ) ) );
                }
                int v[] = { 1, 2, 4, 6 };
                boost::shared_ptr< FieldRangeVector > frv( vec( v, 4, -1 ) );
                Client::Context ctx( ns );
                scoped_ptr<BtreeCursor> _c( BtreeCursor::make( nsdetails( ns ), nsdetails( ns )->idx(1), frv, -1 ) );
                BtreeCursor& c = *_c.get();
                ASSERT_EQUALS( "BtreeCursor a_1 reverse multi", c.toString() );
                double expected[] = { 6, 5, 4, 2, 1 };
                for( int i = 0; i < 5; ++i ) {
                    ASSERT( c.ok() );
                    ASSERT_EQUALS( expected[ i ], c.currKey().firstElement().number() );
                    c.advance();
                }
                ASSERT( !c.ok() );
            }
        };

        class Base2 {
        public:
            virtual ~Base2() { _c.dropCollection( ns() ); }
        protected:
            static const char *ns() { return "unittests.cursortests.Base2"; }
            DBDirectClient _c;
            virtual BSONObj idx() const = 0;
            virtual int direction() const { return 1; }
            void insert( const BSONObj &o ) {
                _objs.push_back( o );
                _c.insert( ns(), o );
            }
            void check( const BSONObj &spec ) {
                {
                    BSONObj keypat = idx();
                    //cout << keypat.toString() << endl;
                    _c.ensureIndex( ns(), idx() );
                }

                Client::Context ctx( ns() );
                FieldRangeSet frs( ns(), spec, true );
                // orphan spec for this test.
                IndexSpec *idxSpec = new IndexSpec( idx() );
                boost::shared_ptr< FieldRangeVector > frv( new FieldRangeVector( frs, *idxSpec, direction() ) );
                scoped_ptr<BtreeCursor> c( BtreeCursor::make( nsdetails( ns() ), nsdetails( ns() )->idx( 1 ), frv, direction() ) );
                Matcher m( spec );
                int count = 0;
                while( c->ok() ) {
                    ASSERT( m.matches( c->current() ) );
                    c->advance();
                    ++count;
                }
                int expectedCount = 0;
                for( vector< BSONObj >::const_iterator i = _objs.begin(); i != _objs.end(); ++i ) {
                    if ( m.matches( *i ) ) {
                        ++expectedCount;
                    }
                }
                ASSERT_EQUALS( expectedCount, count );
            }
        private:
            Lock::GlobalWrite _lk;
            vector< BSONObj > _objs;
        };

        class EqEq : public Base2 {
        public:
            void run() {
                insert( BSON( "a" << 4 << "b" << 5 ) );
                insert( BSON( "a" << 4 << "b" << 5 ) );
                insert( BSON( "a" << 4 << "b" << 4 ) );
                insert( BSON( "a" << 5 << "b" << 4 ) );
                check( BSON( "a" << 4 << "b" << 5 ) );
            }
            virtual BSONObj idx() const { return BSON( "a" << 1 << "b" << 1 ); }
        };

        class EqRange : public Base2 {
        public:
            void run() {
                insert( BSON( "a" << 3 << "b" << 5 ) );
                insert( BSON( "a" << 4 << "b" << 0 ) );
                insert( BSON( "a" << 4 << "b" << 5 ) );
                insert( BSON( "a" << 4 << "b" << 6 ) );
                insert( BSON( "a" << 4 << "b" << 6 ) );
                insert( BSON( "a" << 4 << "b" << 10 ) );
                insert( BSON( "a" << 4 << "b" << 11 ) );
                insert( BSON( "a" << 5 << "b" << 5 ) );
                check( BSON( "a" << 4 << "b" << BSON( "$gte" << 1 << "$lte" << 10 ) ) );
            }
            virtual BSONObj idx() const { return BSON( "a" << 1 << "b" << 1 ); }
        };

        class EqIn : public Base2 {
        public:
            void run() {
                insert( BSON( "a" << 3 << "b" << 5 ) );
                insert( BSON( "a" << 4 << "b" << 0 ) );
                insert( BSON( "a" << 4 << "b" << 5 ) );
                insert( BSON( "a" << 4 << "b" << 6 ) );
                insert( BSON( "a" << 4 << "b" << 6 ) );
                insert( BSON( "a" << 4 << "b" << 10 ) );
                insert( BSON( "a" << 4 << "b" << 11 ) );
                insert( BSON( "a" << 5 << "b" << 5 ) );
                check( BSON( "a" << 4 << "b" << BSON( "$in" << BSON_ARRAY( 5 << 6 << 11 ) ) ) );
            }
            virtual BSONObj idx() const { return BSON( "a" << 1 << "b" << 1 ); }
        };

        class RangeEq : public Base2 {
        public:
            void run() {
                insert( BSON( "a" << 0 << "b" << 4 ) );
                insert( BSON( "a" << 1 << "b" << 4 ) );
                insert( BSON( "a" << 4 << "b" << 3 ) );
                insert( BSON( "a" << 5 << "b" << 4 ) );
                insert( BSON( "a" << 7 << "b" << 4 ) );
                insert( BSON( "a" << 4 << "b" << 4 ) );
                insert( BSON( "a" << 9 << "b" << 6 ) );
                insert( BSON( "a" << 11 << "b" << 1 ) );
                insert( BSON( "a" << 11 << "b" << 4 ) );
                check( BSON( "a" << BSON( "$gte" << 1 << "$lte" << 10 ) << "b" << 4 ) );
            }
            virtual BSONObj idx() const { return BSON( "a" << 1 << "b" << 1 ); }
        };

        class RangeIn : public Base2 {
        public:
            void run() {
                insert( BSON( "a" << 0 << "b" << 4 ) );
                insert( BSON( "a" << 1 << "b" << 5 ) );
                insert( BSON( "a" << 4 << "b" << 3 ) );
                insert( BSON( "a" << 5 << "b" << 4 ) );
                insert( BSON( "a" << 7 << "b" << 5 ) );
                insert( BSON( "a" << 4 << "b" << 4 ) );
                insert( BSON( "a" << 9 << "b" << 6 ) );
                insert( BSON( "a" << 11 << "b" << 1 ) );
                insert( BSON( "a" << 11 << "b" << 4 ) );
                check( BSON( "a" << BSON( "$gte" << 1 << "$lte" << 10 ) << "b" << BSON( "$in" << BSON_ARRAY( 4 << 6 ) ) ) );
            }
            virtual BSONObj idx() const { return BSON( "a" << 1 << "b" << 1 ); }
        };
        
        class AbortImplicitScan : public Base {
        public:
            void run() {
                Lock::GlobalWrite lk;
                IndexSpec idx( BSON( "a" << 1 << "b" << 1 ) );
                _c.ensureIndex( ns(), idx.keyPattern );
                for( int i = 0; i < 300; ++i ) {
                    _c.insert( ns(), BSON( "a" << i << "b" << 5 ) );
                }
                FieldRangeSet frs( ns(), BSON( "b" << 3 ), true );
                boost::shared_ptr<FieldRangeVector> frv( new FieldRangeVector( frs, idx, 1 ) );
                Client::Context ctx( ns() );
                scoped_ptr<BtreeCursor> c( BtreeCursor::make( nsdetails( ns() ), nsdetails( ns() )->idx(1), frv, 1 ) );
                long long initialNscanned = c->nscanned();
                ASSERT( initialNscanned < 200 );
                ASSERT( c->ok() );
                c->advance();
                ASSERT( c->nscanned() > initialNscanned );
                ASSERT( c->nscanned() < 200 );
                ASSERT( c->ok() );
            }
        };

    } // namespace BtreeCursorTests

    class All : public Suite {
    public:
        All() : Suite( "cursor" ) {}

        void setupTests() {
            add< BtreeCursorTests::MultiRange >();
            add< BtreeCursorTests::MultiRangeGap >();
            add< BtreeCursorTests::MultiRangeReverse >();
            add< BtreeCursorTests::EqEq >();
            add< BtreeCursorTests::EqRange >();
            add< BtreeCursorTests::EqIn >();
            add< BtreeCursorTests::RangeEq >();
            add< BtreeCursorTests::RangeIn >();
            add< BtreeCursorTests::AbortImplicitScan >();
        }
    } myall;
} // namespace CursorTests
