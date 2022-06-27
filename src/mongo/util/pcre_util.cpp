/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/util/pcre_util.h"

#include <fmt/format.h>

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/pcre.h"

namespace mongo::pcre_util {

using namespace fmt::literals;

pcre::CompileOptions flagsToOptions(StringData optionFlags, StringData opName) {
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
                uasserted(51108, "{} invalid flag in regex options: {}"_format(opName, flag));
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

std::string quoteMeta(StringData str) {
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
