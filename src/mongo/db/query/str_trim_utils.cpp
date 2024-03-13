/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <string_view>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/db/query/str_trim_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"

namespace mongo::str_trim_utils {

size_t numberOfBytesForCodePoint(char charByte) {
    if ((charByte & 0b11111000) == 0b11110000) {
        return 4;
    } else if ((charByte & 0b11110000) == 0b11100000) {
        return 3;
    } else if ((charByte & 0b11100000) == 0b11000000) {
        return 2;
    } else {
        return 1;
    }
}

std::vector<StringData> extractCodePointsFromChars(StringData utf8String) {
    std::vector<StringData> codePoints;
    std::size_t i = 0;
    while (i < utf8String.size()) {
        uassert(5156305,
                str::stream() << "Failed to parse \"chars\" argument to "
                              << "$trim/$ltrim/$rtrim"
                              << ": Detected invalid UTF-8. Got continuation byte when expecting "
                                 "the start of a new code point.",
                !str::isUTF8ContinuationByte(utf8String[i]));
        codePoints.push_back(utf8String.substr(i, numberOfBytesForCodePoint(utf8String[i])));
        i += numberOfBytesForCodePoint(utf8String[i]);
    }
    uassert(5156304,
            str::stream()
                << "Failed to parse \"chars\" argument to "
                << "$trim/$ltrim/$rtrim"
                << ": Detected invalid UTF-8. Missing expected continuation byte at end of string.",
            i <= utf8String.size());
    return codePoints;
}

bool codePointMatchesAtIndex(StringData input, std::size_t indexOfInput, StringData testCP) {
    for (size_t i = 0; i < testCP.size(); ++i) {
        if (indexOfInput + i >= input.size() || input[indexOfInput + i] != testCP[i]) {
            return false;
        }
    }
    return true;
};

StringData trimFromLeft(StringData input, const std::vector<StringData>& trimCPs) {
    std::size_t bytesTrimmedFromLeft = 0u;
    while (bytesTrimmedFromLeft < input.size()) {
        // Look for any matching code point to trim.
        auto matchingCP = std::find_if(trimCPs.begin(), trimCPs.end(), [&](auto& testCP) {
            return codePointMatchesAtIndex(input, bytesTrimmedFromLeft, testCP);
        });
        if (matchingCP == trimCPs.end()) {
            // Nothing to trim, stop here.
            break;
        }
        bytesTrimmedFromLeft += matchingCP->size();
    }
    return input.substr(bytesTrimmedFromLeft);
}

StringData trimFromRight(StringData input, const std::vector<StringData>& trimCPs) {
    std::size_t bytesTrimmedFromRight = 0u;
    while (bytesTrimmedFromRight < input.size()) {
        std::size_t indexToTrimFrom = input.size() - bytesTrimmedFromRight;
        auto matchingCP = std::find_if(trimCPs.begin(), trimCPs.end(), [&](auto& testCP) {
            if (indexToTrimFrom < testCP.size()) {
                // We've gone off the left of the string.
                return false;
            }
            return codePointMatchesAtIndex(input, indexToTrimFrom - testCP.size(), testCP);
        });
        if (matchingCP == trimCPs.end()) {
            // Nothing to trim, stop here.
            break;
        }
        bytesTrimmedFromRight += matchingCP->size();
    }
    return input.substr(0, input.size() - bytesTrimmedFromRight);
}

StringData doTrim(StringData input,
                  const std::vector<StringData>& trimCPs,
                  bool trimLeft,
                  bool trimRight) {
    if (trimLeft) {
        input = trimFromLeft(input, trimCPs);
    }
    if (trimRight) {
        input = trimFromRight(input, trimCPs);
    }
    return input;
}

}  // namespace mongo::str_trim_utils
