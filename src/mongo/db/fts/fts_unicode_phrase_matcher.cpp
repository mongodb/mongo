// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_unicode_phrase_matcher.h"

#include "mongo/db/fts/unicode/string.h"

#include <string_view>

namespace mongo {
namespace fts {

using std::string;

UnicodeFTSPhraseMatcher::UnicodeFTSPhraseMatcher(const string& language) {
    if (language == "turkish") {
        _caseFoldMode = unicode::CaseFoldMode::kTurkish;
    } else {
        _caseFoldMode = unicode::CaseFoldMode::kNormal;
    }
}

bool UnicodeFTSPhraseMatcher::phraseMatches(std::string_view phrase,
                                            std::string_view haystack,
                                            Options options) const {
    unicode::String::SubstrMatchOptions matchOptions = unicode::String::kNone;

    if (options & kCaseSensitive) {
        matchOptions |= unicode::String::kCaseSensitive;
    }

    if (options & kDiacriticSensitive) {
        matchOptions |= unicode::String::kDiacriticSensitive;
    }

    return unicode::String::substrMatch(haystack, phrase, matchOptions, _caseFoldMode);
}

}  // namespace fts
}  // namespace mongo
