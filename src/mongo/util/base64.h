// util/base64.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

namespace mongo {
    namespace base64 {

        class Alphabet {
        public:
            Alphabet()
                : encode((unsigned char*)
                         "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                         "abcdefghijklmnopqrstuvwxyz"
                         "0123456789"
                         "+/")
                , decode(new unsigned char[257]) {
                memset( decode.get() , 0 , 256 );
                for ( int i=0; i<64; i++ ) {
                    decode[ encode[i] ] = i;
                }

                test();
            }
            void test() {
                verify( strlen( (char*)encode ) == 64 );
                for ( int i=0; i<26; i++ )
                    verify( encode[i] == toupper( encode[i+26] ) );
            }

            char e( int x ) {
                return encode[x&0x3f];
            }

        private:
            const unsigned char * encode;
        public:
            boost::scoped_array<unsigned char> decode;
        };

        extern Alphabet alphabet;


        void encode( stringstream& ss , const char * data , int size );
        string encode( const char * data , int size );
        string encode( const string& s );

        void decode( stringstream& ss , const string& s );
        string decode( const string& s );

        extern const char* chars;

        void testAlphabet();
    }
}
