// util/base64.cpp


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

#include "stdafx.h"

namespace mongo {
    namespace base64 {
        
        class Alphabet {
        public:
            Alphabet(){
                encode = 
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                    "abcdefghijklmnopqrstuvwxyz"
                    "0123456789"
                    "+/";
                
                decode = (char*)malloc(257);
                memset( decode , 0 , 256 );
                for ( int i=0; i<64; i++ ){
                    decode[ encode[i] ] = i;
                }

                test();
            }
            ~Alphabet(){
                delete( decode );
            }

            void test(){
                assert( strlen( encode ) == 64 );
                for ( int i=0; i<26; i++ )
                    assert( encode[i] == toupper( encode[i+26] ) );
            }

            char e( int x ){
                return encode[x&0x3f];
            }
            
        private:
            const char * encode;
        public:
            char * decode;
        } alphabet;
        
        
        void encode( stringstream& ss , const char * data , int size ){
            for ( int i=0; i<size; i+=3 ){
                int left = size - i;
                const char * start = data + i;
                
                // byte 0
                ss << alphabet.e(start[0]>>2);
                
                // byte 1
                char temp = ( start[0] << 4 );
                if ( left == 1 ){
                    ss << alphabet.e(temp);
                    break;
                }
                temp |= ( ( start[1] >> 4 ) & 0xF );
                ss << alphabet.e(temp);

                // byte 2
                temp = ( start[1] & 0xF ) << 2;
                if ( left == 2 ){
                    ss << alphabet.e(temp);
                    break;
                }
                temp |= ( ( start[2] >> 6 ) & 0x3 );
                ss << alphabet.e(temp);

                // byte 3
                ss << alphabet.e(start[2] & 0x3f);
            }

            int mod = size % 3;
            if ( mod == 1 ){
                ss << "==";
            }
            else if ( mod == 2 ){
                ss << "=";
            }
        }


        string encode( const char * data , int size ){
            stringstream ss;
            encode( ss , data ,size );
            return ss.str();
        }
        
        string encode( const string& s ){
            return encode( s.c_str() , s.size() );
        }


        void decode( stringstream& ss , const string& s ){
            uassert( "invalid base64" , s.size() % 4 == 0 );
            const char * data = s.c_str();
            int size = s.size();
            
            char buf[4];
            buf[3] = 0;
            for ( int i=0; i<size; i+=4){
                const char * start = data + i;
                buf[0] = ( ( alphabet.decode[start[0]] << 2 ) & 0xFC ) | ( ( alphabet.decode[start[1]] >> 4 ) & 0x3 );
                buf[1] = ( ( alphabet.decode[start[1]] << 4 ) & 0xF0 ) | ( ( alphabet.decode[start[2]] >> 2 ) & 0xF );
                buf[2] = ( ( alphabet.decode[start[2]] << 6 ) & 0xC0 ) | ( ( alphabet.decode[start[3]] & 0x3F ) );
                ss << buf;
            }
        }
        
        string decode( const string& s ){
            stringstream ss;
            decode( ss , s );
            return ss.str();
        }

    }
}

