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

        const char * alphabet = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789"
            "+/";

        void testAlphabet(){
            assert( strlen( alphabet ) == 64 );
            for ( int i=0; i<26; i++ )
                assert( alphabet[i] == toupper( alphabet[i+26] ) );
        }
        
        
        void encode( stringstream& ss , const char * data , int size ){
            for ( int i=0; i<size; i+=3 ){
                int left = size - i;
                const char * start = data + i;
                
                // byte 0
                ss << alphabet[start[0]>>2];
                
                // byte 1
                char temp = ( start[0] & 0x3 ) << 4;
                if ( left == 1 ){
                    ss << alphabet[temp];
                    break;
                }
                temp |= start[1] >> 4;
                ss << alphabet[temp];

                // byte 2
                temp = ( start[1] & 0xF ) << 2;
                if ( left == 2 ){
                    ss << alphabet[temp];
                    break;
                }
                temp |= start[2] >> 6;
                ss << alphabet[temp];

                // byte 3
                ss << alphabet[start[2] & 0x3f];
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

    }
}

