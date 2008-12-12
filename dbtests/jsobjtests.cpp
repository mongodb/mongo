// jsobjtests.h : Test suite generator headers.
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

#include "../db/jsobj.h"

#include "dbtests.h"

namespace JsobjTests {
	namespace BSONObjTests {
		class Create {
		public:
			void run() {
				BSONObj b;
				ASSERT_EQUALS( 0, b.nFields() );
			}
		};

		class Base {
		protected:
			static BSONObj basic( const char *name, int val ) {
				BSONObjBuilder b;
				b.appendInt( name, val );
				return b.doneAndDecouple();
			}
			static BSONObj basic( const char *name, vector< int > val ) {
				BSONObjBuilder b;
				b.appendIntArray( name, val );
				return b.doneAndDecouple();
			}
			template< class T >
			static BSONObj basic( const char *name, T val ) {
				BSONObjBuilder b;
				b.append( name, val );
				return b.doneAndDecouple();
			}
		};
		
		class WoCompareBasic : public Base {
		public:
			void run() {
				ASSERT( basic( "a", 1 ).woCompare( basic( "a", 1 ) ) == 0 );
				ASSERT( basic( "a", 2 ).woCompare( basic( "a", 1 ) ) > 0 );
				ASSERT( basic( "a", 1 ).woCompare( basic( "a", 2 ) ) < 0 );
				// field name comparison
				ASSERT( basic( "a", 1 ).woCompare( basic( "b", 1 ) ) < 0 );
			}
		};
		
		class NumericCompareBasic : public Base {
		public:
			void run() {
				ASSERT( basic( "a", 1 ).woCompare( basic( "a", 1.0 ) ) == 0 );
			}
		};
		
		class WoCompareEmbeddedObject : public Base {
		public:
			void run() {
				ASSERT( basic( "a", basic( "b", 1 ) ).woCompare
					   ( basic( "a", basic( "b", 1.0 ) ) ) == 0 );
				ASSERT( basic( "a", basic( "b", 1 ) ).woCompare
					   ( basic( "a", basic( "b", 2 ) ) ) < 0 );
			}
		};
		
		class WoCompareEmbeddedArray : public Base {
		public:
			void run() {
				vector< int > i;
				i.push_back( 1 );
				i.push_back( 2 );
				vector< double > d;
				d.push_back( 1 );
				d.push_back( 2 );
				ASSERT( basic( "a", i ).woCompare( basic( "a", d ) ) == 0 );

				vector< int > j;
				j.push_back( 1 );
				j.push_back( 3 );
				ASSERT( basic( "a", i ).woCompare( basic( "a", j ) ) < 0 );
			}
		};
	} // namespace BSONObjTests

	class All : public UnitTest::Suite {
	public:
		All() {
			add< BSONObjTests::Create >();	
			add< BSONObjTests::WoCompareBasic >();	
			add< BSONObjTests::NumericCompareBasic >();
			add< BSONObjTests::WoCompareEmbeddedObject >();	
			add< BSONObjTests::WoCompareEmbeddedArray >();	
		}
	};
	
} // namespace JsobjTests

UnitTest::TestPtr jsobjTests() {
	return UnitTest::createSuite< JsobjTests::All >();
}