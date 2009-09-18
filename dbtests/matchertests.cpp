// matchertests.cpp : matcher unit tests
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

#include "../db/matcher.h"

#include "../db/json.h"

#include "dbtests.h"

namespace MatcherTests {

    class Basic {
    public:
        void run() {
            BSONObj query = fromjson( "{\"a\":\"b\"}" );
            JSMatcher m( query );
            ASSERT( m.matches( fromjson( "{\"a\":\"b\"}" ) ) );
        }
    };
    
    class DoubleEqual {
    public:
        void run() {
            BSONObj query = fromjson( "{\"a\":5}" );
            JSMatcher m( query );
            ASSERT( m.matches( fromjson( "{\"a\":5}" ) ) );            
        }
    };
    
    class MixedNumericEqual {
    public:
        void run() {
            BSONObjBuilder query;
            query.append( "a", 5 );
            JSMatcher m( query.done() );
            ASSERT( m.matches( fromjson( "{\"a\":5}" ) ) );            
        }        
    };
    
    class MixedNumericGt {
    public:
        void run() {
            BSONObj query = fromjson( "{\"a\":{\"$gt\":4}}" );
            JSMatcher m( query );
            BSONObjBuilder b;
            b.append( "a", 5 );
            ASSERT( m.matches( b.done() ) );
        }        
    };
    
    class MixedNumericIN {
    public:
        void run(){
            BSONObj query = fromjson( "{ a : { $in : [4,6] } }" );
            ASSERT_EQUALS( 4 , query["a"].embeddedObject()["$in"].embeddedObject()["0"].number() );
            ASSERT_EQUALS( NumberDouble , query["a"].embeddedObject()["$in"].embeddedObject()["0"].type() );
            
            JSMatcher m( query );

            {
                BSONObjBuilder b;
                b.append( "a" , 4.0 );
                ASSERT( m.matches( b.done() ) );
            }

            {
                BSONObjBuilder b;
                b.append( "a" , 5 );
                ASSERT( ! m.matches( b.done() ) );
            }


            {
                BSONObjBuilder b;
                b.append( "a" , 4 );
                ASSERT( m.matches( b.done() ) );
            }
                
        }
    };

    
    class Size {
    public:
        void run() {
            JSMatcher m( fromjson( "{a:{$size:4}}" ) );
            ASSERT( m.matches( fromjson( "{a:[1,2,3,4]}" ) ) );
            ASSERT( !m.matches( fromjson( "{a:[1,2,3]}" ) ) );
            ASSERT( !m.matches( fromjson( "{a:[1,2,3,'a','b']}" ) ) );
            ASSERT( !m.matches( fromjson( "{a:[[1,2,3,4]]}" ) ) );
        }        
    };
    

    class All : public Suite {
    public:
        All() : Suite( "matcher" ){
        }
        
        void setupTests(){
            add< Basic >();
            add< DoubleEqual >();
            add< MixedNumericEqual >();
            add< MixedNumericGt >();
            add< MixedNumericIN >();
            add< Size >();
        }
    } dball;
    
} // namespace MatcherTests

