// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/str_trim_utils.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>
#include <vector>

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

std::vector<std::string_view> extractCodePointsFromChars(std::string_view utf8String) {
    std::vector<std::string_view> codePoints;

    // Do a conservative upfront allocation for 'codePoints'. Each UTF-8 character is at most 4
    // bytes large. In the worst case, more allocations need to happen inside the loop, but this
    // ensures we do not overallocate.
    codePoints.reserve(utf8String.size() / 4);

    std::size_t i = 0;
    while (i < utf8String.size()) {
        uassert(5156305,
                str::stream() << "Failed to parse \"chars\" argument to "
                              << "$trim/$ltrim/$rtrim"
                              << ": Detected invalid UTF-8. Got continuation byte when expecting "
                                 "the start of a new code point.",
                !str::isUTF8ContinuationByte(utf8String[i]));
        size_t numberOfBytes = numberOfBytesForCodePoint(utf8String[i]);
        codePoints.push_back(utf8String.substr(i, numberOfBytes));
        i += numberOfBytes;
    }
    uassert(5156304,
            str::stream()
                << "Failed to parse \"chars\" argument to "
                << "$trim/$ltrim/$rtrim"
                << ": Detected invalid UTF-8. Missing expected continuation byte at end of string.",
            i <= utf8String.size());
    return codePoints;
}

bool codePointMatchesAtIndex(std::string_view input,
                             std::size_t indexOfInput,
                             std::string_view testCP) {
    for (size_t i = 0; i < testCP.size(); ++i) {
        if (indexOfInput + i >= input.size() || input[indexOfInput + i] != testCP[i]) {
            return false;
        }
    }
    return true;
};

std::string_view trimFromLeft(std::string_view input,
                              const std::vector<std::string_view>& trimCPs) {
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

std::string_view trimFromRight(std::string_view input,
                               const std::vector<std::string_view>& trimCPs) {
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

std::string_view doTrim(std::string_view input,
                        const std::vector<std::string_view>& trimCPs,
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
