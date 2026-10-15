#pragma once

#include <string>

namespace mongo::windows_test_launcher {
// Quote for the Windows CRT, including empty arguments, quotes, and trailing backslashes.
inline std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
}  // namespace mongo::windows_test_launcher
