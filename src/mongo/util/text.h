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

#pragma once

#include <vector>
#include <string>

#include "mongo/base/disallow_copying.h"

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
        bool more() const { return _big[0] != 0; }

        /** get next split string fragment */
        std::string next();

        void split( std::vector<std::string>& l );

        std::vector<std::string> split();
        
        static std::vector<std::string> split( const std::string& big , const std::string& splitter );

        static std::string join( const std::vector<std::string>& l , const std::string& split );

    private:
        const char * _big;
        const char * _splitter;
    };

    /* This doesn't defend against ALL bad UTF8, but it will guarantee that the
     * string can be converted to sequence of codepoints. However, it doesn't
     * guarantee that the codepoints are valid.
     */
    bool isValidUTF8(const char *s);
    bool isValidUTF8(const std::string& s);

    // expect that n contains a base ten number and nothing else after it
    // NOTE win version hasn't been tested directly
    long long parseLL( const char *n );

#if defined(_WIN32)

    std::string toUtf8String(const std::wstring& wide);

    std::wstring toWideString(const char *s);

    bool writeUtf8ToWindowsConsole( const char* utf8String, unsigned int utf8StringSize );

    /* like toWideString but UNICODE macro sensitive */
# if !defined(_UNICODE)
#error temp error 
    inline std::string toNativeString(const char *s) { return s; }
# else
    inline std::wstring toNativeString(const char *s) { return toWideString(s); }
# endif

    class WindowsCommandLine {
        MONGO_DISALLOW_COPYING(WindowsCommandLine);
        char**  _argv;
        char**  _envp;

    public:
        WindowsCommandLine(int argc, wchar_t* argvW[], wchar_t* envpW[]);
        ~WindowsCommandLine();
        char** argv(void) const { return _argv; };
        char** envp(void) const { return _envp; };
    };

#endif // #if defined(_WIN32)

    /**
     * Construct a Windows command line string, UTF-8 encoded, from a vector of
     * UTF-8 arguments, "argv".
     *
     * See "Parsing C++ Command-Line Arguments (C++)"
     * http://msdn.microsoft.com/en-us/library/windows/desktop/17w5ykft(v=vs.85).aspx
     */
    std::string constructUtf8WindowsCommandLine(const std::vector<std::string>& argv);

} // namespace mongo
