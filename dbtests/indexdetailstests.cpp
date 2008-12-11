// indexdetailstests.cpp : IndexDetails unit tests.
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
				if( id_.info.isNull() )
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
			static const char* ns() { return "sys.unittest.indexdetailstests"; }
			const IndexDetails& id() { return id_; }
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
		private:
			IndexDetails id_;
		};
		
		class Create : public Base {
		public:
			void run() {
				create();
				ASSERT_EQUALS( "testIndex", id().indexName() );
				ASSERT_EQUALS( ns(), id().parentNS() );
				// check equal
				ASSERT( !id().keyPattern().woCompare( key() ) );
			}
		};
		
		class GetKeysFromObjectSimple : public Base {
		public:
			void run() {
				create();
				BSONObjBuilder b, e;
				b.append( "b", 4 );
				b.append( "a", 5 );
				e.append( "a", 5 );
				set< BSONObj > keys;
				id().getKeysFromObject( b.done(), keys );
				ASSERT_EQUALS( 1, keys.size() );
				ASSERT( !keys.begin()->woCompare( e.done() ) );
			}
		};
		
		class GetKeysFromObjectDotted : public Base {
		public:
			void run() {
				create();
				BSONObjBuilder a, e, b;
				b.append( "b", 4 );
				a.append( "a", b.done() );
				e.append( "a", b.done() );
				a.append( "c", "foo" );
				set< BSONObj > keys;
				id().getKeysFromObject( a.done(), keys );
				ASSERT_EQUALS( 1, keys.size() );
				// FIXME Why doesn't woCompare expand sub elements?
				// ASSERT( !keys.begin()->woCompare( e.done() ) );
				cout << "first key: " << keys.begin()->toString() << endl;
				ASSERT_EQUALS( string( "b" ), keys.begin()->firstElement().fieldName() );
				ASSERT_EQUALS( 4, keys.begin()->firstElement().number() );
			}
		private:
			virtual BSONObj key() const { return aDotB(); }
		};
		
		class GetKeysFromArraySimple : public Base {
		public:
			void run() {
				create();
				BSONObjBuilder b;
				b.append( "a", shortArray()) ;
				
				set< BSONObj > keys;
				id().getKeysFromObject( b.done(), keys );
				ASSERT_EQUALS( 3, keys.size() );
				int j = 1;
				for( set< BSONObj >::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
					ASSERT_EQUALS( string( "a" ), i->firstElement().fieldName() );
					ASSERT_EQUALS( j, i->firstElement().number() );
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
				
				set< BSONObj > keys;
				id().getKeysFromObject( b.done(), keys );
				ASSERT_EQUALS( 3, keys.size() );
				int j = 1;
				for( set< BSONObj >::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
					ASSERT_EQUALS( string( "a" ), i->firstElement().fieldName() );
					ASSERT_EQUALS( j, i->firstElement().number() );
					ASSERT_EQUALS( 2, i->getField( "b" ).number() );
				}
			}
		private:
			virtual BSONObj key() const { return aAndB(); }
		};
		
		class GetKeysFromArraySecondElement : public Base {
		public:
			void run() {
				create();
				BSONObjBuilder b;
				b.append( "first", 5 );
				b.append( "a", shortArray()) ;
				
				set< BSONObj > keys;
				id().getKeysFromObject( b.done(), keys );
				ASSERT_EQUALS( 3, keys.size() );
				int j = 1;
				for( set< BSONObj >::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
					ASSERT_EQUALS( string( "first" ), i->firstElement().fieldName() );
					ASSERT_EQUALS( j, i->getField( "a" ).number() );
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
				
				set< BSONObj > keys;
				id().getKeysFromObject( a.done(), keys );
				cout << "key: " << keys.begin()->toString() << endl;
				ASSERT_EQUALS( 3, keys.size() );
				int j = 1;
				for( set< BSONObj >::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
					ASSERT_EQUALS( string( "b" ), i->firstElement().fieldName() );
					ASSERT_EQUALS( j, i->firstElement().number() );
				}
			}
		private:
			virtual BSONObj key() const { return aDotB(); }
		};
		
		class ParallelArraysBasic : public Base {
		public:
			void run() {
				create();
				BSONObjBuilder b;
				b.append( "a", shortArray() );
				b.append( "b", shortArray() );
				
				set< BSONObj > keys;
				ASSERT_EXCEPTION( id().getKeysFromObject( b.done(), keys ),
								 UserAssertionException );
			}
		private:
			virtual BSONObj key() const { return aAndB(); }
		};
	} // namespace IndexDetailsTests
	
	// TODO
	// array subelement
	// parallel arrays complex
	// allowed multi array indexes
	
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
		}
	};
}

UnitTest::TestPtr namespaceTests() {
	return UnitTest::createSuite< NamespaceTests::All >();
}