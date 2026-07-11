// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/substr_utils.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo::substr_utils {
using namespace std::literals::string_view_literals;

std::string_view getSubstringCP(std::string_view input, int startingPos, int len) {
    std::size_t startIndexBytes = 0;
    for (int i = 0; i < startingPos; ++i) {
        if (startIndexBytes >= input.size()) {
            return ""sv;
        }
        uassert(34456,
                "$substrCP: invalid UTF-8 string",
                !str::isUTF8ContinuationByte(input[startIndexBytes]));
        std::size_t codePointLength = str::getCodePointLength(input[startIndexBytes]);
        uassert(34457, "$substrCP: invalid UTF-8 string", codePointLength <= 4);
        startIndexBytes += codePointLength;
    }

    std::size_t endIndexBytes = startIndexBytes;
    for (int i = 0; i < len && endIndexBytes < input.size(); ++i) {
        uassert(34458,
                "$substrCP: invalid UTF-8 string",
                !str::isUTF8ContinuationByte(input[endIndexBytes]));
        std::size_t codePointLength = str::getCodePointLength(input[endIndexBytes]);
        uassert(34459, "$substrCP: invalid UTF-8 string", codePointLength <= 4);
        endIndexBytes += codePointLength;
    }
    size_t endPos = endIndexBytes - startIndexBytes;
    return input.substr(startIndexBytes, endPos);
}

}  // namespace mongo::substr_utils
