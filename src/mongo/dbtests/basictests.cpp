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

#include "pch.h"

#include "dbtests.h"
#include "../util/base64.h"
#include "../util/array.h"
#include "../util/text.h"
#include "../util/queue.h"
#include "../util/paths.h"
#include "../util/stringutils.h"
#include "../util/compress.h"
#include "../bson/util/bswap.h"
#include "../db/db.h"

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
                    log() << "temp millis: " << t.millis() << endl;
                ASSERT( sec >= 0 && sec <= 2 );
                t.reset();
            }
            if ( matches < 2 )
                log() << "matches:" << matches << endl;
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

 class bswaptest {
    public:

        void run() {
           {
               unsigned long long a = 0x123456789abcdef0ULL;
               ASSERT_EQUALS( 0xf0debc9a78563412ULL, byteSwap<unsigned long long>( a ) );
           }
           {
               const char* a = "0123456789abcdefghijkl";
               ASSERT_EQUALS( 0x3031323334353637ULL, readBE<unsigned long long>( a ) );
               ASSERT_EQUALS( 0x3736353433323130ULL, readLE<unsigned long long>( a ) );
               ASSERT_EQUALS( 0x3132333435363738ULL, readBE<unsigned long long>( a + 1 ) );
               ASSERT_EQUALS( 0x3837363534333231ULL, readLE<unsigned long long>( a + 1 ) );
               ASSERT_EQUALS( 0x30313233U, readBE<unsigned int>( a ) );
               ASSERT_EQUALS( 0x34333231U, readLE<unsigned int>( a + 1 ) );
               ASSERT_EQUALS( 0x34333231U, little<unsigned int>::ref( a + 1 ) );
           }
           {
               unsigned char a [] = { 0, 0, 0, 0, 0, 0, 0xf0, 0x3f };
               ASSERT_EQUALS( 1.0, readLE<double>( a ) );
               ASSERT_EQUALS( 1.0, little<double>::ref( a ) );
               char b[8];
               copyLE<double>( b, 1.0 );
               ASSERT_EQUALS( 0, memcmp( a, b, 8 ) );
               memset( b, 0xff, 8 );
               little<double>::ref( b ) = 1.0;
               ASSERT_EQUALS( 0, memcmp( a, b, 8 ) );
           }
           {
               unsigned char a [] = { 0x3f, 0xf0, 0, 0, 0, 0, 0, 0 };
               ASSERT_EQUALS( 1.0, readBE<double>( a ) );
               ASSERT_EQUALS( 1.0, big<double>::ref( a ) );
               char b[8];
               copyBE<double>( b, 1.0 );
               ASSERT_EQUALS( 0, memcmp( a, b, 8 ) );
               memset( b, 0xff, 8 );
               big<double>::ref( b ) = 1.0;
               ASSERT_EQUALS( 0, memcmp( a, b, 8 ) );
           }
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

    class LexNumCmp {
    public:
        static void assertCmp( int expected, const char *s1, const char *s2,
                              bool lexOnly = false ) {
            mongo::LexNumCmp cmp( lexOnly );
            ASSERT_EQUALS( expected, cmp.cmp( s1, s2, lexOnly ) );
            ASSERT_EQUALS( expected, cmp.cmp( s1, s2 ) );
            ASSERT_EQUALS( expected < 0, cmp( s1, s2 ) );
            ASSERT_EQUALS( expected < 0, cmp( string( s1 ), string( s2 ) ) );
        }
        void run() {

            ASSERT( ! isNumber( (char)255 ) );

            assertCmp( 0, "a", "a" );
            assertCmp( -1, "a", "aa" );
            assertCmp( 1, "aa", "a" );
            assertCmp( -1, "a", "b" );
            assertCmp( 1, "100", "50" );
            assertCmp( -1, "50", "100" );
            assertCmp( 1, "b", "a" );
            assertCmp( 0, "aa", "aa" );
            assertCmp( -1, "aa", "ab" );
            assertCmp( 1, "ab", "aa" );
            assertCmp( 1, "0", "a" );
            assertCmp( 1, "a0", "aa" );
            assertCmp( -1, "a", "0" );
            assertCmp( -1, "aa", "a0" );
            assertCmp( 0, "0", "0" );
            assertCmp( 0, "10", "10" );
            assertCmp( -1, "1", "10" );
            assertCmp( 1, "10", "1" );
            assertCmp( 1, "11", "10" );
            assertCmp( -1, "10", "11" );
            assertCmp( 1, "f11f", "f10f" );
            assertCmp( -1, "f10f", "f11f" );
            assertCmp( -1, "f11f", "f111" );
            assertCmp( 1, "f111", "f11f" );
            assertCmp( -1, "f12f", "f12g" );
            assertCmp( 1, "f12g", "f12f" );
            assertCmp( 1, "aa{", "aab" );
            assertCmp( -1, "aa{", "aa1" );
            assertCmp( -1, "a1{", "a11" );
            assertCmp( 1, "a1{a", "a1{" );
            assertCmp( -1, "a1{", "a1{a" );
            assertCmp( 1, "21", "11" );
            assertCmp( -1, "11", "21" );

            assertCmp( -1 , "a.0" , "a.1" );
            assertCmp( -1 , "a.0.b" , "a.1" );

            assertCmp( -1 , "b." , "b.|" );
            assertCmp( -1 , "b.0e" , (string("b.") + (char)255).c_str() );
            assertCmp( -1 , "b." , "b.0e" );

            assertCmp( 0, "238947219478347782934718234", "238947219478347782934718234");
            assertCmp( 0, "000238947219478347782934718234", "238947219478347782934718234");
            assertCmp( 1, "000238947219478347782934718235", "238947219478347782934718234");
            assertCmp( -1, "238947219478347782934718234", "238947219478347782934718234.1");
            assertCmp( 0, "238", "000238");
            assertCmp( 0, "002384", "0002384");
            assertCmp( 0, "00002384", "0002384");
            assertCmp( 0, "0", "0");
            assertCmp( 0, "0000", "0");
            assertCmp( 0, "0", "000");
            assertCmp( -1, "0000", "0.0");
            assertCmp( 1, "2380", "238");
            assertCmp( 1, "2385", "2384");
            assertCmp( 1, "2385", "02384");
            assertCmp( 1, "2385", "002384");
            assertCmp( -1, "123.234.4567", "00238");
            assertCmp( 0, "123.234", "00123.234");
            assertCmp( 0, "a.123.b", "a.00123.b");
            assertCmp( 1, "a.123.b", "a.b.00123.b");
            assertCmp( -1, "a.00.0", "a.0.1");
            assertCmp( 0, "01.003.02", "1.3.2");
            assertCmp( -1, "1.3.2", "10.300.20");
            assertCmp( 0, "10.300.20", "000000000000010.0000300.000000020");
            assertCmp( 0, "0000a", "0a");
            assertCmp( -1, "a", "0a");
            assertCmp( -1, "000a", "001a");
            assertCmp( 0, "010a", "0010a");
            
            assertCmp( -1 , "a0" , "a00" );
            assertCmp( 0 , "a.0" , "a.00" );
            assertCmp( -1 , "a.b.c.d0" , "a.b.c.d00" );
            assertCmp( 1 , "a.b.c.0.y" , "a.b.c.00.x" );
            
            assertCmp( -1, "a", "a-" );
            assertCmp( 1, "a-", "a" );
            assertCmp( 0, "a-", "a-" );

            assertCmp( -1, "a", "a-c" );
            assertCmp( 1, "a-c", "a" );
            assertCmp( 0, "a-c", "a-c" );

            assertCmp( 1, "a-c.t", "a.t" );
            assertCmp( -1, "a.t", "a-c.t" );
            assertCmp( 0, "a-c.t", "a-c.t" );

            assertCmp( 1, "ac.t", "a.t" );
            assertCmp( -1, "a.t", "ac.t" );
            assertCmp( 0, "ac.t", "ac.t" );            
        }
    };
    
    class LexNumCmpLexOnly : public LexNumCmp {
    public:
        void run() {
            assertCmp( -1, "0", "00", true );
            assertCmp( 1, "1", "01", true );
            assertCmp( -1, "1", "11", true );
            assertCmp( 1, "2", "11", true );
        }
    };

    class DatabaseValidNames {
    public:
        void run() {
            ASSERT( NamespaceString::validDBName( "foo" ) );
            ASSERT( ! NamespaceString::validDBName( "foo/bar" ) );
            ASSERT( ! NamespaceString::validDBName( "foo bar" ) );
            ASSERT( ! NamespaceString::validDBName( "foo.bar" ) );

            ASSERT( NamespaceString::normal( "asdads" ) );
            ASSERT( ! NamespaceString::normal( "asda$ds" ) );
            ASSERT( NamespaceString::normal( "local.oplog.$main" ) );
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

    class CmdLineParseConfigTest {
    public:
        void run() {
            stringstream ss1;
            istringstream iss1("");
            CmdLine::parseConfigFile( iss1, ss1 );
            stringstream ss2;
            istringstream iss2("password=\'foo bar baz\'");
            CmdLine::parseConfigFile( iss2, ss2 );
            stringstream ss3;
            istringstream iss3("\t    this = false  \n#that = true\n  #another = whocares\n\n  other = monkeys  ");
            CmdLine::parseConfigFile( iss3, ss3 );

            ASSERT( ss1.str().compare("\n") == 0 );
            ASSERT( ss2.str().compare("password=\'foo bar baz\'\n\n") == 0 );
            ASSERT( ss3.str().compare("\n  other = monkeys  \n\n") == 0 );
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

    /** Simple tests for log tees. */
    class LogTee {
    public:
        ~LogTee() {
            // Clean global tees on test failure.
            Logstream::get().removeGlobalTee( &_tee );            
        }
        void run() {
            // Attempting to remove a tee before any tees are added is safe.
            Logstream::get().removeGlobalTee( &_tee );

            // A log is not written to a non global tee.
            log() << "LogTee test" << endl;
            assertNumLogs( 0 );

            // A log is written to a global tee.
            Logstream::get().addGlobalTee( &_tee );
            log() << "LogTee test" << endl;
            assertNumLogs( 1 );

            // A log is not written to a tee removed from the global tee list.
            Logstream::get().removeGlobalTee( &_tee );
            log() << "LogTee test" << endl;
            assertNumLogs( 1 );            
        }
    private:
        void assertNumLogs( int expected ) const {
            ASSERT_EQUALS( expected, _tee.numLogs() );
        }
        class Tee : public mongo::Tee {
        public:
            Tee() :
                _numLogs() {
            }
            virtual void write( LogLevel level, const string &str ) {
                ++_numLogs;
            }
            int numLogs() const { return _numLogs; }
        private:
            int _numLogs;
        };
        Tee _tee;
    };


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
            add< bswaptest >();
            add< AssertTests >();

            add< ArrayTests::basic1 >();
            add< LexNumCmp >();
            add< LexNumCmpLexOnly >();

            add< DatabaseValidNames >();
            add< DatabaseOwnsNS >();

            add< NSValidNames >();

            add< PtrTests >();

            add< StringSplitterTest >();
            add< IsValidUTF8Test >();

            add< QueueTest >();

            add< StrTests >();

            add< HostAndPortTests >();
            add< RelativePathTest >();
            add< CmdLineParseConfigTest >();

            add< CompressionTest1 >();

            add< LogTee >();
        }
    } myall;

} // namespace BasicTests

