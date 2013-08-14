// basictests.cpp : basic unit tests
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

#include "mongo/pch.h"

#include "mongo/db/db.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/array.h"
#include "mongo/util/base64.h"
#include "mongo/util/compress.h"
#include "mongo/util/paths.h"
#include "mongo/util/queue.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"

namespace BasicTests {

    class Rarely {
    public:
        void run() {
            int first = 0;
            int second = 0;
            int third = 0;
            for( int i = 0; i < 128; ++i ) {
                incRarely( first );
                incRarely2( second );
                ONCE ++third;
            }
            ASSERT_EQUALS( 1, first );
            ASSERT_EQUALS( 1, second );
            ASSERT_EQUALS( 1, third );
        }
    private:
        void incRarely( int &c ) {
            RARELY ++c;
        }
        void incRarely2( int &c ) {
            RARELY ++c;
        }
    };

    class Base64Tests {
    public:

        void roundTrip( string s ) {
            ASSERT_EQUALS( s , base64::decode( base64::encode( s ) ) );
        }

        void roundTrip( const unsigned char * _data , int len ) {
            const char *data = (const char *) _data;
            string s = base64::encode( data , len );
            string out = base64::decode( s );
            ASSERT_EQUALS( out.size() , static_cast<size_t>(len) );
            bool broke = false;
            for ( int i=0; i<len; i++ ) {
                if ( data[i] != out[i] )
                    broke = true;
            }
            if ( ! broke )
                return;

            cout << s << endl;
            for ( int i=0; i<len; i++ )
                cout << hex << ( data[i] & 0xFF ) << dec << " ";
            cout << endl;
            for ( int i=0; i<len; i++ )
                cout << hex << ( out[i] & 0xFF ) << dec << " ";
            cout << endl;

            ASSERT(0);
        }

        void run() {

            ASSERT_EQUALS( "ZWxp" , base64::encode( "eli" , 3 ) );
            ASSERT_EQUALS( "ZWxpb3Rz" , base64::encode( "eliots" , 6 ) );
            ASSERT_EQUALS( "ZWxpb3Rz" , base64::encode( "eliots" ) );

            ASSERT_EQUALS( "ZQ==" , base64::encode( "e" , 1 ) );
            ASSERT_EQUALS( "ZWw=" , base64::encode( "el" , 2 ) );

            roundTrip( "e" );
            roundTrip( "el" );
            roundTrip( "eli" );
            roundTrip( "elio" );
            roundTrip( "eliot" );
            roundTrip( "eliots" );
            roundTrip( "eliotsz" );

            unsigned char z[] = { 0x1 , 0x2 , 0x3 , 0x4 };
            roundTrip( z , 4 );

            unsigned char y[] = {
                0x01, 0x10, 0x83, 0x10, 0x51, 0x87, 0x20, 0x92, 0x8B, 0x30,
                0xD3, 0x8F, 0x41, 0x14, 0x93, 0x51, 0x55, 0x97, 0x61, 0x96,
                0x9B, 0x71, 0xD7, 0x9F, 0x82, 0x18, 0xA3, 0x92, 0x59, 0xA7,
                0xA2, 0x9A, 0xAB, 0xB2, 0xDB, 0xAF, 0xC3, 0x1C, 0xB3, 0xD3,
                0x5D, 0xB7, 0xE3, 0x9E, 0xBB, 0xF3, 0xDF, 0xBF
            };
            roundTrip( y , 4 );
            roundTrip( y , 40 );
        }
    };

    namespace stringbuildertests {
#define SBTGB(x) ss << (x); sb << (x);

        class Base {
            virtual void pop() = 0;

        public:
            Base() {}
            virtual ~Base() {}

            void run() {
                pop();
                ASSERT_EQUALS( ss.str() , sb.str() );
            }

            stringstream ss;
            StringBuilder sb;
        };

        class simple1 : public Base {
            void pop() {
                SBTGB(1);
                SBTGB("yo");
                SBTGB(2);
            }
        };

        class simple2 : public Base {
            void pop() {
                SBTGB(1);
                SBTGB("yo");
                SBTGB(2);
                SBTGB( 12123123123LL );
                SBTGB( "xxx" );
                SBTGB( 5.4 );
                SBTGB( 5.4312 );
                SBTGB( "yyy" );
                SBTGB( (short)5 );
                SBTGB( (short)(1231231231231LL) );
            }
        };

        class reset1 {
        public:
            void run() {
                StringBuilder sb;
                sb << "1" << "abc" << "5.17";
                ASSERT_EQUALS( "1abc5.17" , sb.str() );
                ASSERT_EQUALS( "1abc5.17" , sb.str() );
                sb.reset();
                ASSERT_EQUALS( "" , sb.str() );
                sb << "999";
                ASSERT_EQUALS( "999" , sb.str() );
            }
        };

        class reset2 {
        public:
            void run() {
                StringBuilder sb;
                sb << "1" << "abc" << "5.17";
                ASSERT_EQUALS( "1abc5.17" , sb.str() );
                ASSERT_EQUALS( "1abc5.17" , sb.str() );
                sb.reset(1);
                ASSERT_EQUALS( "" , sb.str() );
                sb << "999";
                ASSERT_EQUALS( "999" , sb.str() );
            }
        };

    }

    class sleeptest {
    public:

        void run() {
            Timer t;
            int matches = 0;
            for( int p = 0; p < 3; p++ ) {
                sleepsecs( 1 );
                int sec = (t.millis() + 2)/1000;
                if( sec == 1 ) 
                    matches++;
                else
                    mongo::unittest::log() << "temp millis: " << t.millis() << endl;
                ASSERT( sec >= 0 && sec <= 2 );
                t.reset();
            }
            if ( matches < 2 )
                mongo::unittest::log() << "matches:" << matches << endl;
            ASSERT( matches >= 2 );

            sleepmicros( 1527123 );
            ASSERT( t.micros() > 1000000 );
            ASSERT( t.micros() < 2000000 );

            t.reset();
            sleepmillis( 1727 );
            ASSERT( t.millis() >= 1000 );
            ASSERT( t.millis() <= 2500 );

            {
                int total = 1200;
                int ms = 2;
                t.reset();
                for ( int i=0; i<(total/ms); i++ ) {
                    sleepmillis( ms );
                }
                {
                    int x = t.millis();
                    if ( x < 1000 || x > 2500 ) {
                        cout << "sleeptest finds sleep accuracy to be not great. x: " << x << endl;
                        ASSERT( x >= 1000 );
                        ASSERT( x <= 20000 );
                    }
                }
            }

#ifdef __linux__
            {
                int total = 1200;
                int micros = 100;
                t.reset();
                int numSleeps = 1000*(total/micros);
                for ( int i=0; i<numSleeps; i++ ) {
                    sleepmicros( micros );
                }
                {
                    int y = t.millis();
                    if ( y < 1000 || y > 2500 ) {
                        cout << "sleeptest y: " << y << endl;
                        ASSERT( y >= 1000 );
                        /* ASSERT( y <= 100000 ); */
                    }
                }
            }
#endif

        }

    };

    class SleepBackoffTest {
    public:
        void run() {

            int maxSleepTimeMillis = 1000;
            int lastSleepTimeMillis = -1;
            int epsMillis = 100; // Allowable inprecision for timing

            Backoff backoff( maxSleepTimeMillis, maxSleepTimeMillis * 2 );

            Timer t;

            // Make sure our backoff increases to the maximum value
            int maxSleepCount = 0;
            while( maxSleepCount < 3 ){

                t.reset();

                backoff.nextSleepMillis();

                int elapsedMillis = t.millis();

                mongo::unittest::log() << "Slept for " << elapsedMillis << endl;

                ASSERT( almostGTE( elapsedMillis, lastSleepTimeMillis, epsMillis ) );
                lastSleepTimeMillis = elapsedMillis;

                if( almostEq( elapsedMillis, maxSleepTimeMillis, epsMillis ) ) maxSleepCount++;
            }

            // Make sure that our backoff gets reset if we wait much longer than the maximum wait
            sleepmillis( maxSleepTimeMillis * 4 );

            t.reset();
            backoff.nextSleepMillis();

            ASSERT( almostEq( t.millis(), 0, epsMillis ) );

        }

        bool almostEq( int a, int b, int eps ){
            return std::abs( a - b ) <= eps;
        }

        bool almostGTE( int a, int b, int eps ){
            if( almostEq( a, b, eps ) ) return true;
            return a > b;
        }
    };

    class AssertTests {
    public:

        int x;

        AssertTests() {
            x = 0;
        }

        string foo() {
            x++;
            return "";
        }
        void run() {
            uassert( -1 , foo() , 1 );
            if( x != 0 ) {
                ASSERT_EQUALS( 0 , x );
            }
            try {
                uassert( -1 , foo() , 0 );
            }
            catch ( ... ) {}
            ASSERT_EQUALS( 1 , x );
        }
    };

    namespace ArrayTests {
        class basic1 {
        public:
            void run() {
                FastArray<int> a(100);
                a.push_back( 5 );
                a.push_back( 6 );

                ASSERT_EQUALS( 2 , a.size() );

                FastArray<int>::iterator i = a.begin();
                ASSERT( i != a.end() );
                ASSERT_EQUALS( 5 , *i );
                ++i;
                ASSERT( i != a.end() );
                ASSERT_EQUALS( 6 , *i );
                ++i;
                ASSERT( i == a.end() );
            }
        };
    };

    class ThreadSafeStringTest {
    public:
        void run() {
            ThreadSafeString s;
            s = "eliot";
            ASSERT_EQUALS( s , "eliot" );
            ASSERT( s != "eliot2" );

            ThreadSafeString s2 = s;
            ASSERT_EQUALS( s2 , "eliot" );


            {
                string foo;
                {
                    ThreadSafeString bar;
                    bar = "eliot2";
                    foo = bar.toString();
                }
                ASSERT_EQUALS( "eliot2" , foo );
            }
        }
    };


    class DatabaseOwnsNS {
    public:
        void run() {
            Lock::GlobalWrite lk;
            bool isNew = false;
            // this leaks as ~Database is private
            // if that changes, should put this on the stack
            {
                Database * db = new Database( "dbtests_basictests_ownsns" , isNew );
                verify( isNew );

                ASSERT( db->ownsNS( "dbtests_basictests_ownsns.x" ) );
                ASSERT( db->ownsNS( "dbtests_basictests_ownsns.x.y" ) );
                ASSERT( ! db->ownsNS( "dbtests_basictests_ownsn.x.y" ) );
                ASSERT( ! db->ownsNS( "dbtests_basictests_ownsnsa.x.y" ) );
            }
        }
    };

    class NSValidNames {
    public:
        void run() {
            ASSERT( isValidNS( "test.foo" ) );
            ASSERT( ! isValidNS( "test." ) );
            ASSERT( ! isValidNS( "test" ) );
        }
    };

    class PtrTests {
    public:
        void run() {
            scoped_ptr<int> p1 (new int(1));
            boost::shared_ptr<int> p2 (new int(2));
            scoped_ptr<const int> p3 (new int(3));
            boost::shared_ptr<const int> p4 (new int(4));

            //non-const
            ASSERT_EQUALS( p1.get() , ptr<int>(p1) );
            ASSERT_EQUALS( p2.get() , ptr<int>(p2) );
            ASSERT_EQUALS( p2.get() , ptr<int>(p2.get()) ); // T* constructor
            ASSERT_EQUALS( p2.get() , ptr<int>(ptr<int>(p2)) ); // copy constructor
            ASSERT_EQUALS( *p2      , *ptr<int>(p2));
            ASSERT_EQUALS( p2.get() , ptr<boost::shared_ptr<int> >(&p2)->get() ); // operator->

            //const
            ASSERT_EQUALS( p1.get() , ptr<const int>(p1) );
            ASSERT_EQUALS( p2.get() , ptr<const int>(p2) );
            ASSERT_EQUALS( p2.get() , ptr<const int>(p2.get()) );
            ASSERT_EQUALS( p3.get() , ptr<const int>(p3) );
            ASSERT_EQUALS( p4.get() , ptr<const int>(p4) );
            ASSERT_EQUALS( p4.get() , ptr<const int>(p4.get()) );
            ASSERT_EQUALS( p2.get() , ptr<const int>(ptr<const int>(p2)) );
            ASSERT_EQUALS( p2.get() , ptr<const int>(ptr<int>(p2)) ); // constizing copy constructor
            ASSERT_EQUALS( *p2      , *ptr<int>(p2));
            ASSERT_EQUALS( p2.get() , ptr<const boost::shared_ptr<int> >(&p2)->get() );

            //bool context
            ASSERT( ptr<int>(p1) );
            ASSERT( !ptr<int>(NULL) );
            ASSERT( !ptr<int>() );

#if 0
            // These shouldn't compile
            ASSERT_EQUALS( p3.get() , ptr<int>(p3) );
            ASSERT_EQUALS( p4.get() , ptr<int>(p4) );
            ASSERT_EQUALS( p2.get() , ptr<int>(ptr<const int>(p2)) );
#endif
        }
    };

    struct StringSplitterTest {

        void test( string s ) {
            vector<string> v = StringSplitter::split( s , "," );
            ASSERT_EQUALS( s , StringSplitter::join( v , "," ) );
        }

        void run() {
            test( "a" );
            test( "a,b" );
            test( "a,b,c" );

            vector<string> x = StringSplitter::split( "axbxc" , "x" );
            ASSERT_EQUALS( 3 , (int)x.size() );
            ASSERT_EQUALS( "a" , x[0] );
            ASSERT_EQUALS( "b" , x[1] );
            ASSERT_EQUALS( "c" , x[2] );

            x = StringSplitter::split( "axxbxxc" , "xx" );
            ASSERT_EQUALS( 3 , (int)x.size() );
            ASSERT_EQUALS( "a" , x[0] );
            ASSERT_EQUALS( "b" , x[1] );
            ASSERT_EQUALS( "c" , x[2] );

        }
    };

    struct IsValidUTF8Test {
// macros used to get valid line numbers
#define good(s)  ASSERT(isValidUTF8(s));
#define bad(s)   ASSERT(!isValidUTF8(s));

        void run() {
            good("A");
            good("\xC2\xA2"); // cent: ¢
            good("\xE2\x82\xAC"); // euro: €
            good("\xF0\x9D\x90\x80"); // Blackboard A: 𝐀

            //abrupt end
            bad("\xC2");
            bad("\xE2\x82");
            bad("\xF0\x9D\x90");
            bad("\xC2 ");
            bad("\xE2\x82 ");
            bad("\xF0\x9D\x90 ");

            //too long
            bad("\xF8\x80\x80\x80\x80");
            bad("\xFC\x80\x80\x80\x80\x80");
            bad("\xFE\x80\x80\x80\x80\x80\x80");
            bad("\xFF\x80\x80\x80\x80\x80\x80\x80");

            bad("\xF5\x80\x80\x80"); // U+140000 > U+10FFFF
            bad("\x80"); //cant start with continuation byte
            bad("\xC0\x80"); // 2-byte version of ASCII NUL
#undef good
#undef bad
        }
    };


    class QueueTest {
    public:
        void run() {
            BlockingQueue<int> q;
            Timer t;
            int x;
            ASSERT( ! q.blockingPop( x , 5 ) );
            ASSERT( t.seconds() > 3 && t.seconds() < 9 );

        }
    };

    class StrTests {
    public:

        void run() {
            ASSERT_EQUALS( 1u , str::count( "abc" , 'b' ) );
            ASSERT_EQUALS( 3u , str::count( "babab" , 'b' ) );
        }

    };

    class HostAndPortTests {
    public:
        void run() {
            HostAndPort a( "x1" , 1000 );
            HostAndPort b( "x1" , 1000 );
            HostAndPort c( "x1" , 1001 );
            HostAndPort d( "x2" , 1000 );

            ASSERT( a == b );
            ASSERT( a != c );
            ASSERT( a != d );

        }
    };

    class RelativePathTest {
    public:
        void run() {
            RelativePath a = RelativePath::fromRelativePath( "a" );
            RelativePath b = RelativePath::fromRelativePath( "a" );
            RelativePath c = RelativePath::fromRelativePath( "b" );
            RelativePath d = RelativePath::fromRelativePath( "a/b" );


            ASSERT( a == b );
            ASSERT( a != c );
            ASSERT( a != d );
            ASSERT( c != d );
        }
    };

    struct CompressionTest1 { 
        void run() { 
            const char * c = "this is a test";
            std::string s;
            size_t len = compress(c, strlen(c)+1, &s);
            verify( len > 0 );
            
            std::string out;
            bool ok = uncompress(s.c_str(), s.size(), &out);
            verify(ok);
            verify( strcmp(out.c_str(), c) == 0 );
        }
    } ctest1;

    class All : public Suite {
    public:
        All() : Suite( "basic" ) {
        }

        void setupTests() {
            add< Rarely >();
            add< Base64Tests >();

            add< stringbuildertests::simple1 >();
            add< stringbuildertests::simple2 >();
            add< stringbuildertests::reset1 >();
            add< stringbuildertests::reset2 >();

            add< sleeptest >();
            add< SleepBackoffTest >();
            add< AssertTests >();

            add< ArrayTests::basic1 >();

            add< DatabaseOwnsNS >();

            add< NSValidNames >();

            add< PtrTests >();

            add< StringSplitterTest >();
            add< IsValidUTF8Test >();

            add< QueueTest >();

            add< StrTests >();

            add< HostAndPortTests >();
            add< RelativePathTest >();

            add< CompressionTest1 >();

        }
    } myall;

} // namespace BasicTests

