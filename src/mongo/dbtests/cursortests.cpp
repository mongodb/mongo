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

    namespace BtreeCursor {

        using mongo::BtreeCursor;

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
                const char *ns = "unittests.cursortests.BtreeCursorTests.MultiRange";
                {
                    DBDirectClient c;
                    for( int i = 0; i < 10; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    ASSERT( c.ensureIndex( ns, BSON( "a" << 1 ) ) );
                }
                int v[] = { 1, 2, 4, 6 };
                boost::shared_ptr< FieldRangeVector > frv( vec( v, 4 ) );
                Client::WriteContext ctx( ns );
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
                Client::WriteContext ctx( ns );
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
                const char *ns = "unittests.cursortests.BtreeCursorTests.MultiRangeReverse";
                {
                    DBDirectClient c;
                    for( int i = 0; i < 10; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    ASSERT( c.ensureIndex( ns, BSON( "a" << 1 ) ) );
                }
                int v[] = { 1, 2, 4, 6 };
                boost::shared_ptr< FieldRangeVector > frv( vec( v, 4, -1 ) );
                Client::WriteContext ctx( ns );
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

                Client::WriteContext ctx( ns() );
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
                IndexSpec idx( BSON( "a" << 1 << "b" << 1 ) );
                _c.ensureIndex( ns(), idx.keyPattern );
                for( int i = 0; i < 300; ++i ) {
                    _c.insert( ns(), BSON( "a" << i << "b" << 5 ) );
                }
                FieldRangeSet frs( ns(), BSON( "b" << 3 ), true );
                boost::shared_ptr<FieldRangeVector> frv( new FieldRangeVector( frs, idx, 1 ) );
                Client::WriteContext ctx( ns() );
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

    } // namespace BtreeCursor
    
    namespace ClientCursor {

        using mongo::ClientCursor;
        
        static const char * const ns() { return "unittests.cursortests.clientcursor"; }
        DBDirectClient client;

        class Base {
        public:
            virtual ~Base() {
                client.dropCollection( ns() );
            }
        };        
        
        /**
         * ClientCursor::aboutToDelete() advances a ClientCursor with a refLoc() matching the
         * document to be deleted.
         */
        class AboutToDelete : public Base {
        public:
            void run() {
                populateData();
                Client::WriteContext ctx( ns() );

                // Generate a cursor from the supplied query and advance it to the iterate to be
                // deleted.
                shared_ptr<Cursor> cursor = NamespaceDetailsTransient::getCursor( ns(), query() );
                while( !isExpectedIterate( cursor->current() ) ) {
                    ASSERT( cursor->advance() );
                }
                ClientCursor::Holder clientCursor;
                clientCursor.reset( new ClientCursor( QueryOption_NoCursorTimeout, cursor, ns() ) );
                DiskLoc loc = clientCursor->currLoc();
                ASSERT( !loc.isNull() );
                
                // Yield the cursor.
                ClientCursor::YieldData data;
                clientCursor->prepareToYield( data );
                // The cursor will be advanced in aboutToDelete().
                ClientCursor::aboutToDelete( loc );
                clientCursor->recoverFromYield( data );
                ASSERT( clientCursor->ok() );
                
                // Validate the expected cursor advancement.
                validateIterateAfterYield( clientCursor->current() );
            }
        protected:
            virtual void populateData() const {
                client.insert( ns(), BSON( "a" << 1 ) );
                client.insert( ns(), BSON( "a" << 2 ) );
            }
            virtual BSONObj query() const { return BSONObj(); }
            virtual bool isExpectedIterate( const BSONObj &current ) const {
                return 1 == current[ "a" ].number();
            }
            virtual void validateIterateAfterYield( const BSONObj &current ) const {
                ASSERT_EQUALS( 2, current[ "a" ].number() );
            }
        };
        
        /** aboutToDelete() advances past a document referenced by adjacent cursor iterates. */
        class AboutToDeleteDuplicate : public AboutToDelete {
            virtual void populateData() const {
                client.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 ) ) );
                client.insert( ns(), BSON( "a" << 3 ) );
                client.ensureIndex( ns(), BSON( "a" << 1 ) );
            }
            virtual BSONObj query() const { return BSON( "a" << GT << 0 ); }
            virtual bool isExpectedIterate( const BSONObj &current ) const {
                return BSON_ARRAY( 1 << 2 ) == current[ "a" ].embeddedObject();
            }
            virtual void validateIterateAfterYield( const BSONObj &current ) const {
                ASSERT_EQUALS( 3, current[ "a" ].number() );                
            }
        };
        
        /** aboutToDelete() advances past a document referenced by adjacent cursor clauses. */
        class AboutToDeleteDuplicateNextClause : public AboutToDelete {
            virtual void populateData() const {
                for( int i = 119; i >= 0; --i ) {
                    client.insert( ns(), BSON( "a" << i ) );
                }
                client.ensureIndex( ns(), BSON( "a" << 1 ) );
                client.ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 ) );
            }
            virtual BSONObj query() const { return fromjson( "{$or:[{a:{$gte:0}},{b:1}]}" ); }
            virtual bool isExpectedIterate( const BSONObj &current ) const {
                // In the absence of the aboutToDelete() call, the next iterate will be a:119 from
                // an unindexed cursor over the second clause.
                return 119 == current[ "a" ].number();
            }
            virtual void validateIterateAfterYield( const BSONObj &current ) const {
                // After the aboutToDelete() call, the a:119 iterate of the unindexed cursor is
                // skipped, advancing to the a:118 iterate.
                ASSERT_EQUALS( 118, current[ "a" ].number() );                
            }
        };

        namespace Pin {

            class Base {
            public:
                Base() :
                    _ctx( ns() ),
                    _cursor( theDataFileMgr.findAll( ns() ) ) {
                        ASSERT( _cursor );
                        _clientCursor.reset( new ClientCursor( 0, _cursor, ns() ) );
                }
            protected:
                CursorId cursorid() const { return _clientCursor->cursorid(); }
            private:
                Client::WriteContext _ctx;
                boost::shared_ptr<Cursor> _cursor;
                ClientCursor::Holder _clientCursor;
            };
            
            /** Pin pins a ClientCursor over its lifetime. */
            class PinCursor : public Base {
            public:
                void run() {
                    assertNotPinned();
                    {
                        ClientCursor::Pin pin( cursorid() );
                        assertPinned();
                        ASSERT_THROWS( erase(), AssertionException );
                    }
                    assertNotPinned();
                    ASSERT( erase() );
                }
            private:
                void assertPinned() const {
                    ASSERT( ClientCursor::find( cursorid() ) );
                }
                void assertNotPinned() const {
                    ASSERT_THROWS( ClientCursor::find( cursorid() ), AssertionException );
                }
                bool erase() const {
                    return ClientCursor::erase( cursorid() );
                }
            };
            
            /** A ClientCursor cannot be pinned twice. */
            class PinTwice : public Base {
            public:
                void run() {
                    ClientCursor::Pin pin( cursorid() );
                    ASSERT_THROWS( pinCursor(), AssertionException );
                }
            private:
                void pinCursor() const {
                    ClientCursor::Pin pin( cursorid() );
                }
            };
            
            /** Pin behaves properly if its ClientCursor is destroyed early. */
            class CursorDeleted : public Base {
            public:
                void run() {
                    ClientCursor::Pin pin( cursorid() );
                    ASSERT( pin.c() );
                    // Delete the pinned cursor.
                    ClientCursor::invalidate( ns() );
                    ASSERT( !pin.c() );
                    // pin is destroyed safely, even though its ClientCursor was already destroyed.
                }
            };
            
        } // namespace Pin

    } // namespace ClientCursor
    
    class All : public Suite {
    public:
        All() : Suite( "cursor" ) {}

        void setupTests() {
            add<BtreeCursor::MultiRange>();
            add<BtreeCursor::MultiRangeGap>();
            add<BtreeCursor::MultiRangeReverse>();
            add<BtreeCursor::EqEq>();
            add<BtreeCursor::EqRange>();
            add<BtreeCursor::EqIn>();
            add<BtreeCursor::RangeEq>();
            add<BtreeCursor::RangeIn>();
            add<BtreeCursor::AbortImplicitScan>();
            add<ClientCursor::AboutToDelete>();
            add<ClientCursor::AboutToDeleteDuplicate>();
            add<ClientCursor::AboutToDeleteDuplicateNextClause>();
            add<ClientCursor::Pin::PinCursor>();
            add<ClientCursor::Pin::PinTwice>();
            add<ClientCursor::Pin::CursorDeleted>();
        }
    } myall;
} // namespace CursorTests
