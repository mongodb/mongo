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
        
        void roundTrip( string s ){
            ASSERT_EQUALS( s , base64::decode( base64::encode( s ) ) );
        }
        
        void roundTrip( const unsigned char * _data , int len ){
            const char *data = (const char *) _data;
            string s = base64::encode( data , len );
            string out = base64::decode( s );
            ASSERT_EQUALS( out.size() , static_cast<size_t>(len) );
            bool broke = false;
            for ( int i=0; i<len; i++ ){
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
        
        void run(){

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
            Base(){}
            virtual ~Base(){}

            void run(){
                pop();
                ASSERT_EQUALS( ss.str() , sb.str() );
            }

            stringstream ss;
            StringBuilder sb;
        };
        
        class simple1 : public Base {
            void pop(){
                SBTGB(1);
                SBTGB("yo");
                SBTGB(2);
            }
        };

        class simple2 : public Base {
            void pop(){
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
            void run(){
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
            void run(){
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

        void run(){
            Timer t;
            sleepsecs( 1 );
            ASSERT_EQUALS( 1 , t.seconds() );

            t.reset();
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
                for ( int i=0; i<(total/ms); i++ ){
                    sleepmillis( ms );
                }
                {
                    int x = t.millis();
                    if ( x < 1000 || x > 2500 ){
                        cout << "sleeptest x: " << x << endl;
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
                for ( int i=0; i<numSleeps; i++ ){
                    sleepmicros( micros );
                }
                {
                    int y = t.millis();
                    if ( y < 1000 || y > 2500 ){
                        cout << "sleeptest y: " << y << endl;
                        ASSERT( y >= 1000 );
                        /* ASSERT( y <= 100000 ); */
                    }
                }
            }
#endif
            
        }
        
    };

    class AssertTests {
    public:

        int x;

        AssertTests(){
            x = 0;
        }

        string foo(){
            x++;
            return "";
        }
        void run(){
            uassert( -1 , foo() , 1 );
            if( x != 0 ) {
                ASSERT_EQUALS( 0 , x );
            }
            try {
                uassert( -1 , foo() , 0 );
            }
            catch ( ... ){}
            ASSERT_EQUALS( 1 , x );
        }
    };

    namespace ArrayTests {
        class basic1 {
        public:
            void run(){
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
        void run(){
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
        void run() {
            
            ASSERT( ! isNumber( (char)255 ) );

            ASSERT_EQUALS( 0, lexNumCmp( "a", "a" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "a", "aa" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "aa", "a" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "a", "b" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "100", "50" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "50", "100" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "b", "a" ) );
            ASSERT_EQUALS( 0, lexNumCmp( "aa", "aa" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "aa", "ab" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "ab", "aa" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "0", "a" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "a0", "aa" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "a", "0" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "aa", "a0" ) );
            ASSERT_EQUALS( 0, lexNumCmp( "0", "0" ) );
            ASSERT_EQUALS( 0, lexNumCmp( "10", "10" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "1", "10" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "10", "1" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "11", "10" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "10", "11" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "f11f", "f10f" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "f10f", "f11f" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "f11f", "f111" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "f111", "f11f" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "f12f", "f12g" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "f12g", "f12f" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "aa{", "aab" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "aa{", "aa1" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "a1{", "a11" ) );
            ASSERT_EQUALS( 1, lexNumCmp( "a1{a", "a1{" ) );
            ASSERT_EQUALS( -1, lexNumCmp( "a1{", "a1{a" ) );
            ASSERT_EQUALS( 1, lexNumCmp("21", "11") );
            ASSERT_EQUALS( -1, lexNumCmp("11", "21") );
            
            ASSERT_EQUALS( -1 , lexNumCmp( "a.0" , "a.1" ) );
            ASSERT_EQUALS( -1 , lexNumCmp( "a.0.b" , "a.1" ) );

            ASSERT_EQUALS( -1 , lexNumCmp( "b." , "b.|" ) );
            ASSERT_EQUALS( -1 , lexNumCmp( "b.0e" , (string("b.") + (char)255).c_str() ) );
            ASSERT_EQUALS( -1 , lexNumCmp( "b." , "b.0e" ) );

            ASSERT_EQUALS( 0, lexNumCmp( "238947219478347782934718234", "238947219478347782934718234")); 
            ASSERT_EQUALS( 0, lexNumCmp( "000238947219478347782934718234", "238947219478347782934718234")); 
            ASSERT_EQUALS( 1, lexNumCmp( "000238947219478347782934718235", "238947219478347782934718234")); 
            ASSERT_EQUALS( -1, lexNumCmp( "238947219478347782934718234", "238947219478347782934718234.1")); 
            ASSERT_EQUALS( 0, lexNumCmp( "238", "000238")); 
            ASSERT_EQUALS( 0, lexNumCmp( "002384", "0002384")); 
            ASSERT_EQUALS( 0, lexNumCmp( "00002384", "0002384")); 
            ASSERT_EQUALS( 0, lexNumCmp( "0", "0")); 
            ASSERT_EQUALS( 0, lexNumCmp( "0000", "0")); 
            ASSERT_EQUALS( 0, lexNumCmp( "0", "000"));
            ASSERT_EQUALS( -1, lexNumCmp( "0000", "0.0"));
            ASSERT_EQUALS( 1, lexNumCmp( "2380", "238")); 
            ASSERT_EQUALS( 1, lexNumCmp( "2385", "2384")); 
            ASSERT_EQUALS( 1, lexNumCmp( "2385", "02384")); 
            ASSERT_EQUALS( 1, lexNumCmp( "2385", "002384")); 
            ASSERT_EQUALS( -1, lexNumCmp( "123.234.4567", "00238")); 
            ASSERT_EQUALS( 0, lexNumCmp( "123.234", "00123.234")); 
            ASSERT_EQUALS( 0, lexNumCmp( "a.123.b", "a.00123.b")); 
            ASSERT_EQUALS( 1, lexNumCmp( "a.123.b", "a.b.00123.b")); 
            ASSERT_EQUALS( -1, lexNumCmp( "a.00.0", "a.0.1")); 
            ASSERT_EQUALS( 0, lexNumCmp( "01.003.02", "1.3.2")); 
            ASSERT_EQUALS( -1, lexNumCmp( "1.3.2", "10.300.20")); 
            ASSERT_EQUALS( 0, lexNumCmp( "10.300.20", "000000000000010.0000300.000000020")); 
            ASSERT_EQUALS( 0, lexNumCmp( "0000a", "0a")); 
            ASSERT_EQUALS( -1, lexNumCmp( "a", "0a")); 
            ASSERT_EQUALS( -1, lexNumCmp( "000a", "001a")); 
            ASSERT_EQUALS( 0, lexNumCmp( "010a", "0010a")); 
        }
    };

    class DatabaseValidNames {
    public:
        void run(){
            ASSERT( Database::validDBName( "foo" ) );
            ASSERT( ! Database::validDBName( "foo/bar" ) );
            ASSERT( ! Database::validDBName( "foo.bar" ) );

            ASSERT( nsDollarCheck( "asdads" ) );
            ASSERT( ! nsDollarCheck( "asda$ds" ) );
            ASSERT( nsDollarCheck( "local.oplog.$main" ) );
        }
    };
    
    class PtrTests {
    public:
        void run(){
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

        void test( string s ){
            vector<string> v = StringSplitter::split( s , "," );
            ASSERT_EQUALS( s , StringSplitter::join( v , "," ) );
        }

        void run(){
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



    class All : public Suite {
    public:
        All() : Suite( "basic" ){
        }
        
        void setupTests(){
            add< Rarely >();
            add< Base64Tests >();
            
            add< stringbuildertests::simple1 >();
            add< stringbuildertests::simple2 >();
            add< stringbuildertests::reset1 >();
            add< stringbuildertests::reset2 >();

            add< sleeptest >();
            add< AssertTests >();
            
            add< ArrayTests::basic1 >();
            add< LexNumCmp >();

            add< DatabaseValidNames >();

            add< PtrTests >();

            add< StringSplitterTest >();
            add< IsValidUTF8Test >();
        }
    } myall;
    
} // namespace BasicTests

