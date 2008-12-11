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
				BSONObjBuilder builder;
				builder.append( "ns", ns() );
				builder.append( "name", "testIndex" );
				builder.append( "key", key() );
				BSONObj bobj = builder.done();
				id_.info = theDataFileMgr.insert( ns(), bobj.objdata(), bobj.objsize() );
				// head not needed for current tests
				// idx_.head = BtreeBucket::addHead( id_ );			
			}
			~Base() {
				// FIXME cleanup all btree buckets.
				theDataFileMgr.deleteRecord( ns(), id_.info.rec(), id_.info );
				ASSERT( theDataFileMgr.findAll( ns() )->eof() );
			}
		protected:
			static const char* ns() { return "sys.unittest.indexdetailstests"; }
			const IndexDetails& id() { return id_; }
			virtual BSONObj key() const {
				BSONObjBuilder k;
				k.append( "a", 1 );
				return k.doneAndDecouple();
			}
			BSONObj aDotB() const {
				BSONObjBuilder a;
				BSONObjBuilder b;
				b.append( "b", 1 );
				a.append( "a", b.done() );
				return a.doneAndDecouple();
			}
		private:
			IndexDetails id_;
		};
		
		class Create : public Base {
		public:
			void run() {
				ASSERT_EQUALS( "testIndex", id().indexName() );
				ASSERT_EQUALS( ns(), id().parentNS() );
				// check equal
				ASSERT( !id().keyPattern().woCompare( key() ) );
			}
		};
		
		class GetKeysFromObjectSimple : public Base {
		public:
			void run() {
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
				ASSERT_EQUALS( string( "a" ), keys.begin()->firstElement().fieldName() );
				ASSERT_EQUALS( string( "b" ), keys.begin()->firstElement().embeddedObject().firstElement().fieldName() );
				ASSERT_EQUALS( 4, keys.begin()->firstElement().embeddedObject().firstElement().number() );
			}
		private:
			virtual BSONObj key() const { return aDotB(); }
		};
		
		class GetKeysFromArraySimple : public Base {
		public:
			void run() {
				vector< int > a;
				a.push_back( 1 );
				a.push_back( 2 );
				a.push_back( 3 );
				BSONObjBuilder b;
				b.append( "a", a );
				
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
		
		class GetKeysFromSecondLevelArray : public Base {
		public:
			void run() {
				vector< int > a;
				a.push_back( 1 );
				a.push_back( 2 );
				a.push_back( 3 );
				BSONObjBuilder b;
				b.append( "b", a );
				BSONObjBuilder c;
				c.append( "a", b.done() );
				
				set< BSONObj > keys;
				id().getKeysFromObject( c.done(), keys );
				ASSERT_EQUALS( 3, keys.size() );
				int j = 1;
				for( set< BSONObj >::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
					ASSERT_EQUALS( string( "a" ), i->firstElement().fieldName() );
					ASSERT_EQUALS( string( "a" ), i->firstElement().embeddedObject().firstElement().fieldName() );
					ASSERT_EQUALS( j, i->firstElement().embeddedObject().firstElement().number() );
				}
			}
		private:
			virtual BSONObj key() const { return aDotB(); }
		};
	} // namespace IndexDetailsTests
	
	class All : public UnitTest::Suite {
	public:
		All() {
			add< IndexDetailsTests::Create >();
			add< IndexDetailsTests::GetKeysFromObjectSimple >();
			add< IndexDetailsTests::GetKeysFromObjectDotted >();
			add< IndexDetailsTests::GetKeysFromArraySimple >();
			// Not working yet
			//add< IndexDetailsTests::GetKeysFromSecondLevelArray >();
		}
	};
}

UnitTest::TestPtr namespaceTests() {
	return UnitTest::createSuite< NamespaceTests::All >();
}