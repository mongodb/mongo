// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/* This doesn't defend against ALL bad UTF8, but it will guarantee that the
 * std::string can be converted to sequence of codepoints. However, it doesn't
 * guarantee that the codepoints are valid.
 */
bool isValidUTF8(std::string_view s);

#if defined(_WIN32)

std::string toUtf8String(const std::wstring& wide);

std::wstring toWideStringFromStringData(std::string_view s);
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
