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

#include "stdafx.h"

#include "dbtests.h"
#include "../util/base64.h"

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
            ASSERT( t.millis() <= 2000 );

        }
        
    };
    
    class All : public Suite {
    public:
        All() : Suite( "basic" ){
        }
        
        void setupTests(){
            add< Rarely >();
            add< Base64Tests >();

            add< sleeptest >();
        }
    } myall;
    
} // namespace BasicTests

