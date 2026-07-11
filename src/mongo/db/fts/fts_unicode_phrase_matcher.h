// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/fts/fts_phrase_matcher.h"
#include "mongo/db/fts/unicode/codepoints.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo {
namespace fts {

class FTSLanguage;

/**
 * UnicodeFTSPhraseMatcher
 *
 * A phrase matcher that looks for exact substring matches that ignore diacritics, and with UTF-8
 * aware case folding if the phrase match is not specified as case sensitive. Optionally, the phrase
 * matching can be diacritic sensitive if a parameter is passed to the constructor. Additionally, if
 * the language string passed to the phrase matcher's constructor is Turkish (uses the special I
 * case fold mapping), the phrase matcher will take that into account.
 */
class UnicodeFTSPhraseMatcher final : public FTSPhraseMatcher {
    UnicodeFTSPhraseMatcher(const UnicodeFTSPhraseMatcher&) = delete;
    UnicodeFTSPhraseMatcher& operator=(const UnicodeFTSPhraseMatcher&) = delete;

public:
    UnicodeFTSPhraseMatcher(const std::string& language);

    bool phraseMatches(std::string_view phrase,
                       std::string_view haystack,
                       Options options) const override;

private:
    unicode::CaseFoldMode _caseFoldMode;
};

}  // namespace fts
}  // namespace mongo
