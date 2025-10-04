/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep

#include <memory>
#include <string>
#include <vector>

namespace mongo {

/* This doesn't defend against ALL bad UTF8, but it will guarantee that the
 * std::string can be converted to sequence of codepoints. However, it doesn't
 * guarantee that the codepoints are valid.
 */
bool isValidUTF8(StringData s);

#if defined(_WIN32)

std::string toUtf8String(const std::wstring& wide);

std::wstring toWideStringFromStringData(StringData s);
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
public:
    WindowsCommandLine(int argc, wchar_t** argvW);
    ~WindowsCommandLine();

    char** argv() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
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
