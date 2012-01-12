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
#include "text.h"
#include "unittest.h"
#include "boost/system/system_error.hpp"

namespace mongo {

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
                assert(len == static_cast<int>(buffer.size()));
                return std::string(&buffer[0], buffer.size());
            }
        }

        throw boost::system::system_error(
            ::GetLastError(), boost::system::system_category);
    }

#if defined(_UNICODE)
    std::wstring toWideString(const char *s) {
        std::basic_ostringstream<TCHAR> buf;
        buf << s;
        return buf.str();
    }
#endif

#endif

    struct TextUnitTest : public UnitTest {
        void run() {
            assert( parseLL("123") == 123 );
            assert( parseLL("-123000000000") == -123000000000LL );
        }
    } textUnitTest;

}

