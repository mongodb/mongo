// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_basic_phrase_matcher.h"

#include "mongo/util/ctype.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace mongo {
namespace fts {

bool BasicFTSPhraseMatcher::phraseMatches(std::string_view phrase,
                                          std::string_view haystack,
                                          Options options) const {
    if (options & kCaseSensitive) {
        return haystack.find(phrase) != std::string::npos;
    }

    return std::search(
               haystack.begin(), haystack.end(), phrase.begin(), phrase.end(), [](char a, char b) {
                   return ctype::toLower(a) == ctype::toLower(b);
               }) != haystack.end();
}

}  // namespace fts
}  // namespace mongo
