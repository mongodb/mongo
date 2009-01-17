// namespacetests.cpp : namespace.{h,cpp} unit tests.
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

// Where IndexDetails defined.
#include "../db/namespace.h"

#include "../db/db.h"
#include "../db/json.h"

#include "dbtests.h"

namespace NamespaceTests {
    namespace IndexDetailsTests {
        class Base {
        public:
            Base() {
                dblock lk;
                setClient( ns() );
            }
            ~Base() {
                if ( id_.info.isNull() )
                    return;
                theDataFileMgr.deleteRecord( ns(), id_.info.rec(), id_.info );
                ASSERT( theDataFileMgr.findAll( ns() )->eof() );
            }
        protected:
            void create() {
                BSONObjBuilder builder;
                builder.append( "ns", ns() );
                builder.append( "name", "testIndex" );
                builder.append( "key", key() );
                BSONObj bobj = builder.done();
                id_.info = theDataFileMgr.insert( ns(), bobj.objdata(), bobj.objsize() );
                // head not needed for current tests
                // idx_.head = BtreeBucket::addHead( id_ );
            }
            static const char* ns() {
                return "sys.unittest.indexdetailstests";
            }
            const IndexDetails& id() {
                return id_;
            }
            virtual BSONObj key() const {
                BSONObjBuilder k;
                k.append( "a", 1 );
                return k.doneAndDecouple();
            }
            BSONObj aDotB() const {
                BSONObjBuilder k;
                k.append( "a.b", 1 );
                return k.doneAndDecouple();
            }
            BSONObj aAndB() const {
                BSONObjBuilder k;
                k.append( "a", 1 );
                k.append( "b", 1 );
                return k.doneAndDecouple();
            }
            static vector< int > shortArray() {
                vector< int > a;
                a.push_back( 1 );
                a.push_back( 2 );
                a.push_back( 3 );
                return a;
            }
            static BSONObj simpleBC( int i ) {
                BSONObjBuilder b;
                b.append( "b", i );
                b.append( "c", 4 );
                return b.doneAndDecouple();
            }
            static void checkSize( int expected, const BSONObjSetDefaultOrder  &objs ) {
                ASSERT_EQUALS( BSONObjSetDefaultOrder::size_type( expected ), objs.size() );
            }
            static void assertEquals( const BSONObj &a, const BSONObj &b ) {
                if ( a.woCompare( b ) != 0 ) {
                    out() << "expected: " << a.toString()
                         << ", got: " << b.toString() << endl;
                }
                ASSERT( a.woCompare( b ) == 0 );
            }
        private:
            IndexDetails id_;
        };

        class Create : public Base {
        public:
            void run() {
                create();
                ASSERT_EQUALS( "testIndex", id().indexName() );
                ASSERT_EQUALS( ns(), id().parentNS() );
                assertEquals( key(), id().keyPattern() );
            }
        };

        class GetKeysFromObjectSimple : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b, e;
                b.append( "b", 4 );
                b.append( "a", 5 );
                e.append( "", 5 );
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 1, keys );
                assertEquals( e.doneAndDecouple(), *keys.begin() );
            }
        };

        class GetKeysFromObjectDotted : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder a, e, b;
                b.append( "b", 4 );
                a.append( "a", b.done() );
                a.append( "c", "foo" );
                e.append( "", 4 );
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( a.done(), keys );
                checkSize( 1, keys );
                assertEquals( e.doneAndDecouple(), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class GetKeysFromArraySimple : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "a", shortArray()) ;

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.doneAndDecouple(), *i );
                }
            }
        };

        class GetKeysFromArrayFirstElement : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "a", shortArray() );
                b.append( "b", 2 );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    b.append( "", 2 );
                    assertEquals( b.doneAndDecouple(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                return aAndB();
            }
        };

        class GetKeysFromArraySecondElement : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "first", 5 );
                b.append( "a", shortArray()) ;

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", 5 );
                    b.append( "", j );
                    assertEquals( b.doneAndDecouple(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                BSONObjBuilder k;
                k.append( "first", 1 );
                k.append( "a", 1 );
                return k.doneAndDecouple();
            }
        };

        class GetKeysFromSecondLevelArray : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "b", shortArray() );
                BSONObjBuilder a;
                a.append( "a", b.done() );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( a.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.doneAndDecouple(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class ParallelArraysBasic : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "a", shortArray() );
                b.append( "b", shortArray() );

                BSONObjSetDefaultOrder keys;
                ASSERT_EXCEPTION( id().getKeysFromObject( b.done(), keys ),
                                  UserAssertionException );
            }
        private:
            virtual BSONObj key() const {
                return aAndB();
            }
        };

        class ArraySubobjectBasic : public Base {
        public:
            void run() {
                create();
                vector< BSONObj > elts;
                for ( int i = 1; i < 4; ++i )
                    elts.push_back( simpleBC( i ) );
                BSONObjBuilder b;
                b.append( "a", elts );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.doneAndDecouple(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class ArraySubobjectMultiFieldIndex : public Base {
        public:
            void run() {
                create();
                vector< BSONObj > elts;
                for ( int i = 1; i < 4; ++i )
                    elts.push_back( simpleBC( i ) );
                BSONObjBuilder b;
                b.append( "a", elts );
                b.append( "d", 99 );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder c;
                    c.append( "", j );
                    c.append( "", 99 );
                    assertEquals( c.doneAndDecouple(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                BSONObjBuilder k;
                k.append( "a.b", 1 );
                k.append( "d", 1 );
                return k.doneAndDecouple();
            }
        };

        class ArraySubobjectSingleMissing : public Base {
        public:
            void run() {
                create();
                vector< BSONObj > elts;
                BSONObjBuilder s;
                s.append( "foo", 41 );
                elts.push_back( s.doneAndDecouple() );
                for ( int i = 1; i < 4; ++i )
                    elts.push_back( simpleBC( i ) );
                BSONObjBuilder b;
                b.append( "a", elts );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.doneAndDecouple(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class ArraySubobjectMissing : public Base {
        public:
            void run() {
                create();
                vector< BSONObj > elts;
                BSONObjBuilder s;
                s.append( "foo", 41 );
                for ( int i = 1; i < 4; ++i )
                    elts.push_back( s.done() );
                BSONObjBuilder b;
                b.append( "a", elts );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 0, keys );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

// TODO
// array subelement complex
// parallel arrays complex
// allowed multi array indexes

    } // namespace IndexDetailsTests

    namespace NamespaceDetailsTests {

        class Base {
        public:
            Base( const char *ns = "foo" ) : ns_( ns ) {}
            ~Base() {
                if ( !nsd() )
                    return;
                string s( ns() );
                dropNS( s );
            }
        protected:
            void create() {
                dblock lk;
                setClient( ns() );
                string err;
                ASSERT( userCreateNS( ns(), fromjson( spec() ), err, false ) );
            }
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512}";
            }
            int nRecords() const {
                int count = 0;
                for ( DiskLoc i = nsd()->firstExtent; !i.isNull(); i = i.ext()->xnext )
                    for ( DiskLoc j = i.ext()->firstRecord; !j.isNull();
                            j.setOfs( j.a(), j.rec()->nextOfs ) ) {
                        ++count;
                    }
                ASSERT_EQUALS( count, nsd()->nrecords );
                return count;
            }
            int nExtents() const {
                int count = 0;
                for ( DiskLoc i = nsd()->firstExtent; !i.isNull(); i = i.ext()->xnext )
                    ++count;
                return count;
            }
            static int min( int a, int b ) {
                return a < b ? a : b;
            }
            const char *ns() const {
                return ns_;
            }
            NamespaceDetails *nsd() const {
                return nsdetails( ns() );
            }
        private:
            const char *ns_;
        };

        class Create : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd() );
                ASSERT_EQUALS( 0, nRecords() );
                ASSERT( nsd()->firstExtent == nsd()->capExtent );
                DiskLoc initial = DiskLoc();
                initial.setInvalid();
                ASSERT( initial == nsd()->capFirstNewRecord );
            }
        };

        class SingleAlloc : public Base {
        public:
            void run() {
                create();
                char ch[ 200 ];
                memset( ch, 0, 200 );
                ASSERT( !theDataFileMgr.insert( ns(), ch, 200 ).isNull() );
                ASSERT_EQUALS( 1, nRecords() );
            }
        };

        class Realloc : public Base {
        public:
            void run() {
                create();
                char ch[ 200 ];

                DiskLoc l[ 6 ];
                for ( int i = 0; i < 6; ++i ) {
                    l[ i ] = theDataFileMgr.insert( ns(), ch, 200 );
                    ASSERT( !l[ i ].isNull() );
                    ASSERT_EQUALS( 1 + i % 2, nRecords() );
                    if ( i > 1 )
                        ASSERT( l[ i ] == l[ i - 2 ] );
                }
            }
        };

        class TwoExtent : public Base {
        public:
            void run() {
                create();
                ASSERT_EQUALS( 2, nExtents() );
                char ch[ 200 ];

                DiskLoc l[ 8 ];
                for ( int i = 0; i < 8; ++i ) {
                    l[ i ] = theDataFileMgr.insert( ns(), ch, 200 );
                    ASSERT( !l[ i ].isNull() );
                    ASSERT_EQUALS( i < 2 ? i + 1 : 3 + i % 2, nRecords() );
                    if ( i > 3 )
                        ASSERT( l[ i ] == l[ i - 4 ] );
                }

                // Too big
                char ch2[ 800 ];
                ASSERT( theDataFileMgr.insert( ns(), ch2, 800 ).isNull() );
                ASSERT_EQUALS( 0, nRecords() );
            }
        private:
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":2}";
            }
        };

        class Migrate : public Base {
        public:
            void run() {
                create();
                nsd()->deletedList[ 2 ] = nsd()->deletedList[ 0 ].drec()->nextDeleted.drec()->nextDeleted;
                nsd()->deletedList[ 0 ].drec()->nextDeleted.drec()->nextDeleted = DiskLoc();
                NamespaceDetails *d = nsd();
                zero( &d->capExtent );
                zero( &d->capFirstNewRecord );

                nsd();

                ASSERT( nsd()->firstExtent == nsd()->capExtent );
                ASSERT( nsd()->capExtent.getOfs() != 0 );
                ASSERT( !nsd()->capFirstNewRecord.isValid() );
                int nDeleted = 0;
                for ( DiskLoc i = nsd()->deletedList[ 0 ]; !i.isNull(); i = i.drec()->nextDeleted, ++nDeleted );
                ASSERT_EQUALS( 10, nDeleted );
                ASSERT( nsd()->deletedList[ 1 ].isNull() );
            }
        private:
            static void zero( DiskLoc *d ) {
                memset( d, 0, sizeof( DiskLoc ) );
            }
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":10}";
            }
        };

        // This isn't a particularly useful test, and because it doesn't clean up
        // after itself, /tmp/unittest needs to be cleared after running.
//        class BigCollection : public Base {
//        public:
//            BigCollection() : Base( "NamespaceDetailsTests_BigCollection" ) {}
//            void run() {
//                create();
//                ASSERT_EQUALS( 2, nExtents() );
//            }
//        private:
//            virtual string spec() const {
//                // NOTE 256 added to size in _userCreateNS()
//                long long big = PhysicalDataFile::maxSize() - PDFHeader::headerSize();
//                stringstream ss;
//                ss << "{\"capped\":true,\"size\":" << big << "}";
//                return ss.str();
//            }
//        };

    } // namespace NamespaceDetailsTests

    class All : public UnitTest::Suite {
    public:
        All() {
            add< IndexDetailsTests::Create >();
            add< IndexDetailsTests::GetKeysFromObjectSimple >();
            add< IndexDetailsTests::GetKeysFromObjectDotted >();
            add< IndexDetailsTests::GetKeysFromArraySimple >();
            add< IndexDetailsTests::GetKeysFromArrayFirstElement >();
            add< IndexDetailsTests::GetKeysFromArraySecondElement >();
            add< IndexDetailsTests::GetKeysFromSecondLevelArray >();
            add< IndexDetailsTests::ParallelArraysBasic >();
            add< IndexDetailsTests::ArraySubobjectBasic >();
            add< IndexDetailsTests::ArraySubobjectMultiFieldIndex >();
            add< IndexDetailsTests::ArraySubobjectSingleMissing >();
            add< IndexDetailsTests::ArraySubobjectMissing >();
            add< NamespaceDetailsTests::Create >();
            add< NamespaceDetailsTests::SingleAlloc >();
            add< NamespaceDetailsTests::Realloc >();
            add< NamespaceDetailsTests::TwoExtent >();
            add< NamespaceDetailsTests::Migrate >();
//            add< NamespaceDetailsTests::BigCollection >();
        }
    };
} // namespace NamespaceTests

UnitTest::TestPtr namespaceTests() {
    return UnitTest::createSuite< NamespaceTests::All >();
}
