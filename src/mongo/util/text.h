// text.h

/*
 *    Copyright 2010 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/config.h"

namespace mongo {

class StringSplitter {
public:
    /** @param big the std::string to be split
        @param splitter the delimiter
    */
    StringSplitter(const char* big, const char* splitter) : _big(big), _splitter(splitter) {}

    /** @return true if more to be taken via next() */
    bool more() const {
        return _big[0] != 0;
    }

    /** get next split std::string fragment */
    std::string next();

    void split(std::vector<std::string>& l);

    std::vector<std::string> split();

    static std::vector<std::string> split(const std::string& big, const std::string& splitter);

    static std::string join(const std::vector<std::string>& l, const std::string& split);

private:
    const char* _big;
    const char* _splitter;
};

/* This doesn't defend against ALL bad UTF8, but it will guarantee that the
 * std::string can be converted to sequence of codepoints. However, it doesn't
 * guarantee that the codepoints are valid.
 */
bool isValidUTF8(const char* s);
bool isValidUTF8(const std::string& s);

// expect that n contains a base ten number and nothing else after it
// NOTE win version hasn't been tested directly
long long parseLL(const char* n);

#if defined(_WIN32)

std::string toUtf8String(const std::wstring& wide);

std::wstring toWideString(const char* s);

bool writeUtf8ToWindowsConsole(const char* utf8String, unsigned int utf8StringSize);

/* like toWideString but UNICODE macro sensitive */
#if !defined(_UNICODE)
#error temp error
inline std::string toNativeString(const char* s) {
    return s;
}
#else
inline std::wstring toNativeString(const char* s) {
    return toWideString(s);
}
#endif

class WindowsCommandLine {
    MONGO_DISALLOW_COPYING(WindowsCommandLine);
    char** _argv;
    char** _envp;

public:
    WindowsCommandLine(int argc, wchar_t* argvW[], wchar_t* envpW[]);
    ~WindowsCommandLine();
    char** argv(void) const {
        return _argv;
    };
    char** envp(void) const {
        return _envp;
    };
};

#endif  // #if defined(_WIN32)

/**
 * Construct a Windows command line string, UTF-8 encoded, from a vector of
 * UTF-8 arguments, "argv".
 *
 * See "Parsing C++ Command-Line Arguments (C++)"
 * http://msdn.microsoft.com/en-us/library/windows/desktop/17w5ykft(v=vs.85).aspx
 */
std::string constructUtf8WindowsCommandLine(const std::vector<std::string>& argv);

}  // namespace mongo
