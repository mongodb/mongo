// text.h
/*
 *    Copyright 2010 10gen Inc.
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
        /** @param big the string to be split
            @param splitter the delimiter
        */
        StringSplitter( const char * big , const char * splitter )
            : _big( big ) , _splitter( splitter ) {
        }

        /** @return true if more to be taken via next() */
        bool more() {
            return _big[0] != 0;
        }

        /** get next split string fragment */
        string next() {
            const char * foo = strstr( _big , _splitter );
            if ( foo ) {
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

        void split( vector<string>& l ) {
            while ( more() ) {
                l.push_back( next() );
            }
        }

        vector<string> split() {
            vector<string> l;
            split( l );
            return l;
        }

        static vector<string> split( const string& big , const string& splitter ) {
            StringSplitter ss( big.c_str() , splitter.c_str() );
            return ss.split();
        }

        static string join( vector<string>& l , const string& split ) {
            stringstream ss;
            for ( unsigned i=0; i<l.size(); i++ ) {
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

#if defined(_WIN32)

    std::string toUtf8String(const std::wstring& wide);

    std::wstring toWideString(const char *s);

    /* like toWideString but UNICODE macro sensitive */
# if !defined(_UNICODE)
#error temp error 
    inline std::string toNativeString(const char *s) { return s; }
# else
    inline std::wstring toNativeString(const char *s) { return toWideString(s); }
# endif

#endif

    // expect that n contains a base ten number and nothing else after it
    // NOTE win version hasn't been tested directly
    inline long long parseLL( const char *n ) {
        long long ret;
        uassert( 13307, "cannot convert empty string to long long", *n != 0 );
#if !defined(_WIN32)
        char *endPtr = 0;
        errno = 0;
        ret = strtoll( n, &endPtr, 10 );
        uassert( 13305, "could not convert string to long long", *endPtr == 0 && errno == 0 );
#elif _MSC_VER>=1600    // 1600 is VS2k10 1500 is VS2k8
        size_t endLen = 0;
        try {
            ret = stoll( n, &endLen, 10 );
        }
        catch ( ... ) {
            endLen = 0;
        }
        uassert( 13306, "could not convert string to long long", endLen != 0 && n[ endLen ] == 0 );
#else // stoll() wasn't introduced until VS 2010.
        char* endPtr = 0;
        ret = _strtoi64( n, &endPtr, 10 );
        uassert( 13310, "could not convert string to long long", (*endPtr == 0) && (ret != _I64_MAX) && (ret != _I64_MIN) );
#endif // !defined(_WIN32)
        return ret;
    }

#if defined(_WIN32)

    class WindowsCommandLine {
        char**  _argv;

    public:
        WindowsCommandLine( int argc, wchar_t* argvW[] );
        ~WindowsCommandLine();
        char** argv( void ) const { return _argv; };
    };

#endif // #if defined(_WIN32)

} // namespace mongo
