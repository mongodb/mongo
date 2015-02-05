/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/jsobj.h"
#include "mongo/util/startup_test.h"

namespace mongo {
    struct BsonUnitTest : public StartupTest {
        void testRegex() {

            BSONObjBuilder b;
            b.appendRegex("x", "foo");
            BSONObj o = b.done();

            BSONObjBuilder c;
            c.appendRegex("x", "goo");
            BSONObj p = c.done();

            verify( !o.binaryEqual( p ) );
            verify( o.woCompare( p ) < 0 );

        }
        void testoid() {
            // hardcoded so that we don't need to generate OIDs before initializers run
            OID id("541c5fa6ababec1be47e21b5");
            //            sleepsecs(3);
            OID b;
            // goes with sleep above...
            // b.init();

            b.init( id.toString() );
            verify( b == id );
        }

        void testbounds() {
            BSONObj l , r;
            {
                BSONObjBuilder b;
                b.append( "x" , std::numeric_limits<long long>::max() );
                l = b.obj();
            }
            {
                BSONObjBuilder b;
                b.append( "x" , std::numeric_limits<double>::max() );
                r = b.obj();
            }
            verify( l.woCompare( r ) < 0 );
            verify( r.woCompare( l ) > 0 );
            {
                BSONObjBuilder b;
                b.append( "x" , std::numeric_limits<int>::max() );
                l = b.obj();
            }
            verify( l.woCompare( r ) < 0 );
            verify( r.woCompare( l ) > 0 );
        }

        void testorder() {
            {
                BSONObj x,y,z;
                { BSONObjBuilder b; b.append( "x" , (long long)2 ); x = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (int)3 ); y = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (long long)4 ); z = b.obj(); }
                verify( x.woCompare( y ) < 0 );
                verify( x.woCompare( z ) < 0 );
                verify( y.woCompare( x ) > 0 );
                verify( z.woCompare( x ) > 0 );
                verify( y.woCompare( z ) < 0 );
                verify( z.woCompare( y ) > 0 );
            }

            {
                BSONObj ll,d,i,n,u;
                { BSONObjBuilder b; b.append( "x" , (long long)2 ); ll = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (double)2 ); d = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (int)2 ); i = b.obj(); }
                { BSONObjBuilder b; b.appendNull( "x" ); n = b.obj(); }
                { BSONObjBuilder b; u = b.obj(); }

                verify( ll.woCompare( u ) == d.woCompare( u ) );
                verify( ll.woCompare( u ) == i.woCompare( u ) );
                BSONObj k = BSON( "x" << 1 );
                verify( ll.woCompare( u , k ) == d.woCompare( u , k ) );
                verify( ll.woCompare( u , k ) == i.woCompare( u , k ) );

                verify( u.woCompare( ll ) == u.woCompare( d ) );
                verify( u.woCompare( ll ) == u.woCompare( i ) );
                verify( u.woCompare( ll , k ) == u.woCompare( d , k ) );
                verify( u.woCompare( ll , k ) == u.woCompare( d , k ) );

                verify( i.woCompare( n ) == d.woCompare( n ) );

                verify( ll.woCompare( n ) == d.woCompare( n ) );
                verify( ll.woCompare( n ) == i.woCompare( n ) );
                verify( ll.woCompare( n , k ) == d.woCompare( n , k ) );
                verify( ll.woCompare( n , k ) == i.woCompare( n , k ) );

                verify( n.woCompare( ll ) == n.woCompare( d ) );
                verify( n.woCompare( ll ) == n.woCompare( i ) );
                verify( n.woCompare( ll , k ) == n.woCompare( d , k ) );
                verify( n.woCompare( ll , k ) == n.woCompare( d , k ) );
            }

            {
                BSONObj l,r;
                { BSONObjBuilder b; b.append( "x" , "eliot" ); l = b.obj(); }
                { BSONObjBuilder b; b.appendSymbol( "x" , "eliot" ); r = b.obj(); }
                verify( l.woCompare( r ) == 0 );
                verify( r.woCompare( l ) == 0 );
            }
        }

        void run() {
            testRegex();
            BSONObjBuilder A,B,C;
            A.append("x", 2);
            B.append("x", 2.0);
            C.append("x", 2.1);
            BSONObj a = A.done();
            BSONObj b = B.done();
            BSONObj c = C.done();
            verify( !a.binaryEqual( b ) ); // comments on operator==
            int cmp = a.woCompare(b);
            verify( cmp == 0 );
            cmp = a.woCompare(c);
            verify( cmp < 0 );
            testoid();
            testbounds();
            testorder();
        }
    } bson_unittest;

} // namespace mongo
