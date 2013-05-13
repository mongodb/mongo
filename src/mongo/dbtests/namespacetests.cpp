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
#include "mongo/pch.h"

#include "mongo/db/db.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_selection.h"
#include "mongo/db/json.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/storage/namespace.h"
#include "mongo/dbtests/dbtests.h"


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

                _keyPattern = key().getOwned();
                // The key generation wants these values.
                vector<const char*> fieldNames;
                vector<BSONElement> fixed;

                BSONObjIterator it(_keyPattern);
                while (it.more()) {
                    BSONElement elt = it.next();
                    fieldNames.push_back(elt.fieldName());
                    fixed.push_back(BSONElement());
                }  

                _keyGen.reset(new BtreeKeyGeneratorV1(fieldNames, fixed, sparse));
            }

            scoped_ptr<BtreeKeyGenerator> _keyGen;
            BSONObj _keyPattern;

            static const char* ns() {
                return "unittests.indexdetailstests";
            }

            IndexDetails& id() {
                return id_;
            }

            // TODO: This is testing Btree key creation, not IndexDetails.
            void getKeysFromObject(const BSONObj& obj, BSONObjSet& out) {
                _keyGen->getKeys(obj, &out);
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
                getKeysFromObject( b.done(), keys );
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
                getKeysFromObject( a.done(), keys );
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
                getKeysFromObject( b.done(), keys );
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
                getKeysFromObject( b.done(), keys );
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
                getKeysFromObject( b.done(), keys );
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
                getKeysFromObject( a.done(), keys );
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
                ASSERT_THROWS( getKeysFromObject( b.done(), keys ),
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
                getKeysFromObject( b.done(), keys );
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
                getKeysFromObject( b.done(), keys );
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
                getKeysFromObject( obj, keys );
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
                getKeysFromObject( b.done(), keys );
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
                getKeysFromObject( BSON( "b" << 1 ), keys );
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
                getKeysFromObject( fromjson( "{a:[1,2]}" ), keys );
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
                    getKeysFromObject( fromjson( "{x:'a',y:'b'}" ) , keys );
                    checkSize( 1 , keys );
                    assertEquals( BSON( "" << "a" << "" << "b" ) , *keys.begin() );
                }

                {
                    BSONObjSet keys;
                    getKeysFromObject( fromjson( "{x:'a'}" ) , keys );
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
                getKeysFromObject( fromjson( "{a:[{b:[2]}]}" ), keys );
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
                ASSERT_THROWS( getKeysFromObject( fromjson( "{a:[{b:[1],c:[2]}]}" ), keys ),
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
                getKeysFromObject( fromjson( "{a:[{b:1},{c:2}]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[{b:1},{b:[1,2,3]}]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[1,2]}" ), keys );
                checkSize(2, keys );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[1]}" ), keys );
                checkSize(1, keys );
                keys.clear();

                getKeysFromObject( fromjson( "{a:null}" ), keys );
                checkSize(1, keys );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[1,2]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:1,b:[1,2]}" ), keys );
                checkSize(2, keys );
                keys.clear();

                getKeysFromObject( fromjson( "{a:1,b:[1]}" ), keys );
                checkSize(1, keys );
                keys.clear();

                getKeysFromObject( fromjson( "{a:1,b:null}" ), keys );
                //cout << "YO : " << *(keys.begin()) << endl;
                checkSize(1, keys );
                keys.clear();

                getKeysFromObject( fromjson( "{a:1,b:[]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':undefined,'':null}" ), *keys.begin() );
                keys.clear();
                
                getKeysFromObject( fromjson( "{a:[{b:1}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':{b:1},'':1}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[{b:[]}]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:1}" ), keys );
                checkSize( 0, keys );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 0, keys );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[{c:1}]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:1}" ), keys );
                checkSize( 0, keys );
                keys.clear();
                
                getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 0, keys );
                keys.clear();
                
                getKeysFromObject( fromjson( "{a:[{c:1}]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null}" ), *keys.begin() );
                keys.clear();
                
                getKeysFromObject( fromjson( "{a:[1]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[1,{b:1}]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 0, keys );
                keys.clear();
                
                getKeysFromObject( fromjson( "{a:[1]}" ), keys );
                checkSize( 0, keys );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[1,{b:1}]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[1]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( BSON( "" << 1 ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[[1]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':[1]}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[[]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':undefined}" ), *keys.begin() );
                keys.clear();
                
                getKeysFromObject( fromjson( "{a:{'0':1}}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( BSON( "" << 1 ), *keys.begin() );
                keys.clear();

                ASSERT_THROWS( getKeysFromObject( fromjson( "{a:[{'0':1}]}" ), keys ), UserException );

                ASSERT_THROWS( getKeysFromObject( fromjson( "{a:[1,{'0':2}]}" ), keys ), UserException );
            }
        protected:
            BSONObj key() const { return BSON( "a.0" << 1 ); }
        };

        class DoubleIndexedArrayIndex : public Base {
        public:
            void run() {
             	create();
                
                BSONObjSet keys;
                getKeysFromObject( fromjson( "{a:[[1]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[[]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':null}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[[[]]]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[{b:1}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[{b:[1]}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[{b:[[1]]}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':[1]}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[[{b:1}]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[[{b:[1]}]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();

                getKeysFromObject( fromjson( "{a:[[{b:[[1]]}]]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':[1]}" ), *keys.begin() );
                keys.clear();
                
                getKeysFromObject( fromjson( "{a:[[{b:[]}]]}" ), keys );
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
                getKeysFromObject( fromjson( "{a:[{b:[1]}]}" ), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( fromjson( "{'':1}" ), *keys.begin() );
                keys.clear();
            }
        protected:
            BSONObj key() const { return BSON( "a.0.b.0" << 1 ); }
        };
        
        // also test numeric string field names
        
    } // namespace IndexDetailsTests

    namespace IndexSpecSuitability {
        
        /** An index is helpful for a query on the indexed field. */
        class IndexedQueryField {
        public:
            void run() {
                BSONObj kp( BSON( "a" << 1 ) );
                FieldRangeSet frs( "n/a", BSON( "a" << 2 ), true , true );
                // Checking a return value of HELPFUL instead of OPTIMAL is descriptive rather than
                // normative.  See SERVER-4485.
                ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(kp, frs, BSONObj() ) );
            }
        };
        
        /** An index is useless for a query not on an indexed field. */
        class NoIndexedQueryField {
        public:
            void run() {
                BSONObj kp( BSON( "a" << 1 ) );
                FieldRangeSet frs( "n/a", BSON( "b" << 2 ), true , true );
                ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(kp, frs, BSONObj() ) );
            }
        };
        
        /** An index is useless for a query on the child of the indexed field. */
        class ChildOfIndexQueryField {
        public:
            void run() {
                BSONObj kp(BSON( "a" << 1 ));
                FieldRangeSet frs( "n/a", BSON( "a.b" << 2 ), true , true );
                ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(kp, frs, BSONObj() ) );
            }
        };
        
        /** An index is useless for a query on the parent of the indexed field. */
        class ParentOfIndexQueryField {
        public:
            void run() {
                BSONObj kp(BSON( "a.b" << 1 ));
                FieldRangeSet frs( "n/a", BSON( "a" << 2 ), true , true );
                ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(kp, frs, BSONObj() ) );
            }
        };
        
        /**
         * An index is useless for an equality query containing a field name completing the indexed
         * field path.
         */
        class ObjectMatchCompletingIndexField {
        public:
            void run() {
                BSONObj kp(BSON( "a.b" << 1 ));
                FieldRangeSet frs( "n/a", BSON( "a" << BSON( "b" << 2 ) ), true , true );
                ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(kp, frs, BSONObj() ) );
            }
        };
        
        /** An index is helpful for an ordering on the indexed field. */
        class IndexedOrderField {
        public:
            void run() {
                BSONObj kp(BSON( "a" << 1 ));
                FieldRangeSet frs( "n/a", BSONObj(), true , true );
                ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(kp, frs, BSON( "a" << 1 ) ) );
            }
        };
        
        /** An index is helpful for a reverse direction ordering on the indexed field. */
        class IndexedReverseOrderField {
        public:
            void run() {
                BSONObj kp(BSON( "a" << -1 ));
                FieldRangeSet frs( "n/a", BSONObj(), true , true );
                ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(kp, frs, BSON( "a" << 1 ) ) );
            }
        };
        
        /**
         * An index is helpful for an ordering containing the indexed field, even if the first
         * ordered field is not indexed.  This is a descriptive rather than normative test.
         */
        class NonPrefixIndexedOrderField {
        public:
            void run() {
                BSONObj kp( BSON( "a" << 1 ));
                FieldRangeSet frs( "n/a", BSONObj(), true , true );
                ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(kp, frs, BSON( "b" << 1 << "a" << 1 ) ) );
            }
        };

        /** An index is useless for an ordering on an unindexed field. */
        class NoIndexedOrderField {
        public:
            void run() {
                BSONObj kp(BSON( "a" << 1 ));
                FieldRangeSet frs( "n/a", BSONObj(), true , true );
                ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(kp, frs, BSON( "b" << 1 ) ) );
            }
        };
        
        /** An index is useless for an ordering on the child of an indexed field. */
        class ChildOfIndexOrderField {
        public:
            void run() {
                BSONObj kp( BSON( "a" << 1 ));
                FieldRangeSet frs( "n/a", BSONObj(), true , true );
                ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(kp, frs, BSON( "a.b" << 1 ) ) );
            }
        };
        
        /** An index is useless for an ordering on the parent of an indexed field. */
        class ParentOfIndexOrderField {
        public:
            void run() {
                BSONObj kp( BSON( "a.b" << 1 ) );
                FieldRangeSet frs( "n/a", BSONObj(), true , true );
                ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(kp, frs, BSON( "a" << 1 ) ) );
            }
        };
        
        /** Lexical rather than numeric comparison should be used to determine index suitability. */
        class NumericFieldSuitability {
        public:
            void run() {
                BSONObj kp( BSON( "1" << 1 ));
                FieldRangeSet frs1( "n/a", BSON( "1" << 2 ), true , true );
                ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(kp, frs1, BSONObj() ) );
                FieldRangeSet frs2( "n/a", BSON( "01" << 3), true , true );
                ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(kp, frs2, BSON( "01" << 1 ) ) );
                FieldRangeSet frs3( "n/a", BSONObj() , true , true );
                ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(kp, frs3, BSON( "1" << 1 ) ) );
            }
        };

        namespace TwoD {

            /** A 2d index is optimal for a $within predicate on the indexed field. */
            class Within {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$within:{$box:[[100,0],[120,100]]}}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2d" ) );
                    ASSERT_EQUALS( OPTIMAL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2d index is useless for a $within predicate not on the indexed field. */
            class WithinUnindexed {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$within:{$box:[[100,0],[120,100]]}}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "b" << "2d" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2d index is optimal for a $near predicate on the indexed field. */
            class Near {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$near:[100,0]}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2d" ) );
                    ASSERT_EQUALS( OPTIMAL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /**
             * A 2d index is helpful for a location object equality predicate on the indexed field.
             */
            class LocationObject {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{lat:4,lon:5}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2d" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /**
             * A 2d index is helpful for a predicate object on the indexed field.  This is a
             * descriptive rather than normative test.  See SERVER-8644.
             */
            class PredicateObject {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$gt:4,$lt:5}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2d" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2d index is helpful for an array equality predicate on the indexed field. */
            class Array {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:[1,1]}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2d" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2d index is useless for a numeric equality predicate on the indexed field. */
            class Number {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:1}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2d" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2d index is useless without a predicate on the indexed field. */
            class Missing {
            public:
                void run() {
                    BSONObj query = fromjson( "{}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2d" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };

            /**
             * A 2d index is useless for a $near predicate on the indexed field within a $and
             * clause.  This is a descriptive rather than normative test.  See SERVER-4572.
             */
            class InsideAnd {
            public:
                void run() {
                    BSONObj query = fromjson( "{$and:[{a:{$near:[100,0]}}]}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2d" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /**
             * A 2d index is optimal for a $near predicate on the indexed field adjacent to a $or
             * expression.
             */
            class OutsideOr {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$near:[100,0]},$or:[{b:1}]}" );
                    OrRangeGenerator org( "n/a", query, true );
                    scoped_ptr<FieldRangeSetPair> frsp( org.topFrsp() );
                    BSONObj spec( BSON( "a" << "2d" ) );
                    ASSERT_EQUALS( OPTIMAL,
                                   IndexSelection::isSuitableFor(spec, frsp->getSingleKeyFRS(), BSONObj() ) );
                }
            };

        } // namespace TwoD

        namespace S2 {

            /** A 2dsphere index is optimal for a $geoIntersects predicate on the indexed field. */
            class GeoIntersects {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$geoIntersects:{$geometry:{type:'Point',"
                                              "coordinates:[40,5]}}}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2dsphere" ) );
                    ASSERT_EQUALS( OPTIMAL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2dsphere index is useless for a $geoIntersects predicate on an unindexed field. */
            class GeoIntersectsUnindexed {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$geoIntersects:{$geometry:{type:'Point',"
                                              "coordinates:[40,5]}}}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "b" << "2dsphere" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2dsphere index is optimal for a $near predicate on the indexed field. */
            class Near {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$near:[100,0]}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2dsphere" ) );
                    ASSERT_EQUALS( OPTIMAL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /**
             * A 2dsphere index is useless for a location object equality predicate on the indexed
             * field.
             */
            class LocationObject {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{lat:4,lon:5}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2dsphere" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2dsphere index is useless for a predicate object on the indexed field. */
            class PredicateObject {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$gt:4,$lt:5}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2dsphere" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2dsphere index is helpful for an array equality predicate on the indexed field. */
            class Array {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:[1,1]}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2dsphere" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /**
             * A 2dsphere index is useless for a numeric equality predicate on the indexed field.
             */
            class Number {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:1}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2dsphere" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A 2dsphere index is useless without a predicate on the indexed field. */
            class Missing {
            public:
                void run() {
                    BSONObj query = fromjson( "{}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2dsphere" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };

            /**
             * A 2dsphere index is useless for a $near predicate on the indexed field within a $and
             * clause.  This is a descriptive rather than normative test.  See SERVER-4572.
             */
            class InsideAnd {
            public:
                void run() {
                    BSONObj query = fromjson( "{$and:[{a:{$near:[100,0]}}]}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "2dsphere" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /**
             * A 2dsphere index is optimal for a $near predicate on the indexed field adjacent to a
             * $or expression.
             */
            class OutsideOr {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$near:[100,0]},$or:[{b:1}]}" );
                    OrRangeGenerator org( "n/a", query, true );
                    scoped_ptr<FieldRangeSetPair> frsp( org.topFrsp() );
                    BSONObj spec( BSON( "a" << "2dsphere" ) );
                    ASSERT_EQUALS( OPTIMAL,
                                   IndexSelection::isSuitableFor(spec, frsp->getSingleKeyFRS(), BSONObj() ) );
                }
            };

        } // namespace S2

        namespace Hashed {

            /** A hashed index is helpful for an equality predicate on the indexed field. */
            class Equality {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:5}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "hashed" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A hashed index is useless for a range predicate on the indexed field. */
            class GreaterThan {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$gt:4}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "hashed" ) );
                    ASSERT_EQUALS( USELESS, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /** A hashed index is helpful for a set membership predicate on the indexed field. */
            class InSet {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:{$in:[1,2]}}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "hashed" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /**
             * A hashed index is helpful for an equality predicate on the indexed field, within a
             * singleton $and clause.
             */
            class AndEqualitySingleton {
            public:
                void run() {
                    BSONObj query = fromjson( "{$and:[{a:5}]}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "hashed" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /**
             * A hashed index is helpful for an equality predicate on the indexed field, within a
             * non singleton $and clause.
             */
            class AndEqualityNonSingleton {
            public:
                void run() {
                    BSONObj query = fromjson( "{$and:[{a:5},{b:5}]}" );
                    FieldRangeSet frs( "n/a", query, true, true );
                    BSONObj spec( BSON( "a" << "hashed" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frs, BSONObj() ) );
                }
            };
            
            /**
             * A hashed index is helpful for an equality predicate on the indexed field, within a
             * singleton $or clause.
             */
            class EqualityInsideSingletonOr {
            public:
                void run() {
                    BSONObj query = fromjson( "{$or:[{a:5}]}" );
                    OrRangeGenerator org( "n/a", query, true );
                    scoped_ptr<FieldRangeSetPair> frsp( org.topFrsp() );
                    BSONObj spec( BSON( "a" << "hashed" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frsp->getSingleKeyFRS(),
                                                              BSONObj() ) );
                }
            };
            
            /**
             * A hashed index is helpful for an equality predicate on the indexed field, within a
             * non standalone singleton $or clause.
             */
            class EqualityInsideNonStandaloneSingletonOr {
            public:
                void run() {
                    BSONObj query = fromjson( "{z:5,$or:[{a:5}]}" );
                    OrRangeGenerator org( "n/a", query, true );
                    scoped_ptr<FieldRangeSetPair> frsp( org.topFrsp() );
                    BSONObj spec( BSON( "a" << "hashed" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frsp->getSingleKeyFRS(),
                                                              BSONObj() ) );
                }
            };
            
            /**
             * A hashed index is helpful for an equality predicate on the indexed field, within a
             * non singleton $or clause.
             */
            class EqualityInsideNonSingletonOr {
            public:
                void run() {
                    BSONObj query = fromjson( "{$or:[{a:5},{b:5}]}" );
                    OrRangeGenerator org( "n/a", query, true );
                    scoped_ptr<FieldRangeSetPair> frsp( org.topFrsp() );
                    BSONObj spec( BSON( "a" << "hashed" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frsp->getSingleKeyFRS(),
                                                              BSONObj() ) );
                }
            };
            
            /**
             * A hashed index is helpful for an equality predicate on the indexed field, adjacent to
             * a $or clause.
             */
            class EqualityOutsideOr {
            public:
                void run() {
                    BSONObj query = fromjson( "{a:5,$or:[{z:5}]}" );
                    OrRangeGenerator org( "n/a", query, true );
                    scoped_ptr<FieldRangeSetPair> frsp( org.topFrsp() );
                    BSONObj spec( BSON( "a" << "hashed" ) );
                    ASSERT_EQUALS( HELPFUL, IndexSelection::isSuitableFor(spec, frsp->getSingleKeyFRS(),
                                                              BSONObj() ) );
                }
            };

        } // namespace Hashed

        // GeoHaystack does not implement its own suitability().  See SERVER-8645.
         
    } // namespace IndexSpecSuitability

    namespace MissingFieldTests {

        /** A missing field is represented as null in a btree index. */
        class BtreeIndexMissingField {
        public:
            void run() {
                BSONObj spec( BSON("key" << BSON( "a" << 1 ) ));
                ASSERT_EQUALS(jstNULL, IndexLegacy::getMissingField(spec).firstElement().type());
            }
        };
        
        /** A missing field is represented as null in a 2d index. */
        class TwoDIndexMissingField {
        public:
            void run() {
                BSONObj spec( BSON("key" << BSON( "a" << "2d" ) ));
                ASSERT_EQUALS(jstNULL, IndexLegacy::getMissingField(spec).firstElement().type());
            }
        };

        /** A missing field is represented with the hash of null in a hashed index. */
        class HashedIndexMissingField {
        public:
            void run() {
                BSONObj spec( BSON("key" << BSON( "a" << "hashed" ) ));
                BSONObj nullObj = BSON( "a" << BSONNULL );

                // Call getKeys on the nullObj.
                BSONObjSet nullFieldKeySet;
                HashAccessMethod::getKeysImpl(nullObj, "a", 0, 0, false, &nullFieldKeySet);
                BSONElement nullFieldFromKey = nullFieldKeySet.begin()->firstElement();

                ASSERT_EQUALS( HashAccessMethod::makeSingleKey( nullObj.firstElement(), 0, 0 ),
                               nullFieldFromKey.Long() );

                BSONObj missingField = IndexLegacy::getMissingField(spec);
                ASSERT_EQUALS( NumberLong, missingField.firstElement().type() );
                ASSERT_EQUALS( nullFieldFromKey, missingField.firstElement());
            }
        };

        /**
         * A missing field is represented with the hash of null in a hashed index.  This hash value
         * depends on the hash seed.
         */
        class HashedIndexMissingFieldAlternateSeed {
        public:
            void run() {
                BSONObj spec( BSON("key" << BSON( "a" << "hashed" ) <<  "seed" << 0x5eed ));
                BSONObj nullObj = BSON( "a" << BSONNULL );

                BSONObjSet nullFieldKeySet;
                HashAccessMethod::getKeysImpl(nullObj, "a", 0x5eed, 0, false, &nullFieldKeySet);
                BSONElement nullFieldFromKey = nullFieldKeySet.begin()->firstElement();

                ASSERT_EQUALS( HashAccessMethod::makeSingleKey( nullObj.firstElement(), 0x5eed, 0 ),
                               nullFieldFromKey.Long() );

                // Ensure that getMissingField recognizes that the seed is different (and returns
                // the right key).
                BSONObj missingField = IndexLegacy::getMissingField(spec);
                ASSERT_EQUALS( NumberLong, missingField.firstElement().type());
                ASSERT_EQUALS( nullFieldFromKey, missingField.firstElement());
            }
        };
        
    } // namespace MissingFieldTests
    
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
                for ( DiskLoc i = nsd()->firstExtent(); !i.isNull(); i = i.ext()->xnext ) {
                    int fileNo = i.ext()->firstRecord.a();
                    if ( fileNo == -1 )
                        continue;
                    for ( int j = i.ext()->firstRecord.getOfs(); j != DiskLoc::NullOfs;
                          j = DiskLoc( fileNo, j ).rec()->nextOfs() ) {
                        ++count;
                    }
                }
                ASSERT_EQUALS( count, nsd()->numRecords() );
                return count;
            }
            int nExtents() const {
                int count = 0;
                for ( DiskLoc i = nsd()->firstExtent(); !i.isNull(); i = i.ext()->xnext )
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

            /** Return the smallest DeletedRecord in deletedList, or DiskLoc() if none. */
            DiskLoc smallestDeletedRecord() {
                for( int i = 0; i < Buckets; ++i ) {
                    if ( !nsd()->deletedListEntry( i ).isNull() ) {
                        return nsd()->deletedListEntry( i );
                    }
                }
                return DiskLoc();
            }

            /**
             * 'cook' the deletedList by shrinking the smallest deleted record to size
             * 'newDeletedRecordSize'.
             */
            void cookDeletedList( int newDeletedRecordSize ) {

                // Extract the first DeletedRecord from the deletedList.
                DiskLoc deleted;
                for( int i = 0; i < Buckets; ++i ) {
                    if ( !nsd()->deletedListEntry( i ).isNull() ) {
                        deleted = nsd()->deletedListEntry( i );
                        nsd()->deletedListEntry( i ).writing().Null();
                        break;
                    }
                }
                ASSERT( !deleted.isNull() );

                // Shrink the DeletedRecord's size to newDeletedRecordSize.
                ASSERT_GREATER_THAN_OR_EQUALS( deleted.drec()->lengthWithHeaders(),
                                               newDeletedRecordSize );
                getDur().writingInt( deleted.drec()->lengthWithHeaders() ) = newDeletedRecordSize;

                // Re-insert the DeletedRecord into the deletedList bucket appropriate for its
                // new size.
                nsd()->deletedListEntry( NamespaceDetails::bucket( newDeletedRecordSize ) ).writing() =
                        deleted;
            }
        };

        class Create : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd() );
                ASSERT_EQUALS( 0, nRecords() );
                ASSERT( nsd()->firstExtent() == nsd()->capExtent() );
                DiskLoc initial = DiskLoc();
                initial.setInvalid();
                ASSERT( initial == nsd()->capFirstNewRecord() );
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


        /**
         * Test  Quantize record allocation size for various buckets
         *       @see NamespaceDetails::quantizeAllocationSpace()
         */
        class QuantizeFixedBuckets : public Base {
        public:
            void run() {
                create();
                // explicitly test for a set of known values
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(33),       36);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(1000),     1024);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(10001),    10240);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(100000),   106496);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(1000001),  1048576);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(10000000), 10223616);
            }
        };


        /**
         * Test  Quantize min/max record allocation size
         *       @see NamespaceDetails::quantizeAllocationSpace()
         */
        class QuantizeMinMaxBound : public Base {
        public:
            void run() {
                create();
                // test upper and lower bound
                const int maxSize = 16 * 1024 * 1024;
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(1), 2);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(maxSize), maxSize);
            }
        };

        /**
         * Test  Quantize record allocation on every boundary, as well as boundary-1
         *       @see NamespaceDetails::quantizeAllocationSpace()
         */
        class QuantizeRecordBoundary : public Base {
        public:
            void run() {
                create();
                for (int iBucket = 0; iBucket <= MaxBucket; ++iBucket) {
                    // for each bucket in range [min, max)
                    const int bucketSize = bucketSizes[iBucket];
                    const int prevBucketSize = (iBucket - 1 >= 0) ? bucketSizes[iBucket - 1] : 0;
                    const int intervalSize = bucketSize / 16;
                    for (int iBoundary = prevBucketSize;
                         iBoundary < bucketSize;
                         iBoundary += intervalSize) {
                        // for each quantization boundary within the bucket
                        for (int iSize = iBoundary - 1; iSize <= iBoundary; ++iSize) {
                            // test the quantization boundary - 1, and the boundary itself
                            const int quantized =
                                    NamespaceDetails::quantizeAllocationSpace(iSize);
                            // assert quantized size is greater than or equal to requested size
                            ASSERT(quantized >= iSize);
                            // assert quantized size is within one quantization interval of
                            // the requested size
                            ASSERT(quantized - iSize <= intervalSize);
                            // assert quantization is an idempotent operation
                            ASSERT(quantized ==
                                   NamespaceDetails::quantizeAllocationSpace(quantized));
                        }
                    }
                }
            }
        };

        /**
         * Except for the largest bucket, quantizePowerOf2AllocationSpace quantizes to the nearest
         * bucket size.
         */
        class QuantizePowerOf2ToBucketSize : public Base {
        public:
            void run() {
                create();
                for( int iBucket = 0; iBucket < MaxBucket - 1; ++iBucket ) {
                    int bucketSize = bucketSizes[ iBucket ];
                    int nextBucketSize = bucketSizes[ iBucket + 1 ];

                    // bucketSize - 1 is quantized to bucketSize.
                    ASSERT_EQUALS( bucketSize,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace
                                           ( bucketSize - 1 ) );

                    // bucketSize is quantized to nextBucketSize.
                    // Descriptive rather than normative test.
                    // SERVER-8311 A pre quantized size is rounded to the next quantum level.
                    ASSERT_EQUALS( nextBucketSize,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace
                                           ( bucketSize ) );

                    // bucketSize + 1 is quantized to nextBucketSize.
                    ASSERT_EQUALS( nextBucketSize,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace
                                           ( bucketSize + 1 ) );
                }

                // The next to largest bucket size - 1 is quantized to the next to largest bucket
                // size.
                ASSERT_EQUALS( bucketSizes[ MaxBucket - 1 ],
                               NamespaceDetails::quantizePowerOf2AllocationSpace
                                       ( bucketSizes[ MaxBucket - 1 ] - 1 ) );
            }
        };

        /**
         * Within the largest bucket, quantizePowerOf2AllocationSpace quantizes to the nearest
         * megabyte boundary.
         */
        class QuantizeLargePowerOf2ToMegabyteBoundary : public Base {
        public:
            void run() {
                create();
                
                // Iterate iSize over all 1mb boundaries from the size of the next to largest bucket
                // to the size of the largest bucket + 1mb.
                for( int iSize = bucketSizes[ MaxBucket - 1 ];
                     iSize <= bucketSizes[ MaxBucket ] + 0x100000;
                     iSize += 0x100000 ) {

                    // iSize - 1 is quantized to iSize.
                    ASSERT_EQUALS( iSize,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace( iSize - 1 ) );

                    // iSize is quantized to iSize + 1mb.
                    // Descriptive rather than normative test.
                    // SERVER-8311 A pre quantized size is rounded to the next quantum level.
                    ASSERT_EQUALS( iSize + 0x100000,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace( iSize ) );

                    // iSize + 1 is quantized to iSize + 1mb.
                    ASSERT_EQUALS( iSize + 0x100000,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace( iSize + 1 ) );
                }
            }
        };

        /** getRecordAllocationSize() returns its argument when the padding factor is 1.0. */
        class GetRecordAllocationSizeNoPadding : public Base {
        public:
            void run() {
                create();
                ASSERT_EQUALS( 1.0, nsd()->paddingFactor() );
                ASSERT_EQUALS( 300, nsd()->getRecordAllocationSize( 300 ) );
            }
            virtual string spec() const { return ""; }
        };

        /** getRecordAllocationSize() multiplies by a padding factor > 1.0. */
        class GetRecordAllocationSizeWithPadding : public Base {
        public:
            void run() {
                create();
                double paddingFactor = 1.2;
                nsd()->setPaddingFactor( paddingFactor );
                ASSERT_EQUALS( paddingFactor, nsd()->paddingFactor() );
                ASSERT_EQUALS( static_cast<int>( 300 * paddingFactor ),
                               nsd()->getRecordAllocationSize( 300 ) );
            }
            virtual string spec() const { return ""; }
        };

        /**
         * getRecordAllocationSize() quantizes to the nearest power of 2 when Flag_UsePowerOf2Sizes
         * is set.
         */
        class GetRecordAllocationSizePowerOf2 : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd()->setUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes ) );
                ASSERT( nsd()->isUserFlagSet( NamespaceDetails::Flag_UsePowerOf2Sizes ) );
                ASSERT_EQUALS( 512, nsd()->getRecordAllocationSize( 300 ) );
            }
            virtual string spec() const { return ""; }
        };

        
        /**
         * getRecordAllocationSize() quantizes to the nearest power of 2 when Flag_UsePowerOf2Sizes
         * is set, ignoring the padding factor.
         */
        class GetRecordAllocationSizePowerOf2PaddingIgnored : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd()->setUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes ) );
                ASSERT( nsd()->isUserFlagSet( NamespaceDetails::Flag_UsePowerOf2Sizes ) );
                nsd()->setPaddingFactor( 2.0 );
                ASSERT_EQUALS( 2.0, nsd()->paddingFactor() );
                ASSERT_EQUALS( 512, nsd()->getRecordAllocationSize( 300 ) );
            }
            virtual string spec() const { return ""; }
        };

        /** alloc() quantizes the requested size using quantizeAllocationSpace() rules. */
        class AllocQuantized : public Base {
        public:
            void run() {
                create();
                DiskLoc expectedLocation = nsd()->allocWillBeAt( ns(), 300 );
                DiskLoc actualLocation = nsd()->alloc( ns(), 300 );

                // The expected location returned by allocWillBeAt() matches alloc().
                ASSERT_EQUALS( expectedLocation, actualLocation );

                // The length of the allocated record is quantized.
                ASSERT_EQUALS( 320, actualLocation.rec()->lengthWithHeaders() );
            }
            virtual string spec() const { return ""; }
        };

        /** alloc() does not quantize records in capped collections. */
        class AllocCappedNotQuantized : public Base {
        public:
            void run() {
                create();
                DiskLoc loc = nsd()->alloc( ns(), 300 );
                ASSERT_EQUALS( 300, loc.rec()->lengthWithHeaders() );
            }
            virtual string spec() const { return "{capped:true,size:2048}"; }
        };

        /**
         * alloc() does not quantize records in index collections using quantizeAllocationSpace()
         * rules.
         */
        class AllocIndexNamespaceNotQuantized : public Base {
        public:
            void run() {
                create();

                // Find the indexNamespace name and indexNsd metadata pointer.
                int idIndexNo = nsd()->findIdIndex();
                IndexDetails& idx = nsd()->idx( idIndexNo );
                string indexNamespace = idx.indexNamespace();
                ASSERT( !NamespaceString::normal( indexNamespace.c_str() ) );
                NamespaceDetails* indexNsd = nsdetails( indexNamespace.c_str() );

                // Check that no quantization is performed.
                DiskLoc expectedLocation = indexNsd->allocWillBeAt( indexNamespace.c_str(), 300 );
                DiskLoc actualLocation = indexNsd->alloc( indexNamespace.c_str(), 300 );
                ASSERT_EQUALS( expectedLocation, actualLocation );
                ASSERT_EQUALS( 300, actualLocation.rec()->lengthWithHeaders() );
            }
        };

        /** alloc() quantizes records in index collections to the nearest multiple of 4. */
        class AllocIndexNamespaceSlightlyQuantized : public Base {
        public:
            void run() {
                create();

                // Find the indexNamespace name and indexNsd metadata pointer.
                int idIndexNo = nsd()->findIdIndex();
                IndexDetails& idx = nsd()->idx( idIndexNo );
                string indexNamespace = idx.indexNamespace();
                ASSERT( !NamespaceString::normal( indexNamespace.c_str() ) );
                NamespaceDetails* indexNsd = nsdetails( indexNamespace.c_str() );

                // Check that multiple of 4 quantization is performed.
                DiskLoc expectedLocation = indexNsd->allocWillBeAt( indexNamespace.c_str(), 298 );
                DiskLoc actualLocation = indexNsd->alloc( indexNamespace.c_str(), 298 );
                ASSERT_EQUALS( expectedLocation, actualLocation );
                ASSERT_EQUALS( 300, actualLocation.rec()->lengthWithHeaders() );
            }
        };

        /** alloc() returns a non quantized record larger than the requested size. */
        class AllocUseNonQuantizedDeletedRecord : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 310 );
                DiskLoc expectedLocation = nsd()->allocWillBeAt( ns(), 300 );
                DiskLoc actualLocation = nsd()->alloc( ns(), 300 );
                ASSERT_EQUALS( expectedLocation, actualLocation );
                ASSERT_EQUALS( 310, actualLocation.rec()->lengthWithHeaders() );

                // No deleted records remain after alloc returns the non quantized record.
                ASSERT_EQUALS( DiskLoc(), smallestDeletedRecord() );
            }
            virtual string spec() const { return ""; }
        };

        /** alloc() returns a non quantized record equal to the requested size. */
        class AllocExactSizeNonQuantizedDeletedRecord : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 300 );
                DiskLoc expectedLocation = nsd()->allocWillBeAt( ns(), 300 );
                DiskLoc actualLocation = nsd()->alloc( ns(), 300 );
                ASSERT_EQUALS( expectedLocation, actualLocation );
                ASSERT_EQUALS( 300, actualLocation.rec()->lengthWithHeaders() );
                ASSERT_EQUALS( DiskLoc(), smallestDeletedRecord() );
            }
            virtual string spec() const { return ""; }
        };

        /**
         * alloc() returns a non quantized record equal to the quantized size plus some extra space
         * too small to make a DeletedRecord.
         */
        class AllocQuantizedWithExtra : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 343 );
                DiskLoc expectedLocation = nsd()->allocWillBeAt( ns(), 300 );
                DiskLoc actualLocation = nsd()->alloc( ns(), 300 );
                ASSERT_EQUALS( expectedLocation, actualLocation );
                ASSERT_EQUALS( 343, actualLocation.rec()->lengthWithHeaders() );
                ASSERT_EQUALS( DiskLoc(), smallestDeletedRecord() );
            }
            virtual string spec() const { return ""; }
        };
        
        /**
         * alloc() returns a quantized record when the extra space in the reclaimed deleted record
         * is large enough to form a new deleted record.
         */
        class AllocQuantizedWithoutExtra : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 344 );
                DiskLoc expectedLocation = nsd()->allocWillBeAt( ns(), 300 );
                DiskLoc actualLocation = nsd()->alloc( ns(), 300 );
                ASSERT_EQUALS( expectedLocation, actualLocation );

                // The returned record is quantized from 300 to 320.
                ASSERT_EQUALS( 320, actualLocation.rec()->lengthWithHeaders() );

                // A new 24 byte deleted record is split off.
                ASSERT_EQUALS( 24, smallestDeletedRecord().drec()->lengthWithHeaders() );
            }
            virtual string spec() const { return ""; }
        };

        /**
         * A non quantized deleted record within 1/8 of the requested size is returned as is, even
         * if a quantized portion of the deleted record could be used instead.
         */
        class AllocNotQuantizedNearDeletedSize : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 344 );
                DiskLoc expectedLocation = nsd()->allocWillBeAt( ns(), 319 );
                DiskLoc actualLocation = nsd()->alloc( ns(), 319 );
                ASSERT_EQUALS( expectedLocation, actualLocation );

                // Even though 319 would be quantized to 320 and 344 - 320 == 24 could become a new
                // deleted record, the entire deleted record is returned because
                // ( 344 - 320 ) < ( 320 >> 3 ).
                ASSERT_EQUALS( 344, actualLocation.rec()->lengthWithHeaders() );
                ASSERT_EQUALS( DiskLoc(), smallestDeletedRecord() );
            }
            virtual string spec() const { return ""; }
        };

        /** An attempt to allocate a record larger than the largest deleted record fails. */
        class AllocFailsWithTooSmallDeletedRecord : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 299 );

                // allocWillBeAt() and alloc() fail.
                ASSERT( nsd()->allocWillBeAt( ns(), 300 ).isNull() );
                ASSERT( nsd()->alloc( ns(), 300 ).isNull() );
            }
            virtual string spec() const { return ""; }
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
                    scoped_ptr<ForwardCappedCursor> c( ForwardCappedCursor::make( nsd ) );
                    first = c->currLoc();
                    ASSERT( !first.isNull() );
                    ASSERT( first != last ) ;
                }

                nsd->cappedTruncateAfter(ns(), truncAt, false);
                ASSERT_EQUALS( nsd->numRecords() , 28 );

                {
                    scoped_ptr<ForwardCappedCursor> c( ForwardCappedCursor::make( nsd ) );
                    ASSERT( first == c->currLoc() );
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
                nsd()->deletedListEntry( 2 ) = nsd()->cappedListOfAllDeletedRecords().drec()->nextDeleted().drec()->nextDeleted();
                nsd()->cappedListOfAllDeletedRecords().drec()->nextDeleted().drec()->nextDeleted().writing() = DiskLoc();
                nsd()->cappedLastDelRecLastExtent().Null();
                NamespaceDetails *d = nsd();
                zero( &d->capExtent() );
                zero( &d->capFirstNewRecord() );

                nsd();

                ASSERT( nsd()->firstExtent() == nsd()->capExtent() );
                ASSERT( nsd()->capExtent().getOfs() != 0 );
                ASSERT( !nsd()->capFirstNewRecord().isValid() );
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
        //                long long big = DataFile::maxSize() - DataFileHeader::HeaderSize;
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
                _fieldRangeSet( ns(), BSON( "a" << 1 ), true, true ),
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

        class SwapIndexEntriesTest : public Base {
        public:
            void run() {
                create();
                NamespaceDetails *nsd = nsdetails(ns());

                // Set 2 & 54 as multikey
                nsd->setIndexIsMultikey(ns(), 2, true);
                nsd->setIndexIsMultikey(ns(), 54, true);
                ASSERT(nsd->isMultikey(2));
                ASSERT(nsd->isMultikey(54));

                // Flip 2 & 47
                nsd->setIndexIsMultikey(ns(), 2, false);
                nsd->setIndexIsMultikey(ns(), 47, true);
                ASSERT(!nsd->isMultikey(2));
                ASSERT(nsd->isMultikey(47));

                // Reset entries that are already true
                nsd->setIndexIsMultikey(ns(), 54, true);
                nsd->setIndexIsMultikey(ns(), 47, true);
                ASSERT(nsd->isMultikey(54));
                ASSERT(nsd->isMultikey(47));

                // Two non-multi-key
                nsd->setIndexIsMultikey(ns(), 2, false);
                nsd->setIndexIsMultikey(ns(), 43, false);
                ASSERT(!nsd->isMultikey(2));
                ASSERT(nsd->isMultikey(54));
                ASSERT(nsd->isMultikey(47));
                ASSERT(!nsd->isMultikey(43));
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
            add< IndexSpecSuitability::IndexedQueryField >();
            add< IndexSpecSuitability::NoIndexedQueryField >();
            add< IndexSpecSuitability::ChildOfIndexQueryField >();
            add< IndexSpecSuitability::ParentOfIndexQueryField >();
            add< IndexSpecSuitability::ObjectMatchCompletingIndexField >();
            add< IndexSpecSuitability::NoIndexedQueryField >();
            add< IndexSpecSuitability::IndexedOrderField >();
            add< IndexSpecSuitability::IndexedReverseOrderField >();
            add< IndexSpecSuitability::NonPrefixIndexedOrderField >();
            add< IndexSpecSuitability::NoIndexedOrderField >();
            add< IndexSpecSuitability::ChildOfIndexOrderField >();
            add< IndexSpecSuitability::ParentOfIndexOrderField >();
            add< IndexSpecSuitability::NumericFieldSuitability >();
            add< IndexSpecSuitability::TwoD::Within >();
            add< IndexSpecSuitability::TwoD::WithinUnindexed >();
            add< IndexSpecSuitability::TwoD::Near >();
            add< IndexSpecSuitability::TwoD::LocationObject >();
            add< IndexSpecSuitability::TwoD::PredicateObject >();
            add< IndexSpecSuitability::TwoD::Array >();
            add< IndexSpecSuitability::TwoD::Number >();
            add< IndexSpecSuitability::TwoD::Missing >();
            add< IndexSpecSuitability::TwoD::InsideAnd >();
            add< IndexSpecSuitability::TwoD::OutsideOr >();
            add< IndexSpecSuitability::S2::GeoIntersects >();
            add< IndexSpecSuitability::S2::GeoIntersectsUnindexed >();
            add< IndexSpecSuitability::S2::Near >();
            add< IndexSpecSuitability::S2::LocationObject >();
            add< IndexSpecSuitability::S2::PredicateObject >();
            add< IndexSpecSuitability::S2::Array >();
            add< IndexSpecSuitability::S2::Number >();
            add< IndexSpecSuitability::S2::Missing >();
            add< IndexSpecSuitability::S2::InsideAnd >();
            add< IndexSpecSuitability::S2::OutsideOr >();
            add< IndexSpecSuitability::Hashed::Equality >();
            add< IndexSpecSuitability::Hashed::GreaterThan >();
            add< IndexSpecSuitability::Hashed::InSet >();
            add< IndexSpecSuitability::Hashed::AndEqualitySingleton >();
            add< IndexSpecSuitability::Hashed::AndEqualityNonSingleton >();
            add< IndexSpecSuitability::Hashed::EqualityInsideSingletonOr >();
            add< IndexSpecSuitability::Hashed::EqualityInsideNonStandaloneSingletonOr >();
            add< IndexSpecSuitability::Hashed::EqualityInsideNonSingletonOr >();
            add< IndexSpecSuitability::Hashed::EqualityOutsideOr >();
            add< NamespaceDetailsTests::Create >();
            add< NamespaceDetailsTests::SingleAlloc >();
            add< NamespaceDetailsTests::Realloc >();
            add< NamespaceDetailsTests::QuantizeMinMaxBound >();
            add< NamespaceDetailsTests::QuantizeFixedBuckets >();
            add< NamespaceDetailsTests::QuantizeRecordBoundary >();
            add< NamespaceDetailsTests::QuantizePowerOf2ToBucketSize >();
            add< NamespaceDetailsTests::QuantizeLargePowerOf2ToMegabyteBoundary >();
            add< NamespaceDetailsTests::GetRecordAllocationSizeNoPadding >();
            add< NamespaceDetailsTests::GetRecordAllocationSizeWithPadding >();
            add< NamespaceDetailsTests::GetRecordAllocationSizePowerOf2 >();
            add< NamespaceDetailsTests::GetRecordAllocationSizePowerOf2PaddingIgnored >();
            add< NamespaceDetailsTests::AllocQuantized >();
            add< NamespaceDetailsTests::AllocCappedNotQuantized >();
            add< NamespaceDetailsTests::AllocIndexNamespaceNotQuantized >();
            add< NamespaceDetailsTests::AllocIndexNamespaceSlightlyQuantized >();
            add< NamespaceDetailsTests::AllocUseNonQuantizedDeletedRecord >();
            add< NamespaceDetailsTests::AllocExactSizeNonQuantizedDeletedRecord >();
            add< NamespaceDetailsTests::AllocQuantizedWithExtra >();
            add< NamespaceDetailsTests::AllocQuantizedWithoutExtra >();
            add< NamespaceDetailsTests::AllocNotQuantizedNearDeletedSize >();
            add< NamespaceDetailsTests::AllocFailsWithTooSmallDeletedRecord >();
            add< NamespaceDetailsTests::TwoExtent >();
            add< NamespaceDetailsTests::TruncateCapped >();
            add< NamespaceDetailsTests::Migrate >();
            add< NamespaceDetailsTests::SwapIndexEntriesTest >();
            //            add< NamespaceDetailsTests::BigCollection >();
            add< NamespaceDetailsTests::Size >();
            add< NamespaceDetailsTests::SetIndexIsMultikey >();
            add< NamespaceDetailsTransientTests::ClearQueryCache >();
            add< MissingFieldTests::BtreeIndexMissingField >();
            add< MissingFieldTests::TwoDIndexMissingField >();
            add< MissingFieldTests::HashedIndexMissingField >();
            add< MissingFieldTests::HashedIndexMissingFieldAlternateSeed >();
        }
    } myall;
} // namespace NamespaceTests

