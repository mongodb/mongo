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
#include "pch.h"
#include "../db/namespace.h"

#include "../db/db.h"
#include "../db/json.h"
#include "mongo/db/queryutil.h"

#include "dbtests.h"

namespace NamespaceTests {

    const int MinExtentSize = 4096;

    namespace IndexDetailsTests {
        class Base {
            Lock::GlobalWrite lk;
            Client::Context _context;
        public:
            Base() : _context(ns()) {
            }
            virtual ~Base() {
                if ( id_.info.isNull() )
                    return;
                theDataFileMgr.deleteRecord( ns(), id_.info.rec(), id_.info );
                ASSERT( theDataFileMgr.findAll( ns() )->eof() );
            }
        protected:
            void create( bool sparse = false ) {
                NamespaceDetailsTransient::get( ns() ).deletedIndex();
                BSONObjBuilder builder;
                builder.append( "ns", ns() );
                builder.append( "name", "testIndex" );
                builder.append( "key", key() );
                builder.append( "sparse", sparse );
                BSONObj bobj = builder.done();
                id_.info = theDataFileMgr.insert( ns(), bobj.objdata(), bobj.objsize() );
                // head not needed for current tests
                // idx_.head = BtreeBucket::addHead( id_ );
            }
            static const char* ns() {
                return "unittests.indexdetailstests";
            }
            IndexDetails& id() {
                return id_;
            }
            virtual BSONObj key() const {
                BSONObjBuilder k;
                k.append( "a", 1 );
                return k.obj();
            }
            BSONObj aDotB() const {
                BSONObjBuilder k;
                k.append( "a.b", 1 );
                return k.obj();
            }
            BSONObj aAndB() const {
                BSONObjBuilder k;
                k.append( "a", 1 );
                k.append( "b", 1 );
                return k.obj();
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
                return b.obj();
            }
            static void checkSize( int expected, const BSONObjSet  &objs ) {
                ASSERT_EQUALS( BSONObjSet::size_type( expected ), objs.size() );
            }
            static void assertEquals( const BSONObj &a, const BSONObj &b ) {
                if ( a.woCompare( b ) != 0 ) {
                    out() << "expected: " << a.toString()
                          << ", got: " << b.toString() << endl;
                }
                ASSERT( a.woCompare( b ) == 0 );
            }
            BSONObj nullObj() const {
                BSONObjBuilder b;
                b.appendNull( "" );
                return b.obj();
            }
        private:
            Lock::GlobalWrite lk_;
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
                BSONObjSet keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 1, keys );
                assertEquals( e.obj(), *keys.begin() );
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
                BSONObjSet keys;
                id().getKeysFromObject( a.done(), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( e.obj(), *keys.begin() );
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

                BSONObjSet keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSet::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
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

                BSONObjSet keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSet::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    b.append( "", 2 );
                    assertEquals( b.obj(), *i );
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

                BSONObjSet keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSet::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", 5 );
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                BSONObjBuilder k;
                k.append( "first", 1 );
                k.append( "a", 1 );
                return k.obj();
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

                BSONObjSet keys;
                id().getKeysFromObject( a.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSet::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
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

                BSONObjSet keys;
                ASSERT_THROWS( id().getKeysFromObject( b.done(), keys ),
                                  UserException );
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

                BSONObjSet keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSet::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
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

                BSONObjSet keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSet::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder c;
                    c.append( "", j );
                    c.append( "", 99 );
                    assertEquals( c.obj(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                BSONObjBuilder k;
                k.append( "a.b", 1 );
                k.append( "d", 1 );
                return k.obj();
            }
        };

        class ArraySubobjectSingleMissing : public Base {
        public:
            void run() {
                create();
                vector< BSONObj > elts;
                BSONObjBuilder s;
                s.append( "foo", 41 );
                elts.push_back( s.obj() );
                for ( int i = 1; i < 4; ++i )
                    elts.push_back( simpleBC( i ) );
                BSONObjBuilder b;
                b.append( "a", elts );
                BSONObj obj = b.obj();
                
                BSONObjSet keys;
                id().getKeysFromObject( obj, keys );
                checkSize( 4, keys );
                BSONObjSet::iterator i = keys.begin();
                assertEquals( nullObj(), *i++ ); // see SERVER-3377
                for ( int j = 1; j < 4; ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
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

                BSONObjSet keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 1, keys );
                assertEquals( nullObj(), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class MissingField : public Base {
        public:
            void run() {
                create();
                BSONObjSet keys;
                id().getKeysFromObject( BSON( "b" << 1 ), keys );
                checkSize( 1, keys );
                assertEquals( nullObj(), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return BSON( "a" << 1 );
            }
        };

        class SubobjectMissing : public Base {
        public:
            void run() {
                create();
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[1,2]}" ), keys );
                checkSize( 1, keys );
                assertEquals( nullObj(), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class CompoundMissing : public Base {
        public:
            void run() {
                create();

                {
                    BSONObjSet keys;
                    id().getKeysFromObject( fromjson( "{x:'a',y:'b'}" ) , keys );
                    checkSize( 1 , keys );
                    assertEquals( BSON( "" << "a" << "" << "b" ) , *keys.begin() );
                }

                {
                    BSONObjSet keys;
                    id().getKeysFromObject( fromjson( "{x:'a'}" ) , keys );
                    checkSize( 1 , keys );
                    BSONObjBuilder b;
                    b.append( "" , "a" );
                    b.appendNull( "" );
                    assertEquals( b.obj() , *keys.begin() );
                }

            }

        private:
            virtual BSONObj key() const {
                return BSON( "x" << 1 << "y" << 1 );
            }

        };

        class ArraySubelementComplex : public Base {
        public:
            void run() {
                create();
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[{b:[2]}]}" ), keys );
                checkSize( 1, keys );
                assertEquals( BSON( "" << 2 ), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class ParallelArraysComplex : public Base {
        public:
            void run() {
                create();
                BSONObjSet keys;
                ASSERT_THROWS( id().getKeysFromObject( fromjson( "{a:[{b:[1],c:[2]}]}" ), keys ),
                                  UserException );
            }
        private:
            virtual BSONObj key() const {
                return fromjson( "{'a.b':1,'a.c':1}" );
            }
        };

        class AlternateMissing : public Base {
        public:
            void run() {
                create();
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[{b:1},{c:2}]}" ), keys );
                checkSize( 2, keys );
                BSONObjSet::iterator i = keys.begin();
                {
                    BSONObjBuilder e;
                    e.appendNull( "" );
                    e.append( "", 2 );
                    assertEquals( e.obj(), *i++ );
                }

                {
                    BSONObjBuilder e;
                    e.append( "", 1 );
                    e.appendNull( "" );
                    assertEquals( e.obj(), *i++ );
                }
            }
        private:
            virtual BSONObj key() const {
                return fromjson( "{'a.b':1,'a.c':1}" );
            }
        };

        class MultiComplex : public Base {
        public:
            void run() {
                create();
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[{b:1},{b:[1,2,3]}]}" ), keys );
                checkSize( 3, keys );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class EmptyArray : Base {
        public:
            void run() {
                create();

                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[1,2]}" ), keys );
                checkSize(2, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[1]}" ), keys );
                checkSize(1, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:null}" ), keys );
                checkSize(1, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize(1, keys );
                ASSERT_EQUALS( Undefined, keys.begin()->firstElement().type() );
                keys.clear();
            }
        };
 
        class DoubleArray : Base {
        public:
            void run() {
             	create();   
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[1,2]}" ), keys );
                checkSize(2, keys );
                BSONObjSet::const_iterator i = keys.begin();
                ASSERT_EQUALS( BSON( "" << 1 << "" << 1 ), *i );
                ++i;
                ASSERT_EQUALS( BSON( "" << 2 << "" << 2 ), *i );
                keys.clear();
            }
            
        protected:
            BSONObj key() const {
                return BSON( "a" << 1 << "a" << 1 );
            }
        };
        
        class DoubleEmptyArray : Base {
        public:
            void run() {
             	create();   

                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize(1, keys );
                ASSERT_EQUALS( fromjson( "{'':undefined,'':undefined}" ), *keys.begin() );
                keys.clear();
            }
            
        protected:
            BSONObj key() const {
                return BSON( "a" << 1 << "a" << 1 );
            }
        };

        class MultiEmptyArray : Base {
        public:
            void run() {
                create();

                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:1,b:[1,2]}" ), keys );
                checkSize(2, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:1,b:[1]}" ), keys );
                checkSize(1, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:1,b:null}" ), keys );
                //cout << "YO : " << *(keys.begin()) << endl;
                checkSize(1, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:1,b:[]}" ), keys );
                checkSize(1, keys );
                //cout << "YO : " << *(keys.begin()) << endl;
                BSONObjIterator i( *keys.begin() );
                ASSERT_EQUALS( NumberInt , i.next().type() );
                ASSERT_EQUALS( Undefined , i.next().type() );
                keys.clear();
            }

        protected:
            BSONObj key() const {
                return aAndB();
            }
        };
        
        class NestedEmptyArray : Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.b" << 1 ); }
        };
        
		class MultiNestedEmptyArray : Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null,'':null}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.b" << 1 << "a.c" << 1 ); }
        };
        
        class UnevenNestedEmptyArray : public Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':undefined,'':null}" ), *keys.begin() );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:[{b:1}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':{b:1},'':1}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[{b:[]}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':{b:[]},'':undefined}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a" << 1 << "a.b" << 1 ); }            
        };

        class ReverseUnevenNestedEmptyArray : public Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null,'':undefined}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.b" << 1 << "a" << 1 ); }            
        };
        
        class SparseReverseUnevenNestedEmptyArray : public Base {
        public:
            void run() {
             	create( true );
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null,'':undefined}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.b" << 1 << "a" << 1 ); }            
        };
        
        class SparseEmptyArray : public Base {
        public:
            void run() {
             	create( true );
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:1}" ), keys );
                checkSize( 0, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 0, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[{c:1}]}" ), keys );
                checkSize( 0, keys );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.b" << 1 ); }            
        };

        class SparseEmptyArraySecond : public Base {
        public:
            void run() {
             	create( true );
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:1}" ), keys );
                checkSize( 0, keys );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 0, keys );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:[{c:1}]}" ), keys );
                checkSize( 0, keys );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "z" << 1 << "a.b" << 1 ); }
        };
        
        class NonObjectMissingNestedField : public Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null}" ), *keys.begin() );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:[1]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[1,{b:1}]}" ), keys );
                checkSize( 2, keys );
                BSONObjSet::const_iterator c = keys.begin();
                ASSERT_EQUALS( fromjson( "{'':null}" ), *c );
                ++c;
                ASSERT_EQUALS( fromjson( "{'':1}" ), *c );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.b" << 1 ); }
        };

        class SparseNonObjectMissingNestedField : public Base {
        public:
            void run() {
             	create( true );
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 0, keys );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:[1]}" ), keys );
                checkSize( 0, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[1,{b:1}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.b" << 1 ); }
        };
        
        class IndexedArrayIndex : public Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[1]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( BSON( "" << 1 ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[[1]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':[1]}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[[]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':undefined}" ), *keys.begin() );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:{'0':1}}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( BSON( "" << 1 ), *keys.begin() );
                keys.clear();

                ASSERT_THROWS( id().getKeysFromObject( fromjson( "{a:[{'0':1}]}" ), keys ), UserException );

                ASSERT_THROWS( id().getKeysFromObject( fromjson( "{a:[1,{'0':2}]}" ), keys ), UserException );
            }
        protected:
            BSONObj key() const { return BSON( "a.0" << 1 ); }
        };

        class DoubleIndexedArrayIndex : public Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[[1]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[[]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[[[]]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':undefined}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.0.0" << 1 ); }
        };
        
        class ObjectWithinArray : public Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[{b:1}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[{b:[1]}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[{b:[[1]]}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':[1]}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[[{b:1}]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[[{b:[1]}]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[[{b:[[1]]}]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':[1]}" ), *keys.begin() );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:[[{b:[]}]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':undefined}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.0.b" << 1 ); }
        };

        class ArrayWithinObjectWithinArray : public Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                id().getKeysFromObject( fromjson( "{a:[{b:[1]}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.0.b.0" << 1 ); }
        };
        
        // also test numeric string field names
        
    } // namespace IndexDetailsTests

    namespace IndexSpecTests {
        
        class Suitability {
        public:
            void run() {
                IndexSpec spec( BSON( "a" << 1 ), BSONObj() );
                ASSERT_EQUALS( HELPFUL,
                              spec.suitability( BSON( "a" << 2 << "b" << 3 ), BSONObj() ) );
                ASSERT_EQUALS( USELESS,
                              spec.suitability( BSON( "b" << 3 ), BSONObj() ) );
                ASSERT_EQUALS( HELPFUL,
                              spec.suitability( BSON( "b" << 3 ), BSON( "a" << 1 ) ) );
            }
        };
        
        /** Lexical rather than numeric comparison should be used to determine index suitability. */
        class NumericFieldSuitability {
        public:
            void run() {
                IndexSpec spec( BSON( "1" << 1 ), BSONObj() );
                ASSERT_EQUALS( HELPFUL,
                              spec.suitability( BSON( "1" << 2 ), BSONObj() ) );
                ASSERT_EQUALS( USELESS,
                              spec.suitability( BSON( "01" << 3 ), BSON( "01" << 1 ) ) );
                ASSERT_EQUALS( HELPFUL,
                              spec.suitability( BSONObj(), BSON( "1" << 1 ) ) );                
            }
        };
        
    } // namespace IndexSpecTests
    
    namespace NamespaceDetailsTests {

        class Base {
            const char *ns_;
            Lock::GlobalWrite lk;
            Client::Context _context;
        public:
            Base( const char *ns = "unittests.NamespaceDetailsTests" ) : ns_( ns ) , _context( ns ) {}
            virtual ~Base() {
                if ( !nsd() )
                    return;
                string s( ns() );
                string errmsg;
                BSONObjBuilder result;
                dropCollection( s, errmsg, result );
            }
        protected:
            void create() {
                Lock::GlobalWrite lk;
                string err;
                ASSERT( userCreateNS( ns(), fromjson( spec() ), err, false ) );
            }
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":1}";
            }
            int nRecords() const {
                int count = 0;
                for ( DiskLoc i = nsd()->firstExtent; !i.isNull(); i = i.ext()->xnext ) {
                    int fileNo = i.ext()->firstRecord.a();
                    if ( fileNo == -1 )
                        continue;
                    for ( int j = i.ext()->firstRecord.getOfs(); j != DiskLoc::NullOfs;
                          j = DiskLoc( fileNo, j ).rec()->nextOfs() ) {
                        ++count;
                    }
                }
                ASSERT_EQUALS( count, nsd()->stats.nrecords );
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
                return nsdetails( ns() )->writingWithExtra();
            }
            NamespaceDetailsTransient &nsdt() const {
                return NamespaceDetailsTransient::get( ns() );
            }
            static BSONObj bigObj(bool bGenID=false) {
                BSONObjBuilder b;
				if (bGenID)
					b.appendOID("_id", 0, true);
                string as( 187, 'a' );
                b.append( "a", as );
                return b.obj();
            }
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
                BSONObj b = bigObj();
                ASSERT( !theDataFileMgr.insert( ns(), b.objdata(), b.objsize() ).isNull() );
                ASSERT_EQUALS( 1, nRecords() );
            }
        };

        class Realloc : public Base {
        public:
            void run() {
                create();

                const int N = 20;
                const int Q = 16; // these constants depend on the size of the bson object, the extent size allocated by the system too
                DiskLoc l[ N ];
                for ( int i = 0; i < N; ++i ) {
					BSONObj b = bigObj(true);
                    l[ i ] = theDataFileMgr.insert( ns(), b.objdata(), b.objsize() );
                    ASSERT( !l[ i ].isNull() );
                    ASSERT( nRecords() <= Q );
                    //ASSERT_EQUALS( 1 + i % 2, nRecords() );
                    if ( i >= 16 )
                        ASSERT( l[ i ] == l[ i - Q] );
                }
            }
        };

        class TwoExtent : public Base {
        public:
            void run() {
                create();
                ASSERT_EQUALS( 2, nExtents() );

                BSONObj b = bigObj();

                DiskLoc l[ 8 ];
                for ( int i = 0; i < 8; ++i ) {
                    l[ i ] = theDataFileMgr.insert( ns(), b.objdata(), b.objsize() );
                    ASSERT( !l[ i ].isNull() );
                    //ASSERT_EQUALS( i < 2 ? i + 1 : 3 + i % 2, nRecords() );
                    //if ( i > 3 )
                    //    ASSERT( l[ i ] == l[ i - 4 ] );
                }
                ASSERT( nRecords() == 8 );

                // Too big
                BSONObjBuilder bob;
                bob.append( "a", string( MinExtentSize + 500, 'a' ) ); // min extent size is now 4096
                BSONObj bigger = bob.done();
                ASSERT( theDataFileMgr.insert( ns(), bigger.objdata(), bigger.objsize() ).isNull() );
                ASSERT_EQUALS( 0, nRecords() );
            }
        private:
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":2}";
            }
        };

        /* test  NamespaceDetails::cappedTruncateAfter(const char *ns, DiskLoc loc)
        */
        class TruncateCapped : public Base {
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":2}";
            }
            void pass(int p) {
                create();
                ASSERT_EQUALS( 2, nExtents() );

                BSONObj b = bigObj(true);

                int N = MinExtentSize / b.objsize() * nExtents() + 5;
                int T = N - 4;

                DiskLoc truncAt;
                //DiskLoc l[ 8 ];
                for ( int i = 0; i < N; ++i ) {
					BSONObj bb = bigObj(true);
                    DiskLoc a = theDataFileMgr.insert( ns(), bb.objdata(), bb.objsize() );
                    if( T == i )
                        truncAt = a;
                    ASSERT( !a.isNull() );
                    /*ASSERT_EQUALS( i < 2 ? i + 1 : 3 + i % 2, nRecords() );
                    if ( i > 3 )
                        ASSERT( l[ i ] == l[ i - 4 ] );*/
                }
                ASSERT( nRecords() < N );

                NamespaceDetails *nsd = nsdetails(ns());

                DiskLoc last, first;
                {
                    ReverseCappedCursor c(nsd);
                    last = c.currLoc();
                    ASSERT( !last.isNull() );
                }
                {
                    ForwardCappedCursor c(nsd);
                    first = c.currLoc();
                    ASSERT( !first.isNull() );
                    ASSERT( first != last ) ;
                }

                nsd->cappedTruncateAfter(ns(), truncAt, false);
                ASSERT_EQUALS( nsd->stats.nrecords , 28 );

                {
                    ForwardCappedCursor c(nsd);
                    ASSERT( first == c.currLoc() );
                }
                {
                    ReverseCappedCursor c(nsd);
                    ASSERT( last != c.currLoc() ); // old last should be deleted
                    ASSERT( !last.isNull() );
                }

                // Too big
                BSONObjBuilder bob;
				bob.appendOID("_id", 0, true);
                bob.append( "a", string( MinExtentSize + 300, 'a' ) );
                BSONObj bigger = bob.done();
                ASSERT( theDataFileMgr.insert( ns(), bigger.objdata(), bigger.objsize() ).isNull() );
                ASSERT_EQUALS( 0, nRecords() );
            }
        public:
            void run() {
//                log() << "******** NOT RUNNING TruncateCapped test yet ************" << endl;
                pass(0);
            }
        };

        class Migrate : public Base {
        public:
            void run() {
                create();
                nsd()->deletedList[ 2 ] = nsd()->cappedListOfAllDeletedRecords().drec()->nextDeleted().drec()->nextDeleted();
                nsd()->cappedListOfAllDeletedRecords().drec()->nextDeleted().drec()->nextDeleted().writing() = DiskLoc();
                nsd()->cappedLastDelRecLastExtent().Null();
                NamespaceDetails *d = nsd();
                zero( &d->capExtent );
                zero( &d->capFirstNewRecord );

                nsd();

                ASSERT( nsd()->firstExtent == nsd()->capExtent );
                ASSERT( nsd()->capExtent.getOfs() != 0 );
                ASSERT( !nsd()->capFirstNewRecord.isValid() );
                int nDeleted = 0;
                for ( DiskLoc i = nsd()->cappedListOfAllDeletedRecords(); !i.isNull(); i = i.drec()->nextDeleted(), ++nDeleted );
                ASSERT_EQUALS( 10, nDeleted );
                ASSERT( nsd()->cappedLastDelRecLastExtent().isNull() );
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
        //                long long big = MongoDataFile::maxSize() - DataFileHeader::HeaderSize;
        //                stringstream ss;
        //                ss << "{\"capped\":true,\"size\":" << big << "}";
        //                return ss.str();
        //            }
        //        };

        class Size {
        public:
            void run() {
                ASSERT_EQUALS( 496U, sizeof( NamespaceDetails ) );
            }
        };
        
        class CachedPlanBase : public Base {
        public:
            CachedPlanBase() :
                _fieldRangeSet( ns(), BSON( "a" << 1 ), true ),
                _pattern( _fieldRangeSet, BSONObj() ) {
                create();
            }
        protected:
            void assertCachedIndexKey( const BSONObj &indexKey ) const {
                ASSERT_EQUALS( indexKey,
                              nsdt().cachedQueryPlanForPattern( _pattern ).indexKey() );
            }
            void registerIndexKey( const BSONObj &indexKey ) {
                nsdt().registerCachedQueryPlanForPattern
                        ( _pattern,
                         CachedQueryPlan( indexKey, 1, CandidatePlanCharacter( true, false ) ) );                
            }
            FieldRangeSet _fieldRangeSet;
            QueryPattern _pattern;
        };
        
        /**
         * setIndexIsMultikey() sets the multikey flag for an index and clears the query plan
         * cache.
         */
        class SetIndexIsMultikey : public CachedPlanBase {
        public:
            void run() {
                DBDirectClient client;
                client.ensureIndex( ns(), BSON( "a" << 1 ) );
                registerIndexKey( BSON( "a" << 1 ) );
                
                ASSERT( !nsd()->isMultikey( 1 ) );
                
                nsd()->setIndexIsMultikey( ns(), 1 );
                ASSERT( nsd()->isMultikey( 1 ) );
                assertCachedIndexKey( BSONObj() );
                
                registerIndexKey( BSON( "a" << 1 ) );
                nsd()->setIndexIsMultikey( ns(), 1 );
                assertCachedIndexKey( BSON( "a" << 1 ) );
            }
        };
        
    } // namespace NamespaceDetailsTests

    namespace NamespaceDetailsTransientTests {
        
        /** clearQueryCache() clears the query plan cache. */
        class ClearQueryCache : public NamespaceDetailsTests::CachedPlanBase {
        public:
            void run() {
                // Register a query plan in the query plan cache.
                registerIndexKey( BSON( "a" << 1 ) );
                assertCachedIndexKey( BSON( "a" << 1 ) );
                
                // The query plan is cleared.
                nsdt().clearQueryCache();
                assertCachedIndexKey( BSONObj() );
            }
        };                                                                                         
        
    } // namespace NamespaceDetailsTransientTests
                                                                                 
    class All : public Suite {
    public:
        All() : Suite( "namespace" ) {
        }

        void setupTests() {
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
            add< IndexDetailsTests::ArraySubelementComplex >();
            add< IndexDetailsTests::ParallelArraysComplex >();
            add< IndexDetailsTests::AlternateMissing >();
            add< IndexDetailsTests::MultiComplex >();
            add< IndexDetailsTests::EmptyArray >();
            add< IndexDetailsTests::DoubleArray >();
            add< IndexDetailsTests::DoubleEmptyArray >();
            add< IndexDetailsTests::MultiEmptyArray >();
            add< IndexDetailsTests::NestedEmptyArray >();
            add< IndexDetailsTests::MultiNestedEmptyArray >();
            add< IndexDetailsTests::UnevenNestedEmptyArray >();
            add< IndexDetailsTests::ReverseUnevenNestedEmptyArray >();
            add< IndexDetailsTests::SparseReverseUnevenNestedEmptyArray >();
            add< IndexDetailsTests::SparseEmptyArray >();
            add< IndexDetailsTests::SparseEmptyArraySecond >();
            add< IndexDetailsTests::NonObjectMissingNestedField >();
            add< IndexDetailsTests::SparseNonObjectMissingNestedField >();
            add< IndexDetailsTests::IndexedArrayIndex >();
            add< IndexDetailsTests::DoubleIndexedArrayIndex >();
            add< IndexDetailsTests::ObjectWithinArray >();
            add< IndexDetailsTests::ArrayWithinObjectWithinArray >();
            add< IndexDetailsTests::MissingField >();
            add< IndexDetailsTests::SubobjectMissing >();
            add< IndexDetailsTests::CompoundMissing >();
            add< IndexSpecTests::Suitability >();
            add< IndexSpecTests::NumericFieldSuitability >();
            add< NamespaceDetailsTests::Create >();
            add< NamespaceDetailsTests::SingleAlloc >();
            add< NamespaceDetailsTests::Realloc >();
            add< NamespaceDetailsTests::TwoExtent >();
            add< NamespaceDetailsTests::TruncateCapped >();
            add< NamespaceDetailsTests::Migrate >();
            //            add< NamespaceDetailsTests::BigCollection >();
            add< NamespaceDetailsTests::Size >();
            add< NamespaceDetailsTests::SetIndexIsMultikey >();
            add< NamespaceDetailsTransientTests::ClearQueryCache >();
        }
    } myall;
} // namespace NamespaceTests

