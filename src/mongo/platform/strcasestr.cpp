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

#include "mongo/platform/strcasestr.h"

#if defined(_WIN32)

#include <algorithm>
#include <cstring>
#include <string>

#define STRCASESTR_EMULATION_NAME strcasestr

#include "mongo/util/ctype.h"
#include "mongo/util/str.h"

namespace mongo {
namespace pal {

/**
 * strcasestr -- case-insensitive search for a substring within another string.
 *
 * @param haystack      ptr to C-string to search
 * @param needle        ptr to C-string to try to find within 'haystack'
 * @return              ptr to start of 'needle' within 'haystack' if found, NULL otherwise
 */
const char* STRCASESTR_EMULATION_NAME(const char* haystack, const char* needle) {
    StringData hay(haystack);
    StringData pat(needle);
    auto caseEq = [](char a, char b) {
        return ctype::toLower(a) == ctype::toLower(b);
    };
    auto pos = std::search(hay.begin(), hay.end(), pat.begin(), pat.end(), caseEq);
    if (pos == hay.end())
        return nullptr;
    return haystack + (pos - hay.begin());
}

}  // namespace pal
}  // namespace mongo

#endif  // #if defined(_WIN32)
