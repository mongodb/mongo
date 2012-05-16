// text.cpp

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

#include "pch.h"

#include "mongo/util/text.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/mongoutils/str.h"
#include <boost/smart_ptr/scoped_array.hpp>

using namespace std;

namespace mongo {

    // --- StringSplitter ----

    /** get next split string fragment */
    string StringSplitter::next() {
        const char * foo = strstr( _big , _splitter );
        if ( foo ) {
            string s( _big , foo - _big );
            _big = foo + strlen( _splitter );
            while ( *_big && strstr( _big , _splitter ) == _big )
                _big++;
            return s;
        }
        
        string s = _big;
        _big += strlen( _big );
        return s;
    }


    void StringSplitter::split( vector<string>& l ) {
        while ( more() ) {
            l.push_back( next() );
        }
    }

    vector<string> StringSplitter::split() {
        vector<string> l;
        split( l );
        return l;
    }

    string StringSplitter::join( vector<string>& l , const string& split ) {
        stringstream ss;
        for ( unsigned i=0; i<l.size(); i++ ) {
            if ( i > 0 )
                ss << split;
            ss << l[i];
        }
        return ss.str();
    }

    vector<string> StringSplitter::split( const string& big , const string& splitter ) {
        StringSplitter ss( big.c_str() , splitter.c_str() );
        return ss.split();
    }
    


    // --- utf8 utils ------

    inline int leadingOnes(unsigned char c) {
        if (c < 0x80) return 0;
        static const char _leadingOnes[128] = {
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 0x80 - 0x8F
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 0x90 - 0x99
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 0xA0 - 0xA9
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 0xB0 - 0xB9
            2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 0xC0 - 0xC9
            2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 0xD0 - 0xD9
            3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, // 0xE0 - 0xE9
            4, 4, 4, 4, 4, 4, 4, 4,                         // 0xF0 - 0xF7
            5, 5, 5, 5,             // 0xF8 - 0xFB
            6, 6,       // 0xFC - 0xFD
            7,    // 0xFE
            8, // 0xFF
        };
        return _leadingOnes[c & 0x7f];

    }

    bool isValidUTF8(string s) { 
        return isValidUTF8(s.c_str()); 
    }

    bool isValidUTF8(const char *s) {
        int left = 0; // how many bytes are left in the current codepoint
        while (*s) {
            const unsigned char c = (unsigned char) *(s++);
            const int ones = leadingOnes(c);
            if (left) {
                if (ones != 1) return false; // should be a continuation byte
                left--;
            }
            else {
                if (ones == 0) continue; // ASCII byte
                if (ones == 1) return false; // unexpected continuation byte
                if (c > 0xF4) return false; // codepoint too large (< 0x10FFFF)
                if (c == 0xC0 || c == 0xC1) return false; // codepoints <= 0x7F shouldn't be 2 bytes

                // still valid
                left = ones-1;
            }
        }
        if (left!=0) return false; // string ended mid-codepoint
        return true;
    }

    long long parseLL( const char *n ) {
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

    std::string toUtf8String(const std::wstring& wide) {
        if (wide.size() > boost::integer_traits<int>::const_max)
            throw std::length_error(
                "Wide string cannot be more than INT_MAX characters long.");
        if (wide.size() == 0)
            return "";

        // Calculate necessary buffer size
        int len = ::WideCharToMultiByte(
                      CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                      NULL, 0, NULL, NULL);

        // Perform actual conversion
        if (len > 0) {
            std::vector<char> buffer(len);
            len = ::WideCharToMultiByte(
                      CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                      &buffer[0], static_cast<int>(buffer.size()), NULL, NULL);
            if (len > 0) {
                verify(len == static_cast<int>(buffer.size()));
                return std::string(&buffer[0], buffer.size());
            }
        }

        msgasserted( 16091 ,
                     mongoutils::str::stream() << "can't wstring to utf8: " << ::GetLastError() );
        return "";
    }

    std::wstring toWideString(const char *utf8String) {
        int bufferSize = MultiByteToWideChar(
                CP_UTF8,            // Code page
                0,                  // Flags
                utf8String,         // Input string
                -1,                 // Count, -1 for NUL-terminated
                NULL,               // No output buffer
                0                   // Zero means "compute required size"
        );
        if ( bufferSize == 0 ) {
            return std::wstring();
        }
        boost::scoped_array< wchar_t > tempBuffer( new wchar_t[ bufferSize ] );
        tempBuffer[0] = 0;
        MultiByteToWideChar(
                CP_UTF8,            // Code page
                0,                  // Flags
                utf8String,         // Input string
                -1,                 // Count, -1 for NUL-terminated
                tempBuffer.get(),   // UTF-16 output buffer
                bufferSize          // Buffer size in wide characters
        );
        return std::wstring( tempBuffer.get() );
    }

    WindowsCommandLine::WindowsCommandLine( int argc, wchar_t* argvW[] ) {
        vector < string >   utf8args;
        vector < size_t >   utf8argLength;
        size_t blockSize = argc * sizeof( char * );
        size_t blockPtr = blockSize;
        for ( int i = 0; i < argc; ++i ) {
            utf8args.push_back( toUtf8String( argvW[ i ] ) );
            size_t argLength = utf8args[ i ].length() + 1;
            utf8argLength.push_back( argLength );
            blockSize += argLength;
        }
        _argv = reinterpret_cast< char** >( malloc( blockSize ) );
        for ( int i = 0; i < argc; ++i ) {
            _argv[ i ] = reinterpret_cast< char * >( _argv ) + blockPtr;
            strcpy_s( _argv[ i ], utf8argLength[ i ], utf8args[ i ].c_str() );
            blockPtr += utf8argLength[ i ];
        }
    }

    WindowsCommandLine::~WindowsCommandLine() {
        free( _argv );
    }

#endif // #if defined(_WIN32)

    struct TextUnitTest : public StartupTest {
        void run() {
            verify( parseLL("123") == 123 );
            verify( parseLL("-123000000000") == -123000000000LL );
        }
    } textUnitTest;

}

