// text.h

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
    
    class StringSplitter {
    public:
        StringSplitter( const char * big , const char * splitter )
            : _big( big ) , _splitter( splitter ){
        }

        bool more(){
            return _big[0];
        }

        string next(){
            const char * foo = strstr( _big , _splitter );
            if ( foo ){
                string s( _big , foo - _big );
                _big = foo + 1;
                while ( *_big && strstr( _big , _splitter ) == _big )
                    _big++;
                return s;
            }
            
            string s = _big;
            _big += strlen( _big );
            return s;
        }
        
        void split( vector<string>& l ){
            while ( more() ){
                l.push_back( next() );
            }
        }
        
        vector<string> split(){
            vector<string> l;
            split( l );
            return l;
        }

        static vector<string> split( const string& big , const string& splitter ){
            StringSplitter ss( big.c_str() , splitter.c_str() );
            return ss.split();
        }

        static string join( vector<string>& l , const string& split ){
            stringstream ss;
            for ( unsigned i=0; i<l.size(); i++ ){
                if ( i > 0 )
                    ss << split;
                ss << l[i];
            }
            return ss.str();
        }

    private:
        const char * _big;
        const char * _splitter;
    };
    
    /* This doesn't defend against ALL bad UTF8, but it will guarantee that the
     * string can be converted to sequence of codepoints. However, it doesn't
     * guarantee that the codepoints are valid.
     */
    bool isValidUTF8(const char *s);
    inline bool isValidUTF8(string s) { return isValidUTF8(s.c_str()); }
    
}
