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

#include "pch.h"
#include "../util/timer.h"

#include "../db/matcher.h"
#include "../db/json.h"

#include "dbtests.h"



namespace MatcherTests {

    class Basic {
    public:
        void run() {
            BSONObj query = fromjson( "{\"a\":\"b\"}" );
            Matcher m( query );
            ASSERT( m.matches( fromjson( "{\"a\":\"b\"}" ) ) );
        }
    };

    class DoubleEqual {
    public:
        void run() {
            BSONObj query = fromjson( "{\"a\":5}" );
            Matcher m( query );
            ASSERT( m.matches( fromjson( "{\"a\":5}" ) ) );
        }
    };

    class MixedNumericEqual {
    public:
        void run() {
            BSONObjBuilder query;
            query.append( "a", 5 );
            Matcher m( query.done() );
            ASSERT( m.matches( fromjson( "{\"a\":5}" ) ) );
        }
    };

    class MixedNumericGt {
    public:
        void run() {
            BSONObj query = fromjson( "{\"a\":{\"$gt\":4}}" );
            Matcher m( query );
            BSONObjBuilder b;
            b.append( "a", 5 );
            ASSERT( m.matches( b.done() ) );
        }
    };

    class MixedNumericIN {
    public:
        void run() {
            BSONObj query = fromjson( "{ a : { $in : [4,6] } }" );
            ASSERT_EQUALS( 4 , query["a"].embeddedObject()["$in"].embeddedObject()["0"].number() );
            ASSERT_EQUALS( NumberInt , query["a"].embeddedObject()["$in"].embeddedObject()["0"].type() );

            Matcher m( query );

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

    class MixedNumericEmbedded {
    public:
        void run() {
            Matcher m( BSON( "a" << BSON( "x" << 1 ) ) );
            ASSERT( m.matches( BSON( "a" << BSON( "x" << 1 ) ) ) );
            ASSERT( m.matches( BSON( "a" << BSON( "x" << 1.0 ) ) ) );
        }
    };

    class Size {
    public:
        void run() {
            Matcher m( fromjson( "{a:{$size:4}}" ) );
            ASSERT( m.matches( fromjson( "{a:[1,2,3,4]}" ) ) );
            ASSERT( !m.matches( fromjson( "{a:[1,2,3]}" ) ) );
            ASSERT( !m.matches( fromjson( "{a:[1,2,3,'a','b']}" ) ) );
            ASSERT( !m.matches( fromjson( "{a:[[1,2,3,4]]}" ) ) );
        }
    };


    class TimingBase {
    public:
        long time( const BSONObj& patt , const BSONObj& obj ) {
            Matcher m( patt );
            Timer t;
            for ( int i=0; i<10000; i++ ) {
                ASSERT( m.matches( obj ) );
            }
            return t.millis();
        }
    };

    class AllTiming : public TimingBase {
    public:
        void run() {
            long normal = time( BSON( "x" << 5 ) , BSON( "x" << 5 ) );
            long all = time( BSON( "x" << BSON( "$all" << BSON_ARRAY( 5 ) ) ) , BSON( "x" << 5 ) );

            cout << "normal: " << normal << " all: " << all << endl;
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "matcher" ) {
        }

        void setupTests() {
            add< Basic >();
            add< DoubleEqual >();
            add< MixedNumericEqual >();
            add< MixedNumericGt >();
            add< MixedNumericIN >();
            add< Size >();
            add< MixedNumericEmbedded >();
            add< AllTiming >();
        }
    } dball;

} // namespace MatcherTests

