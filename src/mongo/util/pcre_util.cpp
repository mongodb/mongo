// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/util/pcre_util.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/pcre.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::pcre_util {

pcre::CompileOptions flagsToOptions(std::string_view optionFlags, std::string_view opName) {
    pcre::CompileOptions opt = pcre::UTF;
    for (char flag : optionFlags) {
        switch (flag) {
            case 'i':  // case insensitive
                opt |= pcre::CASELESS;
                continue;
            case 'm':  // newlines match ^ and $
                opt |= pcre::MULTILINE;
                continue;
            case 's':  // allows dot to include newline chars
                opt |= pcre::DOTALL;
                continue;
            case 'u':
                continue;
            case 'x':  // extended mode
                opt |= pcre::EXTENDED;
                continue;
            default:
                uasserted(51108, fmt::format("{} invalid flag in regex options: {}", opName, flag));
        }
    }
    return opt;
}

std::string optionsToFlags(pcre::CompileOptions opt) {
    std::string optionFlags = "";
    if (opt & pcre::CASELESS)
        optionFlags += 'i';
    if (opt & pcre::MULTILINE)
        optionFlags += 'm';
    if (opt & pcre::DOTALL)
        optionFlags += 's';
    if (opt & pcre::EXTENDED)
        optionFlags += 'x';
    return optionFlags;
}

std::string quoteMeta(std::string_view str) {
    std::string result;
    for (char c : str) {
        if (c == '\0') {
            result += "\\0";
            continue;
        }
        if (!ctype::isAlnum(c) && c != '_' && !(c & 0x80))
            result += '\\';
        result += c;
    }
    return result;
}

}  // namespace mongo::pcre_util
